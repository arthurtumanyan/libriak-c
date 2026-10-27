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

/* ---- streamed multipart responses ---------------------------------- */

typedef bool (*chunk_fn)(void *ctx, const char *json, size_t len, riak_status *err);

/* Sink that splits a 200 multipart/mixed body into JSON parts as it arrives. */
typedef struct {
    riak_client *c;
    riak_response *resp;
    riak_mpstream *mp;
    chunk_fn fn;
    void *ctx;
    bool stopped; /* fn asked to stop: ignore the rest */
} stream_ctx;

static bool on_part(void *ctx, riak_part *part, riak_status *err) {
    stream_ctx *s = ctx;
    if (!s->fn(s->ctx, part->body, part->len, err)) {
        if (*err == RIAK_OK) {
            s->stopped = true; /* caller is done: abort the transfer */
            *err = RIAK_E_PARSE;
        }
        return false;
    }
    return true;
}

static bool stream_sink(void *ctx, const char *data, size_t len, riak_status *err) {
    stream_ctx *s = ctx;
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

/* A streamed request is performed; parts are passed to fn. */
static riak_status perform_stream(riak_client *c, const riak_request *base, chunk_fn fn,
                                  void *ctx) {
    riak_response resp = {0};
    stream_ctx s = {.c = c, .resp = &resp, .fn = fn, .ctx = ctx};
    riak_request req = *base;
    req.sink = stream_sink;
    req.sink_ctx = &s;
    req.unbounded = true;
    riak_status st = riak_perform(c, &req, &resp);
    if (s.stopped) {
        riak_clear_error(c); /* the abort was requested, not an error */
        st = RIAK_OK;
    } else if (st == RIAK_OK) {
        if (resp.status != 200) {
            st = riak_http_error(c, &resp);
        } else if (s.mp == NULL || riak_mpstream_finish(s.mp) != RIAK_OK) {
            st = riak_set_error(c, RIAK_E_PARSE, "truncated multipart stream");
        }
    }
    riak_mpstream_free(s.mp);
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
    if (q->server_timeout_ms) {
        snprintf(num, sizeof num, "%u", q->server_timeout_ms);
        rc = rc || riak_url_append_query(url, "timeout", num);
    }
    if (stream) {
        rc = rc || riak_url_append_query(url, "stream", "true");
    }
    return rc ? riak_set_error(c, RIAK_E_NOMEM, "out of memory") : RIAK_OK;
}

typedef struct {
    riak_strlist keys, terms;
    size_t kcap, tcap;
    bool want_terms;
    bool nomem;
} index_acc;

static bool accumulate(const char *key, const char *term, void *ctx) {
    index_acc *a = ctx;
    if (riak_strlist_push(&a->keys, &a->kcap, key, strlen(key)) != 0 ||
        (a->want_terms &&
         riak_strlist_push(&a->terms, &a->tcap, term ? term : "", term ? strlen(term) : 0) != 0)) {
        a->nomem = true;
        return false;
    }
    return true;
}

riak_status riak_index_query_run(riak_client *client, const char *bucket, const riak_index_query *q,
                                 riak_index_result *out) {
    if (client == NULL || out == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    riak_buf url = {0};
    riak_status st = index_url(client, bucket, q, false, &url);
    if (st != RIAK_OK) {
        riak_buf_free(&url);
        return st;
    }
    riak_request req = {.method = "GET", .url = url.data};
    riak_response resp = {0};
    st = riak_perform(client, &req, &resp);
    riak_buf_free(&url);
    if (st != RIAK_OK) {
        return st;
    }
    if (resp.status != 200) {
        st = riak_http_error(client, &resp);
    } else {
        index_acc acc = {.want_terms = q->return_terms};
        bool stopped;
        st = riak_parse_index_json(resp.body.data ? resp.body.data : "", resp.body.len, accumulate,
                                   &acc, &out->continuation, &stopped);
        if (st == RIAK_OK && acc.nomem) {
            st = RIAK_E_NOMEM;
        }
        if (st == RIAK_OK && acc.keys.items == NULL) {
            acc.keys.items = (char **)calloc(1, sizeof(char *));
            st = acc.keys.items ? RIAK_OK : RIAK_E_NOMEM;
        }
        if (st == RIAK_OK && acc.want_terms && acc.terms.items == NULL) {
            acc.terms.items = (char **)calloc(1, sizeof(char *));
            st = acc.terms.items ? RIAK_OK : RIAK_E_NOMEM;
        }
        if (st == RIAK_OK) {
            out->keys = acc.keys.items;
            out->terms = acc.terms.items;
            out->count = acc.keys.count;
        } else {
            riak_strlist_free(&acc.keys);
            riak_strlist_free(&acc.terms);
        }
        if (st != RIAK_OK) {
            riak_set_error(client, st, "cannot parse index response");
        }
    }
    riak_response_free(&resp);
    if (st != RIAK_OK) {
        riak_index_result_free(out);
    }
    return st;
}

typedef struct {
    riak_index_cb cb;
    void *ctx;
    char **continuation;
} index_stream;

static bool forward(const char *key, const char *term, void *ctx) {
    index_stream *s = ctx;
    return s->cb(key, term, s->ctx);
}

static bool index_chunk(void *ctx, const char *json, size_t len, riak_status *err) {
    index_stream *s = ctx;
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
    riak_buf url = {0};
    riak_status st = index_url(client, bucket, q, true, &url);
    if (st == RIAK_OK) {
        index_stream s = {.cb = cb, .ctx = ctx, .continuation = continuation};
        riak_request req = {.method = "GET", .url = url.data};
        st = perform_stream(client, &req, index_chunk, &s);
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
    riak_buf url = {0};
    char num[16];
    int rc = riak_buf_append_str(&url, client->root) ||
             riak_buf_append_str(&url, "search/query/") || riak_url_append_escaped(&url, index) ||
             riak_url_append_query(&url, "wt", "json") || riak_url_append_query(&url, "q", q->q) ||
             search_param(&url, "fq", q->filter) || search_param(&url, "sort", q->sort) ||
             search_param(&url, "fl", q->fields) || search_param(&url, "df", q->default_field) ||
             search_param(&url, "q.op", q->op);
    if (q->start) {
        snprintf(num, sizeof num, "%u", q->start);
        rc = rc || riak_url_append_query(&url, "start", num);
    }
    if (q->rows) {
        snprintf(num, sizeof num, "%u", q->rows);
        rc = rc || riak_url_append_query(&url, "rows", num);
    }
    if (rc) {
        riak_buf_free(&url);
        return riak_set_error(client, RIAK_E_NOMEM, "out of memory");
    }
    char *raw = NULL;
    st = riak_get_body(client, url.data, RIAK_BODY_JSON_OBJECT, &raw);
    riak_buf_free(&url);
    if (st != RIAK_OK) {
        return st;
    }
    st = riak_parse_search(raw, strlen(raw), out);
    out->raw = raw;
    if (st != RIAK_OK) {
        riak_search_result_free(out);
        return riak_set_error(client, st, "cannot parse search response");
    }
    return RIAK_OK;
}

static int search_admin_url(riak_buf *url, const riak_client *c, const char *what,
                            const char *name) {
    return riak_buf_append_str(url, c->root) || riak_buf_append_str(url, "search/") ||
           riak_buf_append_str(url, what) ||
           (name && (riak_buf_append_str(url, "/") || riak_url_append_escaped(url, name)));
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
    return st;
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
    return st;
}

riak_status riak_search_index_get(riak_client *client, const char *index, char **json) {
    riak_status st = client ? riak_check_name(client, index, "index") : RIAK_E_INVALID_ARG;
    return st == RIAK_OK ? admin_get(client, "index", index, RIAK_BODY_JSON_OBJECT, json) : st;
}

riak_status riak_search_index_list(riak_client *client, char **json) {
    return admin_get(client, "index", NULL, RIAK_BODY_JSON_ARRAY, json);
}

riak_status riak_search_index_delete(riak_client *client, const char *index) {
    riak_status st = client ? riak_check_name(client, index, "index") : RIAK_E_INVALID_ARG;
    return st == RIAK_OK ? admin_write(client, "DELETE", "index", index, NULL, NULL) : st;
}

riak_status riak_search_schema_put(riak_client *client, const char *name, const char *xml) {
    if (client == NULL || xml == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    riak_status st = riak_check_name(client, name, "schema");
    return st == RIAK_OK ? admin_write(client, "PUT", "schema", name, "application/xml", xml) : st;
}

riak_status riak_search_schema_get(riak_client *client, const char *name, char **xml) {
    riak_status st = client ? riak_check_name(client, name, "schema") : RIAK_E_INVALID_ARG;
    return st == RIAK_OK ? admin_get(client, "schema", name, RIAK_BODY_RAW, xml) : st;
}

/* ---- MapReduce ----------------------------------------------------- */

static riak_status mapred_request(riak_client *c, const char *job, bool chunked, riak_buf *url,
                                  struct curl_slist **headers) {
    if (c == NULL || job == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    if (riak_buf_append_str(url, c->root) || riak_buf_append_str(url, "mapred") ||
        (chunked && riak_url_append_query(url, "chunked", "true"))) {
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    return riak_add_header(c, headers, "Content-Type", NULL, "application/json");
}

riak_status riak_mapred(riak_client *client, const char *job, char **result) {
    if (result) {
        *result = NULL;
    }
    if (result == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    riak_buf url = {0};
    struct curl_slist *headers = NULL;
    riak_response resp = {0};
    riak_status st = mapred_request(client, job, false, &url, &headers);
    if (st == RIAK_OK) {
        riak_request req = {.method = "POST",
                            .url = url.data,
                            .headers = headers,
                            .body = job,
                            .body_len = strlen(job)};
        st = riak_perform(client, &req, &resp);
    }
    if (st == RIAK_OK) {
        if (resp.status != 200) {
            st = riak_http_error(client, &resp);
        } else if ((*result = riak_buf_steal(&resp.body, NULL)) == NULL) {
            st = riak_set_error(client, RIAK_E_NOMEM, "out of memory");
        }
    }
    riak_response_free(&resp);
    curl_slist_free_all(headers);
    riak_buf_free(&url);
    return st;
}

typedef struct {
    riak_mapred_cb cb;
    void *ctx;
} mapred_stream;

static bool mapred_chunk(void *ctx, const char *json, size_t len, riak_status *err) {
    mapred_stream *m = ctx;
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
    riak_buf url = {0};
    struct curl_slist *headers = NULL;
    riak_status st = mapred_request(client, job, true, &url, &headers);
    if (st == RIAK_OK) {
        mapred_stream m = {.cb = cb, .ctx = ctx};
        riak_request req = {.method = "POST",
                            .url = url.data,
                            .headers = headers,
                            .body = job,
                            .body_len = strlen(job)};
        st = perform_stream(client, &req, mapred_chunk, &m);
    }
    curl_slist_free_all(headers);
    riak_buf_free(&url);
    return st;
}
