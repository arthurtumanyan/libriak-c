/*
 File:   riak.h
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * libriak-c: a client for the Riak KV HTTP API.
 *
 * Ownership: every pointer the library hands back through an out-parameter
 * is owned by the caller and must be released with the matching *_free()
 * function (or riak_free() for plain strings).
 *
 * Threading: call riak_global_init() once before any other thread starts.
 * A riak_client must not be used by two threads at the same time; create
 * one client per thread instead.
 */

#ifndef RIAK_RIAK_H
#define RIAK_RIAK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(RIAK_SHARED)
#ifdef RIAK_BUILDING
#define RIAK_API __declspec(dllexport)
#else
#define RIAK_API __declspec(dllimport)
#endif
#elif defined(__GNUC__)
#define RIAK_API __attribute__((visibility("default")))
#else
#define RIAK_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define RIAK_VERSION_MAJOR 2
#define RIAK_VERSION_MINOR 0
#define RIAK_VERSION_PATCH 0

typedef enum {
    RIAK_OK = 0,
    RIAK_E_INVALID_ARG, /* NULL/empty argument, CR/LF in a header value */
    RIAK_E_NOMEM,
    RIAK_E_TRANSPORT,    /* connection, DNS, timeout ... */
    RIAK_E_TLS,          /* certificate or handshake failure */
    RIAK_E_NOT_FOUND,    /* 404 */
    RIAK_E_SIBLINGS,     /* 300: the object has siblings */
    RIAK_E_PRECONDITION, /* 412: conditional request failed */
    RIAK_E_HTTP,         /* any other unexpected HTTP status */
    RIAK_E_PARSE,        /* malformed response body */
    RIAK_E_NOT_MODIFIED, /* 304: conditional get, object unchanged */
    RIAK_E_UNAUTHORIZED, /* 401/403: authentication or permission failure */
    RIAK_E_CANCELLED     /* async request dropped by riak_async_free() */
} riak_status;

typedef enum { RIAK_LOG_DEBUG, RIAK_LOG_INFO, RIAK_LOG_WARN, RIAK_LOG_ERROR } riak_log_level;
typedef void (*riak_log_fn)(riak_log_level level, const char *message, void *ctx);

/* One HTTP attempt, reported after it completes. */
typedef struct {
    const char *method;
    const char *path; /* request path and query, without scheme and host */
    const char *host; /* node the attempt went to */
    unsigned port;
    unsigned attempt;   /* 0 for the first try, then 1, 2, ... */
    riak_status status; /* RIAK_OK when a response arrived, whatever its HTTP code */
    long http_status;   /* 0 if no response */
    double duration_ms;
    size_t bytes_received; /* response body */
    bool will_retry;
} riak_request_metrics;
typedef void (*riak_metrics_fn)(const riak_request_metrics *m, void *ctx);

/* Quorum values; positive numbers are passed through as-is. */
enum { RIAK_Q_DEFAULT = 0, RIAK_Q_ONE = -1, RIAK_Q_QUORUM = -2, RIAK_Q_ALL = -3 };

typedef struct riak_client riak_client;

typedef struct {
    const char *host;
    unsigned port; /* 0 = 8098 */
} riak_node;

typedef enum {
    RIAK_BALANCE_ROUND_ROBIN = 0, /* spread requests over healthy nodes */
    RIAK_BALANCE_FAILOVER         /* always prefer the first healthy node */
} riak_balance;

typedef struct {
    const char *host;        /* default "127.0.0.1" */
    unsigned port;           /* default 8098 */
    bool tls;                /* use https */
    bool tls_verify;         /* verify peer and host name; default true */
    const char *ca_file;     /* optional CA bundle */
    long connect_timeout_ms; /* 0 = libcurl default */
    long timeout_ms;         /* 0 = no limit */
    const char *client_id;   /* sent as X-Riak-ClientId when set */
    size_t max_body_bytes;   /* response size limit; 0 = default (64 MiB) */

    /* Cluster. With n_nodes > 0 the nodes replace host/port. */
    const riak_node *nodes;
    size_t n_nodes;
    riak_balance balance;
    /* Retries go to the next node with exponential backoff and jitter.
     * Requests that never reached a node are always retried; GET, PUT and
     * DELETE are also retried after a transport error or a 503. POST
     * (counter increments, generated keys) and streams that already
     * delivered data are not. */
    unsigned max_retries;    /* default 2 */
    long retry_base_ms;      /* first backoff; default 50 */
    long retry_max_ms;       /* backoff cap; default 2000 */
    unsigned fail_threshold; /* consecutive failures that eject a node; default 3 */
    long eject_ms;           /* how long an ejected node is skipped; default 5000 */

    /* Riak security: HTTP Basic authentication (requires tls) and client
     * certificates (PEM). */
    const char *username;
    const char *password;
    const char *tls_cert_file;
    const char *tls_key_file;
    const char *tls_key_password;

    /* Observability. Callbacks run on the thread that issues the request. */
    riak_log_fn log_fn;
    riak_log_level log_level; /* minimum level passed to log_fn; default WARN */
    void *log_ctx;
    riak_metrics_fn metrics_fn;
    void *metrics_ctx;
} riak_config;

typedef struct {
    const char *host;
    unsigned port;
    bool healthy; /* not currently ejected */
    unsigned consecutive_failures;
    unsigned long long requests; /* attempts sent to this node */
    unsigned long long failures;
} riak_node_info;

typedef struct {
    const char *name;
    const char *value;
} riak_kv_pair;

typedef struct {
    char *name;
    char *value;
} riak_header;

typedef struct {
    char *data; /* NUL-terminated for convenience; len excludes it */
    size_t len;
    char *content_type;
    char *vclock;
    char *etag;
    char *last_modified;
    bool deleted;      /* tombstone sibling (X-Riak-Deleted) */
    riak_header *meta; /* user metadata, names without "X-Riak-Meta-" */
    size_t n_meta;
    riak_header *indexes; /* secondary indexes, names without "X-Riak-Index-" */
    size_t n_indexes;
    char *links; /* raw Link header value(s), comma separated */
} riak_object;

/* All siblings of one key; vclock is shared by every sibling. */
typedef struct {
    riak_object *items;
    size_t count;
    char *vclock;
} riak_object_set;

/* Choose one of `count` siblings; return its index, or >= count to give up
 * (the call then fails with RIAK_E_SIBLINGS). */
typedef size_t (*riak_resolver_fn)(const riak_object *siblings, size_t count, void *ctx);

typedef struct {
    int r, pr, w, dw, pw, rw; /* RIAK_Q_* or a replica count */
    const char *content_type; /* put only; default application/octet-stream */
    const char *vclock;       /* X-Riak-Vclock from a previous get */
    const riak_kv_pair *meta; /* X-Riak-Meta-<name>: <value> */
    size_t n_meta;
    const riak_kv_pair *indexes; /* X-Riak-Index-<name>: <value> */
    size_t n_indexes;
    const char *const *links; /* raw Link header values */
    size_t n_links;
    bool return_body;                /* put only */
    const char *bucket_type;         /* NULL = "default" */
    const char *if_none_match;       /* ETag, or "*" on put to create only */
    const char *if_match;            /* ETag */
    const char *if_modified_since;   /* HTTP date */
    const char *if_unmodified_since; /* HTTP date */
    unsigned server_timeout_ms;      /* Riak-side timeout; 0 = server default */
    riak_resolver_fn resolver;       /* get only: resolve siblings */
    void *resolver_ctx;
} riak_opts;

typedef struct {
    char **items; /* items[count] == NULL */
    size_t count;
} riak_strlist;

RIAK_API riak_status riak_global_init(void);
RIAK_API void riak_global_cleanup(void);
RIAK_API const char *riak_strerror(riak_status status);

RIAK_API void riak_config_init(riak_config *cfg);
RIAK_API riak_status riak_client_new(const riak_config *cfg, riak_client **out);
RIAK_API void riak_client_free(riak_client *client);
/* Detail for the last failed call on this client; never NULL. */
RIAK_API const char *riak_client_errmsg(const riak_client *client);
/* HTTP status of the last request, 0 if none was received. */
RIAK_API long riak_client_http_status(const riak_client *client);

RIAK_API size_t riak_client_node_count(const riak_client *client);
/* Pointers in *info stay valid for the client's lifetime. */
RIAK_API riak_status riak_client_node_info(const riak_client *client, size_t index,
                                           riak_node_info *info);
/* Ping every node now, updating health; returns the number of healthy nodes. */
RIAK_API size_t riak_client_check_nodes(riak_client *client);

RIAK_API riak_status riak_ping(riak_client *client);

/* With opts->resolver set, siblings are fetched and the resolver picks the
 * object returned; its vclock covers all siblings, so writing it back
 * resolves the conflict. */
RIAK_API riak_status riak_get(riak_client *client, const char *bucket, const char *key,
                              const riak_opts *opts, riak_object *out);
/* Fetch every sibling of a key (a single object when there are none). */
RIAK_API riak_status riak_get_siblings(riak_client *client, const char *bucket, const char *key,
                                       const riak_opts *opts, riak_object_set *out);
/* key == NULL lets Riak generate one; it is returned through created_key. */
RIAK_API riak_status riak_put(riak_client *client, const char *bucket, const char *key,
                              const void *value, size_t len, const riak_opts *opts,
                              riak_object *out, char **created_key);
/* Deleting a key that does not exist is not an error. */
RIAK_API riak_status riak_delete(riak_client *client, const char *bucket, const char *key,
                                 const riak_opts *opts);

RIAK_API riak_status riak_list_buckets(riak_client *client, riak_strlist *out);
RIAK_API riak_status riak_list_keys(riak_client *client, const char *bucket, riak_strlist *out);

/* Bucket properties as a JSON document: {"props":{...}} */
RIAK_API riak_status riak_get_bucket_props(riak_client *client, const char *bucket, char **json);
RIAK_API riak_status riak_set_bucket_props(riak_client *client, const char *bucket,
                                           const char *json);
RIAK_API riak_status riak_reset_bucket_props(riak_client *client, const char *bucket);

/* Bucket-type aware variants; type == NULL means the default type. */
RIAK_API riak_status riak_type_list_buckets(riak_client *client, const char *type,
                                            riak_strlist *out);
RIAK_API riak_status riak_type_list_keys(riak_client *client, const char *type, const char *bucket,
                                         riak_strlist *out);
RIAK_API riak_status riak_type_get_bucket_props(riak_client *client, const char *type,
                                                const char *bucket, char **json);
RIAK_API riak_status riak_type_set_bucket_props(riak_client *client, const char *type,
                                                const char *bucket, const char *json);
RIAK_API riak_status riak_type_reset_bucket_props(riak_client *client, const char *type,
                                                  const char *bucket);
/* Properties of a bucket type itself: {"props":{...}} */
RIAK_API riak_status riak_get_type_props(riak_client *client, const char *type, char **json);

/* Node statistics from /stats as a JSON object. */
RIAK_API riak_status riak_stats(riak_client *client, char **json);

/* ---- Secondary indexes (needs the leveldb or memory backend) ---------- */

typedef struct {
    const char *index;     /* "age_int", "email_bin", "$key" or "$bucket" */
    const char *value;     /* exact match; NULL for a range query */
    const char *range_min; /* range query bounds (inclusive) */
    const char *range_max;
    unsigned max_results;     /* page size; 0 = all results */
    const char *continuation; /* from the previous page */
    bool return_terms;        /* range queries: also return matching terms */
    const char *term_regex;   /* range queries: filter terms by regex */
    const char *bucket_type;  /* NULL = default type */
    unsigned server_timeout_ms;
} riak_index_query;

typedef struct {
    char **keys;  /* keys[count] == NULL */
    char **terms; /* parallel to keys when return_terms, else NULL */
    size_t count;
    char *continuation; /* next page, NULL when there is none */
} riak_index_result;

RIAK_API riak_status riak_index_query_run(riak_client *client, const char *bucket,
                                          const riak_index_query *q, riak_index_result *out);
/* Streamed variant: cb gets each match (term is NULL unless return_terms);
 * return false from cb to stop early. */
typedef bool (*riak_index_cb)(const char *key, const char *term, void *ctx);
RIAK_API riak_status riak_index_query_stream(riak_client *client, const char *bucket,
                                             const riak_index_query *q, riak_index_cb cb, void *ctx,
                                             char **continuation);
RIAK_API void riak_index_result_free(riak_index_result *res);

/* ---- Riak Search (Solr) ---------------------------------------------- */

typedef struct {
    const char *q;             /* Solr query, e.g. "name_s:alice" */
    const char *filter;        /* fq */
    const char *sort;          /* e.g. "age_i desc" */
    const char *fields;        /* fl, comma separated */
    const char *default_field; /* df */
    const char *op;            /* q.op: "AND" or "OR" */
    unsigned start;
    unsigned rows; /* 0 = server default */
} riak_search_query;

typedef struct {
    riak_header *fields; /* every stored field; multi-valued fields repeat */
    size_t n_fields;
    char *bucket_type, *bucket, *key; /* _yz_rt, _yz_rb, _yz_rk */
    double score;
} riak_search_doc;

typedef struct {
    size_t num_found;
    double max_score;
    riak_search_doc *docs;
    size_t count;
    char *raw; /* complete Solr JSON response */
} riak_search_result;

RIAK_API riak_status riak_search(riak_client *client, const char *index, const riak_search_query *q,
                                 riak_search_result *out);
RIAK_API void riak_search_result_free(riak_search_result *res);
/* schema == NULL uses _yz_default; n_val == 0 uses the server default. */
RIAK_API riak_status riak_search_index_create(riak_client *client, const char *index,
                                              const char *schema, unsigned n_val);
RIAK_API riak_status riak_search_index_get(riak_client *client, const char *index, char **json);
RIAK_API riak_status riak_search_index_list(riak_client *client, char **json);
RIAK_API riak_status riak_search_index_delete(riak_client *client, const char *index);
RIAK_API riak_status riak_search_schema_put(riak_client *client, const char *name, const char *xml);
RIAK_API riak_status riak_search_schema_get(riak_client *client, const char *name, char **xml);

/* ---- MapReduce (not available on OpenRiak 4.x) ----------------------- */

/* job is the JSON job document; *result is the JSON array of kept results. */
RIAK_API riak_status riak_mapred(riak_client *client, const char *job, char **result);
/* Streamed variant: cb gets each {phase, data} chunk as it arrives. */
typedef bool (*riak_mapred_cb)(unsigned phase, const char *data_json, void *ctx);
RIAK_API riak_status riak_mapred_stream(riak_client *client, const char *job, riak_mapred_cb cb,
                                        void *ctx);

/* ---- Data types (CRDTs) ---------------------------------------------- */
/* Buckets must belong to a bucket type created with {"datatype": "..."}. */

typedef enum {
    RIAK_DT_COUNTER,
    RIAK_DT_SET,
    RIAK_DT_MAP,
    RIAK_DT_HLL,      /* HyperLogLog: approximate distinct count (Riak 2.2+) */
    RIAK_DT_GSET,     /* grow-only set (Riak 2.9+) */
    RIAK_DT_REGISTER, /* map fields only */
    RIAK_DT_FLAG      /* map fields only */
} riak_dt_type;

typedef struct riak_dt_entry riak_dt_entry;

typedef struct {
    riak_dt_type type;
    int64_t counter; /* COUNTER value, HLL cardinality */
    char **elements; /* SET / GSET members, elements[n_elements] == NULL */
    size_t n_elements;
    char *reg;              /* REGISTER */
    bool flag;              /* FLAG */
    riak_dt_entry *entries; /* MAP fields */
    size_t n_entries;
} riak_dt_value;

struct riak_dt_entry {
    char *name;
    riak_dt_value value;
};

typedef struct {
    riak_dt_value value;
    char *context; /* opaque causal context; needed to remove set/map items */
} riak_datatype;

typedef struct {
    int r, pr, w, dw, pw; /* RIAK_Q_* or a replica count */
    bool return_body;     /* update: return the new value */
    bool no_context;      /* fetch: omit the context */
    unsigned server_timeout_ms;
} riak_dt_opts;

RIAK_API riak_status riak_dt_fetch(riak_client *client, const char *type, const char *bucket,
                                   const char *key, const riak_dt_opts *opts, riak_datatype *out);
RIAK_API void riak_datatype_free(riak_datatype *dt);
/* A map field by name and type, or NULL. */
RIAK_API const riak_dt_value *riak_dt_map_get(const riak_dt_value *map, const char *name,
                                              riak_dt_type type);

/* Update operations are built first, then sent with riak_dt_update().
 * Building never fails visibly: an allocation failure or a call that does
 * not fit the operation's type is remembered and reported by
 * riak_dt_update(). Nested operations returned by riak_dt_op_map_set() and
 * riak_dt_op_map_map() belong to their parent. */
typedef struct riak_dt_op riak_dt_op;

RIAK_API riak_dt_op *riak_dt_op_new(riak_dt_type type); /* COUNTER, SET, MAP, HLL or GSET */
RIAK_API void riak_dt_op_free(riak_dt_op *op);
RIAK_API void riak_dt_op_increment(riak_dt_op *counter, int64_t delta);
RIAK_API void riak_dt_op_add(riak_dt_op *set_hll_gset, const char *element);
RIAK_API void riak_dt_op_remove(riak_dt_op *set, const char *element); /* needs a context */
RIAK_API void riak_dt_op_map_counter(riak_dt_op *map, const char *field, int64_t delta);
RIAK_API void riak_dt_op_map_register(riak_dt_op *map, const char *field, const char *value);
RIAK_API void riak_dt_op_map_flag(riak_dt_op *map, const char *field, bool enable);
RIAK_API riak_dt_op *riak_dt_op_map_set(riak_dt_op *map, const char *field);
RIAK_API riak_dt_op *riak_dt_op_map_map(riak_dt_op *map, const char *field);
RIAK_API void riak_dt_op_map_remove(riak_dt_op *map, const char *field, riak_dt_type type);

/* key == NULL lets Riak generate one, returned through created_key.
 * context comes from a previous fetch (required to remove items). */
RIAK_API riak_status riak_dt_update(riak_client *client, const char *type, const char *bucket,
                                    const char *key, const riak_dt_op *op, const char *context,
                                    const riak_dt_opts *opts, riak_datatype *out,
                                    char **created_key);

/* ---- Asynchronous requests ------------------------------------------ */
/* An async handle owns its own client (nodes, retries, hooks) and runs many
 * requests concurrently on one thread. Callbacks run inside
 * riak_async_run() / riak_async_on_socket() / riak_async_on_timeout(); they
 * may submit further requests but must not call riak_async_free(). */

typedef struct riak_async riak_async;

typedef struct {
    riak_status status;
    long http_status;
    const char *errmsg; /* valid during the callback */
    riak_object object; /* get, and put with return_body */
    char *created_key;  /* put with key == NULL */
} riak_async_result;

/* The callback may keep res->object / res->created_key by copying them and
 * clearing the originals; whatever is left is freed when it returns. */
typedef void (*riak_async_cb)(riak_async_result *res, void *ctx);

RIAK_API riak_status riak_async_new(const riak_config *cfg, riak_async **out);
/* Pending requests are cancelled: their callbacks get RIAK_E_CANCELLED. */
RIAK_API void riak_async_free(riak_async *a);
/* The underlying client, e.g. for riak_client_node_info(). */
RIAK_API riak_client *riak_async_client(riak_async *a);
RIAK_API size_t riak_async_pending(const riak_async *a);

/* Submission validates the arguments and copies everything it needs; an
 * error is returned directly and the callback is not called. */
RIAK_API riak_status riak_async_ping(riak_async *a, riak_async_cb cb, void *ctx);
RIAK_API riak_status riak_async_get(riak_async *a, const char *bucket, const char *key,
                                    const riak_opts *opts, riak_async_cb cb, void *ctx);
RIAK_API riak_status riak_async_put(riak_async *a, const char *bucket, const char *key,
                                    const void *value, size_t len, const riak_opts *opts,
                                    riak_async_cb cb, void *ctx);
RIAK_API riak_status riak_async_delete(riak_async *a, const char *bucket, const char *key,
                                       const riak_opts *opts, riak_async_cb cb, void *ctx);

/* Built-in loop: run until nothing is pending or timeout_ms passes
 * (-1 = no limit). Returns the number of requests still pending. */
RIAK_API size_t riak_async_run(riak_async *a, long timeout_ms);

/* Integration with an external event loop (libuv, libevent, epoll ...):
 * socket_fn is told which events to watch on a descriptor (RIAK_POLL_REMOVE:
 * stop watching), timer_fn when to call riak_async_on_timeout() (-1: no
 * timer). Use either this or riak_async_run(), not both. */
enum { RIAK_POLL_IN = 1, RIAK_POLL_OUT = 2, RIAK_POLL_REMOVE = 4 };
typedef void (*riak_socket_fn)(int fd, int events, void *ctx);
typedef void (*riak_timer_fn)(long timeout_ms, void *ctx);
RIAK_API riak_status riak_async_set_loop(riak_async *a, riak_socket_fn socket_fn,
                                         riak_timer_fn timer_fn, void *ctx);
RIAK_API void riak_async_on_socket(riak_async *a, int fd, int events);
RIAK_API void riak_async_on_timeout(riak_async *a);

RIAK_API void riak_object_free(riak_object *obj);
RIAK_API void riak_object_set_free(riak_object_set *set);
RIAK_API void riak_strlist_free(riak_strlist *list);
RIAK_API void riak_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* RIAK_RIAK_H */
