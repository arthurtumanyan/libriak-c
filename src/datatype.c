/*
 File:   datatype.c
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later

 Riak data types (CRDTs): counters, sets, maps, HyperLogLogs, grow-only
 sets, and registers/flags inside maps.
 */

#include "datatype.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "internal.h"
#include "pb.h"
#include "util.h"

#define MAX_DEPTH RIAK_DT_MAX_DEPTH

/* ---- names --------------------------------------------------------- */

static const char *const type_names[] = {
    [RIAK_DT_COUNTER] = "counter", [RIAK_DT_SET] = "set",   [RIAK_DT_MAP] = "map",
    [RIAK_DT_HLL] = "hll",         [RIAK_DT_GSET] = "gset", [RIAK_DT_REGISTER] = "register",
    [RIAK_DT_FLAG] = "flag",
};

static bool type_from_name(const char *name, riak_dt_type *t) {
    for (size_t i = 0; i < sizeof type_names / sizeof type_names[0]; i++) {
        if (strcmp(name, type_names[i]) == 0) {
            *t = (riak_dt_type)i;
            return true;
        }
    }
    return false;
}

static bool valid_type(riak_dt_type t) {
    return (unsigned)t <= RIAK_DT_FLAG;
}

/* Types that can be fields of a map. */
static bool embeddable(riak_dt_type t) {
    return t == RIAK_DT_COUNTER || t == RIAK_DT_SET || t == RIAK_DT_MAP || t == RIAK_DT_REGISTER ||
           t == RIAK_DT_FLAG;
}

/* ---- values -------------------------------------------------------- */

static void value_free(riak_dt_value *v) {
    riak_strlist l = {.items = v->elements, .count = v->n_elements};
    riak_strlist_free(&l);
    free(v->reg);
    for (size_t i = 0; i < v->n_entries; i++) {
        free(v->entries[i].name);
        value_free(&v->entries[i].value);
    }
    free(v->entries);
    memset(v, 0, sizeof *v);
}

void riak_datatype_free(riak_datatype *dt) {
    if (dt == NULL) {
        return;
    }
    value_free(&dt->value);
    free(dt->context);
    memset(dt, 0, sizeof *dt);
}

const riak_dt_value *riak_dt_map_get(const riak_dt_value *map, const char *name,
                                     riak_dt_type type) {
    if (map == NULL || name == NULL || map->type != RIAK_DT_MAP) {
        return NULL;
    }
    for (size_t i = 0; i < map->n_entries; i++) {
        if (map->entries[i].value.type == type && strcmp(map->entries[i].name, name) == 0) {
            return &map->entries[i].value;
        }
    }
    return NULL;
}

static riak_status parse_value(json_object *j, riak_dt_type type, riak_dt_value *v, int depth);

static riak_status parse_elements(json_object *j, riak_dt_value *v) {
    if (!json_object_is_type(j, json_type_array)) {
        return RIAK_E_PARSE;
    }
    riak_strlist l = {0};
    size_t cap = 0;
    size_t n = json_object_array_length(j);
    for (size_t i = 0; i < n; i++) {
        json_object *e = json_object_array_get_idx(j, i);
        if (!json_object_is_type(e, json_type_string)) {
            riak_strlist_free(&l);
            return RIAK_E_PARSE;
        }
        if (riak_strlist_push(&l, &cap, json_object_get_string(e),
                              (size_t)json_object_get_string_len(e)) != 0) {
            riak_strlist_free(&l);
            return RIAK_E_NOMEM;
        }
    }
    if (l.items == NULL && (l.items = (char **)calloc(1, sizeof(char *))) == NULL) {
        return RIAK_E_NOMEM;
    }
    v->elements = l.items;
    v->n_elements = l.count;
    return RIAK_OK;
}

static riak_status parse_map(json_object *j, riak_dt_value *v, int depth) {
    if (!json_object_is_type(j, json_type_object) || depth > MAX_DEPTH) {
        return RIAK_E_PARSE;
    }
    size_t n = (size_t)json_object_object_length(j);
    if (n > 0 && (v->entries = calloc(n, sizeof *v->entries)) == NULL) {
        return RIAK_E_NOMEM;
    }
    json_object_object_foreach(j, field, fv) {
        const char *us = strrchr(field, '_');
        riak_dt_type ft;
        if (us == NULL || us == field || !type_from_name(us + 1, &ft) || !embeddable(ft)) {
            return RIAK_E_PARSE;
        }
        riak_dt_entry *e = &v->entries[v->n_entries++];
        e->name = riak_strndup(field, (size_t)(us - field));
        if (e->name == NULL) {
            return RIAK_E_NOMEM;
        }
        riak_status st = parse_value(fv, ft, &e->value, depth + 1);
        if (st != RIAK_OK) {
            return st;
        }
    }
    return RIAK_OK;
}

static riak_status parse_value(json_object *j, riak_dt_type type, riak_dt_value *v, int depth) {
    v->type = type;
    switch (type) {
    case RIAK_DT_COUNTER:
    case RIAK_DT_HLL:
        if (!json_object_is_type(j, json_type_int)) {
            return RIAK_E_PARSE;
        }
        v->counter = json_object_get_int64(j);
        return RIAK_OK;
    case RIAK_DT_SET:
    case RIAK_DT_GSET:
        return parse_elements(j, v);
    case RIAK_DT_MAP:
        return parse_map(j, v, depth);
    case RIAK_DT_REGISTER:
        if (!json_object_is_type(j, json_type_string)) {
            return RIAK_E_PARSE;
        }
        v->reg = riak_strdup(json_object_get_string(j));
        return v->reg ? RIAK_OK : RIAK_E_NOMEM;
    case RIAK_DT_FLAG:
        if (!json_object_is_type(j, json_type_boolean)) {
            return RIAK_E_PARSE;
        }
        v->flag = json_object_get_boolean(j);
        return RIAK_OK;
    }
    return RIAK_E_PARSE;
}

riak_status riak_dt_parse(const char *json, size_t len, riak_datatype *out) {
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
    json_object *t, *v, *ctx;
    riak_dt_type type;
    if (obj != NULL && json_tokener_get_error(tok) == json_tokener_success &&
        json_object_is_type(obj, json_type_object) && json_object_object_get_ex(obj, "type", &t) &&
        json_object_is_type(t, json_type_string) &&
        type_from_name(json_object_get_string(t), &type)) {
        if (type == RIAK_DT_REGISTER || type == RIAK_DT_FLAG ||
            !json_object_object_get_ex(obj, "value", &v)) {
            st = RIAK_E_PARSE;
        } else {
            st = parse_value(v, type, &out->value, 0);
        }
        if (st == RIAK_OK && json_object_object_get_ex(obj, "context", &ctx)) {
            if (!json_object_is_type(ctx, json_type_string)) {
                st = RIAK_E_PARSE;
            } else if ((out->context = riak_strdup(json_object_get_string(ctx))) == NULL) {
                st = RIAK_E_NOMEM;
            }
        }
    }
    json_object_put(obj);
    json_tokener_free(tok);
    if (st != RIAK_OK) {
        riak_datatype_free(out);
    }
    return st;
}

/* ---- operations ---------------------------------------------------- */

riak_dt_op *riak_dt_op_new(riak_dt_type type) {
    if (!valid_type(type) || type == RIAK_DT_REGISTER || type == RIAK_DT_FLAG) {
        return NULL;
    }
    riak_dt_op *op = calloc(1, sizeof *op);
    if (op) {
        op->type = type;
    }
    return op;
}

static void fields_free(map_field *f, size_t n) {
    for (size_t i = 0; i < n; i++) {
        free(f[i].name);
        free(f[i].reg);
        riak_dt_op_free(f[i].nested);
    }
    free(f);
}

void riak_dt_op_free(riak_dt_op *op) {
    if (op == NULL) {
        return;
    }
    riak_strlist_free(&op->adds);
    riak_strlist_free(&op->removes);
    fields_free(op->fields, op->n_fields);
    fields_free(op->removed, op->n_removed);
    free(op);
}

static bool check(riak_dt_op *op, bool ok) {
    if (op == NULL) {
        return false;
    }
    if (!ok) {
        op->invalid = true;
    }
    return ok && !op->invalid && !op->nomem;
}

void riak_dt_op_increment(riak_dt_op *counter, int64_t delta) {
    if (!check(counter, counter && counter->type == RIAK_DT_COUNTER)) {
        return;
    }
    if ((delta > 0 && counter->delta > INT64_MAX - delta) ||
        (delta < 0 && counter->delta < INT64_MIN - delta)) {
        counter->invalid = true;
        return;
    }
    counter->delta += delta;
}

void riak_dt_op_add(riak_dt_op *op, const char *element) {
    if (!check(op, op && element &&
                       (op->type == RIAK_DT_SET || op->type == RIAK_DT_HLL ||
                        op->type == RIAK_DT_GSET))) {
        return;
    }
    if (riak_strlist_push(&op->adds, &op->add_cap, element, strlen(element)) != 0) {
        op->nomem = true;
    }
}

void riak_dt_op_remove(riak_dt_op *set, const char *element) {
    if (!check(set, set && element && set->type == RIAK_DT_SET)) {
        return;
    }
    if (riak_strlist_push(&set->removes, &set->remove_cap, element, strlen(element)) != 0) {
        set->nomem = true;
    }
}

/* A field update is found or created. */
static map_field *field(riak_dt_op *map, const char *name, riak_dt_type type) {
    if (!check(map, map && map->type == RIAK_DT_MAP && name && *name && embeddable(type))) {
        return NULL;
    }
    for (size_t i = 0; i < map->n_fields; i++) {
        if (map->fields[i].type == type && strcmp(map->fields[i].name, name) == 0) {
            return &map->fields[i];
        }
    }
    if (map->n_fields == map->field_cap) {
        size_t ncap = map->field_cap ? map->field_cap * 2 : 4;
        map_field *grown = realloc(map->fields, ncap * sizeof *grown);
        if (grown == NULL) {
            map->nomem = true;
            return NULL;
        }
        map->fields = grown;
        map->field_cap = ncap;
    }
    map_field *f = &map->fields[map->n_fields];
    memset(f, 0, sizeof *f);
    f->type = type;
    if ((f->name = riak_strdup(name)) == NULL) {
        map->nomem = true;
        return NULL;
    }
    map->n_fields++;
    return f;
}

void riak_dt_op_map_counter(riak_dt_op *map, const char *name, int64_t delta) {
    map_field *f = field(map, name, RIAK_DT_COUNTER);
    if (f == NULL) {
        return;
    }
    if ((delta > 0 && f->delta > INT64_MAX - delta) ||
        (delta < 0 && f->delta < INT64_MIN - delta)) {
        map->invalid = true;
        return;
    }
    f->delta += delta;
}

void riak_dt_op_map_register(riak_dt_op *map, const char *name, const char *value) {
    if (value == NULL) {
        check(map, false);
        return;
    }
    map_field *f = field(map, name, RIAK_DT_REGISTER);
    if (f == NULL) {
        return;
    }
    char *copy = riak_strdup(value);
    if (copy == NULL) {
        map->nomem = true;
        return;
    }
    free(f->reg);
    f->reg = copy;
}

void riak_dt_op_map_flag(riak_dt_op *map, const char *name, bool enable) {
    map_field *f = field(map, name, RIAK_DT_FLAG);
    if (f) {
        f->flag = enable;
    }
}

static riak_dt_op *nested(riak_dt_op *map, const char *name, riak_dt_type type) {
    map_field *f = field(map, name, type);
    if (f == NULL) {
        return NULL;
    }
    if (f->nested == NULL) {
        if (map->depth >= MAX_DEPTH) {
            map->invalid = true;
            return NULL;
        }
        if ((f->nested = riak_dt_op_new(type)) == NULL) {
            map->nomem = true;
            return NULL;
        }
        f->nested->depth = map->depth + 1;
    }
    return f->nested;
}

riak_dt_op *riak_dt_op_map_set(riak_dt_op *map, const char *name) {
    return nested(map, name, RIAK_DT_SET);
}

riak_dt_op *riak_dt_op_map_map(riak_dt_op *map, const char *name) {
    return nested(map, name, RIAK_DT_MAP);
}

void riak_dt_op_map_remove(riak_dt_op *map, const char *name, riak_dt_type type) {
    if (!check(map, map && map->type == RIAK_DT_MAP && name && *name && embeddable(type))) {
        return;
    }
    if (map->n_removed == map->removed_cap) {
        size_t ncap = map->removed_cap ? map->removed_cap * 2 : 4;
        map_field *grown = realloc(map->removed, ncap * sizeof *grown);
        if (grown == NULL) {
            map->nomem = true;
            return;
        }
        map->removed = grown;
        map->removed_cap = ncap;
    }
    map_field *f = &map->removed[map->n_removed];
    memset(f, 0, sizeof *f);
    f->type = type;
    if ((f->name = riak_strdup(name)) == NULL) {
        map->nomem = true;
        return;
    }
    map->n_removed++;
}

/* ---- serialization ------------------------------------------------- */

static json_object *string_array(const riak_strlist *l) {
    json_object *a = json_object_new_array();
    for (size_t i = 0; a && i < l->count; i++) {
        json_object *s = json_object_new_string(l->items[i]);
        if (s == NULL || json_object_array_add(a, s) != 0) {
            json_object_put(s);
            json_object_put(a);
            return NULL;
        }
    }
    return a;
}

static char *field_key(const map_field *f) {
    return riak_strprintf("%s_%s", f->name, type_names[f->type]);
}

/* NULL is returned with *st set on failure. */
static json_object *op_json(const riak_dt_op *op, riak_status *st, int depth);

static bool put_field(json_object *obj, const char *key, json_object *v, riak_status *st) {
    if (v == NULL) {
        if (*st == RIAK_OK) *st = RIAK_E_NOMEM;
        return false;
    }
    if (json_object_object_add(obj, key, v) != 0) {
        json_object_put(v);
        *st = RIAK_E_NOMEM;
        return false;
    }
    return true;
}

static json_object *map_json(const riak_dt_op *op, riak_status *st, int depth) {
    json_object *root = json_object_new_object();
    if (root == NULL) {
        *st = RIAK_E_NOMEM;
        return NULL;
    }
    if (op->n_fields > 0) {
        json_object *upd = json_object_new_object();
        if (!put_field(root, "update", upd, st)) {
            json_object_put(root);
            return NULL;
        }
        for (size_t i = 0; i < op->n_fields; i++) {
            const map_field *f = &op->fields[i];
            json_object *v = NULL;
            switch (f->type) {
            case RIAK_DT_COUNTER: {
                v = json_object_new_object();
                json_object *n = json_object_new_int64(f->delta);
                if (v && !put_field(v, "increment", n, st)) {
                    json_object_put(v);
                    v = NULL;
                }
                break;
            }
            case RIAK_DT_REGISTER:
                v = f->reg ? json_object_new_string(f->reg) : NULL;
                if (f->reg == NULL) *st = RIAK_E_INVALID_ARG;
                break;
            case RIAK_DT_FLAG:
                v = json_object_new_string(f->flag ? "enable" : "disable");
                break;
            default:
                v = op_json(f->nested, st, depth + 1);
                break;
            }
            char *key = v ? field_key(f) : NULL;
            bool ok = key && put_field(upd, key, v, st);
            if (key == NULL) {
                json_object_put(v);
                if (*st == RIAK_OK) *st = RIAK_E_NOMEM;
            }
            free(key);
            if (!ok) {
                json_object_put(root);
                return NULL;
            }
        }
    }
    if (op->n_removed > 0) {
        json_object *rem = json_object_new_array();
        if (!put_field(root, "remove", rem, st)) {
            json_object_put(root);
            return NULL;
        }
        for (size_t i = 0; i < op->n_removed; i++) {
            char *key = field_key(&op->removed[i]);
            json_object *s = key ? json_object_new_string(key) : NULL;
            free(key);
            if (s == NULL || json_object_array_add(rem, s) != 0) {
                json_object_put(s);
                json_object_put(root);
                *st = RIAK_E_NOMEM;
                return NULL;
            }
        }
    }
    return root;
}

static json_object *op_json(const riak_dt_op *op, riak_status *st, int depth) {
    if (op == NULL || depth > MAX_DEPTH) {
        *st = RIAK_E_INVALID_ARG;
        return NULL;
    }
    if (op->nomem || op->invalid) {
        *st = op->nomem ? RIAK_E_NOMEM : RIAK_E_INVALID_ARG;
        return NULL;
    }
    json_object *root;
    switch (op->type) {
    case RIAK_DT_COUNTER: {
        root = json_object_new_object();
        if (root && !put_field(root, "increment", json_object_new_int64(op->delta), st)) {
            json_object_put(root);
            return NULL;
        }
        break;
    }
    case RIAK_DT_SET:
    case RIAK_DT_HLL:
    case RIAK_DT_GSET:
        if (op->adds.count == 0 && op->removes.count == 0) {
            *st = RIAK_E_INVALID_ARG;
            return NULL;
        }
        root = json_object_new_object();
        if (root && op->adds.count && !put_field(root, "add_all", string_array(&op->adds), st)) {
            json_object_put(root);
            return NULL;
        }
        if (root && op->removes.count &&
            !put_field(root, "remove_all", string_array(&op->removes), st)) {
            json_object_put(root);
            return NULL;
        }
        break;
    case RIAK_DT_MAP:
        if (op->n_fields == 0 && op->n_removed == 0) {
            *st = RIAK_E_INVALID_ARG;
            return NULL;
        }
        return map_json(op, st, depth);
    default:
        *st = RIAK_E_INVALID_ARG;
        return NULL;
    }
    if (root == NULL) {
        *st = RIAK_E_NOMEM;
    }
    return root;
}

riak_status riak_dt_op_to_json(const riak_dt_op *op, const char *context, char **json) {
    *json = NULL;
    riak_status st = RIAK_OK;
    json_object *root = op_json(op, &st, 0);
    if (root == NULL) {
        return st == RIAK_OK ? RIAK_E_NOMEM : st;
    }
    if (context && !put_field(root, "context", json_object_new_string(context), &st)) {
        json_object_put(root);
        return st;
    }
    *json = riak_strdup(json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN));
    json_object_put(root);
    return *json ? RIAK_OK : RIAK_E_NOMEM;
}

/* ---- HTTP ---------------------------------------------------------- */

static const riak_dt_opts no_dt_opts;

static riak_status dt_url(riak_client *c, const char *type, const char *bucket, const char *key,
                          riak_buf *url) {
    riak_status st;
    if ((st = riak_check_name(c, type, "bucket type")) != RIAK_OK ||
        (st = riak_check_name(c, bucket, "bucket")) != RIAK_OK ||
        (key && (st = riak_check_name(c, key, "key")) != RIAK_OK)) {
        return st;
    }
    if (riak_url_bucket(url, c, type, bucket, "datatypes", key) != 0) {
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    return RIAK_OK;
}

static int add_timeout(riak_buf *url, unsigned ms) {
    char num[16];
    if (ms == 0) {
        return 0;
    }
    snprintf(num, sizeof num, "%u", ms);
    return riak_url_append_query(url, "timeout", num);
}

static riak_status parse_body(riak_client *c, riak_response *resp, riak_datatype *out) {
    riak_status st = riak_dt_parse(resp->body.data ? resp->body.data : "", resp->body.len, out);
    return st == RIAK_OK ? st : riak_set_error(c, st, "cannot parse data type");
}

riak_status riak_dt_fetch(riak_client *client, const char *type, const char *bucket,
                          const char *key, const riak_dt_opts *opts, riak_datatype *out) {
    if (client == NULL || out == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    const riak_dt_opts *o = opts ? opts : &no_dt_opts;
    riak_buf url = {0};
    riak_status st = riak_check_name(client, key, "key");
    if (st == RIAK_OK) {
        st = dt_url(client, type, bucket, key, &url);
    }
    if (st == RIAK_OK && riak_is_pb(client)) {
        riak_buf_free(&url);
        return riak_pb_dt_fetch(client, type, bucket, key, o, out);
    }
    if (st == RIAK_OK &&
        (riak_url_append_quorum(&url, "r", o->r) != 0 ||
         riak_url_append_quorum(&url, "pr", o->pr) != 0 ||
         (o->no_context && riak_url_append_query(&url, "include_context", "false") != 0) ||
         add_timeout(&url, o->server_timeout_ms) != 0)) {
        st = riak_set_error(client, RIAK_E_INVALID_ARG, "invalid quorum or out of memory");
    }
    riak_response resp = {0};
    if (st == RIAK_OK) {
        riak_request req = {.method = "GET", .url = url.data};
        st = riak_perform(client, &req, &resp);
        if (st == RIAK_OK) {
            st = resp.status == 200 ? parse_body(client, &resp, out)
                                    : riak_http_error(client, &resp);
        }
    }
    riak_response_free(&resp);
    riak_buf_free(&url);
    return st;
}

/* Location: /types/<t>/buckets/<b>/keys/<key> */
static char *key_from_location(const char *loc) {
    const char *slash = strrchr(loc, '/');
    if (slash == NULL || slash[1] == '\0') {
        return NULL;
    }
    return riak_url_unescape(slash + 1, strcspn(slash + 1, "?#"));
}

riak_status riak_dt_update(riak_client *client, const char *type, const char *bucket,
                           const char *key, const riak_dt_op *op, const char *context,
                           const riak_dt_opts *opts, riak_datatype *out, char **created_key) {
    if (out) memset(out, 0, sizeof *out);
    if (created_key) *created_key = NULL;
    if (client == NULL || op == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    const riak_dt_opts *o = opts ? opts : &no_dt_opts;
    if (riak_is_pb(client)) {
        riak_status st;
        if ((st = riak_check_name(client, type, "bucket type")) != RIAK_OK ||
            (st = riak_check_name(client, bucket, "bucket")) != RIAK_OK ||
            (key && (st = riak_check_name(client, key, "key")) != RIAK_OK)) {
            return st;
        }
        return riak_pb_dt_update(client, type, bucket, key, op, context, o, out, created_key);
    }
    char *body = NULL;
    riak_status st = riak_dt_op_to_json(op, context, &body);
    if (st != RIAK_OK) {
        return riak_set_error(client, st, "invalid data type operation");
    }
    riak_buf url = {0};
    struct curl_slist *headers = NULL;
    riak_response resp = {0};
    if ((st = dt_url(client, type, bucket, key, &url)) != RIAK_OK) {
        goto done;
    }
    if (riak_url_append_quorum(&url, "w", o->w) != 0 ||
        riak_url_append_quorum(&url, "dw", o->dw) != 0 ||
        riak_url_append_quorum(&url, "pw", o->pw) != 0 ||
        (o->return_body && riak_url_append_query(&url, "returnbody", "true") != 0) ||
        add_timeout(&url, o->server_timeout_ms) != 0) {
        st = riak_set_error(client, RIAK_E_INVALID_ARG, "invalid quorum or out of memory");
        goto done;
    }
    if ((st = riak_add_header(client, &headers, "Content-Type", NULL, "application/json")) !=
        RIAK_OK) {
        goto done;
    }
    riak_request req = {.method = "POST",
                        .url = url.data,
                        .headers = headers,
                        .body = body,
                        .body_len = strlen(body)};
    if ((st = riak_perform(client, &req, &resp)) != RIAK_OK) {
        goto done;
    }
    if (resp.status != 200 && resp.status != 201 && resp.status != 204) {
        st = riak_http_error(client, &resp);
        /* removing an absent element or field: {precondition,{not_present,...}} */
        if (resp.status == 500 && resp.body.data && strstr(resp.body.data, "precondition")) {
            st = RIAK_E_PRECONDITION;
        }
        goto done;
    }
    if (created_key && key == NULL) {
        const char *loc = riak_headerlist_get(&resp.headers, "Location");
        if (loc == NULL || (*created_key = key_from_location(loc)) == NULL) {
            st = riak_set_error(client, RIAK_E_PARSE, "no usable Location header");
            goto done;
        }
    }
    if (out && resp.status == 200 && resp.body.len > 0) {
        st = parse_body(client, &resp, out);
    }
done:
    if (st != RIAK_OK && created_key) {
        free(*created_key);
        *created_key = NULL;
    }
    riak_response_free(&resp);
    curl_slist_free_all(headers);
    riak_buf_free(&url);
    free(body);
    return st;
}
