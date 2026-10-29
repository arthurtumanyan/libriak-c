/*
 File:   test_compat.c
 SPDX-License-Identifier: GPL-3.0-or-later

 The 2.0 entry points of src/compat_v2.c against tests/stub_riak.py: each
 takes its structure in the 2.0.0 layout, keeps the 2.0.0 defaults (HTTP)
 and gives the same result as the current function. The shared library
 binds them to LIBRIAK_2.0; here they are called directly
 (RIAK_COMPAT_NO_SYMVER). Programs built against 2.0.0 themselves are run in
 the "abi" CI job.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "compat_v2.h"
#include "riak/riak.h"

static unsigned port;

static void test_config(void) {
    riak_config_v2 cfg;
    memset(&cfg, 0xA5, sizeof cfg);
    compat_v20_config_init(&cfg);
    CHECK_EQ_STR(cfg.host, "127.0.0.1");
    CHECK_EQ_INT(cfg.port, 8098); /* 2.0 spoke HTTP */
    CHECK(cfg.tls_verify && !cfg.tls);
    CHECK_EQ_INT(cfg.max_body_bytes, 64 * 1024 * 1024);
    CHECK_EQ_INT(cfg.max_retries, 2);
    CHECK_EQ_INT(cfg.retry_base_ms, 50);
    CHECK_EQ_INT(cfg.retry_max_ms, 2000);
    CHECK_EQ_INT(cfg.fail_threshold, 3);
    CHECK_EQ_INT(cfg.eject_ms, 5000);
    CHECK_EQ_INT(cfg.log_level, RIAK_LOG_WARN);
    CHECK(cfg.nodes == NULL && cfg.n_nodes == 0 && cfg.username == NULL);

    riak_client *c = NULL;
    cfg.port = 70000;
    CHECK_EQ_INT(compat_v20_client_new(&cfg, &c), RIAK_E_INVALID_ARG);
    CHECK(c == NULL);
    CHECK_EQ_INT(compat_v20_client_new(&cfg, NULL), RIAK_E_INVALID_ARG);

    /* no configuration: the 2.0 defaults, HTTP on 127.0.0.1:8098 */
    CHECK_EQ_INT(compat_v20_client_new(NULL, &c), RIAK_OK);
    if (c) {
        riak_status st = riak_ping(c);
        if (st != RIAK_OK) { /* nothing listens there in the test */
            /* libcurl's wording: HTTP, port 8098 (Protocol Buffers would be :8087) */
            CHECK(strstr(riak_client_errmsg(c), "port 8098") != NULL);
        }
        riak_client_free(c);
    }
    riak_async *a = NULL;
    CHECK_EQ_INT(compat_v20_async_new(NULL, &a), RIAK_OK);
    riak_async_free(a);
}

static riak_client *client(void) {
    riak_config_v2 cfg;
    compat_v20_config_init(&cfg);
    /* the node list of 2.0: host and port only */
    riak_node_v2 nodes[] = {{"127.0.0.1", port}};
    cfg.nodes = nodes;
    cfg.n_nodes = 1;
    cfg.client_id = "compat";
    riak_client *c = NULL;
    CHECK_EQ_INT(compat_v20_client_new(&cfg, &c), RIAK_OK);
    return c;
}

static void test_objects(void) {
    riak_client *c = client();
    if (c == NULL) return;
    CHECK_EQ_INT(riak_ping(c), RIAK_OK);
    CHECK_EQ_INT(riak_client_http_status(c), 200); /* HTTP, not Protocol Buffers */

    riak_kv_pair meta[] = {{"owner", "ops"}};
    riak_opts_v2 put = {.content_type = "text/plain",
                        .meta = meta,
                        .n_meta = 1,
                        .return_body = true,
                        .w = RIAK_Q_QUORUM};
    riak_object o;
    CHECK_EQ_INT(compat_v20_put(c, "compat", "k1", "hello", 5, &put, &o, NULL), RIAK_OK);
    CHECK(o.data && o.len == 5 && memcmp(o.data, "hello", 5) == 0);
    riak_object_free(&o);
    char *created = NULL;
    CHECK_EQ_INT(compat_v20_put(c, "compat", NULL, "x", 1, &put, NULL, &created), RIAK_OK);
    CHECK(created != NULL);
    riak_free(created);

    riak_opts_v2 get = {.r = 1};
    CHECK_EQ_INT(compat_v20_get(c, "compat", "k1", &get, &o), RIAK_OK);
    CHECK_EQ_INT(o.n_meta, 1);
    if (o.n_meta == 1) CHECK_EQ_STR(o.meta[0].value, "ops");
    riak_object_free(&o);
    CHECK_EQ_INT(compat_v20_get(c, "compat", "k1", NULL, &o), RIAK_OK);
    riak_object_free(&o);
    riak_object_set set;
    CHECK_EQ_INT(compat_v20_get_siblings(c, "compat", "k1", &get, &set), RIAK_OK);
    CHECK_EQ_INT(set.count, 1);
    riak_object_set_free(&set);

    riak_opts_v2 del = {.rw = 2};
    CHECK_EQ_INT(compat_v20_delete(c, "compat", "k1", &del), RIAK_OK);
    CHECK_EQ_INT(compat_v20_get(c, "compat", "k1", &get, &o), RIAK_E_NOT_FOUND);
    riak_client_free(c);
}

static bool count_key(const char *key, const char *term, void *ctx) {
    (void)key;
    (void)term;
    ++*(int *)ctx;
    return true;
}

static void test_queries_and_datatypes(void) {
    riak_client *c = client();
    if (c == NULL) return;
    for (int i = 0; i < 3; i++) {
        char key[8];
        snprintf(key, sizeof key, "p%d", i);
        riak_kv_pair idx[] = {{"group_bin", i == 1 ? "odd" : "even"}};
        riak_opts_v2 o = {.indexes = idx, .n_indexes = 1};
        CHECK_EQ_INT(compat_v20_put(c, "compat_ix", key, "x", 1, &o, NULL, NULL), RIAK_OK);
    }
    riak_index_query_v2 q = {.index = "group_bin", .value = "even", .return_terms = false};
    riak_index_result r;
    CHECK_EQ_INT(compat_v20_index_query_run(c, "compat_ix", &q, &r), RIAK_OK);
    CHECK_EQ_INT(r.count, 2);
    riak_index_result_free(&r);
    int n = 0;
    CHECK_EQ_INT(compat_v20_index_query_stream(c, "compat_ix", &q, count_key, &n, NULL), RIAK_OK);
    CHECK_EQ_INT(n, 2);

    riak_dt_op *op = riak_dt_op_new(RIAK_DT_COUNTER);
    riak_dt_op_increment(op, 5);
    riak_dt_opts_v2 rb = {.return_body = true};
    riak_datatype dt;
    CHECK_EQ_INT(compat_v20_dt_update(c, "counters", "compat_dt", "c", op, NULL, &rb, &dt, NULL),
                 RIAK_OK);
    CHECK(dt.value.type == RIAK_DT_COUNTER && dt.value.counter == 5);
    riak_datatype_free(&dt);
    riak_dt_opts_v2 none = {0};
    CHECK_EQ_INT(compat_v20_dt_fetch(c, "counters", "compat_dt", "c", &none, &dt), RIAK_OK);
    CHECK_EQ_INT(dt.value.counter, 5);
    riak_datatype_free(&dt);
    riak_dt_op_free(op);
    riak_client_free(c);
}

typedef struct {
    int done;
    riak_status st[3];
    char body[16];
} async_state;

static void on_put(riak_async_result *res, void *ctx) {
    async_state *s = ctx;
    s->st[0] = res->status;
    s->done++;
}

static void on_get(riak_async_result *res, void *ctx) {
    async_state *s = ctx;
    s->st[1] = res->status;
    if (res->status == RIAK_OK && res->object.len < sizeof s->body) {
        memcpy(s->body, res->object.data, res->object.len);
    }
    s->done++;
}

static void on_delete(riak_async_result *res, void *ctx) {
    async_state *s = ctx;
    s->st[2] = res->status;
    s->done++;
}

static void test_async(void) {
    riak_config_v2 cfg;
    compat_v20_config_init(&cfg);
    cfg.port = port;
    riak_async *a = NULL;
    CHECK_EQ_INT(compat_v20_async_new(&cfg, &a), RIAK_OK);
    if (a == NULL) return;
    async_state s = {0};
    riak_opts_v2 o = {.content_type = "text/plain"};
    CHECK_EQ_INT(compat_v20_async_put(a, "compat_as", "k", "async", 5, &o, on_put, &s), RIAK_OK);
    while (s.done < 1 && riak_async_run(a, 2000) > 0) {
    }
    CHECK_EQ_INT(compat_v20_async_get(a, "compat_as", "k", &o, on_get, &s), RIAK_OK);
    while (s.done < 2 && riak_async_run(a, 2000) > 0) {
    }
    CHECK_EQ_INT(compat_v20_async_delete(a, "compat_as", "k", NULL, on_delete, &s), RIAK_OK);
    while (s.done < 3 && riak_async_run(a, 2000) > 0) {
    }
    CHECK_EQ_INT(s.done, 3);
    CHECK(s.st[0] == RIAK_OK && s.st[1] == RIAK_OK && s.st[2] == RIAK_OK);
    CHECK_EQ_STR(s.body, "async");
    riak_async_free(a);
}

int main(void) {
    const char *p = getenv("RIAK_TEST_PORT");
    if (p == NULL) {
        fprintf(stderr, "run through stub_riak.py (RIAK_TEST_PORT not set)\n");
        return 77;
    }
    port = (unsigned)strtoul(p, NULL, 10);
    CHECK_EQ_INT(riak_global_init(), RIAK_OK);
    RUN(test_config);
    RUN(test_objects);
    RUN(test_queries_and_datatypes);
    RUN(test_async);
    riak_global_cleanup();
    return CHECK_DONE();
}
