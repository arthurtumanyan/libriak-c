/*
 File:   query_api.c
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later

 The Query API of Riak 3.4 (POST .../buckets/<b>/query): aggregated and
 filtered secondary index queries. It is served over HTTP only; a client on
 the Protocol Buffers transport sends it to the node's HTTP port.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "internal.h"
#include "util.h"

void riak_query_result_free(riak_query_result *res) {
    if (res == NULL) {
        return;
    }
    riak_strlist k = {.items = res->keys, .count = res->count};
    riak_strlist_free(&k);
    if (res->terms) {
        riak_strlist t = {.items = res->terms, .count = res->count};
        riak_strlist_free(&t);
    }
    free(res->raw);
    free(res->continuation);
    free(res->result_queue);
    memset(res, 0, sizeof *res);
    res->total = -1;
}

static int add_str(json_object *o, const char *name, const char *v) {
    if (v == NULL) {
        return 0;
    }
    json_object *s = json_object_new_string(v);
    return s && json_object_object_add(o, name, s) == 0 ? 0 : -1;
}

static int add_uint(json_object *o, const char *name, unsigned v) {
    if (v == 0) {
        return 0;
    }
    json_object *n = json_object_new_int64(v);
    return n && json_object_object_add(o, name, n) == 0 ? 0 : -1;
}

static char *request_json(const riak_query *q) {
    json_object *root = json_object_new_object();
    json_object *list = json_object_new_array();
    int rc = root && list ? json_object_object_add(root, "query_list", list) : -1;
    if (rc != 0) {
        json_object_put(list);
    }
    for (size_t i = 0; rc == 0 && i < q->n_parts; i++) {
        const riak_query_part *p = &q->parts[i];
        json_object *e = json_object_new_object();
        if (e == NULL || json_object_array_add(list, e) != 0) {
            json_object_put(e);
            rc = -1;
            break;
        }
        if (q->aggregation_expression) {
            rc = json_object_object_add(e, "aggregation_tag",
                                        json_object_new_int(p->aggregation_tag));
        }
        rc = rc || add_str(e, "index_name", p->index_name) ||
             add_str(e, "start_term", p->start_term) || add_str(e, "end_term", p->end_term) ||
             add_str(e, "evaluation_expression", p->evaluation_expression) ||
             add_str(e, "filter_expression", p->filter_expression) ||
             add_str(e, "regular_expression", p->regular_expression);
    }
    rc = rc || add_str(root, "aggregation_expression", q->aggregation_expression) ||
         add_str(root, "accumulation_option", q->accumulation_option) ||
         add_str(root, "accumulation_term", q->accumulation_term) ||
         add_str(root, "continuation", q->continuation) ||
         add_uint(root, "max_results", q->max_results) || add_uint(root, "timeout", q->timeout_s) ||
         add_uint(root, "inactivity_timeout", q->inactivity_timeout_s);
    if (rc == 0 && q->n_substitutions > 0) {
        json_object *subs = json_object_new_object();
        rc = subs && json_object_object_add(root, "substitutions", subs) == 0 ? 0 : -1;
        for (size_t i = 0; rc == 0 && i < q->n_substitutions; i++) {
            rc = add_str(subs, q->substitutions[i].name, q->substitutions[i].value);
        }
    }
    char *s =
        rc == 0 ? riak_strdup(json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN)) : NULL;
    json_object_put(root);
    return s;
}

/* Results come under a member named after the accumulation option: a list
 * of keys, a list of {term: key} objects, a count, or a {term: count} map. */
static riak_status parse_result(const char *json, riak_query_result *out) {
    json_object *root = json_tokener_parse(json);
    if (root == NULL || !json_object_is_type(root, json_type_object)) {
        json_object_put(root);
        return RIAK_E_PARSE;
    }
    riak_strlist keys = {0}, terms = {0};
    size_t kcap = 0, tcap = 0;
    riak_status st = RIAK_OK;
    bool have_terms = false;
    json_object_object_foreach(root, name, v) {
        if (strcmp(name, "result_queue") == 0) {
            if ((out->result_queue = riak_strdup(json_object_get_string(v))) == NULL) {
                st = RIAK_E_NOMEM;
            }
        } else if (strcmp(name, "count") == 0 || strcmp(name, "raw_count") == 0) {
            out->total = json_object_get_int64(v);
        } else if (strcmp(name, "query_complete") == 0 || strcmp(name, "queued_count") == 0 ||
                   strcmp(name, "returned_count") == 0) {
            /* evaluated below */
        } else if (json_object_is_type(v, json_type_array)) {
            size_t n = json_object_array_length(v);
            for (size_t i = 0; st == RIAK_OK && i < n; i++) {
                json_object *e = json_object_array_get_idx(v, i);
                if (json_object_is_type(e, json_type_string)) {
                    if (riak_strlist_push(&keys, &kcap, json_object_get_string(e),
                                          (size_t)json_object_get_string_len(e)) != 0) {
                        st = RIAK_E_NOMEM;
                    }
                } else if (json_object_is_type(e, json_type_object)) {
                    have_terms = true;
                    json_object_object_foreach(e, term, key) {
                        if (!json_object_is_type(key, json_type_string)) {
                            st = RIAK_E_PARSE;
                        } else if (riak_strlist_push(&terms, &tcap, term, strlen(term)) != 0 ||
                                   riak_strlist_push(&keys, &kcap, json_object_get_string(key),
                                                     (size_t)json_object_get_string_len(key)) !=
                                       0) {
                            st = RIAK_E_NOMEM;
                        }
                    }
                } else {
                    st = RIAK_E_PARSE;
                }
            }
        } else if (json_object_is_type(v, json_type_object)) {
            /* term_with_count / term_with_rawcount: term -> count */
            have_terms = true;
            json_object_object_foreach(v, term, count) {
                char num[32];
                snprintf(num, sizeof num, "%lld", (long long)json_object_get_int64(count));
                if (riak_strlist_push(&terms, &tcap, term, strlen(term)) != 0 ||
                    riak_strlist_push(&keys, &kcap, num, strlen(num)) != 0) {
                    st = RIAK_E_NOMEM;
                }
            }
        }
    }
    json_object *qc, *rc, *qd;
    if (json_object_object_get_ex(root, "query_complete", &qc) &&
        json_object_object_get_ex(root, "returned_count", &rc) &&
        json_object_object_get_ex(root, "queued_count", &qd)) {
        /* query_complete only says the query has finished filling the queue */
        out->complete =
            json_object_get_boolean(qc) && json_object_get_int64(rc) >= json_object_get_int64(qd);
    }
    json_object_put(root);
    /* every term pairs with a key: a response mixing plain keys with
     * {term: key} entries has no consistent shape */
    if (st == RIAK_OK && have_terms && terms.count != keys.count) {
        st = RIAK_E_PARSE;
    }
    if (st == RIAK_OK && keys.items == NULL &&
        (keys.items = (char **)calloc(1, sizeof(char *))) == NULL) {
        st = RIAK_E_NOMEM;
    }
    if (st == RIAK_OK && have_terms && terms.items == NULL &&
        (terms.items = (char **)calloc(1, sizeof(char *))) == NULL) {
        st = RIAK_E_NOMEM;
    }
    if (st != RIAK_OK) {
        riak_strlist_free(&keys);
        riak_strlist_free(&terms);
        return st;
    }
    out->keys = keys.items;
    out->terms = have_terms ? terms.items : NULL;
    out->count = keys.count;
    return RIAK_OK;
}

/* "<root>[types/<t>/]buckets/<b>/query" */
static int query_url(riak_buf *url, riak_client *c, const char *type, const char *bucket) {
    return riak_url_bucket(url, c, type, bucket, "query", NULL);
}

static riak_status finish(riak_client *c, riak_response *resp, riak_query_result *out) {
    if (resp->status != 200) {
        return riak_http_error_or_unsupported(c, resp, "the Query API (Riak 3.4+)");
    }
    out->raw = riak_buf_steal(&resp->body, NULL);
    if (out->raw == NULL) {
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    riak_status st = parse_result(out->raw, out);
    if (st != RIAK_OK) {
        return riak_set_error(c, st, "cannot parse query result");
    }
    const char *cont = riak_headerlist_get(&resp->headers, "X-Riak-Continuation");
    if (cont && (out->continuation = riak_strdup(cont)) == NULL) {
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    return RIAK_OK;
}

riak_status riak_query_run(riak_client *client, const char *bucket, const riak_query *q,
                           riak_query_result *out) {
    if (client == NULL || out == NULL || q == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    out->total = -1;
    riak_status st;
    if ((st = riak_check_name(client, bucket, "bucket")) != RIAK_OK ||
        (q->bucket_type &&
         (st = riak_check_name(client, q->bucket_type, "bucket type")) != RIAK_OK)) {
        return st;
    }
    if (q->parts == NULL || q->n_parts == 0) {
        return riak_set_error(client, RIAK_E_INVALID_ARG, "a query needs at least one part");
    }
    size_t pin;
    if ((st = riak_http_target(client, "the Query API", &pin)) != RIAK_OK) {
        return st;
    }
    char *body = request_json(q);
    riak_buf url = {0};
    if (body == NULL || query_url(&url, client, q->bucket_type, bucket) != 0) {
        free(body);
        riak_buf_free(&url);
        return riak_set_error(client, RIAK_E_NOMEM, "out of memory");
    }
    struct curl_slist *headers = NULL;
    riak_response resp = {0};
    st = riak_add_header(client, &headers, "Content-Type", NULL, "application/json");
    if (st == RIAK_OK) {
        /* a POST, but only reading: queued queries create one queue per run */
        riak_request req = {.method = "POST",
                            .url = url.data,
                            .headers = headers,
                            .body = body,
                            .body_len = strlen(body),
                            .pin = pin};
        st = riak_perform(client, &req, &resp);
    }
    if (st == RIAK_OK) {
        st = finish(client, &resp, out);
    }
    if (st != RIAK_OK) {
        riak_query_result_free(out);
    }
    riak_response_free(&resp);
    curl_slist_free_all(headers);
    riak_buf_free(&url);
    free(body);
    return st;
}

riak_status riak_query_fetch(riak_client *client, const char *bucket_type, const char *bucket,
                             const char *result_queue, unsigned max_results,
                             riak_query_result *out) {
    if (client == NULL || out == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    out->total = -1;
    riak_status st;
    if ((st = riak_check_name(client, bucket, "bucket")) != RIAK_OK ||
        (st = riak_check_name(client, result_queue, "result queue")) != RIAK_OK ||
        (bucket_type && (st = riak_check_name(client, bucket_type, "bucket type")) != RIAK_OK)) {
        return st;
    }
    size_t pin;
    if ((st = riak_http_target(client, "the Query API", &pin)) != RIAK_OK) {
        return st;
    }
    riak_buf url = {0};
    char num[16];
    snprintf(num, sizeof num, "%u", max_results);
    if (query_url(&url, client, bucket_type, bucket) != 0 ||
        riak_url_append_query(&url, "result_queue", result_queue) != 0 ||
        (max_results && riak_url_append_query(&url, "max_results", num) != 0)) {
        riak_buf_free(&url);
        return riak_set_error(client, RIAK_E_NOMEM, "out of memory");
    }
    /* each fetch consumes entries of the queue: never repeated */
    riak_request req = {.method = "GET", .url = url.data, .pin = pin, .no_retry = true};
    riak_response resp = {0};
    st = riak_perform(client, &req, &resp);
    if (st == RIAK_OK) {
        st = finish(client, &resp, out);
    }
    if (st != RIAK_OK) {
        riak_query_result_free(out);
    }
    riak_response_free(&resp);
    riak_buf_free(&url);
    return st;
}
