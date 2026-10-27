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

/**
 * @file riak.h
 * @brief Public interface of libriak-c, a client for the Riak KV HTTP API.
 *
 * @par Ownership
 * Every pointer handed back through an out-parameter is owned by the caller
 * and is released with the matching `*_free()` function, or with riak_free()
 * for plain strings. Input strings are copied when they are needed beyond
 * the call.
 *
 * @par Errors
 * Every operation returns a ::riak_status. A human-readable detail for the
 * last failure is available from riak_client_errmsg().
 *
 * @par Threading
 * riak_global_init() is called once before any other thread is started. A
 * ::riak_client is not shared between threads at the same time; one client
 * per thread is used instead. A ::riak_async handle runs many requests
 * concurrently on a single thread.
 */

#ifndef RIAK_RIAK_H
#define RIAK_RIAK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @cond INTERNAL */
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
/** @endcond */

#ifdef __cplusplus
extern "C" {
#endif

/** @defgroup core Core: status codes, configuration, client lifecycle
 *  @{ */

#define RIAK_VERSION_MAJOR 2 /**< Major version; changes on incompatible API changes. */
#define RIAK_VERSION_MINOR 0 /**< Minor version; changes when features are added. */
#define RIAK_VERSION_PATCH 0 /**< Patch version; changes on fixes only. */

/** Result of every library call. */
typedef enum {
    RIAK_OK = 0,         /**< Success. */
    RIAK_E_INVALID_ARG,  /**< NULL or empty argument, invalid option, CR/LF in a header value. */
    RIAK_E_NOMEM,        /**< Memory allocation failed. */
    RIAK_E_TRANSPORT,    /**< Connection, DNS or timeout failure. */
    RIAK_E_TLS,          /**< Certificate, handshake or client-certificate failure. */
    RIAK_E_NOT_FOUND,    /**< HTTP 404: the key, bucket type or index does not exist. */
    RIAK_E_SIBLINGS,     /**< HTTP 300: the object has siblings that were not resolved. */
    RIAK_E_PRECONDITION, /**< HTTP 412, or a data type remove of an absent element. */
    RIAK_E_HTTP,         /**< Any other unexpected HTTP status, or a response too large. */
    RIAK_E_PARSE,        /**< The response body could not be parsed. */
    RIAK_E_NOT_MODIFIED, /**< HTTP 304: a conditional get found the object unchanged. */
    RIAK_E_UNAUTHORIZED, /**< HTTP 401 or 403: authentication or permission failure. */
    RIAK_E_CANCELLED     /**< An async request was dropped by riak_async_free(). */
} riak_status;

/** Severity passed to a ::riak_log_fn. */
typedef enum {
    RIAK_LOG_DEBUG, /**< Every HTTP attempt. */
    RIAK_LOG_INFO,  /**< Informational events. */
    RIAK_LOG_WARN,  /**< Retries and node ejections. */
    RIAK_LOG_ERROR  /**< Requests that failed after the last retry. */
} riak_log_level;

/**
 * Log callback.
 * @param level Severity of the message.
 * @param message Formatted message; valid only during the call.
 * @param ctx riak_config::log_ctx.
 */
typedef void (*riak_log_fn)(riak_log_level level, const char *message, void *ctx);

/** Measurements of one HTTP attempt, reported through riak_config::metrics_fn. */
typedef struct {
    const char *method;    /**< "GET", "PUT", "POST" or "DELETE". */
    const char *path;      /**< Request path and query, without scheme and host. */
    const char *host;      /**< Node the attempt was sent to. */
    unsigned port;         /**< Port of that node. */
    unsigned attempt;      /**< 0 for the first try, 1 for the first retry, and so on. */
    riak_status status;    /**< ::RIAK_OK when a response arrived, whatever its HTTP code. */
    long http_status;      /**< HTTP status, 0 when no response was received. */
    double duration_ms;    /**< Time from sending to completion. */
    size_t bytes_received; /**< Size of the response body. */
    bool will_retry;       /**< Whether another attempt follows. */
} riak_request_metrics;

/**
 * Metrics callback, called once for every HTTP attempt.
 * @param m Measurements; valid only during the call.
 * @param ctx riak_config::metrics_ctx.
 */
typedef void (*riak_metrics_fn)(const riak_request_metrics *m, void *ctx);

/** Symbolic quorum values for riak_opts and riak_dt_opts; positive numbers
 *  are passed through as replica counts. */
enum {
    RIAK_Q_DEFAULT = 0, /**< The bucket's configured value. */
    RIAK_Q_ONE = -1,    /**< One replica. */
    RIAK_Q_QUORUM = -2, /**< A majority of replicas. */
    RIAK_Q_ALL = -3     /**< All replicas. */
};

/** Opaque client handle; created by riak_client_new(). */
typedef struct riak_client riak_client;

/** One node of a cluster, see riak_config::nodes. */
typedef struct {
    const char *host; /**< Host name or IP address (IPv6 literals are accepted). */
    unsigned port;    /**< HTTP port; 0 selects 8098. */
} riak_node;

/** Node selection strategy of a multi-node client. */
typedef enum {
    RIAK_BALANCE_ROUND_ROBIN = 0, /**< Requests are spread over the healthy nodes. */
    RIAK_BALANCE_FAILOVER         /**< The first healthy node is always preferred. */
} riak_balance;

/**
 * Client configuration. A structure is prepared with riak_config_init(),
 * which sets the defaults listed below, and then adjusted. All strings are
 * copied by riak_client_new(); the structure may be discarded afterwards.
 */
typedef struct {
    const char *host;        /**< Node address; default "127.0.0.1". Ignored when #nodes is set. */
    unsigned port;           /**< Node HTTP port; default 8098. Ignored when #nodes is set. */
    bool tls;                /**< HTTPS is used. */
    bool tls_verify;         /**< Peer certificate and host name are verified; default true. */
    const char *ca_file;     /**< CA bundle (PEM) used for verification; NULL: system store. */
    long connect_timeout_ms; /**< Connection timeout; 0: libcurl default. */
    long timeout_ms;         /**< Whole-request timeout; 0: no limit. */
    const char *client_id;   /**< Sent as X-Riak-ClientId when set. */
    size_t max_body_bytes;   /**< Response size limit; 0 selects 64 MiB. */

    /** Cluster nodes. When #n_nodes is greater than 0 they replace #host and #port. */
    const riak_node *nodes;
    size_t n_nodes;       /**< Number of entries in #nodes. */
    riak_balance balance; /**< Node selection strategy. */
    /**
     * Additional attempts after a failure; default 2. Retries go to the next
     * node after an exponential backoff with jitter. Requests that never
     * reached a node are always retried; GET, PUT and DELETE are also retried
     * after a transport error or an HTTP 503. POST requests (counter
     * increments, generated keys) and streams that already delivered data
     * are not retried, so nothing is applied twice.
     */
    unsigned max_retries;
    long retry_base_ms;      /**< First backoff delay; default 50. */
    long retry_max_ms;       /**< Upper bound of the backoff delay; default 2000. */
    unsigned fail_threshold; /**< Consecutive failures after which a node is ejected; default 3. */
    long eject_ms;           /**< Time an ejected node is skipped; default 5000. */

    const char *username; /**< Riak security user (HTTP Basic); refused unless #tls is set. */
    const char *password; /**< Password of #username; wiped from memory when the client is freed. */
    const char *tls_cert_file;    /**< Client certificate (PEM) for certificate authentication. */
    const char *tls_key_file;     /**< Private key (PEM) of #tls_cert_file. */
    const char *tls_key_password; /**< Passphrase of an encrypted #tls_key_file. */

    riak_log_fn log_fn;         /**< Log callback, run on the thread issuing the request. */
    riak_log_level log_level;   /**< Minimum level passed to #log_fn; default ::RIAK_LOG_WARN. */
    void *log_ctx;              /**< Passed to #log_fn. */
    riak_metrics_fn metrics_fn; /**< Metrics callback, called after every HTTP attempt. */
    void *metrics_ctx;          /**< Passed to #metrics_fn. */
} riak_config;

/** Health and counters of one node, see riak_client_node_info(). */
typedef struct {
    const char *host;              /**< Node host; valid for the client's lifetime. */
    unsigned port;                 /**< Node port. */
    bool healthy;                  /**< The node is not currently ejected. */
    unsigned consecutive_failures; /**< Failures since the last success. */
    unsigned long long requests;   /**< Attempts sent to this node. */
    unsigned long long failures;   /**< Attempts that failed. */
} riak_node_info;

/** Name/value pair supplied by the caller (metadata, index entries). */
typedef struct {
    const char *name;  /**< Name without the X-Riak-Meta- / X-Riak-Index- prefix. */
    const char *value; /**< Value. */
} riak_kv_pair;

/** Name/value pair returned by the library; owned by the enclosing result. */
typedef struct {
    char *name;  /**< Name. */
    char *value; /**< Value. */
} riak_header;

/** A stored object as returned by riak_get(); released with riak_object_free(). */
typedef struct {
    char *data;           /**< Value; NUL-terminated for convenience (#len excludes the NUL). */
    size_t len;           /**< Length of #data. */
    char *content_type;   /**< Content-Type of the value. */
    char *vclock;         /**< Vector clock; passed back in riak_opts::vclock on updates. */
    char *etag;           /**< ETag; usable in riak_opts::if_none_match / riak_opts::if_match. */
    char *last_modified;  /**< Last-Modified, HTTP date format. */
    bool deleted;         /**< The sibling is a tombstone (X-Riak-Deleted). */
    riak_header *meta;    /**< User metadata; names without the X-Riak-Meta- prefix. */
    size_t n_meta;        /**< Number of entries in #meta. */
    riak_header *indexes; /**< Secondary index entries; names without the X-Riak-Index- prefix. */
    size_t n_indexes;     /**< Number of entries in #indexes. */
    char *links;          /**< Raw Link header value(s), comma separated; NULL when absent. */
} riak_object;

/** All siblings of one key; released with riak_object_set_free(). */
typedef struct {
    riak_object *items; /**< The siblings. */
    size_t count;       /**< Number of siblings. */
    char *vclock;       /**< Vector clock shared by every sibling. */
} riak_object_set;

/**
 * Sibling resolver, see riak_opts::resolver.
 * @param siblings The siblings of the key.
 * @param count Number of siblings (at least 2).
 * @param ctx riak_opts::resolver_ctx.
 * @return Index of the sibling to return, or a value >= @p count to give up,
 *         in which case the call fails with ::RIAK_E_SIBLINGS.
 */
typedef size_t (*riak_resolver_fn)(const riak_object *siblings, size_t count, void *ctx);

/** Per-request options for object operations; a zero-initialized structure
 *  (or NULL) selects the server defaults. */
typedef struct {
    int r;  /**< Read quorum (::RIAK_Q_DEFAULT and friends, or a replica count). */
    int pr; /**< Primary read quorum. */
    int w;  /**< Write quorum. */
    int dw; /**< Durable write quorum. */
    int pw; /**< Primary write quorum. */
    int rw; /**< Delete quorum. */
    const char *content_type;    /**< Put: Content-Type; default application/octet-stream. */
    const char *vclock;          /**< Vector clock from a previous get, for updates and deletes. */
    const riak_kv_pair *meta;    /**< Put: user metadata, sent as X-Riak-Meta-name. */
    size_t n_meta;               /**< Number of entries in #meta. */
    const riak_kv_pair *indexes; /**< Put: secondary index entries, sent as X-Riak-Index-name. */
    size_t n_indexes;            /**< Number of entries in #indexes. */
    const char *const *links;    /**< Put: raw Link header values. */
    size_t n_links;              /**< Number of entries in #links. */
    bool return_body;            /**< Put: the stored object is returned. */
    const char *bucket_type;     /**< Bucket type; NULL selects "default". */
    const char *if_none_match;   /**< ETag; "*" on put creates the key only if it is absent. */
    const char *if_match;        /**< ETag the stored object must have. */
    const char *if_modified_since;   /**< HTTP date. */
    const char *if_unmodified_since; /**< HTTP date. */
    unsigned server_timeout_ms;      /**< Riak-side timeout; 0 selects the server default. */
    /** Get: resolves siblings. The chosen sibling is returned with the vector
     *  clock covering all of them, so writing it back resolves the conflict. */
    riak_resolver_fn resolver;
    void *resolver_ctx; /**< Passed to #resolver. */
} riak_opts;

/** NULL-terminated string list; released with riak_strlist_free(). */
typedef struct {
    char **items; /**< The strings; `items[count]` is NULL. */
    size_t count; /**< Number of strings. */
} riak_strlist;

/**
 * Initializes libcurl. Called once, before any other thread is started.
 * @return ::RIAK_OK, or ::RIAK_E_TRANSPORT when libcurl cannot be initialized.
 */
RIAK_API riak_status riak_global_init(void);

/** Releases what riak_global_init() set up. Called once, after all clients are freed. */
RIAK_API void riak_global_cleanup(void);

/**
 * Short description of a status code.
 * @param status Status code.
 * @return A static string; "unknown error" for values outside ::riak_status.
 */
RIAK_API const char *riak_strerror(riak_status status);

/**
 * Fills @p cfg with the defaults documented in ::riak_config.
 * @param cfg Configuration to fill.
 */
RIAK_API void riak_config_init(riak_config *cfg);

/**
 * Creates a client.
 * @param cfg Configuration; NULL selects the defaults (127.0.0.1:8098).
 * @param out Receives the client.
 * @return ::RIAK_OK, ::RIAK_E_INVALID_ARG for an invalid configuration
 *         (bad host, port, header value, credentials without TLS),
 *         or ::RIAK_E_NOMEM.
 */
RIAK_API riak_status riak_client_new(const riak_config *cfg, riak_client **out);

/**
 * Frees a client; NULL is accepted. Stored secrets are wiped.
 * @param client Client to free; NULL is accepted.
 */
RIAK_API void riak_client_free(riak_client *client);

/**
 * Detail of the last failed call on @p client; never NULL.
 * @param client Client handle.
 */
RIAK_API const char *riak_client_errmsg(const riak_client *client);

/**
 * HTTP status of the last request on @p client; 0 when none was received.
 * @param client Client handle.
 */
RIAK_API long riak_client_http_status(const riak_client *client);

/**
 * Number of nodes the client was configured with (at least 1).
 * @param client Client handle.
 */
RIAK_API size_t riak_client_node_count(const riak_client *client);

/**
 * Health and counters of one node.
 * @param client Client handle.
 * @param index Node index, below riak_client_node_count().
 * @param info Receives the data; its strings stay valid for the client's lifetime.
 * @return ::RIAK_OK or ::RIAK_E_INVALID_ARG.
 */
RIAK_API riak_status riak_client_node_info(const riak_client *client, size_t index,
                                           riak_node_info *info);

/**
 * Pings every node once, without retries, and updates node health.
 * @param client Client handle.
 * @return Number of nodes that answered.
 */
RIAK_API size_t riak_client_check_nodes(riak_client *client);

/**
 * Checks that a node answers /ping.
 * @param client Client handle.
 */
RIAK_API riak_status riak_ping(riak_client *client);

/** @} */

/** @defgroup kv Objects and buckets
 *  @{ */

/**
 * Fetches an object.
 * @param client Client handle.
 * @param bucket Bucket name.
 * @param key Key.
 * @param opts Options; NULL selects the defaults. With riak_opts::resolver
 *        set, siblings are fetched and resolved.
 * @param out Receives the object; released with riak_object_free().
 * @return ::RIAK_OK, ::RIAK_E_NOT_FOUND, ::RIAK_E_SIBLINGS (unresolved),
 *         ::RIAK_E_NOT_MODIFIED (conditional get), or another error.
 */
RIAK_API riak_status riak_get(riak_client *client, const char *bucket, const char *key,
                              const riak_opts *opts, riak_object *out);

/**
 * Fetches every sibling of a key; a key without siblings gives a set of one.
 * @param client Client handle.
 * @param bucket Bucket name.
 * @param key Key.
 * @param opts Per-request options; NULL selects the defaults.
 * @param out Receives the siblings; released with riak_object_set_free().
 */
RIAK_API riak_status riak_get_siblings(riak_client *client, const char *bucket, const char *key,
                                       const riak_opts *opts, riak_object_set *out);

/**
 * Stores an object.
 * @param client Client handle.
 * @param bucket Bucket name.
 * @param key Key, or NULL to have Riak generate one.
 * @param value Value bytes; may be NULL only when @p len is 0.
 * @param len Length of @p value in bytes.
 * @param opts Per-request options; NULL selects the defaults.
 * @param out Receives the stored object when riak_opts::return_body
 *        is set; may be NULL.
 * @param created_key Receives the generated key when @p key is NULL;
 *        released with riak_free(). May be NULL.
 * @return ::RIAK_OK, ::RIAK_E_PRECONDITION for a failed condition, or another error.
 */
RIAK_API riak_status riak_put(riak_client *client, const char *bucket, const char *key,
                              const void *value, size_t len, const riak_opts *opts,
                              riak_object *out, char **created_key);

/**
 * Deletes a key; deleting a key that does not exist is not an error.
 * @param client Client handle.
 * @param bucket Bucket name.
 * @param key Key.
 * @param opts Per-request options; NULL selects the defaults.
 */
RIAK_API riak_status riak_delete(riak_client *client, const char *bucket, const char *key,
                                 const riak_opts *opts);

/**
 * Lists the buckets of the default bucket type. Expensive on large clusters.
 * @param client Client handle.
 * @param out Receives the names; released with riak_strlist_free().
 */
RIAK_API riak_status riak_list_buckets(riak_client *client, riak_strlist *out);

/**
 * Lists the keys of a bucket (streamed from the server). Expensive on large
 * clusters; secondary indexes are preferred in production.
 * @param client Client handle.
 * @param bucket Bucket name.
 * @param out Receives the keys; released with riak_strlist_free().
 */
RIAK_API riak_status riak_list_keys(riak_client *client, const char *bucket, riak_strlist *out);

/**
 * Bucket properties as a JSON document `{"props":{...}}`.
 * @param client Client handle.
 * @param bucket Bucket name.
 * @param json Receives the document; released with riak_free().
 */
RIAK_API riak_status riak_get_bucket_props(riak_client *client, const char *bucket, char **json);

/**
 * Sets bucket properties from a JSON document `{"props":{...}}`.
 * @param client Client handle.
 * @param bucket Bucket name.
 * @param json JSON document `{"props":{...}}`.
 */
RIAK_API riak_status riak_set_bucket_props(riak_client *client, const char *bucket,
                                           const char *json);

/**
 * Resets bucket properties to the defaults of the bucket type.
 * @param client Client handle.
 * @param bucket Bucket name.
 */
RIAK_API riak_status riak_reset_bucket_props(riak_client *client, const char *bucket);

/**
 * riak_list_buckets() for bucket type @p type (NULL: default type).
 * @param client Client handle.
 * @param type Bucket type; NULL selects the default type.
 * @param out Receives the names; released with riak_strlist_free().
 */
RIAK_API riak_status riak_type_list_buckets(riak_client *client, const char *type,
                                            riak_strlist *out);
/**
 * riak_list_keys() for bucket type @p type (NULL: default type).
 * @param client Client handle.
 * @param type Bucket type; NULL selects the default type.
 * @param bucket Bucket name.
 * @param out Receives the keys; released with riak_strlist_free().
 */
RIAK_API riak_status riak_type_list_keys(riak_client *client, const char *type, const char *bucket,
                                         riak_strlist *out);
/**
 * riak_get_bucket_props() for bucket type @p type (NULL: default type).
 * @param client Client handle.
 * @param type Bucket type; NULL selects the default type.
 * @param bucket Bucket name.
 * @param json Receives the document; released with riak_free().
 */
RIAK_API riak_status riak_type_get_bucket_props(riak_client *client, const char *type,
                                                const char *bucket, char **json);
/**
 * riak_set_bucket_props() for bucket type @p type (NULL: default type).
 * @param client Client handle.
 * @param type Bucket type; NULL selects the default type.
 * @param bucket Bucket name.
 * @param json JSON document `{"props":{...}}`.
 */
RIAK_API riak_status riak_type_set_bucket_props(riak_client *client, const char *type,
                                                const char *bucket, const char *json);
/**
 * riak_reset_bucket_props() for bucket type @p type (NULL: default type).
 * @param client Client handle.
 * @param type Bucket type; NULL selects the default type.
 * @param bucket Bucket name.
 */
RIAK_API riak_status riak_type_reset_bucket_props(riak_client *client, const char *type,
                                                  const char *bucket);

/**
 * Properties of a bucket type as a JSON document `{"props":{...}}`.
 * @param client Client handle.
 * @param type Bucket type name.
 * @param json Receives the document; released with riak_free().
 * @return ::RIAK_OK, or ::RIAK_E_NOT_FOUND for an unknown type.
 */
RIAK_API riak_status riak_get_type_props(riak_client *client, const char *type, char **json);

/**
 * Node statistics (/stats) as a JSON object.
 * @param client Client handle.
 * @param json Receives the document; released with riak_free().
 */
RIAK_API riak_status riak_stats(riak_client *client, char **json);

/** @} */

/** @defgroup queries Secondary indexes, Search and MapReduce
 *  @{ */

/** A secondary index query; needs the leveldb or memory backend. Exactly one
 *  of #value or the #range_min / #range_max pair is set. */
typedef struct {
    const char *index;          /**< "age_int", "email_bin", "$key" or "$bucket". */
    const char *value;          /**< Exact match; NULL for a range query. */
    const char *range_min;      /**< Inclusive lower bound of a range query. */
    const char *range_max;      /**< Inclusive upper bound of a range query. */
    unsigned max_results;       /**< Page size; 0 returns all results. */
    const char *continuation;   /**< riak_index_result::continuation of the previous page. */
    bool return_terms;          /**< Range queries: matching terms are returned too. */
    const char *term_regex;     /**< Range queries: terms are filtered by this regex. */
    const char *bucket_type;    /**< Bucket type; NULL selects the default type. */
    unsigned server_timeout_ms; /**< Riak-side timeout; 0 selects the server default. */
} riak_index_query;

/** One page of index results; released with riak_index_result_free(). */
typedef struct {
    char **keys;        /**< Matching keys; `keys[count]` is NULL. */
    char **terms;       /**< Matching terms parallel to #keys with return_terms, otherwise NULL. */
    size_t count;       /**< Number of matches. */
    char *continuation; /**< Token for the next page; NULL on the last page. */
} riak_index_result;

/**
 * Runs a secondary index query and returns one page.
 * @param client Client handle.
 * @param bucket Bucket name.
 * @param q The query.
 * @param out Receives the page; released with riak_index_result_free().
 */
RIAK_API riak_status riak_index_query_run(riak_client *client, const char *bucket,
                                          const riak_index_query *q, riak_index_result *out);

/**
 * Callback of riak_index_query_stream().
 * @param key Matching key.
 * @param term Matching term with return_terms, otherwise NULL.
 * @return true to continue, false to stop the stream (the transfer is aborted).
 */
typedef bool (*riak_index_cb)(const char *key, const char *term, void *ctx);

/**
 * Runs a secondary index query with results streamed to @p cb as they arrive.
 * @param client Client handle.
 * @param bucket Bucket name.
 * @param q The query.
 * @param cb Called with each match.
 * @param ctx Passed unchanged to the callback.
 * @param continuation Receives the continuation token when the server sends
 *        one (released with riak_free()); may be NULL.
 */
RIAK_API riak_status riak_index_query_stream(riak_client *client, const char *bucket,
                                             const riak_index_query *q, riak_index_cb cb, void *ctx,
                                             char **continuation);

/**
 * Frees a ::riak_index_result; NULL is accepted.
 * @param res Result to clear; NULL is accepted.
 */
RIAK_API void riak_index_result_free(riak_index_result *res);

/** A Riak Search (Solr) query. */
typedef struct {
    const char *q;             /**< Solr query, e.g. "name_s:alice"; required. */
    const char *filter;        /**< Filter query (fq). */
    const char *sort;          /**< Sort order, e.g. "age_i desc". */
    const char *fields;        /**< Returned fields (fl), comma separated. */
    const char *default_field; /**< Default field (df). */
    const char *op;            /**< Default operator (q.op): "AND" or "OR". */
    unsigned start;            /**< Offset of the first document. */
    unsigned rows;             /**< Page size; 0 selects the server default. */
} riak_search_query;

/** One matching document. */
typedef struct {
    riak_header *fields; /**< Every returned field; multi-valued fields repeat. */
    size_t n_fields;     /**< Number of entries in #fields. */
    char *bucket_type;   /**< Bucket type of the object (_yz_rt). */
    char *bucket;        /**< Bucket of the object (_yz_rb). */
    char *key;           /**< Key of the object (_yz_rk). */
    double score;        /**< Relevance score. */
} riak_search_doc;

/** Result of riak_search(); released with riak_search_result_free(). */
typedef struct {
    size_t num_found;      /**< Total number of matches, beyond this page. */
    double max_score;      /**< Highest score. */
    riak_search_doc *docs; /**< Documents of this page. */
    size_t count;          /**< Number of entries in #docs. */
    char *raw;             /**< Complete Solr JSON response. */
} riak_search_result;

/**
 * Runs a Search query against a search index.
 * @param client Client handle.
 * @param index Search index name.
 * @param q The query.
 * @param out Receives the result; released with riak_search_result_free().
 * @return ::RIAK_OK, ::RIAK_E_NOT_FOUND for an unknown index, or another error.
 */
RIAK_API riak_status riak_search(riak_client *client, const char *index, const riak_search_query *q,
                                 riak_search_result *out);

/**
 * Frees a ::riak_search_result; NULL is accepted.
 * @param res Result to clear; NULL is accepted.
 */
RIAK_API void riak_search_result_free(riak_search_result *res);

/**
 * Creates (or updates) a search index. Creation completes asynchronously on
 * the server; the index is usable once riak_search_index_get() finds it and a
 * query on it succeeds.
 * @param client Client handle.
 * @param index Search index name.
 * @param schema Schema name; NULL selects _yz_default.
 * @param n_val Replica count; 0 selects the server default.
 */
RIAK_API riak_status riak_search_index_create(riak_client *client, const char *index,
                                              const char *schema, unsigned n_val);

/**
 * Definition of a search index as JSON; released with riak_free().
 * @param client Client handle.
 * @param index Search index name.
 * @param json Receives the definition; released with riak_free().
 */
RIAK_API riak_status riak_search_index_get(riak_client *client, const char *index, char **json);

/**
 * All search indexes as a JSON array; released with riak_free().
 * @param client Client handle.
 * @param json Receives the JSON array; released with riak_free().
 */
RIAK_API riak_status riak_search_index_list(riak_client *client, char **json);

/**
 * Deletes a search index.
 * @param client Client handle.
 * @param index Search index name.
 */
RIAK_API riak_status riak_search_index_delete(riak_client *client, const char *index);

/**
 * Stores a Solr schema (XML) under @p name.
 * @param client Client handle.
 * @param name Schema name.
 * @param xml Schema document.
 */
RIAK_API riak_status riak_search_schema_put(riak_client *client, const char *name, const char *xml);

/**
 * Fetches a Solr schema as XML; released with riak_free().
 * @param client Client handle.
 * @param name Schema name.
 * @param xml Receives the schema; released with riak_free().
 */
RIAK_API riak_status riak_search_schema_get(riak_client *client, const char *name, char **xml);

/**
 * Runs a MapReduce job (not available on OpenRiak 4.x).
 * @param client Client handle.
 * @param job The JSON job document.
 * @param result Receives the JSON result; released with riak_free().
 */
RIAK_API riak_status riak_mapred(riak_client *client, const char *job, char **result);

/**
 * Callback of riak_mapred_stream().
 * @param phase Index of the phase that produced the data.
 * @param data_json JSON value with the phase output; valid only during the call.
 * @return true to continue, false to stop the stream.
 */
typedef bool (*riak_mapred_cb)(unsigned phase, const char *data_json, void *ctx);

/**
 * Runs a MapReduce job with each phase result streamed to @p cb.
 * @param client Client handle.
 * @param job The JSON job document.
 * @param cb Called with each phase result.
 * @param ctx Passed unchanged to the callback.
 */
RIAK_API riak_status riak_mapred_stream(riak_client *client, const char *job, riak_mapred_cb cb,
                                        void *ctx);

/** @} */

/** @defgroup datatypes Data types (CRDTs)
 *  Buckets belong to a bucket type created with a `datatype` property, e.g.
 *  `{"props":{"datatype":"map"}}`.
 *  @{ */

/** Kind of a data type value. */
typedef enum {
    RIAK_DT_COUNTER,  /**< Counter. */
    RIAK_DT_SET,      /**< Set of strings. */
    RIAK_DT_MAP,      /**< Map of typed fields. */
    RIAK_DT_HLL,      /**< HyperLogLog: approximate distinct count (Riak 2.2+). */
    RIAK_DT_GSET,     /**< Grow-only set (Riak 2.9+). */
    RIAK_DT_REGISTER, /**< Register; map fields only. */
    RIAK_DT_FLAG      /**< Flag; map fields only. */
} riak_dt_type;

/** One field of a map. */
typedef struct riak_dt_entry riak_dt_entry;

/** A data type value; which members are used depends on #type. */
typedef struct {
    riak_dt_type type;      /**< Kind of value. */
    int64_t counter;        /**< Counter value, or HyperLogLog cardinality. */
    char **elements;        /**< Set / grow-only set members; `elements[n_elements]` is NULL. */
    size_t n_elements;      /**< Number of members. */
    char *reg;              /**< Register value. */
    bool flag;              /**< Flag value. */
    riak_dt_entry *entries; /**< Map fields. */
    size_t n_entries;       /**< Number of map fields. */
} riak_dt_value;

/** One field of a map value. */
struct riak_dt_entry {
    char *name;          /**< Field name without the type suffix. */
    riak_dt_value value; /**< Field value. */
};

/** A fetched data type; released with riak_datatype_free(). */
typedef struct {
    riak_dt_value value; /**< The value. */
    char *context;       /**< Opaque causal context, passed back to remove set/map items. */
} riak_datatype;

/** Options for data type calls; a zero-initialized structure (or NULL) selects the defaults. */
typedef struct {
    int r;                      /**< Read quorum. */
    int pr;                     /**< Primary read quorum. */
    int w;                      /**< Write quorum. */
    int dw;                     /**< Durable write quorum. */
    int pw;                     /**< Primary write quorum. */
    bool return_body;           /**< Update: the new value is returned. */
    bool no_context;            /**< Fetch: the context is omitted. */
    unsigned server_timeout_ms; /**< Riak-side timeout; 0 selects the server default. */
} riak_dt_opts;

/**
 * Fetches a data type.
 * @param client Client handle.
 * @param type Bucket type created with a datatype property.
 * @param bucket Bucket name.
 * @param key Key.
 * @param opts Options; NULL selects the defaults.
 * @param out Receives the value; released with riak_datatype_free().
 * @return ::RIAK_OK, ::RIAK_E_NOT_FOUND, or another error.
 */
RIAK_API riak_status riak_dt_fetch(riak_client *client, const char *type, const char *bucket,
                                   const char *key, const riak_dt_opts *opts, riak_datatype *out);

/**
 * Frees a ::riak_datatype; NULL is accepted.
 * @param dt Data type to clear; NULL is accepted.
 */
RIAK_API void riak_datatype_free(riak_datatype *dt);

/**
 * Looks up a map field.
 * @param map Map value.
 * @param name Field name without the type suffix.
 * @param type Type of the field.
 * @return The field value, or NULL when @p map is not a map or has no such field.
 */
RIAK_API const riak_dt_value *riak_dt_map_get(const riak_dt_value *map, const char *name,
                                              riak_dt_type type);

/**
 * Opaque update operation. Operations are built with the `riak_dt_op_*`
 * calls and sent with riak_dt_update(). Building never fails visibly: an
 * allocation failure, a call that does not fit the operation's type, counter
 * overflow or maps nested deeper than 32 levels are remembered and reported
 * by riak_dt_update(). Nested operations from riak_dt_op_map_set() and
 * riak_dt_op_map_map() belong to their parent.
 */
typedef struct riak_dt_op riak_dt_op;

/** Creates an operation for a counter, set, map, HyperLogLog or grow-only set;
 *  NULL for other types or on allocation failure. Freed with riak_dt_op_free(). * @param type
 * ::RIAK_DT_COUNTER, ::RIAK_DT_SET, ::RIAK_DT_MAP, ::RIAK_DT_HLL or ::RIAK_DT_GSET.
 */
RIAK_API riak_dt_op *riak_dt_op_new(riak_dt_type type);
/**
 * Frees an operation and its nested operations; NULL is accepted.
 * @param op Operation to free.
 */
RIAK_API void riak_dt_op_free(riak_dt_op *op);
/**
 * Counter: adds @p delta (negative values decrement); repeated calls accumulate.
 * @param counter Counter operation.
 * @param delta Amount added; negative values decrement.
 */
RIAK_API void riak_dt_op_increment(riak_dt_op *counter, int64_t delta);
/**
 * Set, HyperLogLog or grow-only set: adds an element.
 * @param set_hll_gset Set, HyperLogLog or grow-only set operation.
 * @param element Element value.
 */
RIAK_API void riak_dt_op_add(riak_dt_op *set_hll_gset, const char *element);
/**
 * Set: removes an element; the context of a previous fetch is passed to riak_dt_update().
 * @param set Set operation.
 * @param element Element value.
 */
RIAK_API void riak_dt_op_remove(riak_dt_op *set, const char *element);
/**
 * Map: adds @p delta to a counter field.
 * @param map Map operation.
 * @param field Map field name.
 * @param delta Amount added; negative values decrement.
 */
RIAK_API void riak_dt_op_map_counter(riak_dt_op *map, const char *field, int64_t delta);
/**
 * Map: sets a register field; the last value set wins.
 * @param map Map operation.
 * @param field Map field name.
 * @param value New register value.
 */
RIAK_API void riak_dt_op_map_register(riak_dt_op *map, const char *field, const char *value);
/**
 * Map: enables or disables a flag field.
 * @param map Map operation.
 * @param field Map field name.
 * @param enable true to enable, false to disable.
 */
RIAK_API void riak_dt_op_map_flag(riak_dt_op *map, const char *field, bool enable);
/**
 * Map: the operation on a set field (created on first use, owned by @p map).
 * @param map Map operation.
 * @param field Map field name.
 */
RIAK_API riak_dt_op *riak_dt_op_map_set(riak_dt_op *map, const char *field);
/**
 * Map: the operation on a nested map field (created on first use, owned by @p map).
 * @param map Map operation.
 * @param field Map field name.
 */
RIAK_API riak_dt_op *riak_dt_op_map_map(riak_dt_op *map, const char *field);
/**
 * Map: removes a field of the given type; needs a context.
 * @param map Map operation.
 * @param field Map field name.
 * @param type Type of the field.
 */
RIAK_API void riak_dt_op_map_remove(riak_dt_op *map, const char *field, riak_dt_type type);

/**
 * Applies an operation.
 * @param client Client handle.
 * @param type Bucket type created with a datatype property.
 * @param bucket Bucket name.
 * @param key Key, or NULL to have Riak generate one.
 * @param op Operation built with the riak_dt_op_* calls.
 * @param context riak_datatype::context of a previous fetch; required for removals.
 * @param opts Options; NULL selects the defaults.
 * @param out Receives the new value with riak_dt_opts::return_body; may be NULL.
 * @param created_key Receives the generated key when @p key is NULL; released with riak_free().
 * @return ::RIAK_OK, ::RIAK_E_INVALID_ARG for an empty or misused operation,
 *         ::RIAK_E_PRECONDITION when an absent element is removed without a
 *         context, ::RIAK_E_HTTP for an operation that does not fit the bucket type.
 */
RIAK_API riak_status riak_dt_update(riak_client *client, const char *type, const char *bucket,
                                    const char *key, const riak_dt_op *op, const char *context,
                                    const riak_dt_opts *opts, riak_datatype *out,
                                    char **created_key);

/** @} */

/** @defgroup async Asynchronous requests
 *  An async handle owns its own client (nodes, retries, hooks) and runs many
 *  requests concurrently on one thread. Callbacks run inside riak_async_run(),
 *  riak_async_on_socket() and riak_async_on_timeout(); further requests may be
 *  submitted from a callback, but riak_async_free() is not called from one.
 *  @{ */

/** Opaque async handle; created by riak_async_new(). */
typedef struct riak_async riak_async;

/** Outcome of an async request, passed to its ::riak_async_cb. */
typedef struct {
    riak_status status; /**< Result of the request. */
    long http_status;   /**< HTTP status; 0 when no response was received. */
    const char *errmsg; /**< Error detail; valid only during the callback. */
    riak_object object; /**< Get, and put with return_body: the object. */
    char *created_key;  /**< Put with a NULL key: the generated key. */
} riak_async_result;

/**
 * Completion callback. riak_async_result::object and
 * riak_async_result::created_key may be kept by copying them and clearing the
 * originals; whatever remains is freed when the callback returns.
 */
typedef void (*riak_async_cb)(riak_async_result *res, void *ctx);

/**
 * Creates an async handle with the same configuration as riak_client_new().
 * @param cfg Configuration; NULL selects the defaults.
 * @param out Receives the handle.
 */
RIAK_API riak_status riak_async_new(const riak_config *cfg, riak_async **out);
/**
 * Frees the handle; pending requests are completed with ::RIAK_E_CANCELLED.
 * @param a Handle to free; NULL is accepted.
 */
RIAK_API void riak_async_free(riak_async *a);
/**
 * The underlying client, e.g. for riak_client_node_info().
 * @param a Async handle.
 */
RIAK_API riak_client *riak_async_client(riak_async *a);
/**
 * Number of requests not yet completed.
 * @param a Async handle.
 */
RIAK_API size_t riak_async_pending(const riak_async *a);

/**
 * Submits a ping. Submission functions validate their arguments and copy
 * everything needed later; an error is returned directly and the callback is
 * not called.
 * @param a Async handle.
 * @param cb Completion callback.
 * @param ctx Passed unchanged to the callback.
 */
RIAK_API riak_status riak_async_ping(riak_async *a, riak_async_cb cb, void *ctx);
/**
 * Submits a riak_get(); only riak_opts::resolver is used after submission.
 * @param a Async handle.
 * @param bucket Bucket name.
 * @param key Key.
 * @param opts Per-request options; NULL selects the defaults.
 * @param cb Completion callback.
 * @param ctx Passed unchanged to the callback.
 */
RIAK_API riak_status riak_async_get(riak_async *a, const char *bucket, const char *key,
                                    const riak_opts *opts, riak_async_cb cb, void *ctx);
/**
 * Submits a riak_put(); @p value is copied.
 * @param a Async handle.
 * @param bucket Bucket name.
 * @param key Key, or NULL to have Riak generate one.
 * @param value Value bytes; copied. May be NULL only when @p len is 0.
 * @param len Length of @p value in bytes.
 * @param opts Per-request options; NULL selects the defaults.
 * @param cb Completion callback.
 * @param ctx Passed unchanged to the callback.
 */
RIAK_API riak_status riak_async_put(riak_async *a, const char *bucket, const char *key,
                                    const void *value, size_t len, const riak_opts *opts,
                                    riak_async_cb cb, void *ctx);
/**
 * Submits a riak_delete().
 * @param a Async handle.
 * @param bucket Bucket name.
 * @param key Key.
 * @param opts Per-request options; NULL selects the defaults.
 * @param cb Completion callback.
 * @param ctx Passed unchanged to the callback.
 */
RIAK_API riak_status riak_async_delete(riak_async *a, const char *bucket, const char *key,
                                       const riak_opts *opts, riak_async_cb cb, void *ctx);

/**
 * Built-in event loop: requests are processed until none is pending or
 * @p timeout_ms has passed.
 * @param a Async handle.
 * @param timeout_ms Time limit; -1 for no limit.
 * @return Number of requests still pending.
 */
RIAK_API size_t riak_async_run(riak_async *a, long timeout_ms);

/** Event mask of ::riak_socket_fn and riak_async_on_socket(). */
enum {
    RIAK_POLL_IN = 1,    /**< The descriptor is watched for reading. */
    RIAK_POLL_OUT = 2,   /**< The descriptor is watched for writing. */
    RIAK_POLL_REMOVE = 4 /**< The descriptor is no longer watched. */
};

/** Called when the events watched on @p fd change (RIAK_POLL_* mask). */
typedef void (*riak_socket_fn)(int fd, int events, void *ctx);
/** Called when riak_async_on_timeout() is due in @p timeout_ms; -1 cancels the timer. */
typedef void (*riak_timer_fn)(long timeout_ms, void *ctx);

/**
 * Connects the handle to an external event loop (libuv, libevent, epoll).
 * Set before the first request; riak_async_run() is not used afterwards.
 * @param a Async handle.
 * @param socket_fn Told which events to watch on each descriptor.
 * @param timer_fn Told when riak_async_on_timeout() is due.
 * @param ctx Passed to @p socket_fn and @p timer_fn.
 */
RIAK_API riak_status riak_async_set_loop(riak_async *a, riak_socket_fn socket_fn,
                                         riak_timer_fn timer_fn, void *ctx);
/**
 * Reports that @p fd is ready (RIAK_POLL_IN and/or RIAK_POLL_OUT).
 * @param a Async handle.
 * @param fd Descriptor that became ready.
 * @param events RIAK_POLL_IN and/or RIAK_POLL_OUT.
 */
RIAK_API void riak_async_on_socket(riak_async *a, int fd, int events);
/**
 * Reports that the timer requested through ::riak_timer_fn has expired.
 * @param a Async handle.
 */
RIAK_API void riak_async_on_timeout(riak_async *a);

/** @} */

/** @defgroup memory Releasing results
 *  @{ */

/**
 * Frees the members of a ::riak_object and clears it; NULL is accepted.
 * @param obj Object to clear; NULL is accepted.
 */
RIAK_API void riak_object_free(riak_object *obj);
/**
 * Frees the members of a ::riak_object_set and clears it; NULL is accepted.
 * @param set Set to clear; NULL is accepted.
 */
RIAK_API void riak_object_set_free(riak_object_set *set);
/**
 * Frees the members of a ::riak_strlist and clears it; NULL is accepted.
 * @param list List to clear; NULL is accepted.
 */
RIAK_API void riak_strlist_free(riak_strlist *list);
/**
 * Frees a string returned by the library (JSON documents, generated keys).
 * @param ptr Pointer returned by the library; NULL is accepted.
 */
RIAK_API void riak_free(void *ptr);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* RIAK_RIAK_H */
