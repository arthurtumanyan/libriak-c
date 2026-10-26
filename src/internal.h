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

struct riak_client {
    CURL *curl;
    char *base_url; /* scheme://host:port/ */
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
} riak_request;

typedef struct {
    long status;
    riak_buf body;
    char *content_type;
    char *vclock;
    char *etag;
    char *last_modified;
    char *location;
} riak_response;

riak_status riak_set_error(riak_client *c, riak_status st, const char *fmt, ...) RIAK_PRINTF(3, 4);
void riak_clear_error(riak_client *c);

/* "<base>buckets/<bucket>[/<suffix>[/<key>]]", escaped. */
int riak_url_bucket(riak_buf *b, const riak_client *c, const char *bucket, const char *suffix,
                    const char *key);

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
