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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    if (t->received > t->c->cfg.max_body_bytes) {
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

riak_status riak_perform(riak_client *c, const riak_request *req, riak_response *resp) {
    CURL *h = c->curl;
    transfer t = {.c = c, .req = req, .resp = resp};
    struct curl_slist *headers = req->headers;
    struct curl_slist *extra = NULL;

    memset(resp, 0, sizeof *resp);
    c->http_status = 0;
    c->curl_err[0] = '\0';
    riak_clear_error(c);

    curl_easy_reset(h);
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, c->curl_err);
    curl_easy_setopt(h, CURLOPT_URL, req->url);
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

    CURLcode rc = curl_easy_perform(h);
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &resp->status);
    c->http_status = resp->status;
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, NULL);
    curl_slist_free_all(extra);

    if (t.aborted) {
        riak_response_free(resp);
        return riak_set_error(c, t.abort_status, "%s", t.abort_msg);
    }
    if (rc != CURLE_OK) {
        riak_response_free(resp);
        return map_curl_error(c, rc);
    }
    return RIAK_OK;

nomem:
    curl_slist_free_all(extra);
    return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
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
    if (riak_buf_append_str(b, c->base_url) != 0) {
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
