/*
 File:   compat_v2.c
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later

 Binary compatibility with libriak-c 2.0.0. A program built against 2.0.0
 passes structures in their 2.0 layout and expects the 2.0 defaults (the
 HTTP transport); its unversioned references bind to the LIBRIAK_2.0 symbols
 defined here, which copy the structures into the current ones (new fields
 zero, transport HTTP) and call the current functions. Programs built
 against this library use the LIBRIAK_2.1 definitions. Built into the shared
 library on ELF platforms only.
 */

#include "compat_v2.h"

#include <stdlib.h>
#include <string.h>

/* Bind each 2.0 entry point to name@LIBRIAK_2.0 (non-default version). The
 * .symver directive is the only way to do so that GCC and Clang share. */
/* NOLINTBEGIN(portability-no-assembler) */
#ifdef RIAK_COMPAT_NO_SYMVER /* the unit test calls the entry points directly */
#define RIAK_COMPAT_V20(fn, name) typedef int riak_compat_unused_##fn
#else
#define RIAK_COMPAT_V20(fn, name) __asm__(".symver " #fn ", " #name "@LIBRIAK_2.0")
#endif

RIAK_COMPAT_V20(compat_v20_config_init, riak_config_init);
RIAK_COMPAT_V20(compat_v20_client_new, riak_client_new);
RIAK_COMPAT_V20(compat_v20_async_new, riak_async_new);
RIAK_COMPAT_V20(compat_v20_get, riak_get);
RIAK_COMPAT_V20(compat_v20_get_siblings, riak_get_siblings);
RIAK_COMPAT_V20(compat_v20_put, riak_put);
RIAK_COMPAT_V20(compat_v20_delete, riak_delete);
RIAK_COMPAT_V20(compat_v20_async_get, riak_async_get);
RIAK_COMPAT_V20(compat_v20_async_put, riak_async_put);
RIAK_COMPAT_V20(compat_v20_async_delete, riak_async_delete);
RIAK_COMPAT_V20(compat_v20_index_query_run, riak_index_query_run);
RIAK_COMPAT_V20(compat_v20_index_query_stream, riak_index_query_stream);
RIAK_COMPAT_V20(compat_v20_dt_fetch, riak_dt_fetch);
RIAK_COMPAT_V20(compat_v20_dt_update, riak_dt_update);
/* NOLINTEND(portability-no-assembler) */

/* ---- conversions ---------------------------------------------------------- */

static const riak_opts *opts_from_v2(const riak_opts_v2 *o, riak_opts *n) {
    if (o == NULL) {
        return NULL;
    }
    memset(n, 0, sizeof *n);
    n->r = o->r;
    n->pr = o->pr;
    n->w = o->w;
    n->dw = o->dw;
    n->pw = o->pw;
    n->rw = o->rw;
    n->content_type = o->content_type;
    n->vclock = o->vclock;
    n->meta = o->meta;
    n->n_meta = o->n_meta;
    n->indexes = o->indexes;
    n->n_indexes = o->n_indexes;
    n->links = o->links;
    n->n_links = o->n_links;
    n->return_body = o->return_body;
    n->bucket_type = o->bucket_type;
    n->if_none_match = o->if_none_match;
    n->if_match = o->if_match;
    n->if_modified_since = o->if_modified_since;
    n->if_unmodified_since = o->if_unmodified_since;
    n->server_timeout_ms = o->server_timeout_ms;
    n->resolver = o->resolver;
    n->resolver_ctx = o->resolver_ctx;
    return n;
}

static const riak_index_query *query_from_v2(const riak_index_query_v2 *q, riak_index_query *n) {
    if (q == NULL) {
        return NULL;
    }
    memset(n, 0, sizeof *n);
    n->index = q->index;
    n->value = q->value;
    n->range_min = q->range_min;
    n->range_max = q->range_max;
    n->max_results = q->max_results;
    n->continuation = q->continuation;
    n->return_terms = q->return_terms;
    n->term_regex = q->term_regex;
    n->bucket_type = q->bucket_type;
    n->server_timeout_ms = q->server_timeout_ms;
    return n;
}

static const riak_dt_opts *dt_opts_from_v2(const riak_dt_opts_v2 *o, riak_dt_opts *n) {
    if (o == NULL) {
        return NULL;
    }
    memset(n, 0, sizeof *n);
    n->r = o->r;
    n->pr = o->pr;
    n->w = o->w;
    n->dw = o->dw;
    n->pw = o->pw;
    n->return_body = o->return_body;
    n->no_context = o->no_context;
    n->server_timeout_ms = o->server_timeout_ms;
    return n;
}

/* A 2.0 configuration as the current one; *nodes receives a node array to be
 * freed by the caller. 2.0 spoke HTTP only. */
static riak_status config_from_v2(const riak_config_v2 *c, riak_config *n, riak_node **nodes) {
    *nodes = NULL;
    riak_config_init(n);
    n->transport = RIAK_TRANSPORT_HTTP;
    n->host = c->host;
    n->port = c->port;
    n->tls = c->tls;
    n->tls_verify = c->tls_verify;
    n->ca_file = c->ca_file;
    n->connect_timeout_ms = c->connect_timeout_ms;
    n->timeout_ms = c->timeout_ms;
    n->client_id = c->client_id;
    n->max_body_bytes = c->max_body_bytes;
    n->n_nodes = c->n_nodes;
    n->balance = c->balance;
    n->max_retries = c->max_retries;
    n->retry_base_ms = c->retry_base_ms;
    n->retry_max_ms = c->retry_max_ms;
    n->fail_threshold = c->fail_threshold;
    n->eject_ms = c->eject_ms;
    n->username = c->username;
    n->password = c->password;
    n->tls_cert_file = c->tls_cert_file;
    n->tls_key_file = c->tls_key_file;
    n->tls_key_password = c->tls_key_password;
    n->log_fn = c->log_fn;
    n->log_level = c->log_level;
    n->log_ctx = c->log_ctx;
    n->metrics_fn = c->metrics_fn;
    n->metrics_ctx = c->metrics_ctx;
    n->nodes = NULL;
    if (c->n_nodes > 0 && c->nodes != NULL) {
        riak_node *copy = calloc(c->n_nodes, sizeof *copy);
        if (copy == NULL) {
            return RIAK_E_NOMEM;
        }
        for (size_t i = 0; i < c->n_nodes; i++) {
            copy[i].host = c->nodes[i].host;
            copy[i].port = c->nodes[i].port;
        }
        n->nodes = copy;
        *nodes = copy;
    }
    return RIAK_OK;
}

/* ---- entry points --------------------------------------------------------- */

void compat_v20_config_init(riak_config_v2 *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->host = "127.0.0.1";
    cfg->port = 8098u;
    cfg->tls_verify = true;
    cfg->max_body_bytes = (size_t)64 * 1024 * 1024;
    cfg->max_retries = 2;
    cfg->retry_base_ms = 50;
    cfg->retry_max_ms = 2000;
    cfg->fail_threshold = 3;
    cfg->eject_ms = 5000;
    cfg->log_level = RIAK_LOG_WARN;
}

riak_status compat_v20_client_new(const riak_config_v2 *cfg, riak_client **out) {
    riak_config_v2 defaults; /* NULL means the 2.0 defaults: HTTP, port 8098 */
    if (cfg == NULL) {
        compat_v20_config_init(&defaults);
        cfg = &defaults;
    }
    riak_config n;
    riak_node *nodes;
    riak_status st = config_from_v2(cfg, &n, &nodes);
    if (st == RIAK_OK) {
        st = riak_client_new(&n, out);
    } else if (out) {
        *out = NULL;
    }
    free(nodes);
    return st;
}

riak_status compat_v20_async_new(const riak_config_v2 *cfg, riak_async **out) {
    riak_config_v2 defaults;
    if (cfg == NULL) {
        compat_v20_config_init(&defaults);
        cfg = &defaults;
    }
    riak_config n;
    riak_node *nodes;
    riak_status st = config_from_v2(cfg, &n, &nodes);
    if (st == RIAK_OK) {
        st = riak_async_new(&n, out);
    } else if (out) {
        *out = NULL;
    }
    free(nodes);
    return st;
}

riak_status compat_v20_get(riak_client *client, const char *bucket, const char *key,
                           const riak_opts_v2 *opts, riak_object *out) {
    riak_opts n;
    return riak_get(client, bucket, key, opts_from_v2(opts, &n), out);
}

riak_status compat_v20_get_siblings(riak_client *client, const char *bucket, const char *key,
                                    const riak_opts_v2 *opts, riak_object_set *out) {
    riak_opts n;
    return riak_get_siblings(client, bucket, key, opts_from_v2(opts, &n), out);
}

riak_status compat_v20_put(riak_client *client, const char *bucket, const char *key,
                           const void *value, size_t len, const riak_opts_v2 *opts,
                           riak_object *out, char **created_key) {
    riak_opts n;
    return riak_put(client, bucket, key, value, len, opts_from_v2(opts, &n), out, created_key);
}

riak_status compat_v20_delete(riak_client *client, const char *bucket, const char *key,
                              const riak_opts_v2 *opts) {
    riak_opts n;
    return riak_delete(client, bucket, key, opts_from_v2(opts, &n));
}

riak_status compat_v20_async_get(riak_async *a, const char *bucket, const char *key,
                                 const riak_opts_v2 *opts, riak_async_cb cb, void *ctx) {
    riak_opts n;
    return riak_async_get(a, bucket, key, opts_from_v2(opts, &n), cb, ctx);
}

riak_status compat_v20_async_put(riak_async *a, const char *bucket, const char *key,
                                 const void *value, size_t len, const riak_opts_v2 *opts,
                                 riak_async_cb cb, void *ctx) {
    riak_opts n;
    return riak_async_put(a, bucket, key, value, len, opts_from_v2(opts, &n), cb, ctx);
}

riak_status compat_v20_async_delete(riak_async *a, const char *bucket, const char *key,
                                    const riak_opts_v2 *opts, riak_async_cb cb, void *ctx) {
    riak_opts n;
    return riak_async_delete(a, bucket, key, opts_from_v2(opts, &n), cb, ctx);
}

riak_status compat_v20_index_query_run(riak_client *client, const char *bucket,
                                       const riak_index_query_v2 *q, riak_index_result *out) {
    riak_index_query n;
    return riak_index_query_run(client, bucket, query_from_v2(q, &n), out);
}

riak_status compat_v20_index_query_stream(riak_client *client, const char *bucket,
                                          const riak_index_query_v2 *q, riak_index_cb cb, void *ctx,
                                          char **continuation) {
    riak_index_query n;
    return riak_index_query_stream(client, bucket, query_from_v2(q, &n), cb, ctx, continuation);
}

riak_status compat_v20_dt_fetch(riak_client *client, const char *type, const char *bucket,
                                const char *key, const riak_dt_opts_v2 *opts, riak_datatype *out) {
    riak_dt_opts n;
    return riak_dt_fetch(client, type, bucket, key, dt_opts_from_v2(opts, &n), out);
}

riak_status compat_v20_dt_update(riak_client *client, const char *type, const char *bucket,
                                 const char *key, const riak_dt_op *op, const char *context,
                                 const riak_dt_opts_v2 *opts, riak_datatype *out,
                                 char **created_key) {
    riak_dt_opts n;
    return riak_dt_update(client, type, bucket, key, op, context, dt_opts_from_v2(opts, &n), out,
                          created_key);
}
