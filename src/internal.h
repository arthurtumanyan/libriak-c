/*
 File:   internal.h
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RIAK_INTERNAL_H
#define RIAK_INTERNAL_H

#include <curl/curl.h>

#include "riak/riak.h"
#include "util.h"

typedef struct {
    char *host;
    unsigned port;
    char *base; /* scheme://host:port, no trailing slash */
    unsigned consecutive_failures;
    long long ejected_until_ms; /* monotonic; 0 = healthy */
    unsigned long long requests;
    unsigned long long failures;
} riak_node_state;

struct riak_client {
    CURL *curl;
    const char *root; /* "/": callers build paths, riak_perform adds the node */
    riak_node_state *nodes;
    size_t n_nodes;
    size_t next_node; /* round-robin cursor */
    size_t last_node; /* node of the latest attempt, for error messages */
    unsigned long long rng;
    char *host;
    char *ca_file;
    char *client_id;
    riak_config cfg; /* string members point at the copies above */
    long http_status;
    char curl_err[CURL_ERROR_SIZE];
    char errmsg[512];
};

/* Data sink for a response body; return false to abort the transfer with
 * the status stored in *abort_status. */
typedef bool (*riak_sink_fn)(void *ctx, const char *data, size_t len, riak_status *abort_status);

typedef struct {
    const char *method; /* GET, PUT, POST, DELETE */
    const char *url;
    struct curl_slist *headers; /* owned by the caller */
    const void *body;           /* PUT/POST only */
    size_t body_len;
    riak_sink_fn sink; /* NULL: collect into riak_response.body */
    void *sink_ctx;
    bool unbounded; /* sink does not accumulate: skip the max_body_bytes cap */
    size_t pin;     /* 0: choose a node and retry; i + 1: only node i, no retries */
} riak_request;

typedef struct {
    long status;
    riak_buf body;
    riak_headerlist headers;
} riak_response;

/* Monotonic clock in milliseconds. */
long long riak_now_ms(void);

riak_status riak_set_error(riak_client *c, riak_status st, const char *fmt, ...) RIAK_PRINTF(3, 4);
void riak_clear_error(riak_client *c);

/* "<base>[types/<type>/]buckets/<bucket>[/<suffix>[/<key>]]", escaped. */
int riak_url_bucket(riak_buf *b, const riak_client *c, const char *type, const char *bucket,
                    const char *suffix, const char *key);

/* Expected body of riak_get_body(). */
enum { RIAK_BODY_RAW, RIAK_BODY_JSON_OBJECT, RIAK_BODY_JSON_ARRAY };

/* GET url, require 200 and a body of the expected kind, return the body. */
riak_status riak_get_body(riak_client *c, const char *url, int kind, char **body);

/* Validate a bucket/key argument. */
riak_status riak_check_name(riak_client *c, const char *s, const char *what);

/* Append "Name: value"; validates both parts. */
riak_status riak_add_header(riak_client *c, struct curl_slist **list, const char *prefix,
                            const char *name, const char *value);

riak_status riak_perform(riak_client *c, const riak_request *req, riak_response *resp);
void riak_response_free(riak_response *resp);
/* Map an unexpected HTTP status to an error and record a message. */
riak_status riak_http_error(riak_client *c, const riak_response *resp);

#endif /* RIAK_INTERNAL_H */
