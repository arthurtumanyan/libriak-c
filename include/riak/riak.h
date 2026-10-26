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
    RIAK_E_NOT_MODIFIED  /* 304: conditional get, object unchanged */
} riak_status;

/* Quorum values; positive numbers are passed through as-is. */
enum { RIAK_Q_DEFAULT = 0, RIAK_Q_ONE = -1, RIAK_Q_QUORUM = -2, RIAK_Q_ALL = -3 };

typedef struct riak_client riak_client;

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
} riak_config;

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

RIAK_API void riak_object_free(riak_object *obj);
RIAK_API void riak_object_set_free(riak_object_set *set);
RIAK_API void riak_strlist_free(riak_strlist *list);
RIAK_API void riak_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* RIAK_RIAK_H */
