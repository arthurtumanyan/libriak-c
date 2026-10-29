/*
 File:   ext.c
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later

 Bucket-type properties, client id, Riak 1.4 counters, object folds, link
 walking and the replication calls (nextgenrepl queues, riak_repl). Each
 call picks the transport's implementation; what a transport or node
 cannot do is reported as RIAK_E_UNSUPPORTED.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "internal.h"
#include "multipart.h"
#include "pb.h"

static const riak_opts no_opts;

static riak_status needs_pb(riak_client *c, const char *what) {
    return riak_set_error(c, RIAK_E_UNSUPPORTED, "%s needs the Protocol Buffers transport", what);
}

/* An HTTP request whose response body is collected; the status is checked
 * by the caller. */
static riak_status http_call(riak_client *c, const char *method, const char *url, const char *ctype,
                             const void *body, size_t len, riak_response *resp) {
    struct curl_slist *headers = NULL;
    riak_status st = RIAK_OK;
    if (ctype) {
        st = riak_add_header(c, &headers, "Content-Type", NULL, ctype);
    }
    if (st == RIAK_OK) {
        riak_request req = {
            .method = method, .url = url, .headers = headers, .body = body, .body_len = len};
        st = riak_perform(c, &req, resp);
    }
    curl_slist_free_all(headers);
    return st;
}

/* ---- bucket-type properties, client id -------------------------------- */

riak_status riak_set_type_props(riak_client *client, const char *type, const char *json) {
    if (client == NULL || json == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    riak_status st = riak_check_name(client, type, "bucket type");
    if (st != RIAK_OK) {
        return st;
    }
    if (riak_is_pb(client)) {
        return riak_pb_set_type_props(client, type, json);
    }
    riak_buf url = {0};
    if (riak_buf_append_str(&url, client->root) || riak_buf_append_str(&url, "types/") ||
        riak_url_append_escaped(&url, type) || riak_buf_append_str(&url, "/props")) {
        riak_buf_free(&url);
        return riak_set_error(client, RIAK_E_NOMEM, "out of memory");
    }
    riak_response resp = {0};
    st = http_call(client, "PUT", url.data, "application/json", json, strlen(json), &resp);
    if (st == RIAK_OK && resp.status != 204 && resp.status != 200) {
        st = riak_http_error(client, &resp);
    }
    riak_response_free(&resp);
    riak_buf_free(&url);
    return st;
}

riak_status riak_get_client_id(riak_client *client, char **id) {
    if (client == NULL || id == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    *id = NULL;
    return riak_is_pb(client) ? riak_pb_client_id(client, id) : needs_pb(client, "client ids");
}

/* ---- Riak 1.4 counters ------------------------------------------------ */

riak_status riak_counter_http_prepare(riak_client *c, const char *bucket, const char *key,
                                      bool update, int64_t amount, const riak_opts *o,
                                      bool want_value, riak_prepared *p) {
    memset(p, 0, sizeof *p);
    int rc = riak_url_bucket(&p->url, c, NULL, bucket, "counters", key);
    if (update) {
        rc = rc || riak_url_append_quorum(&p->url, "w", o->w) ||
             riak_url_append_quorum(&p->url, "dw", o->dw) ||
             riak_url_append_quorum(&p->url, "pw", o->pw) ||
             (want_value && riak_url_append_query(&p->url, "returnvalue", "true"));
    } else {
        rc = rc || riak_url_append_quorum(&p->url, "r", o->r) ||
             riak_url_append_quorum(&p->url, "pr", o->pr) ||
             (o->basic_quorum && riak_url_append_query(&p->url, "basic_quorum", "true")) ||
             (o->notfound_ok && riak_url_append_query(&p->url, "notfound_ok", "true"));
    }
    rc = rc || riak_url_append_quorum(&p->url, "node_confirms", o->node_confirms);
    if (rc) {
        riak_prepared_free(p);
        return riak_set_error(c, RIAK_E_INVALID_ARG, "invalid quorum or out of memory");
    }
    if (!update) {
        p->req = (riak_request){.method = "GET", .url = p->url.data};
        return RIAK_OK;
    }
    char num[32];
    snprintf(num, sizeof num, "%" PRId64, amount);
    riak_status st = riak_add_header(c, &p->headers, "Content-Type", NULL, "text/plain");
    if (st == RIAK_OK && (p->body = riak_strdup(num)) == NULL) {
        st = riak_set_error(c, RIAK_E_NOMEM, "out of memory");
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

riak_status riak_counter_http_finish(riak_client *c, riak_response *resp, int64_t *value) {
    if (resp->status != 200 && resp->status != 204) {
        return riak_http_error(c, resp);
    }
    if (value) {
        char *end = NULL;
        const char *body = resp->body.data ? resp->body.data : "";
        errno = 0;
        long long v = strtoll(body, &end, 10);
        if (end == body || errno == ERANGE) {
            return riak_set_error(c, RIAK_E_PARSE, "malformed counter value");
        }
        *value = v;
    }
    return RIAK_OK;
}

static riak_status counter_http(riak_client *c, const char *bucket, const char *key, bool update,
                                int64_t amount, const riak_opts *o, int64_t *value) {
    riak_prepared p;
    riak_response resp = {0};
    riak_status st =
        riak_counter_http_prepare(c, bucket, key, update, amount, o, value != NULL, &p);
    if (st == RIAK_OK) {
        st = riak_perform(c, &p.req, &resp);
        riak_prepared_free(&p);
    }
    if (st == RIAK_OK) {
        st = riak_counter_http_finish(c, &resp, value);
    }
    riak_response_free(&resp);
    return st;
}

static riak_status counter(riak_client *c, const char *bucket, const char *key, bool update,
                           int64_t amount, const riak_opts *opts, int64_t *value) {
    if (c == NULL || (!update && value == NULL)) {
        return RIAK_E_INVALID_ARG;
    }
    riak_status st;
    const riak_opts *o = opts ? opts : &no_opts;
    if ((st = riak_check_name(c, bucket, "bucket")) != RIAK_OK ||
        (st = riak_check_name(c, key, "key")) != RIAK_OK) {
        return st;
    }
    if (o->bucket_type) {
        return riak_set_error(c, RIAK_E_UNSUPPORTED,
                              "1.4 counters live in the default bucket type");
    }
    return riak_is_pb(c) ? riak_pb_counter(c, bucket, key, update, amount, o, value)
                         : counter_http(c, bucket, key, update, amount, o, value);
}

riak_status riak_counter_update(riak_client *client, const char *bucket, const char *key,
                                int64_t amount, const riak_opts *opts, int64_t *value) {
    return counter(client, bucket, key, true, amount, opts, value);
}

riak_status riak_counter_get(riak_client *client, const char *bucket, const char *key,
                             const riak_opts *opts, int64_t *value) {
    return counter(client, bucket, key, false, 0, opts, value);
}

/* ---- object folds ----------------------------------------------------- */

riak_status riak_fold_objects(riak_client *client, const char *bucket, const riak_fold_query *q,
                              riak_object_cb cb, void *ctx, char **continuation) {
    if (continuation) {
        *continuation = NULL;
    }
    if (client == NULL || q == NULL || cb == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    riak_status st;
    if ((st = riak_check_name(client, bucket, "bucket")) != RIAK_OK ||
        (st = riak_check_name(client, q->start_key, "start_key")) != RIAK_OK ||
        (q->bucket_type &&
         (st = riak_check_name(client, q->bucket_type, "bucket type")) != RIAK_OK)) {
        return st;
    }
    if (!riak_is_pb(client)) {
        return needs_pb(client, "object folds");
    }
    st = riak_pb_fold_objects(client, bucket, q, cb, ctx, continuation);
    if (st != RIAK_OK && continuation) {
        free(*continuation);
        *continuation = NULL;
    }
    return st;
}

/* ---- riak_repl -------------------------------------------------------- */

riak_status riak_get_cluster_id(riak_client *client, char **id) {
    if (client == NULL || id == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    *id = NULL;
    return riak_is_pb(client) ? riak_pb_cluster_id(client, id) : needs_pb(client, "cluster ids");
}

riak_status riak_repl_get(riak_client *client, const char *cluster_id, const char *bucket,
                          const char *key, const riak_opts *opts, riak_object *out) {
    if (client == NULL || out == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    riak_status st;
    if ((st = riak_check_name(client, cluster_id, "cluster id")) != RIAK_OK ||
        (st = riak_check_name(client, bucket, "bucket")) != RIAK_OK ||
        (st = riak_check_name(client, key, "key")) != RIAK_OK) {
        return st;
    }
    if (!riak_is_pb(client)) {
        return needs_pb(client, "riak_repl gets");
    }
    return riak_pb_repl_get(client, cluster_id, bucket, key, opts ? opts : &no_opts, out);
}

/* ---- nextgenrepl queues ---------------------------------------------- */

void riak_repl_fetch_result_free(riak_repl_fetch_result *r) {
    if (r == NULL) {
        return;
    }
    free(r->object);
    free(r->deleted_vclock);
    memset(r, 0, sizeof *r);
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24U) | ((uint32_t)p[1] << 16U) | ((uint32_t)p[2] << 8U) | p[3];
}

/* The tombstone clock arrives over HTTP as term_to_binary(Clock); Riak's own
 * encoding (as in X-Riak-Vclock and over Protocol Buffers) is
 * term_to_binary({encode_raw, Clock}), which is built here so both
 * transports return a clock usable in riak_opts::vclock. */
static char *http_tomb_clock(const uint8_t *term, size_t len) {
    static const uint8_t prefix[] = {131, 104, 2,   119, 10,  'e', 'n', 'c',
                                     'o', 'd', 'e', '_', 'r', 'a', 'w'};
    if (len < 2 || term[0] != 131) {
        return NULL;
    }
    size_t n = sizeof prefix + len - 1;
    uint8_t *buf = malloc(n);
    if (buf == NULL) {
        return NULL;
    }
    memcpy(buf, prefix, sizeof prefix);
    memcpy(buf + sizeof prefix, term + 1, len - 1);
    char *b64 = riak_pb_b64_encode(riak_pb_bytes(buf, n));
    free(buf);
    return b64;
}

/* Body of GET /queuename/<q>?object_format=internal_aaehash: a success
 * mark, a tombstone flag, segment id and hash, the tombstone clock when
 * deleted, then the CRC32 and the encoded object. */
static riak_status parse_http_fetch(riak_client *c, const uint8_t *p, size_t n,
                                    riak_repl_fetch_result *out) {
    if (n >= 1 && p[0] == 0) {
        out->queue_empty = true;
        return RIAK_OK;
    }
    if (n < 10 || p[0] != 1) {
        return riak_set_error(c, RIAK_E_PARSE, "malformed queue fetch response");
    }
    out->deleted = p[1] == 1;
    out->segment_id = be32(p + 2);
    out->segment_hash = be32(p + 6);
    size_t off = 10;
    if (out->deleted) {
        if (n - off < 4 || n - off - 4 < be32(p + off)) {
            return riak_set_error(c, RIAK_E_PARSE, "malformed queue fetch response");
        }
        size_t tcl = be32(p + off);
        if ((out->deleted_vclock = http_tomb_clock(p + off + 4, tcl)) == NULL) {
            return riak_set_error(c, RIAK_E_PARSE, "malformed tombstone clock");
        }
        off += 4 + tcl;
    }
    if (n - off < 4) {
        return riak_set_error(c, RIAK_E_PARSE, "malformed queue fetch response");
    }
    out->has_crc = true;
    out->crc = be32(p + off);
    off += 4;
    out->object_len = n - off;
    if ((out->object = malloc(out->object_len ? out->object_len : 1)) == NULL) {
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    memcpy(out->object, p + off, out->object_len);
    return RIAK_OK;
}

riak_status riak_repl_fetch(riak_client *client, const char *queue, riak_repl_fetch_result *out) {
    if (client == NULL || out == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    riak_status st = riak_check_name(client, queue, "queue");
    if (st != RIAK_OK) {
        return st;
    }
    if (riak_is_pb(client)) {
        st = riak_pb_repl_fetch(client, queue, out);
    } else {
        riak_buf url = {0};
        riak_response resp = {0};
        if (riak_buf_append_str(&url, client->root) || riak_buf_append_str(&url, "queuename/") ||
            riak_url_append_escaped(&url, queue) ||
            riak_url_append_query(&url, "object_format", "internal_aaehash")) {
            st = riak_set_error(client, RIAK_E_NOMEM, "out of memory");
        } else {
            /* each fetch takes an entry off the queue: never repeated */
            riak_request req = {.method = "GET", .url = url.data, .no_retry = true};
            st = riak_perform(client, &req, &resp);
        }
        if (st == RIAK_OK && resp.status == 500 && resp.body.data &&
            strstr(resp.body.data, "{error,notfound}")) {
            /* the queued key no longer exists; Riak has taken the entry off */
            st = riak_set_error(client, RIAK_E_NOT_FOUND, "queued key no longer exists");
        } else if (st == RIAK_OK) {
            st = resp.status == 200
                     ? parse_http_fetch(client, (const uint8_t *)resp.body.data, resp.body.len, out)
                     : riak_http_error_or_unsupported(client, &resp,
                                                      "nextgenrepl queues (Riak 2.9+)");
        }
        riak_response_free(&resp);
        riak_buf_free(&url);
    }
    if (st != RIAK_OK) {
        riak_repl_fetch_result_free(out);
    }
    return st;
}

riak_status riak_repl_push(riak_client *client, const char *queue, const riak_repl_key *keys,
                           size_t n_keys, riak_repl_push_result *out) {
    if (out) {
        memset(out, 0, sizeof *out);
    }
    if (client == NULL || (keys == NULL && n_keys > 0)) {
        return RIAK_E_INVALID_ARG;
    }
    riak_status st = riak_check_name(client, queue, "queue");
    for (size_t i = 0; st == RIAK_OK && i < n_keys; i++) {
        if ((st = riak_check_name(client, keys[i].bucket, "bucket")) == RIAK_OK &&
            (st = riak_check_name(client, keys[i].key, "key")) == RIAK_OK) {
            st = riak_check_name(client, keys[i].vclock, "vclock");
        }
    }
    if (st != RIAK_OK) {
        return st;
    }
    if (!riak_is_pb(client)) {
        /* POST /queuename/<q> exists, but answers without the queue lengths;
         * one transport keeps the result well defined */
        return needs_pb(client, "queue pushes");
    }
    return riak_pb_repl_push(client, queue, keys, n_keys, out);
}

void riak_members_free(riak_members *m) {
    if (m == NULL) {
        return;
    }
    for (size_t i = 0; i < m->count; i++) {
        free(m->items[i].ip);
    }
    free(m->items);
    memset(m, 0, sizeof *m);
}

/* {"up_nodes":[{"ip":"10.0.0.1","port":"8087"},...]} */
static riak_status parse_members(riak_client *c, const char *json, riak_members *out) {
    json_object *root = json_tokener_parse(json ? json : "");
    json_object *arr;
    riak_status st = RIAK_E_PARSE;
    if (root && json_object_object_get_ex(root, "up_nodes", &arr) &&
        json_object_is_type(arr, json_type_array)) {
        size_t n = json_object_array_length(arr);
        out->items = calloc(n ? n : 1, sizeof *out->items);
        st = out->items ? RIAK_OK : RIAK_E_NOMEM;
        for (size_t i = 0; st == RIAK_OK && i < n; i++) {
            json_object *e = json_object_array_get_idx(arr, i), *ip, *port;
            if (!json_object_object_get_ex(e, "ip", &ip) ||
                !json_object_object_get_ex(e, "port", &port)) {
                st = RIAK_E_PARSE;
                break;
            }
            int p = json_object_get_int(port); /* the port is sent as a string */
            if (p <= 0 || p > 65535) {
                st = RIAK_E_PARSE;
                break;
            }
            riak_member *m = &out->items[out->count++];
            m->port = (unsigned)p;
            if ((m->ip = riak_strdup(json_object_get_string(ip))) == NULL) {
                st = RIAK_E_NOMEM;
            }
        }
    }
    json_object_put(root);
    return st == RIAK_OK ? st : riak_set_error(c, st, "cannot parse membership response");
}

riak_status riak_cluster_membership(riak_client *client, riak_members *out) {
    if (client == NULL || out == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    riak_status st;
    if (riak_is_pb(client)) {
        st = riak_pb_membership(client, out);
    } else {
        riak_request req = {.method = "GET", .url = "/membership_request"};
        riak_response resp = {0};
        st = riak_perform(client, &req, &resp);
        if (st == RIAK_OK) {
            st = resp.status == 200 ? parse_members(client, resp.body.data, out)
                                    : riak_http_error_or_unsupported(
                                          client, &resp, "cluster membership (Riak 3.0+)");
        }
        riak_response_free(&resp);
    }
    if (st != RIAK_OK) {
        riak_members_free(out);
    }
    return st;
}

/* ---- link walking ----------------------------------------------------- */

void riak_link_result_free(riak_link_result *res) {
    if (res == NULL) {
        return;
    }
    for (size_t i = 0; i < res->n_phases; i++) {
        riak_link_phase *ph = &res->phases[i];
        for (size_t j = 0; j < ph->count; j++) {
            free(ph->items[j].bucket);
            free(ph->items[j].key);
            riak_object_free(&ph->items[j].object);
        }
        free(ph->items);
    }
    free(res->phases);
    memset(res, 0, sizeof *res);
}

/* mochiweb_util:quote_plus encodes spaces as '+'. */
static char *unquote_plus(const char *s, size_t len) {
    char *copy = riak_strndup(s, len);
    if (copy == NULL) {
        return NULL;
    }
    for (char *p = copy; *p; p++) {
        if (*p == '+') {
            *p = ' ';
        }
    }
    char *out = riak_url_unescape(copy, strlen(copy));
    free(copy);
    return out;
}

/* Location: /buckets/<b>/keys/<k>[?vtag=...] or /riak/<b>/<k> */
static bool location_bucket_key(const char *loc, char **bucket, char **key) {
    const char *b, *sep;
    size_t skip; /* length of the separator before the key */
    if (strncmp(loc, "/buckets/", 9) == 0) {
        b = loc + 9;
        sep = strstr(b, "/keys/");
        skip = 6;
    } else if (strncmp(loc, "/riak/", 6) == 0) {
        b = loc + 6;
        sep = strchr(b, '/');
        skip = 1;
    } else {
        return false;
    }
    if (sep == NULL) {
        return false;
    }
    const char *k = sep + skip;
    *bucket = unquote_plus(b, (size_t)(sep - b));
    *key = unquote_plus(k, strcspn(k, "?"));
    return *bucket && *key;
}

/* One step's part: a multipart/mixed list of objects. */
static riak_status http_link_phase(riak_client *c, const riak_part *part, riak_link_phase *ph) {
    char *boundary = riak_multipart_boundary(riak_headerlist_get(&part->headers, "Content-Type"));
    if (boundary == NULL) {
        return RIAK_E_PARSE;
    }
    riak_part *objs = NULL;
    size_t n = 0;
    riak_status st = riak_multipart_parse(boundary, part->body, part->len, &objs, &n);
    free(boundary);
    if (st != RIAK_OK) {
        return st;
    }
    if ((ph->items = calloc(n ? n : 1, sizeof *ph->items)) == NULL) {
        riak_parts_free(objs, n);
        return RIAK_E_NOMEM;
    }
    for (size_t i = 0; st == RIAK_OK && i < n; i++) {
        riak_linked_object *lo = &ph->items[ph->count++];
        const char *loc = riak_headerlist_get(&objs[i].headers, "Location");
        if (loc == NULL || !location_bucket_key(loc, &lo->bucket, &lo->key)) {
            st = RIAK_E_PARSE;
            break;
        }
        lo->object.data = objs[i].body;
        lo->object.len = objs[i].len;
        objs[i].body = NULL;
        if (riak_object_from_headers(&objs[i].headers, NULL, &lo->object) != 0) {
            st = RIAK_E_NOMEM;
        }
    }
    riak_parts_free(objs, n);
    (void)c;
    return st;
}

static riak_status link_walk_http(riak_client *c, const char *bucket, const char *key,
                                  const riak_link_step *steps, size_t n, riak_link_result *out) {
    riak_buf url = {0};
    int rc = riak_url_bucket(&url, c, NULL, bucket, "keys", key);
    for (size_t i = 0; !rc && i < n; i++) {
        bool keep = steps[i].keep || i + 1 == n;
        rc = riak_buf_append_str(&url, "/") ||
             (steps[i].bucket ? riak_url_append_escaped(&url, steps[i].bucket)
                              : riak_buf_append_str(&url, "_")) ||
             riak_buf_append_str(&url, ",") ||
             (steps[i].tag ? riak_url_append_escaped(&url, steps[i].tag)
                           : riak_buf_append_str(&url, "_")) ||
             riak_buf_append_str(&url, keep ? ",1" : ",0");
    }
    if (rc) {
        riak_buf_free(&url);
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    riak_response resp = {0};
    riak_status st = http_call(c, "GET", url.data, NULL, NULL, 0, &resp);
    riak_buf_free(&url);
    if (st == RIAK_OK && resp.status != 200) {
        st = riak_http_error(c, &resp);
    }
    riak_part *parts = NULL;
    size_t n_parts = 0;
    if (st == RIAK_OK) {
        char *boundary =
            riak_multipart_boundary(riak_headerlist_get(&resp.headers, "Content-Type"));
        st = boundary ? riak_multipart_parse(boundary, resp.body.data ? resp.body.data : "",
                                             resp.body.len, &parts, &n_parts)
                      : RIAK_E_PARSE;
        free(boundary);
        if (st == RIAK_OK &&
            (out->phases = calloc(n_parts ? n_parts : 1, sizeof *out->phases)) == NULL) {
            st = RIAK_E_NOMEM;
        }
        for (size_t i = 0; st == RIAK_OK && i < n_parts; i++) {
            st = http_link_phase(c, &parts[i], &out->phases[out->n_phases++]);
        }
        if (st != RIAK_OK) {
            riak_set_error(c, st, "cannot parse link walk response");
        }
    }
    riak_parts_free(parts, n_parts);
    riak_response_free(&resp);
    return st;
}

/* Protocol Buffers: the walk runs as MapReduce link phases; kept phases
 * return [bucket, key, tag] lists whose objects are then fetched. */
typedef struct {
    riak_client *c;
    const riak_link_step *steps;
    size_t n;
    json_object **found; /* per step: array of [bucket, key, tag] */
    riak_status err;
} walk_ctx;

static bool walk_phase(unsigned phase, const char *data, void *ctx) {
    walk_ctx *w = ctx;
    json_object *arr = json_tokener_parse(data);
    if (phase >= w->n || arr == NULL || !json_object_is_type(arr, json_type_array)) {
        json_object_put(arr);
        w->err = RIAK_E_PARSE;
        return false;
    }
    if (w->found[phase] == NULL) {
        w->found[phase] = arr;
        return true;
    }
    for (size_t i = 0; i < json_object_array_length(arr); i++) {
        json_object_array_add(w->found[phase], json_object_get(json_object_array_get_idx(arr, i)));
    }
    json_object_put(arr);
    return true;
}

static riak_status link_walk_pb(riak_client *c, const char *bucket, const char *key,
                                const riak_link_step *steps, size_t n, riak_link_result *out) {
    json_object *job = json_object_new_object();
    json_object *inputs = json_object_new_array();
    json_object *start = json_object_new_array();
    json_object *query = json_object_new_array();
    json_object_array_add(start, json_object_new_string(bucket));
    json_object_array_add(start, json_object_new_string(key));
    json_object_array_add(inputs, start);
    json_object_object_add(job, "inputs", inputs);
    for (size_t i = 0; i < n; i++) {
        json_object *spec = json_object_new_object();
        json_object *phase = json_object_new_object();
        json_object_object_add(spec, "bucket",
                               json_object_new_string(steps[i].bucket ? steps[i].bucket : "_"));
        json_object_object_add(spec, "tag",
                               json_object_new_string(steps[i].tag ? steps[i].tag : "_"));
        json_object_object_add(spec, "keep", json_object_new_boolean(steps[i].keep || i + 1 == n));
        json_object_object_add(phase, "link", spec);
        json_object_array_add(query, phase);
    }
    json_object_object_add(job, "query", query);
    char *text = riak_strdup(json_object_to_json_string_ext(job, JSON_C_TO_STRING_PLAIN));
    json_object_put(job);
    walk_ctx w = {.c = c, .steps = steps, .n = n};
    w.found = (json_object **)calloc(n, sizeof *w.found);
    riak_status st = text && w.found ? riak_pb_mapred(c, text, walk_phase, &w, NULL)
                                     : riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    free(text);
    if (st == RIAK_OK && w.err != RIAK_OK) {
        st = riak_set_error(c, w.err, "malformed link phase result");
    }
    size_t kept = 0;
    for (size_t i = 0; i < n; i++) {
        kept += steps[i].keep || i + 1 == n;
    }
    if (st == RIAK_OK && (out->phases = calloc(kept, sizeof *out->phases)) == NULL) {
        st = riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    for (size_t i = 0; st == RIAK_OK && i < n; i++) {
        if (!(steps[i].keep || i + 1 == n)) {
            continue;
        }
        riak_link_phase *ph = &out->phases[out->n_phases++];
        size_t m = w.found[i] ? json_object_array_length(w.found[i]) : 0;
        if ((ph->items = calloc(m ? m : 1, sizeof *ph->items)) == NULL) {
            st = riak_set_error(c, RIAK_E_NOMEM, "out of memory");
            break;
        }
        for (size_t j = 0; st == RIAK_OK && j < m; j++) {
            json_object *bkt = json_object_array_get_idx(w.found[i], j);
            /* [bucket, key, tag] with string bucket and key */
            json_object *jb = NULL, *jk = NULL;
            if (json_object_is_type(bkt, json_type_array) && json_object_array_length(bkt) >= 2) {
                jb = json_object_array_get_idx(bkt, 0);
                jk = json_object_array_get_idx(bkt, 1);
            }
            const char *b =
                json_object_is_type(jb, json_type_string) ? json_object_get_string(jb) : NULL;
            const char *k =
                json_object_is_type(jk, json_type_string) ? json_object_get_string(jk) : NULL;
            if (b == NULL || k == NULL) {
                st = riak_set_error(c, RIAK_E_PARSE, "malformed link phase result");
                break;
            }
            /* the link walker drops duplicates and objects that are gone */
            bool dup = false;
            for (size_t d = 0; d < ph->count; d++) {
                const riak_linked_object *seen = &ph->items[d];
                dup = dup || (seen->bucket && seen->key && strcmp(seen->bucket, b) == 0 &&
                              strcmp(seen->key, k) == 0);
            }
            if (dup) {
                continue;
            }
            riak_linked_object *lo = &ph->items[ph->count];
            riak_status gs = riak_pb_get(c, b, k, &no_opts, &lo->object);
            if (gs == RIAK_E_NOT_FOUND) {
                riak_clear_error(c);
                continue;
            }
            if (gs != RIAK_OK) {
                st = gs;
                break;
            }
            lo->bucket = riak_strdup(b);
            lo->key = riak_strdup(k);
            if (lo->bucket == NULL || lo->key == NULL) {
                free(lo->bucket);
                free(lo->key);
                riak_object_free(&lo->object);
                memset(lo, 0, sizeof *lo);
                st = riak_set_error(c, RIAK_E_NOMEM, "out of memory");
                break;
            }
            ph->count++;
        }
    }
    for (size_t i = 0; w.found && i < n; i++) {
        json_object_put(w.found[i]);
    }
    free((void *)w.found);
    return st;
}

riak_status riak_link_walk(riak_client *client, const char *bucket, const char *key,
                           const riak_link_step *steps, size_t n_steps, riak_link_result *out) {
    if (client == NULL || out == NULL || steps == NULL || n_steps == 0) {
        return RIAK_E_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    riak_status st;
    if ((st = riak_check_name(client, bucket, "bucket")) != RIAK_OK ||
        (st = riak_check_name(client, key, "key")) != RIAK_OK) {
        return st;
    }
    st = riak_is_pb(client) ? link_walk_pb(client, bucket, key, steps, n_steps, out)
                            : link_walk_http(client, bucket, key, steps, n_steps, out);
    if (st != RIAK_OK && strstr(riak_client_errmsg(client), "not compatible with") &&
        strstr(riak_client_errmsg(client), "security")) {
        /* a node with Riak security enabled refuses link walking (HTTP 403,
         * or an error from the MapReduce link phase) */
        st = riak_set_error(client, RIAK_E_UNSUPPORTED,
                            "link walking is not available with Riak security enabled");
    }
    if (st != RIAK_OK) {
        riak_link_result_free(out);
    }
    return st;
}
