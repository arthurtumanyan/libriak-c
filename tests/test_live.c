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
#include <unistd.h>

#include "check.h"
#include "riak/riak.h"

static riak_client *rc;
static char bucket[64];
static const char *multi_type; /* bucket type with allow_mult, from RIAK_TEST_TYPE */
static const char *features = "";
static const char *live_host;
static riak_transport live_transport; /* pb:// or pbs:// select Protocol Buffers */
static unsigned live_http_port;       /* RIAK_TEST_HTTP_PORT, for riak_stats() over PB */
static bool live_tls;

/* RIAK_TEST_LOG=1: retries and failed attempts are printed. */
static void live_log(riak_log_level level, const char *msg, void *ctx) {
    (void)ctx;
    fprintf(stderr, "riak %s: %s\n", level >= RIAK_LOG_ERROR ? "error" : "warn", msg);
}

/* Transport, TLS and credentials of the node under test: RIAK_TEST_USER,
 * RIAK_TEST_PASSWORD, RIAK_TEST_CA and RIAK_TEST_TLS_LEGACY=1. */
static void live_config(riak_config *cfg) {
    const char *legacy = getenv("RIAK_TEST_TLS_LEGACY");
    cfg->transport = live_transport;
    cfg->tls = live_tls;
    cfg->username = getenv("RIAK_TEST_USER");
    cfg->password = getenv("RIAK_TEST_PASSWORD");
    cfg->ca_file = getenv("RIAK_TEST_CA");
    cfg->tls_legacy = legacy != NULL && strcmp(legacy, "1") == 0;
    cfg->timeout_ms = 30000;
    if (getenv("RIAK_TEST_LOG")) {
        cfg->log_fn = live_log;
        cfg->log_level = RIAK_LOG_WARN;
    }
}
static unsigned live_port; /* RIAK_TEST_FEATURES, e.g. "2i,search" */

static bool has_feature(const char *f) {
    size_t n = strlen(f);
    /* every occurrence: "repl" also appears inside "nextgenrepl" */
    for (const char *p = strstr(features, f); p; p = strstr(p + 1, f)) {
        if ((p == features || p[-1] == ',') && (p[n] == '\0' || p[n] == ',')) {
            return true;
        }
    }
    return false;
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
    if (live_transport == RIAK_TRANSPORT_PB) {
        /* ETags are HTTP only; the vector clock serves the same purpose */
        inm = (riak_opts){.if_modified = true, .vclock = obj.vclock};
    }
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
    json = NULL;
    /* a node with Search: a missing schema or index stays "not found" */
    CHECK_EQ_INT(riak_search_schema_get(rc, "libriak_no_such_schema", &json), RIAK_E_NOT_FOUND);
    CHECK_EQ_INT(riak_search_index_get(rc, "libriak_no_such_idx", &json), RIAK_E_NOT_FOUND);
    riak_search_query mq = {.q = "*:*"};
    riak_search_result mr;
    CHECK_EQ_INT(riak_search(rc, "libriak_no_such_idx", &mq, &mr), RIAK_E_NOT_FOUND);
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
    CHECK_EQ_INT(riak_mapred(rc, "{\"inputs\":", &result),
                 live_transport == RIAK_TRANSPORT_PB ? RIAK_E_SERVER : RIAK_E_HTTP);
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

    /* a bucket type that does not exist: "not found" on both transports */
    riak_dt_op *nt = riak_dt_op_new(RIAK_DT_COUNTER);
    riak_dt_op_increment(nt, 1);
    CHECK_EQ_INT(
        riak_dt_update(rc, "libriak_no_such_type", bucket, "c", nt, NULL, NULL, NULL, NULL),
        RIAK_E_NOT_FOUND);
    CHECK_EQ_INT(riak_dt_fetch(rc, "libriak_no_such_type", bucket, "c", NULL, &dt),
                 RIAK_E_NOT_FOUND);
    riak_dt_op_free(nt);

    /* HyperLogLog (Riak 2.2+; older nodes cannot create the bucket type) */
    riak_dt_op *h = riak_dt_op_new(RIAK_DT_HLL);
    riak_dt_op_add(h, "a");
    riak_dt_op_add(h, "b");
    riak_dt_op_add(h, "a");
    if (!has_feature("nohll")) {
        CHECK_EQ_INT(riak_dt_update(rc, "libriak_hlls", bucket, "h", h, NULL, &rb, &dt, NULL),
                     RIAK_OK);
        CHECK(dt.value.type == RIAK_DT_HLL && dt.value.counter == 2);
        riak_datatype_free(&dt);
    }
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
                 live_transport == RIAK_TRANSPORT_PB ? RIAK_E_SERVER : RIAK_E_HTTP);
    CHECK_EQ_INT(riak_client_http_status(rc), live_transport == RIAK_TRANSPORT_PB ? 0 : 400);
    riak_dt_op_free(wrong);
}

static void test_failover(void) {
    /* a dead node (nothing listens on port 1) ahead of the real one */
    riak_node nodes[2] = {{"127.0.0.1", 1, 0}, {live_host, live_port, 0}};
    riak_config cfg;
    riak_config_init(&cfg);
    live_config(&cfg);
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

typedef struct {
    int ok, done;
    riak_status last;
} async_tally;

static void async_count(riak_async_result *res, void *ctx) {
    async_tally *t = ctx;
    t->done++;
    t->ok += res->status == RIAK_OK;
    t->last = res->status;
}

static void test_async(void) {
    riak_config cfg;
    riak_config_init(&cfg);
    live_config(&cfg);
    cfg.host = live_host;
    cfg.port = live_port;
    cfg.timeout_ms = 120000;
    riak_async *a = NULL;
    CHECK_EQ_INT(riak_async_new(&cfg, &a), RIAK_OK);
    async_tally put = {0}, get = {0}, del = {0};
    char keys[50][16];
    riak_opts o = {.content_type = "text/plain"};
    for (int i = 0; i < 50; i++) {
        snprintf(keys[i], sizeof keys[i], "async-%d", i);
        CHECK_EQ_INT(
            riak_async_put(a, bucket, keys[i], keys[i], strlen(keys[i]), &o, async_count, &put),
            RIAK_OK);
    }
    CHECK_EQ_INT(riak_async_run(a, 120000), 0);
    CHECK_EQ_INT(put.ok, 50);
    for (int i = 0; i < 50; i++) {
        CHECK_EQ_INT(riak_async_get(a, bucket, keys[i], NULL, async_count, &get), RIAK_OK);
    }
    CHECK_EQ_INT(riak_async_run(a, 120000), 0);
    CHECK_EQ_INT(get.ok, 50);
    for (int i = 0; i < 50; i++) {
        CHECK_EQ_INT(riak_async_delete(a, bucket, keys[i], NULL, async_count, &del), RIAK_OK);
    }
    CHECK_EQ_INT(riak_async_run(a, 120000), 0);
    CHECK_EQ_INT(del.ok, 50);
    riak_async_free(a);
}

typedef struct {
    int done;
    riak_status status;
    riak_async_result kept;
} live_grab;

static void live_take(riak_async_result *res, void *ctx) {
    live_grab *g = ctx;
    g->done++;
    g->status = res->status;
    g->kept = *res;
    memset(&res->siblings, 0, sizeof res->siblings);
    memset(&res->datatype, 0, sizeof res->datatype);
    memset(&res->index, 0, sizeof res->index);
    res->json = NULL;
    res->continuation = NULL;
    if (res->status != RIAK_OK) {
        fprintf(stderr, "  async: %s\n", res->errmsg);
    }
}

static riak_status live_wait(riak_async *a, live_grab *g) {
    while (g->done == 0 && riak_async_run(a, 30000) > 0) {
    }
    CHECK_EQ_INT(g->done, 1);
    return g->status;
}

static void live_grab_free(live_grab *g) {
    riak_object_set_free(&g->kept.siblings);
    riak_datatype_free(&g->kept.datatype);
    riak_index_result_free(&g->kept.index);
    riak_free(g->kept.json);
    riak_free(g->kept.continuation);
    memset(g, 0, sizeof *g);
}

static bool live_count(const char *key, const char *term, void *ctx) {
    (void)key;
    (void)term;
    ++*(int *)ctx;
    return true;
}

static bool live_count_name(const char *name, size_t len, void *ctx) {
    (void)name;
    (void)len;
    ++*(int *)ctx;
    return true;
}

/* The data path over the async handle, compared with the blocking calls. */
static void test_async_data_path(void) {
    riak_config cfg;
    riak_config_init(&cfg);
    live_config(&cfg);
    cfg.host = live_host;
    cfg.port = live_port;
    cfg.http_port = live_http_port;
    riak_async *a = NULL;
    CHECK_EQ_INT(riak_async_new(&cfg, &a), RIAK_OK);
    if (a == NULL) return;
    live_grab g = {0};
    char ab[96];
    snprintf(ab, sizeof ab, "%s-async", bucket);

    /* keys with an index entry; listed, queried and folded by MapReduce */
    for (int i = 0; i < 3; i++) {
        char key[8];
        snprintf(key, sizeof key, "a%d", i);
        riak_kv_pair idx[] = {{"grp_bin", "all"}};
        riak_opts o = {.content_type = "application/json", .indexes = idx, .n_indexes = 1};
        CHECK_EQ_INT(riak_put(rc, ab, key, "\"v\"", 3, &o, NULL, NULL), RIAK_OK);
    }
    int listed = 0;
    CHECK_EQ_INT(riak_async_list_keys_stream(a, NULL, ab, live_count_name, &listed, live_take, &g),
                 RIAK_OK);
    CHECK_EQ_INT(live_wait(a, &g), RIAK_OK);
    CHECK_EQ_INT(listed, 3);
    live_grab_free(&g);

    if (has_feature("2i")) {
        riak_index_query q = {.index = "grp_bin", .value = "all"};
        CHECK_EQ_INT(riak_async_index_query(a, ab, &q, live_take, &g), RIAK_OK);
        CHECK_EQ_INT(live_wait(a, &g), RIAK_OK);
        CHECK_EQ_INT(g.kept.index.count, 3);
        live_grab_free(&g);
        int streamed = 0;
        CHECK_EQ_INT(riak_async_index_query_stream(a, ab, &q, live_count, &streamed, live_take, &g),
                     RIAK_OK);
        CHECK_EQ_INT(live_wait(a, &g), RIAK_OK);
        CHECK_EQ_INT(streamed, 3);
        live_grab_free(&g);
    }

    char job[512];
    snprintf(job, sizeof job,
             "{\"inputs\":[[\"%s\",\"a0\"],[\"%s\",\"a1\"]],\"query\":["
             "{\"map\":{\"language\":\"erlang\",\"module\":\"riak_kv_mapreduce\","
             "\"function\":\"map_object_value\"}},"
             "{\"reduce\":{\"language\":\"erlang\",\"module\":\"riak_kv_mapreduce\","
             "\"function\":\"reduce_count_inputs\"}}]}",
             ab, ab);
    CHECK_EQ_INT(riak_async_mapred(a, job, live_take, &g), RIAK_OK);
    CHECK_EQ_INT(live_wait(a, &g), RIAK_OK);
    CHECK_EQ_STR(g.kept.json, "[2]");
    live_grab_free(&g);

    char *props = NULL;
    CHECK_EQ_INT(riak_get_bucket_props(rc, ab, &props), RIAK_OK);
    CHECK_EQ_INT(riak_async_get_bucket_props(a, NULL, ab, live_take, &g), RIAK_OK);
    CHECK_EQ_INT(live_wait(a, &g), RIAK_OK);
    CHECK_EQ_STR(g.kept.json, props);
    riak_free(props);
    live_grab_free(&g);

    if (multi_type) {
        riak_opts m = {.bucket_type = multi_type, .content_type = "text/plain"};
        CHECK_EQ_INT(riak_put(rc, ab, "sib", "one", 3, &m, NULL, NULL), RIAK_OK);
        CHECK_EQ_INT(riak_put(rc, ab, "sib", "two", 3, &m, NULL, NULL), RIAK_OK);
        CHECK_EQ_INT(riak_async_get_siblings(a, ab, "sib", &m, live_take, &g), RIAK_OK);
        CHECK_EQ_INT(live_wait(a, &g), RIAK_OK);
        CHECK_EQ_INT(g.kept.siblings.count, 2);
        live_grab_free(&g);
        riak_delete(rc, ab, "sib", &m);
    }

    if (has_feature("dt")) {
        riak_dt_op *op = riak_dt_op_new(RIAK_DT_COUNTER);
        riak_dt_op_increment(op, 3);
        riak_dt_opts rb = {.return_body = true};
        CHECK_EQ_INT(
            riak_async_dt_update(a, "libriak_counters", ab, "c", op, NULL, &rb, live_take, &g),
            RIAK_OK);
        riak_dt_op_free(op);
        CHECK_EQ_INT(live_wait(a, &g), RIAK_OK);
        long long after = g.kept.datatype.value.counter;
        live_grab_free(&g);
        CHECK_EQ_INT(riak_async_dt_fetch(a, "libriak_counters", ab, "c", NULL, live_take, &g),
                     RIAK_OK);
        CHECK_EQ_INT(live_wait(a, &g), RIAK_OK);
        CHECK_EQ_INT(g.kept.datatype.value.counter, after);
        live_grab_free(&g);
    }

    if (has_feature("counters")) {
        char cb[112];
        snprintf(cb, sizeof cb, "%s-counters", ab); /* legacy counters need allow_mult */
        CHECK_EQ_INT(riak_set_bucket_props(rc, cb, "{\"props\":{\"allow_mult\":true}}"), RIAK_OK);
        CHECK_EQ_INT(riak_async_counter_update(a, cb, "hits", 4, NULL, live_take, &g), RIAK_OK);
        CHECK_EQ_INT(live_wait(a, &g), RIAK_OK);
        int64_t value = g.kept.counter;
        live_grab_free(&g);
        CHECK_EQ_INT(riak_async_counter_get(a, cb, "hits", NULL, live_take, &g), RIAK_OK);
        CHECK_EQ_INT(live_wait(a, &g), RIAK_OK);
        CHECK_EQ_INT(g.kept.counter, value);
        live_grab_free(&g);
    }

    if (!has_feature("search")) {
        riak_search_query sq = {.q = "*:*"};
        CHECK_EQ_INT(riak_async_search(a, "libriak_none", &sq, live_take, &g), RIAK_OK);
        CHECK_EQ_INT(live_wait(a, &g), RIAK_E_UNSUPPORTED);
        live_grab_free(&g);
    }

    for (int i = 0; i < 3; i++) {
        char key[8];
        snprintf(key, sizeof key, "a%d", i);
        riak_delete(rc, ab, key, NULL);
    }
    riak_async_free(a);
}

static bool count_name(const char *name, size_t len, void *ctx) {
    (void)name;
    (void)len;
    (*(size_t *)ctx)++;
    return true;
}

typedef struct {
    size_t matches;
} cover_tally;

static bool count_match(const char *key, const char *term, void *ctx) {
    (void)key;
    (void)term;
    ((cover_tally *)ctx)->matches++;
    return true;
}

static void test_server_info_preflist_coverage(void) {
    riak_server_info info;
    CHECK_EQ_INT(riak_get_server_info(rc, &info), RIAK_OK);
    CHECK(info.node && strchr(info.node, '@') != NULL);
    CHECK(info.version && *info.version);
    riak_server_info_free(&info);

    riak_opts o = {.content_type = "text/plain"};
    CHECK_EQ_INT(riak_put(rc, bucket, "pl", "x", 1, &o, NULL, NULL), RIAK_OK);
    riak_preflist pl;
    riak_status pst = riak_get_preflist(rc, NULL, bucket, "pl", &pl);
    if (live_transport == RIAK_TRANSPORT_HTTP || has_feature("nopreflist")) {
        /* Protocol Buffers only, and Riak 2.1+ */
        CHECK_EQ_INT(pst, RIAK_E_UNSUPPORTED);
    } else {
        /* n_val primaries, and a list that depends on the key */
        CHECK_EQ_INT(pst, RIAK_OK);
        CHECK_EQ_INT(pl.count, 3);
        long long first = pl.count ? pl.items[0].partition : -1;
        bool varies = false;
        for (int i = 0; i < 16 && !varies && pst == RIAK_OK; i++) {
            char k[16];
            snprintf(k, sizeof k, "pl-%d", i);
            riak_preflist other;
            CHECK_EQ_INT(riak_get_preflist(rc, NULL, bucket, k, &other), RIAK_OK);
            varies = other.count > 0 && other.items[0].partition != first;
            riak_preflist_free(&other);
        }
        CHECK(varies);
    }
    riak_preflist_free(&pl);
    size_t keys = 0;
    CHECK_EQ_INT(riak_list_keys_stream(rc, NULL, bucket, count_name, &keys), RIAK_OK);
    CHECK(keys >= 1);

    if (live_transport != RIAK_TRANSPORT_PB || !has_feature("2i")) {
        riak_delete(rc, bucket, "pl", NULL);
        return;
    }
    /* a coverage plan splits a $bucket query; together the parts find the key */
    riak_coverage_plan plan;
    riak_status st = riak_get_coverage(rc, NULL, bucket, 0, &plan);
    if (!has_feature("coverage")) {
        /* coverage plans are served by Riak TS only, by no Riak KV release */
        CHECK_EQ_INT(st, RIAK_E_UNSUPPORTED);
        CHECK(plan.entries == NULL && plan.count == 0);
        riak_delete(rc, bucket, "pl", NULL);
        return;
    }
    CHECK_EQ_INT(st, RIAK_OK);
    CHECK(plan.count >= 1);
    cover_tally t = {0};
    for (size_t i = 0; i < plan.count; i++) {
        riak_index_query q = {
            .index = "$bucket", .value = bucket, .cover_context = plan.entries[i].cover_context};
        CHECK_EQ_INT(riak_index_query_stream(rc, bucket, &q, count_match, &t, NULL), RIAK_OK);
    }
    CHECK(t.matches >= 1);
    riak_coverage_free(&plan);
    riak_delete(rc, bucket, "pl", NULL);
}

/* ---- Riak 2.9+/3.x features ------------------------------------------- */

static bool is_pb(void) {
    return live_transport == RIAK_TRANSPORT_PB;
}

static void test_new_options(void) {
    riak_opts put = {
        .content_type = "text/plain", .node_confirms = 1, .sync_on_write = RIAK_SYNC_ONE};
    CHECK_EQ_INT(riak_put(rc, bucket, "opt", "v1", 2, &put, NULL, NULL), RIAK_OK);
    riak_object obj;
    riak_opts get = {.node_confirms = 1};
    CHECK_EQ_INT(riak_get(rc, bucket, "opt", &get, &obj), RIAK_OK);
    CHECK_EQ_STR(obj.data, "v1");
    riak_object_free(&obj);

    riak_opts head = {.content_type = "text/plain", .return_head = true};
    riak_status st = riak_put(rc, bucket, "opt", "v2", 2, &head, &obj, NULL);
    if (is_pb()) {
        CHECK_EQ_INT(st, RIAK_OK);
        CHECK(obj.vclock != NULL);
        CHECK_EQ_INT(obj.len, 0);
        riak_object_free(&obj);
    } else {
        CHECK_EQ_INT(st, RIAK_E_UNSUPPORTED);
    }

    CHECK_EQ_INT(riak_delete(rc, bucket, "opt", NULL), RIAK_OK);
    riak_opts dv = {.deletedvclock = true};
    st = riak_get(rc, bucket, "opt", &dv, &obj);
    CHECK_EQ_INT(st, is_pb() ? RIAK_E_NOT_FOUND : RIAK_E_UNSUPPORTED);
    if (is_pb()) {
        /* Riak keeps a tombstone for 3 s (delete_mode): its clock is returned */
        CHECK(obj.vclock != NULL && obj.deleted);
    }
    riak_object_free(&obj);

    riak_index_query q = {.index = "$bucket", .value = bucket, .pagination_sort = true};
    riak_index_result r;
    if (has_feature("2i")) {
        CHECK_EQ_INT(riak_index_query_run(rc, bucket, &q, &r), RIAK_OK);
        riak_index_result_free(&r);
    }
}

static void test_type_props_and_client_id(void) {
    if (multi_type) {
        CHECK_EQ_INT(riak_set_type_props(rc, multi_type, "{\"props\":{\"notfound_ok\":false}}"),
                     RIAK_OK);
        char *json = NULL;
        CHECK_EQ_INT(riak_get_type_props(rc, multi_type, &json), RIAK_OK);
        CHECK(json && strstr(json, "\"notfound_ok\":false"));
        riak_free(json);
        CHECK_EQ_INT(riak_set_type_props(rc, multi_type, "{\"props\":{\"notfound_ok\":true}}"),
                     RIAK_OK);
        CHECK_EQ_INT(riak_get_type_props(rc, multi_type, &json), RIAK_OK);
        CHECK(json && strstr(json, "\"notfound_ok\":true"));
        riak_free(json);
    }
    char *id = NULL;
    CHECK_EQ_INT(riak_get_client_id(rc, &id), is_pb() ? RIAK_OK : RIAK_E_UNSUPPORTED);
    CHECK(!is_pb() || (id && *id));
    riak_free(id);
}

static void test_legacy_counters(void) {
    if (!has_feature("counters")) {
        return;
    }
    char cb[96];
    snprintf(cb, sizeof cb, "%s-counters", bucket);
    /* 1.4 counters need allow_mult on their bucket */
    CHECK_EQ_INT(riak_set_bucket_props(rc, cb, "{\"props\":{\"allow_mult\":true}}"), RIAK_OK);
    int64_t v = 0;
    CHECK_EQ_INT(riak_counter_get(rc, cb, "c", NULL, &v), RIAK_E_NOT_FOUND);
    CHECK_EQ_INT(riak_counter_update(rc, cb, "c", 5, NULL, NULL), RIAK_OK);
    CHECK_EQ_INT(riak_counter_update(rc, cb, "c", -2, NULL, &v), RIAK_OK);
    CHECK_EQ_INT(v, 3);
    CHECK_EQ_INT(riak_counter_get(rc, cb, "c", NULL, &v), RIAK_OK);
    CHECK_EQ_INT(v, 3);
    riak_delete(rc, cb, "c", NULL);
}

typedef struct {
    char keys[8][16];
    int n;
} fold_seen;

static bool fold_cb(const char *key, riak_object *obj, void *ctx) {
    fold_seen *f = ctx;
    if (f->n < 8) snprintf(f->keys[f->n], sizeof f->keys[0], "%s", key);
    f->n++;
    CHECK(obj->data && strncmp(obj->data, "fold-", 5) == 0);
    return true;
}

static void test_fold_objects(void) {
    if (!has_feature("fold")) {
        return;
    }
    char fb[96];
    snprintf(fb, sizeof fb, "%s-fold", bucket);
    riak_opts o = {.content_type = "text/plain"};
    for (int i = 1; i <= 5; i++) {
        char k[8], v[16];
        snprintf(k, sizeof k, "f%d", i);
        snprintf(v, sizeof v, "fold-%d", i);
        CHECK_EQ_INT(riak_put(rc, fb, k, v, strlen(v), &o, NULL, NULL), RIAK_OK);
    }
    fold_seen f = {0};
    riak_fold_query q = {.start_key = "f2", .end_key = "f4"};
    riak_status st = riak_fold_objects(rc, fb, &q, fold_cb, &f, NULL);
    if (!is_pb()) {
        CHECK_EQ_INT(st, RIAK_E_UNSUPPORTED);
    } else {
        CHECK_EQ_INT(st, RIAK_OK);
        CHECK_EQ_INT(f.n, 2); /* f2 and f3: the end key is excluded by default */
        CHECK_EQ_STR(f.keys[0], "f2");
        q.end_inclusive = true;
        q.start_exclusive = true;
        f.n = 0;
        CHECK_EQ_INT(riak_fold_objects(rc, fb, &q, fold_cb, &f, NULL), RIAK_OK);
        CHECK_EQ_INT(f.n, 2); /* f3 and f4 */
        CHECK_EQ_STR(f.keys[1], "f4");
    }
    for (int i = 1; i <= 5; i++) {
        char k[8];
        snprintf(k, sizeof k, "f%d", i);
        riak_delete(rc, fb, k, NULL);
    }
}

static void test_link_walk(void) {
    if (!has_feature("linkwalk")) {
        return;
    }
    char lb[96], link1[256], link2[256];
    snprintf(lb, sizeof lb, "%s-links", bucket);
    snprintf(link1, sizeof link1, "</buckets/%s/keys/bob>; riaktag=\"friend\"", lb);
    snprintf(link2, sizeof link2, "</buckets/%s/keys/carol>; riaktag=\"boss\"", lb);
    const char *links[] = {link1, link2};
    riak_opts with = {.content_type = "text/plain", .links = links, .n_links = 2};
    riak_opts plain = {.content_type = "text/plain"};
    const char *bob_links[] = {link2};
    riak_opts bobo = {.content_type = "text/plain", .links = bob_links, .n_links = 1};
    CHECK_EQ_INT(riak_put(rc, lb, "alice", "A", 1, &with, NULL, NULL), RIAK_OK);
    CHECK_EQ_INT(riak_put(rc, lb, "bob", "B", 1, &bobo, NULL, NULL), RIAK_OK);
    CHECK_EQ_INT(riak_put(rc, lb, "carol", "C", 1, &plain, NULL, NULL), RIAK_OK);

    riak_link_step friends[] = {{.bucket = NULL, .tag = "friend"}};
    riak_link_result res;
    if (getenv("RIAK_TEST_USER")) {
        /* Riak refuses link walking once security is enabled */
        CHECK_EQ_INT(riak_link_walk(rc, lb, "alice", friends, 1, &res), RIAK_E_UNSUPPORTED);
        riak_link_result_free(&res);
        const char *gone[] = {"alice", "bob", "carol"};
        for (int i = 0; i < 3; i++)
            riak_delete(rc, lb, gone[i], NULL);
        return;
    }
    CHECK_EQ_INT(riak_link_walk(rc, lb, "alice", friends, 1, &res), RIAK_OK);
    CHECK_EQ_INT(res.n_phases, 1);
    if (res.n_phases == 1) {
        CHECK_EQ_INT(res.phases[0].count, 1);
        if (res.phases[0].count == 1) {
            CHECK_EQ_STR(res.phases[0].items[0].key, "bob");
            CHECK_EQ_STR(res.phases[0].items[0].bucket, lb);
            CHECK_EQ_STR(res.phases[0].items[0].object.data, "B");
        }
    }
    riak_link_result_free(&res);

    /* alice -> any (kept) -> boss: bob and carol, then carol */
    riak_link_step two[] = {{.keep = true}, {.tag = "boss"}};
    CHECK_EQ_INT(riak_link_walk(rc, lb, "alice", two, 2, &res), RIAK_OK);
    CHECK_EQ_INT(res.n_phases, 2);
    if (res.n_phases == 2) {
        CHECK_EQ_INT(res.phases[0].count, 2);
        CHECK_EQ_INT(res.phases[1].count, 1);
        if (res.phases[1].count == 1) CHECK_EQ_STR(res.phases[1].items[0].key, "carol");
    }
    riak_link_result_free(&res);
    const char *keys[] = {"alice", "bob", "carol"};
    for (int i = 0; i < 3; i++)
        riak_delete(rc, lb, keys[i], NULL);
}

static void test_query_api(void) {
    if (!has_feature("query")) {
        return;
    }
    char qb[96];
    snprintf(qb, sizeof qb, "%s-query", bucket);
    for (int i = 1; i <= 4; i++) {
        char k[8], term[32];
        snprintf(k, sizeof k, "p%d", i);
        snprintf(term, sizeof term, "1980010%d|name%d", i, i);
        riak_kv_pair idx[] = {{"people_bin", term}};
        riak_opts o = {.content_type = "text/plain", .indexes = idx, .n_indexes = 1};
        CHECK_EQ_INT(riak_put(rc, qb, k, "x", 1, &o, NULL, NULL), RIAK_OK);
    }
    riak_query_part part = {
        .index_name = "people_bin", .start_term = "19800101", .end_term = "19800109"};
    riak_query q = {.parts = &part, .n_parts = 1};
    riak_query_result r;
    CHECK_EQ_INT(riak_query_run(rc, qb, &q, &r), RIAK_OK);
    CHECK_EQ_INT(r.count, 4);
    if (r.count == 4) CHECK_EQ_STR(r.keys[0], "p1");
    CHECK(r.terms == NULL && r.total == -1);
    riak_query_result_free(&r);

    q.accumulation_option = "terms";
    CHECK_EQ_INT(riak_query_run(rc, qb, &q, &r), RIAK_OK);
    CHECK_EQ_INT(r.count, 4);
    if (r.count == 4 && r.terms) {
        CHECK_EQ_STR(r.terms[1], "19800102|name2");
        CHECK_EQ_STR(r.keys[1], "p2");
    }
    riak_query_result_free(&r);

    q.accumulation_option = "count";
    CHECK_EQ_INT(riak_query_run(rc, qb, &q, &r), RIAK_OK);
    CHECK_EQ_INT(r.total, 4);
    riak_query_result_free(&r);

    /* evaluation + filter with a substitution */
    riak_query_part eval = {.index_name = "people_bin",
                            .start_term = "0",
                            .end_term = "z",
                            .evaluation_expression = "delim($term, \"|\", ($dob, $name))",
                            .filter_expression = "$name = :n"};
    riak_kv_pair subs[] = {{"n", "name3"}};
    riak_query qe = {.parts = &eval, .n_parts = 1, .substitutions = subs, .n_substitutions = 1};
    CHECK_EQ_INT(riak_query_run(rc, qb, &qe, &r), RIAK_OK);
    CHECK_EQ_INT(r.count, 1);
    if (r.count == 1) CHECK_EQ_STR(r.keys[0], "p3");
    riak_query_result_free(&r);

    /* aggregation of two tagged queries */
    /* terms are "date|name", so the range end 19800104 covers 19800103|name3 */
    riak_query_part two[] = {{.aggregation_tag = 1,
                              .index_name = "people_bin",
                              .start_term = "19800101",
                              .end_term = "19800104"},
                             {.aggregation_tag = 2,
                              .index_name = "people_bin",
                              .start_term = "19800102",
                              .end_term = "19800109"}};
    riak_query qa = {.parts = two,
                     .n_parts = 2,
                     .aggregation_expression = "$1 INTERSECT $2",
                     .accumulation_option = "raw_count"};
    CHECK_EQ_INT(riak_query_run(rc, qb, &qa, &r), RIAK_OK);
    CHECK_EQ_INT(r.total, 2); /* p2 and p3 */
    riak_query_result_free(&r);

    /* pages of max_results, chained by the continuation: every match once */
    const char *paged[] = {"terms", "raw_keys"};
    for (int m = 0; m < 2; m++) {
        riak_query pq = {
            .parts = &part, .n_parts = 1, .accumulation_option = paged[m], .max_results = 3};
        bool seen[5] = {false};
        int pages = 0, total = 0;
        char *cont = NULL;
        for (; pages < 10; pages++) {
            pq.continuation = cont;
            riak_status pst = riak_query_run(rc, qb, &pq, &r);
            CHECK_EQ_INT(pst, RIAK_OK);
            free(cont);
            cont = NULL;
            if (pst != RIAK_OK) {
                fprintf(stderr, "  %s page %d: %s\n", paged[m], pages, riak_client_errmsg(rc));
                break;
            }
            CHECK(r.count <= 3);
            for (size_t i = 0; i < r.count; i++) {
                int n = r.keys[i][0] == 'p' ? atoi(r.keys[i] + 1) : 0;
                CHECK(n >= 1 && n <= 4 && !seen[n]);
                if (n >= 1 && n <= 4) seen[n] = true;
                total++;
            }
            cont = r.continuation ? strdup(r.continuation) : NULL;
            riak_query_result_free(&r);
            if (cont == NULL) break;
        }
        free(cont);
        CHECK_EQ_INT(total, 4);
        CHECK_EQ_INT(pages, 1); /* 3 + 1 results: the second page is the last */
    }

    /* queued results, fetched in batches */
    q.accumulation_option = "queue_raw_keys";
    CHECK_EQ_INT(riak_query_run(rc, qb, &q, &r), RIAK_OK);
    char *queue = r.result_queue ? strdup(r.result_queue) : NULL;
    riak_query_result_free(&r);
    CHECK(queue != NULL);
    size_t got = 0;
    for (int i = 0; queue && i < 20; i++) {
        CHECK_EQ_INT(riak_query_fetch(rc, NULL, qb, queue, 3, &r), RIAK_OK);
        got += r.count;
        bool done = r.complete;
        riak_query_result_free(&r);
        if (done) break;
        sleep_ms(200);
    }
    CHECK_EQ_INT(got, 4);
    free(queue);
    for (int i = 1; i <= 4; i++) {
        char k[8];
        snprintf(k, sizeof k, "p%d", i);
        riak_delete(rc, qb, k, NULL);
    }
}

static void test_aae_folds(void) {
    if (!has_feature("aae")) {
        return;
    }
    char ab[96];
    snprintf(ab, sizeof ab, "%s-aae", bucket);
    riak_opts o = {.content_type = "text/plain"};
    for (int i = 0; i < 10; i++) {
        char k[8];
        snprintf(k, sizeof k, "a%d", i);
        CHECK_EQ_INT(riak_put(rc, ab, k, "aae-value", 9, &o, NULL, NULL), RIAK_OK);
    }
    riak_aae_filter f = {.bucket = ab};
    riak_aae_keys keys;
    CHECK_EQ_INT(riak_aae_fetch_clocks_range(rc, &f, &keys), RIAK_OK);
    CHECK_EQ_INT(keys.count, 10);
    if (keys.count > 0) {
        CHECK_EQ_STR(keys.items[0].bucket, ab);
        CHECK(keys.items[0].vclock && *keys.items[0].vclock);
        /* the clock is a regular vclock: it is accepted by a put */
        riak_opts upd = {.content_type = "text/plain", .vclock = keys.items[0].vclock};
        CHECK_EQ_INT(riak_put(rc, ab, keys.items[0].key, "aae-value", 9, &upd, NULL, NULL),
                     RIAK_OK);
    }
    riak_aae_keys_free(&keys);

    f.start_key = "a2";
    f.end_key = "a4";
    CHECK_EQ_INT(riak_aae_fetch_clocks_range(rc, &f, &keys), RIAK_OK);
    CHECK_EQ_INT(keys.count, 3);
    riak_aae_keys_free(&keys);
    f.start_key = f.end_key = NULL;

    riak_aae_tree tree;
    CHECK_EQ_INT(riak_aae_merge_range(rc, &f, RIAK_AAE_XXSMALL, &tree), RIAK_OK);
    CHECK_EQ_INT(tree.size, RIAK_AAE_XXSMALL);
    CHECK_EQ_INT(tree.level1_len, 16 * 4);
    CHECK_EQ_INT(tree.n_branches, 16);
    size_t nonzero = 0;
    for (size_t i = 0; i < tree.level1_len; i++)
        nonzero += tree.level1[i] != 0;
    CHECK(nonzero > 0);
    if (tree.n_branches > 0) CHECK_EQ_INT(tree.branches[0].len, 256 * 4);
    riak_aae_tree_free(&tree);

    riak_aae_key_counts kc;
    CHECK_EQ_INT(riak_aae_find_keys(rc, &f, RIAK_AAE_OBJECT_SIZE, 1, &kc), RIAK_OK);
    CHECK_EQ_INT(kc.count, 10);
    if (kc.count > 0) CHECK(kc.items[0].value > 9);
    riak_aae_key_counts_free(&kc);
    CHECK_EQ_INT(riak_aae_find_keys(rc, &f, RIAK_AAE_SIBLING_COUNT, 1, &kc), RIAK_OK);
    CHECK_EQ_INT(kc.count, 0);
    riak_aae_key_counts_free(&kc);

    riak_aae_stats stats;
    CHECK_EQ_INT(riak_aae_object_stats(rc, &f, &stats), RIAK_OK);
    CHECK_EQ_INT(stats.total_count, 10);
    CHECK(stats.total_size > 90);
    CHECK(stats.n_sizes >= 1 && stats.n_siblings >= 1);
    riak_aae_stats_free(&stats);

    uint64_t count = 0;
    CHECK_EQ_INT(riak_aae_erase_keys(rc, &f, RIAK_AAE_COUNT, 0, &count), RIAK_OK);
    CHECK_EQ_INT(count, 10);
    CHECK_EQ_INT(riak_aae_reap_tombs(rc, &f, RIAK_AAE_COUNT, 0, &count), RIAK_OK);
    CHECK_EQ_INT(count, 0);
    CHECK_EQ_INT(riak_aae_find_tombs(rc, &f, &keys), RIAK_OK);
    riak_aae_keys_free(&keys);
    CHECK_EQ_INT(riak_aae_repair_keys(rc, &f, &count), RIAK_OK);
    CHECK_EQ_INT(count, 10);
    if (has_feature("nextgenrepl")) {
        CHECK_EQ_INT(riak_aae_repl_keys(rc, &f, "libriak_q", &count), RIAK_OK);
        CHECK_EQ_INT(count, 10);
    }

    riak_aae_buckets buckets;
    CHECK_EQ_INT(riak_aae_list_buckets(rc, 3, &buckets), RIAK_OK);
    bool found = false;
    for (size_t i = 0; i < buckets.count; i++)
        found = found || strcmp(buckets.items[i].bucket, ab) == 0;
    CHECK(found);
    riak_aae_buckets_free(&buckets);

    /* the cached n_val trees: root, its branches, and the clocks of a segment */
    CHECK_EQ_INT(riak_aae_merge_root(rc, 3, &tree), RIAK_OK);
    CHECK_EQ_INT(tree.size, RIAK_AAE_LARGE);
    CHECK_EQ_INT(tree.level1_len, 4096 * 4);
    uint32_t branch = 0;
    for (size_t i = 0; i + 4 <= tree.level1_len; i += 4) {
        if (tree.level1[i] | tree.level1[i + 1] | tree.level1[i + 2] | tree.level1[i + 3]) {
            branch = (uint32_t)(i / 4);
            break;
        }
    }
    riak_aae_tree_free(&tree);
    CHECK_EQ_INT(riak_aae_merge_branches(rc, 3, &branch, 1, &tree), RIAK_OK);
    CHECK_EQ_INT(tree.n_branches, 1);
    uint32_t segment = branch * 256;
    if (tree.n_branches == 1) {
        CHECK_EQ_INT(tree.branches[0].id, branch);
        CHECK_EQ_INT(tree.branches[0].len, 256 * 4);
        for (size_t i = 0; i + 4 <= tree.branches[0].len; i += 4) {
            const uint8_t *h = tree.branches[0].data + i;
            if (h[0] | h[1] | h[2] | h[3]) {
                segment = branch * 256 + (uint32_t)(i / 4);
                break;
            }
        }
    }
    riak_aae_tree_free(&tree);
    CHECK_EQ_INT(riak_aae_fetch_clocks_nval(rc, 3, &segment, 1, &keys), RIAK_OK);
    CHECK(keys.count >= 1);
    riak_aae_keys_free(&keys);

    for (int i = 0; i < 10; i++) {
        char k[8];
        snprintf(k, sizeof k, "a%d", i);
        riak_delete(rc, ab, k, NULL);
    }
}

static void test_nextgenrepl(void) {
    if (!has_feature("nextgenrepl")) {
        return;
    }
    riak_members m;
    CHECK_EQ_INT(riak_cluster_membership(rc, &m), RIAK_OK);
    CHECK(m.count >= 1);
    if (m.count >= 1) CHECK(m.items[0].ip && m.items[0].port > 0);
    riak_members_free(&m);

    /* drain whatever earlier tests queued, then queue one change */
    riak_repl_fetch_result fr;
    for (int i = 0; i < 2000; i++) {
        riak_status fst = riak_repl_fetch(rc, "libriak_q", &fr);
        if (fst == RIAK_E_NOT_FOUND) {
            continue; /* a queued key deleted since (the AAE test queues and deletes) */
        }
        if (fst != RIAK_OK) {
            fprintf(stderr, "fetch %d: %s (%s)\n", i, riak_strerror(fst), riak_client_errmsg(rc));
        }
        CHECK_EQ_INT(fst, RIAK_OK);
        bool empty = fr.queue_empty;
        riak_repl_fetch_result_free(&fr);
        if (empty) break;
    }
    char rb[96];
    snprintf(rb, sizeof rb, "%s-repl", bucket);
    riak_opts o = {.content_type = "text/plain", .return_body = true};
    riak_object obj;
    CHECK_EQ_INT(riak_put(rc, rb, "r1", "replicate-me", 12, &o, &obj, NULL), RIAK_OK);
    bool seen = false;
    for (int i = 0; i < 50 && !seen; i++) {
        CHECK_EQ_INT(riak_repl_fetch(rc, "libriak_q", &fr), RIAK_OK);
        if (!fr.queue_empty) {
            seen = true;
            CHECK(fr.object && fr.object_len > 12);
            CHECK(fr.has_crc && !fr.deleted);
        } else {
            sleep_ms(100);
        }
        riak_repl_fetch_result_free(&fr);
    }
    CHECK(seen);

    riak_repl_key key = {.bucket = rb, .key = "r1", .vclock = obj.vclock};
    riak_repl_push_result pr;
    riak_status st = riak_repl_push(rc, "libriak_q", &key, 1, &pr);
    if (is_pb()) {
        CHECK_EQ_INT(st, RIAK_OK);
        CHECK(pr.queue_exists);
        /* the pushed key comes back from the queue */
        seen = false;
        for (int i = 0; i < 50 && !seen; i++) {
            CHECK_EQ_INT(riak_repl_fetch(rc, "libriak_q", &fr), RIAK_OK);
            seen = !fr.queue_empty;
            riak_repl_fetch_result_free(&fr);
            if (!seen) sleep_ms(100);
        }
        CHECK(seen);
    } else {
        CHECK_EQ_INT(st, RIAK_E_UNSUPPORTED);
    }
    riak_object_free(&obj);
    riak_delete(rc, rb, "r1", NULL);
}

static void test_riak_repl(void) {
    if (!has_feature("repl")) {
        return;
    }
    char *id = NULL;
    riak_status st = riak_get_cluster_id(rc, &id);
    if (!is_pb()) {
        CHECK_EQ_INT(st, RIAK_E_UNSUPPORTED);
        return;
    }
    CHECK_EQ_INT(st, RIAK_OK);
    CHECK(id && *id);
    riak_opts o = {.content_type = "text/plain"};
    CHECK_EQ_INT(riak_put(rc, bucket, "rg", "proxied", 7, &o, NULL, NULL), RIAK_OK);
    riak_object obj;
    CHECK_EQ_INT(riak_repl_get(rc, id ? id : "x", bucket, "rg", NULL, &obj), RIAK_OK);
    CHECK_EQ_STR(obj.data, "proxied");
    riak_object_free(&obj);
    CHECK_EQ_INT(riak_repl_get(rc, id ? id : "x", bucket, "missing", NULL, &obj), RIAK_E_NOT_FOUND);
    riak_object_free(&obj);
    CHECK_EQ_INT(riak_ping(rc), RIAK_OK); /* the connection is still usable */
    riak_free(id);
    riak_delete(rc, bucket, "rg", NULL);
}

/* Features a node does not offer fail with RIAK_E_UNSUPPORTED, never with
 * a misleading "not found" (older releases, or Riak 3.x without Search). */
static void test_missing_features_unsupported(void) {
    if (!has_feature("aae")) {
        riak_aae_filter f = {.bucket = bucket};
        riak_aae_keys k;
        CHECK_EQ_INT(riak_aae_fetch_clocks_range(rc, &f, &k), RIAK_E_UNSUPPORTED);
        riak_aae_tree t;
        CHECK_EQ_INT(riak_aae_merge_root(rc, 3, &t), RIAK_E_UNSUPPORTED);
    }
    if (!has_feature("nextgenrepl")) {
        riak_repl_fetch_result fr;
        CHECK_EQ_INT(riak_repl_fetch(rc, "libriak_q", &fr), RIAK_E_UNSUPPORTED);
        riak_members m;
        CHECK_EQ_INT(riak_cluster_membership(rc, &m), RIAK_E_UNSUPPORTED);
    }
    if (!has_feature("query")) {
        riak_query_part p = {.index_name = "x_bin", .start_term = "a", .end_term = "b"};
        riak_query q = {.parts = &p, .n_parts = 1};
        riak_query_result r;
        CHECK_EQ_INT(riak_query_run(rc, bucket, &q, &r), RIAK_E_UNSUPPORTED);
    }
    if (!has_feature("repl")) {
        char *id = NULL;
        CHECK_EQ_INT(riak_get_cluster_id(rc, &id), RIAK_E_UNSUPPORTED);
    }
    if (!has_feature("search")) {
        /* a node without Riak Search (Riak 3.x): every Search call, both
         * transports */
        riak_search_query sq = {.q = "*:*"};
        riak_search_result sr;
        char *json = NULL;
        CHECK_EQ_INT(riak_search(rc, "missing_index", &sq, &sr), RIAK_E_UNSUPPORTED);
        CHECK_EQ_INT(riak_search_index_create(rc, "libriak_idx", NULL, 0), RIAK_E_UNSUPPORTED);
        CHECK_EQ_INT(riak_search_index_get(rc, "libriak_idx", &json), RIAK_E_UNSUPPORTED);
        CHECK_EQ_INT(riak_search_index_list(rc, &json), RIAK_E_UNSUPPORTED);
        CHECK_EQ_INT(riak_search_index_delete(rc, "libriak_idx"), RIAK_E_UNSUPPORTED);
        CHECK_EQ_INT(riak_search_schema_put(rc, "libriak_schema", "<schema/>"), RIAK_E_UNSUPPORTED);
        CHECK_EQ_INT(riak_search_schema_get(rc, "libriak_schema", &json), RIAK_E_UNSUPPORTED);
        CHECK(json == NULL);
    }
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
    /* pb://host:8087 and pbs://host:8087 (STARTTLS) select Protocol Buffers */
    live_transport = strncmp(scheme, "pb", 2) == 0 ? RIAK_TRANSPORT_PB : RIAK_TRANSPORT_HTTP;
    live_tls = strcmp(scheme, "https") == 0 || strcmp(scheme, "pbs") == 0;
    if (getenv("RIAK_TEST_HTTP_PORT")) {
        live_http_port = (unsigned)strtoul(getenv("RIAK_TEST_HTTP_PORT"), NULL, 10);
    }
    if (getenv("RIAK_TEST_FEATURES")) {
        features = getenv("RIAK_TEST_FEATURES");
    }
    multi_type = getenv("RIAK_TEST_TYPE");
    if (multi_type && *multi_type == '\0') {
        multi_type = NULL;
    }
    /* runs take well under a second against a local node: the time alone
     * would let back-to-back runs share a bucket */
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    snprintf(bucket, sizeof bucket, "libriak-c-test-%ld-%ld-%ld", (long)now.tv_sec, (long)getpid(),
             now.tv_nsec / 1000);
    fprintf(stderr, "bucket %s\n", bucket);

    CHECK_EQ_INT(riak_global_init(), RIAK_OK);
    riak_config cfg;
    riak_config_init(&cfg);
    live_config(&cfg);
    cfg.host = host;
    cfg.port = port;
    cfg.http_port = live_http_port;
    live_host = host;
    live_port = port;

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
    RUN(test_server_info_preflist_coverage);
    RUN(test_new_options);
    RUN(test_type_props_and_client_id);
    RUN(test_legacy_counters);
    RUN(test_fold_objects);
    RUN(test_link_walk);
    RUN(test_query_api);
    RUN(test_aae_folds);
    RUN(test_nextgenrepl);
    RUN(test_riak_repl);
    RUN(test_missing_features_unsupported);
    RUN(test_failover);
    RUN(test_async);
    RUN(test_async_data_path);

    riak_client_free(rc);
    riak_global_cleanup();
    return CHECK_DONE();
}
