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
    RIAK_E_PARSE         /* malformed response body */
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
    int r, pr, w, dw, pw, rw; /* RIAK_Q_* or a replica count */
    const char *content_type; /* put only; default application/octet-stream */
    const char *vclock;       /* X-Riak-Vclock from a previous get */
    const riak_kv_pair *meta; /* X-Riak-Meta-<name>: <value> */
    size_t n_meta;
    const riak_kv_pair *indexes; /* X-Riak-Index-<name>: <value> */
    size_t n_indexes;
    const char *const *links; /* raw Link header values */
    size_t n_links;
    bool return_body; /* put only */
} riak_opts;

typedef struct {
    char *data; /* NUL-terminated for convenience; len excludes it */
    size_t len;
    char *content_type;
    char *vclock;
    char *etag;
    char *last_modified;
} riak_object;

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

RIAK_API riak_status riak_get(riak_client *client, const char *bucket, const char *key,
                              const riak_opts *opts, riak_object *out);
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

RIAK_API void riak_object_free(riak_object *obj);
RIAK_API void riak_strlist_free(riak_strlist *list);
RIAK_API void riak_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* RIAK_RIAK_H */
