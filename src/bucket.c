/*
 File:   bucket.c
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later

 Bucket listing, bucket and bucket-type properties, node statistics.
 */

#include "internal.h"

#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "parse.h"
#include "pb.h"

static riak_status check_type(riak_client *c, const char *type) {
    return type ? riak_check_name(c, type, "bucket type") : RIAK_OK;
}

riak_status riak_get_body(riak_client *c, const char *url, int kind, char **body) {
    riak_request req = {.method = "GET", .url = url};
    riak_response resp = {0};
    riak_status st = riak_perform(c, &req, &resp);
    if (st != RIAK_OK) {
        return st;
    }
    if (resp.status != 200) {
        st = riak_http_error(c, &resp);
    } else {
        json_object *obj = NULL;
        if (kind != RIAK_BODY_RAW) {
            obj = json_tokener_parse(resp.body.data ? resp.body.data : "");
            enum json_type want = kind == RIAK_BODY_JSON_ARRAY ? json_type_array : json_type_object;
            if (obj == NULL || !json_object_is_type(obj, want)) {
                st = riak_set_error(c, RIAK_E_PARSE, "response is not a JSON %s",
                                    kind == RIAK_BODY_JSON_ARRAY ? "array" : "object");
            }
        }
        if (st == RIAK_OK && (*body = riak_buf_steal(&resp.body, NULL)) == NULL) {
            st = riak_set_error(c, RIAK_E_NOMEM, "out of memory");
        }
        json_object_put(obj);
    }
    riak_response_free(&resp);
    return st;
}

typedef struct {
    riak_strlist list;
    size_t cap;
    bool nomem;
} name_acc;

static bool collect_name(const char *name, size_t len, void *ctx) {
    name_acc *a = ctx;
    if (riak_strlist_push(&a->list, &a->cap, name, len) != 0) {
        a->nomem = true;
        return false;
    }
    return true;
}

/* Protocol Buffers: a listing is collected into a NULL-terminated list. */
static riak_status collect_names(riak_client *c, const char *type, const char *bucket,
                                 riak_strlist *out) {
    name_acc a = {0};
    riak_status st = bucket ? riak_pb_list_keys(c, type, bucket, collect_name, &a)
                            : riak_pb_list_buckets(c, type, collect_name, &a);
    if (st == RIAK_OK && a.nomem) {
        st = riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    if (st == RIAK_OK && a.list.items == NULL &&
        (a.list.items = (char **)calloc(1, sizeof(char *))) == NULL) {
        st = riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    if (st != RIAK_OK) {
        riak_strlist_free(&a.list);
        return st;
    }
    *out = a.list;
    return RIAK_OK;
}

riak_status riak_type_list_buckets(riak_client *client, const char *type, riak_strlist *out) {
    if (client == NULL || out == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    riak_status st = check_type(client, type);
    if (st != RIAK_OK) {
        return st;
    }
    if (riak_is_pb(client)) {
        return collect_names(client, type, NULL, out);
    }
    riak_buf url = {0};
    if (riak_buf_append_str(&url, client->root) != 0 ||
        (type &&
         (riak_buf_append_str(&url, "types/") != 0 || riak_url_append_escaped(&url, type) != 0 ||
          riak_buf_append_str(&url, "/") != 0)) ||
        riak_buf_append_str(&url, "buckets?buckets=true") != 0) {
        riak_buf_free(&url);
        return riak_set_error(client, RIAK_E_NOMEM, "out of memory");
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
        st = riak_parse_string_array(resp.body.data ? resp.body.data : "", resp.body.len, "buckets",
                                     out);
        if (st != RIAK_OK) {
            riak_set_error(client, st, "cannot parse bucket list");
        }
    }
    riak_response_free(&resp);
    return st;
}

riak_status riak_list_buckets(riak_client *client, riak_strlist *out) {
    return riak_type_list_buckets(client, NULL, out);
}

static bool keystream_sink(void *ctx, const char *data, size_t len, riak_status *abort_status) {
    riak_status st = riak_keystream_feed(ctx, data, len);
    if (st != RIAK_OK) {
        *abort_status = st;
        return false;
    }
    return true;
}

riak_status riak_type_list_keys(riak_client *client, const char *type, const char *bucket,
                                riak_strlist *out) {
    if (client == NULL || out == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    riak_status st;
    if ((st = check_type(client, type)) != RIAK_OK ||
        (st = riak_check_name(client, bucket, "bucket")) != RIAK_OK) {
        return st;
    }
    if (riak_is_pb(client)) {
        return collect_names(client, type, bucket, out);
    }
    riak_keystream *ks = riak_keystream_new();
    riak_buf url = {0};
    if (ks == NULL || riak_url_bucket(&url, client, type, bucket, "keys", NULL) != 0 ||
        riak_url_append_query(&url, "keys", "stream") != 0) {
        riak_keystream_free(ks);
        riak_buf_free(&url);
        return riak_set_error(client, RIAK_E_NOMEM, "out of memory");
    }
    riak_request req = {.method = "GET", .url = url.data, .sink = keystream_sink, .sink_ctx = ks};
    riak_response resp = {0};
    st = riak_perform(client, &req, &resp);
    if (st == RIAK_OK) {
        if (resp.status != 200) {
            st = riak_http_error(client, &resp);
        } else if ((st = riak_keystream_finish(ks, out)) != RIAK_OK) {
            riak_set_error(client, st, "truncated key stream");
        }
        riak_response_free(&resp);
    }
    riak_keystream_free(ks);
    riak_buf_free(&url);
    return st;
}

riak_status riak_list_keys(riak_client *client, const char *bucket, riak_strlist *out) {
    return riak_type_list_keys(client, NULL, bucket, out);
}

static riak_status props_request(riak_client *client, const char *type, const char *bucket,
                                 const char *method, const char *json, riak_response *resp) {
    riak_status st;
    if ((st = check_type(client, type)) != RIAK_OK ||
        (st = riak_check_name(client, bucket, "bucket")) != RIAK_OK) {
        return st;
    }
    riak_buf url = {0};
    struct curl_slist *headers = NULL;
    if (riak_url_bucket(&url, client, type, bucket, "props", NULL) != 0) {
        riak_buf_free(&url);
        return riak_set_error(client, RIAK_E_NOMEM, "out of memory");
    }
    if (json != NULL) {
        st = riak_add_header(client, &headers, "Content-Type", NULL, "application/json");
    }
    if (st == RIAK_OK) {
        riak_request req = {.method = method,
                            .url = url.data,
                            .headers = headers,
                            .body = json,
                            .body_len = json ? strlen(json) : 0};
        st = riak_perform(client, &req, resp);
    }
    curl_slist_free_all(headers);
    riak_buf_free(&url);
    return st;
}

riak_status riak_type_get_bucket_props(riak_client *client, const char *type, const char *bucket,
                                       char **json) {
    if (client == NULL || json == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    *json = NULL;
    riak_status st;
    if ((st = check_type(client, type)) != RIAK_OK ||
        (st = riak_check_name(client, bucket, "bucket")) != RIAK_OK) {
        return st;
    }
    if (riak_is_pb(client)) {
        return riak_pb_get_bucket_props(client, type, bucket, json);
    }
    riak_buf url = {0};
    if (riak_url_bucket(&url, client, type, bucket, "props", NULL) != 0) {
        riak_buf_free(&url);
        return riak_set_error(client, RIAK_E_NOMEM, "out of memory");
    }
    st = riak_get_body(client, url.data, RIAK_BODY_JSON_OBJECT, json);
    riak_buf_free(&url);
    return st;
}

riak_status riak_get_bucket_props(riak_client *client, const char *bucket, char **json) {
    return riak_type_get_bucket_props(client, NULL, bucket, json);
}

riak_status riak_type_set_bucket_props(riak_client *client, const char *type, const char *bucket,
                                       const char *json) {
    if (client == NULL || json == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    if (riak_is_pb(client)) {
        riak_status st;
        if ((st = check_type(client, type)) != RIAK_OK ||
            (st = riak_check_name(client, bucket, "bucket")) != RIAK_OK) {
            return st;
        }
        return riak_pb_set_bucket_props(client, type, bucket, json);
    }
    riak_response resp = {0};
    riak_status st = props_request(client, type, bucket, "PUT", json, &resp);
    if (st != RIAK_OK) {
        return st;
    }
    if (resp.status != 204 && resp.status != 200) {
        st = riak_http_error(client, &resp);
    }
    riak_response_free(&resp);
    return st;
}

riak_status riak_set_bucket_props(riak_client *client, const char *bucket, const char *json) {
    return riak_type_set_bucket_props(client, NULL, bucket, json);
}

riak_status riak_type_reset_bucket_props(riak_client *client, const char *type,
                                         const char *bucket) {
    if (client == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    if (riak_is_pb(client)) {
        riak_status st;
        if ((st = check_type(client, type)) != RIAK_OK ||
            (st = riak_check_name(client, bucket, "bucket")) != RIAK_OK) {
            return st;
        }
        return riak_pb_reset_bucket_props(client, type, bucket);
    }
    riak_response resp = {0};
    riak_status st = props_request(client, type, bucket, "DELETE", NULL, &resp);
    if (st != RIAK_OK) {
        return st;
    }
    if (resp.status != 204) {
        st = riak_http_error(client, &resp);
    }
    riak_response_free(&resp);
    return st;
}

riak_status riak_reset_bucket_props(riak_client *client, const char *bucket) {
    return riak_type_reset_bucket_props(client, NULL, bucket);
}

riak_status riak_get_type_props(riak_client *client, const char *type, char **json) {
    if (client == NULL || json == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    *json = NULL;
    riak_status st = riak_check_name(client, type, "bucket type");
    if (st != RIAK_OK) {
        return st;
    }
    if (riak_is_pb(client)) {
        return riak_pb_get_type_props(client, type, json);
    }
    riak_buf url = {0};
    if (riak_buf_append_str(&url, client->root) != 0 || riak_buf_append_str(&url, "types/") != 0 ||
        riak_url_append_escaped(&url, type) != 0 || riak_buf_append_str(&url, "/props") != 0) {
        riak_buf_free(&url);
        return riak_set_error(client, RIAK_E_NOMEM, "out of memory");
    }
    st = riak_get_body(client, url.data, RIAK_BODY_JSON_OBJECT, json);
    riak_buf_free(&url);
    return st;
}

riak_status riak_http_target(riak_client *c, const char *what, size_t *pin) {
    *pin = 0;
    if (!riak_is_pb(c)) {
        return RIAK_OK;
    }
    /* Protocol Buffers lacks the request: the node's HTTP port serves it */
    for (size_t i = 0; i < c->n_nodes; i++) {
        if (c->nodes[i].base != NULL) {
            *pin = i + 1;
            return RIAK_OK;
        }
    }
    return riak_set_error(c, RIAK_E_UNSUPPORTED, "%s need an HTTP port (riak_config.http_port)",
                          what);
}

riak_status riak_stats(riak_client *client, char **json) {
    if (client == NULL || json == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    *json = NULL;
    size_t pin;
    riak_status target = riak_http_target(client, "statistics", &pin);
    if (target != RIAK_OK) {
        return target;
    }
    riak_request req = {.method = "GET", .url = "/stats", .pin = pin};
    riak_response resp = {0};
    riak_status st = riak_perform(client, &req, &resp);
    if (st == RIAK_OK) {
        json_object *obj = NULL;
        if (resp.status != 200) {
            st = riak_http_error(client, &resp);
        } else if ((obj = json_tokener_parse(resp.body.data ? resp.body.data : "")) == NULL ||
                   !json_object_is_type(obj, json_type_object)) {
            st = riak_set_error(client, RIAK_E_PARSE, "response is not a JSON object");
        } else if ((*json = riak_buf_steal(&resp.body, NULL)) == NULL) {
            st = riak_set_error(client, RIAK_E_NOMEM, "out of memory");
        }
        json_object_put(obj);
    }
    riak_response_free(&resp);
    return st;
}

/* ---- streamed listings ------------------------------------------------ */

typedef struct {
    riak_keystream *ks;
    riak_name_cb cb;
    void *ctx;
    bool stopped;
} name_stream;

/* Names parsed so far are handed to the callback as the body arrives. */
static bool name_sink(void *ctx, const char *data, size_t len, riak_status *abort_status) {
    name_stream *s = ctx;
    riak_status st = riak_keystream_feed(s->ks, data, len);
    if (st != RIAK_OK) {
        *abort_status = st;
        return false;
    }
    riak_strlist l;
    riak_keystream_take(s->ks, &l);
    bool more = true;
    for (size_t i = 0; more && i < l.count; i++) {
        more = s->cb(l.items[i], strlen(l.items[i]), s->ctx);
    }
    riak_strlist_free(&l);
    if (!more) {
        s->stopped = true;
        *abort_status = RIAK_E_PARSE;
    }
    return more;
}

static riak_status http_list_stream(riak_client *c, const char *type, const char *bucket,
                                    riak_name_cb cb, void *ctx) {
    riak_buf url = {0};
    int rc;
    if (bucket) {
        rc = riak_url_bucket(&url, c, type, bucket, "keys", NULL) ||
             riak_url_append_query(&url, "keys", "stream");
    } else {
        rc = riak_buf_append_str(&url, c->root) ||
             (type && (riak_buf_append_str(&url, "types/") || riak_url_append_escaped(&url, type) ||
                       riak_buf_append_str(&url, "/"))) ||
             riak_buf_append_str(&url, "buckets?buckets=stream");
    }
    name_stream s = {.ks = rc ? NULL : riak_keystream_new_field(bucket ? "keys" : "buckets"),
                     .cb = cb,
                     .ctx = ctx};
    if (s.ks == NULL) {
        riak_buf_free(&url);
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    riak_request req = {
        .method = "GET", .url = url.data, .sink = name_sink, .sink_ctx = &s, .unbounded = true};
    riak_response resp = {0};
    riak_status st = riak_perform(c, &req, &resp);
    if (s.stopped) {
        riak_clear_error(c); /* the callback asked to stop */
        st = RIAK_OK;
    } else if (st == RIAK_OK) {
        riak_strlist rest;
        if (resp.status != 200) {
            st = riak_http_error(c, &resp);
        } else if ((st = riak_keystream_finish(s.ks, &rest)) != RIAK_OK) {
            riak_set_error(c, st, "truncated listing");
        } else {
            riak_strlist_free(&rest);
        }
    }
    riak_response_free(&resp);
    riak_keystream_free(s.ks);
    riak_buf_free(&url);
    return st;
}

riak_status riak_list_keys_stream(riak_client *client, const char *type, const char *bucket,
                                  riak_name_cb cb, void *ctx) {
    if (client == NULL || cb == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    riak_status st;
    if ((st = check_type(client, type)) != RIAK_OK ||
        (st = riak_check_name(client, bucket, "bucket")) != RIAK_OK) {
        return st;
    }
    return riak_is_pb(client) ? riak_pb_list_keys(client, type, bucket, cb, ctx)
                              : http_list_stream(client, type, bucket, cb, ctx);
}

riak_status riak_list_buckets_stream(riak_client *client, const char *type, riak_name_cb cb,
                                     void *ctx) {
    if (client == NULL || cb == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    riak_status st = check_type(client, type);
    if (st != RIAK_OK) {
        return st;
    }
    return riak_is_pb(client) ? riak_pb_list_buckets(client, type, cb, ctx)
                              : http_list_stream(client, type, NULL, cb, ctx);
}

/* ---- preference lists and coverage ------------------------------------ */

void riak_preflist_free(riak_preflist *list) {
    if (list == NULL) {
        return;
    }
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].node);
    }
    free(list->items);
    memset(list, 0, sizeof *list);
}

riak_status riak_get_preflist(riak_client *client, const char *type, const char *bucket,
                              const char *key, riak_preflist *out) {
    if (client == NULL || out == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    riak_status st;
    if ((st = check_type(client, type)) != RIAK_OK ||
        (st = riak_check_name(client, bucket, "bucket")) != RIAK_OK ||
        (st = riak_check_name(client, key, "key")) != RIAK_OK) {
        return st;
    }
    if (riak_is_pb(client)) {
        return riak_pb_preflist(client, type, bucket, key, out);
    }
    /* Riak 2.x's HTTP resource ignores the key and answers the same list for
     * every key; Riak 3.0 removed it */
    return riak_set_error(client, RIAK_E_UNSUPPORTED,
                          "preference lists are served over Protocol Buffers only");
}

void riak_coverage_free(riak_coverage_plan *plan) {
    if (plan == NULL) {
        return;
    }
    for (size_t i = 0; i < plan->count; i++) {
        free(plan->entries[i].host);
        free(plan->entries[i].description);
        free(plan->entries[i].cover_context);
    }
    free(plan->entries);
    memset(plan, 0, sizeof *plan);
}

riak_status riak_get_coverage(riak_client *client, const char *type, const char *bucket,
                              unsigned min_partitions, riak_coverage_plan *out) {
    if (client == NULL || out == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    riak_status st;
    if ((st = check_type(client, type)) != RIAK_OK ||
        (st = riak_check_name(client, bucket, "bucket")) != RIAK_OK) {
        return st;
    }
    if (!riak_is_pb(client)) {
        return riak_set_error(client, RIAK_E_UNSUPPORTED,
                              "coverage plans need the Protocol Buffers transport");
    }
    return riak_pb_coverage(client, type, bucket, min_partitions, out);
}
