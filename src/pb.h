/*
 File:   pb.h
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later

 Protocol Buffers transport: message codes, the connection engine shared by
 the blocking and the async API, and the operations behind the public calls.
 */

#ifndef RIAK_PB_H
#define RIAK_PB_H

#include <stdint.h>

#include <openssl/ssl.h>
#include <sys/socket.h>

#include "internal.h"
#include "riak.pb-c.h"
#include "riak_dt.pb-c.h"
#include "riak_kv.pb-c.h"
#include "riak_search.pb-c.h"
#include "riak_yokozuna.pb-c.h"

/* Message codes from proto/riak_pb_messages.csv. */
enum {
    RIAK_PB_ERROR_RESP = 0,
    RIAK_PB_PING_REQ = 1,
    RIAK_PB_PING_RESP = 2,
    RIAK_PB_SET_CLIENT_ID_REQ = 5,
    RIAK_PB_SET_CLIENT_ID_RESP = 6,
    RIAK_PB_GET_SERVER_INFO_REQ = 7,
    RIAK_PB_GET_SERVER_INFO_RESP = 8,
    RIAK_PB_GET_REQ = 9,
    RIAK_PB_GET_RESP = 10,
    RIAK_PB_PUT_REQ = 11,
    RIAK_PB_PUT_RESP = 12,
    RIAK_PB_DEL_REQ = 13,
    RIAK_PB_DEL_RESP = 14,
    RIAK_PB_LIST_BUCKETS_REQ = 15,
    RIAK_PB_LIST_BUCKETS_RESP = 16,
    RIAK_PB_LIST_KEYS_REQ = 17,
    RIAK_PB_LIST_KEYS_RESP = 18,
    RIAK_PB_GET_BUCKET_REQ = 19,
    RIAK_PB_GET_BUCKET_RESP = 20,
    RIAK_PB_SET_BUCKET_REQ = 21,
    RIAK_PB_SET_BUCKET_RESP = 22,
    RIAK_PB_MAPRED_REQ = 23,
    RIAK_PB_MAPRED_RESP = 24,
    RIAK_PB_INDEX_REQ = 25,
    RIAK_PB_INDEX_RESP = 26,
    RIAK_PB_SEARCH_QUERY_REQ = 27,
    RIAK_PB_SEARCH_QUERY_RESP = 28,
    RIAK_PB_RESET_BUCKET_REQ = 29,
    RIAK_PB_RESET_BUCKET_RESP = 30,
    RIAK_PB_GET_BUCKET_TYPE_REQ = 31,
    RIAK_PB_SET_BUCKET_TYPE_REQ = 32,
    RIAK_PB_GET_PREFLIST_REQ = 33,
    RIAK_PB_GET_PREFLIST_RESP = 34,
    RIAK_PB_YZ_INDEX_GET_REQ = 54,
    RIAK_PB_YZ_INDEX_GET_RESP = 55,
    RIAK_PB_YZ_INDEX_PUT_REQ = 56,
    RIAK_PB_YZ_INDEX_DELETE_REQ = 57,
    RIAK_PB_YZ_SCHEMA_GET_REQ = 58,
    RIAK_PB_YZ_SCHEMA_GET_RESP = 59,
    RIAK_PB_YZ_SCHEMA_PUT_REQ = 60,
    RIAK_PB_COVERAGE_REQ = 70,
    RIAK_PB_COVERAGE_RESP = 71,
    RIAK_PB_DT_FETCH_REQ = 80,
    RIAK_PB_DT_FETCH_RESP = 81,
    RIAK_PB_DT_UPDATE_REQ = 82,
    RIAK_PB_DT_UPDATE_RESP = 83,
    RIAK_PB_AUTH_REQ = 253,
    RIAK_PB_AUTH_RESP = 254,
    RIAK_PB_START_TLS = 255
};

/* Quorum values on the wire (riak_pb_kv_codec). */
#define RIAK_PB_Q_ONE 4294967294u
#define RIAK_PB_Q_QUORUM 4294967293u
#define RIAK_PB_Q_ALL 4294967292u

/* One response message of a streamed request. The callback sets *done on the
 * last message; returning false stops the stream, with *err left at RIAK_OK
 * when the caller asked for it and set otherwise. */
typedef bool (*riak_pb_sink_fn)(void *ctx, const uint8_t *data, size_t len, bool *done,
                                riak_status *err);

typedef struct {
    uint8_t code;      /* request message code */
    uint8_t resp_code; /* expected response code */
    const uint8_t *payload;
    size_t len;
    riak_pb_sink_fn sink; /* NULL: the single response lands in riak_pb_response.body */
    void *sink_ctx;
    bool idempotent;
    size_t pin;       /* 0: choose a node and retry; i + 1: only node i, no retries */
    const char *name; /* message name for logs and metrics */
    const char *path; /* equivalent HTTP path for logs and metrics */
} riak_pb_request;

typedef struct {
    riak_buf body;
    bool stopped; /* a sink asked to stop */
} riak_pb_response;

/* ---- connection engine (pbconn.c) ---------------------------------- */

typedef enum {
    RIAK_PBC_CONNECTING,
    RIAK_PBC_STARTTLS,
    RIAK_PBC_HANDSHAKE,
    RIAK_PBC_AUTH,
    RIAK_PBC_CLIENT_ID,
    RIAK_PBC_READY,
    RIAK_PBC_BUSY,
    RIAK_PBC_BROKEN
} riak_pbconn_state;

/* Events a connection waits for, as returned by riak_pbconn_step(). */
enum { RIAK_PBC_WANT_READ = 1, RIAK_PBC_WANT_WRITE = 2 };

typedef struct riak_pbconn riak_pbconn;
struct riak_pbconn {
    riak_client *c;
    size_t node;
    SSL *ssl;
    struct sockaddr_storage *addrs; /* connection candidates */
    socklen_t *addr_lens;
    size_t n_addrs, next_addr;
    long long connect_deadline, request_deadline; /* ms, 0 = none */

    riak_buf out; /* frame being written */
    size_t out_off;
    uint8_t *rbuf; /* read-ahead buffer */
    size_t rpos, rlen;
    size_t hdr_len;
    riak_buf in; /* payload being read */
    size_t frame_len;

    /* current request */
    const riak_pb_request *req;
    riak_pb_response *resp;
    long long started_ms;
    size_t received;

    riak_pbconn *pool_next; /* async pool bookkeeping */
    void *owner;

    int fd;
    riak_pbconn_state state;
    int want; /* last result of riak_pbconn_step() */
    unsigned attempt;
    riak_status st;
    enum riak_retry retry;
    bool sent;      /* the request frame was handed to the socket */
    bool delivered; /* a sink saw a message */
    bool finished;
    uint8_t hdr[5]; /* frame header being read */
    char errmsg[256];
};

/* A connection to `node` is created and its connect started. NULL is returned
 * with the client error set on failure. */
riak_pbconn *riak_pbconn_open(riak_client *c, size_t node);
void riak_pbconn_free(riak_pbconn *k);
/* An idle connection kept from an earlier request can be reused. */
bool riak_pbconn_reusable(riak_pbconn *k);
/* The request is queued; it is written once the connection is ready. */
void riak_pbconn_begin(riak_pbconn *k, const riak_pb_request *req, riak_pb_response *resp,
                       unsigned attempt);
/* I/O is done as far as possible without blocking. The result is a mask of
 * RIAK_PBC_WANT_* while the request is in progress, or 0 once it finished
 * (successfully or not). */
int riak_pbconn_step(riak_pbconn *k);
/* Deadline of the current phase (ms, 0 = none). */
long long riak_pbconn_deadline(const riak_pbconn *k);
/* The request is failed with a timeout. */
void riak_pbconn_timeout(riak_pbconn *k);
/* The outcome is collected: error message, node health, metrics and log.
 * *retry says what a further attempt may do. */
riak_status riak_pbconn_finish(riak_pbconn *k, enum riak_retry *retry);

/* ---- client state and blocking requests (pb.c) --------------------- */

typedef struct riak_pb_state riak_pb_state;

riak_status riak_pb_client_init(riak_client *c);
void riak_pb_client_free(riak_client *c);
/* The TLS context, created on first use; NULL with a message in err on failure. */
SSL_CTX *riak_pb_ssl_ctx(riak_client *c, char *err, size_t cap);
/* BIO that sends with MSG_NOSIGNAL, so a closed peer never raises SIGPIPE. */
const BIO_METHOD *riak_pb_bio_method(riak_client *c);
/* Addresses of a node, resolved and cached; copies are handed out. */
riak_status riak_pb_resolve(riak_client *c, size_t node, struct sockaddr_storage **addrs,
                            socklen_t **lens, size_t *n);

riak_status riak_pb_perform(riak_client *c, const riak_pb_request *req, riak_pb_response *resp);

/* A message is packed, sent, and the response unpacked with resp_desc into
 * *out (when both are set). */
typedef struct {
    uint8_t code, resp_code;
    bool idempotent;
    size_t pin;
    const char *name;
    const char *path;
} riak_pb_call;

riak_status riak_pb_exec(riak_client *c, const riak_pb_call *call, const ProtobufCMessage *msg,
                         const ProtobufCMessageDescriptor *resp_desc, ProtobufCMessage **out);
/* Like riak_pb_exec() but every response message goes to sink. */
riak_status riak_pb_stream(riak_client *c, const riak_pb_call *call, const ProtobufCMessage *msg,
                           riak_pb_sink_fn sink, void *ctx);

/* Status and message from an RpbErrorResp payload. *retry is set for
 * overload and timeout errors. */
riak_status riak_pb_server_error(const uint8_t *data, size_t len, char *msg, size_t cap,
                                 enum riak_retry *retry);

/* Quorum option to wire value; false for an invalid value. */
bool riak_pb_quorum(int q, protobuf_c_boolean *has, uint32_t *out);
/* A string or byte range as ProtobufCBinaryData (not copied). */
ProtobufCBinaryData riak_pb_bin(const char *s);
ProtobufCBinaryData riak_pb_bytes(const void *p, size_t len);
/* Copy of binary data as a NUL-terminated string. */
char *riak_pb_strdup(ProtobufCBinaryData b);
/* Base64 for binary vector clocks, contexts and cover contexts; NULL / false
 * on ENOMEM or malformed input. A decoded buffer is freed with free(). */
char *riak_pb_b64_encode(ProtobufCBinaryData b);
bool riak_pb_b64_decode(const char *s, ProtobufCBinaryData *out);
/* "<root>[types/<t>/]buckets/<b>[/<suffix>[/<key>]]" for logs and metrics; NULL on ENOMEM. */
char *riak_pb_path(riak_client *c, const char *type, const char *bucket, const char *suffix,
                   const char *key);

/* ---- operations (pb_kv.c, pb_query.c, pb_dt.c) ---------------------- */

riak_status riak_pb_ping(riak_client *c, size_t pin);
riak_status riak_pb_get(riak_client *c, const char *bucket, const char *key, const riak_opts *o,
                        riak_object *out);
riak_status riak_pb_get_siblings(riak_client *c, const char *bucket, const char *key,
                                 const riak_opts *o, riak_object_set *out);
riak_status riak_pb_put(riak_client *c, const char *bucket, const char *key, const void *value,
                        size_t len, const riak_opts *o, riak_object *out, char **created_key);
riak_status riak_pb_delete(riak_client *c, const char *bucket, const char *key, const riak_opts *o);
riak_status riak_pb_list_buckets(riak_client *c, const char *type, riak_name_cb cb, void *ctx);
riak_status riak_pb_list_keys(riak_client *c, const char *type, const char *bucket, riak_name_cb cb,
                              void *ctx);
riak_status riak_pb_get_bucket_props(riak_client *c, const char *type, const char *bucket,
                                     char **json);
riak_status riak_pb_set_bucket_props(riak_client *c, const char *type, const char *bucket,
                                     const char *json);
riak_status riak_pb_reset_bucket_props(riak_client *c, const char *type, const char *bucket);
riak_status riak_pb_get_type_props(riak_client *c, const char *type, char **json);
riak_status riak_pb_server_info(riak_client *c, riak_server_info *out);
riak_status riak_pb_preflist(riak_client *c, const char *type, const char *bucket, const char *key,
                             riak_preflist *out);
riak_status riak_pb_coverage(riak_client *c, const char *type, const char *bucket,
                             unsigned min_partitions, riak_coverage_plan *out);

riak_status riak_pb_index(riak_client *c, const char *bucket, const riak_index_query *q,
                          riak_index_cb cb, void *ctx, char **continuation);
riak_status riak_pb_search(riak_client *c, const char *index, const riak_search_query *q,
                           riak_search_result *out);
riak_status riak_pb_search_index_create(riak_client *c, const char *index, const char *schema,
                                        unsigned n_val);
riak_status riak_pb_search_index_get(riak_client *c, const char *index, char **json);
riak_status riak_pb_search_index_delete(riak_client *c, const char *index);
riak_status riak_pb_search_schema_put(riak_client *c, const char *name, const char *xml);
riak_status riak_pb_search_schema_get(riak_client *c, const char *name, char **xml);
riak_status riak_pb_mapred(riak_client *c, const char *job, riak_mapred_cb cb, void *ctx,
                           char **result);

riak_status riak_pb_dt_fetch(riak_client *c, const char *type, const char *bucket, const char *key,
                             const riak_dt_opts *o, riak_datatype *out);
riak_status riak_pb_dt_update(riak_client *c, const char *type, const char *bucket, const char *key,
                              const riak_dt_op *op, const char *context, const riak_dt_opts *o,
                              riak_datatype *out, char **created_key);

/* ---- encoding helpers shared with the async API (pb_kv.c) ------------ */

/* A packed request with everything it points to. */
typedef struct {
    uint8_t *payload;
    size_t len;
    char *path;
    riak_pb_request req;
} riak_pb_prepared;

void riak_pb_prepared_free(riak_pb_prepared *p);
riak_status riak_pb_prepare_get(riak_client *c, const char *bucket, const char *key,
                                const riak_opts *o, riak_pb_prepared *p);
riak_status riak_pb_finish_get(riak_client *c, const riak_opts *o, riak_pb_response *resp,
                               riak_object *out);
riak_status riak_pb_prepare_put(riak_client *c, const char *bucket, const char *key,
                                const void *value, size_t len, const riak_opts *o,
                                riak_pb_prepared *p);
riak_status riak_pb_finish_put(riak_client *c, riak_pb_response *resp, riak_object *out,
                               char **created_key);
riak_status riak_pb_prepare_delete(riak_client *c, const char *bucket, const char *key,
                                   const riak_opts *o, riak_pb_prepared *p);
riak_status riak_pb_prepare_ping(riak_client *c, riak_pb_prepared *p);

/* ---- async (async_pb.c) --------------------------------------------- */

typedef struct riak_pb_async riak_pb_async;
struct riak_async;

riak_pb_async *riak_pb_async_new(riak_client *c);
void riak_pb_async_free(riak_pb_async *a);
size_t riak_pb_async_pending(const riak_pb_async *a);
enum riak_pb_async_kind { RIAK_PBA_PING, RIAK_PBA_GET, RIAK_PBA_PUT, RIAK_PBA_DELETE };
riak_status riak_pb_async_submit(riak_pb_async *a, enum riak_pb_async_kind kind, const char *bucket,
                                 const char *key, const void *value, size_t len,
                                 const riak_opts *opts, riak_async_cb cb, void *ctx);
size_t riak_pb_async_run(riak_pb_async *a, long timeout_ms);
void riak_pb_async_set_loop(riak_pb_async *a, riak_socket_fn socket_fn, riak_timer_fn timer_fn,
                            void *ctx);
void riak_pb_async_on_socket(riak_pb_async *a, int fd, int events);
void riak_pb_async_on_timeout(riak_pb_async *a);

#endif /* RIAK_PB_H */
