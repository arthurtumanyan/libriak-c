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
    riak_node node[1] = {{"127.0.0.1", ports[0]}};
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
    riak_node node[1] = {{"127.0.0.1", ports[1]}};
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
    riak_node nodes[2] = {{"127.0.0.1", 1}, {"127.0.0.1", ports[0]}};
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
    riak_node two[2] = {{"127.0.0.1", ports[1]}, {"127.0.0.1", ports[2]}};
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
    riak_node node[1] = {{"127.0.0.1", ports[2]}};
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
    riak_node node[1] = {{"127.0.0.1", ports[0]}};
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
    riak_node nodes[2] = {{"127.0.0.1", 1}, {"127.0.0.1", ports[0]}};
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
    riak_global_cleanup();
    return CHECK_DONE();
}
