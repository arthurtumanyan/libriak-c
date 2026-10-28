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
    char *username, *password; /* password is wiped on free */
    char *cert_file, *key_file, *key_password;
    riak_config cfg; /* string members point at the copies above */
    long http_status;
    char errmsg[512];
    struct riak_pb_state *pb; /* Protocol Buffers transport state */
};

static inline bool riak_is_pb(const riak_client *c) {
    return c->cfg.transport == RIAK_TRANSPORT_PB;
}

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

/* Validation of a bucket/key argument. */
riak_status riak_check_name(riak_client *c, const char *s, const char *what);

/* "Name: value" is appended after both parts are validated. */
riak_status riak_add_header(riak_client *c, struct curl_slist **list, const char *prefix,
                            const char *name, const char *value);

riak_status riak_perform(riak_client *c, const riak_request *req, riak_response *resp);

/* ---- operations split into prepare (build) and finish (interpret) ---- */

/* A request ready to send; owns its URL, headers and optional body copy.
 * The heap pointers inside req stay valid if the struct is copied. */
typedef struct {
    riak_buf url;
    struct curl_slist *headers;
    char *body;
    riak_request req;
} riak_prepared;

void riak_prepared_free(riak_prepared *p);

/* copy_body: keep a private copy of value (needed when the call returns
 * before the request is sent, i.e. for async requests). */
riak_status riak_kv_prepare_get(riak_client *c, const char *bucket, const char *key,
                                const riak_opts *o, riak_prepared *p);
riak_status riak_kv_finish_get(riak_client *c, const riak_opts *o, riak_response *resp,
                               riak_object *out);
riak_status riak_kv_prepare_put(riak_client *c, const char *bucket, const char *key,
                                const void *value, size_t len, const riak_opts *o, bool copy_body,
                                riak_prepared *p);
riak_status riak_kv_finish_put(riak_client *c, bool generated_key, riak_response *resp,
                               riak_object *out, char **created_key);
riak_status riak_kv_prepare_delete(riak_client *c, const char *bucket, const char *key,
                                   const riak_opts *o, riak_prepared *p);
riak_status riak_kv_finish_delete(riak_client *c, riak_response *resp);
riak_status riak_prepare_ping(riak_client *c, riak_prepared *p);
riak_status riak_finish_ping(riak_client *c, riak_response *resp);

/* ---- one attempt on one easy handle (shared by sync and async) ---- */

enum riak_retry { RIAK_RETRY_NONE, RIAK_RETRY_ANY, RIAK_RETRY_IDEMPOTENT };

typedef struct {
    riak_client *c;
    CURL *h;
    const riak_request *req;
    riak_response *resp;
    size_t node;
    unsigned attempt;
    char *url;
    struct curl_slist *extra;
    size_t received;
    riak_status abort_status;
    const char *abort_msg;
    bool aborted;
    long long started_ms;
    char curl_err[CURL_ERROR_SIZE];
    char tls_alert[96];  /* first TLS alert received from the server */
    bool cert_requested; /* the server asked for a client certificate */
} riak_attempt;

/* h is configured for req on the given node. On failure nothing needs freeing. */
riak_status riak_attempt_setup(riak_attempt *a, riak_client *c, CURL *h, size_t node,
                               unsigned attempt, const riak_request *req, riak_response *resp);
/* The outcome of a finished transfer is collected: status, error message, node
 * health, metrics and log; *retry says what a further attempt may do. */
riak_status riak_attempt_finish(riak_attempt *a, CURLcode rc, enum riak_retry *retry);
/* Appended to handshake failures that an old Erlang/OTP TLS server causes. */
#define LEGACY_HINT " (nodes on an old Erlang/OTP, such as Riak KV 2.2, need tls_legacy)"

/* The settings of riak_config::tls_legacy are applied to an OpenSSL SSL_CTX
 * (passed as void * so this header needs no OpenSSL include); 0 on success. */
int riak_tls_legacy_ctx(void *ssl_ctx);

/* Whether an HTTP request may be repeated (GET, PUT, DELETE). */
bool riak_request_idempotent(const riak_request *req);

/* ---- cluster behavior shared by every transport (cluster.c) ---- */

/* Whether another attempt is allowed after one that returned `retry`. */
bool riak_retry_allowed(const riak_client *c, bool idempotent, enum riak_retry retry,
                        unsigned attempts_done);
/* Next node (SIZE_MAX: first attempt of a request). */
size_t riak_pick_node(riak_client *c, size_t after);
/* Backoff before attempt number `attempt` + 1, in milliseconds. */
long long riak_backoff_ms(riak_client *c, unsigned attempt);
/* The health of a node is updated after an attempt; failures may eject it. */
void riak_node_result(riak_client *c, size_t node, bool ok);
/* Prefix the current error message with the node of the last attempt. */
void riak_error_add_node(riak_client *c, riak_status st);
void riak_sleep_ms(long long ms);

/* Outcome of one attempt, passed to riak_attempt_report(). */
typedef struct {
    const char *method; /* HTTP method or protocol message name */
    const char *path;
    size_t node;
    unsigned attempt;
    riak_status status;
    long http_status;
    long long started_ms;
    size_t received;
    enum riak_retry retry;
    bool will_retry;
    const char *failure; /* logged when status is RIAK_OK but the attempt failed */
    riak_transport transport;
} riak_attempt_info;

/* The metrics callback is run and a failed attempt is logged. */
void riak_attempt_report(riak_client *c, const riak_attempt_info *info);

void riak_log(riak_client *c, riak_log_level level, const char *fmt, ...) RIAK_PRINTF(3, 4);
void riak_response_free(riak_response *resp);
/* An unexpected HTTP status is mapped to an error and a message is recorded. */
riak_status riak_http_error(riak_client *c, const riak_response *resp);

#endif /* RIAK_INTERNAL_H */
