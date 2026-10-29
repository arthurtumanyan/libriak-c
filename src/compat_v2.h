/*
 File:   compat_v2.h
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later

 The public structures of libriak-c 2.0.0 whose layout has changed since,
 frozen as 2.0.0 declared them (git show 2d07d75:include/riak/riak.h), and
 the 2.0 entry points of the functions that take them. Programs built
 against 2.0.0 call these under the symbol version LIBRIAK_2.0 (see
 cmake/libriak-c.map); each converts the structure and calls the current
 function. Never edit the structures: they describe compiled programs.
 */

#ifndef RIAK_COMPAT_V2_H
#define RIAK_COMPAT_V2_H

#include "riak/riak.h"

typedef struct {
    const char *host;
    unsigned port;
} riak_node_v2;

typedef struct {
    const char *host;
    unsigned port;
    bool tls;
    bool tls_verify;
    const char *ca_file;
    long connect_timeout_ms;
    long timeout_ms;
    const char *client_id;
    size_t max_body_bytes;
    const riak_node_v2 *nodes;
    size_t n_nodes;
    riak_balance balance;
    unsigned max_retries;
    long retry_base_ms;
    long retry_max_ms;
    unsigned fail_threshold;
    long eject_ms;
    const char *username;
    const char *password;
    const char *tls_cert_file;
    const char *tls_key_file;
    const char *tls_key_password;
    riak_log_fn log_fn;
    riak_log_level log_level;
    void *log_ctx;
    riak_metrics_fn metrics_fn;
    void *metrics_ctx;
} riak_config_v2;

typedef struct {
    int r;
    int pr;
    int w;
    int dw;
    int pw;
    int rw;
    const char *content_type;
    const char *vclock;
    const riak_kv_pair *meta;
    size_t n_meta;
    const riak_kv_pair *indexes;
    size_t n_indexes;
    const char *const *links;
    size_t n_links;
    bool return_body;
    const char *bucket_type;
    const char *if_none_match;
    const char *if_match;
    const char *if_modified_since;
    const char *if_unmodified_since;
    unsigned server_timeout_ms;
    riak_resolver_fn resolver;
    void *resolver_ctx;
} riak_opts_v2;

typedef struct {
    const char *index;
    const char *value;
    const char *range_min;
    const char *range_max;
    unsigned max_results;
    const char *continuation;
    bool return_terms;
    const char *term_regex;
    const char *bucket_type;
    unsigned server_timeout_ms;
} riak_index_query_v2;

typedef struct {
    int r;
    int pr;
    int w;
    int dw;
    int pw;
    bool return_body;
    bool no_context;
    unsigned server_timeout_ms;
} riak_dt_opts_v2;

/* The 2.0 entry points; the names are not exported themselves (the version
 * script keeps them local), only their LIBRIAK_2.0 aliases (riak_get@LIBRIAK_2.0
 * and so on), which need default visibility. */
#define RIAK_COMPAT __attribute__((visibility("default")))
RIAK_COMPAT void compat_v20_config_init(riak_config_v2 *cfg);
RIAK_COMPAT riak_status compat_v20_client_new(const riak_config_v2 *cfg, riak_client **out);
RIAK_COMPAT riak_status compat_v20_async_new(const riak_config_v2 *cfg, riak_async **out);
RIAK_COMPAT riak_status compat_v20_get(riak_client *client, const char *bucket, const char *key,
                                       const riak_opts_v2 *opts, riak_object *out);
RIAK_COMPAT riak_status compat_v20_get_siblings(riak_client *client, const char *bucket,
                                                const char *key, const riak_opts_v2 *opts,
                                                riak_object_set *out);
RIAK_COMPAT riak_status compat_v20_put(riak_client *client, const char *bucket, const char *key,
                                       const void *value, size_t len, const riak_opts_v2 *opts,
                                       riak_object *out, char **created_key);
RIAK_COMPAT riak_status compat_v20_delete(riak_client *client, const char *bucket, const char *key,
                                          const riak_opts_v2 *opts);
RIAK_COMPAT riak_status compat_v20_async_get(riak_async *a, const char *bucket, const char *key,
                                             const riak_opts_v2 *opts, riak_async_cb cb, void *ctx);
RIAK_COMPAT riak_status compat_v20_async_put(riak_async *a, const char *bucket, const char *key,
                                             const void *value, size_t len,
                                             const riak_opts_v2 *opts, riak_async_cb cb, void *ctx);
RIAK_COMPAT riak_status compat_v20_async_delete(riak_async *a, const char *bucket, const char *key,
                                                const riak_opts_v2 *opts, riak_async_cb cb,
                                                void *ctx);
RIAK_COMPAT riak_status compat_v20_index_query_run(riak_client *client, const char *bucket,
                                                   const riak_index_query_v2 *q,
                                                   riak_index_result *out);
RIAK_COMPAT riak_status compat_v20_index_query_stream(riak_client *client, const char *bucket,
                                                      const riak_index_query_v2 *q,
                                                      riak_index_cb cb, void *ctx,
                                                      char **continuation);
RIAK_COMPAT riak_status compat_v20_dt_fetch(riak_client *client, const char *type,
                                            const char *bucket, const char *key,
                                            const riak_dt_opts_v2 *opts, riak_datatype *out);
RIAK_COMPAT riak_status compat_v20_dt_update(riak_client *client, const char *type,
                                             const char *bucket, const char *key,
                                             const riak_dt_op *op, const char *context,
                                             const riak_dt_opts_v2 *opts, riak_datatype *out,
                                             char **created_key);

#endif /* RIAK_COMPAT_V2_H */
