/*
 File:   pb_query.c
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later

 Protocol Buffers: secondary index queries (with coverage contexts), Riak
 Search queries and administration, MapReduce. Search results and index
 definitions are returned as the same JSON documents HTTP produces.
 */

#include "pb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "parse.h"

/* ---- secondary indexes --------------------------------------------- */

typedef struct {
    riak_index_cb cb;
    void *ctx;
    char **continuation;
} index_ctx;

static bool index_sink(void *ctx, const uint8_t *data, size_t len, bool *done, riak_status *err) {
    index_ctx *x = ctx;
    RpbIndexResp *r = rpb_index_resp__unpack(NULL, len, data);
    if (r == NULL) {
        *err = RIAK_E_PARSE;
        return false;
    }
    bool more = true;
    for (size_t i = 0; more && i < r->n_keys; i++) {
        char *key = riak_pb_strdup(r->keys[i]);
        if (key == NULL) {
            *err = RIAK_E_NOMEM;
            more = false;
            break;
        }
        more = x->cb(key, NULL, x->ctx);
        free(key);
    }
    /* results are term -> key pairs (return_terms) */
    for (size_t i = 0; more && i < r->n_results; i++) {
        char *term = riak_pb_strdup(r->results[i]->key);
        char *key = r->results[i]->has_value ? riak_pb_strdup(r->results[i]->value) : NULL;
        if (term == NULL || key == NULL) {
            free(term);
            free(key);
            *err = RIAK_E_NOMEM;
            more = false;
            break;
        }
        more = x->cb(key, term, x->ctx);
        free(term);
        free(key);
    }
    if (*err == RIAK_OK && r->has_continuation && x->continuation) {
        char *cont = riak_pb_strdup(r->continuation);
        if (cont == NULL) {
            *err = RIAK_E_NOMEM;
            more = false;
        } else {
            free(*x->continuation);
            *x->continuation = cont;
        }
    }
    *done = r->has_done && r->done;
    rpb_index_resp__free_unpacked(r, NULL);
    return more && *err == RIAK_OK;
}

riak_status riak_pb_index(riak_client *c, const char *bucket, const riak_index_query *q,
                          riak_index_cb cb, void *ctx, char **continuation) {
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
    RpbIndexReq m = RPB_INDEX_REQ__INIT;
    m.bucket = riak_pb_bin(bucket);
    m.index = riak_pb_bin(q->index);
    if (exact) {
        m.qtype = RPB_INDEX_REQ__INDEX_QUERY_TYPE__eq;
        m.has_key = 1;
        m.key = riak_pb_bin(q->value);
    } else {
        m.qtype = RPB_INDEX_REQ__INDEX_QUERY_TYPE__range;
        m.has_range_min = m.has_range_max = 1;
        m.range_min = riak_pb_bin(q->range_min);
        m.range_max = riak_pb_bin(q->range_max);
    }
    m.has_return_terms = q->return_terms;
    m.return_terms = q->return_terms;
    m.has_stream = 1;
    m.stream = 1;
    m.has_max_results = q->max_results > 0;
    m.max_results = q->max_results;
    if (q->continuation) {
        m.has_continuation = 1;
        m.continuation = riak_pb_bin(q->continuation);
    }
    m.has_timeout = q->server_timeout_ms > 0;
    m.timeout = q->server_timeout_ms;
    if (q->bucket_type) {
        m.has_type = 1;
        m.type = riak_pb_bin(q->bucket_type);
    }
    if (q->term_regex) {
        m.has_term_regex = 1;
        m.term_regex = riak_pb_bin(q->term_regex);
    }
    ProtobufCBinaryData cover = {0};
    if (q->cover_context) {
        if (!riak_pb_b64_decode(q->cover_context, &cover)) {
            return riak_set_error(c, RIAK_E_INVALID_ARG, "invalid cover_context");
        }
        m.has_cover_context = 1;
        m.cover_context = cover;
    }
    riak_buf path = {0};
    if (riak_url_bucket(&path, c, q->bucket_type, bucket, "index", NULL) != 0 ||
        riak_buf_append_str(&path, "/") != 0 || riak_url_append_escaped(&path, q->index) != 0) {
        free(cover.data);
        riak_buf_free(&path);
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    riak_pb_call call = {.code = RIAK_PB_INDEX_REQ,
                         .resp_code = RIAK_PB_INDEX_RESP,
                         .idempotent = true,
                         .name = "RpbIndexReq",
                         .path = path.data};
    index_ctx x = {.cb = cb, .ctx = ctx, .continuation = continuation};
    st = riak_pb_stream(c, &call, &m.base, index_sink, &x);
    free(cover.data);
    riak_buf_free(&path);
    return st;
}

/* ---- Riak Search ------------------------------------------------------ */

/* The response as the Solr JSON document HTTP returns. */
static char *search_json(const RpbSearchQueryResp *r) {
    json_object *root = json_object_new_object();
    json_object *resp = json_object_new_object();
    json_object *docs = json_object_new_array();
    if (root == NULL || resp == NULL || docs == NULL) {
        json_object_put(root);
        json_object_put(resp);
        json_object_put(docs);
        return NULL;
    }
    json_object_object_add(root, "response", resp);
    json_object_object_add(resp, "numFound", json_object_new_int64(r->num_found));
    json_object_object_add(resp, "start", json_object_new_int64(0));
    json_object_object_add(resp, "maxScore", json_object_new_double(r->max_score));
    json_object_object_add(resp, "docs", docs);
    for (size_t i = 0; i < r->n_docs; i++) {
        json_object *doc = json_object_new_object();
        if (doc == NULL || json_object_array_add(docs, doc) != 0) {
            json_object_put(doc);
            json_object_put(root);
            return NULL;
        }
        const RpbSearchDoc *d = r->docs[i];
        for (size_t j = 0; j < d->n_fields; j++) {
            char *name = riak_pb_strdup(d->fields[j]->key);
            if (name == NULL) {
                json_object_put(root);
                return NULL;
            }
            const ProtobufCBinaryData *v = &d->fields[j]->value;
            json_object *val;
            if (strcmp(name, "score") == 0) {
                char *s = riak_pb_strdup(*v);
                val = json_object_new_double(s ? strtod(s, NULL) : 0.0);
                free(s);
            } else {
                val =
                    json_object_new_string_len(d->fields[j]->has_value ? (const char *)v->data : "",
                                               d->fields[j]->has_value ? (int)v->len : 0);
            }
            json_object *prev;
            if (json_object_object_get_ex(doc, name, &prev)) {
                /* a multi-valued field becomes an array */
                if (!json_object_is_type(prev, json_type_array)) {
                    json_object *arr = json_object_new_array();
                    json_object_array_add(arr, json_object_get(prev));
                    json_object_object_add(doc, name, arr);
                    prev = arr;
                }
                json_object_array_add(prev, val);
            } else {
                json_object_object_add(doc, name, val);
            }
            free(name);
        }
    }
    char *s = riak_strdup(json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN));
    json_object_put(root);
    return s;
}

riak_status riak_pb_search(riak_client *c, const char *index, const riak_search_query *q,
                           riak_search_result *out) {
    RpbSearchQueryReq m = RPB_SEARCH_QUERY_REQ__INIT;
    m.q = riak_pb_bin(q->q);
    m.index = riak_pb_bin(index);
    m.has_rows = q->rows > 0;
    m.rows = q->rows;
    m.has_start = q->start > 0;
    m.start = q->start;
#define OPT(f, v)                                                                                  \
    if (v) {                                                                                       \
        m.has_##f = 1;                                                                             \
        m.f = riak_pb_bin(v);                                                                      \
    }
    OPT(sort, q->sort);
    OPT(filter, q->filter);
    OPT(df, q->default_field);
    OPT(op, q->op);
#undef OPT
    /* fl: a comma separated list becomes repeated fields */
    char *fields = q->fields ? riak_strdup(q->fields) : NULL;
    ProtobufCBinaryData fl[64];
    if (q->fields && fields == NULL) {
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    for (char *save = NULL, *tok = fields ? strtok_r(fields, ", ", &save) : NULL; tok;
         tok = strtok_r(NULL, ", ", &save)) {
        if (m.n_fl == sizeof fl / sizeof fl[0]) {
            free(fields);
            return riak_set_error(c, RIAK_E_INVALID_ARG, "too many fields");
        }
        fl[m.n_fl++] = riak_pb_bin(tok);
    }
    m.fl = m.n_fl ? fl : NULL;
    char *path = riak_strprintf("/search/query/%s", index);
    if (path == NULL) {
        free(fields);
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    riak_pb_call call = {.code = RIAK_PB_SEARCH_QUERY_REQ,
                         .resp_code = RIAK_PB_SEARCH_QUERY_RESP,
                         .idempotent = true,
                         .name = "RpbSearchQueryReq",
                         .path = path};
    ProtobufCMessage *msg;
    riak_status st = riak_pb_exec(c, &call, &m.base, &rpb_search_query_resp__descriptor, &msg);
    free(path);
    free(fields);
    if (st != RIAK_OK) {
        return st;
    }
    char *raw = search_json((RpbSearchQueryResp *)msg);
    rpb_search_query_resp__free_unpacked((RpbSearchQueryResp *)msg, NULL);
    if (raw == NULL) {
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    st = riak_parse_search(raw, strlen(raw), out);
    out->raw = raw;
    if (st != RIAK_OK) {
        riak_search_result_free(out);
        return riak_set_error(c, st, "cannot parse search response");
    }
    return RIAK_OK;
}

/* ---- Search administration ------------------------------------------ */

static riak_status admin_call(riak_client *c, uint8_t code, uint8_t resp_code, const char *name,
                              const ProtobufCMessage *m, const char *path,
                              const ProtobufCMessageDescriptor *desc, ProtobufCMessage **out) {
    riak_pb_call call = {
        .code = code, .resp_code = resp_code, .idempotent = true, .name = name, .path = path};
    return riak_pb_exec(c, &call, m, desc, out);
}

riak_status riak_pb_search_index_create(riak_client *c, const char *index, const char *schema,
                                        unsigned n_val) {
    RpbYokozunaIndex idx = RPB_YOKOZUNA_INDEX__INIT;
    idx.name = riak_pb_bin(index);
    if (schema) {
        idx.has_schema = 1;
        idx.schema = riak_pb_bin(schema);
    }
    idx.has_n_val = n_val > 0;
    idx.n_val = n_val;
    RpbYokozunaIndexPutReq m = RPB_YOKOZUNA_INDEX_PUT_REQ__INIT;
    m.index = &idx;
    return admin_call(c, RIAK_PB_YZ_INDEX_PUT_REQ, RIAK_PB_PUT_RESP, "RpbYokozunaIndexPutReq",
                      &m.base, "/search/index", NULL, NULL);
}

static json_object *index_json(const RpbYokozunaIndex *i) {
    json_object *o = json_object_new_object();
    if (o == NULL) return NULL;
    json_object_object_add(
        o, "name", json_object_new_string_len((const char *)i->name.data, (int)i->name.len));
    if (i->has_n_val) json_object_object_add(o, "n_val", json_object_new_int64(i->n_val));
    if (i->has_schema)
        json_object_object_add(
            o, "schema",
            json_object_new_string_len((const char *)i->schema.data, (int)i->schema.len));
    return o;
}

riak_status riak_pb_search_index_get(riak_client *c, const char *index, char **json) {
    RpbYokozunaIndexGetReq m = RPB_YOKOZUNA_INDEX_GET_REQ__INIT;
    if (index) {
        m.has_name = 1;
        m.name = riak_pb_bin(index);
    }
    ProtobufCMessage *msg;
    riak_status st =
        admin_call(c, RIAK_PB_YZ_INDEX_GET_REQ, RIAK_PB_YZ_INDEX_GET_RESP, "RpbYokozunaIndexGetReq",
                   &m.base, "/search/index", &rpb_yokozuna_index_get_resp__descriptor, &msg);
    if (st != RIAK_OK) {
        return st;
    }
    RpbYokozunaIndexGetResp *r = (RpbYokozunaIndexGetResp *)msg;
    json_object *doc;
    if (index) {
        doc = r->n_index > 0 ? index_json(r->index[0]) : NULL;
        if (r->n_index == 0) {
            rpb_yokozuna_index_get_resp__free_unpacked(r, NULL);
            return riak_set_error(c, RIAK_E_NOT_FOUND, "search index not found");
        }
    } else {
        doc = json_object_new_array();
        for (size_t i = 0; doc && i < r->n_index; i++) {
            json_object_array_add(doc, index_json(r->index[i]));
        }
    }
    rpb_yokozuna_index_get_resp__free_unpacked(r, NULL);
    *json = doc ? riak_strdup(json_object_to_json_string_ext(doc, JSON_C_TO_STRING_PLAIN)) : NULL;
    json_object_put(doc);
    return *json ? RIAK_OK : riak_set_error(c, RIAK_E_NOMEM, "out of memory");
}

riak_status riak_pb_search_index_delete(riak_client *c, const char *index) {
    RpbYokozunaIndexDeleteReq m = RPB_YOKOZUNA_INDEX_DELETE_REQ__INIT;
    m.name = riak_pb_bin(index);
    return admin_call(c, RIAK_PB_YZ_INDEX_DELETE_REQ, RIAK_PB_DEL_RESP, "RpbYokozunaIndexDeleteReq",
                      &m.base, "/search/index", NULL, NULL);
}

riak_status riak_pb_search_schema_put(riak_client *c, const char *name, const char *xml) {
    RpbYokozunaSchema schema = RPB_YOKOZUNA_SCHEMA__INIT;
    schema.name = riak_pb_bin(name);
    schema.has_content = 1;
    schema.content = riak_pb_bin(xml);
    RpbYokozunaSchemaPutReq m = RPB_YOKOZUNA_SCHEMA_PUT_REQ__INIT;
    m.schema = &schema;
    return admin_call(c, RIAK_PB_YZ_SCHEMA_PUT_REQ, RIAK_PB_PUT_RESP, "RpbYokozunaSchemaPutReq",
                      &m.base, "/search/schema", NULL, NULL);
}

riak_status riak_pb_search_schema_get(riak_client *c, const char *name, char **xml) {
    RpbYokozunaSchemaGetReq m = RPB_YOKOZUNA_SCHEMA_GET_REQ__INIT;
    m.name = riak_pb_bin(name);
    ProtobufCMessage *msg;
    riak_status st = admin_call(c, RIAK_PB_YZ_SCHEMA_GET_REQ, RIAK_PB_YZ_SCHEMA_GET_RESP,
                                "RpbYokozunaSchemaGetReq", &m.base, "/search/schema",
                                &rpb_yokozuna_schema_get_resp__descriptor, &msg);
    if (st != RIAK_OK) {
        return st;
    }
    RpbYokozunaSchemaGetResp *r = (RpbYokozunaSchemaGetResp *)msg;
    *xml = r->schema->has_content ? riak_pb_strdup(r->schema->content) : riak_strdup("");
    rpb_yokozuna_schema_get_resp__free_unpacked(r, NULL);
    return *xml ? RIAK_OK : riak_set_error(c, RIAK_E_NOMEM, "out of memory");
}

/* ---- MapReduce ------------------------------------------------------- */

typedef struct {
    riak_mapred_cb cb; /* streaming */
    void *ctx;
    json_object **phases; /* collecting: results per phase */
    unsigned n_phases;
} mapred_ctx;

static bool mapred_sink(void *ctx, const uint8_t *data, size_t len, bool *done, riak_status *err) {
    mapred_ctx *x = ctx;
    RpbMapRedResp *r = rpb_map_red_resp__unpack(NULL, len, data);
    if (r == NULL) {
        *err = RIAK_E_PARSE;
        return false;
    }
    bool more = true;
    if (r->has_response) {
        char *json = riak_pb_strdup(r->response);
        unsigned phase = r->has_phase ? r->phase : 0;
        if (json == NULL) {
            *err = RIAK_E_NOMEM;
            more = false;
        } else if (x->cb) {
            more = x->cb(phase, json, x->ctx);
        } else {
            json_object *items = json_tokener_parse(json);
            if (items == NULL || !json_object_is_type(items, json_type_array) || phase > 1024) {
                json_object_put(items);
                *err = RIAK_E_PARSE;
                more = false;
            } else {
                if (phase >= x->n_phases) {
                    json_object **grown =
                        (json_object **)realloc((void *)x->phases, (phase + 1) * sizeof *grown);
                    if (grown == NULL) {
                        json_object_put(items);
                        free(json);
                        *err = RIAK_E_NOMEM;
                        rpb_map_red_resp__free_unpacked(r, NULL);
                        return false;
                    }
                    for (unsigned i = x->n_phases; i <= phase; i++) {
                        grown[i] = NULL;
                    }
                    x->phases = grown;
                    x->n_phases = phase + 1;
                }
                if (x->phases[phase] == NULL) {
                    x->phases[phase] = items;
                } else {
                    size_t n = json_object_array_length(items);
                    for (size_t i = 0; i < n; i++) {
                        json_object_array_add(x->phases[phase],
                                              json_object_get(json_object_array_get_idx(items, i)));
                    }
                    json_object_put(items);
                }
            }
        }
        free(json);
    }
    *done = r->has_done && r->done;
    rpb_map_red_resp__free_unpacked(r, NULL);
    return more;
}

riak_status riak_pb_mapred(riak_client *c, const char *job, riak_mapred_cb cb, void *ctx,
                           char **result) {
    RpbMapRedReq m = RPB_MAP_RED_REQ__INIT;
    m.request = riak_pb_bin(job);
    m.content_type = riak_pb_bin("application/json");
    riak_pb_call call = {.code = RIAK_PB_MAPRED_REQ,
                         .resp_code = RIAK_PB_MAPRED_RESP,
                         .idempotent = false, /* POST over HTTP: not repeated */
                         .name = "RpbMapRedReq",
                         .path = "/mapred"};
    mapred_ctx x = {.cb = cb, .ctx = ctx};
    riak_status st = riak_pb_stream(c, &call, &m.base, mapred_sink, &x);
    if (st == RIAK_OK && result) {
        /* one phase: its results; several: one list per phase, as HTTP returns */
        json_object *doc;
        size_t used = 0;
        unsigned last = 0;
        for (unsigned i = 0; i < x.n_phases; i++) {
            if (x.phases[i]) {
                used++;
                last = i;
            }
        }
        if (used == 1) {
            doc = json_object_get(x.phases[last]);
        } else {
            doc = json_object_new_array();
            for (unsigned i = 0; doc && i < x.n_phases; i++) {
                if (x.phases[i]) json_object_array_add(doc, json_object_get(x.phases[i]));
            }
        }
        *result =
            doc ? riak_strdup(json_object_to_json_string_ext(doc, JSON_C_TO_STRING_PLAIN)) : NULL;
        json_object_put(doc);
        if (*result == NULL) {
            st = riak_set_error(c, RIAK_E_NOMEM, "out of memory");
        }
    }
    for (unsigned i = 0; i < x.n_phases; i++) {
        json_object_put(x.phases[i]);
    }
    free((void *)x.phases);
    return st;
}
