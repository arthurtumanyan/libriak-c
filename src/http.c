/*
 File:   http.c
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later

 The single place where libcurl requests are issued. The easy handle is
 reset before every request so options such as CURLOPT_CUSTOMREQUEST or
 CURLOPT_HTTPHEADER never leak from one call into the next, while the
 connection cache is kept.
 */

#include "internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RIAK_USER_AGENT "libriak-c/2.0"

typedef struct {
    riak_client *c;
    const riak_request *req;
    riak_response *resp;
    size_t received;
    riak_status abort_status;
    const char *abort_msg;
    bool aborted;
} transfer;

static size_t abort_transfer(transfer *t, riak_status st, const char *msg) {
    t->abort_status = st;
    t->abort_msg = msg;
    t->aborted = true;
    return 0;
}

static size_t write_cb(char *data, size_t size, size_t nmemb, void *userp) {
    transfer *t = userp;
    size_t len = size * nmemb;
    t->received += len;
    if (!t->req->unbounded && t->received > t->c->cfg.max_body_bytes) {
        return abort_transfer(t, RIAK_E_HTTP, "response exceeds max_body_bytes");
    }
    if (t->req->sink != NULL) {
        long code = 0;
        curl_easy_getinfo(t->c->curl, CURLINFO_RESPONSE_CODE, &code);
        /* error bodies are collected as text for the error message */
        if (code == 200) {
            riak_status st = RIAK_E_PARSE;
            if (!t->req->sink(t->req->sink_ctx, data, len, &st)) {
                return abort_transfer(t, st,
                                      st == RIAK_E_NOMEM
                                          ? "out of memory while reading the response"
                                          : "malformed response body");
            }
            return len;
        }
    }
    if (riak_buf_append(&t->resp->body, data, len) != 0) {
        return abort_transfer(t, RIAK_E_NOMEM, "out of memory while reading the response");
    }
    return len;
}

static size_t header_cb(char *data, size_t size, size_t nitems, void *userp) {
    transfer *t = userp;
    size_t len = size * nitems;

    if (len >= 5 && strncmp(data, "HTTP/", 5) == 0) {
        /* a new response (e.g. after 100 Continue): forget earlier headers */
        riak_headerlist_clear(&t->resp->headers);
        return len;
    }
    if (riak_headerlist_add_line(&t->resp->headers, data, len) < 0) {
        return abort_transfer(t, RIAK_E_NOMEM, "out of memory while reading headers");
    }
    return len;
}

static riak_status map_curl_error(riak_client *c, CURLcode rc) {
    const char *detail = c->curl_err[0] ? c->curl_err : curl_easy_strerror(rc);
    switch (rc) {
    case CURLE_OUT_OF_MEMORY:
        return riak_set_error(c, RIAK_E_NOMEM, "%s", detail);
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CIPHER:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_ISSUER_ERROR:
        return riak_set_error(c, RIAK_E_TLS, "%s", detail);
    default:
        return riak_set_error(c, RIAK_E_TRANSPORT, "%s", detail);
    }
}

/* What a failed attempt allows. */
enum retry { RETRY_NONE, RETRY_ANY, RETRY_IDEMPOTENT };

/* One attempt against one node. */
static riak_status perform_once(riak_client *c, riak_node_state *node, const riak_request *req,
                                riak_response *resp, enum retry *retry) {
    CURL *h = c->curl;
    transfer t = {.c = c, .req = req, .resp = resp};
    struct curl_slist *headers = req->headers;
    struct curl_slist *extra = NULL;

    *retry = RETRY_NONE;
    memset(resp, 0, sizeof *resp);
    c->http_status = 0;
    c->curl_err[0] = '\0';
    riak_clear_error(c);

    char *url = riak_strprintf("%s%s", node->base, req->url);
    if (url == NULL) {
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    curl_easy_reset(h);
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, c->curl_err);
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_USERAGENT, RIAK_USER_AGENT);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(h, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, c->cfg.tls_verify ? 1L : 0L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, c->cfg.tls_verify ? 2L : 0L);
    if (c->ca_file) {
        curl_easy_setopt(h, CURLOPT_CAINFO, c->ca_file);
    }
    if (c->cfg.connect_timeout_ms > 0) {
        curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, c->cfg.connect_timeout_ms);
    }
    if (c->cfg.timeout_ms > 0) {
        curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, c->cfg.timeout_ms);
    }
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &t);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &t);

    if (strcmp(req->method, "GET") != 0) {
        if (strcmp(req->method, "POST") == 0) {
            curl_easy_setopt(h, CURLOPT_POST, 1L);
        } else {
            curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, req->method);
        }
        if (req->body != NULL || strcmp(req->method, "DELETE") != 0) {
            curl_easy_setopt(h, CURLOPT_POSTFIELDS, req->body ? req->body : "");
            curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)req->body_len);
        }
    }

    /* the caller's list is borrowed, so extra headers go onto a copy */
    for (struct curl_slist *it = headers; it; it = it->next) {
        struct curl_slist *n = curl_slist_append(extra, it->data);
        if (n == NULL) goto nomem;
        extra = n;
    }
    if (c->client_id) {
        char *hdr = riak_strprintf("X-Riak-ClientId: %s", c->client_id);
        struct curl_slist *n = hdr ? curl_slist_append(extra, hdr) : NULL;
        free(hdr);
        if (n == NULL) goto nomem;
        extra = n;
    }
    {
        /* disable "Expect: 100-continue" round trips */
        struct curl_slist *n = curl_slist_append(extra, "Expect:");
        if (n == NULL) goto nomem;
        extra = n;
    }
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, extra);

    node->requests++;
    CURLcode rc = curl_easy_perform(h);
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &resp->status);
    c->http_status = resp->status;
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, NULL);
    curl_easy_setopt(h, CURLOPT_URL, NULL);
    curl_slist_free_all(extra);
    free(url);

    if (t.aborted) {
        riak_response_free(resp);
        return riak_set_error(c, t.abort_status, "%s", t.abort_msg);
    }
    if (rc != CURLE_OK) {
        riak_response_free(resp);
        bool not_sent = rc == CURLE_COULDNT_CONNECT || rc == CURLE_COULDNT_RESOLVE_HOST ||
                        rc == CURLE_COULDNT_RESOLVE_PROXY;
        riak_status st = map_curl_error(c, rc);
        if (not_sent) {
            *retry = RETRY_ANY;
        } else if (st == RIAK_E_TRANSPORT && (req->sink == NULL || t.received == 0)) {
            *retry = RETRY_IDEMPOTENT; /* nothing was handed to a streaming callback yet */
        }
        return st;
    }
    if (resp->status == 503) {
        *retry = RETRY_IDEMPOTENT; /* node overloaded or still starting */
    }
    return RIAK_OK;

nomem:
    curl_slist_free_all(extra);
    free(url);
    return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
}

long long riak_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool node_usable(const riak_node_state *n, long long now) {
    return n->ejected_until_ms == 0 || now >= n->ejected_until_ms;
}

/* Next node to try after `after` (SIZE_MAX: first attempt). */
static size_t pick_node(riak_client *c, size_t after) {
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

static void node_result(riak_client *c, riak_node_state *n, bool ok) {
    if (ok) {
        n->consecutive_failures = 0;
        n->ejected_until_ms = 0;
        return;
    }
    n->failures++;
    if (++n->consecutive_failures >= c->cfg.fail_threshold) {
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
static void backoff(riak_client *c, unsigned attempt) {
    long long delay = c->cfg.retry_base_ms;
    for (unsigned i = 0; i < attempt && delay < c->cfg.retry_max_ms; i++) {
        delay *= 2;
    }
    if (delay > c->cfg.retry_max_ms) {
        delay = c->cfg.retry_max_ms;
    }
    long long half = delay / 2;
    long long ms =
        half + (half > 0 ? (long long)(next_random(c) % (unsigned long long)(half + 1)) : 0);
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0) {
    }
}

static bool idempotent(const char *method) {
    return strcmp(method, "GET") == 0 || strcmp(method, "PUT") == 0 ||
           strcmp(method, "DELETE") == 0;
}

riak_status riak_perform(riak_client *c, const riak_request *req, riak_response *resp) {
    if (req->pin > 0) {
        c->last_node = (req->pin - 1) % c->n_nodes;
        riak_node_state *n = &c->nodes[c->last_node];
        enum retry retry;
        riak_status st = perform_once(c, n, req, resp, &retry);
        node_result(c, n, st == RIAK_OK && resp->status != 503);
        return st;
    }
    unsigned attempts = c->cfg.max_retries + 1;
    size_t k = pick_node(c, SIZE_MAX);
    for (unsigned attempt = 0;; attempt++) {
        riak_node_state *n = &c->nodes[k];
        c->last_node = k;
        enum retry retry;
        riak_status st = perform_once(c, n, req, resp, &retry);
        bool failed = retry != RETRY_NONE;
        node_result(c, n, !failed);
        bool again = (retry == RETRY_ANY || (retry == RETRY_IDEMPOTENT && idempotent(req->method)));
        if (!again || attempt + 1 >= attempts) {
            if (st != RIAK_OK && c->n_nodes > 1) {
                char detail[sizeof c->errmsg];
                snprintf(detail, sizeof detail, "%s", c->errmsg);
                riak_set_error(c, st, "%s:%u: %s", n->host, n->port, detail);
            }
            return st;
        }
        riak_response_free(resp);
        backoff(c, attempt);
        k = pick_node(c, k);
    }
}

void riak_response_free(riak_response *resp) {
    riak_buf_free(&resp->body);
    riak_headerlist_clear(&resp->headers);
    long status = resp->status;
    memset(resp, 0, sizeof *resp);
    resp->status = status;
}

riak_status riak_http_error(riak_client *c, const riak_response *resp) {
    riak_status st;
    switch (resp->status) {
    case 300:
        st = RIAK_E_SIBLINGS;
        break;
    case 304:
        st = RIAK_E_NOT_MODIFIED;
        break;
    case 404:
        st = RIAK_E_NOT_FOUND;
        break;
    case 412:
        st = RIAK_E_PRECONDITION;
        break;
    default:
        st = RIAK_E_HTTP;
        break;
    }
    int n = resp->body.len > 200 ? 200 : (int)resp->body.len;
    if (c->n_nodes > 1) {
        const riak_node_state *node = &c->nodes[c->last_node];
        return riak_set_error(c, st, "%s:%u: HTTP %ld%s%.*s", node->host, node->port, resp->status,
                              n ? ": " : "", n, n ? resp->body.data : "");
    }
    return riak_set_error(c, st, "HTTP %ld%s%.*s", resp->status, n ? ": " : "", n,
                          n ? resp->body.data : "");
}

riak_status riak_add_header(riak_client *c, struct curl_slist **list, const char *prefix,
                            const char *name, const char *value) {
    if ((name && !riak_header_name_ok(name)) || !riak_header_value_ok(value)) {
        return riak_set_error(c, RIAK_E_INVALID_ARG, "invalid header %s%s", prefix,
                              name ? name : "");
    }
    char *line = riak_strprintf("%s%s: %s", prefix, name ? name : "", value);
    struct curl_slist *n = line ? curl_slist_append(*list, line) : NULL;
    free(line);
    if (n == NULL) {
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    *list = n;
    return RIAK_OK;
}

int riak_url_bucket(riak_buf *b, const riak_client *c, const char *type, const char *bucket,
                    const char *suffix, const char *key) {
    if (riak_buf_append_str(b, c->root) != 0) {
        return -1;
    }
    if (type && (riak_buf_append_str(b, "types/") != 0 || riak_url_append_escaped(b, type) != 0 ||
                 riak_buf_append_str(b, "/") != 0)) {
        return -1;
    }
    if (riak_buf_append_str(b, "buckets/") != 0 || riak_url_append_escaped(b, bucket) != 0) {
        return -1;
    }
    if (suffix && (riak_buf_append_str(b, "/") != 0 || riak_buf_append_str(b, suffix) != 0)) {
        return -1;
    }
    if (key && (riak_buf_append_str(b, "/") != 0 || riak_url_append_escaped(b, key) != 0)) {
        return -1;
    }
    return 0;
}
