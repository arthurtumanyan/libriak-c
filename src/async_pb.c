/*
 File:   async_pb.c
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later

 Concurrent Protocol Buffers requests on one thread. Each node has a pool
 of connections (at most riak_config.pb_max_connections_per_node); a
 request takes an idle connection, opens a new one, or waits for one in a
 per-node queue. Retries, node selection and health tracking go through
 the same cluster code as blocking calls; backoffs are timers.
 */

#include "pb.h"

#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_MAX_CONNECTIONS 8

typedef struct pbop pbop;
struct pbop {
    riak_pb_async *a;
    enum riak_pb_async_kind kind;
    riak_pb_prepared p;
    riak_opts opts; /* only resolver / resolver_ctx are used after submission */
    bool generated_key;
    riak_async_cb cb;
    void *ctx;
    riak_pb_response resp;
    riak_pbconn *conn; /* while an attempt is in flight */
    size_t node;
    unsigned attempt;
    long long retry_at; /* > 0: waiting for a retry */
    bool queued;        /* waiting for a connection */
    pbop *prev, *next;  /* every pending request */
    pbop *qnext;        /* per-node wait queue */
};

typedef struct {
    riak_pbconn *k;
    int watched; /* RIAK_POLL_* last reported to the external loop */
    bool kick;   /* needs a step before waiting for events */
} active_conn;

struct riak_pb_async {
    riak_client *c;
    pbop *ops;
    size_t pending;
    unsigned max_conns;
    riak_pbconn **idle; /* per node, linked through pool_next */
    unsigned *open;     /* per node: connections in use or idle */
    pbop **qhead, **qtail;
    active_conn *active;
    size_t n_active, active_cap;
    riak_socket_fn socket_fn;
    riak_timer_fn timer_fn;
    void *loop_ctx;
};

static const riak_opts no_opts;

/* ---- bookkeeping ------------------------------------------------------ */

static void unlink_op(riak_pb_async *a, pbop *o) {
    if (o->prev)
        o->prev->next = o->next;
    else
        a->ops = o->next;
    if (o->next) o->next->prev = o->prev;
    o->prev = o->next = NULL;
    a->pending--;
}

static void enqueue(riak_pb_async *a, pbop *o) {
    o->queued = true;
    o->qnext = NULL;
    if (a->qtail[o->node])
        a->qtail[o->node]->qnext = o;
    else
        a->qhead[o->node] = o;
    a->qtail[o->node] = o;
}

static pbop *dequeue(riak_pb_async *a, size_t node) {
    pbop *o = a->qhead[node];
    if (o) {
        a->qhead[node] = o->qnext;
        if (a->qhead[node] == NULL) a->qtail[node] = NULL;
        o->qnext = NULL;
        o->queued = false;
    }
    return o;
}

static void unqueue(riak_pb_async *a, pbop *o) {
    pbop **pp = &a->qhead[o->node];
    pbop *prev = NULL;
    while (*pp && *pp != o) {
        prev = *pp;
        pp = &(*pp)->qnext;
    }
    if (*pp == o) {
        *pp = o->qnext;
        if (a->qtail[o->node] == o) a->qtail[o->node] = prev;
    }
    o->queued = false;
}

static active_conn *find_active(riak_pb_async *a, const riak_pbconn *k) {
    for (size_t i = 0; i < a->n_active; i++) {
        if (a->active[i].k == k) return &a->active[i];
    }
    return NULL;
}

static bool add_active(riak_pb_async *a, riak_pbconn *k) {
    if (a->n_active == a->active_cap) {
        size_t ncap = a->active_cap ? a->active_cap * 2 : 16;
        active_conn *grown = realloc(a->active, ncap * sizeof *grown);
        if (grown == NULL) return false;
        a->active = grown;
        a->active_cap = ncap;
    }
    a->active[a->n_active++] = (active_conn){.k = k, .kick = true};
    return true;
}

static void watch(riak_pb_async *a, active_conn *e, int events) {
    if (a->socket_fn && e->k->fd >= 0 && e->watched != events) {
        a->socket_fn(e->k->fd, events ? events : RIAK_POLL_REMOVE, a->loop_ctx);
    }
    e->watched = events;
}

static void remove_active(riak_pb_async *a, riak_pbconn *k) {
    active_conn *e = find_active(a, k);
    if (e == NULL) return;
    if (a->socket_fn && e->watched && k->fd >= 0) {
        a->socket_fn(k->fd, RIAK_POLL_REMOVE, a->loop_ctx);
    }
    *e = a->active[--a->n_active];
}

static void release_conn(riak_pb_async *a, riak_pbconn *k) {
    remove_active(a, k);
    k->owner = NULL;
    if (k->state == RIAK_PBC_READY) {
        k->pool_next = a->idle[k->node];
        a->idle[k->node] = k;
    } else {
        a->open[k->node]--;
        riak_pbconn_free(k);
    }
}

/* ---- completion -------------------------------------------------------- */

static void finish_op(riak_pb_async *a, pbop *o, riak_status st) {
    riak_client *c = a->c;
    riak_async_result res = {.status = st};
    if (st == RIAK_OK) {
        switch (o->kind) {
        case RIAK_PBA_GET:
            res.status = riak_pb_finish_get(c, &o->opts, &o->resp, &res.object);
            break;
        case RIAK_PBA_PUT:
            res.status = riak_pb_finish_put(c, &o->resp, o->opts.return_body ? &res.object : NULL,
                                            o->generated_key ? &res.created_key : NULL);
            break;
        default:
            break;
        }
    } else if (st == RIAK_E_NOT_FOUND && o->kind == RIAK_PBA_DELETE) {
        riak_clear_error(c); /* deleting an absent key succeeds */
        res.status = RIAK_OK;
    }
    res.errmsg = res.status == RIAK_OK ? "" : c->errmsg;
    o->cb(&res, o->ctx);
    riak_object_free(&res.object);
    free(res.created_key);
    riak_pb_prepared_free(&o->p);
    riak_buf_free(&o->resp.body);
    free(o);
}

static void complete(riak_pb_async *a, pbop *o, riak_status st) {
    unlink_op(a, o);
    finish_op(a, o, st);
}

/* ---- attempts ---------------------------------------------------------- */

static riak_pbconn *take_conn(riak_pb_async *a, size_t node) {
    while (a->idle[node]) {
        riak_pbconn *k = a->idle[node];
        a->idle[node] = k->pool_next;
        k->pool_next = NULL;
        if (riak_pbconn_reusable(k)) return k;
        a->open[node]--;
        riak_pbconn_free(k);
    }
    if (a->open[node] >= a->max_conns) return NULL;
    riak_pbconn *k = riak_pbconn_open(a->c, node);
    if (k) a->open[node]++;
    return k;
}

static void start(riak_pb_async *a, pbop *o) {
    o->retry_at = 0;
    riak_pbconn *k = take_conn(a, o->node);
    if (k == NULL) {
        if (a->open[o->node] >= a->max_conns) {
            enqueue(a, o); /* a connection of this node frees up later */
        } else {
            complete(a, o, riak_set_error(a->c, RIAK_E_NOMEM, "out of memory"));
        }
        return;
    }
    if (!add_active(a, k)) {
        a->open[o->node]--;
        riak_pbconn_free(k);
        complete(a, o, riak_set_error(a->c, RIAK_E_NOMEM, "out of memory"));
        return;
    }
    k->owner = o;
    o->conn = k;
    a->c->nodes[o->node].requests++;
    riak_pbconn_begin(k, &o->p.req, &o->resp, o->attempt);
    /* I/O starts from the loop, never inside a submission call */
}

/* The attempt on k ended: retry, or complete the request; then a request
 * waiting for a connection of that node is started. */
static void attempt_done(riak_pb_async *a, riak_pbconn *k) {
    pbop *o = k->owner;
    size_t node = k->node;
    enum riak_retry retry;
    a->c->http_status = 0;
    riak_status st = riak_pbconn_finish(k, &retry);
    o->conn = NULL;
    release_conn(a, k);
    if (riak_retry_allowed(a->c, o->p.req.idempotent, retry, o->attempt + 1)) {
        riak_buf_free(&o->resp.body);
        o->resp.stopped = false;
        o->retry_at = riak_now_ms() + riak_backoff_ms(a->c, o->attempt);
        if (o->retry_at <= 0) o->retry_at = 1;
        o->attempt++;
        o->node = riak_pick_node(a->c, o->node);
    } else {
        riak_error_add_node(a->c, st);
        complete(a, o, st);
    }
    pbop *next = dequeue(a, node);
    if (next) start(a, next);
}

static int poll_events(int want) {
    unsigned w = (unsigned)want;
    return ((w & (unsigned)RIAK_PBC_WANT_READ) ? RIAK_POLL_IN : 0) +
           ((w & (unsigned)RIAK_PBC_WANT_WRITE) ? RIAK_POLL_OUT : 0);
}

static void drive(riak_pb_async *a, riak_pbconn *k) {
    active_conn *e = find_active(a, k);
    if (e == NULL) return;
    e->kick = false;
    int want = riak_pbconn_step(k);
    if (want != 0) {
        watch(a, e, poll_events(want));
        return;
    }
    attempt_done(a, k);
}

/* Connections that were (re)started are stepped; expired ones time out. */
static void service(riak_pb_async *a) {
    long long now = riak_now_ms();
    for (pbop *o = a->ops; o;) {
        pbop *following = o->next; /* start may complete and free o */
        if (o->retry_at > 0 && o->retry_at <= now) start(a, o);
        o = following;
    }
    for (bool again = true; again;) {
        again = false;
        for (size_t i = 0; i < a->n_active; i++) {
            riak_pbconn *k = a->active[i].k;
            long long d = riak_pbconn_deadline(k);
            if (a->active[i].kick || (d > 0 && d <= now)) {
                if (!a->active[i].kick) riak_pbconn_timeout(k);
                drive(a, k);
                again = true; /* the array may have changed */
                break;
            }
        }
    }
}

/* Earliest retry or connection deadline (0 = none). */
static long long next_deadline(const riak_pb_async *a) {
    long long next = 0;
    for (const pbop *o = a->ops; o; o = o->next) {
        if (o->retry_at > 0 && (next == 0 || o->retry_at < next)) next = o->retry_at;
    }
    for (size_t i = 0; i < a->n_active; i++) {
        long long d = riak_pbconn_deadline(a->active[i].k);
        if (d > 0 && (next == 0 || d < next)) next = d;
    }
    return next;
}

static void rearm_timer(riak_pb_async *a) {
    if (a->timer_fn == NULL) return;
    for (size_t i = 0; i < a->n_active; i++) {
        if (a->active[i].kick) {
            a->timer_fn(0, a->loop_ctx);
            return;
        }
    }
    long long next = next_deadline(a);
    if (next == 0) {
        a->timer_fn(-1, a->loop_ctx);
        return;
    }
    long long ms = next - riak_now_ms();
    a->timer_fn(ms > 0 ? (ms > LONG_MAX ? LONG_MAX : (long)ms) : 0, a->loop_ctx);
}

/* ---- public entry points ----------------------------------------------- */

riak_pb_async *riak_pb_async_new(riak_client *c) {
    riak_pb_async *a = calloc(1, sizeof *a);
    if (a == NULL) return NULL;
    a->c = c;
    a->max_conns = c->cfg.pb_max_connections_per_node ? c->cfg.pb_max_connections_per_node
                                                      : DEFAULT_MAX_CONNECTIONS;
    a->idle = (riak_pbconn **)calloc(c->n_nodes, sizeof *a->idle);
    a->open = calloc(c->n_nodes, sizeof *a->open);
    a->qhead = (pbop **)calloc(c->n_nodes, sizeof *a->qhead);
    a->qtail = (pbop **)calloc(c->n_nodes, sizeof *a->qtail);
    if (a->idle == NULL || a->open == NULL || a->qhead == NULL || a->qtail == NULL) {
        riak_pb_async_free(a);
        return NULL;
    }
    return a;
}

size_t riak_pb_async_pending(const riak_pb_async *a) {
    return a->pending;
}

riak_status riak_pb_async_submit(riak_pb_async *a, enum riak_pb_async_kind kind, const char *bucket,
                                 const char *key, const void *value, size_t len,
                                 const riak_opts *opts, riak_async_cb cb, void *ctx) {
    riak_client *c = a->c;
    pbop *o = calloc(1, sizeof *o);
    if (o == NULL) return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    o->a = a;
    o->kind = kind;
    o->opts = opts ? *opts : no_opts;
    o->cb = cb;
    o->ctx = ctx;
    riak_status st;
    switch (kind) {
    case RIAK_PBA_PING:
        st = riak_pb_prepare_ping(c, &o->p);
        break;
    case RIAK_PBA_GET:
        st = riak_pb_prepare_get(c, bucket, key, &o->opts, &o->p);
        break;
    case RIAK_PBA_PUT:
        o->generated_key = key == NULL;
        st = riak_pb_prepare_put(c, bucket, key, value, len, &o->opts, &o->p);
        break;
    default:
        st = riak_pb_prepare_delete(c, bucket, key, &o->opts, &o->p);
        break;
    }
    if (st != RIAK_OK) {
        free(o);
        return st;
    }
    o->next = a->ops;
    if (a->ops) a->ops->prev = o;
    a->ops = o;
    a->pending++;
    o->node = riak_pick_node(c, SIZE_MAX);
    start(a, o);
    rearm_timer(a);
    return RIAK_OK;
}

size_t riak_pb_async_run(riak_pb_async *a, long timeout_ms) {
    long long deadline = timeout_ms >= 0 ? riak_now_ms() + timeout_ms : 0;
    struct pollfd *fds = NULL;
    riak_pbconn **conns = NULL;
    size_t cap = 0;
    for (;;) {
        service(a);
        if (a->pending == 0) break;
        long long now = riak_now_ms();
        if (deadline && now >= deadline) break;
        long long wait = 1000;
        long long next = next_deadline(a);
        if (next && next - now < wait) wait = next - now;
        if (deadline && deadline - now < wait) wait = deadline - now;
        if (wait < 0) wait = 0;
        if (a->n_active > cap) {
            size_t ncap = a->n_active * 2;
            struct pollfd *f = realloc(fds, ncap * sizeof *f);
            if (f) fds = f;
            riak_pbconn **k = (riak_pbconn **)realloc((void *)conns, ncap * sizeof *k);
            if (k) conns = k;
            if (f == NULL || k == NULL) {
                riak_sleep_ms(wait); /* no memory for the poll set: try again later */
                continue;
            }
            cap = ncap;
        }
        size_t n = 0;
        for (size_t i = 0; i < a->n_active; i++) {
            unsigned ev = (unsigned)a->active[i].watched;
            if (a->active[i].k->fd < 0 || ev == 0) continue;
            fds[n] =
                (struct pollfd){.fd = a->active[i].k->fd,
                                .events = (short)(((ev & (unsigned)RIAK_POLL_IN) ? POLLIN : 0) +
                                                  ((ev & (unsigned)RIAK_POLL_OUT) ? POLLOUT : 0))};
            conns[n++] = a->active[i].k;
        }
        int r = poll(fds, n, (int)wait);
        for (size_t i = 0; r > 0 && i < n; i++) {
            if (fds[i].revents) drive(a, conns[i]); /* a stale entry is ignored by drive */
        }
    }
    free(fds);
    free((void *)conns);
    return a->pending;
}

void riak_pb_async_set_loop(riak_pb_async *a, riak_socket_fn socket_fn, riak_timer_fn timer_fn,
                            void *ctx) {
    a->socket_fn = socket_fn;
    a->timer_fn = timer_fn;
    a->loop_ctx = ctx;
}

void riak_pb_async_on_socket(riak_pb_async *a, int fd, int events) {
    (void)events; /* the connection state says what to do next */
    for (size_t i = 0; i < a->n_active; i++) {
        if (a->active[i].k->fd == fd) {
            drive(a, a->active[i].k);
            break;
        }
    }
    service(a);
    rearm_timer(a);
}

void riak_pb_async_on_timeout(riak_pb_async *a) {
    service(a);
    rearm_timer(a);
}

void riak_pb_async_free(riak_pb_async *a) {
    if (a == NULL) return;
    riak_client *c = a->c;
    while (a->ops) {
        pbop *o = a->ops;
        if (o->conn) {
            riak_pbconn *k = o->conn;
            remove_active(a, k);
            a->open[k->node]--;
            riak_pbconn_free(k);
            o->conn = NULL;
        }
        if (o->queued) unqueue(a, o);
        unlink_op(a, o);
        riak_set_error(c, RIAK_E_CANCELLED, "cancelled");
        riak_buf_free(&o->resp.body);
        finish_op(a, o, RIAK_E_CANCELLED);
    }
    for (size_t i = 0; a->idle && i < c->n_nodes; i++) {
        while (a->idle[i]) {
            riak_pbconn *k = a->idle[i];
            a->idle[i] = k->pool_next;
            riak_pbconn_free(k);
        }
    }
    free((void *)a->idle);
    free(a->open);
    free((void *)a->qhead);
    free((void *)a->qtail);
    free(a->active);
    free(a);
}
