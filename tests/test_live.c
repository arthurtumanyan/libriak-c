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
 Secondary index and Search tests need a node built from tests/docker
 (leveldb backend, search = on) and RIAK_TEST_FEATURES=2i,search.
 Data type tests ("dt") need bucket types libriak_counters, libriak_sets,
 libriak_maps and libriak_hlls created with {"props":{"datatype":"<type>"}};
 "gset" (Riak 2.9+) also needs libriak_gsets.
 */

#define _POSIX_C_SOURCE 200809L /* nanosleep */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "check.h"
#include "riak/riak.h"

static riak_client *rc;
static char bucket[64];
static const char *multi_type; /* bucket type with allow_mult, from RIAK_TEST_TYPE */
static const char *features = "";
static const char *live_host;
static unsigned live_port; /* RIAK_TEST_FEATURES, e.g. "2i,search" */

static bool has_feature(const char *f) {
    const char *p = strstr(features, f);
    size_t n = strlen(f);
    return p && (p == features || p[-1] == ',') && (p[n] == '\0' || p[n] == ',');
}

static void sleep_ms(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

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

static bool count_key(const char *key, const char *term, void *ctx) {
    (void)key;
    (void)term;
    (*(size_t *)ctx)++;
    return true;
}

static void test_secondary_indexes(void) {
    if (!has_feature("2i")) {
        fprintf(stderr, "2i not in RIAK_TEST_FEATURES; skipping\n");
        return;
    }
    /* a fresh bucket: $bucket would also list tombstones of other tests */
    char b2i[80];
    snprintf(b2i, sizeof b2i, "%s-2i", bucket);
    for (int i = 0; i < 5; i++) {
        char key[8], age[8], name[8];
        snprintf(key, sizeof key, "p%d", i);
        snprintf(age, sizeof age, "%d", 20 + i);
        snprintf(name, sizeof name, "n%d", i);
        riak_kv_pair idx[] = {{"age_int", age}, {"name_bin", name}};
        riak_opts o = {.content_type = "text/plain", .indexes = idx, .n_indexes = 2};
        CHECK_EQ_INT(riak_put(rc, b2i, key, "x", 1, &o, NULL, NULL), RIAK_OK);
    }
    riak_index_result r;
    riak_index_query exact = {.index = "age_int", .value = "22"};
    CHECK_EQ_INT(riak_index_query_run(rc, b2i, &exact, &r), RIAK_OK);
    CHECK(r.count == 1 && strcmp(r.keys[0], "p2") == 0);
    riak_index_result_free(&r);

    riak_index_query range = {.index = "name_bin",
                              .range_min = "n1",
                              .range_max = "n3",
                              .return_terms = true,
                              .max_results = 2};
    CHECK_EQ_INT(riak_index_query_run(rc, b2i, &range, &r), RIAK_OK);
    CHECK_EQ_INT(r.count, 2);
    CHECK(r.continuation != NULL);
    if (r.count == 2 && r.terms) {
        CHECK_EQ_STR(r.terms[0], "n1");
        CHECK_EQ_STR(r.keys[0], "p1");
    }
    range.continuation = r.continuation;
    riak_index_result page2;
    CHECK_EQ_INT(riak_index_query_run(rc, b2i, &range, &page2), RIAK_OK);
    CHECK(page2.count == 1 && strcmp(page2.keys[0], "p3") == 0);
    riak_index_result_free(&page2);
    riak_index_result_free(&r);

    size_t n = 0;
    riak_index_query all = {.index = "$key", .range_min = "p0", .range_max = "p9"};
    CHECK_EQ_INT(riak_index_query_stream(rc, b2i, &all, count_key, &n, NULL), RIAK_OK);
    CHECK_EQ_INT(n, 5);
    riak_index_query bkt = {.index = "$bucket", .value = b2i};
    CHECK_EQ_INT(riak_index_query_run(rc, b2i, &bkt, &r), RIAK_OK);
    CHECK_EQ_INT(r.count, 5);
    riak_index_result_free(&r);
    for (int i = 0; i < 5; i++) {
        char key[8];
        snprintf(key, sizeof key, "p%d", i);
        riak_delete(rc, b2i, key, NULL);
    }
}

static void test_search(void) {
    if (!has_feature("search")) {
        fprintf(stderr, "search not in RIAK_TEST_FEATURES; skipping\n");
        return;
    }
    char index[80];
    snprintf(index, sizeof index, "%s_idx", bucket);
    for (char *p = index; *p; p++) {
        if (*p == '-') *p = '_';
    }
    CHECK_EQ_INT(riak_search_index_create(rc, index, NULL, 0), RIAK_OK);
    char *json = NULL;
    /* index creation is asynchronous: wait until it is visible */
    riak_status st = RIAK_E_NOT_FOUND;
    for (int i = 0; i < 60 && st != RIAK_OK; i++) {
        st = riak_search_index_get(rc, index, &json);
        if (st != RIAK_OK) sleep_ms(500);
    }
    CHECK_EQ_INT(st, RIAK_OK);
    CHECK(json && strstr(json, "_yz_default"));
    riak_free(json);
    CHECK_EQ_INT(riak_search_index_list(rc, &json), RIAK_OK);
    CHECK(json && strstr(json, index));
    riak_free(json);
    /* the Solr core starts after the index is visible; wait until it answers */
    riak_search_query probe = {.q = "*:*", .rows = 1};
    riak_search_result res = {0};
    st = RIAK_E_HTTP;
    for (int i = 0; i < 60 && st != RIAK_OK; i++) {
        st = riak_search(rc, index, &probe, &res);
        riak_search_result_free(&res);
        if (st != RIAK_OK) sleep_ms(500);
    }
    CHECK_EQ_INT(st, RIAK_OK);
    CHECK_EQ_INT(riak_search_schema_get(rc, "_yz_default", &json), RIAK_OK);
    CHECK(json && strstr(json, "<schema"));
    riak_free(json);

    char props[160];
    snprintf(props, sizeof props, "{\"props\":{\"search_index\":\"%s\"}}", index);
    CHECK_EQ_INT(riak_set_bucket_props(rc, bucket, props), RIAK_OK);
    riak_opts o = {.content_type = "application/json"};
    const char *alice = "{\"name_s\":\"alice\",\"age_i\":30}";
    const char *bob = "{\"name_s\":\"bob\",\"age_i\":40}";
    CHECK_EQ_INT(riak_put(rc, bucket, "u1", alice, strlen(alice), &o, NULL, NULL), RIAK_OK);
    CHECK_EQ_INT(riak_put(rc, bucket, "u2", bob, strlen(bob), &o, NULL, NULL), RIAK_OK);
    riak_search_query q = {.q = "name_s:alice"};
    /* documents become searchable after Solr's soft commit (~1 s) */
    for (int i = 0; i < 30; i++) {
        riak_search_result_free(&res);
        st = riak_search(rc, index, &q, &res);
        if (st != RIAK_OK || res.num_found > 0) break;
        sleep_ms(500);
    }
    CHECK_EQ_INT(st, RIAK_OK);
    CHECK_EQ_INT(res.num_found, 1);
    if (res.count == 1) {
        CHECK_EQ_STR(res.docs[0].key, "u1");
        CHECK_EQ_STR(res.docs[0].bucket, bucket);
    }
    CHECK(res.raw != NULL);
    riak_search_result_free(&res);

    riak_search_query sorted = {.q = "age_i:[1 TO 100]", .sort = "age_i desc", .rows = 1};
    CHECK_EQ_INT(riak_search(rc, index, &sorted, &res), RIAK_OK);
    CHECK(res.count == 1 && res.num_found == 2 && strcmp(res.docs[0].key, "u2") == 0);
    riak_search_result_free(&res);

    riak_delete(rc, bucket, "u1", NULL);
    riak_delete(rc, bucket, "u2", NULL);
    CHECK_EQ_INT(riak_reset_bucket_props(rc, bucket), RIAK_OK);
    CHECK_EQ_INT(riak_search_index_delete(rc, index), RIAK_OK);
}

static bool collect_phase(unsigned phase, const char *data, void *ctx) {
    size_t *seen = ctx;
    if (phase == 1 && strcmp(data, "[2]") == 0) {
        seen[1]++;
    } else if (phase == 0) {
        seen[0]++;
    }
    return true;
}

static void test_mapreduce(void) {
    riak_opts o = {.content_type = "text/plain"};
    CHECK_EQ_INT(riak_put(rc, bucket, "m1", "va", 2, &o, NULL, NULL), RIAK_OK);
    CHECK_EQ_INT(riak_put(rc, bucket, "m2", "vb", 2, &o, NULL, NULL), RIAK_OK);
    char job[512];
    snprintf(job, sizeof job,
             "{\"inputs\":[[\"%s\",\"m1\"],[\"%s\",\"m2\"]],\"query\":["
             "{\"map\":{\"language\":\"erlang\",\"module\":\"riak_kv_mapreduce\","
             "\"function\":\"map_object_value\",\"keep\":true}},"
             "{\"reduce\":{\"language\":\"erlang\",\"module\":\"riak_kv_mapreduce\","
             "\"function\":\"reduce_count_inputs\",\"keep\":true}}]}",
             bucket, bucket);
    char *result = NULL;
    CHECK_EQ_INT(riak_mapred(rc, job, &result), RIAK_OK);
    CHECK(result && strstr(result, "2"));
    riak_free(result);
    size_t seen[2] = {0, 0};
    CHECK_EQ_INT(riak_mapred_stream(rc, job, collect_phase, seen), RIAK_OK);
    CHECK(seen[0] >= 1);
    CHECK_EQ_INT(seen[1], 1);
    CHECK_EQ_INT(riak_mapred(rc, "{\"inputs\":", &result), RIAK_E_HTTP);
    riak_delete(rc, bucket, "m1", NULL);
    riak_delete(rc, bucket, "m2", NULL);
}

static void test_datatypes(void) {
    if (!has_feature("dt")) {
        fprintf(stderr, "dt not in RIAK_TEST_FEATURES; skipping\n");
        return;
    }
    riak_dt_opts rb = {.return_body = true};
    riak_datatype dt;

    /* counter */
    riak_dt_op *c = riak_dt_op_new(RIAK_DT_COUNTER);
    riak_dt_op_increment(c, 7);
    riak_dt_op_increment(c, -2);
    CHECK_EQ_INT(riak_dt_update(rc, "libriak_counters", bucket, "c", c, NULL, &rb, &dt, NULL),
                 RIAK_OK);
    CHECK(dt.value.type == RIAK_DT_COUNTER && dt.value.counter == 5);
    riak_datatype_free(&dt);
    char *key = NULL;
    CHECK_EQ_INT(riak_dt_update(rc, "libriak_counters", bucket, NULL, c, NULL, NULL, NULL, &key),
                 RIAK_OK);
    CHECK(key != NULL && *key);
    CHECK_EQ_INT(riak_dt_fetch(rc, "libriak_counters", bucket, key, NULL, &dt), RIAK_OK);
    CHECK_EQ_INT(dt.value.counter, 5);
    riak_datatype_free(&dt);
    riak_free(key);
    riak_dt_op_free(c);
    CHECK_EQ_INT(riak_dt_fetch(rc, "libriak_counters", bucket, "absent", NULL, &dt),
                 RIAK_E_NOT_FOUND);

    /* set: add, then remove with the context */
    riak_dt_op *s = riak_dt_op_new(RIAK_DT_SET);
    riak_dt_op_add(s, "a");
    riak_dt_op_add(s, "b");
    riak_dt_op_add(s, "c");
    CHECK_EQ_INT(riak_dt_update(rc, "libriak_sets", bucket, "s", s, NULL, NULL, NULL, NULL),
                 RIAK_OK);
    riak_dt_op_free(s);
    CHECK_EQ_INT(riak_dt_fetch(rc, "libriak_sets", bucket, "s", NULL, &dt), RIAK_OK);
    CHECK_EQ_INT(dt.value.n_elements, 3);
    CHECK(dt.context != NULL);
    s = riak_dt_op_new(RIAK_DT_SET);
    riak_dt_op_remove(s, "a");
    riak_datatype after;
    CHECK_EQ_INT(riak_dt_update(rc, "libriak_sets", bucket, "s", s, dt.context, &rb, &after, NULL),
                 RIAK_OK);
    CHECK(after.value.n_elements == 2 && strcmp(after.value.elements[0], "b") == 0);
    riak_datatype_free(&after);
    riak_dt_op_free(s);
    s = riak_dt_op_new(RIAK_DT_SET);
    riak_dt_op_remove(s, "nope");
    /* without a context Riak checks presence; with one the remove is a no-op */
    CHECK_EQ_INT(riak_dt_update(rc, "libriak_sets", bucket, "s", s, NULL, NULL, NULL, NULL),
                 RIAK_E_PRECONDITION);
    CHECK_EQ_INT(riak_dt_update(rc, "libriak_sets", bucket, "s", s, dt.context, NULL, NULL, NULL),
                 RIAK_OK);
    riak_dt_op_free(s);
    riak_datatype_free(&dt);

    /* map with every embeddable type, nested map, then a field removal */
    riak_dt_op *m = riak_dt_op_new(RIAK_DT_MAP);
    riak_dt_op_map_register(m, "name", "alice");
    riak_dt_op_map_flag(m, "admin", true);
    riak_dt_op_map_counter(m, "visits", 3);
    riak_dt_op_add(riak_dt_op_map_set(m, "tags"), "x");
    riak_dt_op_map_register(riak_dt_op_map_map(m, "addr"), "city", "Yerevan");
    CHECK_EQ_INT(riak_dt_update(rc, "libriak_maps", bucket, "m", m, NULL, &rb, &dt, NULL), RIAK_OK);
    riak_dt_op_free(m);
    const riak_dt_value *v = riak_dt_map_get(&dt.value, "name", RIAK_DT_REGISTER);
    CHECK(v && strcmp(v->reg, "alice") == 0);
    v = riak_dt_map_get(&dt.value, "admin", RIAK_DT_FLAG);
    CHECK(v && v->flag);
    v = riak_dt_map_get(&dt.value, "visits", RIAK_DT_COUNTER);
    CHECK(v && v->counter == 3);
    v = riak_dt_map_get(&dt.value, "tags", RIAK_DT_SET);
    CHECK(v && v->n_elements == 1);
    v = riak_dt_map_get(riak_dt_map_get(&dt.value, "addr", RIAK_DT_MAP), "city", RIAK_DT_REGISTER);
    CHECK(v && strcmp(v->reg, "Yerevan") == 0);
    m = riak_dt_op_new(RIAK_DT_MAP);
    riak_dt_op_map_remove(m, "admin", RIAK_DT_FLAG);
    riak_dt_op_map_counter(m, "visits", 1);
    CHECK_EQ_INT(riak_dt_update(rc, "libriak_maps", bucket, "m", m, dt.context, &rb, &after, NULL),
                 RIAK_OK);
    CHECK(riak_dt_map_get(&after.value, "admin", RIAK_DT_FLAG) == NULL);
    v = riak_dt_map_get(&after.value, "visits", RIAK_DT_COUNTER);
    CHECK(v && v->counter == 4);
    riak_datatype_free(&after);
    riak_datatype_free(&dt);
    riak_dt_op_free(m);

    /* HyperLogLog */
    riak_dt_op *h = riak_dt_op_new(RIAK_DT_HLL);
    riak_dt_op_add(h, "a");
    riak_dt_op_add(h, "b");
    riak_dt_op_add(h, "a");
    CHECK_EQ_INT(riak_dt_update(rc, "libriak_hlls", bucket, "h", h, NULL, &rb, &dt, NULL), RIAK_OK);
    CHECK(dt.value.type == RIAK_DT_HLL && dt.value.counter == 2);
    riak_datatype_free(&dt);
    riak_dt_op_free(h);

    if (has_feature("gset")) {
        riak_dt_op *g = riak_dt_op_new(RIAK_DT_GSET);
        riak_dt_op_add(g, "only-grows");
        CHECK_EQ_INT(riak_dt_update(rc, "libriak_gsets", bucket, "g", g, NULL, &rb, &dt, NULL),
                     RIAK_OK);
        CHECK(dt.value.type == RIAK_DT_GSET && dt.value.n_elements == 1);
        riak_datatype_free(&dt);
        riak_dt_op_free(g);
    }

    /* operation that does not fit the bucket type */
    riak_dt_op *wrong = riak_dt_op_new(RIAK_DT_SET);
    riak_dt_op_add(wrong, "x");
    CHECK_EQ_INT(riak_dt_update(rc, "libriak_counters", bucket, "c", wrong, NULL, NULL, NULL, NULL),
                 RIAK_E_HTTP);
    CHECK_EQ_INT(riak_client_http_status(rc), 400);
    riak_dt_op_free(wrong);
}

static void test_failover(void) {
    /* a dead node (nothing listens on port 1) ahead of the real one */
    riak_node nodes[2] = {{"127.0.0.1", 1}, {live_host, live_port}};
    riak_config cfg;
    riak_config_init(&cfg);
    cfg.nodes = nodes;
    cfg.n_nodes = 2;
    cfg.balance = RIAK_BALANCE_FAILOVER;
    cfg.timeout_ms = 30000;
    riak_client *c = NULL;
    CHECK_EQ_INT(riak_client_new(&cfg, &c), RIAK_OK);
    CHECK_EQ_INT(riak_put(c, bucket, "fo", "x", 1, NULL, NULL, NULL), RIAK_OK);
    riak_object obj;
    CHECK_EQ_INT(riak_get(c, bucket, "fo", NULL, &obj), RIAK_OK);
    riak_object_free(&obj);
    CHECK_EQ_INT(riak_delete(c, bucket, "fo", NULL), RIAK_OK);
    CHECK_EQ_INT(riak_client_check_nodes(c), 1);
    riak_node_info info;
    CHECK_EQ_INT(riak_client_node_info(c, 0, &info), RIAK_OK);
    CHECK(!info.healthy);
    riak_client_free(c);
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
    if (getenv("RIAK_TEST_FEATURES")) {
        features = getenv("RIAK_TEST_FEATURES");
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
    live_host = host;
    live_port = port;
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
    RUN(test_secondary_indexes);
    RUN(test_search);
    RUN(test_mapreduce);
    RUN(test_datatypes);
    RUN(test_failover);

    riak_client_free(rc);
    riak_global_cleanup();
    return CHECK_DONE();
}
