/*
 File:   test_pb.c
 SPDX-License-Identifier: GPL-3.0-or-later

 The Protocol Buffers transport against the in-process stub of stub_pb.h:
 every operation, the mapping to the HTTP-shaped results, errors, retries,
 timeouts, connection reuse, async requests and (with RIAK_TEST_CERTS set)
 STARTTLS with authentication.
 */

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "riak/riak.h"
#include "stub_pb.h"

static unsigned port;

/* base64 of the stub's binary vclock, cover context and data type context */
#define VCLOCK_B64 "a84AYWAB"
#define COVER_B64 "AAFjYw=="
#define DT_CTX_B64 "AGN0eA=="

static riak_client *client_cfg(riak_config *cfg) {
    riak_client *c = NULL;
    CHECK_EQ_INT(riak_client_new(cfg, &c), RIAK_OK);
    return c;
}

static riak_client *client(void) {
    riak_config cfg;
    riak_config_init(&cfg);
    cfg.port = port;
    cfg.client_id = "tester";
    return client_cfg(&cfg);
}

static void test_ping_info_reuse(void) {
    riak_client *c = client();
    int before = atomic_load(&stub.accepts);
    for (int i = 0; i < 5; i++) {
        CHECK_EQ_INT(riak_ping(c), RIAK_OK);
    }
    CHECK_EQ_INT(atomic_load(&stub.accepts) - before, 1); /* one kept connection */
    CHECK_EQ_STR(stub.client_id, "tester");
    riak_server_info info;
    CHECK_EQ_INT(riak_get_server_info(c, &info), RIAK_OK);
    CHECK_EQ_STR(info.node, "riak@stub");
    CHECK_EQ_STR(info.version, "3.2.0");
    riak_server_info_free(&info);
    CHECK_EQ_INT(riak_client_http_status(c), 0);
    CHECK_EQ_INT(riak_client_check_nodes(c), 1);
    riak_client_free(c);
}

static size_t pick_second(const riak_object *s, size_t n, void *ctx) {
    (void)s;
    (void)ctx;
    return n - 1;
}

static void test_get(void) {
    riak_client *c = client();
    riak_object o;
    riak_opts opts = {.r = RIAK_Q_QUORUM, .sloppy_quorum = true, .n_val = 2};
    CHECK_EQ_INT(riak_get(c, "b", "k", &opts, &o), RIAK_OK);
    CHECK_EQ_INT(stub.get_r, 4294967293u);
    CHECK(stub.get_sloppy);
    CHECK_EQ_INT(stub.get_n_val, 2);
    CHECK_EQ_STR(o.data, "hello");
    CHECK_EQ_INT(o.len, 5);
    CHECK_EQ_STR(o.content_type, "text/plain; charset=utf-8");
    CHECK_EQ_STR(o.vclock, VCLOCK_B64);
    CHECK_EQ_STR(o.etag, "\"vt1\"");
    CHECK_EQ_STR(o.last_modified, "Tue, 14 Nov 2023 22:13:20 GMT");
    CHECK_EQ_INT(o.n_meta, 1);
    CHECK_EQ_STR(o.meta[0].name, "color");
    CHECK_EQ_STR(o.meta[0].value, "blue");
    CHECK_EQ_INT(o.n_indexes, 1);
    CHECK_EQ_STR(o.indexes[0].name, "email_bin");
    CHECK_EQ_STR(o.links, "</buckets/people/keys/bob%20smith>; riaktag=\"friend\"");
    CHECK(!o.deleted);
    riak_object_free(&o);

    riak_opts head = {.head = true};
    CHECK_EQ_INT(riak_get(c, "b", "k", &head, &o), RIAK_OK);
    CHECK(stub.get_head);
    CHECK_EQ_INT(o.len, 0);
    riak_object_free(&o);

    CHECK_EQ_INT(riak_get(c, "b", "missing", NULL, &o), RIAK_E_NOT_FOUND);
    CHECK_EQ_INT(riak_get(c, "b", "tomb", NULL, &o), RIAK_E_NOT_FOUND);
    CHECK_EQ_INT(riak_get(c, "b", "sibs", NULL, &o), RIAK_E_SIBLINGS);
    CHECK_EQ_INT(riak_get(c, "b", "forbidden", NULL, &o), RIAK_E_UNAUTHORIZED);
    CHECK(strstr(riak_client_errmsg(c), "Permission denied") != NULL);

    riak_opts res = {.resolver = pick_second};
    CHECK_EQ_INT(riak_get(c, "b", "sibs", &res, &o), RIAK_OK);
    CHECK_EQ_STR(o.data, "two");
    CHECK_EQ_STR(o.vclock, VCLOCK_B64);
    riak_object_free(&o);

    riak_object_set set;
    CHECK_EQ_INT(riak_get_siblings(c, "b", "sibs", NULL, &set), RIAK_OK);
    CHECK_EQ_INT(set.count, 2);
    CHECK_EQ_STR(set.vclock, VCLOCK_B64);
    CHECK_EQ_STR(set.items[0].data, "one");
    riak_object_set_free(&set);
    CHECK_EQ_INT(riak_get_siblings(c, "b", "k", NULL, &set), RIAK_OK);
    CHECK_EQ_INT(set.count, 1);
    riak_object_set_free(&set);

    riak_opts cond = {.if_modified = true, .vclock = VCLOCK_B64};
    CHECK_EQ_INT(riak_get(c, "b", "unchanged", &cond, &o), RIAK_E_NOT_MODIFIED);
    cond.vclock = NULL;
    CHECK_EQ_INT(riak_get(c, "b", "unchanged", &cond, &o), RIAK_E_INVALID_ARG);
    riak_opts etag = {.if_none_match = "\"x\""};
    CHECK_EQ_INT(riak_get(c, "b", "k", &etag, &o), RIAK_E_UNSUPPORTED);
    CHECK_EQ_INT(riak_get(c, "", "k", NULL, &o), RIAK_E_INVALID_ARG);
    riak_opts badq = {.r = -9};
    CHECK_EQ_INT(riak_get(c, "b", "k", &badq, &o), RIAK_E_INVALID_ARG);
    riak_client_free(c);
}

static void test_put_delete(void) {
    riak_client *c = client();
    riak_kv_pair meta[] = {{"a", "1"}, {"b", "2"}};
    riak_kv_pair idx[] = {{"age_int", "42"}};
    const char *links[] = {
        "</buckets/people/keys/carol%2Fx>; riaktag=\"boss\", </buckets/people>; rel=\"up\""};
    riak_opts opts = {.content_type = "application/json; charset=utf-8",
                      .meta = meta,
                      .n_meta = 2,
                      .indexes = idx,
                      .n_indexes = 1,
                      .links = links,
                      .n_links = 1,
                      .vclock = VCLOCK_B64,
                      .w = 2};
    CHECK_EQ_INT(riak_put(c, "b", "k", "{}", 2, &opts, NULL, NULL), RIAK_OK);
    CHECK_EQ_STR(stub.put_value, "{}");
    CHECK_EQ_STR(stub.put_ctype, "application/json");
    CHECK_EQ_STR(stub.put_charset, "utf-8");
    CHECK_EQ_STR(stub.put_key, "k");
    CHECK_EQ_INT(stub.put_meta, 2);
    CHECK_EQ_INT(stub.put_indexes, 1);
    CHECK_EQ_INT(stub.put_links, 1);
    CHECK_EQ_STR(stub.put_link_key, "carol/x");
    CHECK_EQ_STR(stub.put_link_tag, "boss");
    CHECK_EQ_INT(stub.put_vclock_len, sizeof STUB_VCLOCK);
    CHECK(memcmp(stub.put_vclock, STUB_VCLOCK, sizeof STUB_VCLOCK) == 0);
    CHECK_EQ_INT(stub.put_w, 2);

    char *key = NULL;
    riak_object out;
    riak_opts rb = {.return_body = true};
    CHECK_EQ_INT(riak_put(c, "b", NULL, "v", 1, &rb, &out, &key), RIAK_OK);
    CHECK_EQ_STR(key, "genkey");
    CHECK_EQ_STR(stub.put_key, "");
    CHECK_EQ_STR(out.data, "v");
    CHECK_EQ_STR(out.content_type, "application/octet-stream");
    CHECK_EQ_STR(out.vclock, VCLOCK_B64);
    riak_object_free(&out);
    riak_free(key);

    riak_opts create = {.if_none_match = "*"};
    CHECK_EQ_INT(riak_put(c, "b", "exists", "v", 1, &create, NULL, NULL), RIAK_E_PRECONDITION);
    CHECK_EQ_INT(riak_put(c, "b", "fresh", "v", 1, &create, NULL, NULL), RIAK_OK);
    CHECK(stub.put_if_none_match);
    riak_opts stale = {.if_not_modified = true, .vclock = "AAAA"};
    CHECK_EQ_INT(riak_put(c, "b", "k", "v", 1, &stale, NULL, NULL), RIAK_E_PRECONDITION);
    stale.vclock = VCLOCK_B64;
    CHECK_EQ_INT(riak_put(c, "b", "k", "v", 1, &stale, NULL, NULL), RIAK_OK);
    riak_opts bad_vc = {.vclock = "not base64!"};
    CHECK_EQ_INT(riak_put(c, "b", "k", "v", 1, &bad_vc, NULL, NULL), RIAK_E_INVALID_ARG);
    const char *bad_links[] = {"nonsense"};
    riak_opts bl = {.links = bad_links, .n_links = 1};
    CHECK_EQ_INT(riak_put(c, "b", "k", "v", 1, &bl, NULL, NULL), RIAK_E_INVALID_ARG);
    riak_opts ifm = {.if_match = "\"x\""};
    CHECK_EQ_INT(riak_put(c, "b", "k", "v", 1, &ifm, NULL, NULL), RIAK_E_UNSUPPORTED);

    CHECK_EQ_INT(riak_delete(c, "b", "k", NULL), RIAK_OK);
    riak_opts dq = {.rw = RIAK_Q_ALL, .vclock = VCLOCK_B64};
    CHECK_EQ_INT(riak_delete(c, "b", "k", &dq), RIAK_OK);
    riak_client_free(c);
}

typedef struct {
    char names[8][8];
    int n;
    int stop_after;
} names;

static bool on_name(const char *name, size_t len, void *ctx) {
    names *n = ctx;
    if (n->n < 8) snprintf(n->names[n->n], sizeof n->names[0], "%.*s", (int)len, name);
    n->n++;
    return n->stop_after == 0 || n->n < n->stop_after;
}

static void test_lists(void) {
    riak_client *c = client();
    riak_strlist l;
    CHECK_EQ_INT(riak_list_keys(c, "b", &l), RIAK_OK);
    CHECK_EQ_INT(l.count, 3);
    CHECK_EQ_STR(l.items[2], "n3");
    CHECK(l.items[3] == NULL);
    riak_strlist_free(&l);
    CHECK_EQ_INT(riak_type_list_buckets(c, "maps", &l), RIAK_OK);
    CHECK_EQ_INT(l.count, 3);
    riak_strlist_free(&l);
    names n = {.stop_after = 2};
    CHECK_EQ_INT(riak_list_keys_stream(c, NULL, "b", on_name, &n), RIAK_OK);
    CHECK_EQ_INT(n.n, 2);
    /* the stopped stream's connection is dropped; the next call still works */
    CHECK_EQ_INT(riak_ping(c), RIAK_OK);
    names all = {0};
    CHECK_EQ_INT(riak_list_buckets_stream(c, NULL, on_name, &all), RIAK_OK);
    CHECK_EQ_INT(all.n, 3);
    CHECK_EQ_STR(all.names[1], "n2");
    riak_client_free(c);
}

static void test_props(void) {
    riak_client *c = client();
    char *json = NULL;
    CHECK_EQ_INT(riak_get_bucket_props(c, "b", &json), RIAK_OK);
    CHECK(json && strstr(json, "\"n_val\":3"));
    CHECK(json && strstr(json, "\"allow_mult\":true"));
    CHECK(json && strstr(json, "\"r\":\"quorum\""));
    CHECK(json && strstr(json, "\"precommit\":[{\"name\":\"validate\"}]"));
    CHECK(json && strstr(json, "\"datatype\":\"map\""));
    CHECK(json && strstr(json, "\"chash_keyfun\":{\"mod\":\"riak_core_util\""));
    CHECK(json && strstr(json, "\"name\":\"b\""));
    riak_free(json);
    CHECK_EQ_INT(riak_get_type_props(c, "maps", &json), RIAK_OK);
    CHECK(json && strstr(json, "\"props\""));
    riak_free(json);
    CHECK_EQ_INT(riak_set_bucket_props(
                     c, "b",
                     "{\"props\":{\"name\":\"b\",\"n_val\":5,\"allow_mult\":true,\"w\":\"all\","
                     "\"precommit\":[{\"mod\":\"m\",\"fun\":\"f\"},{\"name\":\"h\"}],"
                     "\"postcommit\":[],\"repl\":\"realtime\"}}"),
                 RIAK_OK);
    CHECK_EQ_INT(stub.set_n_val, 5);
    CHECK(stub.set_allow_mult);
    CHECK_EQ_INT(stub.set_precommit, 2);
    CHECK_EQ_INT(riak_set_bucket_props(c, "b", "{\"props\":{\"dvv_enabled\":true}}"),
                 RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_set_bucket_props(c, "b", "{\"props\":{\"n_val\":\"x\"}}"),
                 RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_set_bucket_props(c, "b", "[]"), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_reset_bucket_props(c, "b"), RIAK_OK);
    CHECK_EQ_INT(riak_type_reset_bucket_props(c, "maps", "b"), RIAK_OK);
    riak_client_free(c);
}

static void test_index(void) {
    riak_client *c = client();
    riak_index_query q = {.index = "age_int", .range_min = "1", .range_max = "9"};
    riak_index_result r;
    CHECK_EQ_INT(riak_index_query_run(c, "b", &q, &r), RIAK_OK);
    CHECK_EQ_INT(r.count, 2);
    CHECK_EQ_STR(r.keys[1], "k2");
    CHECK(r.terms == NULL);
    CHECK_EQ_STR(r.continuation, "c2");
    riak_index_result_free(&r);
    q.return_terms = true;
    q.cover_context = COVER_B64;
    CHECK_EQ_INT(riak_index_query_run(c, "b", &q, &r), RIAK_OK);
    CHECK_EQ_STR(r.terms[0], "t1");
    CHECK_EQ_STR(r.keys[0], "k1");
    CHECK_EQ_INT(stub.cover_len, sizeof STUB_COVER);
    CHECK(memcmp(stub.cover, STUB_COVER, sizeof STUB_COVER) == 0);
    riak_index_result_free(&r);
    riak_index_query bad = {.index = "x"};
    CHECK_EQ_INT(riak_index_query_run(c, "b", &bad, &r), RIAK_E_INVALID_ARG);

    riak_coverage_plan plan;
    CHECK_EQ_INT(riak_get_coverage(c, NULL, "b", 0, &plan), RIAK_OK);
    CHECK_EQ_INT(plan.count, 1);
    CHECK_EQ_STR(plan.entries[0].host, "10.0.0.1");
    CHECK_EQ_INT(plan.entries[0].port, 8087);
    CHECK_EQ_STR(plan.entries[0].cover_context, COVER_B64);
    riak_coverage_free(&plan);

    riak_preflist pl;
    CHECK_EQ_INT(riak_get_preflist(c, NULL, "b", "k", &pl), RIAK_OK);
    CHECK_EQ_INT(pl.count, 2);
    CHECK_EQ_STR(pl.items[0].node, "riak@a");
    CHECK(pl.items[0].primary && !pl.items[1].primary);
    riak_preflist_free(&pl);
    riak_client_free(c);
}

static void test_search(void) {
    riak_client *c = client();
    riak_search_query q = {.q = "tag_ss:a", .fields = "_yz_rk, tag_ss", .rows = 10};
    riak_search_result r;
    CHECK_EQ_INT(riak_search(c, "people", &q, &r), RIAK_OK);
    CHECK_EQ_INT(r.num_found, 7);
    CHECK_EQ_INT(r.count, 1);
    CHECK_EQ_STR(r.docs[0].key, "alice");
    CHECK_EQ_STR(r.docs[0].bucket, "people");
    CHECK_EQ_INT(r.docs[0].n_fields, 4);
    CHECK(r.docs[0].score > 1.4 && r.docs[0].score < 1.6);
    CHECK(strstr(r.raw, "\"numFound\":7") != NULL);
    riak_search_result_free(&r);

    char *json = NULL;
    CHECK_EQ_INT(riak_search_index_get(c, "people", &json), RIAK_OK);
    CHECK_EQ_STR(json, "{\"name\":\"people\",\"n_val\":3,\"schema\":\"_yz_default\"}");
    riak_free(json);
    CHECK_EQ_INT(riak_search_index_get(c, "missing", &json), RIAK_E_NOT_FOUND);
    CHECK_EQ_INT(riak_search_index_list(c, &json), RIAK_OK);
    CHECK(json && json[0] == '[');
    riak_free(json);
    CHECK_EQ_INT(riak_search_index_create(c, "people", "_yz_default", 3), RIAK_OK);
    CHECK_EQ_INT(riak_search_index_delete(c, "people"), RIAK_OK);
    CHECK_EQ_INT(riak_search_schema_put(c, "sch", "<schema/>"), RIAK_OK);
    char *xml = NULL;
    CHECK_EQ_INT(riak_search_schema_get(c, "sch", &xml), RIAK_OK);
    CHECK_EQ_STR(xml, "<schema/>");
    riak_free(xml);
    riak_client_free(c);
}

static int mr_calls;

static bool on_phase(unsigned phase, const char *data, void *ctx) {
    (void)ctx;
    mr_calls++;
    if (mr_calls == 3) {
        CHECK_EQ_INT(phase, 1);
        CHECK_EQ_STR(data, "[\"x\"]");
    }
    return true;
}

static void test_mapred(void) {
    riak_client *c = client();
    char *res = NULL;
    CHECK_EQ_INT(riak_mapred(c, "{\"inputs\":\"b\"}", &res), RIAK_OK);
    CHECK_EQ_STR(res, "[[1,2,3],[\"x\"]]");
    riak_free(res);
    CHECK_EQ_INT(riak_mapred_stream(c, "{\"inputs\":\"b\"}", on_phase, NULL), RIAK_OK);
    CHECK_EQ_INT(mr_calls, 3);
    riak_client_free(c);
}

static void test_datatypes(void) {
    riak_client *c = client();
    riak_datatype dt;
    CHECK_EQ_INT(riak_dt_fetch(c, "maps", "b", "k", NULL, &dt), RIAK_OK);
    CHECK_EQ_INT(dt.value.type, RIAK_DT_MAP);
    CHECK_EQ_INT(dt.value.n_entries, 5);
    CHECK_EQ_STR(dt.context, DT_CTX_B64);
    const riak_dt_value *v = riak_dt_map_get(&dt.value, "visits", RIAK_DT_COUNTER);
    CHECK(v && v->counter == 5);
    v = riak_dt_map_get(&dt.value, "tags", RIAK_DT_SET);
    CHECK(v && v->n_elements == 2 && strcmp(v->elements[1], "b") == 0);
    v = riak_dt_map_get(&dt.value, "name", RIAK_DT_REGISTER);
    CHECK(v && strcmp(v->reg, "hi") == 0);
    v = riak_dt_map_get(&dt.value, "active", RIAK_DT_FLAG);
    CHECK(v && v->flag);
    v = riak_dt_map_get(&dt.value, "sub", RIAK_DT_MAP);
    CHECK(v && riak_dt_map_get(v, "n", RIAK_DT_COUNTER)->counter == 1);
    riak_datatype_free(&dt);
    CHECK_EQ_INT(riak_dt_fetch(c, "counters", "b", "k", NULL, &dt), RIAK_OK);
    CHECK_EQ_INT(dt.value.type, RIAK_DT_COUNTER);
    CHECK_EQ_INT(dt.value.counter, -5);
    riak_datatype_free(&dt);
    CHECK_EQ_INT(riak_dt_fetch(c, "counters", "b", "missing", NULL, &dt), RIAK_E_NOT_FOUND);

    riak_dt_op *op = riak_dt_op_new(RIAK_DT_COUNTER);
    riak_dt_op_increment(op, 3);
    riak_dt_opts rb = {.return_body = true};
    char *key = NULL;
    CHECK_EQ_INT(riak_dt_update(c, "counters", "b", NULL, op, NULL, &rb, &dt, &key), RIAK_OK);
    CHECK_EQ_INT(stub.dt_kind, 1);
    CHECK_EQ_INT(stub.dt_increment, 3);
    CHECK_EQ_STR(key, "dtkey");
    CHECK_EQ_INT(dt.value.counter, 7);
    CHECK_EQ_STR(dt.context, DT_CTX_B64);
    riak_datatype_free(&dt);
    riak_free(key);
    riak_dt_op_free(op);

    op = riak_dt_op_new(RIAK_DT_SET);
    riak_dt_op_remove(op, "zz");
    CHECK_EQ_INT(riak_dt_update(c, "sets", "b", "k", op, NULL, NULL, NULL, NULL),
                 RIAK_E_PRECONDITION);
    CHECK_EQ_INT(riak_dt_update(c, "sets", "b", "k", op, DT_CTX_B64, NULL, NULL, NULL), RIAK_OK);
    CHECK_EQ_INT(stub.dt_removes, 1);
    CHECK_EQ_INT(stub.dt_context_len, sizeof STUB_DT_CTX);
    riak_dt_op_free(op);

    op = riak_dt_op_new(RIAK_DT_MAP);
    riak_dt_op_map_counter(op, "visits", 1);
    riak_dt_op_map_register(op, "name", "bob");
    riak_dt_op_map_flag(op, "active", false);
    riak_dt_op_add(riak_dt_op_map_set(op, "tags"), "c");
    riak_dt_op_map_counter(riak_dt_op_map_map(op, "sub"), "n", 2);
    riak_dt_op_map_remove(op, "old", RIAK_DT_REGISTER);
    CHECK_EQ_INT(riak_dt_update(c, "maps", "b", "k", op, DT_CTX_B64, NULL, NULL, NULL), RIAK_OK);
    CHECK_EQ_INT(stub.dt_kind, 3);
    CHECK_EQ_INT(stub.dt_map_updates, 5);
    CHECK_EQ_INT(stub.dt_map_removes, 1);
    riak_dt_op_free(op);

    op = riak_dt_op_new(RIAK_DT_HLL);
    riak_dt_op_add(op, "x");
    riak_dt_op_add(op, "y");
    CHECK_EQ_INT(riak_dt_update(c, "hlls", "b", "k", op, NULL, NULL, NULL, NULL), RIAK_OK);
    CHECK_EQ_INT(stub.dt_kind, 4);
    CHECK_EQ_INT(stub.dt_adds, 2);
    riak_dt_op_free(op);

    op = riak_dt_op_new(RIAK_DT_SET); /* empty */
    CHECK_EQ_INT(riak_dt_update(c, "sets", "b", "k", op, NULL, NULL, NULL, NULL),
                 RIAK_E_INVALID_ARG);
    riak_dt_op_free(op);
    riak_client_free(c);
}

typedef struct {
    int n, retries;
    riak_transport transport;
} metrics;

static void on_metrics(const riak_request_metrics *m, void *ctx) {
    metrics *x = ctx;
    x->n++;
    x->retries += m->will_retry;
    x->transport = m->transport;
}

/* A kept connection that the server closed while idle is replaced before
 * use, so the next request succeeds even without retries. */
static void test_idle_close(void) {
    riak_config cfg;
    riak_config_init(&cfg);
    cfg.port = port;
    cfg.max_retries = 0;
    riak_client *c = client_cfg(&cfg);
    riak_object o;
    CHECK_EQ_INT(riak_get(c, "b", "bye", NULL, &o), RIAK_E_NOT_FOUND);
    stub_sleep_ms(100); /* the close reaches the client */
    int before = atomic_load(&stub.accepts);
    CHECK_EQ_INT(riak_ping(c), RIAK_OK);
    CHECK_EQ_INT(atomic_load(&stub.accepts) - before, 1);
    riak_client_free(c);
}

static void test_errors_retries(void) {
    riak_node nodes[2] = {{"127.0.0.1", port, 0}, {"127.0.0.1", port, 0}};
    riak_config cfg;
    riak_config_init(&cfg);
    cfg.nodes = nodes;
    cfg.n_nodes = 2;
    cfg.retry_base_ms = 1;
    metrics m = {0};
    cfg.metrics_fn = on_metrics;
    cfg.metrics_ctx = &m;
    riak_client *c = client_cfg(&cfg);
    riak_object o;
    atomic_store(&stub.overload, 1);
    CHECK_EQ_INT(riak_get(c, "b", "overload", NULL, &o), RIAK_OK);
    riak_object_free(&o);
    CHECK_EQ_INT(m.n, 2);
    CHECK_EQ_INT(m.retries, 1);
    CHECK_EQ_INT(m.transport, RIAK_TRANSPORT_PB);
    atomic_store(&stub.drop, 1);
    CHECK_EQ_INT(riak_get(c, "b", "drop", NULL, &o), RIAK_OK);
    riak_object_free(&o);
    atomic_store(&stub.drop, 1);
    CHECK_EQ_INT(riak_put(c, "b", NULL, "v", 1, NULL, NULL, NULL), RIAK_OK); /* not "drop" */
    riak_client_free(c);

    /* a generated-key put is not repeated after the request was sent */
    atomic_store(&stub.overload, 0);

    riak_config_init(&cfg);
    cfg.port = port;
    cfg.max_body_bytes = 1000;
    c = client_cfg(&cfg);
    CHECK_EQ_INT(riak_get(c, "b", "big", NULL, &o), RIAK_E_HTTP);
    CHECK(strstr(riak_client_errmsg(c), "max_body_bytes") != NULL);
    CHECK_EQ_INT(riak_ping(c), RIAK_OK);
    char *json;
    CHECK_EQ_INT(riak_stats(c, &json), RIAK_E_UNSUPPORTED);
    riak_client_free(c);

    riak_config_init(&cfg);
    cfg.port = port;
    cfg.timeout_ms = 300;
    cfg.max_retries = 0;
    c = client_cfg(&cfg);
    CHECK_EQ_INT(riak_get(c, "b", "slow", NULL, &o), RIAK_E_TRANSPORT);
    CHECK(strstr(riak_client_errmsg(c), "timed out") != NULL);
    riak_client_free(c);

    riak_config_init(&cfg);
    cfg.port = 1;
    cfg.max_retries = 0;
    c = client_cfg(&cfg);
    CHECK_EQ_INT(riak_ping(c), RIAK_E_TRANSPORT);
    CHECK(strstr(riak_client_errmsg(c), "cannot connect") != NULL);
    riak_client_free(c);

    riak_config_init(&cfg);
    cfg.host = "no-such-host.invalid";
    cfg.max_retries = 0;
    c = client_cfg(&cfg);
    CHECK_EQ_INT(riak_ping(c), RIAK_E_TRANSPORT);
    riak_client_free(c);

    riak_config_init(&cfg);
    cfg.transport = RIAK_TRANSPORT_HTTP;
    c = client_cfg(&cfg);
    riak_coverage_plan plan;
    CHECK_EQ_INT(riak_get_coverage(c, NULL, "b", 0, &plan), RIAK_E_UNSUPPORTED);
    riak_opts sloppy = {.sloppy_quorum = true};
    CHECK_EQ_INT(riak_get(c, "b", "k", &sloppy, &o), RIAK_E_UNSUPPORTED);
    riak_client_free(c);
}

/* ---- async ----------------------------------------------------------- */

typedef struct {
    int done, ok;
    char key[16];
} tally;

static void on_done(riak_async_result *r, void *ctx) {
    tally *t = ctx;
    t->done++;
    t->ok += r->status == RIAK_OK;
    if (r->created_key) snprintf(t->key, sizeof t->key, "%s", r->created_key);
}

static void test_async(void) {
    riak_config cfg;
    riak_config_init(&cfg);
    cfg.port = port;
    cfg.pb_max_connections_per_node = 2;
    riak_async *a = NULL;
    CHECK_EQ_INT(riak_async_new(&cfg, &a), RIAK_OK);
    tally t = {0};
    int before = atomic_load(&stub.accepts);
    for (int i = 0; i < 20; i++) {
        CHECK_EQ_INT(riak_async_get(a, "b", "k", NULL, on_done, &t), RIAK_OK);
    }
    CHECK_EQ_INT(riak_async_put(a, "b", NULL, "v", 1, NULL, on_done, &t), RIAK_OK);
    CHECK_EQ_INT(riak_async_delete(a, "b", "k", NULL, on_done, &t), RIAK_OK);
    CHECK_EQ_INT(riak_async_ping(a, on_done, &t), RIAK_OK);
    CHECK_EQ_INT(riak_async_get(a, "", "k", NULL, on_done, &t), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_async_pending(a), 23);
    CHECK_EQ_INT(riak_async_run(a, 5000), 0);
    CHECK_EQ_INT(t.done, 23);
    CHECK_EQ_INT(t.ok, 23);
    CHECK_EQ_STR(t.key, "genkey");
    CHECK(atomic_load(&stub.accepts) - before <= 2);
    /* cancelled on free */
    tally late = {0};
    CHECK_EQ_INT(riak_async_get(a, "b", "slow", NULL, on_done, &late), RIAK_OK);
    riak_async_free(a);
    CHECK_EQ_INT(late.done, 1);
    CHECK_EQ_INT(late.ok, 0);
}

/* A minimal external event loop with poll(). */
typedef struct {
    int fds[16];
    int ev[16];
    int n;
    long timeout;
} loop;

static void on_sock(int fd, int events, void *ctx) {
    loop *l = ctx;
    for (int i = 0; i < l->n; i++) {
        if (l->fds[i] == fd) {
            if (events == RIAK_POLL_REMOVE) {
                l->fds[i] = l->fds[--l->n];
                l->ev[i] = l->ev[l->n];
            } else {
                l->ev[i] = events;
            }
            return;
        }
    }
    if (events != RIAK_POLL_REMOVE && l->n < 16) {
        l->fds[l->n] = fd;
        l->ev[l->n++] = events;
    }
}

static void on_timer(long ms, void *ctx) {
    ((loop *)ctx)->timeout = ms;
}

static void test_async_loop(void) {
    riak_config cfg;
    riak_config_init(&cfg);
    cfg.port = port;
    riak_async *a = NULL;
    CHECK_EQ_INT(riak_async_new(&cfg, &a), RIAK_OK);
    loop l = {.timeout = -1};
    CHECK_EQ_INT(riak_async_set_loop(a, on_sock, on_timer, &l), RIAK_OK);
    tally t = {0};
    for (int i = 0; i < 5; i++) {
        CHECK_EQ_INT(riak_async_get(a, "b", "k", NULL, on_done, &t), RIAK_OK);
    }
    for (int spins = 0; riak_async_pending(a) > 0 && spins < 1000; spins++) {
        struct pollfd p[16];
        for (int i = 0; i < l.n; i++) {
            p[i] = (struct pollfd){.fd = l.fds[i],
                                   .events = (short)(((l.ev[i] & RIAK_POLL_IN) ? POLLIN : 0) |
                                                     ((l.ev[i] & RIAK_POLL_OUT) ? POLLOUT : 0))};
        }
        int n = l.n;
        int r = poll(p, (nfds_t)n, l.timeout < 0 ? 1000 : (int)l.timeout);
        if (r == 0) {
            riak_async_on_timeout(a);
            continue;
        }
        for (int i = 0; i < n; i++) {
            if (p[i].revents) {
                riak_async_on_socket(a, p[i].fd,
                                     ((p[i].revents & POLLIN) ? RIAK_POLL_IN : 0) |
                                         ((p[i].revents & POLLOUT) ? RIAK_POLL_OUT : 0));
            }
        }
    }
    CHECK_EQ_INT(t.done, 5);
    CHECK_EQ_INT(t.ok, 5);
    riak_async_free(a);
}

/* ---- TLS and authentication ---------------------------------------- */

static char *cert_path(const char *dir, const char *name) {
    static char paths[8][512];
    static int next;
    char *p = paths[next++ % 8];
    snprintf(p, sizeof paths[0], "%s/%s", dir, name);
    return p;
}

static void test_tls(void) {
    riak_config cfg;
    riak_config_init(&cfg);
    cfg.port = port;
    cfg.tls = true;
    cfg.max_retries = 0;
    riak_client *c = client_cfg(&cfg);
    CHECK_EQ_INT(riak_ping(c), RIAK_E_TLS); /* security off: STARTTLS is refused */
    riak_client_free(c);

    const char *dir = getenv("RIAK_TEST_CERTS");
    if (dir == NULL) {
        fprintf(stderr, "skip test_tls: RIAK_TEST_CERTS not set\n");
        return;
    }
    CHECK_EQ_INT(stub_start_tls(cert_path(dir, "server.pem"), cert_path(dir, "server.key"),
                                cert_path(dir, "ca.pem")),
                 0);
    cfg.ca_file = cert_path(dir, "ca.pem");
    cfg.username = "alice";
    cfg.password = "secret";
    c = client_cfg(&cfg);
    int auth = atomic_load(&stub.auth_ok);
    CHECK_EQ_INT(riak_ping(c), RIAK_OK);
    riak_object o;
    CHECK_EQ_INT(riak_get(c, "b", "k", NULL, &o), RIAK_OK);
    riak_object_free(&o);
    CHECK_EQ_INT(atomic_load(&stub.auth_ok) - auth, 1);
    CHECK_EQ_INT(atomic_load(&stub.tls_version), TLS1_3_VERSION);
    riak_client_free(c);

    /* old-node compatibility caps the protocol at TLS 1.2 */
    cfg.tls_legacy = true;
    c = client_cfg(&cfg);
    CHECK_EQ_INT(riak_ping(c), RIAK_OK);
    CHECK_EQ_INT(atomic_load(&stub.tls_version), TLS1_2_VERSION);
    riak_client_free(c);
    cfg.tls_legacy = false;

    /* a server that drops the connection on the ClientHello, as old ones do */
    atomic_store(&stub.tls_drop, 1);
    c = client_cfg(&cfg);
    CHECK_EQ_INT(riak_ping(c), RIAK_E_TLS);
    CHECK(strstr(riak_client_errmsg(c), "need tls_legacy") != NULL);
    riak_client_free(c);
    cfg.tls_legacy = true;
    c = client_cfg(&cfg);
    CHECK_EQ_INT(riak_ping(c), RIAK_E_TLS);
    CHECK(strstr(riak_client_errmsg(c), "need tls_legacy") == NULL);
    riak_client_free(c);
    cfg.tls_legacy = false;
    atomic_store(&stub.tls_drop, 0);

    cfg.password = "wrong";
    c = client_cfg(&cfg);
    CHECK_EQ_INT(riak_ping(c), RIAK_E_UNAUTHORIZED);
    riak_client_free(c);

    cfg.password = "secret";
    cfg.ca_file = cert_path(dir, "other.pem");
    c = client_cfg(&cfg);
    CHECK_EQ_INT(riak_ping(c), RIAK_E_TLS);
    CHECK(strstr(riak_client_errmsg(c), "verif") != NULL);
    riak_client_free(c);

    cfg.tls_verify = false;
    c = client_cfg(&cfg);
    CHECK_EQ_INT(riak_ping(c), RIAK_OK);
    riak_client_free(c);

    cfg.tls_verify = true;
    cfg.ca_file = cert_path(dir, "ca.pem");
    cfg.tls_cert_file = cert_path(dir, "client.pem");
    cfg.tls_key_file = cert_path(dir, "client-enc.key");
    cfg.tls_key_password = "secret";
    c = client_cfg(&cfg);
    CHECK_EQ_INT(riak_ping(c), RIAK_OK);
    riak_client_free(c);

    cfg.tls_key_password = "wrong";
    c = client_cfg(&cfg);
    CHECK_EQ_INT(riak_ping(c), RIAK_E_TLS);
    CHECK(strstr(riak_client_errmsg(c), "client key") != NULL);
    riak_client_free(c);

    cfg.tls_cert_file = NULL;
    cfg.tls_key_file = NULL;
    cfg.tls_key_password = NULL;
    cfg.host = "localhost"; /* SAN DNS:localhost */
    c = client_cfg(&cfg);
    CHECK_EQ_INT(riak_ping(c), RIAK_OK);
    riak_client_free(c);

    /* a certificate from the trusted CA, but for another name */
    CHECK_EQ_INT(stub_start_tls(cert_path(dir, "wrong.pem"), cert_path(dir, "wrong.key"),
                                cert_path(dir, "ca.pem")),
                 0);
    const char *hosts[] = {"localhost", "127.0.0.1"};
    for (size_t i = 0; i < 2; i++) {
        cfg.host = hosts[i];
        c = client_cfg(&cfg);
        CHECK_EQ_INT(riak_ping(c), RIAK_E_TLS);
        CHECK(strstr(riak_client_errmsg(c), "verif") != NULL);
        riak_client_free(c);
    }
    cfg.tls_verify = false;
    c = client_cfg(&cfg);
    CHECK_EQ_INT(riak_ping(c), RIAK_OK);
    riak_client_free(c);
}

int main(void) {
    port = stub_start();
    CHECK_EQ_INT(riak_global_init(), RIAK_OK);
    RUN(test_ping_info_reuse);
    RUN(test_get);
    RUN(test_put_delete);
    RUN(test_lists);
    RUN(test_props);
    RUN(test_index);
    RUN(test_search);
    RUN(test_mapred);
    RUN(test_datatypes);
    RUN(test_idle_close);
    RUN(test_errors_retries);
    RUN(test_async);
    RUN(test_async_loop);
    RUN(test_tls);
    riak_global_cleanup();
    return CHECK_DONE();
}
