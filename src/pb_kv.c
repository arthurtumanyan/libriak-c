/*
 File:   pb_kv.c
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later

 Protocol Buffers: objects, siblings, listings, bucket and bucket-type
 properties, server information, preference lists and coverage plans.
 Results have the same shape as over HTTP: vector clocks are base64, ETags
 are quoted vtags, Last-Modified is an HTTP date, links use the Link header
 syntax and bucket properties are the HTTP JSON document.
 */

#include "pb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <json-c/json.h>
#include <openssl/evp.h>

/* ---- base64 (vector clocks and contexts are binary) ----------------- */

char *riak_pb_b64_encode(ProtobufCBinaryData b) {
    if (b.len > (size_t)INT32_MAX / 4 * 3 - 3) {
        return NULL;
    }
    char *out = malloc(4 * ((b.len + 2) / 3) + 1);
    if (out != NULL) {
        EVP_EncodeBlock((unsigned char *)out, b.data, (int)b.len);
    }
    return out;
}

bool riak_pb_b64_decode(const char *s, ProtobufCBinaryData *out) {
    size_t n = strlen(s);
    out->data = NULL;
    out->len = 0;
    if (n % 4 != 0 || n > INT32_MAX) {
        return false;
    }
    /* strict: the alphabet only, '=' only as padding (EVP_DecodeBlock would
     * skip whitespace) */
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        bool alpha = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                     (ch >= '0' && ch <= '9') || ch == '+' || ch == '/';
        bool pad = ch == '=' && i >= n - 2 && (i == n - 1 || s[n - 1] == '=');
        if (!alpha && !pad) {
            return false;
        }
    }
    out->data = malloc(n / 4 * 3 + 1);
    if (out->data == NULL) {
        return false;
    }
    int len = EVP_DecodeBlock(out->data, (const unsigned char *)s, (int)n);
    if (len < 0) {
        free(out->data);
        out->data = NULL;
        return false;
    }
    /* EVP_DecodeBlock counts the padding as zero bytes */
    for (size_t i = n; i > 0 && s[i - 1] == '=' && len > 0; i--) {
        len--;
    }
    out->len = (size_t)len;
    return true;
}

/* ---- RpbContent <-> riak_object -------------------------------------- */

static char *http_date(uint32_t secs) {
    static const char *const days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    time_t t = (time_t)secs;
    struct tm tm;
    if (gmtime_r(&t, &tm) == NULL) {
        return NULL;
    }
    return riak_strprintf("%s, %02d %s %d %02d:%02d:%02d GMT", days[tm.tm_wday], tm.tm_mday,
                          months[tm.tm_mon], tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static int copy_pairs(RpbPair *const *pairs, size_t n, riak_header **out, size_t *count) {
    *out = NULL;
    *count = 0;
    if (n == 0) {
        return 0;
    }
    if ((*out = calloc(n, sizeof **out)) == NULL) {
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        riak_header *h = &(*out)[i];
        h->name = riak_pb_strdup(pairs[i]->key);
        h->value = pairs[i]->has_value ? riak_pb_strdup(pairs[i]->value) : riak_strdup("");
        (*count)++;
        if (h->name == NULL || h->value == NULL) {
            return -1;
        }
    }
    return 0;
}

/* Links as the HTTP Link header would carry them. */
static int links_string(RpbLink *const *links, size_t n, char **out) {
    *out = NULL;
    if (n == 0) {
        return 0;
    }
    riak_buf b = {0};
    for (size_t i = 0; i < n; i++) {
        char *bucket = links[i]->has_bucket ? riak_pb_strdup(links[i]->bucket) : riak_strdup("");
        char *key = links[i]->has_key ? riak_pb_strdup(links[i]->key) : riak_strdup("");
        char *tag = links[i]->has_tag ? riak_pb_strdup(links[i]->tag) : riak_strdup("");
        int rc = bucket == NULL || key == NULL || tag == NULL ||
                 (i > 0 && riak_buf_append_str(&b, ", ") != 0) ||
                 riak_buf_append_str(&b, "</buckets/") != 0 ||
                 riak_url_append_escaped(&b, bucket) != 0 ||
                 riak_buf_append_str(&b, "/keys/") != 0 || riak_url_append_escaped(&b, key) != 0 ||
                 riak_buf_append_str(&b, ">; riaktag=\"") != 0 ||
                 riak_buf_append_str(&b, tag) != 0 || riak_buf_append_str(&b, "\"") != 0;
        free(bucket);
        free(key);
        free(tag);
        if (rc) {
            riak_buf_free(&b);
            return -1;
        }
    }
    *out = riak_buf_steal(&b, NULL);
    return *out ? 0 : -1;
}

static riak_status content_to_object(riak_client *c, const RpbContent *ct,
                                     const ProtobufCBinaryData *vclock, riak_object *o) {
    memset(o, 0, sizeof *o);
    bool fail = false;
    o->data = riak_strndup(ct->value.data ? (const char *)ct->value.data : "", ct->value.len);
    o->len = ct->value.len;
    fail = fail || o->data == NULL;
    if (ct->has_content_type) {
        if (ct->has_charset) {
            o->content_type = riak_strprintf("%.*s; charset=%.*s", (int)ct->content_type.len,
                                             (const char *)ct->content_type.data,
                                             (int)ct->charset.len, (const char *)ct->charset.data);
        } else {
            o->content_type = riak_pb_strdup(ct->content_type);
        }
        fail = fail || o->content_type == NULL;
    }
    if (vclock) {
        fail = fail || (o->vclock = riak_pb_b64_encode(*vclock)) == NULL;
    }
    if (ct->has_vtag) {
        fail = fail || (o->etag = riak_strprintf("\"%.*s\"", (int)ct->vtag.len,
                                                 (const char *)ct->vtag.data)) == NULL;
    }
    if (ct->has_last_mod) {
        fail = fail || (o->last_modified = http_date(ct->last_mod)) == NULL;
    }
    o->deleted = ct->has_deleted && ct->deleted;
    fail = fail || copy_pairs(ct->usermeta, ct->n_usermeta, &o->meta, &o->n_meta) != 0;
    fail = fail || copy_pairs(ct->indexes, ct->n_indexes, &o->indexes, &o->n_indexes) != 0;
    fail = fail || links_string(ct->links, ct->n_links, &o->links) != 0;
    if (fail) {
        riak_object_free(o);
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    return RIAK_OK;
}

/* Everything a PutReq content points to that had to be allocated. */
typedef struct {
    RpbContent content;
    RpbPair *meta, *indexes;
    RpbPair **meta_p, **indexes_p;
    RpbLink *links;
    RpbLink **links_p;
    size_t n_links, links_cap;
    char *strings; /* unescaped link buckets and keys, one allocation each */
    char **owned;
    size_t n_owned, owned_cap;
} content_build;

static void content_build_free(content_build *b) {
    free(b->meta);
    free(b->indexes);
    free((void *)b->meta_p);
    free((void *)b->indexes_p);
    free(b->links);
    free((void *)b->links_p);
    for (size_t i = 0; i < b->n_owned; i++) {
        free(b->owned[i]);
    }
    free((void *)b->owned);
}

static char *own(content_build *b, char *s) {
    if (s == NULL) {
        return NULL;
    }
    if (b->n_owned == b->owned_cap) {
        size_t ncap = b->owned_cap ? b->owned_cap * 2 : 8;
        char **grown = (char **)realloc((void *)b->owned, ncap * sizeof *grown);
        if (grown == NULL) {
            free(s);
            return NULL;
        }
        b->owned = grown;
        b->owned_cap = ncap;
    }
    b->owned[b->n_owned++] = s;
    return s;
}

static bool pairs_from(const riak_kv_pair *kv, size_t n, RpbPair **arr, RpbPair ***ptrs) {
    if (n == 0) {
        return true;
    }
    *arr = calloc(n, sizeof **arr);
    *ptrs = (RpbPair **)calloc(n, sizeof **ptrs);
    if (*arr == NULL || *ptrs == NULL) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        rpb_pair__init(&(*arr)[i]);
        (*arr)[i].key = riak_pb_bin(kv[i].name);
        (*arr)[i].has_value = 1;
        (*arr)[i].value = riak_pb_bin(kv[i].value);
        (*ptrs)[i] = &(*arr)[i];
    }
    return true;
}

/* One Link header entry: </buckets/b/keys/k>; riaktag="t" or </riak/b/k>; riaktag="t".
 * Entries without riaktag (rel="up") are skipped. -1 malformed, -2 ENOMEM. */
static int parse_link(content_build *b, const char *s, size_t len) {
    const char *lt = memchr(s, '<', len);
    const char *gt = lt ? memchr(lt, '>', len - (size_t)(lt - s)) : NULL;
    if (lt == NULL || gt == NULL) {
        return -1;
    }
    const char *rest = gt + 1;
    size_t rest_len = len - (size_t)(rest - s);
    const char *tag = NULL;
    size_t tag_len = 0;
    for (size_t i = 0; i + 9 <= rest_len; i++) {
        if (memcmp(rest + i, "riaktag=\"", 9) == 0) {
            tag = rest + i + 9;
            const char *q = memchr(tag, '"', rest_len - i - 9);
            if (q == NULL) {
                return -1;
            }
            tag_len = (size_t)(q - tag);
            break;
        }
    }
    if (tag == NULL) {
        return 0;
    }
    const char *path = lt + 1;
    size_t plen = (size_t)(gt - path);
    const char *bk, *sep;
    if (plen > 9 && memcmp(path, "/buckets/", 9) == 0) {
        bk = path + 9;
        sep = "/keys/";
    } else if (plen > 6 && memcmp(path, "/riak/", 6) == 0) {
        bk = path + 6;
        sep = "/";
    } else {
        return -1;
    }
    size_t bklen = plen - (size_t)(bk - path);
    const char *mid = NULL;
    for (size_t i = 0; i + strlen(sep) <= bklen; i++) {
        if (memcmp(bk + i, sep, strlen(sep)) == 0) {
            mid = bk + i;
            break;
        }
    }
    if (mid == NULL || mid == bk) {
        return -1;
    }
    const char *key = mid + strlen(sep);
    size_t key_len = bklen - (size_t)(key - bk);
    char *bucket_s = own(b, riak_url_unescape(bk, (size_t)(mid - bk)));
    char *key_s = own(b, riak_url_unescape(key, key_len));
    char *tag_s = own(b, riak_strndup(tag, tag_len));
    if (bucket_s == NULL || key_s == NULL || tag_s == NULL) {
        return -2;
    }
    if (b->n_links == b->links_cap) {
        size_t ncap = b->links_cap ? b->links_cap * 2 : 4;
        RpbLink *grown = realloc(b->links, ncap * sizeof *grown);
        if (grown == NULL) {
            return -2;
        }
        b->links = grown;
        b->links_cap = ncap;
    }
    RpbLink *l = &b->links[b->n_links++];
    rpb_link__init(l);
    l->has_bucket = l->has_key = l->has_tag = 1;
    l->bucket = riak_pb_bin(bucket_s);
    l->key = riak_pb_bin(key_s);
    l->tag = riak_pb_bin(tag_s);
    return 0;
}

static riak_status build_content(riak_client *c, const void *value, size_t len, const riak_opts *o,
                                 content_build *b) {
    memset(b, 0, sizeof *b);
    RpbContent *ct = &b->content;
    rpb_content__init(ct);
    ct->value = riak_pb_bytes(value ? value : "", len);
    const char *type = o->content_type ? o->content_type : "application/octet-stream";
    const char *semi = strchr(type, ';');
    const char *cs = semi ? strstr(semi, "charset=") : NULL;
    ct->has_content_type = 1;
    if (semi) {
        char *t = own(b, riak_strndup(type, (size_t)(semi - type)));
        if (t == NULL) {
            return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
        }
        while (*t && (t[strlen(t) - 1] == ' ' || t[strlen(t) - 1] == '\t')) {
            t[strlen(t) - 1] = '\0';
        }
        ct->content_type = riak_pb_bin(t);
    } else {
        ct->content_type = riak_pb_bin(type);
    }
    if (cs) {
        cs += 8;
        ct->has_charset = 1;
        ct->charset = riak_pb_bytes(cs, strcspn(cs, "; \t"));
    }
    if (!pairs_from(o->meta, o->n_meta, &b->meta, &b->meta_p) ||
        !pairs_from(o->indexes, o->n_indexes, &b->indexes, &b->indexes_p)) {
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    for (size_t i = 0; i < o->n_meta; i++) {
        if (o->meta[i].name == NULL || o->meta[i].value == NULL) {
            return riak_set_error(c, RIAK_E_INVALID_ARG, "invalid metadata entry");
        }
    }
    for (size_t i = 0; i < o->n_indexes; i++) {
        if (o->indexes[i].name == NULL || o->indexes[i].value == NULL) {
            return riak_set_error(c, RIAK_E_INVALID_ARG, "invalid index entry");
        }
    }
    ct->usermeta = b->meta_p;
    ct->n_usermeta = o->n_meta;
    ct->indexes = b->indexes_p;
    ct->n_indexes = o->n_indexes;
    for (size_t i = 0; i < o->n_links; i++) {
        const char *s = o->links[i];
        if (s == NULL) {
            return riak_set_error(c, RIAK_E_INVALID_ARG, "invalid link");
        }
        /* a value may hold several comma-separated entries */
        while (*s) {
            size_t n = strcspn(s, ",");
            const char *gt = memchr(s, '>', n);
            if (gt == NULL && s[n] == ',') {
                /* a comma inside <...> */
                const char *close = strchr(s, '>');
                n = close ? strcspn(close, ",") + (size_t)(close - s) : strlen(s);
            }
            int rc = parse_link(b, s, n);
            if (rc == -2) {
                return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
            }
            if (rc == -1) {
                return riak_set_error(c, RIAK_E_INVALID_ARG, "invalid link: %s", o->links[i]);
            }
            s += n;
            while (*s == ',' || *s == ' ') {
                s++;
            }
        }
    }
    if (b->n_links) {
        b->links_p = (RpbLink **)calloc(b->n_links, sizeof *b->links_p);
        if (b->links_p == NULL) {
            return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
        }
        for (size_t i = 0; i < b->n_links; i++) {
            b->links_p[i] = &b->links[i];
        }
        ct->links = b->links_p;
        ct->n_links = b->n_links;
    }
    return RIAK_OK;
}

/* ---- shared request plumbing ----------------------------------------- */

void riak_pb_prepared_free(riak_pb_prepared *p) {
    free(p->payload);
    free(p->path);
    memset(p, 0, sizeof *p);
}

static riak_status finish_prepare(riak_client *c, const ProtobufCMessage *msg, char *path,
                                  riak_pb_prepared *p) {
    p->path = path;
    if (path == NULL) {
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    p->len = protobuf_c_message_get_packed_size(msg);
    p->payload = malloc(p->len ? p->len : 1);
    if (p->payload == NULL) {
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    protobuf_c_message_pack(msg, p->payload);
    p->req.payload = p->payload;
    p->req.len = p->len;
    p->req.path = p->path;
    return RIAK_OK;
}

static riak_status unsupported(riak_client *c, const char *what) {
    return riak_set_error(c, RIAK_E_UNSUPPORTED, "%s is not supported over Protocol Buffers", what);
}

static riak_status check_object(riak_client *c, const char *bucket, const char *key,
                                const riak_opts *o) {
    riak_status st;
    if ((st = riak_check_name(c, bucket, "bucket")) != RIAK_OK ||
        (key && (st = riak_check_name(c, key, "key")) != RIAK_OK) ||
        (o->bucket_type && (st = riak_check_name(c, o->bucket_type, "bucket type")) != RIAK_OK)) {
        return st;
    }
    return RIAK_OK;
}

#define QUORUM(field, value) (riak_pb_quorum((value), &m.has_##field, &m.field))

/* ---- get ------------------------------------------------------------- */

riak_status riak_pb_prepare_get(riak_client *c, const char *bucket, const char *key,
                                const riak_opts *o, riak_pb_prepared *p) {
    memset(p, 0, sizeof *p);
    riak_status st = check_object(c, bucket, key, o);
    if (st == RIAK_OK && key == NULL) {
        st = riak_check_name(c, key, "key");
    }
    if (st != RIAK_OK) {
        return st;
    }
    if (o->if_none_match) return unsupported(c, "if_none_match on get (use if_modified)");
    if (o->if_match) return unsupported(c, "if_match on get");
    if (o->if_modified_since) return unsupported(c, "if_modified_since");
    if (o->if_unmodified_since) return unsupported(c, "if_unmodified_since");
    RpbGetReq m = RPB_GET_REQ__INIT;
    m.bucket = riak_pb_bin(bucket);
    m.key = riak_pb_bin(key);
    if (!QUORUM(r, o->r) || !QUORUM(pr, o->pr)) {
        return riak_set_error(c, RIAK_E_INVALID_ARG, "invalid quorum");
    }
    m.has_basic_quorum = o->basic_quorum;
    m.basic_quorum = o->basic_quorum;
    m.has_notfound_ok = o->notfound_ok;
    m.notfound_ok = o->notfound_ok;
    m.has_head = o->head;
    m.head = o->head;
    m.has_sloppy_quorum = o->sloppy_quorum;
    m.sloppy_quorum = o->sloppy_quorum;
    m.has_n_val = o->n_val > 0;
    m.n_val = o->n_val;
    m.has_timeout = o->server_timeout_ms > 0;
    m.timeout = o->server_timeout_ms;
    if (o->bucket_type) {
        m.has_type = 1;
        m.type = riak_pb_bin(o->bucket_type);
    }
    ProtobufCBinaryData vc = {0};
    if (o->if_modified) {
        if (o->vclock == NULL || !riak_pb_b64_decode(o->vclock, &vc)) {
            return riak_set_error(c, RIAK_E_INVALID_ARG, "if_modified needs a valid vclock");
        }
        m.has_if_modified = 1;
        m.if_modified = vc;
    }
    p->req = (riak_pb_request){.code = RIAK_PB_GET_REQ,
                               .resp_code = RIAK_PB_GET_RESP,
                               .idempotent = true,
                               .name = "RpbGetReq"};
    st = finish_prepare(c, &m.base, riak_pb_path(c, o->bucket_type, bucket, "keys", key), p);
    free(vc.data);
    if (st != RIAK_OK) {
        riak_pb_prepared_free(p);
    }
    return st;
}

static riak_status to_set(riak_client *c, const RpbGetResp *r, riak_object_set *out) {
    memset(out, 0, sizeof *out);
    if (r->has_vclock && (out->vclock = riak_pb_b64_encode(r->vclock)) == NULL) {
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    if ((out->items = calloc(r->n_content, sizeof *out->items)) == NULL) {
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    for (size_t i = 0; i < r->n_content; i++) {
        riak_status st =
            content_to_object(c, r->content[i], r->has_vclock ? &r->vclock : NULL, &out->items[i]);
        if (st != RIAK_OK) {
            return st;
        }
        out->count++;
    }
    return RIAK_OK;
}

/* No content: not found, or unchanged for a conditional get. */
static riak_status empty_get(riak_client *c, const RpbGetResp *r) {
    if (r->has_unchanged && r->unchanged) {
        return riak_set_error(c, RIAK_E_NOT_MODIFIED, "not modified");
    }
    return riak_set_error(c, RIAK_E_NOT_FOUND, "not found");
}

riak_status riak_pb_finish_get(riak_client *c, const riak_opts *o, riak_pb_response *resp,
                               riak_object *out) {
    RpbGetResp *r = rpb_get_resp__unpack(NULL, resp->body.len, (const uint8_t *)resp->body.data);
    if (r == NULL) {
        return riak_set_error(c, RIAK_E_PARSE, "malformed RpbGetResp");
    }
    riak_status st;
    if (r->n_content == 0 ||
        (r->n_content == 1 && r->content[0]->has_deleted && r->content[0]->deleted)) {
        st = empty_get(c, r);
    } else if (r->n_content == 1) {
        st = content_to_object(c, r->content[0], r->has_vclock ? &r->vclock : NULL, out);
    } else if (o->resolver == NULL) {
        st = riak_set_error(c, RIAK_E_SIBLINGS, "object has %zu siblings", r->n_content);
    } else {
        riak_object_set set;
        st = to_set(c, r, &set);
        if (st == RIAK_OK) {
            size_t pick = o->resolver(set.items, set.count, o->resolver_ctx);
            if (pick < set.count) {
                *out = set.items[pick];
                memset(&set.items[pick], 0, sizeof set.items[pick]);
            } else {
                st =
                    riak_set_error(c, RIAK_E_SIBLINGS, "resolver declined %zu siblings", set.count);
            }
        }
        riak_object_set_free(&set);
    }
    rpb_get_resp__free_unpacked(r, NULL);
    return st;
}

static riak_status run_prepared(riak_client *c, riak_status st, riak_pb_prepared *p,
                                riak_pb_response *resp) {
    if (st == RIAK_OK) {
        st = riak_pb_perform(c, &p->req, resp);
    }
    riak_pb_prepared_free(p);
    return st;
}

riak_status riak_pb_get(riak_client *c, const char *bucket, const char *key, const riak_opts *o,
                        riak_object *out) {
    riak_pb_prepared p;
    riak_pb_response resp = {0};
    riak_status st = run_prepared(c, riak_pb_prepare_get(c, bucket, key, o, &p), &p, &resp);
    if (st == RIAK_OK) {
        st = riak_pb_finish_get(c, o, &resp, out);
    }
    riak_buf_free(&resp.body);
    return st;
}

riak_status riak_pb_get_siblings(riak_client *c, const char *bucket, const char *key,
                                 const riak_opts *o, riak_object_set *out) {
    riak_pb_prepared p;
    riak_pb_response resp = {0};
    riak_status st = run_prepared(c, riak_pb_prepare_get(c, bucket, key, o, &p), &p, &resp);
    if (st != RIAK_OK) {
        return st;
    }
    RpbGetResp *r = rpb_get_resp__unpack(NULL, resp.body.len, (const uint8_t *)resp.body.data);
    riak_buf_free(&resp.body);
    if (r == NULL) {
        return riak_set_error(c, RIAK_E_PARSE, "malformed RpbGetResp");
    }
    if (r->n_content == 0 ||
        (r->n_content == 1 && r->content[0]->has_deleted && r->content[0]->deleted)) {
        st = empty_get(c, r);
    } else {
        st = to_set(c, r, out);
    }
    rpb_get_resp__free_unpacked(r, NULL);
    if (st != RIAK_OK) {
        riak_object_set_free(out);
    }
    return st;
}

/* ---- put ------------------------------------------------------------- */

riak_status riak_pb_prepare_put(riak_client *c, const char *bucket, const char *key,
                                const void *value, size_t len, const riak_opts *o,
                                riak_pb_prepared *p) {
    memset(p, 0, sizeof *p);
    if (value == NULL && len > 0) {
        return riak_set_error(c, RIAK_E_INVALID_ARG, "value is NULL");
    }
    riak_status st = check_object(c, bucket, key, o);
    if (st != RIAK_OK) {
        return st;
    }
    if (o->if_none_match && strcmp(o->if_none_match, "*") != 0) {
        return unsupported(c, "an ETag in if_none_match on put");
    }
    if (o->if_match) return unsupported(c, "if_match (use if_not_modified)");
    if (o->if_modified_since) return unsupported(c, "if_modified_since");
    if (o->if_unmodified_since) return unsupported(c, "if_unmodified_since");
    content_build cb;
    st = build_content(c, value, len, o, &cb);
    RpbPutReq m = RPB_PUT_REQ__INIT;
    ProtobufCBinaryData vc = {0};
    if (st == RIAK_OK) {
        m.bucket = riak_pb_bin(bucket);
        if (key) {
            m.has_key = 1;
            m.key = riak_pb_bin(key);
        }
        m.content = &cb.content;
        if (!QUORUM(w, o->w) || !QUORUM(dw, o->dw) || !QUORUM(pw, o->pw)) {
            st = riak_set_error(c, RIAK_E_INVALID_ARG, "invalid quorum");
        }
    }
    if (st == RIAK_OK && o->vclock) {
        if (!riak_pb_b64_decode(o->vclock, &vc)) {
            st = riak_set_error(c, RIAK_E_INVALID_ARG, "invalid vclock");
        } else {
            m.has_vclock = 1;
            m.vclock = vc;
        }
    }
    if (st == RIAK_OK && o->if_not_modified && !m.has_vclock) {
        st = riak_set_error(c, RIAK_E_INVALID_ARG, "if_not_modified needs a vclock");
    }
    if (st == RIAK_OK) {
        m.has_return_body = o->return_body;
        m.return_body = o->return_body;
        m.has_if_not_modified = o->if_not_modified;
        m.if_not_modified = o->if_not_modified;
        m.has_if_none_match = o->if_none_match != NULL;
        m.if_none_match = o->if_none_match != NULL;
        m.has_timeout = o->server_timeout_ms > 0;
        m.timeout = o->server_timeout_ms;
        m.has_sloppy_quorum = o->sloppy_quorum;
        m.sloppy_quorum = o->sloppy_quorum;
        m.has_n_val = o->n_val > 0;
        m.n_val = o->n_val;
        if (o->bucket_type) {
            m.has_type = 1;
            m.type = riak_pb_bin(o->bucket_type);
        }
        p->req = (riak_pb_request){.code = RIAK_PB_PUT_REQ,
                                   .resp_code = RIAK_PB_PUT_RESP,
                                   /* a generated key would create a second object */
                                   .idempotent = key != NULL,
                                   .name = "RpbPutReq"};
        st = finish_prepare(c, &m.base, riak_pb_path(c, o->bucket_type, bucket, "keys", key), p);
    }
    free(vc.data);
    content_build_free(&cb);
    if (st != RIAK_OK) {
        riak_pb_prepared_free(p);
    }
    return st;
}

riak_status riak_pb_finish_put(riak_client *c, riak_pb_response *resp, riak_object *out,
                               char **created_key) {
    RpbPutResp *r = rpb_put_resp__unpack(NULL, resp->body.len, (const uint8_t *)resp->body.data);
    if (r == NULL) {
        return riak_set_error(c, RIAK_E_PARSE, "malformed RpbPutResp");
    }
    riak_status st = RIAK_OK;
    if (created_key) {
        if (!r->has_key || (*created_key = riak_pb_strdup(r->key)) == NULL) {
            st = r->has_key ? riak_set_error(c, RIAK_E_NOMEM, "out of memory")
                            : riak_set_error(c, RIAK_E_PARSE, "no generated key in the response");
        }
    }
    if (st == RIAK_OK && out && r->n_content > 1) {
        st = riak_set_error(c, RIAK_E_SIBLINGS, "object has %zu siblings", r->n_content);
    } else if (st == RIAK_OK && out && r->n_content == 1) {
        st = content_to_object(c, r->content[0], r->has_vclock ? &r->vclock : NULL, out);
    }
    if (st != RIAK_OK && created_key) {
        free(*created_key);
        *created_key = NULL;
    }
    rpb_put_resp__free_unpacked(r, NULL);
    return st;
}

riak_status riak_pb_put(riak_client *c, const char *bucket, const char *key, const void *value,
                        size_t len, const riak_opts *o, riak_object *out, char **created_key) {
    riak_pb_prepared p;
    riak_pb_response resp = {0};
    riak_status st =
        run_prepared(c, riak_pb_prepare_put(c, bucket, key, value, len, o, &p), &p, &resp);
    if (st == RIAK_OK) {
        st = riak_pb_finish_put(c, &resp, out, key == NULL ? created_key : NULL);
    }
    riak_buf_free(&resp.body);
    return st;
}

/* ---- delete ---------------------------------------------------------- */

riak_status riak_pb_prepare_delete(riak_client *c, const char *bucket, const char *key,
                                   const riak_opts *o, riak_pb_prepared *p) {
    memset(p, 0, sizeof *p);
    riak_status st = check_object(c, bucket, key, o);
    if (st == RIAK_OK && key == NULL) {
        st = riak_check_name(c, key, "key");
    }
    if (st != RIAK_OK) {
        return st;
    }
    RpbDelReq m = RPB_DEL_REQ__INIT;
    m.bucket = riak_pb_bin(bucket);
    m.key = riak_pb_bin(key);
    if (!QUORUM(rw, o->rw) || !QUORUM(r, o->r) || !QUORUM(w, o->w) || !QUORUM(pr, o->pr) ||
        !QUORUM(pw, o->pw) || !QUORUM(dw, o->dw)) {
        return riak_set_error(c, RIAK_E_INVALID_ARG, "invalid quorum");
    }
    ProtobufCBinaryData vc = {0};
    if (o->vclock) {
        if (!riak_pb_b64_decode(o->vclock, &vc)) {
            return riak_set_error(c, RIAK_E_INVALID_ARG, "invalid vclock");
        }
        m.has_vclock = 1;
        m.vclock = vc;
    }
    m.has_timeout = o->server_timeout_ms > 0;
    m.timeout = o->server_timeout_ms;
    m.has_sloppy_quorum = o->sloppy_quorum;
    m.sloppy_quorum = o->sloppy_quorum;
    m.has_n_val = o->n_val > 0;
    m.n_val = o->n_val;
    if (o->bucket_type) {
        m.has_type = 1;
        m.type = riak_pb_bin(o->bucket_type);
    }
    p->req = (riak_pb_request){.code = RIAK_PB_DEL_REQ,
                               .resp_code = RIAK_PB_DEL_RESP,
                               .idempotent = true,
                               .name = "RpbDelReq"};
    st = finish_prepare(c, &m.base, riak_pb_path(c, o->bucket_type, bucket, "keys", key), p);
    free(vc.data);
    if (st != RIAK_OK) {
        riak_pb_prepared_free(p);
    }
    return st;
}

riak_status riak_pb_delete(riak_client *c, const char *bucket, const char *key,
                           const riak_opts *o) {
    riak_pb_prepared p;
    riak_pb_response resp = {0};
    riak_status st = run_prepared(c, riak_pb_prepare_delete(c, bucket, key, o, &p), &p, &resp);
    riak_buf_free(&resp.body);
    if (st == RIAK_E_NOT_FOUND) {
        riak_clear_error(c); /* deleting an absent key succeeds, as over HTTP */
        st = RIAK_OK;
    }
    return st;
}

/* ---- ping, server info ---------------------------------------------- */

riak_status riak_pb_prepare_ping(riak_client *c, riak_pb_prepared *p) {
    memset(p, 0, sizeof *p);
    p->path = riak_strdup("/ping");
    if (p->path == NULL) {
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    p->req = (riak_pb_request){.code = RIAK_PB_PING_REQ,
                               .resp_code = RIAK_PB_PING_RESP,
                               .idempotent = true,
                               .name = "RpbPingReq",
                               .path = p->path};
    return RIAK_OK;
}

riak_status riak_pb_ping(riak_client *c, size_t pin) {
    riak_pb_call call = {.code = RIAK_PB_PING_REQ,
                         .resp_code = RIAK_PB_PING_RESP,
                         .idempotent = true,
                         .pin = pin,
                         .name = "RpbPingReq",
                         .path = "/ping"};
    return riak_pb_exec(c, &call, NULL, NULL, NULL);
}

riak_status riak_pb_server_info(riak_client *c, riak_server_info *out) {
    riak_pb_call call = {.code = RIAK_PB_GET_SERVER_INFO_REQ,
                         .resp_code = RIAK_PB_GET_SERVER_INFO_RESP,
                         .idempotent = true,
                         .name = "RpbGetServerInfoReq",
                         .path = "/stats"};
    ProtobufCMessage *msg;
    riak_status st = riak_pb_exec(c, &call, NULL, &rpb_get_server_info_resp__descriptor, &msg);
    if (st != RIAK_OK) {
        return st;
    }
    RpbGetServerInfoResp *r = (RpbGetServerInfoResp *)msg;
    out->node = r->has_node ? riak_pb_strdup(r->node) : riak_strdup("");
    out->version = r->has_server_version ? riak_pb_strdup(r->server_version) : riak_strdup("");
    rpb_get_server_info_resp__free_unpacked(r, NULL);
    if (out->node == NULL || out->version == NULL) {
        riak_server_info_free(out);
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    return RIAK_OK;
}

/* ---- listings -------------------------------------------------------- */

typedef struct {
    riak_name_cb cb;
    void *ctx;
    bool buckets;
} list_ctx;

static bool list_sink(void *ctx, const uint8_t *data, size_t len, bool *done, riak_status *err) {
    list_ctx *l = ctx;
    ProtobufCBinaryData *names;
    size_t n;
    protobuf_c_boolean has_done, is_done;
    ProtobufCMessage *msg;
    if (l->buckets) {
        RpbListBucketsResp *r = rpb_list_buckets_resp__unpack(NULL, len, data);
        msg = (ProtobufCMessage *)r;
        if (r == NULL) {
            *err = RIAK_E_PARSE;
            return false;
        }
        names = r->buckets;
        n = r->n_buckets;
        has_done = r->has_done;
        is_done = r->done;
    } else {
        RpbListKeysResp *r = rpb_list_keys_resp__unpack(NULL, len, data);
        msg = (ProtobufCMessage *)r;
        if (r == NULL) {
            *err = RIAK_E_PARSE;
            return false;
        }
        names = r->keys;
        n = r->n_keys;
        has_done = r->has_done;
        is_done = r->done;
    }
    bool more = true;
    for (size_t i = 0; more && i < n; i++) {
        char *s = riak_pb_strdup(names[i]);
        if (s == NULL) {
            *err = RIAK_E_NOMEM;
            protobuf_c_message_free_unpacked(msg, NULL);
            return false;
        }
        more = l->cb(s, names[i].len, l->ctx);
        free(s);
    }
    *done = has_done && is_done;
    protobuf_c_message_free_unpacked(msg, NULL);
    return more;
}

riak_status riak_pb_list_buckets(riak_client *c, const char *type, riak_name_cb cb, void *ctx) {
    RpbListBucketsReq m = RPB_LIST_BUCKETS_REQ__INIT;
    m.has_stream = 1;
    m.stream = 1;
    if (type) {
        m.has_type = 1;
        m.type = riak_pb_bin(type);
    }
    riak_pb_call call = {.code = RIAK_PB_LIST_BUCKETS_REQ,
                         .resp_code = RIAK_PB_LIST_BUCKETS_RESP,
                         .idempotent = true,
                         .name = "RpbListBucketsReq",
                         .path = "/buckets?buckets=stream"};
    list_ctx l = {.cb = cb, .ctx = ctx, .buckets = true};
    return riak_pb_stream(c, &call, &m.base, list_sink, &l);
}

riak_status riak_pb_list_keys(riak_client *c, const char *type, const char *bucket, riak_name_cb cb,
                              void *ctx) {
    RpbListKeysReq m = RPB_LIST_KEYS_REQ__INIT;
    m.bucket = riak_pb_bin(bucket);
    if (type) {
        m.has_type = 1;
        m.type = riak_pb_bin(type);
    }
    char *path = riak_pb_path(c, type, bucket, "keys", NULL);
    if (path == NULL) {
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    riak_pb_call call = {.code = RIAK_PB_LIST_KEYS_REQ,
                         .resp_code = RIAK_PB_LIST_KEYS_RESP,
                         .idempotent = true,
                         .name = "RpbListKeysReq",
                         .path = path};
    list_ctx l = {.cb = cb, .ctx = ctx, .buckets = false};
    riak_status st = riak_pb_stream(c, &call, &m.base, list_sink, &l);
    free(path);
    return st;
}

/* ---- bucket properties <-> JSON ------------------------------------- */

static json_object *quorum_json(uint32_t q) {
    switch (q) {
    case RIAK_PB_Q_ONE:
        return json_object_new_string("one");
    case RIAK_PB_Q_QUORUM:
        return json_object_new_string("quorum");
    case RIAK_PB_Q_ALL:
        return json_object_new_string("all");
    case RIAK_PB_Q_ALL - 1:
        return json_object_new_string("default");
    default:
        return json_object_new_int64(q);
    }
}

static json_object *modfun_json(const RpbModFun *mf) {
    json_object *o = json_object_new_object();
    if (o) {
        json_object_object_add(
            o, "mod",
            json_object_new_string_len((const char *)mf->module.data, (int)mf->module.len));
        json_object_object_add(
            o, "fun",
            json_object_new_string_len((const char *)mf->function.data, (int)mf->function.len));
    }
    return o;
}

static json_object *hooks_json(RpbCommitHook *const *hooks, size_t n) {
    json_object *a = json_object_new_array();
    for (size_t i = 0; a && i < n; i++) {
        json_object *h;
        if (hooks[i]->modfun) {
            h = modfun_json(hooks[i]->modfun);
        } else {
            h = json_object_new_object();
            if (h && hooks[i]->has_name) {
                json_object_object_add(h, "name",
                                       json_object_new_string_len((const char *)hooks[i]->name.data,
                                                                  (int)hooks[i]->name.len));
            }
        }
        json_object_array_add(a, h);
    }
    return a;
}

static json_object *bytes_json(ProtobufCBinaryData b) {
    return json_object_new_string_len((const char *)b.data, (int)b.len);
}

static char *props_to_json(const RpbBucketProps *p, const char *name) {
    json_object *root = json_object_new_object();
    json_object *o = json_object_new_object();
    if (root == NULL || o == NULL) {
        json_object_put(root);
        json_object_put(o);
        return NULL;
    }
    json_object_object_add(root, "props", o);
    if (name) json_object_object_add(o, "name", json_object_new_string(name));
#define U32(f)                                                                                     \
    if (p->has_##f) json_object_object_add(o, #f, json_object_new_int64(p->f))
#define BOOL(f)                                                                                    \
    if (p->has_##f) json_object_object_add(o, #f, json_object_new_boolean(p->f))
#define QRM(f)                                                                                     \
    if (p->has_##f) json_object_object_add(o, #f, quorum_json(p->f))
#define BYTES(f)                                                                                   \
    if (p->has_##f) json_object_object_add(o, #f, bytes_json(p->f))
    U32(n_val);
    BOOL(allow_mult);
    BOOL(last_write_wins);
    if (p->n_precommit || (p->has_has_precommit && p->has_precommit))
        json_object_object_add(o, "precommit", hooks_json(p->precommit, p->n_precommit));
    if (p->n_postcommit || (p->has_has_postcommit && p->has_postcommit))
        json_object_object_add(o, "postcommit", hooks_json(p->postcommit, p->n_postcommit));
    if (p->chash_keyfun) json_object_object_add(o, "chash_keyfun", modfun_json(p->chash_keyfun));
    if (p->linkfun) json_object_object_add(o, "linkfun", modfun_json(p->linkfun));
    U32(old_vclock);
    U32(young_vclock);
    U32(big_vclock);
    U32(small_vclock);
    QRM(pr);
    QRM(r);
    QRM(w);
    QRM(pw);
    QRM(dw);
    QRM(rw);
    BOOL(basic_quorum);
    BOOL(notfound_ok);
    BYTES(backend);
    BOOL(search);
    if (p->has_repl) {
        static const char *const modes[] = {"false", "realtime", "fullsync", "true"};
        unsigned m = (unsigned)p->repl;
        json_object_object_add(o, "repl", json_object_new_string(m < 4 ? modes[m] : "false"));
    }
    BYTES(search_index);
    BYTES(datatype);
    BOOL(consistent);
    BOOL(write_once);
    U32(hll_precision);
#undef U32
#undef BOOL
#undef QRM
#undef BYTES
    char *s = riak_strdup(json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN));
    json_object_put(root);
    return s;
}

/* Allocations made while turning JSON into RpbBucketProps. */
typedef struct {
    RpbBucketProps props;
    RpbModFun modfuns[2];
    RpbModFun *hook_mf;
    RpbCommitHook *hooks;
    RpbCommitHook **hook_p;
    size_t n_hooks;
    size_t pre_off, post_off; /* where each hook list starts in hooks */
} props_build;

static void props_build_free(props_build *b) {
    free(b->hook_mf);
    free(b->hooks);
    free((void *)b->hook_p);
}

static bool quorum_from_json(json_object *v, uint32_t *out) {
    if (json_object_is_type(v, json_type_int)) {
        int64_t n = json_object_get_int64(v);
        if (n < 0 || n > UINT32_MAX) return false;
        *out = (uint32_t)n;
        return true;
    }
    if (!json_object_is_type(v, json_type_string)) return false;
    const char *s = json_object_get_string(v);
    if (strcmp(s, "one") == 0)
        *out = RIAK_PB_Q_ONE;
    else if (strcmp(s, "quorum") == 0)
        *out = RIAK_PB_Q_QUORUM;
    else if (strcmp(s, "all") == 0)
        *out = RIAK_PB_Q_ALL;
    else if (strcmp(s, "default") == 0)
        *out = RIAK_PB_Q_ALL - 1;
    else
        return false;
    return true;
}

static bool modfun_from_json(json_object *v, RpbModFun *mf) {
    json_object *mod, *fun;
    if (!json_object_is_type(v, json_type_object) || !json_object_object_get_ex(v, "mod", &mod) ||
        !json_object_object_get_ex(v, "fun", &fun) || !json_object_is_type(mod, json_type_string) ||
        !json_object_is_type(fun, json_type_string)) {
        return false;
    }
    rpb_mod_fun__init(mf);
    mf->module = riak_pb_bin(json_object_get_string(mod));
    mf->function = riak_pb_bin(json_object_get_string(fun));
    return true;
}

/* -1 invalid, -2 ENOMEM */
static int hooks_from_json(json_object *v, props_build *b, size_t *off, size_t *n) {
    if (!json_object_is_type(v, json_type_array)) return -1;
    size_t count = json_object_array_length(v);
    size_t base = b->n_hooks;
    RpbCommitHook *hooks = realloc(b->hooks, (base + count + 1) * sizeof *hooks);
    if (hooks == NULL) return -2;
    b->hooks = hooks;
    RpbModFun *mfs = realloc(b->hook_mf, (base + count + 1) * sizeof *mfs);
    if (mfs == NULL) return -2;
    b->hook_mf = mfs;
    for (size_t i = 0; i < count; i++) {
        json_object *h = json_object_array_get_idx(v, i);
        RpbCommitHook *hk = &b->hooks[base + i];
        rpb_commit_hook__init(hk);
        json_object *name;
        if (json_object_is_type(h, json_type_object) &&
            json_object_object_get_ex(h, "name", &name) &&
            json_object_is_type(name, json_type_string)) {
            hk->has_name = 1;
            hk->name = riak_pb_bin(json_object_get_string(name));
        } else if (modfun_from_json(h, &b->hook_mf[base + i])) {
            hk->modfun = &b->hook_mf[base + i];
        } else {
            return -1;
        }
    }
    b->n_hooks = base + count;
    *n = count;
    /* pointers are filled in once every hook list is parsed (realloc moves them) */
    *off = base;
    return 0;
}

static riak_status props_from_json(riak_client *c, const char *json, json_object **root_out,
                                   props_build *b) {
    memset(b, 0, sizeof *b);
    RpbBucketProps *p = &b->props;
    rpb_bucket_props__init(p);
    json_object *root = json_tokener_parse(json);
    *root_out = root;
    json_object *props;
    if (root == NULL || !json_object_is_type(root, json_type_object) ||
        !json_object_object_get_ex(root, "props", &props) ||
        !json_object_is_type(props, json_type_object)) {
        return riak_set_error(c, RIAK_E_INVALID_ARG, "properties must be {\"props\":{...}}");
    }
    json_object_object_foreach(props, k, v) {
        bool ok = true;
        bool known = true;
#define IS(name) (strcmp(k, name) == 0)
        if (IS("name")) {
            /* read-only, returned by a get */
        } else if (IS("n_val") || IS("old_vclock") || IS("young_vclock") || IS("big_vclock") ||
                   IS("small_vclock") || IS("hll_precision")) {
            int64_t n = json_object_get_int64(v);
            ok = json_object_is_type(v, json_type_int) && n >= 0 && n <= UINT32_MAX;
            uint32_t u = (uint32_t)n;
            if (IS("n_val")) {
                p->has_n_val = 1;
                p->n_val = u;
            } else if (IS("old_vclock")) {
                p->has_old_vclock = 1;
                p->old_vclock = u;
            } else if (IS("young_vclock")) {
                p->has_young_vclock = 1;
                p->young_vclock = u;
            } else if (IS("big_vclock")) {
                p->has_big_vclock = 1;
                p->big_vclock = u;
            } else if (IS("small_vclock")) {
                p->has_small_vclock = 1;
                p->small_vclock = u;
            } else {
                p->has_hll_precision = 1;
                p->hll_precision = u;
            }
        } else if (IS("allow_mult") || IS("last_write_wins") || IS("basic_quorum") ||
                   IS("notfound_ok") || IS("search") || IS("consistent") || IS("write_once")) {
            ok = json_object_is_type(v, json_type_boolean);
            protobuf_c_boolean bv = json_object_get_boolean(v);
            if (IS("allow_mult")) {
                p->has_allow_mult = 1;
                p->allow_mult = bv;
            } else if (IS("last_write_wins")) {
                p->has_last_write_wins = 1;
                p->last_write_wins = bv;
            } else if (IS("basic_quorum")) {
                p->has_basic_quorum = 1;
                p->basic_quorum = bv;
            } else if (IS("notfound_ok")) {
                p->has_notfound_ok = 1;
                p->notfound_ok = bv;
            } else if (IS("search")) {
                p->has_search = 1;
                p->search = bv;
            } else if (IS("consistent")) {
                p->has_consistent = 1;
                p->consistent = bv;
            } else {
                p->has_write_once = 1;
                p->write_once = bv;
            }
        } else if (IS("pr") || IS("r") || IS("w") || IS("pw") || IS("dw") || IS("rw")) {
            uint32_t q = 0;
            ok = quorum_from_json(v, &q);
            if (IS("pr")) {
                p->has_pr = 1;
                p->pr = q;
            } else if (IS("r")) {
                p->has_r = 1;
                p->r = q;
            } else if (IS("w")) {
                p->has_w = 1;
                p->w = q;
            } else if (IS("pw")) {
                p->has_pw = 1;
                p->pw = q;
            } else if (IS("dw")) {
                p->has_dw = 1;
                p->dw = q;
            } else {
                p->has_rw = 1;
                p->rw = q;
            }
        } else if (IS("backend") || IS("search_index") || IS("datatype")) {
            ok = json_object_is_type(v, json_type_string);
            ProtobufCBinaryData bin = riak_pb_bin(json_object_get_string(v));
            if (IS("backend")) {
                p->has_backend = 1;
                p->backend = bin;
            } else if (IS("search_index")) {
                p->has_search_index = 1;
                p->search_index = bin;
            } else {
                p->has_datatype = 1;
                p->datatype = bin;
            }
        } else if (IS("repl")) {
            const char *s =
                json_object_is_type(v, json_type_string) ? json_object_get_string(v) : "";
            p->has_repl = 1;
            if (json_object_is_type(v, json_type_boolean)) {
                p->repl = json_object_get_boolean(v) ? RPB_BUCKET_PROPS__RPB_REPL_MODE__TRUE
                                                     : RPB_BUCKET_PROPS__RPB_REPL_MODE__FALSE;
            } else if (strcmp(s, "false") == 0)
                p->repl = RPB_BUCKET_PROPS__RPB_REPL_MODE__FALSE;
            else if (strcmp(s, "realtime") == 0)
                p->repl = RPB_BUCKET_PROPS__RPB_REPL_MODE__REALTIME;
            else if (strcmp(s, "fullsync") == 0)
                p->repl = RPB_BUCKET_PROPS__RPB_REPL_MODE__FULLSYNC;
            else if (strcmp(s, "true") == 0)
                p->repl = RPB_BUCKET_PROPS__RPB_REPL_MODE__TRUE;
            else
                ok = false;
        } else if (IS("chash_keyfun")) {
            ok = modfun_from_json(v, &b->modfuns[0]);
            p->chash_keyfun = &b->modfuns[0];
        } else if (IS("linkfun")) {
            ok = modfun_from_json(v, &b->modfuns[1]);
            p->linkfun = &b->modfuns[1];
        } else if (IS("precommit") || IS("postcommit")) {
            bool pre = IS("precommit");
            int rc = hooks_from_json(v, b, pre ? &b->pre_off : &b->post_off,
                                     pre ? &p->n_precommit : &p->n_postcommit);
            if (rc == -2) return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
            ok = rc == 0;
            if (pre) {
                p->has_has_precommit = 1;
                p->has_precommit = p->n_precommit > 0;
            } else {
                p->has_has_postcommit = 1;
                p->has_postcommit = p->n_postcommit > 0;
            }
        } else {
            known = false;
        }
#undef IS
        if (!known) {
            return riak_set_error(c, RIAK_E_INVALID_ARG,
                                  "property \"%s\" cannot be set over Protocol Buffers", k);
        }
        if (!ok) {
            return riak_set_error(c, RIAK_E_INVALID_ARG, "invalid value for property \"%s\"", k);
        }
    }
    /* hook lists are slices of b->hooks */
    if (b->n_hooks) {
        b->hook_p = (RpbCommitHook **)calloc(b->n_hooks, sizeof *b->hook_p);
        if (b->hook_p == NULL) return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
        for (size_t i = 0; i < b->n_hooks; i++) {
            b->hook_p[i] = &b->hooks[i];
            if (b->hooks[i].modfun) {
                b->hooks[i].modfun = &b->hook_mf[i];
            }
        }
    }
    p->precommit = p->n_precommit ? b->hook_p + b->pre_off : NULL;
    p->postcommit = p->n_postcommit ? b->hook_p + b->post_off : NULL;
    return RIAK_OK;
}

static riak_status bucket_props_call(riak_client *c, uint8_t code, const char *name,
                                     const ProtobufCMessage *m, const char *path, char **json,
                                     const char *bucket) {
    riak_pb_call call = {.code = code,
                         .resp_code = RIAK_PB_GET_BUCKET_RESP,
                         .idempotent = true,
                         .name = name,
                         .path = path};
    ProtobufCMessage *msg;
    riak_status st = riak_pb_exec(c, &call, m, &rpb_get_bucket_resp__descriptor, &msg);
    if (st != RIAK_OK) {
        return st;
    }
    RpbGetBucketResp *r = (RpbGetBucketResp *)msg;
    *json = props_to_json(r->props, bucket);
    rpb_get_bucket_resp__free_unpacked(r, NULL);
    return *json ? RIAK_OK : riak_set_error(c, RIAK_E_NOMEM, "out of memory");
}

riak_status riak_pb_get_bucket_props(riak_client *c, const char *type, const char *bucket,
                                     char **json) {
    RpbGetBucketReq m = RPB_GET_BUCKET_REQ__INIT;
    m.bucket = riak_pb_bin(bucket);
    if (type) {
        m.has_type = 1;
        m.type = riak_pb_bin(type);
    }
    char *path = riak_pb_path(c, type, bucket, "props", NULL);
    if (path == NULL) return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    riak_status st = bucket_props_call(c, RIAK_PB_GET_BUCKET_REQ, "RpbGetBucketReq", &m.base, path,
                                       json, bucket);
    free(path);
    return st;
}

riak_status riak_pb_get_type_props(riak_client *c, const char *type, char **json) {
    RpbGetBucketTypeReq m = RPB_GET_BUCKET_TYPE_REQ__INIT;
    m.type = riak_pb_bin(type);
    char *path = riak_strprintf("/types/%s/props", type);
    if (path == NULL) return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    riak_status st = bucket_props_call(c, RIAK_PB_GET_BUCKET_TYPE_REQ, "RpbGetBucketTypeReq",
                                       &m.base, path, json, NULL);
    free(path);
    return st;
}

riak_status riak_pb_set_bucket_props(riak_client *c, const char *type, const char *bucket,
                                     const char *json) {
    json_object *root = NULL;
    props_build b;
    riak_status st = props_from_json(c, json, &root, &b);
    char *path = NULL;
    if (st == RIAK_OK) {
        path = riak_pb_path(c, type, bucket, "props", NULL);
        if (path == NULL) st = riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    if (st == RIAK_OK) {
        RpbSetBucketReq m = RPB_SET_BUCKET_REQ__INIT;
        m.bucket = riak_pb_bin(bucket);
        m.props = &b.props;
        if (type) {
            m.has_type = 1;
            m.type = riak_pb_bin(type);
        }
        riak_pb_call call = {.code = RIAK_PB_SET_BUCKET_REQ,
                             .resp_code = RIAK_PB_SET_BUCKET_RESP,
                             .idempotent = true,
                             .name = "RpbSetBucketReq",
                             .path = path};
        st = riak_pb_exec(c, &call, &m.base, NULL, NULL);
    }
    free(path);
    props_build_free(&b);
    json_object_put(root);
    return st;
}

riak_status riak_pb_reset_bucket_props(riak_client *c, const char *type, const char *bucket) {
    RpbResetBucketReq m = RPB_RESET_BUCKET_REQ__INIT;
    m.bucket = riak_pb_bin(bucket);
    if (type) {
        m.has_type = 1;
        m.type = riak_pb_bin(type);
    }
    char *path = riak_pb_path(c, type, bucket, "props", NULL);
    if (path == NULL) return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    riak_pb_call call = {.code = RIAK_PB_RESET_BUCKET_REQ,
                         .resp_code = RIAK_PB_RESET_BUCKET_RESP,
                         .idempotent = true,
                         .name = "RpbResetBucketReq",
                         .path = path};
    riak_status st = riak_pb_exec(c, &call, &m.base, NULL, NULL);
    free(path);
    return st;
}

/* ---- preference lists, coverage ------------------------------------- */

riak_status riak_pb_preflist(riak_client *c, const char *type, const char *bucket, const char *key,
                             riak_preflist *out) {
    RpbGetBucketKeyPreflistReq m = RPB_GET_BUCKET_KEY_PREFLIST_REQ__INIT;
    m.bucket = riak_pb_bin(bucket);
    m.key = riak_pb_bin(key);
    if (type) {
        m.has_type = 1;
        m.type = riak_pb_bin(type);
    }
    char *path = riak_pb_path(c, type, bucket, "keys", key);
    if (path == NULL) return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    riak_pb_call call = {.code = RIAK_PB_GET_PREFLIST_REQ,
                         .resp_code = RIAK_PB_GET_PREFLIST_RESP,
                         .idempotent = true,
                         .name = "RpbGetBucketKeyPreflistReq",
                         .path = path};
    ProtobufCMessage *msg;
    riak_status st =
        riak_pb_exec(c, &call, &m.base, &rpb_get_bucket_key_preflist_resp__descriptor, &msg);
    free(path);
    if (st != RIAK_OK) {
        return st;
    }
    RpbGetBucketKeyPreflistResp *r = (RpbGetBucketKeyPreflistResp *)msg;
    size_t n = r->n_preflist;
    if ((out->items = calloc(n ? n : 1, sizeof *out->items)) == NULL) {
        rpb_get_bucket_key_preflist_resp__free_unpacked(r, NULL);
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    for (size_t i = 0; st == RIAK_OK && i < n; i++) {
        riak_preflist_item *it = &out->items[i];
        it->partition = r->preflist[i]->partition;
        it->primary = r->preflist[i]->primary;
        out->count++;
        if ((it->node = riak_pb_strdup(r->preflist[i]->node)) == NULL) {
            st = riak_set_error(c, RIAK_E_NOMEM, "out of memory");
        }
    }
    rpb_get_bucket_key_preflist_resp__free_unpacked(r, NULL);
    if (st != RIAK_OK) {
        riak_preflist_free(out);
    }
    return st;
}

riak_status riak_pb_coverage(riak_client *c, const char *type, const char *bucket,
                             unsigned min_partitions, riak_coverage_plan *out) {
    RpbCoverageReq m = RPB_COVERAGE_REQ__INIT;
    m.bucket = riak_pb_bin(bucket);
    if (type) {
        m.has_type = 1;
        m.type = riak_pb_bin(type);
    }
    m.has_min_partitions = min_partitions > 0;
    m.min_partitions = min_partitions;
    char *path = riak_pb_path(c, type, bucket, "coverage", NULL);
    if (path == NULL) return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    riak_pb_call call = {.code = RIAK_PB_COVERAGE_REQ,
                         .resp_code = RIAK_PB_COVERAGE_RESP,
                         .idempotent = true,
                         .name = "RpbCoverageReq",
                         .path = path};
    ProtobufCMessage *msg;
    riak_status st = riak_pb_exec(c, &call, &m.base, &rpb_coverage_resp__descriptor, &msg);
    free(path);
    if (st != RIAK_OK) {
        return st;
    }
    RpbCoverageResp *r = (RpbCoverageResp *)msg;
    size_t n = r->n_entries;
    if ((out->entries = calloc(n ? n : 1, sizeof *out->entries)) == NULL) {
        rpb_coverage_resp__free_unpacked(r, NULL);
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    for (size_t i = 0; st == RIAK_OK && i < n; i++) {
        const RpbCoverageEntry *e = r->entries[i];
        riak_coverage_entry *d = &out->entries[i];
        out->count++;
        d->port = e->port;
        d->host = riak_pb_strdup(e->ip);
        d->description = e->has_keyspace_desc ? riak_pb_strdup(e->keyspace_desc) : riak_strdup("");
        d->cover_context = riak_pb_b64_encode(e->cover_context);
        if (d->host == NULL || d->description == NULL || d->cover_context == NULL) {
            st = riak_set_error(c, RIAK_E_NOMEM, "out of memory");
        }
    }
    rpb_coverage_resp__free_unpacked(r, NULL);
    if (st != RIAK_OK) {
        riak_coverage_free(out);
    }
    return st;
}
