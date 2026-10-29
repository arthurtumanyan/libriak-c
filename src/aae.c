/*
 File:   aae.c
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later

 AAE folds (Riak 2.9+ with tictac AAE): cached n_val trees, range trees,
 key and clock folds, key finders, object statistics, tombstone reaping,
 erasing, repair and replication of key ranges, bucket listing.

 The transports encode results differently. Over Protocol Buffers a tree
 branch is a 32-bit id followed by zlib-compressed hashes; over HTTP the
 cached branches are base64, range-tree branches base64 of zlib data, and
 every base64 value may arrive as a JSON array of character codes (Riak's
 JSON encoder writes Erlang strings that way). Results are returned in one
 form: uncompressed bytes, and vector clocks as base64 strings.
 */

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>
/* next_in becomes a const pointer */
#define ZLIB_CONST
#include <zlib.h>

#include "internal.h"
#include "pb.h"

static const char *const tree_sizes[] = {"xxsmall", "xsmall", "small", "medium", "large", "xlarge"};
/* level-one width of each tree size */
static const size_t tree_widths[] = {16, 64, 256, 1024, 4096, 16384};

/* ---- frees ------------------------------------------------------------ */

void riak_aae_tree_free(riak_aae_tree *t) {
    if (t == NULL) {
        return;
    }
    free(t->level1);
    for (size_t i = 0; i < t->n_branches; i++) {
        free(t->branches[i].data);
    }
    free(t->branches);
    memset(t, 0, sizeof *t);
}

void riak_aae_keys_free(riak_aae_keys *k) {
    if (k == NULL) {
        return;
    }
    for (size_t i = 0; i < k->count; i++) {
        free(k->items[i].bucket_type);
        free(k->items[i].bucket);
        free(k->items[i].key);
        free(k->items[i].vclock);
    }
    free(k->items);
    memset(k, 0, sizeof *k);
}

void riak_aae_key_counts_free(riak_aae_key_counts *k) {
    if (k == NULL) {
        return;
    }
    for (size_t i = 0; i < k->count; i++) {
        free(k->items[i].key);
    }
    free(k->items);
    memset(k, 0, sizeof *k);
}

void riak_aae_stats_free(riak_aae_stats *s) {
    if (s == NULL) {
        return;
    }
    free(s->sizes);
    free(s->siblings);
    memset(s, 0, sizeof *s);
}

void riak_aae_buckets_free(riak_aae_buckets *b) {
    if (b == NULL) {
        return;
    }
    for (size_t i = 0; i < b->count; i++) {
        free(b->items[i].bucket_type);
        free(b->items[i].bucket);
    }
    free(b->items);
    memset(b, 0, sizeof *b);
}

/* ---- decoding helpers ------------------------------------------------- */

/* zlib (zlib:compress) data is inflated into a new buffer of at most limit
 * bytes, so a hostile peer cannot expand a small branch without bound. The
 * buffer holds one byte more than the limit: a stream that fills it is too
 * long, one that ends within the limit is complete. */
static riak_status inflate_all(const uint8_t *in, size_t len, size_t limit, uint8_t **out,
                               size_t *out_len) {
    if (len > UINT_MAX || limit >= SIZE_MAX / 2) {
        return RIAK_E_PARSE;
    }
    z_stream z;
    memset(&z, 0, sizeof z);
    if (inflateInit(&z) != Z_OK) {
        return RIAK_E_NOMEM;
    }
    size_t max = limit + 1;
    size_t cap = len * 4 + 64, n = 0; /* len <= UINT_MAX: no overflow */
    if (cap > max) {
        cap = max;
    }
    uint8_t *buf = malloc(cap);
    riak_status st = buf ? RIAK_OK : RIAK_E_NOMEM;
    z.next_in = in;
    z.avail_in = (uInt)len;
    while (st == RIAK_OK) {
        if (n == cap) {
            if (cap >= max) {
                st = RIAK_E_PARSE;
                break;
            }
            size_t grow = cap > max / 2 ? max : cap * 2;
            uint8_t *grown = realloc(buf, grow);
            if (grown == NULL) {
                st = RIAK_E_NOMEM;
                break;
            }
            buf = grown;
            cap = grow;
        }
        size_t room = cap - n;
        z.next_out = buf + n;
        z.avail_out = room > UINT_MAX ? UINT_MAX : (uInt)room;
        uInt before = z.avail_out;
        int rc = inflate(&z, Z_NO_FLUSH);
        n += before - z.avail_out;
        if (rc == Z_STREAM_END) {
            if (n > limit) {
                st = RIAK_E_PARSE;
            }
            break;
        }
        if (rc != Z_OK && !(rc == Z_BUF_ERROR && z.avail_out == 0)) {
            st = RIAK_E_PARSE;
        }
    }
    inflateEnd(&z);
    if (st != RIAK_OK) {
        free(buf);
        return st;
    }
    *out = buf;
    *out_len = n;
    return RIAK_OK;
}

/* A JSON value holding base64: a string, or an array of character codes. */
static char *json_b64(json_object *v) {
    if (json_object_is_type(v, json_type_string)) {
        return riak_strdup(json_object_get_string(v));
    }
    if (!json_object_is_type(v, json_type_array)) {
        return NULL;
    }
    size_t n = json_object_array_length(v);
    char *s = malloc(n + 1);
    if (s == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        int ch = json_object_get_int(json_object_array_get_idx(v, i));
        if (ch <= 0 || ch > 127) {
            free(s);
            return NULL;
        }
        s[i] = (char)ch;
    }
    s[n] = '\0';
    return s;
}

static riak_status json_bytes(json_object *v, uint8_t **out, size_t *len) {
    char *b64 = json_b64(v);
    ProtobufCBinaryData b;
    bool ok = b64 && riak_pb_b64_decode(b64, &b);
    free(b64);
    if (!ok) {
        return RIAK_E_PARSE;
    }
    *out = b.data ? b.data : malloc(1);
    *len = b.len;
    return *out ? RIAK_OK : RIAK_E_NOMEM;
}

/* protobuf-c declares repeated fields mutable, though packing only reads
 * them; the caller's const array is passed without a cast. */
static uint32_t *ids_field(const uint32_t *ids) {
    uint32_t *p;
    memcpy((void *)&p, (const void *)&ids, sizeof p);
    return p;
}

static char *dup_bin(ProtobufCBinaryData b) {
    return riak_pb_strdup(b);
}

static riak_status size_from_width(size_t width, riak_aae_tree_size *size) {
    for (size_t i = 0; i < sizeof tree_widths / sizeof tree_widths[0]; i++) {
        if (tree_widths[i] == width) {
            *size = (riak_aae_tree_size)i;
            return RIAK_OK;
        }
    }
    return RIAK_E_PARSE;
}

/* ---- argument checks -------------------------------------------------- */

static riak_status check_filter(riak_client *c, const riak_aae_filter *f) {
    if (f == NULL) {
        return riak_set_error(c, RIAK_E_INVALID_ARG, "filter must not be NULL");
    }
    riak_status st;
    if ((st = riak_check_name(c, f->bucket, "bucket")) != RIAK_OK ||
        (f->bucket_type && (st = riak_check_name(c, f->bucket_type, "bucket type")) != RIAK_OK)) {
        return st;
    }
    if ((f->start_key == NULL) != (f->end_key == NULL)) {
        return riak_set_error(c, RIAK_E_INVALID_ARG, "set both start_key and end_key, or neither");
    }
    if ((f->segments == NULL && f->n_segments > 0) ||
        (unsigned)f->segment_tree_size > RIAK_AAE_XLARGE) {
        return riak_set_error(c, RIAK_E_INVALID_ARG, "invalid segment filter");
    }
    return RIAK_OK;
}

static bool has_mod_range(const riak_aae_filter *f) {
    return f->modified_start != 0 || f->modified_end != 0;
}

/* ---- Protocol Buffers -------------------------------------------------- */

/* The common range fields; the generated request structs all name them
 * alike, so one macro fills any of them. */
#define PB_RANGE(m, f)                                                                             \
    do {                                                                                           \
        if ((f)->bucket_type) {                                                                    \
            (m).has_type = 1;                                                                      \
            (m).type = riak_pb_bin((f)->bucket_type);                                              \
        }                                                                                          \
        (m).bucket = riak_pb_bin((f)->bucket);                                                     \
        (m).key_range = (f)->start_key != NULL;                                                    \
        if ((f)->start_key) {                                                                      \
            (m).has_start_key = (m).has_end_key = 1;                                               \
            (m).start_key = riak_pb_bin((f)->start_key);                                           \
            (m).end_key = riak_pb_bin((f)->end_key);                                               \
        }                                                                                          \
        (m).modified_range = has_mod_range(f);                                                     \
        if ((m).modified_range) {                                                                  \
            (m).has_last_mod_start = (m).has_last_mod_end = 1;                                     \
            (m).last_mod_start = (f)->modified_start;                                              \
            (m).last_mod_end = (f)->modified_end;                                                  \
        }                                                                                          \
    } while (0)

#define PB_SEGMENTS(m, f, enum_type)                                                               \
    do {                                                                                           \
        (m).segment_filter = (f)->n_segments > 0;                                                  \
        if ((m).segment_filter) {                                                                  \
            (m).n_id_filter = (f)->n_segments;                                                     \
            (m).id_filter = ids_field((f)->segments);                                              \
            (m).has_filter_tree_size = 1;                                                          \
            (m).filter_tree_size = (enum_type)(f)->segment_tree_size;                              \
        }                                                                                          \
    } while (0)

static riak_status pb_fold(riak_client *c, uint8_t code, uint8_t resp_code, const char *name,
                           const ProtobufCMessage *m, bool idempotent, ProtobufCMessage **out) {
    const ProtobufCMessageDescriptor *desc =
        resp_code == RIAK_PB_AAE_TREE_RESP        ? &rpb_aae_fold_tree_resp__descriptor
        : resp_code == RIAK_PB_AAE_KEY_VALUE_RESP ? &rpb_aae_fold_key_value_resp__descriptor
        : resp_code == RIAK_PB_AAE_KEY_COUNT_RESP ? &rpb_aae_fold_key_count_resp__descriptor
                                                  : &rpb_aae_fold_list_buckets_resp__descriptor;
    riak_pb_call call = {.code = code,
                         .resp_code = resp_code,
                         .idempotent = idempotent,
                         .name = name,
                         .path = "/aaefold"};
    return riak_pb_exec(c, &call, m, desc, out);
}

static riak_status pb_tree(riak_client *c, RpbAaeFoldTreeResp *r, riak_aae_tree *out) {
    riak_status st = RIAK_OK;
    if ((unsigned)r->size > (unsigned)RIAK_AAE_XLARGE) {
        return riak_set_error(c, RIAK_E_PARSE, "unknown tree size %d", (int)r->size);
    }
    out->size = (riak_aae_tree_size)r->size;
    if (r->has_level_one && r->level_one.len > 0) {
        out->level1_len = r->level_one.len;
        if ((out->level1 = malloc(r->level_one.len)) == NULL) {
            return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
        }
        memcpy(out->level1, r->level_one.data, r->level_one.len);
    }
    if (r->n_level_two > 0 &&
        (out->branches = calloc(r->n_level_two, sizeof *out->branches)) == NULL) {
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    for (size_t i = 0; st == RIAK_OK && i < r->n_level_two; i++) {
        const ProtobufCBinaryData *b = &r->level_two[i];
        if (b->len < 4) {
            return riak_set_error(c, RIAK_E_PARSE, "malformed tree branch");
        }
        riak_aae_branch *br = &out->branches[out->n_branches++];
        br->id = ((uint32_t)b->data[0] << 24U) | ((uint32_t)b->data[1] << 16U) |
                 ((uint32_t)b->data[2] << 8U) | b->data[3];
        st = inflate_all(b->data + 4, b->len - 4, c->cfg.max_body_bytes, &br->data, &br->len);
    }
    return st == RIAK_OK ? st : riak_set_error(c, st, "cannot decode tree branch");
}

static riak_status pb_keys(riak_client *c, RpbAaeFoldKeyValueResp *r, riak_aae_keys *out) {
    if (r->n_keys_value > 0 && (out->items = calloc(r->n_keys_value, sizeof *out->items)) == NULL) {
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    for (size_t i = 0; i < r->n_keys_value; i++) {
        const RpbKeysValue *kv = r->keys_value[i];
        riak_aae_key_clock *k = &out->items[out->count++];
        if ((kv->has_type && (k->bucket_type = dup_bin(kv->type)) == NULL) ||
            (k->bucket = dup_bin(kv->bucket)) == NULL || (k->key = dup_bin(kv->key)) == NULL ||
            (k->vclock = riak_pb_b64_encode(kv->value)) == NULL) {
            return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
        }
    }
    return RIAK_OK;
}

/* A count result: the value of tag "dispatched_count". */
static riak_status pb_count(riak_client *c, RpbAaeFoldKeyCountResp *r, uint64_t *count) {
    for (size_t i = 0; i < r->n_keys_count; i++) {
        if (r->keys_count[i]->tag.len == 16 &&
            memcmp(r->keys_count[i]->tag.data, "dispatched_count", 16) == 0) {
            *count = r->keys_count[i]->count;
            return RIAK_OK;
        }
    }
    return riak_set_error(c, RIAK_E_PARSE, "no dispatched_count in the response");
}

/* ---- HTTP --------------------------------------------------------------- */

/* "?filter=<base64 of the JSON document>" */
static int add_filter(riak_buf *url, json_object *doc) {
    const char *json = json_object_to_json_string_ext(doc, JSON_C_TO_STRING_PLAIN);
    char *b64 = json ? riak_pb_b64_encode(riak_pb_bytes(json, strlen(json))) : NULL;
    int rc = b64 ? riak_url_append_query(url, "filter", b64) : -1;
    free(b64);
    return rc;
}

static json_object *range_json(const riak_aae_filter *f, bool segments) {
    json_object *o = json_object_new_object();
    if (o == NULL) {
        return NULL;
    }
    if (f->start_key) {
        json_object *kr = json_object_new_object();
        json_object_object_add(kr, "start", json_object_new_string(f->start_key));
        json_object_object_add(kr, "end", json_object_new_string(f->end_key));
        json_object_object_add(o, "key_range", kr);
    }
    if (has_mod_range(f)) {
        json_object *dr = json_object_new_object();
        json_object_object_add(dr, "start", json_object_new_int64(f->modified_start));
        json_object_object_add(dr, "end", json_object_new_int64(f->modified_end));
        json_object_object_add(o, "date_range", dr);
    }
    if (segments && f->n_segments > 0) {
        json_object *sf = json_object_new_object();
        json_object *ids = json_object_new_array();
        for (size_t i = 0; i < f->n_segments; i++) {
            json_object_array_add(ids, json_object_new_int64(f->segments[i]));
        }
        json_object_object_add(sf, "segments", ids);
        json_object_object_add(sf, "tree_size",
                               json_object_new_string(tree_sizes[f->segment_tree_size]));
        json_object_object_add(o, "segment_filter", sf);
    }
    return o;
}

/* "<root><what>/[types/<t>/]buckets/<b>[/<suffix>]" */
static int range_url(riak_buf *url, riak_client *c, const char *what, const riak_aae_filter *f,
                     const char *suffix) {
    return riak_buf_append_str(url, c->root) || riak_buf_append_str(url, what) ||
           riak_buf_append_str(url, "/") ||
           (f->bucket_type &&
            (riak_buf_append_str(url, "types/") || riak_url_append_escaped(url, f->bucket_type) ||
             riak_buf_append_str(url, "/"))) ||
           riak_buf_append_str(url, "buckets/") || riak_url_append_escaped(url, f->bucket) ||
           (suffix && (riak_buf_append_str(url, "/") || riak_buf_append_str(url, suffix)));
}

/* GET url (optionally with a filter document) and parse the JSON body. */
static riak_status http_fold(riak_client *c, riak_buf *url, json_object *filter, bool repeatable,
                             json_object **out) {
    *out = NULL;
    if (filter && add_filter(url, filter) != 0) {
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    riak_request req = {.method = "GET", .url = url->data, .no_retry = !repeatable};
    riak_response resp = {0};
    riak_status st = riak_perform(c, &req, &resp);
    if (st == RIAK_OK && resp.status != 200) {
        st = riak_http_error_or_unsupported(c, &resp, "AAE folds (Riak 2.9+, tictac AAE)");
    }
    if (st == RIAK_OK) {
        *out = json_tokener_parse(resp.body.data ? resp.body.data : "");
        if (*out == NULL || !json_object_is_type(*out, json_type_object)) {
            json_object_put(*out);
            *out = NULL;
            st = riak_set_error(c, RIAK_E_PARSE, "malformed AAE fold response");
        }
    }
    riak_response_free(&resp);
    return st;
}

static riak_status http_keys(riak_client *c, json_object *doc, riak_aae_keys *out) {
    json_object *arr;
    if (!json_object_object_get_ex(doc, "keys-clocks", &arr) ||
        !json_object_is_type(arr, json_type_array)) {
        return riak_set_error(c, RIAK_E_PARSE, "no keys-clocks in the response");
    }
    size_t n = json_object_array_length(arr);
    if ((out->items = calloc(n ? n : 1, sizeof *out->items)) == NULL) {
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    for (size_t i = 0; i < n; i++) {
        json_object *e = json_object_array_get_idx(arr, i), *t, *b, *k, *clk;
        riak_aae_key_clock *kc = &out->items[out->count++];
        if (!json_object_object_get_ex(e, "bucket", &b) ||
            !json_object_object_get_ex(e, "key", &k) ||
            !json_object_object_get_ex(e, "clock", &clk)) {
            return riak_set_error(c, RIAK_E_PARSE, "malformed keys-clocks entry");
        }
        if ((json_object_object_get_ex(e, "bucket-type", &t) &&
             (kc->bucket_type = riak_strdup(json_object_get_string(t))) == NULL) ||
            (kc->bucket = riak_strdup(json_object_get_string(b))) == NULL ||
            (kc->key = riak_strdup(json_object_get_string(k))) == NULL ||
            (kc->vclock = json_b64(clk)) == NULL) {
            return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
        }
    }
    return RIAK_OK;
}

static riak_status http_count(riak_client *c, json_object *doc, uint64_t *count) {
    json_object *v;
    if (!json_object_object_get_ex(doc, "dispatched_count", &v) ||
        !json_object_is_type(v, json_type_int) || json_object_get_int64(v) < 0) {
        return riak_set_error(c, RIAK_E_PARSE, "no dispatched_count in the response");
    }
    *count = (uint64_t)json_object_get_int64(v);
    return RIAK_OK;
}

/* ---- cached n_val trees ------------------------------------------------ */

static riak_status check_nval(riak_client *c, unsigned n_val) {
    return n_val == 0 ? riak_set_error(c, RIAK_E_INVALID_ARG, "n_val must not be 0") : RIAK_OK;
}

riak_status riak_aae_merge_root(riak_client *client, unsigned n_val, riak_aae_tree *out) {
    if (client == NULL || out == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    riak_status st = check_nval(client, n_val);
    if (st != RIAK_OK) {
        return st;
    }
    if (riak_is_pb(client)) {
        RpbAaeFoldMergeRootNValReq m = RPB_AAE_FOLD_MERGE_ROOT_NVAL_REQ__INIT;
        m.n_val = n_val;
        ProtobufCMessage *msg;
        st = pb_fold(client, RIAK_PB_AAE_MERGE_ROOT_NVAL_REQ, RIAK_PB_AAE_TREE_RESP,
                     "RpbAaeFoldMergeRootNValReq", &m.base, true, &msg);
        if (st == RIAK_OK) {
            st = pb_tree(client, (RpbAaeFoldTreeResp *)msg, out);
            protobuf_c_message_free_unpacked(msg, NULL);
        }
    } else {
        riak_buf url = {0};
        char path[64];
        snprintf(path, sizeof path, "cachedtrees/nvals/%u/root", n_val);
        json_object *doc = NULL, *root;
        st = riak_buf_append_str(&url, client->root) || riak_buf_append_str(&url, path)
                 ? riak_set_error(client, RIAK_E_NOMEM, "out of memory")
                 : http_fold(client, &url, NULL, true, &doc);
        if (st == RIAK_OK) {
            if (!json_object_object_get_ex(doc, "root", &root) ||
                json_bytes(root, &out->level1, &out->level1_len) != RIAK_OK) {
                st = riak_set_error(client, RIAK_E_PARSE, "malformed tree root");
            } else {
                out->size = RIAK_AAE_LARGE; /* cached trees are always large */
            }
        }
        json_object_put(doc);
        riak_buf_free(&url);
    }
    if (st != RIAK_OK) {
        riak_aae_tree_free(out);
    }
    return st;
}

riak_status riak_aae_merge_branches(riak_client *client, unsigned n_val, const uint32_t *ids,
                                    size_t n_ids, riak_aae_tree *out) {
    if (client == NULL || out == NULL || (ids == NULL && n_ids > 0)) {
        return RIAK_E_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    riak_status st = check_nval(client, n_val);
    if (st != RIAK_OK) {
        return st;
    }
    if (riak_is_pb(client)) {
        RpbAaeFoldMergeBranchNValReq m = RPB_AAE_FOLD_MERGE_BRANCH_NVAL_REQ__INIT;
        m.n_val = n_val;
        m.n_id_filter = n_ids;
        m.id_filter = ids_field(ids);
        ProtobufCMessage *msg;
        st = pb_fold(client, RIAK_PB_AAE_MERGE_BRANCH_NVAL_REQ, RIAK_PB_AAE_TREE_RESP,
                     "RpbAaeFoldMergeBranchNValReq", &m.base, true, &msg);
        if (st == RIAK_OK) {
            st = pb_tree(client, (RpbAaeFoldTreeResp *)msg, out);
            protobuf_c_message_free_unpacked(msg, NULL);
        }
    } else {
        riak_buf url = {0};
        char path[64];
        snprintf(path, sizeof path, "cachedtrees/nvals/%u/branch", n_val);
        json_object *list = json_object_new_array(), *doc = NULL, *arr;
        for (size_t i = 0; list && i < n_ids; i++) {
            json_object_array_add(list, json_object_new_int64(ids[i]));
        }
        st = list == NULL || riak_buf_append_str(&url, client->root) ||
                     riak_buf_append_str(&url, path)
                 ? riak_set_error(client, RIAK_E_NOMEM, "out of memory")
                 : http_fold(client, &url, list, true, &doc);
        if (st == RIAK_OK && (!json_object_object_get_ex(doc, "branches", &arr) ||
                              !json_object_is_type(arr, json_type_array))) {
            st = riak_set_error(client, RIAK_E_PARSE, "no branches in the response");
        }
        size_t n = st == RIAK_OK ? json_object_array_length(arr) : 0;
        if (st == RIAK_OK && (out->branches = calloc(n ? n : 1, sizeof *out->branches)) == NULL) {
            st = riak_set_error(client, RIAK_E_NOMEM, "out of memory");
        }
        for (size_t i = 0; st == RIAK_OK && i < n; i++) {
            json_object *e = json_object_array_get_idx(arr, i), *id, *data;
            riak_aae_branch *br = &out->branches[out->n_branches++];
            if (!json_object_object_get_ex(e, "branch-id", &id) ||
                !json_object_object_get_ex(e, "branch", &data) ||
                json_bytes(data, &br->data, &br->len) != RIAK_OK) {
                st = riak_set_error(client, RIAK_E_PARSE, "malformed branch");
            } else {
                br->id = (uint32_t)json_object_get_int64(id);
            }
        }
        out->size = RIAK_AAE_LARGE;
        json_object_put(list);
        json_object_put(doc);
        riak_buf_free(&url);
    }
    if (st != RIAK_OK) {
        riak_aae_tree_free(out);
    }
    return st;
}

riak_status riak_aae_fetch_clocks_nval(riak_client *client, unsigned n_val,
                                       const uint32_t *segments, size_t n_segments,
                                       riak_aae_keys *out) {
    if (client == NULL || out == NULL || (segments == NULL && n_segments > 0)) {
        return RIAK_E_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    riak_status st = check_nval(client, n_val);
    if (st != RIAK_OK) {
        return st;
    }
    if (riak_is_pb(client)) {
        RpbAaeFoldFetchClocksNValReq m = RPB_AAE_FOLD_FETCH_CLOCKS_NVAL_REQ__INIT;
        m.n_val = n_val;
        m.n_id_filter = n_segments;
        m.id_filter = ids_field(segments);
        ProtobufCMessage *msg;
        st = pb_fold(client, RIAK_PB_AAE_FETCH_CLOCKS_NVAL_REQ, RIAK_PB_AAE_KEY_VALUE_RESP,
                     "RpbAaeFoldFetchClocksNValReq", &m.base, true, &msg);
        if (st == RIAK_OK) {
            st = pb_keys(client, (RpbAaeFoldKeyValueResp *)msg, out);
            protobuf_c_message_free_unpacked(msg, NULL);
        }
    } else {
        riak_buf url = {0};
        char path[64];
        snprintf(path, sizeof path, "cachedtrees/nvals/%u/keysclocks", n_val);
        json_object *list = json_object_new_array(), *doc = NULL;
        for (size_t i = 0; list && i < n_segments; i++) {
            json_object_array_add(list, json_object_new_int64(segments[i]));
        }
        st = list == NULL || riak_buf_append_str(&url, client->root) ||
                     riak_buf_append_str(&url, path)
                 ? riak_set_error(client, RIAK_E_NOMEM, "out of memory")
                 : http_fold(client, &url, list, true, &doc);
        if (st == RIAK_OK) {
            st = http_keys(client, doc, out);
        }
        json_object_put(list);
        json_object_put(doc);
        riak_buf_free(&url);
    }
    if (st != RIAK_OK) {
        riak_aae_keys_free(out);
    }
    return st;
}

/* ---- range folds -------------------------------------------------------- */

riak_status riak_aae_merge_range(riak_client *client, const riak_aae_filter *f,
                                 riak_aae_tree_size size, riak_aae_tree *out) {
    if (client == NULL || out == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    riak_status st = check_filter(client, f);
    if (st == RIAK_OK && (unsigned)size > RIAK_AAE_XLARGE) {
        st = riak_set_error(client, RIAK_E_INVALID_ARG, "invalid tree size");
    }
    if (st != RIAK_OK) {
        return st;
    }
    if (riak_is_pb(client)) {
        RpbAaeFoldMergeTreesRangeReq m = RPB_AAE_FOLD_MERGE_TREES_RANGE_REQ__INIT;
        PB_RANGE(m, f);
        PB_SEGMENTS(m, f, RpbAaeFoldMergeTreesRangeReq__RpbTreeSize);
        m.tree_size = (RpbAaeFoldMergeTreesRangeReq__RpbTreeSize)size;
        m.use_prehash = !f->use_hash_iv;
        m.has_init_vector = f->use_hash_iv;
        m.init_vector = f->hash_iv;
        ProtobufCMessage *msg;
        st = pb_fold(client, RIAK_PB_AAE_MERGE_TREES_RANGE_REQ, RIAK_PB_AAE_TREE_RESP,
                     "RpbAaeFoldMergeTreesRangeReq", &m.base, true, &msg);
        if (st == RIAK_OK) {
            st = pb_tree(client, (RpbAaeFoldTreeResp *)msg, out);
            protobuf_c_message_free_unpacked(msg, NULL);
        }
    } else {
        riak_buf url = {0};
        char suffix[32];
        snprintf(suffix, sizeof suffix, "trees/%s", tree_sizes[size]);
        json_object *filter = range_json(f, true), *doc = NULL, *tree, *l1, *l2;
        if (filter && f->use_hash_iv) {
            json_object_object_add(filter, "hash_iv", json_object_new_int64(f->hash_iv));
        }
        st = filter == NULL || range_url(&url, client, "rangetrees", f, suffix)
                 ? riak_set_error(client, RIAK_E_NOMEM, "out of memory")
                 : http_fold(client, &url, filter, true, &doc);
        if (st == RIAK_OK && (!json_object_object_get_ex(doc, "tree", &tree) ||
                              !json_object_object_get_ex(tree, "level1", &l1) ||
                              !json_object_object_get_ex(tree, "level2", &l2) ||
                              !json_object_is_type(l2, json_type_object) ||
                              json_bytes(l1, &out->level1, &out->level1_len) != RIAK_OK ||
                              size_from_width(out->level1_len / 4, &out->size) != RIAK_OK)) {
            st = riak_set_error(client, RIAK_E_PARSE, "malformed range tree");
        }
        if (st == RIAK_OK) {
            size_t n = (size_t)json_object_object_length(l2);
            if ((out->branches = calloc(n ? n : 1, sizeof *out->branches)) == NULL) {
                st = riak_set_error(client, RIAK_E_NOMEM, "out of memory");
            }
            size_t used = 0;
            json_object_object_foreach(l2, id, data) {
                if (st != RIAK_OK || used >= n) {
                    break;
                }
                riak_aae_branch *br = &out->branches[used++];
                out->n_branches = used;
                uint8_t *z = NULL;
                size_t zlen = 0;
                char *end = NULL;
                unsigned long bid = strtoul(id, &end, 10);
                br->id = (uint32_t)bid;
                st = end == id || *end != '\0' || bid > UINT32_MAX ? RIAK_E_PARSE
                                                                   : json_bytes(data, &z, &zlen);
                if (st == RIAK_OK) {
                    st = inflate_all(z, zlen, client->cfg.max_body_bytes, &br->data, &br->len);
                }
                free(z);
                if (st != RIAK_OK) {
                    riak_set_error(client, st, "malformed tree branch");
                }
            }
        }
        json_object_put(filter);
        json_object_put(doc);
        riak_buf_free(&url);
    }
    if (st != RIAK_OK) {
        riak_aae_tree_free(out);
    }
    return st;
}

/* fetch_clocks_range and find_tombs share their shape. */
static riak_status clocks_range(riak_client *client, const riak_aae_filter *f, bool tombs,
                                riak_aae_keys *out) {
    if (client == NULL || out == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    riak_status st = check_filter(client, f);
    if (st != RIAK_OK) {
        return st;
    }
    if (riak_is_pb(client)) {
        ProtobufCMessage *msg;
        if (tombs) {
            RpbAaeFoldFindTombsReq m = RPB_AAE_FOLD_FIND_TOMBS_REQ__INIT;
            PB_RANGE(m, f);
            PB_SEGMENTS(m, f, RpbAaeFoldFindTombsReq__RpbTreeSize);
            st = pb_fold(client, RIAK_PB_AAE_FIND_TOMBS_REQ, RIAK_PB_AAE_KEY_VALUE_RESP,
                         "RpbAaeFoldFindTombsReq", &m.base, true, &msg);
        } else {
            RpbAaeFoldFetchClocksRangeReq m = RPB_AAE_FOLD_FETCH_CLOCKS_RANGE_REQ__INIT;
            PB_RANGE(m, f);
            PB_SEGMENTS(m, f, RpbAaeFoldFetchClocksRangeReq__RpbTreeSize);
            st = pb_fold(client, RIAK_PB_AAE_FETCH_CLOCKS_RANGE_REQ, RIAK_PB_AAE_KEY_VALUE_RESP,
                         "RpbAaeFoldFetchClocksRangeReq", &m.base, true, &msg);
        }
        if (st == RIAK_OK) {
            st = pb_keys(client, (RpbAaeFoldKeyValueResp *)msg, out);
            protobuf_c_message_free_unpacked(msg, NULL);
        }
    } else {
        riak_buf url = {0};
        json_object *filter = range_json(f, true), *doc = NULL;
        st = filter == NULL || range_url(&url, client, tombs ? "tombs" : "rangetrees", f,
                                         tombs ? NULL : "keysclocks")
                 ? riak_set_error(client, RIAK_E_NOMEM, "out of memory")
                 : http_fold(client, &url, filter, true, &doc);
        if (st == RIAK_OK) {
            st = http_keys(client, doc, out);
        }
        json_object_put(filter);
        json_object_put(doc);
        riak_buf_free(&url);
    }
    if (st != RIAK_OK) {
        riak_aae_keys_free(out);
    }
    return st;
}

riak_status riak_aae_fetch_clocks_range(riak_client *client, const riak_aae_filter *f,
                                        riak_aae_keys *out) {
    return clocks_range(client, f, false, out);
}

riak_status riak_aae_find_tombs(riak_client *client, const riak_aae_filter *f, riak_aae_keys *out) {
    return clocks_range(client, f, true, out);
}

riak_status riak_aae_find_keys(riak_client *client, const riak_aae_filter *f, riak_aae_find what,
                               uint32_t limit, riak_aae_key_counts *out) {
    if (client == NULL || out == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    riak_status st = check_filter(client, f);
    if (st == RIAK_OK && (unsigned)what > RIAK_AAE_OBJECT_SIZE) {
        st = riak_set_error(client, RIAK_E_INVALID_ARG, "invalid finder");
    }
    if (st != RIAK_OK) {
        return st;
    }
    if (riak_is_pb(client)) {
        RpbAaeFoldFindKeysReq m = RPB_AAE_FOLD_FIND_KEYS_REQ__INIT;
        PB_RANGE(m, f);
        m.finder = what == RIAK_AAE_SIBLING_COUNT
                       ? RPB_AAE_FOLD_FIND_KEYS_REQ__FIND_TYPE__sibling_count
                       : RPB_AAE_FOLD_FIND_KEYS_REQ__FIND_TYPE__object_size;
        m.find_limit = limit;
        ProtobufCMessage *msg;
        st = pb_fold(client, RIAK_PB_AAE_FIND_KEYS_REQ, RIAK_PB_AAE_KEY_COUNT_RESP,
                     "RpbAaeFoldFindKeysReq", &m.base, true, &msg);
        if (st == RIAK_OK) {
            RpbAaeFoldKeyCountResp *r = (RpbAaeFoldKeyCountResp *)msg;
            size_t n = r->n_keys_count;
            if ((out->items = calloc(n ? n : 1, sizeof *out->items)) == NULL) {
                st = riak_set_error(client, RIAK_E_NOMEM, "out of memory");
            }
            for (size_t i = 0; st == RIAK_OK && i < n; i++) {
                riak_aae_key_count *kc = &out->items[out->count++];
                kc->value = r->keys_count[i]->count;
                if ((kc->key = dup_bin(r->keys_count[i]->tag)) == NULL) {
                    st = riak_set_error(client, RIAK_E_NOMEM, "out of memory");
                }
            }
            protobuf_c_message_free_unpacked(msg, NULL);
        }
    } else {
        riak_buf url = {0};
        char suffix[48];
        snprintf(suffix, sizeof suffix, "%s/%u",
                 what == RIAK_AAE_SIBLING_COUNT ? "counts" : "sizes", limit);
        json_object *filter = range_json(f, false), *doc = NULL, *arr;
        st =
            filter == NULL || range_url(&url, client,
                                        what == RIAK_AAE_SIBLING_COUNT ? "siblings" : "objectsizes",
                                        f, suffix)
                ? riak_set_error(client, RIAK_E_NOMEM, "out of memory")
                : http_fold(client, &url, filter, true, &doc);
        if (st == RIAK_OK && (!json_object_object_get_ex(doc, "results", &arr) ||
                              !json_object_is_type(arr, json_type_array))) {
            st = riak_set_error(client, RIAK_E_PARSE, "no results in the response");
        }
        size_t n = st == RIAK_OK ? json_object_array_length(arr) : 0;
        if (st == RIAK_OK && (out->items = calloc(n ? n : 1, sizeof *out->items)) == NULL) {
            st = riak_set_error(client, RIAK_E_NOMEM, "out of memory");
        }
        for (size_t i = 0; st == RIAK_OK && i < n; i++) {
            json_object *e = json_object_array_get_idx(arr, i), *k, *v;
            riak_aae_key_count *kc = &out->items[out->count++];
            if (!json_object_object_get_ex(e, "key", &k) ||
                !json_object_object_get_ex(e, "value", &v)) {
                st = riak_set_error(client, RIAK_E_PARSE, "malformed result");
            } else if ((kc->key = riak_strdup(json_object_get_string(k))) == NULL) {
                st = riak_set_error(client, RIAK_E_NOMEM, "out of memory");
            } else {
                kc->value = (uint64_t)json_object_get_int64(v);
            }
        }
        json_object_put(filter);
        json_object_put(doc);
        riak_buf_free(&url);
    }
    if (st != RIAK_OK) {
        riak_aae_key_counts_free(out);
    }
    return st;
}

static riak_status push_hist(riak_aae_histogram **arr, size_t *n, uint32_t order, uint64_t count) {
    riak_aae_histogram *grown = realloc(*arr, (*n + 1) * sizeof **arr);
    if (grown == NULL) {
        return RIAK_E_NOMEM;
    }
    *arr = grown;
    grown[*n].order = order;
    grown[*n].count = count;
    (*n)++;
    return RIAK_OK;
}

riak_status riak_aae_object_stats(riak_client *client, const riak_aae_filter *f,
                                  riak_aae_stats *out) {
    if (client == NULL || out == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    riak_status st = check_filter(client, f);
    if (st != RIAK_OK) {
        return st;
    }
    if (riak_is_pb(client)) {
        RpbAaeFoldObjectStatsReq m = RPB_AAE_FOLD_OBJECT_STATS_REQ__INIT;
        PB_RANGE(m, f);
        ProtobufCMessage *msg;
        st = pb_fold(client, RIAK_PB_AAE_OBJECT_STATS_REQ, RIAK_PB_AAE_KEY_COUNT_RESP,
                     "RpbAaeFoldObjectStatsReq", &m.base, true, &msg);
        if (st == RIAK_OK) {
            RpbAaeFoldKeyCountResp *r = (RpbAaeFoldKeyCountResp *)msg;
            for (size_t i = 0; st == RIAK_OK && i < r->n_keys_count; i++) {
                const RpbKeysCount *kc = r->keys_count[i];
                char *tag = dup_bin(kc->tag);
                if (tag == NULL) {
                    st = RIAK_E_NOMEM;
                } else if (strcmp(tag, "total_count") == 0) {
                    out->total_count = kc->count;
                } else if (strcmp(tag, "total_size") == 0) {
                    out->total_size = kc->count;
                } else if (strcmp(tag, "sizes") == 0) {
                    st = push_hist(&out->sizes, &out->n_sizes, kc->order, kc->count);
                } else if (strcmp(tag, "siblings") == 0) {
                    st = push_hist(&out->siblings, &out->n_siblings, kc->order, kc->count);
                }
                free(tag);
            }
            protobuf_c_message_free_unpacked(msg, NULL);
            if (st != RIAK_OK) {
                riak_set_error(client, st, "out of memory");
            }
        }
    } else {
        riak_buf url = {0};
        json_object *filter = range_json(f, false), *doc = NULL, *v;
        st = filter == NULL || range_url(&url, client, "objectstats", f, NULL)
                 ? riak_set_error(client, RIAK_E_NOMEM, "out of memory")
                 : http_fold(client, &url, filter, true, &doc);
        if (st == RIAK_OK) {
            if (json_object_object_get_ex(doc, "total_count", &v)) {
                out->total_count = (uint64_t)json_object_get_int64(v);
            }
            if (json_object_object_get_ex(doc, "total_size", &v)) {
                out->total_size = (uint64_t)json_object_get_int64(v);
            }
            const char *names[] = {"sizes", "siblings"};
            for (int h = 0; st == RIAK_OK && h < 2; h++) {
                if (!json_object_object_get_ex(doc, names[h], &v) ||
                    !json_object_is_type(v, json_type_object)) {
                    continue;
                }
                json_object_object_foreach(v, order, count) {
                    char *end = NULL;
                    unsigned long ord = strtoul(order, &end, 10);
                    if (st == RIAK_OK && (end == order || *end != '\0' || ord > UINT32_MAX)) {
                        st = riak_set_error(client, RIAK_E_PARSE, "malformed histogram");
                    }
                    if (st == RIAK_OK) {
                        st = h == 0 ? push_hist(&out->sizes, &out->n_sizes, (uint32_t)ord,
                                                (uint64_t)json_object_get_int64(count))
                                    : push_hist(&out->siblings, &out->n_siblings, (uint32_t)ord,
                                                (uint64_t)json_object_get_int64(count));
                        if (st != RIAK_OK) {
                            riak_set_error(client, st, "out of memory");
                        }
                    }
                }
            }
        }
        json_object_put(filter);
        json_object_put(doc);
        riak_buf_free(&url);
    }
    if (st != RIAK_OK) {
        riak_aae_stats_free(out);
    }
    return st;
}

/* reap_tombs, erase_keys, repair_keys and repl_keys return a count. */
enum change_op { OP_REAP, OP_ERASE, OP_REPAIR, OP_REPL };

static riak_status change(riak_client *client, const riak_aae_filter *f, enum change_op op,
                          riak_aae_change how, uint32_t job_id, const char *queue,
                          uint64_t *count) {
    if (client == NULL || count == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    *count = 0;
    riak_status st = check_filter(client, f);
    if (st == RIAK_OK && (unsigned)how > RIAK_AAE_JOB) {
        st = riak_set_error(client, RIAK_E_INVALID_ARG, "invalid change method");
    }
    if (st == RIAK_OK && how == RIAK_AAE_JOB && job_id == 0) {
        st = riak_set_error(client, RIAK_E_INVALID_ARG, "a job needs a job id above 0");
    }
    if (st == RIAK_OK && op == OP_REPL) {
        st = riak_check_name(client, queue, "queue");
    }
    if (st != RIAK_OK) {
        return st;
    }
    /* counting only reads; every other change must not run twice */
    bool repeatable = (op == OP_REAP || op == OP_ERASE) && how == RIAK_AAE_COUNT;
    if (riak_is_pb(client)) {
        ProtobufCMessage *msg = NULL;
        if (op == OP_REAP) {
            RpbAaeFoldReapTombsReq m = RPB_AAE_FOLD_REAP_TOMBS_REQ__INIT;
            PB_RANGE(m, f);
            PB_SEGMENTS(m, f, RpbAaeFoldReapTombsReq__RpbTreeSize);
            m.change_method =
                (RpbAaeFoldReapTombsReq__ChangeMethod)(how == RIAK_AAE_COUNT
                                                           ? RPB_AAE_FOLD_REAP_TOMBS_REQ__CHANGE_METHOD__count
                                                       : how == RIAK_AAE_LOCAL
                                                           ? RPB_AAE_FOLD_REAP_TOMBS_REQ__CHANGE_METHOD__local
                                                           : RPB_AAE_FOLD_REAP_TOMBS_REQ__CHANGE_METHOD__job);
            m.has_job_id = how == RIAK_AAE_JOB;
            m.job_id = job_id;
            st = pb_fold(client, RIAK_PB_AAE_REAP_TOMBS_REQ, RIAK_PB_AAE_KEY_COUNT_RESP,
                         "RpbAaeFoldReapTombsReq", &m.base, repeatable, &msg);
        } else if (op == OP_ERASE) {
            RpbAaeFoldEraseKeysReq m = RPB_AAE_FOLD_ERASE_KEYS_REQ__INIT;
            PB_RANGE(m, f);
            PB_SEGMENTS(m, f, RpbAaeFoldEraseKeysReq__RpbTreeSize);
            m.change_method =
                (RpbAaeFoldEraseKeysReq__ChangeMethod)(how == RIAK_AAE_COUNT
                                                           ? RPB_AAE_FOLD_ERASE_KEYS_REQ__CHANGE_METHOD__count
                                                       : how == RIAK_AAE_LOCAL
                                                           ? RPB_AAE_FOLD_ERASE_KEYS_REQ__CHANGE_METHOD__local
                                                           : RPB_AAE_FOLD_ERASE_KEYS_REQ__CHANGE_METHOD__job);
            m.has_job_id = how == RIAK_AAE_JOB;
            m.job_id = job_id;
            st = pb_fold(client, RIAK_PB_AAE_ERASE_KEYS_REQ, RIAK_PB_AAE_KEY_COUNT_RESP,
                         "RpbAaeFoldEraseKeysReq", &m.base, repeatable, &msg);
        } else if (op == OP_REPAIR) {
            RpbAaeFoldRepairKeysReq m = RPB_AAE_FOLD_REPAIR_KEYS_REQ__INIT;
            PB_RANGE(m, f);
            st = pb_fold(client, RIAK_PB_AAE_REPAIR_KEYS_REQ, RIAK_PB_AAE_KEY_COUNT_RESP,
                         "RpbAaeFoldRepairKeysReq", &m.base, repeatable, &msg);
        } else {
            RpbAaeFoldReplKeysReq m = RPB_AAE_FOLD_REPL_KEYS_REQ__INIT;
            PB_RANGE(m, f);
            m.queuename = riak_pb_bin(queue);
            st = pb_fold(client, RIAK_PB_AAE_REPL_KEYS_REQ, RIAK_PB_AAE_KEY_COUNT_RESP,
                         "RpbAaeFoldReplKeysReq", &m.base, repeatable, &msg);
        }
        if (st == RIAK_OK) {
            st = pb_count(client, (RpbAaeFoldKeyCountResp *)msg, count);
        }
        if (msg) {
            protobuf_c_message_free_unpacked(msg, NULL);
        }
        return st;
    }
    riak_buf url = {0};
    json_object *filter = range_json(f, op == OP_REAP || op == OP_ERASE), *doc = NULL;
    if (filter && (op == OP_REAP || op == OP_ERASE)) {
        json_object *cm;
        if (how == RIAK_AAE_JOB) {
            cm = json_object_new_object();
            json_object_object_add(cm, "job_id", json_object_new_int64(job_id));
        } else {
            cm = json_object_new_string(how == RIAK_AAE_COUNT ? "count" : "local");
        }
        json_object_object_add(filter, "change_method", cm);
    }
    riak_buf suffix = {0};
    int rc = op == OP_REPL && (riak_buf_append_str(&suffix, "queuename/") ||
                               riak_url_append_escaped(&suffix, queue));
    const char *what = op == OP_REAP     ? "reap"
                       : op == OP_ERASE  ? "erase"
                       : op == OP_REPAIR ? "rangerepair"
                                         : "rangerepl";
    st = filter == NULL || rc || range_url(&url, client, what, f, suffix.data)
             ? riak_set_error(client, RIAK_E_NOMEM, "out of memory")
             : http_fold(client, &url, filter, repeatable, &doc);
    if (st == RIAK_OK) {
        st = http_count(client, doc, count);
    }
    json_object_put(filter);
    json_object_put(doc);
    riak_buf_free(&suffix);
    riak_buf_free(&url);
    return st;
}

riak_status riak_aae_reap_tombs(riak_client *client, const riak_aae_filter *f, riak_aae_change how,
                                uint32_t job_id, uint64_t *count) {
    return change(client, f, OP_REAP, how, job_id, NULL, count);
}

riak_status riak_aae_erase_keys(riak_client *client, const riak_aae_filter *f, riak_aae_change how,
                                uint32_t job_id, uint64_t *count) {
    return change(client, f, OP_ERASE, how, job_id, NULL, count);
}

riak_status riak_aae_repair_keys(riak_client *client, const riak_aae_filter *f, uint64_t *count) {
    return change(client, f, OP_REPAIR, RIAK_AAE_COUNT, 0, NULL, count);
}

riak_status riak_aae_repl_keys(riak_client *client, const riak_aae_filter *f, const char *queue,
                               uint64_t *count) {
    return change(client, f, OP_REPL, RIAK_AAE_COUNT, 0, queue, count);
}

/* ---- bucket listing ---------------------------------------------------- */

riak_status riak_aae_list_buckets(riak_client *client, unsigned n_val, riak_aae_buckets *out) {
    if (client == NULL || out == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    riak_status st = check_nval(client, n_val);
    if (st != RIAK_OK) {
        return st;
    }
    if (riak_is_pb(client)) {
        RpbAaeFoldListBucketsReq m = RPB_AAE_FOLD_LIST_BUCKETS_REQ__INIT;
        m.has_n_val = 1;
        m.n_val = n_val;
        ProtobufCMessage *msg;
        st = pb_fold(client, RIAK_PB_AAE_LIST_BUCKETS_REQ, RIAK_PB_AAE_LIST_BUCKETS_RESP,
                     "RpbAaeFoldListBucketsReq", &m.base, true, &msg);
        if (st == RIAK_OK) {
            RpbAaeFoldListBucketsResp *r = (RpbAaeFoldListBucketsResp *)msg;
            size_t n = r->n_bucket_list;
            if ((out->items = calloc(n ? n : 1, sizeof *out->items)) == NULL) {
                st = riak_set_error(client, RIAK_E_NOMEM, "out of memory");
            }
            for (size_t i = 0; st == RIAK_OK && i < n; i++) {
                const RpbAaeFoldBucket *b = r->bucket_list[i];
                riak_aae_bucket *ab = &out->items[out->count++];
                if ((b->has_type && (ab->bucket_type = dup_bin(b->type)) == NULL) ||
                    (ab->bucket = dup_bin(b->bucket)) == NULL) {
                    st = riak_set_error(client, RIAK_E_NOMEM, "out of memory");
                }
            }
            protobuf_c_message_free_unpacked(msg, NULL);
        }
    } else {
        riak_buf url = {0};
        char num[16];
        snprintf(num, sizeof num, "%u", n_val);
        json_object *doc = NULL, *arr;
        st = riak_buf_append_str(&url, client->root) ||
                     riak_buf_append_str(&url, "aaebucketlist") ||
                     riak_url_append_query(&url, "nval", num)
                 ? riak_set_error(client, RIAK_E_NOMEM, "out of memory")
                 : http_fold(client, &url, NULL, true, &doc);
        if (st == RIAK_OK && (!json_object_object_get_ex(doc, "results", &arr) ||
                              !json_object_is_type(arr, json_type_array))) {
            st = riak_set_error(client, RIAK_E_PARSE, "no results in the response");
        }
        size_t n = st == RIAK_OK ? json_object_array_length(arr) : 0;
        if (st == RIAK_OK && (out->items = calloc(n ? n : 1, sizeof *out->items)) == NULL) {
            st = riak_set_error(client, RIAK_E_NOMEM, "out of memory");
        }
        for (size_t i = 0; st == RIAK_OK && i < n; i++) {
            json_object *e = json_object_array_get_idx(arr, i), *t, *b;
            riak_aae_bucket *ab = &out->items[out->count++];
            if (!json_object_object_get_ex(e, "bucket", &b)) {
                st = riak_set_error(client, RIAK_E_PARSE, "malformed bucket entry");
            } else if ((json_object_object_get_ex(e, "bucket-type", &t) &&
                        (ab->bucket_type = riak_strdup(json_object_get_string(t))) == NULL) ||
                       (ab->bucket = riak_strdup(json_object_get_string(b))) == NULL) {
                st = riak_set_error(client, RIAK_E_NOMEM, "out of memory");
            }
        }
        json_object_put(doc);
        riak_buf_free(&url);
    }
    if (st != RIAK_OK) {
        riak_aae_buckets_free(out);
    }
    return st;
}
