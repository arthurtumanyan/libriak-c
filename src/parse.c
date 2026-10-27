/*
 File:   parse.c
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "parse.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "util.h"

struct riak_keystream {
    json_tokener *tok;
    riak_strlist list;
    size_t cap;
    bool pending; /* an object has started but not finished */
};

/* Every string of obj[field] is appended to out. */
static riak_status collect(json_object *obj, const char *field, riak_strlist *out, size_t *cap) {
    json_object *arr;
    if (!json_object_is_type(obj, json_type_object) ||
        !json_object_object_get_ex(obj, field, &arr) ||
        !json_object_is_type(arr, json_type_array)) {
        return RIAK_E_PARSE;
    }
    size_t n = json_object_array_length(arr);
    for (size_t i = 0; i < n; i++) {
        json_object *item = json_object_array_get_idx(arr, i);
        if (!json_object_is_type(item, json_type_string)) {
            return RIAK_E_PARSE;
        }
        const char *s = json_object_get_string(item);
        size_t len = (size_t)json_object_get_string_len(item);
        if (riak_strlist_push(out, cap, s, len) != 0) {
            return RIAK_E_NOMEM;
        }
    }
    return RIAK_OK;
}

static riak_status ensure_terminated(riak_strlist *out, size_t *cap) {
    if (out->items != NULL) {
        return RIAK_OK;
    }
    out->items = (char **)calloc(1, sizeof(char *));
    *cap = 1;
    return out->items ? RIAK_OK : RIAK_E_NOMEM;
}

riak_status riak_parse_string_array(const char *json, size_t len, const char *field,
                                    riak_strlist *out) {
    memset(out, 0, sizeof *out);
    if (len > INT_MAX) {
        return RIAK_E_PARSE;
    }
    json_tokener *tok = json_tokener_new();
    if (tok == NULL) {
        return RIAK_E_NOMEM;
    }
    json_object *obj = json_tokener_parse_ex(tok, json, (int)len);
    riak_status st = RIAK_E_PARSE;
    size_t cap = 0;
    if (obj != NULL && json_tokener_get_error(tok) == json_tokener_success) {
        st = collect(obj, field, out, &cap);
        if (st == RIAK_OK) {
            st = ensure_terminated(out, &cap);
        }
    }
    json_object_put(obj);
    json_tokener_free(tok);
    if (st != RIAK_OK) {
        riak_strlist_free(out);
    }
    return st;
}

riak_keystream *riak_keystream_new(void) {
    riak_keystream *ks = calloc(1, sizeof *ks);
    if (ks == NULL) {
        return NULL;
    }
    ks->tok = json_tokener_new();
    if (ks->tok == NULL) {
        free(ks);
        return NULL;
    }
    return ks;
}

static bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

riak_status riak_keystream_feed(riak_keystream *ks, const char *data, size_t len) {
    while (len > 0) {
        if (!ks->pending) {
            while (len > 0 && is_ws(*data)) {
                data++;
                len--;
            }
            if (len == 0) {
                break;
            }
        }
        int slice = len > INT_MAX ? INT_MAX : (int)len;
        json_object *obj = json_tokener_parse_ex(ks->tok, data, slice);
        enum json_tokener_error err = json_tokener_get_error(ks->tok);
        if (err == json_tokener_continue) {
            ks->pending = true;
            data += slice;
            len -= (size_t)slice;
            continue;
        }
        if (err != json_tokener_success || obj == NULL) {
            json_object_put(obj);
            return RIAK_E_PARSE;
        }
        size_t used = json_tokener_get_parse_end(ks->tok);
        json_tokener_reset(ks->tok);
        ks->pending = false;
        riak_status st = collect(obj, "keys", &ks->list, &ks->cap);
        json_object_put(obj);
        if (st != RIAK_OK) {
            return st;
        }
        data += used;
        len -= used;
    }
    return RIAK_OK;
}

riak_status riak_keystream_finish(riak_keystream *ks, riak_strlist *out) {
    if (ks->pending) {
        return RIAK_E_PARSE;
    }
    riak_status st = ensure_terminated(&ks->list, &ks->cap);
    if (st != RIAK_OK) {
        return st;
    }
    *out = ks->list;
    memset(&ks->list, 0, sizeof ks->list);
    ks->cap = 0;
    return RIAK_OK;
}

void riak_keystream_free(riak_keystream *ks) {
    if (ks == NULL) {
        return;
    }
    json_tokener_free(ks->tok);
    riak_strlist_free(&ks->list);
    free(ks);
}

/* A complete JSON document is parsed; NULL on syntax error or ENOMEM. */
static json_object *parse_doc(const char *json, size_t len, riak_status *st) {
    *st = RIAK_E_PARSE;
    if (len > INT_MAX) {
        return NULL;
    }
    json_tokener *tok = json_tokener_new();
    if (tok == NULL) {
        *st = RIAK_E_NOMEM;
        return NULL;
    }
    json_object *obj = json_tokener_parse_ex(tok, json, (int)len);
    if (obj != NULL && json_tokener_get_error(tok) != json_tokener_success) {
        json_object_put(obj);
        obj = NULL;
    }
    json_tokener_free(tok);
    if (obj != NULL) {
        *st = RIAK_OK;
    }
    return obj;
}

riak_status riak_parse_index_json(const char *json, size_t len, riak_index_visit_fn visit,
                                  void *ctx, char **continuation, bool *stopped) {
    *stopped = false;
    riak_status st;
    json_object *obj = parse_doc(json, len, &st);
    if (obj == NULL) {
        return st;
    }
    st = RIAK_E_PARSE;
    if (!json_object_is_type(obj, json_type_object)) {
        goto done;
    }
    json_object *v;
    if (json_object_object_get_ex(obj, "continuation", &v)) {
        if (!json_object_is_type(v, json_type_string)) {
            goto done;
        }
        char *c = riak_strdup(json_object_get_string(v));
        if (c == NULL) {
            st = RIAK_E_NOMEM;
            goto done;
        }
        free(*continuation);
        *continuation = c;
    }
    json_object *keys = NULL, *results = NULL;
    bool has_keys = json_object_object_get_ex(obj, "keys", &keys);
    bool has_results = json_object_object_get_ex(obj, "results", &results);
    if ((has_keys && !json_object_is_type(keys, json_type_array)) ||
        (has_results && !json_object_is_type(results, json_type_array))) {
        goto done;
    }
    if (has_keys) {
        size_t n = json_object_array_length(keys);
        for (size_t i = 0; i < n; i++) {
            json_object *k = json_object_array_get_idx(keys, i);
            if (!json_object_is_type(k, json_type_string)) {
                goto done;
            }
            if (!visit(json_object_get_string(k), NULL, ctx)) {
                *stopped = true;
                st = RIAK_OK;
                goto done;
            }
        }
    }
    if (has_results) {
        size_t n = json_object_array_length(results);
        for (size_t i = 0; i < n; i++) {
            json_object *r = json_object_array_get_idx(results, i);
            if (!json_object_is_type(r, json_type_object) || json_object_object_length(r) != 1) {
                goto done;
            }
            json_object_object_foreach(r, term, key) {
                if (!json_object_is_type(key, json_type_string)) {
                    goto done;
                }
                if (!visit(json_object_get_string(key), term, ctx)) {
                    *stopped = true;
                    st = RIAK_OK;
                    goto done;
                }
            }
        }
    }
    st = RIAK_OK;
done:
    json_object_put(obj);
    return st;
}

static int push_field(riak_search_doc *doc, size_t *cap, const char *name, json_object *v) {
    if (doc->n_fields == *cap) {
        size_t ncap = *cap ? *cap * 2 : 8;
        riak_header *grown = realloc(doc->fields, ncap * sizeof *grown);
        if (grown == NULL) {
            return -1;
        }
        doc->fields = grown;
        *cap = ncap;
    }
    riak_header *f = &doc->fields[doc->n_fields];
    f->name = riak_strdup(name);
    f->value = riak_strdup(json_object_get_string(v)); /* JSON text for non-strings */
    doc->n_fields++;
    return f->name && f->value ? 0 : -1;
}

static int set_string(char **dst, json_object *v) {
    if (!json_object_is_type(v, json_type_string)) {
        return 0;
    }
    *dst = riak_strdup(json_object_get_string(v));
    return *dst ? 0 : -1;
}

static riak_status parse_doc_fields(json_object *jdoc, riak_search_doc *doc) {
    if (!json_object_is_type(jdoc, json_type_object)) {
        return RIAK_E_PARSE;
    }
    size_t cap = 0;
    json_object_object_foreach(jdoc, name, v) {
        int rc = 0;
        if (strcmp(name, "score") == 0) {
            doc->score = json_object_get_double(v);
            continue;
        }
        if (strcmp(name, "_yz_rk") == 0) {
            rc = set_string(&doc->key, v);
        } else if (strcmp(name, "_yz_rb") == 0) {
            rc = set_string(&doc->bucket, v);
        } else if (strcmp(name, "_yz_rt") == 0) {
            rc = set_string(&doc->bucket_type, v);
        }
        if (rc == 0 && json_object_is_type(v, json_type_array)) {
            size_t n = json_object_array_length(v);
            for (size_t i = 0; rc == 0 && i < n; i++) {
                rc = push_field(doc, &cap, name, json_object_array_get_idx(v, i));
            }
        } else if (rc == 0 && v != NULL) {
            rc = push_field(doc, &cap, name, v);
        }
        if (rc != 0) {
            return RIAK_E_NOMEM;
        }
    }
    return RIAK_OK;
}

riak_status riak_parse_search(const char *json, size_t len, riak_search_result *out) {
    riak_status st;
    json_object *obj = parse_doc(json, len, &st);
    if (obj == NULL) {
        return st;
    }
    st = RIAK_E_PARSE;
    json_object *resp, *docs, *v;
    if (!json_object_is_type(obj, json_type_object) ||
        !json_object_object_get_ex(obj, "response", &resp) ||
        !json_object_is_type(resp, json_type_object) ||
        !json_object_object_get_ex(resp, "docs", &docs) ||
        !json_object_is_type(docs, json_type_array)) {
        goto done;
    }
    if (json_object_object_get_ex(resp, "numFound", &v)) {
        int64_t n = json_object_get_int64(v);
        out->num_found = n > 0 ? (size_t)n : 0;
    }
    if (json_object_object_get_ex(resp, "maxScore", &v)) {
        out->max_score = json_object_get_double(v);
    }
    size_t n = json_object_array_length(docs);
    if (n > 0) {
        out->docs = calloc(n, sizeof *out->docs);
        if (out->docs == NULL) {
            st = RIAK_E_NOMEM;
            goto done;
        }
    }
    for (size_t i = 0; i < n; i++) {
        out->count = i + 1;
        if ((st = parse_doc_fields(json_object_array_get_idx(docs, i), &out->docs[i])) != RIAK_OK) {
            goto done;
        }
    }
    st = RIAK_OK;
done:
    json_object_put(obj);
    return st;
}

riak_status riak_parse_mapred_chunk(const char *json, size_t len, unsigned *phase, char **data) {
    *data = NULL;
    riak_status st;
    json_object *obj = parse_doc(json, len, &st);
    if (obj == NULL) {
        return st;
    }
    st = RIAK_E_PARSE;
    json_object *p, *d;
    if (json_object_is_type(obj, json_type_object) && json_object_object_get_ex(obj, "phase", &p) &&
        json_object_is_type(p, json_type_int) && json_object_get_int64(p) >= 0 &&
        json_object_get_int64(p) <= UINT32_MAX && json_object_object_get_ex(obj, "data", &d)) {
        *phase = (unsigned)json_object_get_int64(p);
        *data = riak_strdup(json_object_to_json_string_ext(d, JSON_C_TO_STRING_PLAIN));
        st = *data ? RIAK_OK : RIAK_E_NOMEM;
    }
    json_object_put(obj);
    return st;
}

void riak_search_result_free(riak_search_result *res) {
    if (res == NULL) {
        return;
    }
    for (size_t i = 0; i < res->count; i++) {
        riak_search_doc *d = &res->docs[i];
        for (size_t j = 0; j < d->n_fields; j++) {
            free(d->fields[j].name);
            free(d->fields[j].value);
        }
        free(d->fields);
        free(d->bucket_type);
        free(d->bucket);
        free(d->key);
    }
    free(res->docs);
    free(res->raw);
    memset(res, 0, sizeof *res);
}
