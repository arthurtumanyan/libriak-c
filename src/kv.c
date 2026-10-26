/*
 File:   kv.c
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later

 Object get / put / delete.
 */

#include "internal.h"

#include <stdlib.h>
#include <string.h>

static const riak_opts no_opts;

static int add_quorums(riak_buf *url, const riak_opts *o, bool r, bool w, bool rw) {
    if (r && (riak_url_append_quorum(url, "r", o->r) != 0 ||
              riak_url_append_quorum(url, "pr", o->pr) != 0)) {
        return -1;
    }
    if (w && (riak_url_append_quorum(url, "w", o->w) != 0 ||
              riak_url_append_quorum(url, "dw", o->dw) != 0 ||
              riak_url_append_quorum(url, "pw", o->pw) != 0)) {
        return -1;
    }
    if (rw && riak_url_append_quorum(url, "rw", o->rw) != 0) {
        return -1;
    }
    return 0;
}

static riak_status object_headers(riak_client *c, const riak_opts *o, bool is_write,
                                  struct curl_slist **h) {
    riak_status st = RIAK_OK;
    if (is_write) {
        st = riak_add_header(c, h, "Content-Type", NULL,
                             o->content_type ? o->content_type : "application/octet-stream");
    }
    if (st == RIAK_OK && o->vclock) {
        st = riak_add_header(c, h, "X-Riak-Vclock", NULL, o->vclock);
    }
    if (!is_write) {
        return st;
    }
    for (size_t i = 0; st == RIAK_OK && i < o->n_meta; i++) {
        st = riak_add_header(c, h, "X-Riak-Meta-", o->meta[i].name, o->meta[i].value);
    }
    for (size_t i = 0; st == RIAK_OK && i < o->n_indexes; i++) {
        st = riak_add_header(c, h, "X-Riak-Index-", o->indexes[i].name, o->indexes[i].value);
    }
    for (size_t i = 0; st == RIAK_OK && i < o->n_links; i++) {
        st = riak_add_header(c, h, "Link", NULL, o->links[i]);
    }
    return st;
}

static riak_status fill_object(riak_client *c, riak_response *resp, riak_object *out) {
    out->data = riak_buf_steal(&resp->body, &out->len);
    if (out->data == NULL) {
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    out->content_type = resp->content_type;
    out->vclock = resp->vclock;
    out->etag = resp->etag;
    out->last_modified = resp->last_modified;
    resp->content_type = resp->vclock = resp->etag = resp->last_modified = NULL;
    return RIAK_OK;
}

riak_status riak_get(riak_client *client, const char *bucket, const char *key,
                     const riak_opts *opts, riak_object *out) {
    if (client == NULL || out == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    riak_status st;
    if ((st = riak_check_name(client, bucket, "bucket")) != RIAK_OK ||
        (st = riak_check_name(client, key, "key")) != RIAK_OK) {
        return st;
    }
    const riak_opts *o = opts ? opts : &no_opts;

    riak_buf url = {0};
    struct curl_slist *headers = NULL;
    riak_response resp = {0};
    if (riak_url_bucket(&url, client, bucket, "keys", key) != 0 ||
        add_quorums(&url, o, true, false, false) != 0) {
        st = riak_set_error(client, RIAK_E_INVALID_ARG, "invalid quorum or out of memory");
        goto done;
    }
    if ((st = object_headers(client, o, false, &headers)) != RIAK_OK) {
        goto done;
    }
    riak_request req = {.method = "GET", .url = url.data, .headers = headers};
    if ((st = riak_perform(client, &req, &resp)) != RIAK_OK) {
        goto done;
    }
    st = resp.status == 200 ? fill_object(client, &resp, out) : riak_http_error(client, &resp);
done:
    riak_response_free(&resp);
    curl_slist_free_all(headers);
    riak_buf_free(&url);
    return st;
}

/* Location: /buckets/<bucket>/keys/<key> (or /riak/<bucket>/<key>). */
static char *key_from_location(const char *loc) {
    const char *slash = strrchr(loc, '/');
    if (slash == NULL || slash[1] == '\0') {
        return NULL;
    }
    const char *k = slash + 1;
    size_t len = strcspn(k, "?#");
    return riak_url_unescape(k, len);
}

riak_status riak_put(riak_client *client, const char *bucket, const char *key, const void *value,
                     size_t len, const riak_opts *opts, riak_object *out, char **created_key) {
    if (client == NULL || (value == NULL && len > 0)) {
        return RIAK_E_INVALID_ARG;
    }
    if (out) memset(out, 0, sizeof *out);
    if (created_key) *created_key = NULL;
    riak_status st;
    if ((st = riak_check_name(client, bucket, "bucket")) != RIAK_OK) {
        return st;
    }
    if (key != NULL && (st = riak_check_name(client, key, "key")) != RIAK_OK) {
        return st;
    }
    const riak_opts *o = opts ? opts : &no_opts;

    riak_buf url = {0};
    struct curl_slist *headers = NULL;
    riak_response resp = {0};
    if (riak_url_bucket(&url, client, bucket, "keys", key) != 0 ||
        add_quorums(&url, o, false, true, false) != 0 ||
        (o->return_body && riak_url_append_query(&url, "returnbody", "true") != 0)) {
        st = riak_set_error(client, RIAK_E_INVALID_ARG, "invalid quorum or out of memory");
        goto done;
    }
    if ((st = object_headers(client, o, true, &headers)) != RIAK_OK) {
        goto done;
    }
    riak_request req = {.method = key ? "PUT" : "POST",
                        .url = url.data,
                        .headers = headers,
                        .body = value ? value : "",
                        .body_len = len};
    if ((st = riak_perform(client, &req, &resp)) != RIAK_OK) {
        goto done;
    }
    if (resp.status != 200 && resp.status != 201 && resp.status != 204) {
        st = riak_http_error(client, &resp);
        goto done;
    }
    if (created_key && key == NULL) {
        *created_key = resp.location ? key_from_location(resp.location) : NULL;
        if (*created_key == NULL) {
            st = riak_set_error(client, RIAK_E_PARSE, "no usable Location header");
            goto done;
        }
    }
    if (out && resp.status != 204) {
        st = fill_object(client, &resp, out);
    }
done:
    if (st != RIAK_OK && created_key) {
        free(*created_key);
        *created_key = NULL;
    }
    riak_response_free(&resp);
    curl_slist_free_all(headers);
    riak_buf_free(&url);
    return st;
}

riak_status riak_delete(riak_client *client, const char *bucket, const char *key,
                        const riak_opts *opts) {
    if (client == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    riak_status st;
    if ((st = riak_check_name(client, bucket, "bucket")) != RIAK_OK ||
        (st = riak_check_name(client, key, "key")) != RIAK_OK) {
        return st;
    }
    const riak_opts *o = opts ? opts : &no_opts;

    riak_buf url = {0};
    struct curl_slist *headers = NULL;
    riak_response resp = {0};
    if (riak_url_bucket(&url, client, bucket, "keys", key) != 0 ||
        add_quorums(&url, o, true, true, true) != 0) {
        st = riak_set_error(client, RIAK_E_INVALID_ARG, "invalid quorum or out of memory");
        goto done;
    }
    if ((st = object_headers(client, o, false, &headers)) != RIAK_OK) {
        goto done;
    }
    riak_request req = {.method = "DELETE", .url = url.data, .headers = headers};
    if ((st = riak_perform(client, &req, &resp)) != RIAK_OK) {
        goto done;
    }
    if (resp.status != 204 && resp.status != 404) {
        st = riak_http_error(client, &resp);
    }
done:
    riak_response_free(&resp);
    curl_slist_free_all(headers);
    riak_buf_free(&url);
    return st;
}
