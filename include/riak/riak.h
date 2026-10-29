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
 * concurrently on a single thread. A callback of a streaming call (names,
 * keys, objects, MapReduce results) does not use the client running it:
 * such a call returns ::RIAK_E_INVALID_ARG, and riak_client_free() called
 * from one is ignored (and logged) instead of freeing the client in use.
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
    RIAK_E_CANCELLED,    /**< An async request was dropped by riak_async_free(). */
    RIAK_E_UNSUPPORTED,  /**< The operation or option is not available on the transport or node. */
    RIAK_E_SERVER        /**< Protocol Buffers: the server returned an error message. */
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

/** Wire protocol used to talk to Riak, see riak_config::transport. */
typedef enum {
    RIAK_TRANSPORT_PB = 0, /**< Protocol Buffers (default port 8087). */
    RIAK_TRANSPORT_HTTP    /**< HTTP (default port 8098). */
} riak_transport;

/** Measurements of one attempt, reported through riak_config::metrics_fn. */
typedef struct {
    const char *method;       /**< HTTP method, or the request message name ("RpbGetReq"). */
    const char *path;         /**< Request path and query, without scheme and host; for
                                   Protocol Buffers the equivalent HTTP resource path. */
    const char *host;         /**< Node the attempt was sent to. */
    unsigned port;            /**< Port of that node. */
    unsigned attempt;         /**< 0 for the first try, 1 for the first retry, and so on. */
    riak_status status;       /**< ::RIAK_OK when a response arrived, whatever its HTTP code. */
    long http_status;         /**< HTTP status, 0 when no response was received. */
    double duration_ms;       /**< Time from sending to completion. */
    size_t bytes_received;    /**< Size of the response body. */
    bool will_retry;          /**< Whether another attempt follows. */
    riak_transport transport; /**< Transport of the attempt. */
} riak_request_metrics;

/**
 * Metrics callback, called once for every attempt.
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
    const char *host;   /**< Host name or IP address (IPv6 literals are accepted). */
    unsigned port;      /**< Port; 0 selects 8087 (Protocol Buffers) or 8098 (HTTP). */
    unsigned http_port; /**< Protocol Buffers: HTTP port used by riak_stats(); 0: none. */
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
    riak_transport transport; /**< Wire protocol; default ::RIAK_TRANSPORT_PB. */
    const char *host;         /**< Node address; default "127.0.0.1". Ignored when #nodes is set. */
    /** Node port; 0 (the default) selects 8087 for Protocol Buffers and 8098
     *  for HTTP. Ignored when #nodes is set. */
    unsigned port;
    unsigned http_port;      /**< Protocol Buffers: HTTP port for riak_stats(); 0: none. */
    bool tls;                /**< HTTPS, or STARTTLS on a Protocol Buffers connection. */
    bool tls_verify;         /**< Peer certificate and host name are verified; default true. */
    const char *ca_file;     /**< CA bundle (PEM) used for verification; NULL: system store. */
    long connect_timeout_ms; /**< Connection timeout; 0: libcurl default. */
    long timeout_ms;         /**< Whole-request timeout; 0: no limit. */
    const char *client_id;   /**< Sent as X-Riak-ClientId, or with RpbSetClientIdReq. */
    size_t max_body_bytes;   /**< Response size limit (per message on streams); 0: 64 MiB. */
    /** Async Protocol Buffers requests: connections opened per node; 0 selects 8. */
    unsigned pb_max_connections_per_node;

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

    const char *username; /**< Riak security user; refused unless #tls is set. */
    const char *password; /**< Password of #username; wiped from memory when the client is freed. */
    const char *tls_cert_file;    /**< Client certificate (PEM) for certificate authentication. */
    const char *tls_key_file;     /**< Private key (PEM) of #tls_cert_file. */
    const char *tls_key_password; /**< Passphrase of an encrypted #tls_key_file. */
    /**
     * Interoperability with nodes on an old Erlang/OTP TLS stack (Riak KV 2.2
     * and older), which reject OpenSSL 3 clients: TLS 1.2 at most, no RSA-PSS
     * or EdDSA signature algorithms, legacy renegotiation accepted and no DHE
     * key exchange (such nodes then use RSA key exchange, without forward
     * secrecy). Off by default; meant only for such nodes.
     */
    bool tls_legacy;

    riak_log_fn log_fn;         /**< Log callback, run on the thread issuing the request. */
    riak_log_level log_level;   /**< Minimum level passed to #log_fn; default ::RIAK_LOG_WARN. */
    void *log_ctx;              /**< Passed to #log_fn. */
    riak_metrics_fn metrics_fn; /**< Metrics callback, called after every attempt. */
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

/** Disk synchronization of a write, see riak_opts::sync_on_write. */
typedef enum {
    RIAK_SYNC_DEFAULT = 0, /**< The bucket's setting. */
    RIAK_SYNC_BACKEND,     /**< The backend's own policy. */
    RIAK_SYNC_ONE,         /**< The coordinating vnode flushes before acknowledging. */
    RIAK_SYNC_ALL          /**< Every vnode flushes before acknowledging. */
} riak_sync;

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
    bool head;          /**< Get: only metadata is returned; riak_object::data is empty. */
    bool basic_quorum;  /**< Get: a not-found answer is returned once a quorum agrees. */
    bool notfound_ok;   /**< Get: a not-found answer counts towards the read quorum. */
    bool sloppy_quorum; /**< Protocol Buffers: fallback vnodes count towards quorums. */
    unsigned n_val;     /**< Protocol Buffers: replica count for this request; 0: bucket's. */
    /** Protocol Buffers get: ::RIAK_E_NOT_MODIFIED is returned when the stored
     *  object still has the vector clock #vclock. */
    bool if_modified;
    /** Protocol Buffers put: ::RIAK_E_PRECONDITION is returned unless the
     *  stored object still has the vector clock #vclock. */
    bool if_not_modified;
    /** Distinct physical nodes that must acknowledge a write, or serve a read
     *  (Riak 2.9+); takes the same values as #w. */
    int node_confirms;
    riak_sync sync_on_write; /**< Put: flush to disk before acknowledging (Riak 3.0+). */
    bool return_head;        /**< Protocol Buffers put: the stored metadata, without the value. */
    /** Protocol Buffers get: for a deleted key the result is still
     *  ::RIAK_E_NOT_FOUND, but riak_object::vclock holds the tombstone's
     *  vector clock and riak_object::deleted is set. */
    bool deletedvclock;
    /** Put: the object is stored as given, without a new vector clock entry
     *  (used by replication). */
    bool asis;
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
 * Frees a client; NULL is accepted. Stored secrets are wiped. A call from a
 * callback of a request on this client is ignored, since the client is in use.
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
 * Node statistics (/stats) as a JSON object. Statistics are served only over
 * HTTP: with the Protocol Buffers transport the request goes to the first
 * node with an HTTP port (riak_config::http_port, riak_node::http_port).
 * @param client Client handle.
 * @param json Receives the document; released with riak_free().
 * @return ::RIAK_OK, or ::RIAK_E_UNSUPPORTED over Protocol Buffers without an HTTP port.
 */
RIAK_API riak_status riak_stats(riak_client *client, char **json);

/** Node name and version reported by riak_server_info(); released with riak_server_info_free(). */
typedef struct {
    char *node;    /**< Erlang node name, e.g. "riak@127.0.0.1". */
    char *version; /**< Riak version. */
} riak_server_info;

/**
 * Name and version of the node that answers.
 * @param client Client handle.
 * @param out Receives the information; released with riak_server_info_free().
 */
RIAK_API riak_status riak_get_server_info(riak_client *client, riak_server_info *out);

/**
 * Frees a ::riak_server_info; NULL is accepted.
 * @param info Information to clear.
 */
RIAK_API void riak_server_info_free(riak_server_info *info);

/** One vnode of a preference list. */
typedef struct {
    long long partition; /**< Partition index. */
    char *node;          /**< Node that owns the vnode. */
    bool primary;        /**< The vnode is a primary, not a fallback. */
} riak_preflist_item;

/** Preference list of a key; released with riak_preflist_free(). */
typedef struct {
    riak_preflist_item *items; /**< The vnodes. */
    size_t count;              /**< Number of entries in #items. */
} riak_preflist;

/**
 * Preference list (the vnodes that hold the replicas) of a key.
 * @param client Client handle.
 * @param type Bucket type; NULL selects "default".
 * @param bucket Bucket name.
 * @param key Key.
 * @param out Receives the list; released with riak_preflist_free().
 */
RIAK_API riak_status riak_get_preflist(riak_client *client, const char *type, const char *bucket,
                                       const char *key, riak_preflist *out);

/**
 * Frees a ::riak_preflist; NULL is accepted.
 * @param list List to clear.
 */
RIAK_API void riak_preflist_free(riak_preflist *list);

/**
 * Callback of riak_list_keys_stream() and riak_list_buckets_stream().
 * @param name A key or bucket name; valid only during the call.
 * @param len Length of @p name.
 * @param ctx Context passed to the listing call.
 * @return true to continue, false to stop the listing.
 */
typedef bool (*riak_name_cb)(const char *name, size_t len, void *ctx);

/**
 * Lists the keys of a bucket, passing each to @p cb as it arrives (expensive:
 * every key in the cluster is visited).
 * @param client Client handle.
 * @param type Bucket type; NULL selects "default".
 * @param bucket Bucket name.
 * @param cb Called with each key.
 * @param ctx Passed unchanged to the callback.
 */
RIAK_API riak_status riak_list_keys_stream(riak_client *client, const char *type,
                                           const char *bucket, riak_name_cb cb, void *ctx);

/**
 * Lists the buckets of a bucket type, passing each to @p cb as it arrives.
 * @param client Client handle.
 * @param type Bucket type; NULL selects "default".
 * @param cb Called with each bucket name.
 * @param ctx Passed unchanged to the callback.
 */
RIAK_API riak_status riak_list_buckets_stream(riak_client *client, const char *type,
                                              riak_name_cb cb, void *ctx);

/** One entry of a coverage plan. */
typedef struct {
    char *host;          /**< Address of the node to query. */
    unsigned port;       /**< Protocol Buffers port of that node. */
    char *description;   /**< Human-readable key space description. */
    char *cover_context; /**< Opaque context (base64) for riak_index_query::cover_context. */
} riak_coverage_entry;

/** Coverage plan of a bucket; released with riak_coverage_free(). */
typedef struct {
    riak_coverage_entry *entries; /**< The entries. */
    size_t count;                 /**< Number of entries. */
} riak_coverage_plan;

/**
 * Coverage plan for parallel secondary index queries (Protocol Buffers only).
 * Each entry is queried with riak_index_query::cover_context set.
 * @param client Client handle.
 * @param type Bucket type; NULL selects "default".
 * @param bucket Bucket name.
 * @param min_partitions Minimum number of entries; 0 selects the server default.
 * @param out Receives the plan; released with riak_coverage_free().
 * @return ::RIAK_OK, or ::RIAK_E_UNSUPPORTED over HTTP or when the node does not
 *         implement coverage requests.
 */
RIAK_API riak_status riak_get_coverage(riak_client *client, const char *type, const char *bucket,
                                       unsigned min_partitions, riak_coverage_plan *out);

/**
 * Frees a ::riak_coverage_plan; NULL is accepted.
 * @param plan Plan to clear.
 */
RIAK_API void riak_coverage_free(riak_coverage_plan *plan);

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
    /** Protocol Buffers: limits the query to one entry of riak_coverage(). */
    const char *cover_context;
    /** Results of a query without #max_results are sorted (paged queries
     *  always are). */
    bool pagination_sort;
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
    int node_confirms;          /**< Distinct physical nodes that must acknowledge (Riak 2.9+). */
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

/** @defgroup extended Counters, folds, Query API, link walking, replication
 *  Calls that a node may not offer: an operation the transport or the
 *  node's Riak version does not support returns ::RIAK_E_UNSUPPORTED.
 *  @{ */

/**
 * Sets properties of a bucket type, as a JSON document `{"props":{...}}`.
 * @param client Client handle.
 * @param type Bucket type name.
 * @param json Properties to change.
 */
RIAK_API riak_status riak_set_type_props(riak_client *client, const char *type, const char *json);

/**
 * Client id the node associates with this connection (Protocol Buffers;
 * deprecated by Riak since 1.4).
 * @param client Client handle.
 * @param id Receives the id; released with riak_free().
 */
RIAK_API riak_status riak_get_client_id(riak_client *client, char **id);

/**
 * Adds @p amount to a Riak 1.4 counter, a counter kept in an `allow_mult`
 * bucket without a bucket type (data type counters are preferred).
 * @param client Client handle.
 * @param bucket Bucket name.
 * @param key Key.
 * @param amount Amount added; negative values decrement.
 * @param opts Quorums (w, dw, pw, node_confirms); NULL selects the defaults.
 * @param value Receives the new value; NULL when it is not needed.
 */
RIAK_API riak_status riak_counter_update(riak_client *client, const char *bucket, const char *key,
                                         int64_t amount, const riak_opts *opts, int64_t *value);

/**
 * Value of a Riak 1.4 counter.
 * @param client Client handle.
 * @param bucket Bucket name.
 * @param key Key.
 * @param opts Quorums (r, pr, basic_quorum, notfound_ok, node_confirms); NULL selects the defaults.
 * @param value Receives the value.
 * @return ::RIAK_OK, ::RIAK_E_NOT_FOUND for an absent counter, or another error.
 */
RIAK_API riak_status riak_counter_get(riak_client *client, const char *bucket, const char *key,
                                      const riak_opts *opts, int64_t *value);

/** A key range of riak_fold_objects(). */
typedef struct {
    const char *start_key;      /**< First key of the range; required. */
    const char *end_key;        /**< Last key; NULL: to the end of the bucket. */
    bool start_exclusive;       /**< #start_key itself is excluded. */
    bool end_inclusive;         /**< #end_key itself is included. */
    unsigned max_results;       /**< Page size; 0 returns all objects. */
    const char *continuation;   /**< Continuation of the previous page. */
    const char *bucket_type;    /**< Bucket type; NULL selects "default". */
    unsigned server_timeout_ms; /**< Riak-side timeout; 0 selects the server default. */
    const char *cover_context;  /**< Limits the fold to one coverage plan entry. */
} riak_fold_query;

/**
 * Callback of riak_fold_objects().
 * @param key Key of the object.
 * @param obj The object; it may be kept by copying the structure and
 *            clearing it, otherwise it is freed when the callback returns.
 * @param ctx Context passed to riak_fold_objects().
 * @return true to continue, false to stop.
 */
typedef bool (*riak_object_cb)(const char *key, riak_object *obj, void *ctx);

/**
 * Folds over the objects of a key range, values included (Protocol Buffers;
 * the "CS bucket" fold of Riak).
 * @param client Client handle.
 * @param bucket Bucket name.
 * @param q The range.
 * @param cb Called with each object, in key order.
 * @param ctx Passed unchanged to the callback.
 * @param continuation Receives the continuation of a paged fold (released
 *        with riak_free()); may be NULL.
 */
RIAK_API riak_status riak_fold_objects(riak_client *client, const char *bucket,
                                       const riak_fold_query *q, riak_object_cb cb, void *ctx,
                                       char **continuation);

/** One step of riak_link_walk(). */
typedef struct {
    const char *bucket; /**< Follow only links to this bucket; NULL: any. */
    const char *tag;    /**< Follow only links with this riaktag; NULL: any. */
    bool keep;          /**< The objects reached by this step are returned. */
} riak_link_step;

/** An object reached by a link walk. */
typedef struct {
    char *bucket;       /**< Bucket of the object. */
    char *key;          /**< Key of the object. */
    riak_object object; /**< The object. */
} riak_linked_object;

/** Objects of one kept step. */
typedef struct {
    riak_linked_object *items; /**< The objects. */
    size_t count;              /**< Number of objects. */
} riak_link_phase;

/** Result of riak_link_walk(); released with riak_link_result_free(). */
typedef struct {
    riak_link_phase *phases; /**< One entry per step with riak_link_step::keep set. */
    size_t n_phases;         /**< Number of entries. */
} riak_link_result;

/**
 * Follows links from an object. The last step is always kept. Over HTTP the
 * link walker of the node is used; over Protocol Buffers the walk runs as a
 * MapReduce job with link phases and the kept objects are fetched.
 * @param client Client handle.
 * @param bucket Bucket of the starting object.
 * @param key Key of the starting object.
 * @param steps The steps; at least one.
 * @param n_steps Number of steps.
 * @param out Receives the objects; released with riak_link_result_free().
 * @return ::RIAK_OK, or ::RIAK_E_UNSUPPORTED on a node with Riak security
 *         enabled (Riak refuses link walking there), or another error.
 */
RIAK_API riak_status riak_link_walk(riak_client *client, const char *bucket, const char *key,
                                    const riak_link_step *steps, size_t n_steps,
                                    riak_link_result *out);

/**
 * Frees a ::riak_link_result; NULL is accepted.
 * @param res Result to clear.
 */
RIAK_API void riak_link_result_free(riak_link_result *res);

/** One index query of a ::riak_query (Riak 3.4+). */
typedef struct {
    int aggregation_tag;    /**< Referenced as `$tag` by riak_query::aggregation_expression. */
    const char *index_name; /**< Binary index, e.g. "people_bin". */
    const char *start_term; /**< Start of the term range. */
    const char *end_term;   /**< End of the term range. */
    const char *evaluation_expression; /**< Projects attributes from the term. */
    const char *filter_expression;     /**< Filters on the projected attributes. */
    const char *regular_expression;    /**< Filters terms; excludes the expressions above. */
} riak_query_part;

/** A Query API request (Riak 3.4+), see the Riak documentation for the
 *  expression syntax. */
typedef struct {
    const riak_query_part *parts;       /**< The index queries; at least one. */
    size_t n_parts;                     /**< Number of entries in #parts. */
    const char *aggregation_expression; /**< e.g. "$1 INTERSECT $2"; NULL for one query. */
    const char *accumulation_option;    /**< "keys" (default), "raw_keys", "terms", "count", ... */
    const char *accumulation_term;      /**< Attribute accumulated by the term options. */
    const riak_kv_pair *substitutions;  /**< Values substituted for `:name` in expressions. */
    size_t n_substitutions;             /**< Number of entries in #substitutions. */
    unsigned max_results;               /**< Page size ("terms" and "raw_keys" only). */
    const char *continuation;           /**< Continuation of the previous page. */
    unsigned timeout_s;                 /**< Riak-side timeout in seconds; 0: default. */
    unsigned inactivity_timeout_s;      /**< Queue options: queue lifetime without fetches. */
    const char *bucket_type;            /**< Bucket type; NULL selects "default". */
} riak_query;

/** Result of a Query API request; released with riak_query_result_free(). */
typedef struct {
    char *raw; /**< The complete JSON response. */
    char *
        *keys; /**< Keys of "keys" and "raw_keys" results, and of term results; NULL-terminated. */
    char **terms;       /**< Terms parallel to #keys for term results, otherwise NULL. */
    size_t count;       /**< Entries in #keys. */
    long long total;    /**< Count of "count" and "raw_count" results; -1 otherwise. */
    char *continuation; /**< Continuation of the next page; NULL on the last one. */
    char *result_queue; /**< Queue reference of "queue_*" options; NULL otherwise. */
    bool complete;      /**< riak_query_fetch(): the query has finished. */
} riak_query_result;

/**
 * Runs a Query API request (Riak 3.4+). The Query API is served over HTTP
 * only; with the Protocol Buffers transport it goes to the HTTP port
 * (riak_config::http_port), as riak_stats() does.
 * @param client Client handle.
 * @param bucket Bucket name.
 * @param q The request.
 * @param out Receives the result; released with riak_query_result_free().
 */
RIAK_API riak_status riak_query_run(riak_client *client, const char *bucket, const riak_query *q,
                                    riak_query_result *out);

/**
 * Fetches the next batch of a queued query (accumulation option
 * "queue_raw_keys" or "queue_raw_terms", Riak 3.4.1+).
 * @param client Client handle.
 * @param bucket_type Bucket type of the query; NULL selects "default".
 * @param bucket Bucket of the query.
 * @param result_queue riak_query_result::result_queue of the query.
 * @param max_results Batch size; 0 selects the server default.
 * @param out Receives the batch; riak_query_result::complete is set once
 *            the query has finished.
 */
RIAK_API riak_status riak_query_fetch(riak_client *client, const char *bucket_type,
                                      const char *bucket, const char *result_queue,
                                      unsigned max_results, riak_query_result *out);

/**
 * Frees a ::riak_query_result; NULL is accepted.
 * @param res Result to clear.
 */
RIAK_API void riak_query_result_free(riak_query_result *res);

/** Size of an AAE tree. */
typedef enum {
    RIAK_AAE_XXSMALL = 0, /**< Extra extra small (16 branches). */
    RIAK_AAE_XSMALL,      /**< Extra small. */
    RIAK_AAE_SMALL,       /**< Small. */
    RIAK_AAE_MEDIUM,      /**< Medium. */
    RIAK_AAE_LARGE,       /**< Large (the cached n_val trees). */
    RIAK_AAE_XLARGE       /**< Extra large. */
} riak_aae_tree_size;

/** Selection of an AAE range fold (Riak 2.9+, tictac AAE active). */
typedef struct {
    const char *bucket_type;  /**< Bucket type; NULL selects "default". */
    const char *bucket;       /**< Bucket; required. */
    const char *start_key;    /**< Key range start; NULL with #end_key NULL: every key. */
    const char *end_key;      /**< Key range end (inclusive). */
    const uint32_t *segments; /**< Segment filter; NULL: every segment. */
    size_t n_segments;        /**< Number of entries in #segments. */
    riak_aae_tree_size segment_tree_size; /**< Tree size the segment ids refer to. */
    uint32_t modified_start; /**< Last-modified range start (Unix seconds); with #modified_end. */
    uint32_t modified_end;   /**< Last-modified range end; 0 (with start 0): no range. */
    /** riak_aae_merge_range(): clocks are hashed again with #hash_iv as
     *  initialisation vector, instead of using the stored hashes (avoids
     *  collisions when comparing clusters). */
    bool use_hash_iv;
    uint32_t hash_iv; /**< Initialisation vector for #use_hash_iv. */
} riak_aae_filter;

/** One level-two segment block of an AAE tree, uncompressed. */
typedef struct {
    uint32_t id;   /**< Branch id. */
    uint8_t *data; /**< Segment hashes. */
    size_t len;    /**< Length of #data. */
} riak_aae_branch;

/** An AAE tree or part of one; released with riak_aae_tree_free(). */
typedef struct {
    riak_aae_tree_size size;   /**< Size of the tree. */
    uint8_t *level1;           /**< Level-one hashes; NULL for branch results. */
    size_t level1_len;         /**< Length of #level1. */
    riak_aae_branch *branches; /**< Level-two blocks. */
    size_t n_branches;         /**< Number of entries in #branches. */
} riak_aae_tree;

/** A key with its vector clock, as returned by clock folds. */
typedef struct {
    char *bucket_type; /**< Bucket type; NULL for the default type. */
    char *bucket;      /**< Bucket. */
    char *key;         /**< Key. */
    char *vclock;      /**< Vector clock, base64 as in riak_object::vclock. */
} riak_aae_key_clock;

/** Keys and clocks; released with riak_aae_keys_free(). */
typedef struct {
    riak_aae_key_clock *items; /**< The keys. */
    size_t count;              /**< Number of keys. */
} riak_aae_keys;

/** A key and the number reported for it by riak_aae_find_keys(). */
typedef struct {
    char *key;      /**< Key. */
    uint64_t value; /**< Sibling count or object size. */
} riak_aae_key_count;

/** Result of riak_aae_find_keys(); released with riak_aae_key_counts_free(). */
typedef struct {
    riak_aae_key_count *items; /**< The keys. */
    size_t count;              /**< Number of keys. */
} riak_aae_key_counts;

/** One bucket of a histogram in ::riak_aae_stats. */
typedef struct {
    uint32_t order; /**< Order of magnitude (sizes) or sibling count. */
    uint64_t count; /**< Objects in this bucket. */
} riak_aae_histogram;

/** Result of riak_aae_object_stats(); released with riak_aae_stats_free(). */
typedef struct {
    uint64_t total_count;         /**< Objects in the range. */
    uint64_t total_size;          /**< Their total size in bytes. */
    riak_aae_histogram *sizes;    /**< Objects per order of magnitude of size. */
    size_t n_sizes;               /**< Entries in #sizes. */
    riak_aae_histogram *siblings; /**< Objects per sibling count. */
    size_t n_siblings;            /**< Entries in #siblings. */
} riak_aae_stats;

/** What riak_aae_find_keys() reports. */
typedef enum {
    RIAK_AAE_SIBLING_COUNT = 0, /**< Keys with more siblings than the limit. */
    RIAK_AAE_OBJECT_SIZE        /**< Keys larger than the limit (bytes). */
} riak_aae_find;

/** How riak_aae_reap_tombs() and riak_aae_erase_keys() act. */
typedef enum {
    RIAK_AAE_COUNT = 0, /**< Nothing is changed; the matching keys are counted. */
    RIAK_AAE_LOCAL,     /**< Each node's own reaper/eraser queue does the work. */
    RIAK_AAE_JOB        /**< A dedicated worker on the coordinating node (see job id). */
} riak_aae_change;

/** Bucket listed by riak_aae_list_buckets(). */
typedef struct {
    char *bucket_type; /**< Bucket type; NULL for the default type. */
    char *bucket;      /**< Bucket. */
} riak_aae_bucket;

/** Result of riak_aae_list_buckets(); released with riak_aae_buckets_free(). */
typedef struct {
    riak_aae_bucket *items; /**< The buckets. */
    size_t count;           /**< Number of buckets. */
} riak_aae_buckets;

/**
 * Root (level one) of the cached AAE tree of an n_val (Riak 2.9+).
 * @param client Client handle.
 * @param n_val The n_val.
 * @param out Receives the tree; released with riak_aae_tree_free().
 */
RIAK_API riak_status riak_aae_merge_root(riak_client *client, unsigned n_val, riak_aae_tree *out);
/**
 * Branches (level two) of the cached AAE tree of an n_val.
 * @param client Client handle.
 * @param n_val The n_val.
 * @param ids Branch ids.
 * @param n_ids Number of ids.
 * @param out Receives the branches; released with riak_aae_tree_free().
 */
RIAK_API riak_status riak_aae_merge_branches(riak_client *client, unsigned n_val,
                                             const uint32_t *ids, size_t n_ids, riak_aae_tree *out);
/**
 * Keys and clocks in segments of the cached AAE tree of an n_val.
 * @param client Client handle.
 * @param n_val The n_val.
 * @param segments Segment ids.
 * @param n_segments Number of ids.
 * @param out Receives the keys; released with riak_aae_keys_free().
 */
RIAK_API riak_status riak_aae_fetch_clocks_nval(riak_client *client, unsigned n_val,
                                                const uint32_t *segments, size_t n_segments,
                                                riak_aae_keys *out);
/**
 * AAE tree of a range, built on demand.
 * @param client Client handle.
 * @param f The range.
 * @param size Size of the tree to build.
 * @param out Receives the tree; released with riak_aae_tree_free().
 */
RIAK_API riak_status riak_aae_merge_range(riak_client *client, const riak_aae_filter *f,
                                          riak_aae_tree_size size, riak_aae_tree *out);
/**
 * Keys and clocks of a range.
 * @param client Client handle.
 * @param f The range.
 * @param out Receives the keys; released with riak_aae_keys_free().
 */
RIAK_API riak_status riak_aae_fetch_clocks_range(riak_client *client, const riak_aae_filter *f,
                                                 riak_aae_keys *out);
/**
 * Keys of a range with more siblings, or larger, than @p limit.
 * @param client Client handle.
 * @param f The range.
 * @param what Siblings or size.
 * @param limit The threshold.
 * @param out Receives the keys; released with riak_aae_key_counts_free().
 */
RIAK_API riak_status riak_aae_find_keys(riak_client *client, const riak_aae_filter *f,
                                        riak_aae_find what, uint32_t limit,
                                        riak_aae_key_counts *out);
/**
 * Object count, total size and size and sibling histograms of a range.
 * @param client Client handle.
 * @param f The range.
 * @param out Receives the statistics; released with riak_aae_stats_free().
 */
RIAK_API riak_status riak_aae_object_stats(riak_client *client, const riak_aae_filter *f,
                                           riak_aae_stats *out);
/**
 * Tombstones of a range, with their clocks.
 * @param client Client handle.
 * @param f The range.
 * @param out Receives the keys; released with riak_aae_keys_free().
 */
RIAK_API riak_status riak_aae_find_tombs(riak_client *client, const riak_aae_filter *f,
                                         riak_aae_keys *out);
/**
 * Reaps the tombstones of a range.
 * @param client Client handle.
 * @param f The range.
 * @param how Count only, local queues, or a dedicated job.
 * @param job_id Job id for ::RIAK_AAE_JOB.
 * @param count Receives the number of tombstones counted or dispatched.
 */
RIAK_API riak_status riak_aae_reap_tombs(riak_client *client, const riak_aae_filter *f,
                                         riak_aae_change how, uint32_t job_id, uint64_t *count);
/**
 * Deletes the keys of a range.
 * @param client Client handle.
 * @param f The range.
 * @param how Count only, local queues, or a dedicated job.
 * @param job_id Job id for ::RIAK_AAE_JOB.
 * @param count Receives the number of keys counted or dispatched.
 */
RIAK_API riak_status riak_aae_erase_keys(riak_client *client, const riak_aae_filter *f,
                                         riak_aae_change how, uint32_t job_id, uint64_t *count);
/**
 * Prompts read repair of the keys of a range.
 * @param client Client handle.
 * @param f The range.
 * @param count Receives the number of keys dispatched.
 */
RIAK_API riak_status riak_aae_repair_keys(riak_client *client, const riak_aae_filter *f,
                                          uint64_t *count);
/**
 * Queues the keys of a range for replication on a nextgenrepl queue.
 * @param client Client handle.
 * @param f The range.
 * @param queue Queue name.
 * @param count Receives the number of keys queued.
 */
RIAK_API riak_status riak_aae_repl_keys(riak_client *client, const riak_aae_filter *f,
                                        const char *queue, uint64_t *count);
/**
 * Buckets with objects of an n_val, found through AAE (cheaper than
 * riak_list_buckets()).
 * @param client Client handle.
 * @param n_val The n_val.
 * @param out Receives the buckets; released with riak_aae_buckets_free().
 */
RIAK_API riak_status riak_aae_list_buckets(riak_client *client, unsigned n_val,
                                           riak_aae_buckets *out);

/** Frees a ::riak_aae_tree; NULL is accepted. @param t Tree to clear. */
RIAK_API void riak_aae_tree_free(riak_aae_tree *t);
/** Frees a ::riak_aae_keys; NULL is accepted. @param k Keys to clear. */
RIAK_API void riak_aae_keys_free(riak_aae_keys *k);
/** Frees a ::riak_aae_key_counts; NULL is accepted. @param k Keys to clear. */
RIAK_API void riak_aae_key_counts_free(riak_aae_key_counts *k);
/** Frees a ::riak_aae_stats; NULL is accepted. @param s Statistics to clear. */
RIAK_API void riak_aae_stats_free(riak_aae_stats *s);
/** Frees a ::riak_aae_buckets; NULL is accepted. @param b Buckets to clear. */
RIAK_API void riak_aae_buckets_free(riak_aae_buckets *b);

/** Result of riak_repl_fetch(); released with riak_repl_fetch_result_free(). */
typedef struct {
    bool queue_empty;      /**< Nothing was waiting on the queue. */
    bool deleted;          /**< The change is a deletion. */
    uint8_t *object;       /**< The replicated object in Riak's internal encoding. */
    size_t object_len;     /**< Length of #object. */
    char *deleted_vclock;  /**< Vector clock of a deletion (base64); NULL otherwise. */
    bool has_crc;          /**< #crc is set. */
    uint32_t crc;          /**< CRC32 of #object. */
    uint32_t segment_id;   /**< AAE segment of the key. */
    uint32_t segment_hash; /**< Hash of the key within the segment. */
} riak_repl_fetch_result;

/**
 * Takes the next change from a nextgenrepl queue (Riak 2.9+).
 * @param client Client handle.
 * @param queue Queue name.
 * @param out Receives the change; released with riak_repl_fetch_result_free().
 * @return ::RIAK_OK (riak_repl_fetch_result::queue_empty when nothing waits),
 *         ::RIAK_E_NOT_FOUND when the queued key no longer exists (the entry
 *         is taken off the queue; the next call continues), or another error.
 */
RIAK_API riak_status riak_repl_fetch(riak_client *client, const char *queue,
                                     riak_repl_fetch_result *out);
/**
 * Frees a ::riak_repl_fetch_result; NULL is accepted.
 * @param r Result to clear.
 */
RIAK_API void riak_repl_fetch_result_free(riak_repl_fetch_result *r);

/** A key to queue with riak_repl_push(). */
typedef struct {
    const char *bucket_type; /**< Bucket type; NULL for the default type. */
    const char *bucket;      /**< Bucket. */
    const char *key;         /**< Key. */
    const char *vclock;      /**< Vector clock (base64) of the version to replicate. */
} riak_repl_key;

/** Queue lengths reported by riak_repl_push(). */
typedef struct {
    bool queue_exists;        /**< The queue is defined on the node. */
    uint32_t fold_length;     /**< Entries waiting from full-sync folds. */
    uint32_t fsync_length;    /**< Entries waiting from full-sync. */
    uint32_t realtime_length; /**< Entries waiting from real-time changes. */
} riak_repl_push_result;

/**
 * Pushes keys onto a nextgenrepl queue (Protocol Buffers, Riak 2.9+).
 * @param client Client handle.
 * @param queue Queue name.
 * @param keys The keys.
 * @param n_keys Number of keys.
 * @param out Receives the queue lengths; may be NULL.
 */
RIAK_API riak_status riak_repl_push(riak_client *client, const char *queue,
                                    const riak_repl_key *keys, size_t n_keys,
                                    riak_repl_push_result *out);

/** A cluster member reported by riak_cluster_membership(). */
typedef struct {
    char *ip;      /**< Address. */
    unsigned port; /**< Port. */
} riak_member;

/** Result of riak_cluster_membership(); released with riak_members_free(). */
typedef struct {
    riak_member *items; /**< The members that are up. */
    size_t count;       /**< Number of members. */
} riak_members;

/**
 * Nodes of the cluster that are up, as used by nextgenrepl sinks (Riak 3.0+).
 * @param client Client handle.
 * @param out Receives the members; released with riak_members_free().
 */
RIAK_API riak_status riak_cluster_membership(riak_client *client, riak_members *out);
/**
 * Frees a ::riak_members; NULL is accepted.
 * @param m Members to clear.
 */
RIAK_API void riak_members_free(riak_members *m);

/**
 * Id of the cluster for riak_repl (Protocol Buffers).
 * @param client Client handle.
 * @param id Receives the id; released with riak_free().
 */
RIAK_API riak_status riak_get_cluster_id(riak_client *client, char **id);

/**
 * Get proxied by riak_repl for a cluster that receives replication from
 * @p cluster_id (Protocol Buffers; the default bucket type only).
 * @param client Client handle.
 * @param cluster_id Id of the source cluster.
 * @param bucket Bucket name.
 * @param key Key.
 * @param opts r, pr, basic_quorum, notfound_ok, head, deletedvclock,
 *        sloppy_quorum, n_val, if_modified + vclock; NULL selects the defaults.
 * @param out Receives the object.
 */
RIAK_API riak_status riak_repl_get(riak_client *client, const char *cluster_id, const char *bucket,
                                   const char *key, const riak_opts *opts, riak_object *out);

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
