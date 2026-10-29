/*
 File:   query.c
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later

 Secondary index queries, Riak Search and MapReduce.
 */

#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "multipart.h"
#include "parse.h"
#include "pb.h"

/* ---- streamed multipart responses ---------------------------------- */

static bool on_part(void *ctx, riak_part *part, riak_status *err) {
    riak_http_stream *s = ctx;
    if (!s->fn(s->ctx, part->body, part->len, err)) {
        if (*err == RIAK_OK) {
            s->stopped = true; /* caller is done: abort the transfer */
            *err = RIAK_E_PARSE;
        }
        return false;
    }
    return true;
}

/* Sink that splits a 200 multipart/mixed body into JSON parts as it arrives. */
bool riak_http_stream_sink(void *ctx, const char *data, size_t len, riak_status *err) {
    riak_http_stream *s = ctx;
    if (s->mp == NULL) {
        char *boundary =
            riak_multipart_boundary(riak_headerlist_get(&s->resp->headers, "Content-Type"));
        s->mp = boundary ? riak_mpstream_new(boundary, s->c->cfg.max_body_bytes, on_part, s) : NULL;
        free(boundary);
        if (s->mp == NULL) {
            *err = RIAK_E_PARSE;
            return false;
        }
    }
    riak_status st = riak_mpstream_feed(s->mp, data, len);
    if (st != RIAK_OK) {
        *err = st;
        return false;
    }
    return true;
}

void riak_http_stream_attach(riak_http_stream *s, riak_request *req) {
    req->sink = riak_http_stream_sink;
    req->sink_ctx = s;
    req->unbounded = true;
}

riak_status riak_http_stream_end(riak_http_stream *s, riak_status st) {
    if (s->stopped) {
        riak_clear_error(s->c); /* the abort was requested, not an error */
        st = RIAK_OK;
    } else if (st == RIAK_OK) {
        if (s->resp->status != 200) {
            st = riak_http_error(s->c, s->resp);
        } else if (s->mp == NULL || riak_mpstream_finish(s->mp) != RIAK_OK) {
            st = riak_set_error(s->c, RIAK_E_PARSE, "truncated multipart stream");
        }
    }
    riak_mpstream_free(s->mp);
    s->mp = NULL;
    return st;
}

/* A streamed request is performed; parts are passed to fn. */
static riak_status perform_stream(riak_client *c, const riak_request *base, riak_chunk_fn fn,
                                  void *ctx) {
    riak_response resp = {0};
    riak_http_stream s = {.c = c, .resp = &resp, .fn = fn, .ctx = ctx};
    riak_request req = *base;
    riak_http_stream_attach(&s, &req);
    riak_status st = riak_http_stream_end(&s, riak_perform(c, &req, &resp));
    riak_response_free(&resp);
    return st;
}

/* ---- secondary indexes --------------------------------------------- */

static riak_status index_url(riak_client *c, const char *bucket, const riak_index_query *q,
                             bool stream, riak_buf *url) {
    riak_status st;
    if (q == NULL) {
        return riak_set_error(c, RIAK_E_INVALID_ARG, "query must not be NULL");
    }
    if ((st = riak_check_name(c, bucket, "bucket")) != RIAK_OK ||
        (st = riak_check_name(c, q->index, "index")) != RIAK_OK ||
        (q->bucket_type && (st = riak_check_name(c, q->bucket_type, "bucket type")) != RIAK_OK)) {
        return st;
    }
    bool exact = q->value != NULL;
    bool range = q->range_min != NULL || q->range_max != NULL;
    if (exact == range || (range && (q->range_min == NULL || q->range_max == NULL))) {
        return riak_set_error(c, RIAK_E_INVALID_ARG,
                              "set either value or both range_min and range_max");
    }
    if (q->cover_context) {
        return riak_set_error(c, RIAK_E_UNSUPPORTED,
                              "cover_context needs the Protocol Buffers transport");
    }
    char num[16];
    int rc = riak_url_bucket(url, c, q->bucket_type, bucket, "index", NULL);
    rc = rc || riak_buf_append_str(url, "/") || riak_url_append_escaped(url, q->index);
    if (exact) {
        rc = rc || riak_buf_append_str(url, "/") || riak_url_append_escaped(url, q->value);
    } else {
        rc = rc || riak_buf_append_str(url, "/") || riak_url_append_escaped(url, q->range_min) ||
             riak_buf_append_str(url, "/") || riak_url_append_escaped(url, q->range_max);
    }
    if (q->max_results) {
        snprintf(num, sizeof num, "%u", q->max_results);
        rc = rc || riak_url_append_query(url, "max_results", num);
    }
    if (q->continuation) {
        rc = rc || riak_url_append_query(url, "continuation", q->continuation);
    }
    if (q->return_terms) {
        rc = rc || riak_url_append_query(url, "return_terms", "true");
    }
    if (q->term_regex) {
        rc = rc || riak_url_append_query(url, "term_regex", q->term_regex);
    }
    if (q->pagination_sort) {
        rc = rc || riak_url_append_query(url, "pagination_sort", "true");
    }
    if (q->server_timeout_ms) {
        snprintf(num, sizeof num, "%u", q->server_timeout_ms);
        rc = rc || riak_url_append_query(url, "timeout", num);
    }
    if (stream) {
        rc = rc || riak_url_append_query(url, "stream", "true");
    }
    return rc ? riak_set_error(c, RIAK_E_NOMEM, "out of memory") : RIAK_OK;
}

bool riak_index_acc_add(const char *key, const char *term, void *ctx) {
    riak_index_acc *a = ctx;
    if (riak_strlist_push(&a->keys, &a->kcap, key, strlen(key)) != 0 ||
        (a->want_terms &&
         riak_strlist_push(&a->terms, &a->tcap, term ? term : "", term ? strlen(term) : 0) != 0)) {
        a->nomem = true;
        return false;
    }
    return true;
}

/* The accumulated matches become the result page; on failure they are freed. */
riak_status riak_index_acc_take(riak_index_acc *acc, riak_index_result *out) {
    riak_status st = acc->nomem ? RIAK_E_NOMEM : RIAK_OK;
    if (st == RIAK_OK && acc->keys.items == NULL) {
        acc->keys.items = (char **)calloc(1, sizeof(char *));
        st = acc->keys.items ? RIAK_OK : RIAK_E_NOMEM;
    }
    if (st == RIAK_OK && acc->want_terms && acc->terms.items == NULL) {
        acc->terms.items = (char **)calloc(1, sizeof(char *));
        st = acc->terms.items ? RIAK_OK : RIAK_E_NOMEM;
    }
    if (st == RIAK_OK) {
        out->keys = acc->keys.items;
        out->terms = acc->terms.items;
        out->count = acc->keys.count;
    } else {
        riak_strlist_free(&acc->keys);
        riak_strlist_free(&acc->terms);
    }
    return st;
}

static riak_status pb_index_run(riak_client *c, const char *bucket, const riak_index_query *q,
                                riak_index_result *out) {
    riak_index_acc acc = {.want_terms = q && q->return_terms};
    riak_status st = riak_pb_index(c, bucket, q, riak_index_acc_add, &acc, &out->continuation);
    if (st == RIAK_OK) {
        st = riak_index_acc_take(&acc, out);
        if (st != RIAK_OK) {
            riak_set_error(c, st, "out of memory");
        }
    } else {
        riak_strlist_free(&acc.keys);
        riak_strlist_free(&acc.terms);
    }
    if (st != RIAK_OK) {
        riak_index_result_free(out);
    }
    return st;
}

riak_status riak_index_http_prepare(riak_client *c, const char *bucket, const riak_index_query *q,
                                    bool stream, riak_prepared *p) {
    memset(p, 0, sizeof *p);
    riak_status st = index_url(c, bucket, q, stream, &p->url);
    if (st != RIAK_OK) {
        riak_prepared_free(p);
        return st;
    }
    p->req = (riak_request){.method = "GET", .url = p->url.data};
    return RIAK_OK;
}

riak_status riak_index_http_finish(riak_client *c, riak_response *resp, bool want_terms,
                                   riak_index_result *out) {
    memset(out, 0, sizeof *out);
    if (resp->status != 200) {
        return riak_http_error(c, resp);
    }
    riak_index_acc acc = {.want_terms = want_terms};
    bool stopped;
    riak_status st = riak_parse_index_json(resp->body.data ? resp->body.data : "", resp->body.len,
                                           riak_index_acc_add, &acc, &out->continuation, &stopped);
    if (st == RIAK_OK) {
        st = riak_index_acc_take(&acc, out);
    } else {
        riak_strlist_free(&acc.keys);
        riak_strlist_free(&acc.terms);
    }
    if (st != RIAK_OK) {
        riak_set_error(c, st, "cannot parse index response");
        riak_index_result_free(out);
    }
    return st;
}

riak_status riak_index_query_run(riak_client *client, const char *bucket, const riak_index_query *q,
                                 riak_index_result *out) {
    if (client == NULL || out == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    if (riak_is_pb(client)) {
        return pb_index_run(client, bucket, q, out);
    }
    riak_prepared p;
    riak_status st = riak_index_http_prepare(client, bucket, q, false, &p);
    if (st != RIAK_OK) {
        return st;
    }
    riak_response resp = {0};
    st = riak_perform(client, &p.req, &resp);
    riak_prepared_free(&p);
    if (st == RIAK_OK) {
        st = riak_index_http_finish(client, &resp, q->return_terms, out);
    }
    riak_response_free(&resp);
    return st;
}

static bool forward(const char *key, const char *term, void *ctx) {
    riak_index_stream *s = ctx;
    return s->cb(key, term, s->ctx);
}

bool riak_index_chunk(void *ctx, const char *json, size_t len, riak_status *err) {
    riak_index_stream *s = ctx;
    char *cont = NULL;
    bool stopped;
    *err = riak_parse_index_json(json, len, forward, s, &cont, &stopped);
    if (*err == RIAK_OK && cont && s->continuation) {
        free(*s->continuation);
        *s->continuation = cont;
        cont = NULL;
    }
    free(cont);
    return *err == RIAK_OK && !stopped;
}

riak_status riak_index_query_stream(riak_client *client, const char *bucket,
                                    const riak_index_query *q, riak_index_cb cb, void *ctx,
                                    char **continuation) {
    if (continuation) {
        *continuation = NULL;
    }
    if (client == NULL || cb == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    if (riak_is_pb(client)) {
        riak_status st = riak_pb_index(client, bucket, q, cb, ctx, continuation);
        if (st != RIAK_OK && continuation) {
            free(*continuation);
            *continuation = NULL;
        }
        return st;
    }
    riak_buf url = {0};
    riak_status st = index_url(client, bucket, q, true, &url);
    if (st == RIAK_OK) {
        riak_index_stream s = {.cb = cb, .ctx = ctx, .continuation = continuation};
        riak_request req = {.method = "GET", .url = url.data};
        st = perform_stream(client, &req, riak_index_chunk, &s);
    }
    riak_buf_free(&url);
    if (st != RIAK_OK && continuation) {
        free(*continuation);
        *continuation = NULL;
    }
    return st;
}

void riak_index_result_free(riak_index_result *res) {
    if (res == NULL) {
        return;
    }
    riak_strlist k = {.items = res->keys, .count = res->count};
    riak_strlist_free(&k);
    if (res->terms) {
        riak_strlist t = {.items = res->terms, .count = res->count};
        riak_strlist_free(&t);
    }
    free(res->continuation);
    memset(res, 0, sizeof *res);
}

/* ---- Riak Search --------------------------------------------------- */

static int search_param(riak_buf *url, const char *name, const char *value) {
    return value ? riak_url_append_query(url, name, value) : 0;
}

static riak_status search_not_found(riak_client *c, riak_status st);

riak_status riak_search_http_prepare(riak_client *c, const char *index, const riak_search_query *q,
                                     riak_prepared *p) {
    memset(p, 0, sizeof *p);
    riak_buf *url = &p->url;
    char num[16];
    int rc = riak_buf_append_str(url, c->root) || riak_buf_append_str(url, "search/query/") ||
             riak_url_append_escaped(url, index) || riak_url_append_query(url, "wt", "json") ||
             riak_url_append_query(url, "q", q->q) || search_param(url, "fq", q->filter) ||
             search_param(url, "sort", q->sort) || search_param(url, "fl", q->fields) ||
             search_param(url, "df", q->default_field) || search_param(url, "q.op", q->op);
    if (q->start) {
        snprintf(num, sizeof num, "%u", q->start);
        rc = rc || riak_url_append_query(url, "start", num);
    }
    if (q->rows) {
        snprintf(num, sizeof num, "%u", q->rows);
        rc = rc || riak_url_append_query(url, "rows", num);
    }
    if (rc) {
        riak_prepared_free(p);
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    p->req = (riak_request){.method = "GET", .url = p->url.data};
    return RIAK_OK;
}

riak_status riak_search_http_finish(riak_client *c, riak_response *resp, riak_search_result *out) {
    memset(out, 0, sizeof *out);
    char *raw = NULL;
    riak_status st = riak_body_take(c, resp, RIAK_BODY_JSON_OBJECT, &raw);
    if (st != RIAK_OK) {
        return search_not_found(c, st);
    }
    st = riak_parse_search(raw, strlen(raw), out);
    out->raw = raw;
    if (st != RIAK_OK) {
        riak_search_result_free(out);
        return riak_set_error(c, st, "cannot parse search response");
    }
    return RIAK_OK;
}

riak_status riak_search(riak_client *client, const char *index, const riak_search_query *q,
                        riak_search_result *out) {
    if (client == NULL || out == NULL || q == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    riak_status st;
    if ((st = riak_check_name(client, index, "index")) != RIAK_OK ||
        (st = riak_check_name(client, q->q, "query")) != RIAK_OK) {
        return st;
    }
    if (riak_is_pb(client)) {
        return riak_pb_search(client, index, q, out);
    }
    riak_prepared p;
    if ((st = riak_search_http_prepare(client, index, q, &p)) != RIAK_OK) {
        return st;
    }
    riak_response resp = {0};
    st = riak_perform(client, &p.req, &resp);
    riak_prepared_free(&p);
    if (st == RIAK_OK) {
        st = riak_search_http_finish(client, &resp, out);
    }
    riak_response_free(&resp);
    return st;
}

static int search_admin_url(riak_buf *url, const riak_client *c, const char *what,
                            const char *name) {
    return riak_buf_append_str(url, c->root) || riak_buf_append_str(url, "search/") ||
           riak_buf_append_str(url, what) ||
           (name && (riak_buf_append_str(url, "/") || riak_url_append_escaped(url, name)));
}

/* Riak 2.x answers a query on a missing index, or a missing schema, with
 * Webmachine's own 404 page; a node without Search (Riak 3.x, or search = off)
 * answers every Search path with it. The index list tells them apart: it
 * exists whenever Search does. A "not found" from a node without Search
 * becomes RIAK_E_UNSUPPORTED; otherwise it is kept with its message. */
static riak_status search_not_found(riak_client *c, riak_status st) {
    if (st != RIAK_E_NOT_FOUND || riak_is_pb(c)) {
        return st;
    }
    char errmsg[sizeof c->errmsg];
    memcpy(errmsg, c->errmsg, sizeof errmsg);
    long http_status = c->http_status;
    size_t node = c->last_node;
    riak_buf url = {0};
    riak_response resp = {0};
    bool absent = false;
    if (search_admin_url(&url, c, "index", NULL) == 0) {
        /* the node that answered "not found" */
        riak_request req = {.method = "GET", .url = url.data, .pin = node + 1};
        absent = riak_perform(c, &req, &resp) == RIAK_OK && riak_http_no_resource(&resp);
    }
    riak_response_free(&resp);
    riak_buf_free(&url);
    memcpy(c->errmsg, errmsg, sizeof errmsg);
    c->http_status = http_status;
    c->last_node = node;
    return absent ? riak_set_error(c, RIAK_E_UNSUPPORTED, "this node does not offer Riak Search")
                  : st;
}

/* PUT/DELETE an admin resource and require 204 (or 200). */
static riak_status admin_write(riak_client *c, const char *method, const char *what,
                               const char *name, const char *ctype, const char *body) {
    riak_buf url = {0};
    struct curl_slist *headers = NULL;
    riak_response resp = {0};
    riak_status st = RIAK_OK;
    if (search_admin_url(&url, c, what, name)) {
        st = riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    } else if (ctype) {
        st = riak_add_header(c, &headers, "Content-Type", NULL, ctype);
    }
    if (st == RIAK_OK) {
        riak_request req = {.method = method,
                            .url = url.data,
                            .headers = headers,
                            .body = body,
                            .body_len = body ? strlen(body) : 0};
        st = riak_perform(c, &req, &resp);
        if (st == RIAK_OK && resp.status != 204 && resp.status != 200) {
            st = riak_http_error(c, &resp);
        }
    }
    riak_response_free(&resp);
    curl_slist_free_all(headers);
    riak_buf_free(&url);
    return search_not_found(c, st);
}

riak_status riak_search_index_create(riak_client *client, const char *index, const char *schema,
                                     unsigned n_val) {
    if (client == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    riak_status st = riak_check_name(client, index, "index");
    if (st != RIAK_OK) {
        return st;
    }
    if (schema && strpbrk(schema, "\"\\\r\n") != NULL) {
        return riak_set_error(client, RIAK_E_INVALID_ARG, "invalid schema name");
    }
    if (riak_is_pb(client)) {
        return riak_pb_search_index_create(client, index, schema, n_val);
    }
    riak_buf body = {0};
    int rc = riak_buf_append_str(&body, "{");
    if (schema) {
        /* schema names are plain identifiers; reject anything needing escapes */
        if (strpbrk(schema, "\"\\\r\n") != NULL) {
            riak_buf_free(&body);
            return riak_set_error(client, RIAK_E_INVALID_ARG, "invalid schema name");
        }
        rc = rc || riak_buf_append_str(&body, "\"schema\":\"") ||
             riak_buf_append_str(&body, schema) || riak_buf_append_str(&body, "\"");
    }
    if (n_val) {
        char num[32];
        snprintf(num, sizeof num, "%s\"n_val\":%u", schema ? "," : "", n_val);
        rc = rc || riak_buf_append_str(&body, num);
    }
    rc = rc || riak_buf_append_str(&body, "}");
    if (rc) {
        riak_buf_free(&body);
        return riak_set_error(client, RIAK_E_NOMEM, "out of memory");
    }
    st = admin_write(client, "PUT", "index", index, "application/json", body.data);
    riak_buf_free(&body);
    return st;
}

static riak_status admin_get(riak_client *c, const char *what, const char *name, int kind,
                             char **out) {
    if (c == NULL || out == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    *out = NULL;
    riak_buf url = {0};
    if (search_admin_url(&url, c, what, name)) {
        riak_buf_free(&url);
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    riak_status st = riak_get_body(c, url.data, kind, out);
    riak_buf_free(&url);
    return search_not_found(c, st);
}

riak_status riak_search_index_get(riak_client *client, const char *index, char **json) {
    riak_status st = client ? riak_check_name(client, index, "index") : RIAK_E_INVALID_ARG;
    if (st == RIAK_OK && json && riak_is_pb(client)) {
        *json = NULL;
        return riak_pb_search_index_get(client, index, json);
    }
    return st == RIAK_OK ? admin_get(client, "index", index, RIAK_BODY_JSON_OBJECT, json) : st;
}

riak_status riak_search_index_list(riak_client *client, char **json) {
    if (client && json && riak_is_pb(client)) {
        *json = NULL;
        return riak_pb_search_index_get(client, NULL, json);
    }
    return admin_get(client, "index", NULL, RIAK_BODY_JSON_ARRAY, json);
}

riak_status riak_search_index_delete(riak_client *client, const char *index) {
    riak_status st = client ? riak_check_name(client, index, "index") : RIAK_E_INVALID_ARG;
    if (st == RIAK_OK && riak_is_pb(client)) {
        return riak_pb_search_index_delete(client, index);
    }
    return st == RIAK_OK ? admin_write(client, "DELETE", "index", index, NULL, NULL) : st;
}

riak_status riak_search_schema_put(riak_client *client, const char *name, const char *xml) {
    if (client == NULL || xml == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    riak_status st = riak_check_name(client, name, "schema");
    if (st == RIAK_OK && riak_is_pb(client)) {
        return riak_pb_search_schema_put(client, name, xml);
    }
    return st == RIAK_OK ? admin_write(client, "PUT", "schema", name, "application/xml", xml) : st;
}

riak_status riak_search_schema_get(riak_client *client, const char *name, char **xml) {
    riak_status st = client ? riak_check_name(client, name, "schema") : RIAK_E_INVALID_ARG;
    if (st == RIAK_OK && xml && riak_is_pb(client)) {
        *xml = NULL;
        return riak_pb_search_schema_get(client, name, xml);
    }
    return st == RIAK_OK ? admin_get(client, "schema", name, RIAK_BODY_RAW, xml) : st;
}

/* ---- MapReduce ----------------------------------------------------- */

riak_status riak_mapred_http_prepare(riak_client *c, const char *job, bool chunked,
                                     riak_prepared *p) {
    memset(p, 0, sizeof *p);
    if (c == NULL || job == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    riak_status st = RIAK_OK;
    if (riak_buf_append_str(&p->url, c->root) || riak_buf_append_str(&p->url, "mapred") ||
        (chunked && riak_url_append_query(&p->url, "chunked", "true")) ||
        (p->body = riak_strdup(job)) == NULL) {
        st = riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    } else {
        st = riak_add_header(c, &p->headers, "Content-Type", NULL, "application/json");
    }
    if (st != RIAK_OK) {
        riak_prepared_free(p);
        return st;
    }
    p->req = (riak_request){.method = "POST",
                            .url = p->url.data,
                            .headers = p->headers,
                            .body = p->body,
                            .body_len = strlen(p->body)};
    return RIAK_OK;
}

riak_status riak_mapred_http_finish(riak_client *c, riak_response *resp, char **result) {
    *result = NULL;
    if (resp->status != 200) {
        return riak_http_error(c, resp);
    }
    if ((*result = riak_buf_steal(&resp->body, NULL)) == NULL) {
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    return RIAK_OK;
}

riak_status riak_mapred(riak_client *client, const char *job, char **result) {
    if (result) {
        *result = NULL;
    }
    if (result == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    if (client && job && riak_is_pb(client)) {
        return riak_pb_mapred(client, job, NULL, NULL, result);
    }
    riak_prepared p;
    riak_status st = riak_mapred_http_prepare(client, job, false, &p);
    if (st != RIAK_OK) {
        return st;
    }
    riak_response resp = {0};
    st = riak_perform(client, &p.req, &resp);
    riak_prepared_free(&p);
    if (st == RIAK_OK) {
        st = riak_mapred_http_finish(client, &resp, result);
    }
    riak_response_free(&resp);
    return st;
}

bool riak_mapred_chunk(void *ctx, const char *json, size_t len, riak_status *err) {
    riak_mapred_stream_ctx *m = ctx;
    unsigned phase;
    char *data;
    *err = riak_parse_mapred_chunk(json, len, &phase, &data);
    if (*err != RIAK_OK) {
        return false;
    }
    bool more = m->cb(phase, data, m->ctx);
    free(data);
    return more;
}

riak_status riak_mapred_stream(riak_client *client, const char *job, riak_mapred_cb cb, void *ctx) {
    if (cb == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    if (client && job && riak_is_pb(client)) {
        return riak_pb_mapred(client, job, cb, ctx, NULL);
    }
    riak_prepared p;
    riak_status st = riak_mapred_http_prepare(client, job, true, &p);
    if (st == RIAK_OK) {
        riak_mapred_stream_ctx m = {.cb = cb, .ctx = ctx};
        st = perform_stream(client, &p.req, riak_mapred_chunk, &m);
    }
    riak_prepared_free(&p);
    return st;
}
