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
        if (rc == CURLE_SSL_CONNECT_ERROR && !a->c->cfg.tls_legacy) {
            return riak_set_error(a->c, RIAK_E_TLS, "%s%s", detail, LEGACY_HINT);
        }
        return riak_set_error(a->c, RIAK_E_TLS, "%s", detail);
    default:
        return riak_set_error(a->c, RIAK_E_TRANSPORT, "%s", detail);
    }
}

/* CURLOPT_SSL_CTX_FUNCTION: riak_config::tls_legacy on curl's OpenSSL context. */
static CURLcode legacy_ctx_cb(CURL *h, void *ssl_ctx, void *userp) {
    (void)h;
    (void)userp;
    return riak_tls_legacy_ctx(ssl_ctx) == 0 ? CURLE_OK : CURLE_SSL_CIPHER;
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
    if (c->cfg.tls && c->cfg.tls_legacy) {
        curl_easy_setopt(h, CURLOPT_SSLVERSION,
                         (long)((unsigned long)CURL_SSLVERSION_TLSv1_2 |
                                (unsigned long)CURL_SSLVERSION_MAX_TLSv1_2));
        if (curl_easy_setopt(h, CURLOPT_SSL_CTX_FUNCTION, legacy_ctx_cb) != CURLE_OK) {
            free(a->url);
            a->url = NULL;
            return riak_set_error(c, RIAK_E_TLS, "tls_legacy needs libcurl built with OpenSSL");
        }
    }
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

    if (strcmp(req->method, "HEAD") == 0) {
        curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
    } else if (strcmp(req->method, "GET") != 0) {
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

riak_status riak_attempt_finish(riak_attempt *a, CURLcode rc, enum riak_retry *retry) {
    riak_client *c = a->c;
    riak_response *resp = a->resp;
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
    riak_node_result(c, a->node, *retry == RIAK_RETRY_NONE);

    bool again = a->req->pin == 0 &&
                 riak_retry_allowed(c, riak_request_idempotent(a->req), *retry, a->attempt + 1);
    riak_attempt_info info = {.method = a->req->method,
                              .path = a->req->url,
                              .node = a->node,
                              .attempt = a->attempt,
                              .status = st,
                              .http_status = resp->status,
                              .started_ms = a->started_ms,
                              .received = a->received,
                              .retry = *retry,
                              .will_retry = again,
                              .failure = "HTTP 503",
                              .transport = RIAK_TRANSPORT_HTTP};
    riak_attempt_report(c, &info);
    free(a->url);
    a->url = NULL;
    return st;
}

bool riak_request_idempotent(const riak_request *req) {
    return !req->no_retry &&
           (strcmp(req->method, "GET") == 0 || strcmp(req->method, "HEAD") == 0 ||
            strcmp(req->method, "PUT") == 0 || strcmp(req->method, "DELETE") == 0);
}

static riak_status perform(riak_client *c, const riak_request *req, riak_response *resp) {
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
        if (req->pin > 0 ||
            !riak_retry_allowed(c, riak_request_idempotent(req), retry, attempt + 1)) {
            riak_error_add_node(c, st);
            return st;
        }
        riak_response_free(resp);
        riak_sleep_ms(riak_backoff_ms(c, attempt));
        k = riak_pick_node(c, k);
    }
}

riak_status riak_perform(riak_client *c, const riak_request *req, riak_response *resp) {
    if (c->busy) {
        return riak_set_error(c, RIAK_E_INVALID_ARG, RIAK_CLIENT_BUSY_MSG);
    }
    c->busy = true;
    riak_status st = perform(c, req, resp);
    c->busy = false;
    return st;
}

void riak_response_free(riak_response *resp) {
    riak_buf_free(&resp->body);
    riak_headerlist_clear(&resp->headers);
    long status = resp->status;
    memset(resp, 0, sizeof *resp);
    resp->status = status;
}

riak_status riak_http_error_or_unsupported(riak_client *c, const riak_response *resp,
                                           const char *what) {
    static const char page[] = "<HTML><HEAD><TITLE>404 Not Found</TITLE>";
    if (resp->status == 404 && resp->body.len >= sizeof page - 1 &&
        memcmp(resp->body.data, page, sizeof page - 1) == 0) {
        return riak_set_error(c, RIAK_E_UNSUPPORTED, "this node does not offer %s", what);
    }
    return riak_http_error(c, resp);
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
    case 426: /* security is enabled and the request was not sent over HTTPS */
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
