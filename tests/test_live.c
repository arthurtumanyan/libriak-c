/*
 File:   test_live.c
 SPDX-License-Identifier: GPL-3.0-or-later

 Tests against a real Riak node. Skipped unless RIAK_TEST_URL is set, e.g.
   docker run -d -p 8098:8098 basho/riak-kv
   RIAK_TEST_URL=http://127.0.0.1:8098 ctest -R live
 Bucket type and sibling tests also need an allow_mult bucket type:
   riak-admin bucket-type create libriak_multi '{"props":{"allow_mult":true}}'
   riak-admin bucket-type activate libriak_multi
   RIAK_TEST_TYPE=libriak_multi
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "check.h"
#include "riak/riak.h"

static riak_client *rc;
static char bucket[64];
static const char *multi_type; /* bucket type with allow_mult, from RIAK_TEST_TYPE */

static void test_ping(void) {
    CHECK_EQ_INT(riak_ping(rc), RIAK_OK);
}

static void test_roundtrip(void) {
    riak_kv_pair meta[] = {{"origin", "libriak-c"}};
    riak_opts o = {.content_type = "text/plain", .meta = meta, .n_meta = 1, .w = RIAK_Q_QUORUM};
    const char *key = "key with/slash?";
    CHECK_EQ_INT(riak_put(rc, bucket, key, "value-1", 7, &o, NULL, NULL), RIAK_OK);

    riak_object obj;
    riak_opts r = {.r = RIAK_Q_QUORUM};
    CHECK_EQ_INT(riak_get(rc, bucket, key, &r, &obj), RIAK_OK);
    CHECK_EQ_INT(obj.len, 7);
    CHECK_EQ_STR(obj.data, "value-1");
    CHECK_EQ_STR(obj.content_type, "text/plain");
    CHECK(obj.vclock != NULL);

    /* update with the vclock, asking for the stored body back */
    riak_opts upd = {.content_type = "text/plain", .vclock = obj.vclock, .return_body = true};
    riak_object back;
    CHECK_EQ_INT(riak_put(rc, bucket, key, "value-2", 7, &upd, &back, NULL), RIAK_OK);
    CHECK_EQ_STR(back.data, "value-2");
    riak_object_free(&back);
    riak_object_free(&obj);

    CHECK_EQ_INT(riak_delete(rc, bucket, key, NULL), RIAK_OK);
    CHECK_EQ_INT(riak_get(rc, bucket, key, NULL, &obj), RIAK_E_NOT_FOUND);
}

static void test_autokey_and_list(void) {
    char *key = NULL;
    CHECK_EQ_INT(riak_put(rc, bucket, NULL, "auto", 4, NULL, NULL, &key), RIAK_OK);
    CHECK(key != NULL && key[0] != '\0');

    riak_strlist keys;
    CHECK_EQ_INT(riak_list_keys(rc, bucket, &keys), RIAK_OK);
    bool found = false;
    for (char **k = keys.items; k && *k; k++) {
        found = found || (key && strcmp(*k, key) == 0);
    }
    CHECK(found);
    riak_strlist_free(&keys);

    riak_strlist buckets;
    CHECK_EQ_INT(riak_list_buckets(rc, &buckets), RIAK_OK);
    found = false;
    for (char **b = buckets.items; b && *b; b++) {
        found = found || strcmp(*b, bucket) == 0;
    }
    CHECK(found);
    riak_strlist_free(&buckets);

    if (key) {
        CHECK_EQ_INT(riak_delete(rc, bucket, key, NULL), RIAK_OK);
    }
    riak_free(key);
}

static void test_bucket_props(void) {
    CHECK_EQ_INT(riak_set_bucket_props(rc, bucket, "{\"props\":{\"n_val\":2}}"), RIAK_OK);
    char *json = NULL;
    CHECK_EQ_INT(riak_get_bucket_props(rc, bucket, &json), RIAK_OK);
    CHECK(json != NULL && strstr(json, "\"n_val\":2") != NULL);
    riak_free(json);
    CHECK_EQ_INT(riak_reset_bucket_props(rc, bucket), RIAK_OK);
}

static void test_conditional_and_stats(void) {
    riak_opts o = {.content_type = "text/plain"};
    CHECK_EQ_INT(riak_put(rc, bucket, "cond", "v1", 2, &o, NULL, NULL), RIAK_OK);
    riak_object obj;
    CHECK_EQ_INT(riak_get(rc, bucket, "cond", NULL, &obj), RIAK_OK);
    CHECK(obj.etag != NULL);

    riak_opts inm = {.if_none_match = obj.etag};
    riak_object again;
    CHECK_EQ_INT(riak_get(rc, bucket, "cond", &inm, &again), RIAK_E_NOT_MODIFIED);
    riak_opts create = {.if_none_match = "*", .content_type = "text/plain"};
    CHECK_EQ_INT(riak_put(rc, bucket, "cond", "v2", 2, &create, NULL, NULL), RIAK_E_PRECONDITION);
    riak_object_free(&obj);
    CHECK_EQ_INT(riak_delete(rc, bucket, "cond", NULL), RIAK_OK);

    riak_opts slow = {.server_timeout_ms = 5000};
    CHECK_EQ_INT(riak_get(rc, bucket, "cond", &slow, &obj), RIAK_E_NOT_FOUND);

    char *json = NULL;
    CHECK_EQ_INT(riak_stats(rc, &json), RIAK_OK);
    CHECK(json && strstr(json, "\"vnode_gets\""));
    riak_free(json);
}

static size_t pick_last(const riak_object *s, size_t n, void *ctx) {
    (void)s;
    (void)ctx;
    return n - 1;
}

static void test_types_and_siblings(void) {
    if (multi_type == NULL) {
        fprintf(stderr, "RIAK_TEST_TYPE not set; skipping bucket type tests\n");
        return;
    }
    char *json = NULL;
    CHECK_EQ_INT(riak_get_type_props(rc, multi_type, &json), RIAK_OK);
    CHECK(json && strstr(json, "\"allow_mult\":true"));
    riak_free(json);

    riak_opts o = {.bucket_type = multi_type, .content_type = "text/plain"};
    CHECK_EQ_INT(riak_put(rc, bucket, "s", "one", 3, &o, NULL, NULL), RIAK_OK);
    CHECK_EQ_INT(riak_put(rc, bucket, "s", "two", 3, &o, NULL, NULL), RIAK_OK);

    riak_object obj;
    CHECK_EQ_INT(riak_get(rc, bucket, "s", &o, &obj), RIAK_E_SIBLINGS);
    riak_object_set set;
    CHECK_EQ_INT(riak_get_siblings(rc, bucket, "s", &o, &set), RIAK_OK);
    CHECK_EQ_INT(set.count, 2);
    CHECK(set.vclock != NULL);
    riak_object_set_free(&set);

    riak_opts res = o;
    res.resolver = pick_last;
    CHECK_EQ_INT(riak_get(rc, bucket, "s", &res, &obj), RIAK_OK);
    riak_opts wb = o;
    wb.vclock = obj.vclock;
    CHECK_EQ_INT(riak_put(rc, bucket, "s", "merged", 6, &wb, NULL, NULL), RIAK_OK);
    riak_object_free(&obj);
    CHECK_EQ_INT(riak_get(rc, bucket, "s", &o, &obj), RIAK_OK);
    CHECK_EQ_STR(obj.data, "merged");
    riak_object_free(&obj);

    riak_strlist keys;
    CHECK_EQ_INT(riak_type_list_keys(rc, multi_type, bucket, &keys), RIAK_OK);
    CHECK(keys.count == 1 && strcmp(keys.items[0], "s") == 0);
    riak_strlist_free(&keys);
    CHECK_EQ_INT(riak_type_get_bucket_props(rc, multi_type, bucket, &json), RIAK_OK);
    riak_free(json);
    CHECK_EQ_INT(riak_delete(rc, bucket, "s", &o), RIAK_OK);
}

int main(void) {
    const char *url = getenv("RIAK_TEST_URL");
    if (url == NULL || *url == '\0') {
        fprintf(stderr, "RIAK_TEST_URL not set; skipping live tests\n");
        return 77;
    }
    char host[256] = {0};
    unsigned port = 8098;
    char scheme[8] = {0};
    if (sscanf(url, "%7[^:]://%255[^:/]:%u", scheme, host, &port) < 2) {
        fprintf(stderr, "cannot parse RIAK_TEST_URL=%s\n", url);
        return 1;
    }
    multi_type = getenv("RIAK_TEST_TYPE");
    if (multi_type && *multi_type == '\0') {
        multi_type = NULL;
    }
    snprintf(bucket, sizeof bucket, "libriak-c-test-%ld", (long)time(NULL));

    CHECK_EQ_INT(riak_global_init(), RIAK_OK);
    riak_config cfg;
    riak_config_init(&cfg);
    cfg.host = host;
    cfg.port = port;
    cfg.tls = strcmp(scheme, "https") == 0;
    cfg.timeout_ms = 30000;
    if (riak_client_new(&cfg, &rc) != RIAK_OK) {
        return 1;
    }

    RUN(test_ping);
    RUN(test_roundtrip);
    RUN(test_autokey_and_list);
    RUN(test_bucket_props);
    RUN(test_conditional_and_stats);
    RUN(test_types_and_siblings);

    riak_client_free(rc);
    riak_global_cleanup();
    return CHECK_DONE();
}
