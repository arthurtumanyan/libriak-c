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
#include "pb.h"

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
    return riak_url_append_quorum(url, "node_confirms", o->node_confirms);
}

/* Put-only query parameters. */
static int add_write_flags(riak_buf *url, const riak_opts *o) {
    static const char *const sync[] = {NULL, "backend", "one", "all"};
    if ((unsigned)o->sync_on_write > RIAK_SYNC_ALL) {
        return -1;
    }
    if (sync[o->sync_on_write] &&
        riak_url_append_query(url, "sync_on_write", sync[o->sync_on_write]) != 0) {
        return -1;
    }
    return o->asis ? riak_url_append_query(url, "asis", "true") : 0;
}

enum op { OP_GET, OP_GET_SIBLINGS, OP_PUT, OP_DELETE };

/* Options only the Protocol Buffers transport can express. */
static riak_status http_unsupported(riak_client *c, const riak_opts *o) {
    const char *what = o->sloppy_quorum     ? "sloppy_quorum"
                       : o->n_val           ? "n_val"
                       : o->if_modified     ? "if_modified"
                       : o->if_not_modified ? "if_not_modified"
                       : o->return_head     ? "return_head"
                       : o->deletedvclock   ? "deletedvclock"
                                            : NULL;
    if (what) {
        return riak_set_error(c, RIAK_E_UNSUPPORTED, "%s needs the Protocol Buffers transport",
                              what);
    }
    return RIAK_OK;
}

static int add_read_flags(riak_buf *url, const riak_opts *o) {
    if (o->basic_quorum && riak_url_append_query(url, "basic_quorum", "true") != 0) {
        return -1;
    }
    if (o->notfound_ok && riak_url_append_query(url, "notfound_ok", "true") != 0) {
        return -1;
    }
    return 0;
}

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

void riak_prepared_free(riak_prepared *p) {
    riak_buf_free(&p->url);
    curl_slist_free_all(p->headers);
    free(p->body);
    memset(p, 0, sizeof *p);
}

static riak_status prepare_fetch(riak_client *c, const char *bucket, const char *key,
                                 const riak_opts *o, enum op op, riak_prepared *p) {
    memset(p, 0, sizeof *p);
    riak_status st;
    if ((st = riak_check_name(c, bucket, "bucket")) != RIAK_OK ||
        (st = riak_check_name(c, key, "key")) != RIAK_OK ||
        (o->bucket_type && (st = riak_check_name(c, o->bucket_type, "bucket type")) != RIAK_OK) ||
        (st = http_unsupported(c, o)) != RIAK_OK) {
        return st;
    }
    if (riak_url_bucket(&p->url, c, o->bucket_type, bucket, "keys", key) != 0 ||
        add_quorums(&p->url, o, true, false, false) != 0 || add_timeout(&p->url, o) != 0 ||
        add_read_flags(&p->url, o) != 0) {
        st = riak_set_error(c, RIAK_E_INVALID_ARG, "invalid quorum or out of memory");
    } else {
        st = object_headers(c, o, op, &p->headers);
    }
    if (st != RIAK_OK) {
        riak_prepared_free(p);
        return st;
    }
    /* HEAD returns the metadata only; siblings still need the multipart body */
    p->req = (riak_request){.method = o->head && op == OP_GET ? "HEAD" : "GET",
                            .url = p->url.data,
                            .headers = p->headers};
    return RIAK_OK;
}

/* A prepared request is run, then what prepare allocated is freed. */
static riak_status run(riak_client *c, riak_status st, riak_prepared *p, riak_response *resp) {
    if (st == RIAK_OK) {
        st = riak_perform(c, &p->req, resp);
    }
    riak_prepared_free(p);
    return st;
}

/* A 200 or 300 (multipart/mixed) response is turned into a sibling set. */
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

riak_status riak_kv_prepare_siblings(riak_client *c, const char *bucket, const char *key,
                                     const riak_opts *o, riak_prepared *p) {
    return prepare_fetch(c, bucket, key, o, OP_GET_SIBLINGS, p);
}

riak_status riak_kv_finish_siblings(riak_client *c, riak_response *resp, riak_object_set *out) {
    memset(out, 0, sizeof *out);
    riak_status st = (resp->status == 200 || resp->status == 300) ? to_sibling_set(c, resp, out)
                                                                  : riak_http_error(c, resp);
    if (st != RIAK_OK) {
        riak_object_set_free(out);
    }
    return st;
}

riak_status riak_get_siblings(riak_client *client, const char *bucket, const char *key,
                              const riak_opts *opts, riak_object_set *out) {
    if (client == NULL || out == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    if (riak_is_pb(client)) {
        return riak_pb_get_siblings(client, bucket, key, opts ? opts : &no_opts, out);
    }
    riak_response resp = {0};
    riak_prepared p;
    riak_status st = riak_kv_prepare_siblings(client, bucket, key, opts ? opts : &no_opts, &p);
    st = run(client, st, &p, &resp);
    if (st == RIAK_OK) {
        st = riak_kv_finish_siblings(client, &resp, out);
    }
    riak_response_free(&resp);
    return st;
}

riak_status riak_kv_prepare_get(riak_client *c, const char *bucket, const char *key,
                                const riak_opts *o, riak_prepared *p) {
    return prepare_fetch(c, bucket, key, o, o->resolver ? OP_GET_SIBLINGS : OP_GET, p);
}

riak_status riak_kv_finish_get(riak_client *c, const riak_opts *o, riak_response *resp,
                               riak_object *out) {
    if (resp->status == 200) {
        return fill_object(c, resp, out);
    }
    if (resp->status != 300 || o->resolver == NULL) {
        return riak_http_error(c, resp);
    }
    riak_object_set set = {0};
    riak_status st = to_sibling_set(c, resp, &set);
    if (st == RIAK_OK) {
        size_t pick = o->resolver(set.items, set.count, o->resolver_ctx);
        if (pick < set.count) {
            *out = set.items[pick];
            memset(&set.items[pick], 0, sizeof set.items[pick]);
        } else {
            st = riak_set_error(c, RIAK_E_SIBLINGS, "resolver declined %zu siblings", set.count);
        }
    }
    riak_object_set_free(&set);
    return st;
}

riak_status riak_get(riak_client *client, const char *bucket, const char *key,
                     const riak_opts *opts, riak_object *out) {
    if (client == NULL || out == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    const riak_opts *o = opts ? opts : &no_opts;
    if (riak_is_pb(client)) {
        return riak_pb_get(client, bucket, key, o, out);
    }
    riak_response resp = {0};
    riak_prepared p;
    riak_status st = run(client, riak_kv_prepare_get(client, bucket, key, o, &p), &p, &resp);
    if (st == RIAK_OK) {
        st = riak_kv_finish_get(client, o, &resp, out);
    }
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

riak_status riak_kv_prepare_put(riak_client *c, const char *bucket, const char *key,
                                const void *value, size_t len, const riak_opts *o, bool copy_body,
                                riak_prepared *p) {
    memset(p, 0, sizeof *p);
    if (value == NULL && len > 0) {
        return riak_set_error(c, RIAK_E_INVALID_ARG, "value is NULL");
    }
    riak_status st;
    if ((st = riak_check_name(c, bucket, "bucket")) != RIAK_OK ||
        (key != NULL && (st = riak_check_name(c, key, "key")) != RIAK_OK) ||
        (o->bucket_type && (st = riak_check_name(c, o->bucket_type, "bucket type")) != RIAK_OK) ||
        (st = http_unsupported(c, o)) != RIAK_OK) {
        return st;
    }
    if (riak_url_bucket(&p->url, c, o->bucket_type, bucket, "keys", key) != 0 ||
        add_quorums(&p->url, o, false, true, false) != 0 || add_timeout(&p->url, o) != 0 ||
        (o->return_body && riak_url_append_query(&p->url, "returnbody", "true") != 0) ||
        add_write_flags(&p->url, o) != 0) {
        st = riak_set_error(c, RIAK_E_INVALID_ARG, "invalid quorum or out of memory");
    } else if ((st = object_headers(c, o, OP_PUT, &p->headers)) == RIAK_OK && copy_body) {
        p->body = malloc(len ? len : 1);
        if (p->body == NULL) {
            st = riak_set_error(c, RIAK_E_NOMEM, "out of memory");
        } else if (len) {
            memcpy(p->body, value, len);
        }
    }
    if (st != RIAK_OK) {
        riak_prepared_free(p);
        return st;
    }
    p->req = (riak_request){.method = key ? "PUT" : "POST",
                            .url = p->url.data,
                            .headers = p->headers,
                            .body = copy_body ? p->body : (value ? value : ""),
                            .body_len = len};
    return RIAK_OK;
}

riak_status riak_kv_finish_put(riak_client *c, bool generated_key, riak_response *resp,
                               riak_object *out, char **created_key) {
    if (resp->status != 200 && resp->status != 201 && resp->status != 204) {
        return riak_http_error(c, resp);
    }
    if (created_key && generated_key) {
        const char *location = riak_headerlist_get(&resp->headers, "Location");
        *created_key = location ? key_from_location(location) : NULL;
        if (*created_key == NULL) {
            return riak_set_error(c, RIAK_E_PARSE, "no usable Location header");
        }
    }
    if (out && resp->status != 204) {
        riak_status st = fill_object(c, resp, out);
        if (st != RIAK_OK && created_key) {
            free(*created_key);
            *created_key = NULL;
        }
        return st;
    }
    return RIAK_OK;
}

riak_status riak_put(riak_client *client, const char *bucket, const char *key, const void *value,
                     size_t len, const riak_opts *opts, riak_object *out, char **created_key) {
    if (out) memset(out, 0, sizeof *out);
    if (created_key) *created_key = NULL;
    if (client == NULL || (value == NULL && len > 0)) {
        return RIAK_E_INVALID_ARG;
    }
    const riak_opts *o = opts ? opts : &no_opts;
    if (riak_is_pb(client)) {
        return riak_pb_put(client, bucket, key, value, len, o, out, created_key);
    }
    riak_response resp = {0};
    riak_prepared p;
    riak_status st =
        run(client, riak_kv_prepare_put(client, bucket, key, value, len, o, false, &p), &p, &resp);
    if (st == RIAK_OK) {
        st = riak_kv_finish_put(client, key == NULL, &resp, out, created_key);
    }
    riak_response_free(&resp);
    return st;
}

riak_status riak_kv_prepare_delete(riak_client *c, const char *bucket, const char *key,
                                   const riak_opts *o, riak_prepared *p) {
    memset(p, 0, sizeof *p);
    riak_status st;
    if ((st = riak_check_name(c, bucket, "bucket")) != RIAK_OK ||
        (st = riak_check_name(c, key, "key")) != RIAK_OK ||
        (o->bucket_type && (st = riak_check_name(c, o->bucket_type, "bucket type")) != RIAK_OK) ||
        (st = http_unsupported(c, o)) != RIAK_OK) {
        return st;
    }
    if (riak_url_bucket(&p->url, c, o->bucket_type, bucket, "keys", key) != 0 ||
        add_quorums(&p->url, o, true, true, true) != 0 || add_timeout(&p->url, o) != 0) {
        st = riak_set_error(c, RIAK_E_INVALID_ARG, "invalid quorum or out of memory");
    } else {
        st = object_headers(c, o, OP_DELETE, &p->headers);
    }
    if (st != RIAK_OK) {
        riak_prepared_free(p);
        return st;
    }
    p->req = (riak_request){.method = "DELETE", .url = p->url.data, .headers = p->headers};
    return RIAK_OK;
}

riak_status riak_kv_finish_delete(riak_client *c, riak_response *resp) {
    return resp->status == 204 || resp->status == 404 ? RIAK_OK : riak_http_error(c, resp);
}

riak_status riak_delete(riak_client *client, const char *bucket, const char *key,
                        const riak_opts *opts) {
    if (client == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    if (riak_is_pb(client)) {
        return riak_pb_delete(client, bucket, key, opts ? opts : &no_opts);
    }
    riak_response resp = {0};
    riak_prepared p;
    riak_status st = run(
        client, riak_kv_prepare_delete(client, bucket, key, opts ? opts : &no_opts, &p), &p, &resp);
    if (st == RIAK_OK) {
        st = riak_kv_finish_delete(client, &resp);
    }
    riak_response_free(&resp);
    return st;
}
