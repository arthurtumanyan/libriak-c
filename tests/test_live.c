/*
 File:   test_live.c
 SPDX-License-Identifier: GPL-3.0-or-later

 Tests against a real Riak node. Skipped unless RIAK_TEST_URL is set, e.g.
   docker run -d -p 8098:8098 basho/riak-kv
   RIAK_TEST_URL=http://127.0.0.1:8098 ctest -R live
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "check.h"
#include "riak/riak.h"

static riak_client *rc;
static char bucket[64];

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

    riak_client_free(rc);
    riak_global_cleanup();
    return CHECK_DONE();
}
