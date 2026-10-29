/*
 File:   pb_dt.c
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later

 Protocol Buffers: data types (DtFetchReq / DtUpdateReq). Operations are
 converted straight from the riak_dt_op tree; contexts are base64 so they
 are interchangeable with the HTTP transport.
 */

#include "pb.h"

#include <stdlib.h>
#include <string.h>

#include "datatype.h"

static const riak_dt_opts no_dt_opts;

/* ---- values ---------------------------------------------------------- */

static bool field_type(MapField__MapFieldType t, riak_dt_type *out) {
    switch (t) {
    case MAP_FIELD__MAP_FIELD_TYPE__COUNTER:
        *out = RIAK_DT_COUNTER;
        return true;
    case MAP_FIELD__MAP_FIELD_TYPE__SET:
        *out = RIAK_DT_SET;
        return true;
    case MAP_FIELD__MAP_FIELD_TYPE__REGISTER:
        *out = RIAK_DT_REGISTER;
        return true;
    case MAP_FIELD__MAP_FIELD_TYPE__FLAG:
        *out = RIAK_DT_FLAG;
        return true;
    case MAP_FIELD__MAP_FIELD_TYPE__MAP:
        *out = RIAK_DT_MAP;
        return true;
    default:
        return false;
    }
}

static MapField__MapFieldType wire_field_type(riak_dt_type t) {
    switch (t) {
    case RIAK_DT_COUNTER:
        return MAP_FIELD__MAP_FIELD_TYPE__COUNTER;
    case RIAK_DT_SET:
        return MAP_FIELD__MAP_FIELD_TYPE__SET;
    case RIAK_DT_REGISTER:
        return MAP_FIELD__MAP_FIELD_TYPE__REGISTER;
    case RIAK_DT_FLAG:
        return MAP_FIELD__MAP_FIELD_TYPE__FLAG;
    default:
        return MAP_FIELD__MAP_FIELD_TYPE__MAP;
    }
}

static riak_status elements(ProtobufCBinaryData *items, size_t n, riak_dt_value *v) {
    riak_strlist l = {0};
    size_t cap = 0;
    for (size_t i = 0; i < n; i++) {
        if (riak_strlist_push(&l, &cap, items[i].data ? (const char *)items[i].data : "",
                              items[i].len) != 0) {
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

static riak_status map_value(MapEntry *const *entries, size_t n, riak_dt_value *v, int depth);

static riak_status entry_value(const MapEntry *e, riak_dt_type t, riak_dt_value *v, int depth) {
    v->type = t;
    switch (t) {
    case RIAK_DT_COUNTER:
        v->counter = e->counter_value;
        return RIAK_OK;
    case RIAK_DT_SET:
        return elements(e->set_value, e->n_set_value, v);
    case RIAK_DT_REGISTER:
        v->reg = riak_pb_strdup(e->register_value);
        return v->reg ? RIAK_OK : RIAK_E_NOMEM;
    case RIAK_DT_FLAG:
        v->flag = e->has_flag_value && e->flag_value;
        return RIAK_OK;
    default:
        return map_value(e->map_value, e->n_map_value, v, depth + 1);
    }
}

static riak_status map_value(MapEntry *const *entries, size_t n, riak_dt_value *v, int depth) {
    v->type = RIAK_DT_MAP;
    if (depth > RIAK_DT_MAX_DEPTH) {
        return RIAK_E_PARSE;
    }
    if (n > 0 && (v->entries = calloc(n, sizeof *v->entries)) == NULL) {
        return RIAK_E_NOMEM;
    }
    for (size_t i = 0; i < n; i++) {
        riak_dt_type t;
        if (entries[i]->field == NULL || !field_type(entries[i]->field->type, &t)) {
            return RIAK_E_PARSE;
        }
        riak_dt_entry *d = &v->entries[v->n_entries++];
        if ((d->name = riak_pb_strdup(entries[i]->field->name)) == NULL) {
            return RIAK_E_NOMEM;
        }
        riak_status st = entry_value(entries[i], t, &d->value, depth);
        if (st != RIAK_OK) {
            return st;
        }
    }
    return RIAK_OK;
}

/* The value fields shared by DtValue and DtUpdateResp. */
typedef struct {
    protobuf_c_boolean has_counter;
    int64_t counter;
    ProtobufCBinaryData *set;
    size_t n_set;
    MapEntry **map;
    size_t n_map;
    protobuf_c_boolean has_hll;
    uint64_t hll;
    ProtobufCBinaryData *gset;
    size_t n_gset;
} wire_value;

static riak_status value_from_wire(const wire_value *w, riak_dt_type t, riak_dt_value *v) {
    v->type = t;
    switch (t) {
    case RIAK_DT_COUNTER:
        v->counter = w->counter;
        return RIAK_OK;
    case RIAK_DT_HLL:
        v->counter = (int64_t)w->hll;
        return RIAK_OK;
    case RIAK_DT_SET:
        return elements(w->set, w->n_set, v);
    case RIAK_DT_GSET:
        return elements(w->gset, w->n_gset, v);
    case RIAK_DT_MAP:
        return map_value(w->map, w->n_map, v, 0);
    default:
        return RIAK_E_PARSE;
    }
}

static riak_status set_context(riak_client *c, protobuf_c_boolean has, ProtobufCBinaryData ctx,
                               riak_datatype *out) {
    if (has && (out->context = riak_pb_b64_encode(ctx)) == NULL) {
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    return RIAK_OK;
}

/* ---- fetch ----------------------------------------------------------- */

riak_status riak_pb_dt_fetch(riak_client *c, const char *type, const char *bucket, const char *key,
                             const riak_dt_opts *opts, riak_datatype *out) {
    const riak_dt_opts *o = opts ? opts : &no_dt_opts;
    DtFetchReq m = DT_FETCH_REQ__INIT;
    m.bucket = riak_pb_bin(bucket);
    m.key = riak_pb_bin(key);
    m.type = riak_pb_bin(type);
    if (!riak_pb_quorum(o->r, &m.has_r, &m.r) || !riak_pb_quorum(o->pr, &m.has_pr, &m.pr) ||
        !riak_pb_quorum(o->node_confirms, &m.has_node_confirms, &m.node_confirms)) {
        return riak_set_error(c, RIAK_E_INVALID_ARG, "invalid quorum");
    }
    m.has_timeout = o->server_timeout_ms > 0;
    m.timeout = o->server_timeout_ms;
    m.has_include_context = 1;
    m.include_context = !o->no_context;
    char *path = riak_pb_path(c, type, bucket, "datatypes", key);
    if (path == NULL) {
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    riak_pb_call call = {.code = RIAK_PB_DT_FETCH_REQ,
                         .resp_code = RIAK_PB_DT_FETCH_RESP,
                         .idempotent = true,
                         .name = "DtFetchReq",
                         .path = path};
    ProtobufCMessage *msg;
    riak_status st = riak_pb_exec(c, &call, &m.base, &dt_fetch_resp__descriptor, &msg);
    free(path);
    if (st != RIAK_OK) {
        return st;
    }
    DtFetchResp *r = (DtFetchResp *)msg;
    riak_dt_type t;
    switch (r->type) {
    case DT_FETCH_RESP__DATA_TYPE__COUNTER:
        t = RIAK_DT_COUNTER;
        break;
    case DT_FETCH_RESP__DATA_TYPE__SET:
        t = RIAK_DT_SET;
        break;
    case DT_FETCH_RESP__DATA_TYPE__MAP:
        t = RIAK_DT_MAP;
        break;
    case DT_FETCH_RESP__DATA_TYPE__HLL:
        t = RIAK_DT_HLL;
        break;
    case DT_FETCH_RESP__DATA_TYPE__GSET:
        t = RIAK_DT_GSET;
        break;
    default:
        dt_fetch_resp__free_unpacked(r, NULL);
        return riak_set_error(c, RIAK_E_PARSE, "unknown data type in the response");
    }
    if (r->value == NULL) {
        dt_fetch_resp__free_unpacked(r, NULL);
        return riak_set_error(c, RIAK_E_NOT_FOUND, "not found");
    }
    const DtValue *dv = r->value;
    wire_value w = {.has_counter = dv->has_counter_value,
                    .counter = dv->counter_value,
                    .set = dv->set_value,
                    .n_set = dv->n_set_value,
                    .map = dv->map_value,
                    .n_map = dv->n_map_value,
                    .has_hll = dv->has_hll_value,
                    .hll = dv->hll_value,
                    .gset = dv->gset_value,
                    .n_gset = dv->n_gset_value};
    st = value_from_wire(&w, t, &out->value);
    if (st != RIAK_OK) {
        riak_set_error(c, st, "cannot decode data type");
    } else {
        st = set_context(c, r->has_context, r->context, out);
    }
    dt_fetch_resp__free_unpacked(r, NULL);
    if (st != RIAK_OK) {
        riak_datatype_free(out);
    }
    return st;
}

/* ---- update ---------------------------------------------------------- */

/* Every allocation of a DtOp tree, freed together. */
typedef struct {
    void **ptrs;
    size_t n, cap;
} arena;

/* n zeroed elements of the given size (at least one, so the result is never NULL on success). */
static void *arena_alloc(arena *a, size_t n, size_t size) {
    if (a->n == a->cap) {
        size_t ncap = a->cap ? a->cap * 2 : 16;
        void **grown = (void **)realloc((void *)a->ptrs, ncap * sizeof *grown);
        if (grown == NULL) {
            return NULL;
        }
        a->ptrs = grown;
        a->cap = ncap;
    }
    void *p = calloc(n ? n : 1, size);
    if (p != NULL) {
        a->ptrs[a->n++] = p;
    }
    return p;
}

static void arena_free(arena *a) {
    for (size_t i = 0; i < a->n; i++) {
        free(a->ptrs[i]);
    }
    free((void *)a->ptrs);
}

static ProtobufCBinaryData *bin_list(arena *a, const riak_strlist *l) {
    ProtobufCBinaryData *b = arena_alloc(a, l->count, sizeof *b);
    for (size_t i = 0; b && i < l->count; i++) {
        b[i] = riak_pb_bin(l->items[i]);
    }
    return b;
}

static riak_status map_op(arena *a, const riak_dt_op *op, MapOp **out, int depth);

static riak_status set_op(arena *a, const riak_dt_op *op, SetOp **out) {
    if (op->adds.count == 0 && op->removes.count == 0) {
        return RIAK_E_INVALID_ARG;
    }
    SetOp *s = arena_alloc(a, 1, sizeof *s);
    if (s == NULL) return RIAK_E_NOMEM;
    set_op__init(s);
    s->n_adds = op->adds.count;
    s->n_removes = op->removes.count;
    if ((s->adds = bin_list(a, &op->adds)) == NULL ||
        (s->removes = bin_list(a, &op->removes)) == NULL) {
        return RIAK_E_NOMEM;
    }
    *out = s;
    return RIAK_OK;
}

static riak_status check_op(const riak_dt_op *op, int depth) {
    if (op == NULL || depth > RIAK_DT_MAX_DEPTH) {
        return RIAK_E_INVALID_ARG;
    }
    if (op->nomem) return RIAK_E_NOMEM;
    if (op->invalid) return RIAK_E_INVALID_ARG;
    return RIAK_OK;
}

static riak_status map_op(arena *a, const riak_dt_op *op, MapOp **out, int depth) {
    riak_status st = check_op(op, depth);
    if (st != RIAK_OK) return st;
    if (op->n_fields == 0 && op->n_removed == 0) return RIAK_E_INVALID_ARG;
    MapOp *m = arena_alloc(a, 1, sizeof *m);
    if (m == NULL) return RIAK_E_NOMEM;
    map_op__init(m);
    m->n_removes = op->n_removed;
    m->removes = (MapField **)arena_alloc(a, op->n_removed, sizeof *m->removes);
    m->n_updates = op->n_fields;
    m->updates = (MapUpdate **)arena_alloc(a, op->n_fields, sizeof *m->updates);
    if (m->removes == NULL || m->updates == NULL) return RIAK_E_NOMEM;
    for (size_t i = 0; i < op->n_removed; i++) {
        MapField *f = arena_alloc(a, 1, sizeof *f);
        if (f == NULL) return RIAK_E_NOMEM;
        map_field__init(f);
        f->name = riak_pb_bin(op->removed[i].name);
        f->type = wire_field_type(op->removed[i].type);
        m->removes[i] = f;
    }
    for (size_t i = 0; i < op->n_fields; i++) {
        const map_field *mf = &op->fields[i];
        MapUpdate *u = arena_alloc(a, 1, sizeof *u);
        MapField *f = arena_alloc(a, 1, sizeof *f);
        if (u == NULL || f == NULL) return RIAK_E_NOMEM;
        map_update__init(u);
        map_field__init(f);
        f->name = riak_pb_bin(mf->name);
        f->type = wire_field_type(mf->type);
        u->field = f;
        switch (mf->type) {
        case RIAK_DT_COUNTER: {
            CounterOp *co = arena_alloc(a, 1, sizeof *co);
            if (co == NULL) return RIAK_E_NOMEM;
            counter_op__init(co);
            co->has_increment = 1;
            co->increment = mf->delta;
            u->counter_op = co;
            break;
        }
        case RIAK_DT_REGISTER:
            if (mf->reg == NULL) return RIAK_E_INVALID_ARG;
            u->has_register_op = 1;
            u->register_op = riak_pb_bin(mf->reg);
            break;
        case RIAK_DT_FLAG:
            u->has_flag_op = 1;
            u->flag_op = mf->flag ? MAP_UPDATE__FLAG_OP__ENABLE : MAP_UPDATE__FLAG_OP__DISABLE;
            break;
        case RIAK_DT_SET:
            if ((st = check_op(mf->nested, depth + 1)) != RIAK_OK ||
                (st = set_op(a, mf->nested, &u->set_op)) != RIAK_OK) {
                return st;
            }
            break;
        default:
            if ((st = map_op(a, mf->nested, &u->map_op, depth + 1)) != RIAK_OK) {
                return st;
            }
            break;
        }
        m->updates[i] = u;
    }
    *out = m;
    return RIAK_OK;
}

static riak_status build_op(arena *a, const riak_dt_op *op, DtOp *d) {
    dt_op__init(d);
    riak_status st = check_op(op, 0);
    if (st != RIAK_OK) return st;
    switch (op->type) {
    case RIAK_DT_COUNTER: {
        CounterOp *co = arena_alloc(a, 1, sizeof *co);
        if (co == NULL) return RIAK_E_NOMEM;
        counter_op__init(co);
        co->has_increment = 1;
        co->increment = op->delta;
        d->counter_op = co;
        return RIAK_OK;
    }
    case RIAK_DT_SET:
        return set_op(a, op, &d->set_op);
    case RIAK_DT_HLL:
    case RIAK_DT_GSET: {
        if (op->adds.count == 0 || op->removes.count != 0) return RIAK_E_INVALID_ARG;
        ProtobufCBinaryData *adds = bin_list(a, &op->adds);
        if (adds == NULL) return RIAK_E_NOMEM;
        if (op->type == RIAK_DT_HLL) {
            HllOp *h = arena_alloc(a, 1, sizeof *h);
            if (h == NULL) return RIAK_E_NOMEM;
            hll_op__init(h);
            h->n_adds = op->adds.count;
            h->adds = adds;
            d->hll_op = h;
        } else {
            GSetOp *g = arena_alloc(a, 1, sizeof *g);
            if (g == NULL) return RIAK_E_NOMEM;
            gset_op__init(g);
            g->n_adds = op->adds.count;
            g->adds = adds;
            d->gset_op = g;
        }
        return RIAK_OK;
    }
    case RIAK_DT_MAP:
        return map_op(a, op, &d->map_op, 0);
    default:
        return RIAK_E_INVALID_ARG;
    }
}

riak_status riak_pb_dt_update(riak_client *c, const char *type, const char *bucket, const char *key,
                              const riak_dt_op *op, const char *context, const riak_dt_opts *opts,
                              riak_datatype *out, char **created_key) {
    const riak_dt_opts *o = opts ? opts : &no_dt_opts;
    arena a = {0};
    DtOp dop;
    riak_status st = build_op(&a, op, &dop);
    if (st != RIAK_OK) {
        arena_free(&a);
        return riak_set_error(c, st, "invalid data type operation");
    }
    DtUpdateReq m = DT_UPDATE_REQ__INIT;
    m.bucket = riak_pb_bin(bucket);
    m.type = riak_pb_bin(type);
    m.op = &dop;
    if (key) {
        m.has_key = 1;
        m.key = riak_pb_bin(key);
    }
    ProtobufCBinaryData ctx = {0};
    if (context) {
        if (!riak_pb_b64_decode(context, &ctx)) {
            arena_free(&a);
            return riak_set_error(c, RIAK_E_INVALID_ARG, "invalid context");
        }
        m.has_context = 1;
        m.context = ctx;
    }
    if (!riak_pb_quorum(o->w, &m.has_w, &m.w) || !riak_pb_quorum(o->dw, &m.has_dw, &m.dw) ||
        !riak_pb_quorum(o->pw, &m.has_pw, &m.pw) ||
        !riak_pb_quorum(o->node_confirms, &m.has_node_confirms, &m.node_confirms)) {
        free(ctx.data);
        arena_free(&a);
        return riak_set_error(c, RIAK_E_INVALID_ARG, "invalid quorum");
    }
    m.has_return_body = o->return_body;
    m.return_body = o->return_body;
    m.has_timeout = o->server_timeout_ms > 0;
    m.timeout = o->server_timeout_ms;
    char *path = riak_pb_path(c, type, bucket, "datatypes", key);
    if (path == NULL) {
        free(ctx.data);
        arena_free(&a);
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    riak_pb_call call = {.code = RIAK_PB_DT_UPDATE_REQ,
                         .resp_code = RIAK_PB_DT_UPDATE_RESP,
                         .idempotent = false, /* counters would count twice */
                         .name = "DtUpdateReq",
                         .path = path};
    ProtobufCMessage *msg;
    st = riak_pb_exec(c, &call, &m.base, &dt_update_resp__descriptor, &msg);
    free(path);
    free(ctx.data);
    arena_free(&a);
    if (st != RIAK_OK) {
        return st;
    }
    DtUpdateResp *r = (DtUpdateResp *)msg;
    if (created_key && key == NULL) {
        if (!r->has_key) {
            st = riak_set_error(c, RIAK_E_PARSE, "no generated key in the response");
        } else if ((*created_key = riak_pb_strdup(r->key)) == NULL) {
            st = riak_set_error(c, RIAK_E_NOMEM, "out of memory");
        }
    }
    if (st == RIAK_OK && out && o->return_body) {
        wire_value w = {.has_counter = r->has_counter_value,
                        .counter = r->counter_value,
                        .set = r->set_value,
                        .n_set = r->n_set_value,
                        .map = r->map_value,
                        .n_map = r->n_map_value,
                        .has_hll = r->has_hll_value,
                        .hll = r->hll_value,
                        .gset = r->gset_value,
                        .n_gset = r->n_gset_value};
        st = value_from_wire(&w, op->type, &out->value);
        if (st != RIAK_OK) {
            riak_set_error(c, st, "cannot decode data type");
        } else {
            st = set_context(c, r->has_context, r->context, out);
        }
        if (st != RIAK_OK) {
            riak_datatype_free(out);
        }
    }
    dt_update_resp__free_unpacked(r, NULL);
    if (st != RIAK_OK && created_key) {
        free(*created_key);
        *created_key = NULL;
    }
    return st;
}
