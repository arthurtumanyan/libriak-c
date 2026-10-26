/*
 File:   kv.c
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later

 Object get / put / delete.
 */

#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "multipart.h"

static const riak_opts no_opts;

static int add_timeout(riak_buf *url, const riak_opts *o) {
    if (o->server_timeout_ms == 0) {
        return 0;
    }
    char num[16];
    snprintf(num, sizeof num, "%u", o->server_timeout_ms);
    return riak_url_append_query(url, "timeout", num);
}

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

enum op { OP_GET, OP_GET_SIBLINGS, OP_PUT, OP_DELETE };

static riak_status object_headers(riak_client *c, const riak_opts *o, enum op op,
                                  struct curl_slist **h) {
    riak_status st = RIAK_OK;
    if (op == OP_PUT) {
        st = riak_add_header(c, h, "Content-Type", NULL,
                             o->content_type ? o->content_type : "application/octet-stream");
    }
    if (op == OP_GET_SIBLINGS) {
        st = riak_add_header(c, h, "Accept", NULL, "multipart/mixed, */*;q=0.9");
    }
    if (st == RIAK_OK && o->vclock) {
        st = riak_add_header(c, h, "X-Riak-Vclock", NULL, o->vclock);
    }
    if (op == OP_DELETE) {
        return st;
    }
    const riak_kv_pair cond[] = {{"If-None-Match", o->if_none_match},
                                 {"If-Match", o->if_match},
                                 {"If-Modified-Since", o->if_modified_since},
                                 {"If-Unmodified-Since", o->if_unmodified_since}};
    for (size_t i = 0; st == RIAK_OK && i < sizeof cond / sizeof cond[0]; i++) {
        if (cond[i].value) {
            st = riak_add_header(c, h, cond[i].name, NULL, cond[i].value);
        }
    }
    if (op != OP_PUT) {
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
    if (out->data == NULL || riak_object_from_headers(&resp->headers, NULL, out) != 0) {
        riak_object_free(out);
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    return RIAK_OK;
}

/* Issue the GET for riak_get / riak_get_siblings. */
static riak_status fetch(riak_client *c, const char *bucket, const char *key, const riak_opts *o,
                         enum op op, riak_response *resp) {
    riak_status st;
    if ((st = riak_check_name(c, bucket, "bucket")) != RIAK_OK ||
        (st = riak_check_name(c, key, "key")) != RIAK_OK ||
        (o->bucket_type && (st = riak_check_name(c, o->bucket_type, "bucket type")) != RIAK_OK)) {
        return st;
    }
    riak_buf url = {0};
    struct curl_slist *headers = NULL;
    if (riak_url_bucket(&url, c, o->bucket_type, bucket, "keys", key) != 0 ||
        add_quorums(&url, o, true, false, false) != 0 || add_timeout(&url, o) != 0) {
        st = riak_set_error(c, RIAK_E_INVALID_ARG, "invalid quorum or out of memory");
    } else if ((st = object_headers(c, o, op, &headers)) == RIAK_OK) {
        riak_request req = {.method = "GET", .url = url.data, .headers = headers};
        st = riak_perform(c, &req, resp);
    }
    curl_slist_free_all(headers);
    riak_buf_free(&url);
    return st;
}

/* Turn a 200 or 300 (multipart/mixed) response into a sibling set. */
static riak_status to_sibling_set(riak_client *c, riak_response *resp, riak_object_set *out) {
    if (resp->status == 200) {
        out->items = calloc(1, sizeof *out->items);
        if (out->items == NULL) {
            return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
        }
        riak_status st = fill_object(c, resp, &out->items[0]);
        if (st != RIAK_OK) {
            return st;
        }
        out->count = 1;
        if (out->items[0].vclock && (out->vclock = riak_strdup(out->items[0].vclock)) == NULL) {
            return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
        }
        return RIAK_OK;
    }
    char *boundary = riak_multipart_boundary(riak_headerlist_get(&resp->headers, "Content-Type"));
    if (boundary == NULL) {
        return riak_set_error(c, RIAK_E_PARSE, "siblings response is not multipart/mixed");
    }
    riak_part *parts = NULL;
    size_t n = 0;
    riak_status st = riak_multipart_parse(boundary, resp->body.data ? resp->body.data : "",
                                          resp->body.len, &parts, &n);
    free(boundary);
    if (st != RIAK_OK) {
        return riak_set_error(c, st, "cannot parse siblings");
    }
    const char *vclock = riak_headerlist_get(&resp->headers, "X-Riak-Vclock");
    if (n == 0 || (vclock && (out->vclock = riak_strdup(vclock)) == NULL) ||
        (out->items = calloc(n, sizeof *out->items)) == NULL) {
        riak_parts_free(parts, n);
        return riak_set_error(c, n ? RIAK_E_NOMEM : RIAK_E_PARSE,
                              n ? "out of memory" : "empty siblings response");
    }
    for (size_t i = 0; i < n; i++) {
        riak_object *obj = &out->items[i];
        obj->data = parts[i].body;
        obj->len = parts[i].len;
        parts[i].body = NULL;
        out->count++;
        if (riak_object_from_headers(&parts[i].headers, vclock, obj) != 0) {
            riak_parts_free(parts, n);
            return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
        }
    }
    riak_parts_free(parts, n);
    return RIAK_OK;
}

riak_status riak_get_siblings(riak_client *client, const char *bucket, const char *key,
                              const riak_opts *opts, riak_object_set *out) {
    if (client == NULL || out == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    riak_response resp = {0};
    riak_status st = fetch(client, bucket, key, opts ? opts : &no_opts, OP_GET_SIBLINGS, &resp);
    if (st == RIAK_OK) {
        st = (resp.status == 200 || resp.status == 300) ? to_sibling_set(client, &resp, out)
                                                        : riak_http_error(client, &resp);
    }
    if (st != RIAK_OK) {
        riak_object_set_free(out);
    }
    riak_response_free(&resp);
    return st;
}

riak_status riak_get(riak_client *client, const char *bucket, const char *key,
                     const riak_opts *opts, riak_object *out) {
    if (client == NULL || out == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    const riak_opts *o = opts ? opts : &no_opts;
    riak_response resp = {0};
    riak_status st = fetch(client, bucket, key, o, o->resolver ? OP_GET_SIBLINGS : OP_GET, &resp);
    if (st != RIAK_OK) {
        goto done;
    }
    if (resp.status == 200) {
        st = fill_object(client, &resp, out);
    } else if (resp.status == 300 && o->resolver) {
        riak_object_set set = {0};
        st = to_sibling_set(client, &resp, &set);
        if (st == RIAK_OK) {
            size_t pick = o->resolver(set.items, set.count, o->resolver_ctx);
            if (pick < set.count) {
                *out = set.items[pick];
                memset(&set.items[pick], 0, sizeof set.items[pick]);
            } else {
                st = riak_set_error(client, RIAK_E_SIBLINGS, "resolver declined %zu siblings",
                                    set.count);
            }
        }
        riak_object_set_free(&set);
    } else {
        st = riak_http_error(client, &resp);
    }
done:
    riak_response_free(&resp);
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
    const riak_opts *o = opts ? opts : &no_opts;
    if ((key != NULL && (st = riak_check_name(client, key, "key")) != RIAK_OK) ||
        (o->bucket_type &&
         (st = riak_check_name(client, o->bucket_type, "bucket type")) != RIAK_OK)) {
        return st;
    }

    riak_buf url = {0};
    struct curl_slist *headers = NULL;
    riak_response resp = {0};
    if (riak_url_bucket(&url, client, o->bucket_type, bucket, "keys", key) != 0 ||
        add_quorums(&url, o, false, true, false) != 0 || add_timeout(&url, o) != 0 ||
        (o->return_body && riak_url_append_query(&url, "returnbody", "true") != 0)) {
        st = riak_set_error(client, RIAK_E_INVALID_ARG, "invalid quorum or out of memory");
        goto done;
    }
    if ((st = object_headers(client, o, OP_PUT, &headers)) != RIAK_OK) {
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
        const char *location = riak_headerlist_get(&resp.headers, "Location");
        *created_key = location ? key_from_location(location) : NULL;
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
    if (o->bucket_type &&
        (st = riak_check_name(client, o->bucket_type, "bucket type")) != RIAK_OK) {
        return st;
    }

    riak_buf url = {0};
    struct curl_slist *headers = NULL;
    riak_response resp = {0};
    if (riak_url_bucket(&url, client, o->bucket_type, bucket, "keys", key) != 0 ||
        add_quorums(&url, o, true, true, true) != 0 || add_timeout(&url, o) != 0) {
        st = riak_set_error(client, RIAK_E_INVALID_ARG, "invalid quorum or out of memory");
        goto done;
    }
    if ((st = object_headers(client, o, OP_DELETE, &headers)) != RIAK_OK) {
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
