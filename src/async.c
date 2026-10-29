/*
 File:   async.c
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later

 Concurrent requests on a curl multi handle. Each request goes through the
 same prepare / attempt / finish steps as the blocking API, including node
 selection, retries with backoff (scheduled as timers, never sleeping) and
 health tracking.
 */

#include "async_op.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct op op;
struct op {
    riak_attempt at; /* CURLOPT_PRIVATE points here */
    riak_async *a;
    const riak_aop_def *def;
    void *state;
    riak_prepared p;
    riak_async_cb cb;
    void *ctx;
    riak_response resp;
    CURL *h; /* while an attempt is in flight */
    size_t node;
    unsigned attempt;
    long long retry_at; /* > 0: waiting for a retry */
    op *prev, *next;
};

struct riak_async {
    riak_client *c;
    riak_pb_async *pb; /* Protocol Buffers: everything below is unused */
    CURLM *multi;
    op *ops; /* every pending request */
    size_t pending;
    CURL **pool; /* idle easy handles, reused for their connection caches */
    size_t n_pool, pool_cap;
    riak_socket_fn socket_fn;
    riak_timer_fn timer_fn;
    void *loop_ctx;
    long long curl_deadline; /* ms, 0 = none */
};

static CURL *take_handle(riak_async *a) {
    return a->n_pool ? a->pool[--a->n_pool] : curl_easy_init();
}

static void give_handle(riak_async *a, CURL *h) {
    if (a->n_pool == a->pool_cap) {
        size_t ncap = a->pool_cap ? a->pool_cap * 2 : 8;
        CURL **grown = (CURL **)realloc((void *)a->pool, ncap * sizeof *grown);
        if (grown == NULL) {
            curl_easy_cleanup(h);
            return;
        }
        a->pool = grown;
        a->pool_cap = ncap;
    }
    a->pool[a->n_pool++] = h;
}

static void unlink_op(riak_async *a, op *o) {
    if (o->prev)
        o->prev->next = o->next;
    else
        a->ops = o->next;
    if (o->next) o->next->prev = o->prev;
    a->pending--;
}

static void op_free(op *o) {
    riak_prepared_free(&o->p);
    riak_response_free(&o->resp);
    if (o->def->free_state) o->def->free_state(o->state);
    free(o);
}

/* The final response is interpreted, the callback is run and the request,
 * already removed from the pending list, is freed. */
static void finish_op(riak_async *a, op *o, riak_status st) {
    riak_client *c = a->c;
    riak_async_result res = {.status = st, .http_status = o->resp.status, .kind = o->def->kind};
    if (st == RIAK_OK && o->def->finish_http) {
        res.status = o->def->finish_http(c, o->state, &o->resp, &res);
    } else if (st != RIAK_OK && o->def->on_error) {
        res.status = o->def->on_error(c, o->state, st);
    }
    res.errmsg = res.status == RIAK_OK ? "" : c->errmsg;
    o->cb(&res, o->ctx);
    riak_async_result_clear(&res);
    op_free(o);
}
static void complete(riak_async *a, op *o, riak_status st) {
    unlink_op(a, o);
    finish_op(a, o, st);
}

static void start_attempt(riak_async *a, op *o) {
    o->retry_at = 0;
    CURL *h = take_handle(a);
    riak_status st =
        h ? riak_attempt_setup(&o->at, a->c, h, o->node, o->attempt, &o->p.req, &o->resp)
          : riak_set_error(a->c, RIAK_E_NOMEM, "out of memory");
    if (st == RIAK_OK && curl_multi_add_handle(a->multi, h) != CURLM_OK) {
        enum riak_retry ignored;
        st = riak_attempt_finish(&o->at, CURLE_OUT_OF_MEMORY, &ignored);
        if (st == RIAK_OK) st = riak_set_error(a->c, RIAK_E_NOMEM, "cannot add transfer");
    }
    if (st != RIAK_OK) {
        if (h) give_handle(a, h);
        complete(a, o, st);
        return;
    }
    o->h = h;
}

/* Every transfer that curl reports as done is handled. */
static void drain(riak_async *a) {
    CURLMsg *msg;
    int left;
    while ((msg = curl_multi_info_read(a->multi, &left)) != NULL) {
        if (msg->msg != CURLMSG_DONE) {
            continue;
        }
        CURL *h = msg->easy_handle;
        CURLcode rc = msg->data.result;
        char *priv = NULL;
        curl_easy_getinfo(h, CURLINFO_PRIVATE, &priv);
        op *o = (op *)(void *)(priv - offsetof(op, at));
        curl_multi_remove_handle(a->multi, h);
        o->h = NULL;
        enum riak_retry retry;
        a->c->http_status = 0;
        riak_status st = riak_attempt_finish(&o->at, rc, &retry);
        give_handle(a, h);
        /* a pinned request goes to its node only, as in the blocking path */
        if (o->p.req.pin == 0 &&
            riak_retry_allowed(a->c, riak_request_idempotent(&o->p.req), retry, o->attempt + 1)) {
            riak_response_free(&o->resp);
            o->retry_at = riak_now_ms() + riak_backoff_ms(a->c, o->attempt);
            if (o->retry_at <= 0) o->retry_at = 1;
            o->attempt++;
            o->node = riak_pick_node(a->c, o->node);
            continue;
        }
        riak_error_add_node(a->c, st);
        complete(a, o, st);
    }
}

/* Retries whose backoff has passed are started; the result is the earliest one still
 * waiting (0 = none). */
static long long start_due_retries(riak_async *a) {
    long long now = riak_now_ms();
    long long next = 0;
    for (op *o = a->ops; o;) {
        op *following = o->next; /* start_attempt may complete and free o */
        if (o->retry_at > 0) {
            if (o->retry_at <= now) {
                start_attempt(a, o);
            } else if (next == 0 || o->retry_at < next) {
                next = o->retry_at;
            }
        }
        o = following;
    }
    return next;
}

riak_status riak_async_submit_http(riak_async *a, const riak_aop_def *def, void *state,
                                   riak_prepared *p, riak_async_cb cb, void *ctx) {
    op *o = calloc(1, sizeof *o);
    if (o == NULL) {
        riak_prepared_free(p);
        if (def->free_state) def->free_state(state);
        return riak_set_error(a->c, RIAK_E_NOMEM, "out of memory");
    }
    o->a = a;
    o->def = def;
    o->state = state;
    o->p = *p; /* the heap pointers inside req stay valid */
    memset(p, 0, sizeof *p);
    o->cb = cb;
    o->ctx = ctx;
    if (def->bind_http) def->bind_http(state, &o->resp);
    o->next = a->ops;
    if (a->ops) a->ops->prev = o;
    a->ops = o;
    a->pending++;
    o->node =
        o->p.req.pin > 0 ? (o->p.req.pin - 1) % a->c->n_nodes : riak_pick_node(a->c, SIZE_MAX);
    /* adding the transfer makes curl ask for a timer; the loop takes over */
    start_attempt(a, o);
    return RIAK_OK;
}

riak_pb_async *riak_async_pb_engine(riak_async *a) {
    return a->pb;
}

size_t riak_async_run(riak_async *a, long timeout_ms) {
    if (a == NULL) return 0;
    if (a->pb) return riak_pb_async_run(a->pb, timeout_ms);
    long long deadline = timeout_ms >= 0 ? riak_now_ms() + timeout_ms : 0;
    for (;;) {
        long long next_retry = start_due_retries(a);
        int running = 0;
        curl_multi_perform(a->multi, &running);
        drain(a);
        if (a->pending == 0) {
            return 0;
        }
        long long now = riak_now_ms();
        if (deadline && now >= deadline) {
            return a->pending;
        }
        long long wait = 1000;
        if (next_retry && next_retry - now < wait) wait = next_retry - now;
        if (deadline && deadline - now < wait) wait = deadline - now;
        if (wait < 0) wait = 0;
        curl_multi_poll(a->multi, NULL, 0, (int)wait, NULL);
    }
}

/* ---- external event loop ------------------------------------------- */

static void rearm_timer(riak_async *a) {
    if (a->timer_fn == NULL) return;
    long long next = 0;
    for (op *o = a->ops; o; o = o->next) {
        if (o->retry_at > 0 && (next == 0 || o->retry_at < next)) next = o->retry_at;
    }
    if (a->curl_deadline && (next == 0 || a->curl_deadline < next)) next = a->curl_deadline;
    if (next == 0) {
        a->timer_fn(-1, a->loop_ctx);
        return;
    }
    long long ms = next - riak_now_ms();
    a->timer_fn(ms > 0 ? (long)ms : 0, a->loop_ctx);
}

static int curl_socket_cb(CURL *e, curl_socket_t s, int what, void *userp, void *socketp) {
    (void)e;
    (void)socketp;
    riak_async *a = userp;
    int ev = what == CURL_POLL_REMOVE ? RIAK_POLL_REMOVE
             : what == CURL_POLL_IN   ? RIAK_POLL_IN
             : what == CURL_POLL_OUT  ? RIAK_POLL_OUT
                                      : (int)((unsigned)RIAK_POLL_IN | (unsigned)RIAK_POLL_OUT);
    a->socket_fn((int)s, ev, a->loop_ctx);
    return 0;
}

static int curl_timer_cb(CURLM *m, long timeout_ms, void *userp) {
    (void)m;
    riak_async *a = userp;
    a->curl_deadline = timeout_ms < 0 ? 0 : riak_now_ms() + timeout_ms;
    if (a->curl_deadline == 0 && timeout_ms >= 0) a->curl_deadline = 1;
    rearm_timer(a);
    return 0;
}

riak_status riak_async_set_loop(riak_async *a, riak_socket_fn socket_fn, riak_timer_fn timer_fn,
                                void *ctx) {
    if (a == NULL || socket_fn == NULL || timer_fn == NULL || riak_async_pending(a) > 0) {
        return RIAK_E_INVALID_ARG;
    }
    if (a->pb) {
        a->socket_fn = socket_fn;
        riak_pb_async_set_loop(a->pb, socket_fn, timer_fn, ctx);
        return RIAK_OK;
    }
    a->socket_fn = socket_fn;
    a->timer_fn = timer_fn;
    a->loop_ctx = ctx;
    curl_multi_setopt(a->multi, CURLMOPT_SOCKETFUNCTION, curl_socket_cb);
    curl_multi_setopt(a->multi, CURLMOPT_SOCKETDATA, a);
    curl_multi_setopt(a->multi, CURLMOPT_TIMERFUNCTION, curl_timer_cb);
    curl_multi_setopt(a->multi, CURLMOPT_TIMERDATA, a);
    return RIAK_OK;
}

void riak_async_on_socket(riak_async *a, int fd, int events) {
    if (a == NULL || a->socket_fn == NULL) return;
    if (a->pb) {
        riak_pb_async_on_socket(a->pb, fd, events);
        return;
    }
    unsigned ev = (unsigned)events;
    int mask = (int)(((ev & (unsigned)RIAK_POLL_IN) ? (unsigned)CURL_CSELECT_IN : 0U) |
                     ((ev & (unsigned)RIAK_POLL_OUT) ? (unsigned)CURL_CSELECT_OUT : 0U));
    int running;
    curl_multi_socket_action(a->multi, (curl_socket_t)fd, mask, &running);
    drain(a);
    rearm_timer(a);
}

void riak_async_on_timeout(riak_async *a) {
    if (a == NULL || a->socket_fn == NULL) return;
    if (a->pb) {
        riak_pb_async_on_timeout(a->pb);
        return;
    }
    int running;
    if (a->curl_deadline && riak_now_ms() >= a->curl_deadline) {
        a->curl_deadline = 0;
    }
    curl_multi_socket_action(a->multi, CURL_SOCKET_TIMEOUT, 0, &running);
    drain(a);
    start_due_retries(a);
    curl_multi_socket_action(a->multi, CURL_SOCKET_TIMEOUT, 0, &running);
    drain(a);
    rearm_timer(a);
}

/* ---- lifecycle ----------------------------------------------------- */

riak_status riak_async_new(const riak_config *cfg, riak_async **out) {
    if (out == NULL) return RIAK_E_INVALID_ARG;
    *out = NULL;
    riak_async *a = calloc(1, sizeof *a);
    if (a == NULL) return RIAK_E_NOMEM;
    riak_status st = riak_client_new(cfg, &a->c);
    if (st != RIAK_OK) {
        free(a);
        return st;
    }
    if (riak_is_pb(a->c)) {
        a->pb = riak_pb_async_new(a->c);
        if (a->pb == NULL) {
            riak_client_free(a->c);
            free(a);
            return RIAK_E_NOMEM;
        }
        *out = a;
        return RIAK_OK;
    }
    a->multi = curl_multi_init();
    if (a->multi == NULL) {
        riak_client_free(a->c);
        free(a);
        return RIAK_E_NOMEM;
    }
    *out = a;
    return RIAK_OK;
}

riak_client *riak_async_client(riak_async *a) {
    return a ? a->c : NULL;
}

size_t riak_async_pending(const riak_async *a) {
    return a ? (a->pb ? riak_pb_async_pending(a->pb) : a->pending) : 0;
}

void riak_async_free(riak_async *a) {
    if (a == NULL) return;
    if (a->pb) {
        riak_pb_async_free(a->pb);
        riak_client_free(a->c);
        free(a);
        return;
    }
    while (a->ops) {
        op *o = a->ops;
        a->ops = o->next;
        if (a->ops) a->ops->prev = NULL;
        a->pending--;
        if (o->h) {
            curl_multi_remove_handle(a->multi, o->h);
            curl_slist_free_all(o->at.extra);
            free(o->at.url);
            o->at.extra = NULL;
            o->at.url = NULL;
            give_handle(a, o->h);
            o->h = NULL;
        }
        riak_set_error(a->c, RIAK_E_CANCELLED, "cancelled");
        riak_response_free(&o->resp);
        o->resp.status = 0;
        finish_op(a, o, RIAK_E_CANCELLED);
    }
    for (size_t i = 0; i < a->n_pool; i++) {
        curl_easy_cleanup(a->pool[i]);
    }
    free((void *)a->pool);
    curl_multi_cleanup(a->multi);
    riak_client_free(a->c);
    free(a);
}
