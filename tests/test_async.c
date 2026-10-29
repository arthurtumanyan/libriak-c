/*
 File:   test_async.c
 SPDX-License-Identifier: GPL-3.0-or-later

 Asynchronous API against tests/stub_riak.py (three nodes, RIAK_TEST_PORTS).
 */

#define _POSIX_C_SOURCE 200809L /* clock_gettime, poll */

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <curl/curl.h>

#include "check.h"
#include "riak/riak.h"

static unsigned ports[3];

static size_t sink(char *d, size_t s, size_t n, void *u) {
    (void)d;
    (void)u;
    return s * n;
}

static void set_mode(int node, const char *mode) {
    char url[96];
    snprintf(url, sizeof url, "http://127.0.0.1:%u/_mode?set=%s", ports[node], mode);
    CURL *h = curl_easy_init();
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, sink);
    curl_easy_perform(h);
    curl_easy_cleanup(h);
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

static riak_async *make(const riak_node *nodes, size_t n, unsigned retries) {
    riak_config cfg;
    riak_config_init(&cfg);
    cfg.transport = RIAK_TRANSPORT_HTTP;
    cfg.nodes = nodes;
    cfg.n_nodes = n;
    cfg.balance = RIAK_BALANCE_FAILOVER;
    cfg.max_retries = retries;
    cfg.retry_base_ms = 10;
    riak_async *a = NULL;
    CHECK_EQ_INT(riak_async_new(&cfg, &a), RIAK_OK);
    return a;
}

typedef struct {
    int done;
    int ok;
    riak_status last;
    long http;
    char data[64];
    char key[64];
} tally;

static void count(riak_async_result *res, void *ctx) {
    tally *t = ctx;
    t->done++;
    t->ok += res->status == RIAK_OK;
    t->last = res->status;
    t->http = res->http_status;
    if (res->object.data) {
        snprintf(t->data, sizeof t->data, "%s", res->object.data);
    }
    if (res->created_key) {
        snprintf(t->key, sizeof t->key, "%s", res->created_key);
    }
}

static void test_basic_ops(void) {
    riak_node node[1] = {{"127.0.0.1", ports[0], 0}};
    riak_async *a = make(node, 1, 0);
    tally t = {0};
    CHECK_EQ_INT(riak_async_ping(a, count, &t), RIAK_OK);
    riak_opts o = {.content_type = "text/plain"};
    CHECK_EQ_INT(riak_async_put(a, "ab", "k1", "hello", 5, &o, count, &t), RIAK_OK);
    CHECK_EQ_INT(riak_async_pending(a), 2);
    CHECK_EQ_INT(riak_async_run(a, 5000), 0);
    CHECK_EQ_INT(t.ok, 2);

    tally g = {0};
    CHECK_EQ_INT(riak_async_get(a, "ab", "k1", NULL, count, &g), RIAK_OK);
    CHECK_EQ_INT(riak_async_get(a, "ab", "absent", NULL, count, &g), RIAK_OK);
    CHECK_EQ_INT(riak_async_run(a, 5000), 0);
    CHECK_EQ_INT(g.done, 2);
    CHECK_EQ_INT(g.ok, 1);
    CHECK_EQ_STR(g.data, "hello");

    tally p = {0};
    CHECK_EQ_INT(riak_async_put(a, "ab", NULL, "x", 1, NULL, count, &p), RIAK_OK);
    riak_opts rb = {.return_body = true, .content_type = "text/plain"};
    CHECK_EQ_INT(riak_async_put(a, "ab", "k2", "body", 4, &rb, count, &p), RIAK_OK);
    CHECK_EQ_INT(riak_async_delete(a, "ab", "k1", NULL, count, &p), RIAK_OK);
    CHECK_EQ_INT(riak_async_run(a, 5000), 0);
    CHECK_EQ_INT(p.ok, 3);
    CHECK_EQ_STR(p.key, "auto key");
    CHECK_EQ_STR(p.data, "body");

    /* argument errors come back directly, without a callback */
    tally none = {0};
    CHECK_EQ_INT(riak_async_get(a, "", "k", NULL, count, &none), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_async_put(a, "b", "k", NULL, 3, NULL, count, &none), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_async_delete(a, "b", NULL, NULL, count, &none), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_async_ping(a, NULL, NULL), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_async_ping(NULL, count, &none), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_async_run(a, 0), 0);
    CHECK_EQ_INT(none.done, 0);
    CHECK(riak_async_client(a) != NULL);
    riak_async_free(a);
}

static void test_concurrency(void) {
    riak_node node[1] = {{"127.0.0.1", ports[1], 0}};
    riak_async *a = make(node, 1, 0);
    set_mode(1, "slow"); /* every request takes 300 ms */
    tally t = {0};
    double t0 = now_ms();
    for (int i = 0; i < 10; i++) {
        CHECK_EQ_INT(riak_async_ping(a, count, &t), RIAK_OK);
    }
    CHECK_EQ_INT(riak_async_run(a, 10000), 0);
    double took = now_ms() - t0;
    set_mode(1, "ok");
    CHECK_EQ_INT(t.ok, 10);
    /* sequentially this would take 3 s */
    CHECK(took < 1500.0);
    riak_async_free(a);
}

static void test_retries(void) {
    /* a dead node first: refused connections fail over */
    riak_node nodes[2] = {{"127.0.0.1", 1, 0}, {"127.0.0.1", ports[0], 0}};
    riak_async *a = make(nodes, 2, 2);
    tally t = {0};
    for (int i = 0; i < 5; i++) {
        CHECK_EQ_INT(riak_async_ping(a, count, &t), RIAK_OK);
    }
    CHECK_EQ_INT(riak_async_run(a, 5000), 0);
    CHECK_EQ_INT(t.ok, 5);
    riak_node_info info;
    CHECK_EQ_INT(riak_client_node_info(riak_async_client(a), 0, &info), RIAK_OK);
    CHECK(info.failures >= 1);
    riak_async_free(a);

    /* 503: GET is retried on the next node, POST is not */
    riak_node two[2] = {{"127.0.0.1", ports[1], 0}, {"127.0.0.1", ports[2], 0}};
    a = make(two, 2, 2);
    set_mode(1, "503");
    tally g = {0}, post = {0};
    CHECK_EQ_INT(riak_async_get(a, "ab", "k2", NULL, count, &g), RIAK_OK);
    CHECK_EQ_INT(riak_async_put(a, "ab", NULL, "x", 1, NULL, count, &post), RIAK_OK);
    CHECK_EQ_INT(riak_async_run(a, 5000), 0);
    set_mode(1, "ok");
    CHECK_EQ_INT(g.ok, 1);
    CHECK_EQ_INT(post.last, RIAK_E_HTTP);
    CHECK_EQ_INT(post.http, 503);
    riak_async_free(a);
}

static void cancelled(riak_async_result *res, void *ctx) {
    tally *t = ctx;
    t->done++;
    t->last = res->status;
    CHECK_EQ_STR(res->errmsg, "cancelled");
}

static void test_cancel_and_timeout(void) {
    riak_node node[1] = {{"127.0.0.1", ports[2], 0}};
    riak_async *a = make(node, 1, 0);
    set_mode(2, "slow");
    tally t = {0};
    CHECK_EQ_INT(riak_async_ping(a, cancelled, &t), RIAK_OK);
    CHECK_EQ_INT(riak_async_ping(a, cancelled, &t), RIAK_OK);
    CHECK_EQ_INT(riak_async_run(a, 50), 2); /* still running after 50 ms */
    riak_async_free(a);
    set_mode(2, "ok");
    CHECK_EQ_INT(t.done, 2);
    CHECK_EQ_INT(t.last, RIAK_E_CANCELLED);
}

/* A callback that keeps the object and chains a delete. */
typedef struct {
    riak_async *a;
    riak_object kept;
    tally del;
} chain;

static void keep_and_delete(riak_async_result *res, void *ctx) {
    chain *c = ctx;
    CHECK_EQ_INT(res->status, RIAK_OK);
    c->kept = res->object; /* take ownership */
    memset(&res->object, 0, sizeof res->object);
    CHECK_EQ_INT(riak_async_delete(c->a, "ab", "k2", NULL, count, &c->del), RIAK_OK);
}

static void test_chaining_and_ownership(void) {
    riak_node node[1] = {{"127.0.0.1", ports[0], 0}};
    chain c = {.a = make(node, 1, 0)};
    CHECK_EQ_INT(riak_async_get(c.a, "ab", "k2", NULL, keep_and_delete, &c), RIAK_OK);
    CHECK_EQ_INT(riak_async_run(c.a, 5000), 0);
    CHECK_EQ_STR(c.kept.data, "body");
    CHECK_EQ_INT(c.del.ok, 1);
    riak_object_free(&c.kept);
    riak_async_free(c.a);
}

/* ---- an external poll() loop driven through the hooks ---- */

typedef struct {
    struct pollfd fds[16];
    int n;
    long timer_ms; /* -1: none */
    int socket_calls, timer_calls;
} loop;

static void on_socket(int fd, int events, void *ctx) {
    loop *l = ctx;
    l->socket_calls++;
    int i = 0;
    while (i < l->n && l->fds[i].fd != fd)
        i++;
    if (events & RIAK_POLL_REMOVE) {
        if (i < l->n) l->fds[i] = l->fds[--l->n];
        return;
    }
    if (i == l->n && l->n < 16) l->fds[l->n++].fd = fd;
    l->fds[i].events =
        (short)(((events & RIAK_POLL_IN) ? POLLIN : 0) | ((events & RIAK_POLL_OUT) ? POLLOUT : 0));
}

static void on_timer(long timeout_ms, void *ctx) {
    loop *l = ctx;
    l->timer_calls++;
    l->timer_ms = timeout_ms;
}

static void drive(riak_async *a, loop *l) {
    double end = now_ms() + 5000;
    while (riak_async_pending(a) > 0 && now_ms() < end) {
        int wait = l->timer_ms < 0 ? 100 : (int)(l->timer_ms < 100 ? l->timer_ms : 100);
        int r = poll(l->fds, (nfds_t)l->n, wait);
        if (r == 0) {
            riak_async_on_timeout(a);
            continue;
        }
        struct pollfd ready[16];
        int n = l->n;
        memcpy(ready, l->fds, sizeof ready);
        for (int i = 0; i < n; i++) {
            if (ready[i].revents) {
                int ev = ((ready[i].revents & (POLLIN | POLLHUP | POLLERR)) ? RIAK_POLL_IN : 0) |
                         ((ready[i].revents & POLLOUT) ? RIAK_POLL_OUT : 0);
                riak_async_on_socket(a, ready[i].fd, ev);
            }
        }
    }
}

static void test_external_loop(void) {
    riak_node nodes[2] = {{"127.0.0.1", 1, 0}, {"127.0.0.1", ports[0], 0}};
    riak_async *a = make(nodes, 2, 2);
    loop l = {.timer_ms = -1};
    CHECK_EQ_INT(riak_async_set_loop(a, on_socket, on_timer, &l), RIAK_OK);
    tally t = {0};
    for (int i = 0; i < 4; i++) {
        CHECK_EQ_INT(riak_async_ping(a, count, &t), RIAK_OK);
    }
    CHECK_EQ_INT(riak_async_get(a, "ab", "k2", NULL, count, &t), RIAK_OK);
    drive(a, &l);
    CHECK_EQ_INT(t.done, 5);
    CHECK_EQ_INT(t.ok, 4); /* k2 was deleted by the chaining test */
    CHECK(l.socket_calls > 0 && l.timer_calls > 0);
    CHECK_EQ_INT(l.n, 0); /* every descriptor was handed back */
    CHECK_EQ_INT(riak_async_set_loop(a, NULL, on_timer, &l), RIAK_E_INVALID_ARG);
    riak_async_free(a);
}

/* ---- the data path and streams: each async result equals the blocking one */

typedef struct {
    int done;
    riak_status status;
    riak_async_kind kind;
    riak_async_result kept; /* members taken out of the result */
} grab;

static void take(riak_async_result *res, void *ctx) {
    grab *g = ctx;
    g->done++;
    g->status = res->status;
    g->kind = res->kind;
    g->kept = *res; /* ownership moves here; the library frees what is left */
    memset(&res->object, 0, sizeof res->object);
    memset(&res->siblings, 0, sizeof res->siblings);
    memset(&res->datatype, 0, sizeof res->datatype);
    memset(&res->index, 0, sizeof res->index);
    memset(&res->search, 0, sizeof res->search);
    res->json = NULL;
    res->created_key = NULL;
    res->continuation = NULL;
    g->kept.errmsg = NULL;
}

static void grab_free(grab *g) {
    riak_object_free(&g->kept.object);
    riak_object_set_free(&g->kept.siblings);
    riak_datatype_free(&g->kept.datatype);
    riak_index_result_free(&g->kept.index);
    riak_search_result_free(&g->kept.search);
    riak_free(g->kept.json);
    riak_free(g->kept.created_key);
    riak_free(g->kept.continuation);
    memset(g, 0, sizeof *g);
}

static riak_status wait_one(riak_async *a, grab *g) {
    while (g->done == 0 && riak_async_run(a, 5000) > 0) {
    }
    CHECK_EQ_INT(g->done, 1);
    return g->status;
}

typedef struct {
    int n;
    int stop_at;
    char last[64];
} names;

static bool on_name(const char *name, size_t len, void *ctx) {
    names *n = ctx;
    n->n++;
    snprintf(n->last, sizeof n->last, "%.*s", (int)len, name);
    return n->stop_at == 0 || n->n < n->stop_at;
}

static bool on_match(const char *key, const char *term, void *ctx) {
    (void)term;
    names *n = ctx;
    n->n++;
    snprintf(n->last, sizeof n->last, "%s", key);
    return n->stop_at == 0 || n->n < n->stop_at;
}

static bool on_phase(unsigned phase, const char *data, void *ctx) {
    names *n = ctx;
    n->n++;
    snprintf(n->last, sizeof n->last, "%u:%s", phase, data);
    return true;
}

static void test_data_path(void) {
    riak_node node[1] = {{"127.0.0.1", ports[2], 0}};
    riak_async *a = make(node, 1, 0);
    riak_client *c = riak_async_client(a); /* the blocking reference, same stub */
    grab g = {0};

    /* siblings */
    riak_opts multi = {.bucket_type = "multi", .content_type = "text/plain"};
    CHECK_EQ_INT(riak_put(c, "asb", "k", "one", 3, &multi, NULL, NULL), RIAK_OK);
    CHECK_EQ_INT(riak_put(c, "asb", "k", "two", 3, &multi, NULL, NULL), RIAK_OK);
    CHECK_EQ_INT(riak_async_get_siblings(a, "asb", "k", &multi, take, &g), RIAK_OK);
    CHECK_EQ_INT(wait_one(a, &g), RIAK_OK);
    CHECK_EQ_INT(g.kind, RIAK_ASYNC_GET_SIBLINGS);
    CHECK_EQ_INT(g.kept.siblings.count, 2);
    CHECK(g.kept.siblings.vclock != NULL);
    grab_free(&g);

    /* data types: update with return_body and a generated key, then fetch */
    riak_dt_op *op = riak_dt_op_new(RIAK_DT_COUNTER);
    riak_dt_op_increment(op, 7);
    riak_dt_opts rb = {.return_body = true};
    CHECK_EQ_INT(riak_async_dt_update(a, "counters", "adt", "c", op, NULL, &rb, take, &g), RIAK_OK);
    riak_dt_op_free(op); /* encoded on submission */
    CHECK_EQ_INT(wait_one(a, &g), RIAK_OK);
    CHECK(g.kept.datatype.value.type == RIAK_DT_COUNTER && g.kept.datatype.value.counter == 7);
    grab_free(&g);
    CHECK_EQ_INT(riak_async_dt_fetch(a, "counters", "adt", "c", NULL, take, &g), RIAK_OK);
    CHECK_EQ_INT(wait_one(a, &g), RIAK_OK);
    CHECK_EQ_INT(g.kept.datatype.value.counter, 7);
    grab_free(&g);
    op = riak_dt_op_new(RIAK_DT_COUNTER);
    riak_dt_op_increment(op, 1);
    CHECK_EQ_INT(riak_async_dt_update(a, "counters", "adt", NULL, op, NULL, NULL, take, &g),
                 RIAK_OK);
    riak_dt_op_free(op);
    CHECK_EQ_INT(wait_one(a, &g), RIAK_OK);
    CHECK(g.kept.created_key != NULL);
    grab_free(&g);
    CHECK_EQ_INT(riak_async_dt_fetch(a, "counters", "adt", "absent", NULL, take, &g), RIAK_OK);
    CHECK_EQ_INT(wait_one(a, &g), RIAK_E_NOT_FOUND);
    grab_free(&g);

    /* legacy counters */
    CHECK_EQ_INT(riak_async_counter_update(a, "acnt", "c", 5, NULL, take, &g), RIAK_OK);
    CHECK_EQ_INT(wait_one(a, &g), RIAK_OK);
    CHECK_EQ_INT(g.kept.counter, 5);
    grab_free(&g);
    CHECK_EQ_INT(riak_async_counter_get(a, "acnt", "c", NULL, take, &g), RIAK_OK);
    CHECK_EQ_INT(wait_one(a, &g), RIAK_OK);
    CHECK_EQ_INT(g.kind, RIAK_ASYNC_COUNTER_GET);
    CHECK_EQ_INT(g.kept.counter, 5);
    grab_free(&g);

    /* secondary indexes: a page, and the same matches streamed */
    for (int i = 0; i < 4; i++) {
        char key[8];
        snprintf(key, sizeof key, "p%d", i);
        riak_kv_pair idx[] = {{"grp_bin", i % 2 ? "odd" : "even"}};
        riak_opts o = {.indexes = idx, .n_indexes = 1};
        CHECK_EQ_INT(riak_put(c, "aix", key, "x", 1, &o, NULL, NULL), RIAK_OK);
    }
    riak_index_query q = {.index = "grp_bin", .value = "even", .return_terms = true};
    riak_index_result ref;
    CHECK_EQ_INT(riak_index_query_run(c, "aix", &q, &ref), RIAK_OK);
    CHECK_EQ_INT(riak_async_index_query(a, "aix", &q, take, &g), RIAK_OK);
    CHECK_EQ_INT(wait_one(a, &g), RIAK_OK);
    CHECK_EQ_INT(g.kept.index.count, ref.count);
    CHECK_EQ_INT(g.kept.index.count, 2);
    if (g.kept.index.count == 2 && ref.count == 2) {
        CHECK_EQ_STR(g.kept.index.keys[0], ref.keys[0]);
        CHECK_EQ_STR(g.kept.index.keys[1], ref.keys[1]);
        CHECK((g.kept.index.terms == NULL) == (ref.terms == NULL));
    }
    riak_index_result_free(&ref);
    grab_free(&g);
    names m = {0};
    CHECK_EQ_INT(riak_async_index_query_stream(a, "aix", &q, on_match, &m, take, &g), RIAK_OK);
    CHECK_EQ_INT(wait_one(a, &g), RIAK_OK);
    CHECK_EQ_INT(m.n, 2);
    grab_free(&g);
    names one = {.stop_at = 1};
    CHECK_EQ_INT(riak_async_index_query_stream(a, "aix", &q, on_match, &one, take, &g), RIAK_OK);
    CHECK_EQ_INT(wait_one(a, &g), RIAK_OK); /* stopped by the callback: not an error */
    CHECK_EQ_INT(one.n, 1);
    grab_free(&g);

    /* MapReduce, collected and streamed */
    CHECK_EQ_INT(riak_put(c, "amr", "a", "\"va\"", 4, NULL, NULL, NULL), RIAK_OK);
    CHECK_EQ_INT(riak_put(c, "amr", "b", "\"vb\"", 4, NULL, NULL, NULL), RIAK_OK);
    const char *job = "{\"inputs\":[[\"amr\",\"a\"],[\"amr\",\"b\"]],\"query\":["
                      "{\"map\":{\"language\":\"erlang\",\"module\":\"riak_kv_mapreduce\","
                      "\"function\":\"map_object_value\"}},"
                      "{\"reduce\":{\"language\":\"erlang\",\"module\":\"riak_kv_mapreduce\","
                      "\"function\":\"reduce_count_inputs\"}}]}";
    CHECK_EQ_INT(riak_async_mapred(a, job, take, &g), RIAK_OK);
    CHECK_EQ_INT(wait_one(a, &g), RIAK_OK);
    CHECK_EQ_STR(g.kept.json, "[2]");
    grab_free(&g);
    names ph = {0};
    CHECK_EQ_INT(riak_async_mapred_stream(a, job, on_phase, &ph, take, &g), RIAK_OK);
    CHECK_EQ_INT(wait_one(a, &g), RIAK_OK);
    CHECK(ph.n >= 1);
    grab_free(&g);

    /* bucket properties */
    CHECK_EQ_INT(
        riak_async_set_bucket_props(a, NULL, "aprops", "{\"props\":{\"n_val\":2}}", take, &g),
        RIAK_OK);
    CHECK_EQ_INT(wait_one(a, &g), RIAK_OK);
    grab_free(&g);
    CHECK_EQ_INT(riak_async_get_bucket_props(a, NULL, "aprops", take, &g), RIAK_OK);
    CHECK_EQ_INT(wait_one(a, &g), RIAK_OK);
    char *props = NULL;
    CHECK_EQ_INT(riak_get_bucket_props(c, "aprops", &props), RIAK_OK);
    CHECK_EQ_STR(g.kept.json, props); /* the document the blocking call returns */
    CHECK(props && strstr(props, "n_val") != NULL);
    riak_free(props);
    grab_free(&g);

    /* listings: every key of the 1000-key bucket, and an early stop */
    names all = {0};
    CHECK_EQ_INT(riak_async_list_keys_stream(a, NULL, "many", on_name, &all, take, &g), RIAK_OK);
    CHECK_EQ_INT(wait_one(a, &g), RIAK_OK);
    CHECK_EQ_INT(all.n, 1000);
    grab_free(&g);
    names three = {.stop_at = 3};
    CHECK_EQ_INT(riak_async_list_keys_stream(a, NULL, "many", on_name, &three, take, &g), RIAK_OK);
    CHECK_EQ_INT(wait_one(a, &g), RIAK_OK);
    CHECK_EQ_INT(three.n, 3);
    grab_free(&g);
    names buckets = {0};
    CHECK_EQ_INT(riak_async_list_buckets_stream(a, NULL, on_name, &buckets, take, &g), RIAK_OK);
    CHECK_EQ_INT(wait_one(a, &g), RIAK_OK);
    CHECK(buckets.n >= 4);
    grab_free(&g);

    /* argument errors come back directly, without a callback */
    CHECK_EQ_INT(riak_async_index_query(a, "", &q, take, &g), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_async_index_query_stream(a, "aix", &q, NULL, NULL, take, &g),
                 RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_async_mapred(a, NULL, take, &g), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_async_set_bucket_props(a, NULL, "b", NULL, take, &g), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_async_dt_fetch(a, "", "b", "k", NULL, take, &g), RIAK_E_INVALID_ARG);
    riak_opts typed = {.bucket_type = "t1"};
    CHECK_EQ_INT(riak_async_counter_get(a, "b", "k", &typed, take, &g), RIAK_E_UNSUPPORTED);
    CHECK_EQ_INT(riak_async_run(a, 0), 0);
    CHECK_EQ_INT(g.done, 0);
    riak_async_free(a);
}

/* A stream cancelled by riak_async_free() frees its parser. */
static void test_stream_cancel(void) {
    riak_node node[1] = {{"127.0.0.1", ports[2], 0}};
    riak_async *a = make(node, 1, 0);
    grab g = {0};
    names n = {0};
    CHECK_EQ_INT(riak_async_list_keys_stream(a, NULL, "many", on_name, &n, take, &g), RIAK_OK);
    riak_async_run(a, 1); /* the listing has started, perhaps delivered some names */
    riak_async_free(a);
    CHECK_EQ_INT(g.done, 1);
    CHECK(g.status == RIAK_E_CANCELLED || g.status == RIAK_OK);
    grab_free(&g);
}

int main(void) {
    const char *ps = getenv("RIAK_TEST_PORTS");
    if (ps == NULL || sscanf(ps, "%u,%u,%u", &ports[0], &ports[1], &ports[2]) != 3) {
        fprintf(stderr, "run through stub_riak.py (RIAK_TEST_PORTS not set)\n");
        return 77;
    }
    CHECK_EQ_INT(riak_global_init(), RIAK_OK);
    RUN(test_basic_ops);
    RUN(test_concurrency);
    RUN(test_retries);
    RUN(test_cancel_and_timeout);
    RUN(test_chaining_and_ownership);
    RUN(test_external_loop);
    RUN(test_data_path);
    RUN(test_stream_cancel);
    riak_global_cleanup();
    return CHECK_DONE();
}
