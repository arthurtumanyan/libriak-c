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
    /* the system's text for the connect error (strerror_r) reaches the message */
    riak_config closed;
    riak_config_init(&closed);
    closed.port = 1;
    closed.max_retries = 0;
    riak_client *cc = client_cfg(&closed);
    CHECK_EQ_INT(riak_ping(cc), RIAK_E_TRANSPORT);
    CHECK(strstr(riak_client_errmsg(cc), "127.0.0.1:1") != NULL);
    CHECK(strstr(riak_client_errmsg(cc), "Connection refused") != NULL);
    riak_client_free(cc);
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

/* ---- Riak 2.9+/3.x calls ------------------------------------------------- */

static void test_new_options_pb(void) {
    riak_client *c = client();
    riak_opts put = {.content_type = "text/plain",
                     .node_confirms = RIAK_Q_QUORUM,
                     .sync_on_write = RIAK_SYNC_ALL,
                     .asis = true};
    CHECK_EQ_INT(riak_put(c, "b", "k", "v", 1, &put, NULL, NULL), RIAK_OK);
    CHECK_EQ_INT(ext.node_confirms, 4294967293u);
    CHECK_EQ_INT(ext.put_sync, 3); /* all = 2, recorded +1 */
    CHECK_EQ_INT(ext.put_asis, 1);
    for (riak_sync s = RIAK_SYNC_BACKEND; s <= RIAK_SYNC_ALL; s++) {
        riak_opts so = {.sync_on_write = s};
        CHECK_EQ_INT(riak_put(c, "b", "k", "v", 1, &so, NULL, NULL), RIAK_OK);
        CHECK_EQ_INT(ext.put_sync, (unsigned)s); /* backend 0, one 1, all 2; recorded + 1 */
    }
    riak_opts no_sync = {0};
    CHECK_EQ_INT(riak_put(c, "b", "k", "v", 1, &no_sync, NULL, NULL), RIAK_OK);
    CHECK_EQ_INT(ext.put_sync, 0);
    riak_opts bad_sync = {.sync_on_write = (riak_sync)9};
    CHECK_EQ_INT(riak_put(c, "b", "k", "v", 1, &bad_sync, NULL, NULL), RIAK_E_INVALID_ARG);

    riak_object o;
    riak_opts head = {.return_head = true};
    CHECK_EQ_INT(riak_put(c, "b", "k", "value", 5, &head, &o, NULL), RIAK_OK);
    CHECK_EQ_INT(ext.put_return_head, 1);
    CHECK_EQ_INT(o.len, 0);
    CHECK_EQ_STR(o.vclock, VCLOCK_B64);
    riak_object_free(&o);

    riak_opts nc = {.node_confirms = 2};
    CHECK_EQ_INT(riak_get(c, "b", "k", &nc, &o), RIAK_OK);
    CHECK_EQ_INT(ext.node_confirms, 2);
    riak_object_free(&o);
    CHECK_EQ_INT(riak_delete(c, "b", "k", &nc), RIAK_E_UNSUPPORTED);

    riak_opts dv = {.deletedvclock = true};
    CHECK_EQ_INT(riak_get(c, "b", "tombdv", &dv, &o), RIAK_E_NOT_FOUND);
    CHECK_EQ_INT(ext.get_deletedvclock, 1);
    CHECK(o.deleted);
    CHECK_EQ_STR(o.vclock, VCLOCK_B64);
    riak_object_free(&o);
    CHECK_EQ_INT(riak_get(c, "b", "tombdv", NULL, &o), RIAK_E_NOT_FOUND);
    CHECK(o.vclock == NULL && !o.deleted);

    riak_index_query q = {
        .index = "age_int", .range_min = "1", .range_max = "9", .pagination_sort = true};
    riak_index_result r;
    CHECK_EQ_INT(riak_index_query_run(c, "b", &q, &r), RIAK_OK);
    CHECK_EQ_INT(ext.index_pagination_sort, 1);
    riak_index_result_free(&r);

    riak_dt_op *op = riak_dt_op_new(RIAK_DT_COUNTER);
    riak_dt_op_increment(op, 1);
    riak_dt_opts dto = {.node_confirms = 3};
    CHECK_EQ_INT(riak_dt_update(c, "counters", "b", "k", op, NULL, &dto, NULL, NULL), RIAK_OK);
    CHECK_EQ_INT(ext.dt_node_confirms, 3);
    riak_dt_op_free(op);

    /* gset + return_body: sent without return_body, then fetched (Riak 3.4
     * would crash and drop the connection otherwise) */
    op = riak_dt_op_new(RIAK_DT_GSET);
    riak_dt_op_add(op, "x");
    riak_dt_opts rb = {.return_body = true};
    riak_datatype dt;
    unsigned fetches = ext.dt_fetches;
    int dropped = atomic_load(&ext.dropped);
    CHECK_EQ_INT(riak_dt_update(c, "counters", "b", "k", op, NULL, &rb, &dt, NULL), RIAK_OK);
    CHECK_EQ_INT(ext.dt_return_body, 0);
    CHECK_EQ_INT(ext.dt_fetches - fetches, 1);
    CHECK_EQ_INT(atomic_load(&ext.dropped), dropped);
    CHECK_EQ_INT(dt.value.type, RIAK_DT_COUNTER); /* whatever the fetch returned */
    riak_datatype_free(&dt);
    riak_dt_op_free(op);
    riak_client_free(c);
}

static void test_props_counters_ids(void) {
    riak_client *c = client();
    CHECK_EQ_INT(riak_set_type_props(c, "maps", "{\"props\":{\"notfound_ok\":false}}"), RIAK_OK);
    CHECK_EQ_STR(ext.type_props_type, "maps");
    CHECK_EQ_INT(ext.type_notfound_ok, 0);
    CHECK_EQ_INT(riak_set_type_props(c, "maps", "{\"props\":{\"bogus\":1}}"), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_set_type_props(c, "", "{}"), RIAK_E_INVALID_ARG);

    /* new bucket props round-trip through the JSON mapping */
    CHECK_EQ_INT(
        riak_set_bucket_props(c, "b",
                              "{\"props\":{\"node_confirms\":\"quorum\",\"sync_on_write\":\"one\","
                              "\"aae_tree_exclude\":true}}"),
        RIAK_OK);
    CHECK_EQ_INT(ext.props_sync, 1 + 1);                    /* one */
    CHECK_EQ_INT(ext.props_node_confirms, 4294967293u + 1); /* quorum */
    CHECK_EQ_INT(ext.props_aae_exclude, 1);
    const char *syncs[] = {"backend", "one", "all"};
    for (unsigned i = 0; i < 3; i++) {
        char props[96];
        snprintf(props, sizeof props, "{\"props\":{\"sync_on_write\":\"%s\",\"node_confirms\":%u}}",
                 syncs[i], i + 1);
        CHECK_EQ_INT(riak_set_bucket_props(c, "b", props), RIAK_OK);
        CHECK_EQ_INT(ext.props_sync, i + 1);
        CHECK_EQ_INT(ext.props_node_confirms, i + 2);
        CHECK_EQ_INT(ext.props_aae_exclude, -1);
    }
    CHECK_EQ_INT(riak_set_bucket_props(c, "b", "{\"props\":{\"sync_on_write\":\"never\"}}"),
                 RIAK_E_INVALID_ARG);
    char *props = NULL;
    CHECK_EQ_INT(riak_get_bucket_props(c, "b", &props), RIAK_OK);
    CHECK(props && strstr(props, "\"sync_on_write\":\"one\"") &&
          strstr(props, "\"node_confirms\":2") && strstr(props, "\"aae_tree_exclude\":true"));
    riak_free(props);

    char *id = NULL;
    CHECK_EQ_INT(riak_get_client_id(c, &id), RIAK_OK);
    CHECK_EQ_STR(id, "AQID"); /* base64 of 01 02 03 */
    riak_free(id);
    CHECK_EQ_INT(riak_get_cluster_id(c, &id), RIAK_OK);
    CHECK_EQ_STR(id, "{'riak@stub',{1,2,3}}");
    riak_free(id);

    int64_t v = 0;
    CHECK_EQ_INT(riak_counter_get(c, "b", "c", NULL, &v), RIAK_OK);
    CHECK_EQ_INT(v, 42);
    CHECK_EQ_INT(ext.counter_r, 0);
    CHECK_EQ_INT(ext.counter_basic_quorum, -1);
    riak_opts rq = {.r = 2, .basic_quorum = true, .notfound_ok = true};
    CHECK_EQ_INT(riak_counter_get(c, "b", "c", &rq, &v), RIAK_OK);
    CHECK_EQ_INT(ext.counter_r, 2);
    CHECK_EQ_INT(ext.counter_basic_quorum, 1);
    CHECK_EQ_INT(ext.counter_notfound_ok, 1);
    CHECK_EQ_INT(riak_counter_get(c, "b", "missing", NULL, &v), RIAK_E_NOT_FOUND);
    riak_opts nc = {.node_confirms = 1};
    CHECK_EQ_INT(riak_counter_update(c, "b", "c", -5, &nc, &v), RIAK_OK);
    CHECK_EQ_INT(ext.counter_amount, -5);
    CHECK_EQ_INT(ext.counter_returnvalue, 1);
    CHECK_EQ_INT(ext.node_confirms, 1);
    CHECK_EQ_INT(v, 35);
    CHECK_EQ_INT(riak_counter_update(c, "b", "c", 3, NULL, NULL), RIAK_OK);
    CHECK_EQ_INT(ext.counter_returnvalue, 0);
    riak_opts typed = {.bucket_type = "maps"};
    CHECK_EQ_INT(riak_counter_get(c, "b", "c", &typed, &v), RIAK_E_UNSUPPORTED);
    CHECK_EQ_INT(riak_counter_get(c, "b", "c", NULL, NULL), RIAK_E_INVALID_ARG);

    riak_object o;
    CHECK_EQ_INT(riak_repl_get(c, "cluster-a", "b", "k", NULL, &o), RIAK_OK);
    CHECK_EQ_STR(ext.repl_cluster, "cluster-a");
    CHECK_EQ_STR(o.data, "proxied");
    riak_object_free(&o);
    CHECK_EQ_INT(riak_repl_get(c, "cluster-a", "b", "missing", NULL, &o), RIAK_E_NOT_FOUND);
    CHECK_EQ_INT(riak_repl_get(c, "cluster-a", "b", "k", &typed, &o), RIAK_E_UNSUPPORTED);
    CHECK_EQ_INT(riak_repl_get(c, "", "b", "k", NULL, &o), RIAK_E_INVALID_ARG);
    riak_client_free(c);
}

typedef struct {
    int n;
    char last[16];
    int stop_at;
} fold_count;

static bool on_fold(const char *key, riak_object *obj, void *ctx) {
    fold_count *f = ctx;
    f->n++;
    snprintf(f->last, sizeof f->last, "%s", key);
    CHECK(obj->data && strncmp(obj->data, "fold-", 5) == 0);
    CHECK_EQ_STR(obj->vclock, VCLOCK_B64);
    return f->stop_at == 0 || f->n < f->stop_at;
}

static void test_fold_and_links(void) {
    riak_client *c = client();
    fold_count f = {0};
    char *next = NULL;
    riak_fold_query q = {.start_key = "f1",
                         .end_key = "f9",
                         .start_exclusive = true,
                         .end_inclusive = true,
                         .max_results = 2};
    CHECK_EQ_INT(riak_fold_objects(c, "b", &q, on_fold, &f, &next), RIAK_OK);
    CHECK_EQ_INT(f.n, 2);
    CHECK_EQ_STR(f.last, "f2");
    CHECK_EQ_STR(next, "next-page");
    CHECK_EQ_STR(ext.fold_start, "f1");
    CHECK_EQ_STR(ext.fold_end, "f9");
    CHECK_EQ_INT(ext.fold_start_incl, 0);
    CHECK_EQ_INT(ext.fold_end_incl, 1);
    CHECK_EQ_STR(ext.fold_bucket, "b");
    CHECK_EQ_INT(ext.fold_max, 2);
    CHECK_EQ_STR(ext.fold_cont, "");
    CHECK_EQ_STR(ext.fold_type, "");
    /* the next page carries the continuation and the bucket type */
    riak_fold_query page = q;
    page.continuation = next;
    page.bucket_type = "maps";
    fold_count f2 = {0};
    CHECK_EQ_INT(riak_fold_objects(c, "b", &page, on_fold, &f2, NULL), RIAK_OK);
    CHECK_EQ_STR(ext.fold_cont, "next-page");
    CHECK_EQ_STR(ext.fold_type, "maps");
    riak_free(next);
    fold_count stop = {.stop_at = 1};
    CHECK_EQ_INT(riak_fold_objects(c, "b", &q, on_fold, &stop, NULL), RIAK_OK);
    CHECK_EQ_INT(stop.n, 1);
    /* an error after a page's continuation: no continuation is returned */
    char *after = NULL;
    fold_count f3 = {0};
    CHECK_EQ_INT(riak_fold_objects(c, "foldfail", &q, on_fold, &f3, &after), RIAK_E_SERVER);
    CHECK(after == NULL);
    riak_fold_query nostart = {0};
    CHECK_EQ_INT(riak_fold_objects(c, "b", &nostart, on_fold, &f, NULL), RIAK_E_INVALID_ARG);

    /* PB link walking runs MapReduce link phases and fetches the objects;
     * keys that are gone are dropped and duplicates collapse */
    riak_link_step one[] = {{.tag = "friend"}};
    riak_link_result res;
    CHECK_EQ_INT(riak_link_walk(c, "lb", "alice", one, 1, &res), RIAK_OK);
    CHECK_EQ_INT(res.n_phases, 1);
    if (res.n_phases == 1) {
        CHECK_EQ_INT(res.phases[0].count, 1); /* "missing" was dropped */
        if (res.phases[0].count == 1) {
            CHECK_EQ_STR(res.phases[0].items[0].bucket, "lb");
            CHECK_EQ_STR(res.phases[0].items[0].key, "bob");
            CHECK_EQ_STR(res.phases[0].items[0].object.data, "hello");
        }
    }
    riak_link_result_free(&res);
    riak_link_step two[] = {{.keep = true}, {.tag = "boss"}};
    CHECK_EQ_INT(riak_link_walk(c, "lb", "alice", two, 2, &res), RIAK_OK);
    CHECK_EQ_INT(res.n_phases, 2);
    if (res.n_phases == 2) {
        CHECK_EQ_INT(res.phases[0].count, 2);
        CHECK_EQ_INT(res.phases[1].count, 1); /* the duplicate carol collapsed */
    }
    riak_link_result_free(&res);
    CHECK_EQ_INT(riak_link_walk(c, "lb", "alice", one, 0, &res), RIAK_E_INVALID_ARG);
    /* phase entries that are not [bucket, key, tag] lists */
    CHECK_EQ_INT(riak_link_walk(c, "oddlinks", "alice", one, 1, &res), RIAK_E_PARSE);
    CHECK(res.phases == NULL && res.n_phases == 0);
    CHECK_EQ_INT(riak_link_walk(c, "numlinks", "alice", one, 1, &res), RIAK_E_PARSE);
    CHECK(res.phases == NULL && res.n_phases == 0);
    CHECK_EQ_INT(riak_link_walk(c, "numkeys", "alice", one, 1, &res), RIAK_E_PARSE);
    CHECK(res.phases == NULL && res.n_phases == 0);
    riak_client_free(c);
}

/* A callback that calls back into the client running it is refused: it would
 * otherwise reuse the connection of the stream in progress. */
typedef struct {
    riak_client *c;
    int calls;
    riak_status ping, get, fold;
} reentry;

static bool reenter_name(const char *name, size_t len, void *ctx) {
    reentry *r = ctx;
    (void)name;
    (void)len;
    if (r->calls++ == 0) {
        riak_object o;
        r->ping = riak_ping(r->c);
        CHECK(strstr(riak_client_errmsg(r->c), "busy") != NULL);
        r->get = riak_get(r->c, "b", "k", NULL, &o);
        riak_client_free(r->c); /* ignored: the client is in use */
    }
    return true;
}

static bool reenter_object(const char *key, riak_object *obj, void *ctx) {
    reentry *r = ctx;
    (void)key;
    (void)obj;
    if (r->calls++ == 0) {
        names n = {0};
        r->fold = riak_list_keys_stream(r->c, NULL, "b", on_name, &n);
        CHECK_EQ_INT(n.n, 0);
    }
    return true;
}

static void test_reentry_refused(void) {
    riak_client *c = client();
    reentry r = {.c = c, .ping = RIAK_OK, .get = RIAK_OK};
    CHECK_EQ_INT(riak_list_keys_stream(c, NULL, "b", reenter_name, &r), RIAK_OK);
    CHECK_EQ_INT(r.calls, 3); /* the outer stream is unharmed */
    CHECK_EQ_INT(r.ping, RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(r.get, RIAK_E_INVALID_ARG);
    reentry f = {.c = c, .fold = RIAK_OK};
    riak_fold_query q = {.start_key = "f1", .end_key = "f9", .max_results = 2};
    CHECK_EQ_INT(riak_fold_objects(c, "b", &q, reenter_object, &f, NULL), RIAK_OK);
    CHECK_EQ_INT(f.calls, 2);
    CHECK_EQ_INT(f.fold, RIAK_E_INVALID_ARG);
    /* the guard is released once the outer call returns */
    CHECK_EQ_INT(riak_ping(c), RIAK_OK);
    riak_client_free(c);
}

static void test_repl_queue(void) {
    riak_client *c = client();
    riak_repl_fetch_result fr;
    CHECK_EQ_INT(riak_repl_fetch(c, "q", &fr), RIAK_OK);
    CHECK_EQ_STR(ext.fetch_queue, "q");
    CHECK_EQ_STR(ext.fetch_encoding, "internal_aaehash");
    CHECK(!fr.queue_empty && !fr.deleted);
    CHECK_EQ_INT(fr.object_len, 6);
    CHECK(fr.object && fr.object[0] == 0x83);
    CHECK(fr.has_crc && fr.crc == 0xDEADBEEFu);
    CHECK_EQ_INT(fr.segment_id, 77);
    CHECK_EQ_INT(fr.segment_hash, 88);
    CHECK(fr.deleted_vclock == NULL);
    riak_repl_fetch_result_free(&fr);
    CHECK_EQ_INT(riak_repl_fetch(c, "tomb", &fr), RIAK_OK);
    CHECK(fr.deleted);
    CHECK_EQ_STR(fr.deleted_vclock, VCLOCK_B64);
    riak_repl_fetch_result_free(&fr);
    CHECK_EQ_INT(riak_repl_fetch(c, "empty", &fr), RIAK_OK);
    CHECK(fr.queue_empty && fr.object == NULL);
    riak_repl_fetch_result_free(&fr);
    CHECK_EQ_INT(riak_repl_fetch(c, "gone", &fr), RIAK_E_NOT_FOUND);

    riak_repl_key keys[] = {
        {.bucket = "b", .key = "k1", .vclock = VCLOCK_B64},
        {.bucket_type = "maps", .bucket = "b", .key = "k2", .vclock = VCLOCK_B64}};
    riak_repl_push_result pr;
    CHECK_EQ_INT(riak_repl_push(c, "q", keys, 2, &pr), RIAK_OK);
    CHECK_EQ_INT(ext.push_keys, 2);
    CHECK_EQ_INT(ext.push_vclock_len, sizeof STUB_VCLOCK);
    CHECK_EQ_STR(ext.push_type[0], "");
    CHECK_EQ_STR(ext.push_bucket[0], "b");
    CHECK_EQ_STR(ext.push_key[0], "k1");
    CHECK_EQ_STR(ext.push_type[1], "maps");
    CHECK_EQ_STR(ext.push_bucket[1], "b");
    CHECK_EQ_STR(ext.push_key[1], "k2");
    riak_repl_key nullclock = {.bucket = "b", .key = "k"};
    CHECK_EQ_INT(riak_repl_push(c, "q", &nullclock, 1, NULL), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_repl_push(c, "q", NULL, 1, NULL), RIAK_E_INVALID_ARG);
    CHECK(pr.queue_exists);
    CHECK_EQ_INT(pr.fold_length, 1);
    CHECK_EQ_INT(pr.fsync_length, 2);
    CHECK_EQ_INT(pr.realtime_length, 3);
    riak_repl_key bad = {.bucket = "b", .key = "k", .vclock = "not base64!"};
    CHECK_EQ_INT(riak_repl_push(c, "q", &bad, 1, NULL), RIAK_E_INVALID_ARG);

    riak_members m;
    int dropped = atomic_load(&ext.dropped);
    CHECK_EQ_INT(riak_cluster_membership(c, &m), RIAK_OK);
    CHECK_EQ_INT(atomic_load(&ext.dropped), dropped); /* an empty request: no crash */
    CHECK_EQ_INT(m.count, 1);
    if (m.count == 1) {
        CHECK_EQ_STR(m.items[0].ip, "10.0.0.7");
        CHECK_EQ_INT(m.items[0].port, 8087);
    }
    riak_members_free(&m);
    riak_client_free(c);
}

static void test_aae_pb(void) {
    riak_client *c = client();
    riak_aae_tree t;
    CHECK_EQ_INT(riak_aae_merge_root(c, 3, &t), RIAK_OK);
    CHECK_EQ_INT(ext.aae_n_val, 3);
    CHECK_EQ_INT(t.size, RIAK_AAE_LARGE);
    CHECK_EQ_INT(t.level1_len, 4096 * 4);
    CHECK(t.level1[0] == 0xAB);
    CHECK_EQ_INT(t.n_branches, 0);
    riak_aae_tree_free(&t);
    CHECK_EQ_INT(riak_aae_merge_root(c, 0, &t), RIAK_E_INVALID_ARG);

    uint32_t ids[] = {5};
    CHECK_EQ_INT(riak_aae_merge_branches(c, 3, ids, 1, &t), RIAK_OK);
    CHECK_EQ_INT(ext.aae_ids, 1);
    CHECK_EQ_INT(t.n_branches, 2);
    if (t.n_branches == 2) {
        CHECK_EQ_INT(t.branches[0].id, 5);
        CHECK_EQ_INT(t.branches[0].len, 1024); /* inflated */
        CHECK(t.branches[0].data[0] == 0x10 && t.branches[0].data[1023] == 0x10);
        CHECK_EQ_INT(t.branches[1].id, 9);
        CHECK(t.branches[1].data[0] == 0x11);
    }
    riak_aae_tree_free(&t);

    /* a branch inflating past max_body_bytes is refused: the 1024-byte
     * branches arrive in a response far below that limit */
    riak_config small;
    riak_config_init(&small);
    small.port = port;
    small.max_body_bytes = 1023; /* one byte short of a branch */
    riak_client *sc = client_cfg(&small);
    CHECK_EQ_INT(riak_aae_merge_branches(sc, 3, ids, 1, &t), RIAK_E_PARSE);
    CHECK(strstr(riak_client_errmsg(sc), "branch") != NULL);
    CHECK_EQ_INT(t.n_branches, 0);
    CHECK(t.branches == NULL);
    small.max_body_bytes = 1024; /* exactly a branch */
    riak_client_free(sc);
    sc = client_cfg(&small);
    CHECK_EQ_INT(riak_aae_merge_branches(sc, 3, ids, 1, &t), RIAK_OK);
    CHECK_EQ_INT(t.n_branches, 2);
    if (t.n_branches == 2) CHECK_EQ_INT(t.branches[1].len, 1024);
    riak_aae_tree_free(&t);
    riak_client_free(sc);

    /* a tree size Riak does not have */
    atomic_store(&ext.bad_tree_size, 1);
    CHECK_EQ_INT(riak_aae_merge_root(c, 3, &t), RIAK_E_PARSE);
    CHECK(t.level1 == NULL);
    atomic_store(&ext.bad_tree_size, 0);

    riak_aae_keys k;
    CHECK_EQ_INT(riak_aae_fetch_clocks_nval(c, 3, ids, 1, &k), RIAK_OK);
    CHECK_EQ_INT(k.count, 2);
    if (k.count == 2) {
        CHECK_EQ_STR(k.items[1].key, "k2");
        CHECK_EQ_STR(k.items[1].vclock, VCLOCK_B64);
        CHECK(k.items[0].bucket_type == NULL);
    }
    riak_aae_keys_free(&k);

    uint32_t segs[] = {1, 2, 3};
    riak_aae_filter f = {.bucket_type = "maps",
                         .bucket = "aaeb",
                         .start_key = "a",
                         .end_key = "z",
                         .segments = segs,
                         .n_segments = 3,
                         .segment_tree_size = RIAK_AAE_SMALL,
                         .modified_start = 100,
                         .modified_end = 200,
                         .use_hash_iv = true,
                         .hash_iv = 7};
    CHECK_EQ_INT(riak_aae_merge_range(c, &f, RIAK_AAE_XXSMALL, &t), RIAK_OK);
    CHECK_EQ_INT(ext.aae_tree_size, 0);
    CHECK_EQ_INT(ext.aae_key_range, 1);
    CHECK_EQ_INT(ext.aae_seg_filter, 1);
    CHECK_EQ_INT(ext.aae_ids, 3);
    CHECK_EQ_INT(ext.aae_mod_range, 1);
    CHECK_EQ_INT(ext.aae_mod_start, 100);
    CHECK_EQ_INT(ext.aae_mod_end, 200);
    CHECK_EQ_INT(ext.aae_prehash, 0);
    CHECK_EQ_INT(ext.aae_iv, 7);
    CHECK_EQ_STR(ext.aae_type, "maps");
    CHECK_EQ_STR(ext.aae_bucket, "aaeb");
    CHECK_EQ_STR(ext.aae_start, "a");
    CHECK_EQ_STR(ext.aae_end, "z");
    CHECK(ext.aae_segs[0] == 1 && ext.aae_segs[1] == 2 && ext.aae_segs[2] == 3);
    CHECK_EQ_INT(ext.aae_filter_size, 2 + 1); /* small */
    CHECK_EQ_INT(t.size, RIAK_AAE_XXSMALL);
    CHECK_EQ_INT(t.level1_len, 64);
    CHECK_EQ_INT(t.n_branches, 2);
    riak_aae_tree_free(&t);
    CHECK_EQ_INT(riak_aae_merge_range(c, &f, (riak_aae_tree_size)9, &t), RIAK_E_INVALID_ARG);

    memset(ext.aae_segs, 0, sizeof ext.aae_segs);
    ext.aae_filter_size = 0;
    ext.aae_bucket[0] = '\0';
    CHECK_EQ_INT(riak_aae_fetch_clocks_range(c, &f, &k), RIAK_OK);
    CHECK_EQ_STR(ext.aae_bucket, "aaeb");
    CHECK_EQ_STR(ext.aae_start, "a");
    CHECK(ext.aae_segs[0] == 1 && ext.aae_segs[2] == 3);
    CHECK_EQ_INT(ext.aae_filter_size, 2 + 1);
    CHECK_EQ_INT(k.count, 2);
    if (k.count == 2) {
        CHECK_EQ_STR(k.items[0].bucket_type, "maps");
        CHECK_EQ_STR(k.items[0].bucket, "aaeb");
        CHECK_EQ_STR(k.items[0].key, "k1");
        CHECK_EQ_STR(k.items[1].key, "k2");
    }
    riak_aae_keys_free(&k);
    /* without a segment filter or key range nothing of them is sent */
    riak_aae_filter plain = {.bucket = "aaeb"};
    CHECK_EQ_INT(riak_aae_fetch_clocks_range(c, &plain, &k), RIAK_OK);
    CHECK_EQ_INT(ext.aae_seg_filter, 0);
    CHECK_EQ_INT(ext.aae_key_range, 0);
    CHECK_EQ_INT(ext.aae_filter_size, 0);
    CHECK_EQ_STR(ext.aae_type, "");
    riak_aae_keys_free(&k);

    /* filters and enums out of range never reach the node */
    unsigned before = ext.received[214];
    riak_aae_filter bad_size = f;
    bad_size.segment_tree_size = (riak_aae_tree_size)9;
    CHECK_EQ_INT(riak_aae_fetch_clocks_range(c, &bad_size, &k), RIAK_E_INVALID_ARG);
    riak_aae_filter no_segs = f;
    no_segs.segments = NULL;
    CHECK_EQ_INT(riak_aae_fetch_clocks_range(c, &no_segs, &k), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(ext.received[214], before);
    riak_aae_key_counts kc0;
    CHECK_EQ_INT(riak_aae_find_keys(c, &f, (riak_aae_find)7, 1, &kc0), RIAK_E_INVALID_ARG);
    uint64_t n0;
    CHECK_EQ_INT(riak_aae_erase_keys(c, &f, (riak_aae_change)9, 0, &n0), RIAK_E_INVALID_ARG);
    riak_aae_tree t0;
    riak_aae_stats st0;
    riak_aae_buckets b0;
    CHECK_EQ_INT(riak_aae_merge_root(c, 3, NULL), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_aae_merge_branches(c, 3, NULL, 1, &t0), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_aae_merge_range(c, &f, RIAK_AAE_SMALL, NULL), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_aae_fetch_clocks_range(c, &f, NULL), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_aae_find_keys(c, &f, RIAK_AAE_OBJECT_SIZE, 1, NULL), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_aae_object_stats(c, &f, NULL), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_aae_object_stats(c, NULL, &st0), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_aae_list_buckets(c, 3, NULL), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_aae_list_buckets(NULL, 3, &b0), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_aae_find_tombs(c, &f, &k), RIAK_OK);
    CHECK_EQ_INT(k.count, 2);
    riak_aae_keys_free(&k);

    riak_aae_filter half = {.bucket = "aaeb", .start_key = "a"};
    CHECK_EQ_INT(riak_aae_fetch_clocks_range(c, &half, &k), RIAK_E_INVALID_ARG);
    riak_aae_filter nob = {0};
    CHECK_EQ_INT(riak_aae_fetch_clocks_range(c, &nob, &k), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_aae_fetch_clocks_range(c, NULL, &k), RIAK_E_INVALID_ARG);

    riak_aae_key_counts kc;
    CHECK_EQ_INT(riak_aae_find_keys(c, &f, RIAK_AAE_OBJECT_SIZE, 1000, &kc), RIAK_OK);
    CHECK_EQ_INT(ext.aae_finder, 1);
    CHECK_EQ_INT(ext.aae_limit, 1000);
    CHECK_EQ_INT(kc.count, 1);
    if (kc.count == 1) {
        CHECK_EQ_STR(kc.items[0].key, "big-key");
        CHECK_EQ_INT(kc.items[0].value, 4096);
    }
    riak_aae_key_counts_free(&kc);
    CHECK_EQ_INT(riak_aae_find_keys(c, &f, RIAK_AAE_SIBLING_COUNT, 2, &kc), RIAK_OK);
    CHECK_EQ_INT(ext.aae_finder, 0);
    CHECK_EQ_INT(ext.aae_limit, 2);
    riak_aae_key_counts_free(&kc);

    riak_aae_stats st;
    CHECK_EQ_INT(riak_aae_object_stats(c, &f, &st), RIAK_OK);
    CHECK_EQ_INT(st.total_count, 12);
    CHECK_EQ_INT(st.total_size, 3400);
    CHECK_EQ_INT(st.n_sizes, 2);
    CHECK_EQ_INT(st.n_siblings, 1);
    if (st.n_sizes == 2) {
        CHECK_EQ_INT(st.sizes[1].order, 3);
        CHECK_EQ_INT(st.sizes[1].count, 2);
    }
    riak_aae_stats_free(&st);

    uint64_t n = 0;
    CHECK_EQ_INT(riak_aae_reap_tombs(c, &f, RIAK_AAE_JOB, 42, &n), RIAK_OK);
    CHECK_EQ_INT(n, 7);
    CHECK_EQ_INT(ext.aae_change, 0); /* job */
    CHECK_EQ_INT(ext.aae_job, 42);
    CHECK_EQ_INT(riak_aae_reap_tombs(c, &f, RIAK_AAE_JOB, 0, &n), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_aae_erase_keys(c, &f, RIAK_AAE_COUNT, 0, &n), RIAK_OK);
    CHECK_EQ_INT(n, 8);
    CHECK_EQ_INT(ext.aae_change, 2); /* count */
    CHECK_EQ_INT(riak_aae_erase_keys(c, &f, RIAK_AAE_LOCAL, 0, &n), RIAK_OK);
    CHECK_EQ_INT(ext.aae_change, 1);
    CHECK_EQ_INT(riak_aae_repair_keys(c, &f, &n), RIAK_OK);
    CHECK_EQ_INT(n, 6);
    CHECK_EQ_STR(ext.aae_bucket, "aaeb");
    CHECK_EQ_INT(riak_aae_repl_keys(c, &f, "libriak_q", &n), RIAK_OK);
    CHECK_EQ_INT(n, 5);
    CHECK_EQ_STR(ext.aae_end, "z");
    CHECK_EQ_STR(ext.aae_queue, "libriak_q");
    CHECK_EQ_INT(riak_aae_repl_keys(c, &f, NULL, &n), RIAK_E_INVALID_ARG);

    riak_aae_buckets b;
    CHECK_EQ_INT(riak_aae_list_buckets(c, 2, &b), RIAK_OK);
    CHECK_EQ_INT(ext.aae_n_val, 2);
    CHECK_EQ_INT(b.count, 2);
    if (b.count == 2) {
        CHECK(b.items[0].bucket_type == NULL);
        CHECK_EQ_STR(b.items[1].bucket_type, "maps");
        CHECK_EQ_STR(b.items[1].bucket, "typed");
    }
    riak_aae_buckets_free(&b);
    riak_client_free(c);
}

/* Requests that change state are sent once: a connection lost after the
 * request may have been applied is reported, not repeated. Reads are retried. */
static riak_status dropped_once(riak_client *c, int code, riak_status (*call)(riak_client *),
                                unsigned *sent) {
    unsigned before = ext.received[code];
    atomic_store(&ext.drop_code, code);
    riak_status st = call(c);
    *sent = ext.received[code] - before;
    atomic_store(&ext.drop_code, 0);
    return st;
}

static const riak_aae_filter replay_filter = {.bucket = "aaeb"};

static riak_status call_push(riak_client *c) {
    riak_repl_key k = {.bucket = "b", .key = "k", .vclock = VCLOCK_B64};
    return riak_repl_push(c, "q", &k, 1, NULL);
}
static riak_status call_fetch(riak_client *c) {
    riak_repl_fetch_result fr;
    riak_status st = riak_repl_fetch(c, "q", &fr);
    if (st == RIAK_OK) riak_repl_fetch_result_free(&fr);
    return st;
}
static riak_status call_counter(riak_client *c) {
    return riak_counter_update(c, "b", "c", 1, NULL, NULL);
}
static riak_status call_reap_job(riak_client *c) {
    uint64_t n;
    return riak_aae_reap_tombs(c, &replay_filter, RIAK_AAE_JOB, 1, &n);
}
static riak_status call_erase_local(riak_client *c) {
    uint64_t n;
    return riak_aae_erase_keys(c, &replay_filter, RIAK_AAE_LOCAL, 0, &n);
}
static riak_status call_repair(riak_client *c) {
    uint64_t n;
    return riak_aae_repair_keys(c, &replay_filter, &n);
}
static riak_status call_repl_keys(riak_client *c) {
    uint64_t n;
    return riak_aae_repl_keys(c, &replay_filter, "q", &n);
}
static riak_status call_erase_count(riak_client *c) {
    uint64_t n;
    return riak_aae_erase_keys(c, &replay_filter, RIAK_AAE_COUNT, 0, &n);
}
static riak_status call_root(riak_client *c) {
    riak_aae_tree t;
    riak_status st = riak_aae_merge_root(c, 3, &t);
    if (st == RIAK_OK) riak_aae_tree_free(&t);
    return st;
}
static riak_status call_type_props(riak_client *c) {
    return riak_set_type_props(c, "maps", "{\"props\":{\"n_val\":3}}");
}
static riak_status call_counter_get(riak_client *c) {
    int64_t v;
    return riak_counter_get(c, "b", "c", NULL, &v);
}

static void test_no_replay_pb(void) {
    riak_config cfg;
    riak_config_init(&cfg);
    cfg.port = port;
    cfg.max_retries = 2;
    cfg.retry_base_ms = 1;
    riak_client *c = client_cfg(&cfg);
    struct {
        const char *name;
        int code;
        riak_status (*call)(riak_client *);
        bool repeatable;
    } cases[] = {
        {"push", 204, call_push, false},
        {"fetch", 202, call_fetch, false},
        {"counter update", 50, call_counter, false},
        {"reap job", 230, call_reap_job, false},
        {"erase local", 231, call_erase_local, false},
        {"repair", 219, call_repair, false},
        {"repl keys", 217, call_repl_keys, false},
        {"erase count", 231, call_erase_count, true},
        {"merge root", 210, call_root, true},
        {"counter get", 52, call_counter_get, true},
        {"type props", 32, call_type_props, true},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        unsigned sent = 0;
        riak_status st = dropped_once(c, cases[i].code, cases[i].call, &sent);
        if (cases[i].repeatable) {
            CHECK_EQ_INT(st, RIAK_OK);
            CHECK_EQ_INT(sent, 2);
        } else {
            CHECK_EQ_INT(st, RIAK_E_TRANSPORT);
            CHECK_EQ_INT(sent, 1);
        }
        if ((st == RIAK_OK) != cases[i].repeatable) {
            fprintf(stderr, "  case: %s\n", cases[i].name);
        }
    }
    riak_client_free(c);
}

/* A Riak 2.2 node answers the newer requests with "Unknown message code";
 * every call reports RIAK_E_UNSUPPORTED and the connection stays usable. */
static void test_old_node_pb(void) {
    riak_client *c = client();
    atomic_store(&ext.old_node, 1);
    riak_aae_tree t;
    CHECK_EQ_INT(riak_aae_merge_root(c, 3, &t), RIAK_E_UNSUPPORTED);
    riak_repl_fetch_result fr;
    CHECK_EQ_INT(riak_repl_fetch(c, "q", &fr), RIAK_E_UNSUPPORTED);
    riak_members m;
    CHECK_EQ_INT(riak_cluster_membership(c, &m), RIAK_E_UNSUPPORTED);
    char *id = NULL;
    CHECK_EQ_INT(riak_get_cluster_id(c, &id), RIAK_E_UNSUPPORTED);
    CHECK_EQ_INT(riak_get_client_id(c, &id), RIAK_E_UNSUPPORTED);
    int64_t v;
    CHECK_EQ_INT(riak_counter_get(c, "b", "c", NULL, &v), RIAK_E_UNSUPPORTED);
    CHECK(strstr(riak_client_errmsg(c), "Unknown message code") != NULL);
    CHECK_EQ_INT(riak_ping(c), RIAK_OK);
    atomic_store(&ext.old_node, 0);
    riak_client_free(c);
}

/* After a handler crash Riak closes the connection; the client must not
 * reuse it, so the next request succeeds even without retries. */
static void test_handler_crash_closes(void) {
    riak_config cfg;
    riak_config_init(&cfg);
    cfg.port = port;
    cfg.max_retries = 0;
    riak_client *c = client_cfg(&cfg);
    CHECK_EQ_INT(riak_ping(c), RIAK_OK);
    int before = atomic_load(&stub.accepts);
    riak_object o;
    CHECK_EQ_INT(riak_get(c, "b", "crash", NULL, &o), RIAK_E_SERVER);
    CHECK(strstr(riak_client_errmsg(c), "Error processing incoming message") != NULL);
    CHECK_EQ_INT(riak_ping(c), RIAK_OK);
    CHECK_EQ_INT(atomic_load(&stub.accepts) - before, 1); /* a new connection */
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
    RUN(test_new_options_pb);
    RUN(test_props_counters_ids);
    RUN(test_fold_and_links);
    RUN(test_reentry_refused);
    RUN(test_repl_queue);
    RUN(test_aae_pb);
    RUN(test_no_replay_pb);
    RUN(test_old_node_pb);
    RUN(test_handler_crash_closes);
    RUN(test_async);
    RUN(test_async_loop);
    RUN(test_tls);
    riak_global_cleanup();
    return CHECK_DONE();
}
