/*
 File:   cluster.c
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later

 Node selection, retry policy, backoff, health tracking and attempt
 reporting. Nothing here depends on the wire protocol, so every transport
 shares the same cluster behavior.
 */

#include "internal.h"

#include <stdint.h>
#include <stdio.h>
#include <time.h>

long long riak_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void riak_sleep_ms(long long ms) {
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0) {
    }
}

static bool node_usable(const riak_node_state *n, long long now) {
    return n->ejected_until_ms == 0 || now >= n->ejected_until_ms;
}

size_t riak_pick_node(riak_client *c, size_t after) {
    size_t n = c->n_nodes;
    long long now = riak_now_ms();
    size_t start;
    if (after != SIZE_MAX) {
        start = (after + 1) % n;
    } else if (c->cfg.balance == RIAK_BALANCE_ROUND_ROBIN) {
        start = c->next_node++ % n;
    } else {
        start = 0;
    }
    for (size_t i = 0; i < n; i++) {
        size_t k = (start + i) % n;
        if (node_usable(&c->nodes[k], now)) {
            return k;
        }
    }
    /* every node is ejected: try the one that comes back first */
    size_t best = start;
    for (size_t k = 0; k < n; k++) {
        if (c->nodes[k].ejected_until_ms < c->nodes[best].ejected_until_ms) {
            best = k;
        }
    }
    return best;
}

void riak_node_result(riak_client *c, size_t node, bool ok) {
    riak_node_state *n = &c->nodes[node];
    if (ok) {
        n->consecutive_failures = 0;
        n->ejected_until_ms = 0;
        return;
    }
    n->failures++;
    if (++n->consecutive_failures >= c->cfg.fail_threshold) {
        if (n->ejected_until_ms == 0 || riak_now_ms() >= n->ejected_until_ms) {
            riak_log(c, RIAK_LOG_WARN, "node %s:%u ejected for %ld ms after %u failures", n->host,
                     n->port, c->cfg.eject_ms, n->consecutive_failures);
        }
        n->ejected_until_ms = riak_now_ms() + c->cfg.eject_ms;
    }
}

static unsigned long long next_random(riak_client *c) {
    /* xorshift64* */
    c->rng ^= c->rng >> 12U;
    c->rng ^= c->rng << 25U;
    c->rng ^= c->rng >> 27U;
    return c->rng * 2685821657736338717ULL;
}

/* "Equal jitter": half the exponential delay plus a random half. */
long long riak_backoff_ms(riak_client *c, unsigned attempt) {
    long long delay = c->cfg.retry_base_ms;
    for (unsigned i = 0; i < attempt && delay < c->cfg.retry_max_ms; i++) {
        delay *= 2;
    }
    if (delay > c->cfg.retry_max_ms) {
        delay = c->cfg.retry_max_ms;
    }
    long long half = delay / 2;
    return half + (half > 0 ? (long long)(next_random(c) % (unsigned long long)(half + 1)) : 0);
}

bool riak_retry_allowed(const riak_client *c, bool idempotent, enum riak_retry retry,
                        unsigned attempts_done) {
    if (attempts_done > c->cfg.max_retries) {
        return false;
    }
    return retry == RIAK_RETRY_ANY || (retry == RIAK_RETRY_IDEMPOTENT && idempotent);
}

void riak_error_add_node(riak_client *c, riak_status st) {
    if (st != RIAK_OK && c->n_nodes > 1) {
        const riak_node_state *n = &c->nodes[c->last_node];
        char detail[sizeof c->errmsg];
        snprintf(detail, sizeof detail, "%s", c->errmsg);
        riak_set_error(c, st, "%s:%u: %s", n->host, n->port, detail);
    }
}

void riak_attempt_report(riak_client *c, const riak_attempt_info *info) {
    const riak_node_state *node = &c->nodes[info->node];
    if (c->cfg.metrics_fn) {
        riak_request_metrics m = {.method = info->method,
                                  .path = info->path,
                                  .host = node->host,
                                  .port = node->port,
                                  .attempt = info->attempt,
                                  .status = info->status,
                                  .http_status = info->http_status,
                                  .duration_ms = (double)(riak_now_ms() - info->started_ms),
                                  .bytes_received = info->received,
                                  .will_retry = info->will_retry};
        c->cfg.metrics_fn(&m, c->cfg.metrics_ctx);
    }
    if (info->retry != RIAK_RETRY_NONE) {
        riak_log(c, info->will_retry ? RIAK_LOG_WARN : RIAK_LOG_ERROR,
                 "%s %s on %s:%u failed (%s)%s", info->method, info->path, node->host, node->port,
                 info->status != RIAK_OK ? c->errmsg : info->failure,
                 info->will_retry ? ", retrying" : "");
    }
}
