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

static size_t abort_transfer(riak_attempt *a, riak_status st, const char *msg) {
    a->abort_status = st;
    a->abort_msg = msg;
    a->aborted = true;
    return 0;
}

static size_t write_cb(char *data, size_t size, size_t nmemb, void *userp) {
    riak_attempt *a = userp;
    size_t len = size * nmemb;
    a->received += len;
    if (!a->req->unbounded && a->received > a->c->cfg.max_body_bytes) {
        return abort_transfer(a, RIAK_E_HTTP, "response exceeds max_body_bytes");
    }
    if (a->req->sink != NULL) {
        long code = 0;
        curl_easy_getinfo(a->h, CURLINFO_RESPONSE_CODE, &code);
        /* error bodies are collected as text for the error message */
        if (code == 200) {
            riak_status st = RIAK_E_PARSE;
            if (!a->req->sink(a->req->sink_ctx, data, len, &st)) {
                return abort_transfer(a, st,
                                      st == RIAK_E_NOMEM
                                          ? "out of memory while reading the response"
                                          : "malformed response body");
            }
            return len;
        }
    }
    if (riak_buf_append(&a->resp->body, data, len) != 0) {
        return abort_transfer(a, RIAK_E_NOMEM, "out of memory while reading the response");
    }
    return len;
}

static size_t header_cb(char *data, size_t size, size_t nitems, void *userp) {
    riak_attempt *a = userp;
    size_t len = size * nitems;

    if (len >= 5 && strncmp(data, "HTTP/", 5) == 0) {
        /* a new response (e.g. after 100 Continue): forget earlier headers */
        riak_headerlist_clear(&a->resp->headers);
        return len;
    }
    if (riak_headerlist_add_line(&a->resp->headers, data, len) < 0) {
        return abort_transfer(a, RIAK_E_NOMEM, "out of memory while reading headers");
    }
    return len;
}

/* With TLS 1.3 a server rejects a client without the certificate it asked
 * for only after the handshake, so curl reports a plain send/receive error
 * (sometimes with an alert, sometimes as a reset). Watch the handshake for
 * the certificate request and for incoming alerts so the failure reads as
 * TLS instead of a retryable network error. */
static bool contains(const char *data, size_t size, const char *needle) {
    size_t n = strlen(needle);
    for (size_t i = 0; i + n <= size; i++) {
        if (memcmp(data + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

static int debug_cb(CURL *h, curl_infotype type, char *data, size_t size, void *userp) {
    (void)h;
    riak_attempt *a = userp;
    if (type != CURLINFO_TEXT) {
        return 0;
    }
    if (contains(data, size, "TLS handshake, Request CERT")) {
        a->cert_requested = true;
    } else if (a->tls_alert[0] == '\0' && contains(data, size, "(IN), TLS alert")) {
        /* e.g. "TLSv1.3 (IN), TLS alert, unknown CA (560):" */
        size_t n = size;
        while (n > 0 && (data[n - 1] == '\n' || data[n - 1] == '\r' || data[n - 1] == ':')) {
            n--;
        }
        if (n >= sizeof a->tls_alert) {
            n = sizeof a->tls_alert - 1;
        }
        memcpy(a->tls_alert, data, n);
        a->tls_alert[n] = '\0';
    }
    return 0;
}

static riak_status map_curl_error(riak_attempt *a, CURLcode rc) {
    const char *detail = a->curl_err[0] ? a->curl_err : curl_easy_strerror(rc);
    if ((rc == CURLE_RECV_ERROR || rc == CURLE_SEND_ERROR || rc == CURLE_GOT_NOTHING) &&
        (a->tls_alert[0] != '\0' || (a->cert_requested && a->c->cert_file == NULL))) {
        if (a->cert_requested && a->c->cert_file == NULL) {
            /* alert 116 (certificate_required) is often printed as "unknown" */
            return riak_set_error(a->c, RIAK_E_TLS, "server requires a client certificate%s%s%s",
                                  a->tls_alert[0] ? " (" : "", a->tls_alert,
                                  a->tls_alert[0] ? ")" : "");
        }
        return riak_set_error(a->c, RIAK_E_TLS, "%s", a->tls_alert);
    }
    switch (rc) {
    case CURLE_OUT_OF_MEMORY:
        return riak_set_error(a->c, RIAK_E_NOMEM, "%s", detail);
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CIPHER:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_ISSUER_ERROR:
    case CURLE_SSL_CRL_BADFILE:
#if LIBCURL_VERSION_NUM >= 0x074400
    case CURLE_SSL_CLIENTCERT:
#endif
        return riak_set_error(a->c, RIAK_E_TLS, "%s", detail);
    default:
        return riak_set_error(a->c, RIAK_E_TRANSPORT, "%s", detail);
    }
}

static riak_status append_header(struct curl_slist **list, const char *line) {
    struct curl_slist *n = curl_slist_append(*list, line);
    if (n == NULL) {
        return RIAK_E_NOMEM;
    }
    *list = n;
    return RIAK_OK;
}

riak_status riak_attempt_setup(riak_attempt *a, riak_client *c, CURL *h, size_t node,
                               unsigned attempt, const riak_request *req, riak_response *resp) {
    memset(a, 0, sizeof *a);
    memset(resp, 0, sizeof *resp);
    a->c = c;
    a->h = h;
    a->req = req;
    a->resp = resp;
    a->node = node;
    a->attempt = attempt;
    a->url = riak_strprintf("%s%s", c->nodes[node].base, req->url);
    if (a->url == NULL) {
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }

    curl_easy_reset(h);
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, a->curl_err);
    curl_easy_setopt(h, CURLOPT_URL, a->url);
    curl_easy_setopt(h, CURLOPT_PRIVATE, a);
    curl_easy_setopt(h, CURLOPT_USERAGENT, RIAK_USER_AGENT);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(h, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    if (c->cfg.tls) {
        curl_easy_setopt(h, CURLOPT_DEBUGFUNCTION, debug_cb);
        curl_easy_setopt(h, CURLOPT_DEBUGDATA, a);
        curl_easy_setopt(h, CURLOPT_VERBOSE, 1L);
    }
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, c->cfg.tls_verify ? 1L : 0L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, c->cfg.tls_verify ? 2L : 0L);
    if (c->ca_file) {
        curl_easy_setopt(h, CURLOPT_CAINFO, c->ca_file);
    }
    if (c->cert_file) {
        curl_easy_setopt(h, CURLOPT_SSLCERT, c->cert_file);
        curl_easy_setopt(h, CURLOPT_SSLCERTTYPE, "PEM");
    }
    if (c->key_file) {
        curl_easy_setopt(h, CURLOPT_SSLKEY, c->key_file);
        curl_easy_setopt(h, CURLOPT_SSLKEYTYPE, "PEM");
    }
    if (c->key_password) {
        curl_easy_setopt(h, CURLOPT_KEYPASSWD, c->key_password);
    }
    if (c->username) {
        curl_easy_setopt(h, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
        curl_easy_setopt(h, CURLOPT_USERNAME, c->username);
        curl_easy_setopt(h, CURLOPT_PASSWORD, c->password ? c->password : "");
    }
    if (c->cfg.connect_timeout_ms > 0) {
        curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, c->cfg.connect_timeout_ms);
    }
    if (c->cfg.timeout_ms > 0) {
        curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, c->cfg.timeout_ms);
    }
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, a);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, a);

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
    riak_status st = RIAK_OK;
    for (struct curl_slist *it = req->headers; st == RIAK_OK && it; it = it->next) {
        st = append_header(&a->extra, it->data);
    }
    if (st == RIAK_OK && c->client_id) {
        char *hdr = riak_strprintf("X-Riak-ClientId: %s", c->client_id);
        st = hdr ? append_header(&a->extra, hdr) : RIAK_E_NOMEM;
        free(hdr);
    }
    if (st == RIAK_OK) {
        /* disable "Expect: 100-continue" round trips */
        st = append_header(&a->extra, "Expect:");
    }
    if (st != RIAK_OK) {
        curl_slist_free_all(a->extra);
        free(a->url);
        a->extra = NULL;
        a->url = NULL;
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, a->extra);
    c->nodes[node].requests++;
    a->started_ms = riak_now_ms();
    riak_log(c, RIAK_LOG_DEBUG, "%s %s%s (attempt %u)", req->method, c->nodes[node].base, req->url,
             attempt);
    return RIAK_OK;
}

static void node_result(riak_client *c, riak_node_state *n, bool ok) {
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

riak_status riak_attempt_finish(riak_attempt *a, CURLcode rc, enum riak_retry *retry) {
    riak_client *c = a->c;
    riak_response *resp = a->resp;
    riak_node_state *node = &c->nodes[a->node];
    *retry = RIAK_RETRY_NONE;
    c->last_node = a->node;

    curl_easy_getinfo(a->h, CURLINFO_RESPONSE_CODE, &resp->status);
    c->http_status = resp->status;
    curl_easy_setopt(a->h, CURLOPT_HTTPHEADER, NULL);
    curl_easy_setopt(a->h, CURLOPT_URL, NULL);
    curl_slist_free_all(a->extra);
    a->extra = NULL;

    riak_status st = RIAK_OK;
    riak_clear_error(c);
    if (a->aborted) {
        riak_response_free(resp);
        st = riak_set_error(c, a->abort_status, "%s", a->abort_msg);
    } else if (rc != CURLE_OK) {
        riak_response_free(resp);
        bool not_sent = rc == CURLE_COULDNT_CONNECT || rc == CURLE_COULDNT_RESOLVE_HOST ||
                        rc == CURLE_COULDNT_RESOLVE_PROXY;
        st = map_curl_error(a, rc);
        if (not_sent) {
            *retry = RIAK_RETRY_ANY;
        } else if (st == RIAK_E_TRANSPORT && (a->req->sink == NULL || a->received == 0)) {
            *retry = RIAK_RETRY_IDEMPOTENT; /* nothing was handed to a streaming callback yet */
        }
    } else if (resp->status == 503) {
        *retry = RIAK_RETRY_IDEMPOTENT; /* node overloaded or still starting */
    }
    node_result(c, node, *retry == RIAK_RETRY_NONE);

    bool again = a->req->pin == 0 && riak_retry_allowed(c, a->req, *retry, a->attempt + 1);
    if (c->cfg.metrics_fn) {
        riak_request_metrics m = {.method = a->req->method,
                                  .path = a->req->url,
                                  .host = node->host,
                                  .port = node->port,
                                  .attempt = a->attempt,
                                  .status = st,
                                  .http_status = resp->status,
                                  .duration_ms = (double)(riak_now_ms() - a->started_ms),
                                  .bytes_received = a->received,
                                  .will_retry = again};
        c->cfg.metrics_fn(&m, c->cfg.metrics_ctx);
    }
    if (*retry != RIAK_RETRY_NONE) {
        riak_log(c, again ? RIAK_LOG_WARN : RIAK_LOG_ERROR, "%s %s on %s:%u failed (%s)%s",
                 a->req->method, a->req->url, node->host, node->port,
                 st != RIAK_OK ? c->errmsg : "HTTP 503", again ? ", retrying" : "");
    }
    free(a->url);
    a->url = NULL;
    return st;
}

long long riak_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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

static void sleep_ms(long long ms) {
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0) {
    }
}

static bool idempotent(const char *method) {
    return strcmp(method, "GET") == 0 || strcmp(method, "PUT") == 0 ||
           strcmp(method, "DELETE") == 0;
}

bool riak_retry_allowed(const riak_client *c, const riak_request *req, enum riak_retry retry,
                        unsigned attempts_done) {
    if (attempts_done > c->cfg.max_retries) {
        return false;
    }
    return retry == RIAK_RETRY_ANY || (retry == RIAK_RETRY_IDEMPOTENT && idempotent(req->method));
}

void riak_error_add_node(riak_client *c, riak_status st) {
    if (st != RIAK_OK && c->n_nodes > 1) {
        const riak_node_state *n = &c->nodes[c->last_node];
        char detail[sizeof c->errmsg];
        snprintf(detail, sizeof detail, "%s", c->errmsg);
        riak_set_error(c, st, "%s:%u: %s", n->host, n->port, detail);
    }
}

riak_status riak_perform(riak_client *c, const riak_request *req, riak_response *resp) {
    size_t k = req->pin > 0 ? (req->pin - 1) % c->n_nodes : riak_pick_node(c, SIZE_MAX);
    for (unsigned attempt = 0;; attempt++) {
        riak_attempt a;
        c->http_status = 0;
        riak_status st = riak_attempt_setup(&a, c, c->curl, k, attempt, req, resp);
        if (st != RIAK_OK) {
            return st;
        }
        enum riak_retry retry;
        st = riak_attempt_finish(&a, curl_easy_perform(c->curl), &retry);
        if (req->pin > 0 || !riak_retry_allowed(c, req, retry, attempt + 1)) {
            riak_error_add_node(c, st);
            return st;
        }
        riak_response_free(resp);
        sleep_ms(riak_backoff_ms(c, attempt));
        k = riak_pick_node(c, k);
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
    case 401:
    case 403:
        st = RIAK_E_UNAUTHORIZED;
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
