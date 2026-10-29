/*
 File:   async_ops.c
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later

 The asynchronous calls. Each validates its arguments as the blocking call
 does, builds the request with the blocking call's prepare function for the
 handle's transport, and submits it with an operation definition whose
 finish functions are the blocking call's. Arguments are copied or encoded
 on submission; only callbacks and their contexts are used afterwards.
 */

#include "async_op.h"
#include "datatype.h"
#include "multipart.h"
#include "parse.h"

#include <stdlib.h>
#include <string.h>

void riak_async_result_clear(riak_async_result *res) {
    riak_object_free(&res->object);
    free(res->created_key);
    res->created_key = NULL;
    riak_object_set_free(&res->siblings);
    riak_datatype_free(&res->datatype);
    riak_index_result_free(&res->index);
    riak_search_result_free(&res->search);
    free(res->json);
    res->json = NULL;
    free(res->continuation);
    res->continuation = NULL;
}

static void free_plain(void *state) {
    free(state);
}

/* The prepared request of one transport is submitted, and the engine owns
 * state from then on. When preparing failed (st), the caller frees state
 * itself before the call, which only releases the prepared request. */
static riak_status submit(riak_async *a, const riak_aop_def *def, void *state, riak_status st,
                          riak_prepared *hp, riak_pb_prepared *pp, riak_async_cb cb, void *ctx) {
    riak_pb_async *pa = riak_async_pb_engine(a);
    if (st != RIAK_OK) {
        /* the caller has freed state: the request was never submitted */
        if (pa) {
            riak_pb_prepared_free(pp);
        } else {
            riak_prepared_free(hp);
        }
        return st;
    }
    return pa ? riak_pb_async_submit_op(pa, def, state, pp, cb, ctx)
              : riak_async_submit_http(a, def, state, hp, cb, ctx);
}

static void *new_state(riak_async *a, size_t size) {
    void *s = calloc(1, size);
    if (s == NULL) riak_set_error(riak_async_client(a), RIAK_E_NOMEM, "out of memory");
    return s;
}

/* ---- ping, get, put, delete ------------------------------------------ */

static riak_status ping_http(riak_client *c, void *state, riak_response *resp,
                             riak_async_result *res) {
    (void)state;
    (void)res;
    return riak_finish_ping(c, resp);
}

static const riak_aop_def ping_def = {.kind = RIAK_ASYNC_PING, .finish_http = ping_http};

riak_status riak_async_ping(riak_async *a, riak_async_cb cb, void *ctx) {
    if (a == NULL || cb == NULL) return RIAK_E_INVALID_ARG;
    riak_client *c = riak_async_client(a);
    riak_prepared hp = {0};
    riak_pb_prepared pp = {0};
    riak_status st =
        riak_async_pb_engine(a) ? riak_pb_prepare_ping(c, &pp) : riak_prepare_ping(c, &hp);
    return submit(a, &ping_def, NULL, st, &hp, &pp, cb, ctx);
}

typedef struct {
    riak_opts opts; /* resolver and resolver_ctx are used when the answer comes */
} get_state;

static riak_status get_http(riak_client *c, void *state, riak_response *resp,
                            riak_async_result *res) {
    return riak_kv_finish_get(c, &((get_state *)state)->opts, resp, &res->object);
}

static riak_status get_pb(riak_client *c, void *state, riak_pb_response *resp,
                          riak_async_result *res) {
    return riak_pb_finish_get(c, &((get_state *)state)->opts, resp, &res->object);
}

static const riak_aop_def get_def = {
    .kind = RIAK_ASYNC_GET, .finish_http = get_http, .finish_pb = get_pb, .free_state = free_plain};

riak_status riak_async_get(riak_async *a, const char *bucket, const char *key,
                           const riak_opts *opts, riak_async_cb cb, void *ctx) {
    if (a == NULL || cb == NULL) return RIAK_E_INVALID_ARG;
    riak_client *c = riak_async_client(a);
    get_state *s = new_state(a, sizeof *s);
    if (s == NULL) return RIAK_E_NOMEM;
    if (opts) s->opts = *opts;
    riak_prepared hp = {0};
    riak_pb_prepared pp = {0};
    riak_status st = riak_async_pb_engine(a) ? riak_pb_prepare_get(c, bucket, key, &s->opts, &pp)
                                             : riak_kv_prepare_get(c, bucket, key, &s->opts, &hp);
    if (st != RIAK_OK) {
        free_plain(s);
        s = NULL;
    }
    return submit(a, &get_def, s, st, &hp, &pp, cb, ctx);
}

typedef struct {
    bool generated_key, return_body;
} put_state;

static riak_status put_http(riak_client *c, void *state, riak_response *resp,
                            riak_async_result *res) {
    put_state *s = state;
    return riak_kv_finish_put(c, s->generated_key, resp, &res->object,
                              s->generated_key ? &res->created_key : NULL);
}

static riak_status put_pb(riak_client *c, void *state, riak_pb_response *resp,
                          riak_async_result *res) {
    put_state *s = state;
    return riak_pb_finish_put(c, resp, s->return_body ? &res->object : NULL,
                              s->generated_key ? &res->created_key : NULL);
}

static const riak_aop_def put_def = {
    .kind = RIAK_ASYNC_PUT, .finish_http = put_http, .finish_pb = put_pb, .free_state = free_plain};

riak_status riak_async_put(riak_async *a, const char *bucket, const char *key, const void *value,
                           size_t len, const riak_opts *opts, riak_async_cb cb, void *ctx) {
    if (a == NULL || cb == NULL || (value == NULL && len > 0)) return RIAK_E_INVALID_ARG;
    static const riak_opts no_opts;
    const riak_opts *o = opts ? opts : &no_opts;
    riak_client *c = riak_async_client(a);
    put_state *s = new_state(a, sizeof *s);
    if (s == NULL) return RIAK_E_NOMEM;
    s->generated_key = key == NULL;
    s->return_body = o->return_body;
    riak_prepared hp = {0};
    riak_pb_prepared pp = {0};
    riak_status st = riak_async_pb_engine(a)
                         ? riak_pb_prepare_put(c, bucket, key, value, len, o, &pp)
                         : riak_kv_prepare_put(c, bucket, key, value, len, o, true, &hp);
    if (st != RIAK_OK) {
        free_plain(s);
        s = NULL;
    }
    return submit(a, &put_def, s, st, &hp, &pp, cb, ctx);
}

static riak_status delete_http(riak_client *c, void *state, riak_response *resp,
                               riak_async_result *res) {
    (void)state;
    (void)res;
    return riak_kv_finish_delete(c, resp);
}

static riak_status delete_pb_error(riak_client *c, void *state, riak_status st) {
    (void)state;
    if (st == RIAK_E_NOT_FOUND) {
        riak_clear_error(c); /* deleting an absent key succeeds */
        return RIAK_OK;
    }
    return st;
}

static const riak_aop_def delete_http_def = {.kind = RIAK_ASYNC_DELETE, .finish_http = delete_http};
static const riak_aop_def delete_pb_def = {.kind = RIAK_ASYNC_DELETE, .on_error = delete_pb_error};

riak_status riak_async_delete(riak_async *a, const char *bucket, const char *key,
                              const riak_opts *opts, riak_async_cb cb, void *ctx) {
    if (a == NULL || cb == NULL) return RIAK_E_INVALID_ARG;
    static const riak_opts no_opts;
    const riak_opts *o = opts ? opts : &no_opts;
    riak_client *c = riak_async_client(a);
    riak_prepared hp = {0};
    riak_pb_prepared pp = {0};
    bool pb = riak_async_pb_engine(a) != NULL;
    riak_status st = pb ? riak_pb_prepare_delete(c, bucket, key, o, &pp)
                        : riak_kv_prepare_delete(c, bucket, key, o, &hp);
    return submit(a, pb ? &delete_pb_def : &delete_http_def, NULL, st, &hp, &pp, cb, ctx);
}

/* ---- siblings ---------------------------------------------------------- */

static riak_status siblings_http(riak_client *c, void *state, riak_response *resp,
                                 riak_async_result *res) {
    (void)state;
    return riak_kv_finish_siblings(c, resp, &res->siblings);
}

static riak_status siblings_pb(riak_client *c, void *state, riak_pb_response *resp,
                               riak_async_result *res) {
    (void)state;
    return riak_pb_finish_siblings(c, resp, &res->siblings);
}

static const riak_aop_def siblings_def = {
    .kind = RIAK_ASYNC_GET_SIBLINGS, .finish_http = siblings_http, .finish_pb = siblings_pb};

riak_status riak_async_get_siblings(riak_async *a, const char *bucket, const char *key,
                                    const riak_opts *opts, riak_async_cb cb, void *ctx) {
    if (a == NULL || cb == NULL) return RIAK_E_INVALID_ARG;
    static const riak_opts no_opts;
    const riak_opts *o = opts ? opts : &no_opts;
    riak_client *c = riak_async_client(a);
    riak_prepared hp = {0};
    riak_pb_prepared pp = {0};
    riak_status st = riak_async_pb_engine(a) ? riak_pb_prepare_get(c, bucket, key, o, &pp)
                                             : riak_kv_prepare_siblings(c, bucket, key, o, &hp);
    return submit(a, &siblings_def, NULL, st, &hp, &pp, cb, ctx);
}

/* ---- data types --------------------------------------------------------- */

static riak_status dt_names(riak_client *c, const char *type, const char *bucket, const char *key,
                            bool key_required) {
    riak_status st;
    if ((st = riak_check_name(c, type, "bucket type")) != RIAK_OK ||
        (st = riak_check_name(c, bucket, "bucket")) != RIAK_OK ||
        ((key_required || key) && (st = riak_check_name(c, key, "key")) != RIAK_OK)) {
        return st;
    }
    return RIAK_OK;
}

static riak_status dt_fetch_http(riak_client *c, void *state, riak_response *resp,
                                 riak_async_result *res) {
    (void)state;
    return riak_dt_http_finish_fetch(c, resp, &res->datatype);
}

static riak_status dt_fetch_pb(riak_client *c, void *state, riak_pb_response *resp,
                               riak_async_result *res) {
    (void)state;
    return riak_pb_dt_finish_fetch(c, resp, &res->datatype);
}

static const riak_aop_def dt_fetch_def = {
    .kind = RIAK_ASYNC_DT_FETCH, .finish_http = dt_fetch_http, .finish_pb = dt_fetch_pb};

riak_status riak_async_dt_fetch(riak_async *a, const char *type, const char *bucket,
                                const char *key, const riak_dt_opts *opts, riak_async_cb cb,
                                void *ctx) {
    if (a == NULL || cb == NULL) return RIAK_E_INVALID_ARG;
    static const riak_dt_opts no_opts;
    const riak_dt_opts *o = opts ? opts : &no_opts;
    riak_client *c = riak_async_client(a);
    riak_status st = dt_names(c, type, bucket, key, true);
    if (st != RIAK_OK) return st;
    riak_prepared hp = {0};
    riak_pb_prepared pp = {0};
    st = riak_async_pb_engine(a) ? riak_pb_dt_prepare_fetch(c, type, bucket, key, o, &pp)
                                 : riak_dt_http_prepare_fetch(c, type, bucket, key, o, &hp);
    return submit(a, &dt_fetch_def, NULL, st, &hp, &pp, cb, ctx);
}

typedef struct {
    riak_dt_type type;
    bool return_body, generated_key;
} dt_update_state;

static riak_status dt_update_http(riak_client *c, void *state, riak_response *resp,
                                  riak_async_result *res) {
    dt_update_state *s = state;
    return riak_dt_http_finish_update(c, resp, s->generated_key, &res->datatype,
                                      s->generated_key ? &res->created_key : NULL);
}

static riak_status dt_update_pb(riak_client *c, void *state, riak_pb_response *resp,
                                riak_async_result *res) {
    dt_update_state *s = state;
    return riak_pb_dt_finish_update(c, resp, s->type, s->return_body, s->generated_key,
                                    &res->datatype, s->generated_key ? &res->created_key : NULL);
}

static const riak_aop_def dt_update_def = {.kind = RIAK_ASYNC_DT_UPDATE,
                                           .finish_http = dt_update_http,
                                           .finish_pb = dt_update_pb,
                                           .free_state = free_plain};

riak_status riak_async_dt_update(riak_async *a, const char *type, const char *bucket,
                                 const char *key, const riak_dt_op *op, const char *context,
                                 const riak_dt_opts *opts, riak_async_cb cb, void *ctx) {
    if (a == NULL || cb == NULL || op == NULL) return RIAK_E_INVALID_ARG;
    static const riak_dt_opts no_opts;
    const riak_dt_opts *o = opts ? opts : &no_opts;
    riak_client *c = riak_async_client(a);
    riak_status st = dt_names(c, type, bucket, key, false);
    if (st != RIAK_OK) return st;
    dt_update_state *s = new_state(a, sizeof *s);
    if (s == NULL) return RIAK_E_NOMEM;
    s->type = op->type;
    s->return_body = o->return_body;
    s->generated_key = key == NULL;
    riak_prepared hp = {0};
    riak_pb_prepared pp = {0};
    st = riak_async_pb_engine(a)
             ? riak_pb_dt_prepare_update(c, type, bucket, key, op, context, o, &pp)
             : riak_dt_http_prepare_update(c, type, bucket, key, op, context, o, &hp);
    if (st != RIAK_OK) {
        free_plain(s);
        s = NULL;
    }
    return submit(a, &dt_update_def, s, st, &hp, &pp, cb, ctx);
}

/* ---- legacy counters ---------------------------------------------------- */

typedef struct {
    bool update;
} counter_state;

static riak_status counter_http(riak_client *c, void *state, riak_response *resp,
                                riak_async_result *res) {
    (void)state;
    return riak_counter_http_finish(c, resp, &res->counter);
}

static riak_status counter_pb(riak_client *c, void *state, riak_pb_response *resp,
                              riak_async_result *res) {
    return riak_pb_counter_finish(c, resp, ((counter_state *)state)->update, &res->counter);
}

static const riak_aop_def counter_get_def = {.kind = RIAK_ASYNC_COUNTER_GET,
                                             .finish_http = counter_http,
                                             .finish_pb = counter_pb,
                                             .free_state = free_plain};
static const riak_aop_def counter_update_def = {.kind = RIAK_ASYNC_COUNTER_UPDATE,
                                                .finish_http = counter_http,
                                                .finish_pb = counter_pb,
                                                .free_state = free_plain};

static riak_status counter(riak_async *a, const char *bucket, const char *key, bool update,
                           int64_t amount, const riak_opts *opts, riak_async_cb cb, void *ctx) {
    if (a == NULL || cb == NULL) return RIAK_E_INVALID_ARG;
    static const riak_opts no_opts;
    const riak_opts *o = opts ? opts : &no_opts;
    riak_client *c = riak_async_client(a);
    riak_status st;
    if ((st = riak_check_name(c, bucket, "bucket")) != RIAK_OK ||
        (st = riak_check_name(c, key, "key")) != RIAK_OK) {
        return st;
    }
    if (o->bucket_type) {
        return riak_set_error(c, RIAK_E_UNSUPPORTED,
                              "1.4 counters live in the default bucket type");
    }
    counter_state *s = new_state(a, sizeof *s);
    if (s == NULL) return RIAK_E_NOMEM;
    s->update = update;
    riak_prepared hp = {0};
    riak_pb_prepared pp = {0};
    st = riak_async_pb_engine(a)
             ? riak_pb_counter_prepare(c, bucket, key, update, amount, o, true, &pp)
             : riak_counter_http_prepare(c, bucket, key, update, amount, o, true, &hp);
    if (st != RIAK_OK) {
        free_plain(s);
        s = NULL;
    }
    return submit(a, update ? &counter_update_def : &counter_get_def, s, st, &hp, &pp, cb, ctx);
}

riak_status riak_async_counter_get(riak_async *a, const char *bucket, const char *key,
                                   const riak_opts *opts, riak_async_cb cb, void *ctx) {
    return counter(a, bucket, key, false, 0, opts, cb, ctx);
}

riak_status riak_async_counter_update(riak_async *a, const char *bucket, const char *key,
                                      int64_t amount, const riak_opts *opts, riak_async_cb cb,
                                      void *ctx) {
    return counter(a, bucket, key, true, amount, opts, cb, ctx);
}

/* ---- secondary indexes ----------------------------------------------------- */

static riak_status index_names(riak_client *c, const char *bucket, const riak_index_query *q) {
    riak_status st;
    if (q == NULL) {
        return riak_set_error(c, RIAK_E_INVALID_ARG, "query must not be NULL");
    }
    if ((st = riak_check_name(c, bucket, "bucket")) != RIAK_OK ||
        (st = riak_check_name(c, q->index, "index")) != RIAK_OK ||
        (q->bucket_type && (st = riak_check_name(c, q->bucket_type, "bucket type")) != RIAK_OK)) {
        return st;
    }
    return RIAK_OK;
}

typedef struct {
    riak_index_acc acc; /* Protocol Buffers: matches as they arrive */
    riak_pb_index_ctx x;
    char *continuation;
} index_state;

static void index_state_free(void *state) {
    index_state *s = state;
    riak_strlist_free(&s->acc.keys);
    riak_strlist_free(&s->acc.terms);
    free(s->continuation);
    free(s);
}

static riak_status index_http(riak_client *c, void *state, riak_response *resp,
                              riak_async_result *res) {
    return riak_index_http_finish(c, resp, ((index_state *)state)->acc.want_terms, &res->index);
}

static riak_status index_pb(riak_client *c, void *state, riak_pb_response *resp,
                            riak_async_result *res) {
    (void)resp;
    index_state *s = state;
    riak_status st = riak_index_acc_take(&s->acc, &res->index);
    memset(&s->acc, 0, sizeof s->acc); /* now owned by res, or freed */
    if (st != RIAK_OK) {
        return riak_set_error(c, st, "out of memory");
    }
    res->index.continuation = s->continuation;
    s->continuation = NULL;
    return RIAK_OK;
}

static const riak_aop_def index_def = {.kind = RIAK_ASYNC_INDEX_QUERY,
                                       .finish_http = index_http,
                                       .finish_pb = index_pb,
                                       .free_state = index_state_free};

riak_status riak_async_index_query(riak_async *a, const char *bucket, const riak_index_query *q,
                                   riak_async_cb cb, void *ctx) {
    if (a == NULL || cb == NULL) return RIAK_E_INVALID_ARG;
    riak_client *c = riak_async_client(a);
    riak_status st = index_names(c, bucket, q);
    if (st != RIAK_OK) return st;
    index_state *s = new_state(a, sizeof *s);
    if (s == NULL) return RIAK_E_NOMEM;
    s->acc.want_terms = q->return_terms;
    s->x = (riak_pb_index_ctx){
        .cb = riak_index_acc_add, .ctx = &s->acc, .continuation = &s->continuation};
    riak_prepared hp = {0};
    riak_pb_prepared pp = {0};
    st = riak_async_pb_engine(a) ? riak_pb_index_prepare(c, bucket, q, &s->x, &pp)
                                 : riak_index_http_prepare(c, bucket, q, false, &hp);
    if (st != RIAK_OK) {
        index_state_free(s);
        s = NULL;
    }
    return submit(a, &index_def, s, st, &hp, &pp, cb, ctx);
}

typedef struct {
    riak_http_stream hs;  /* HTTP: the multipart parser */
    riak_index_stream is; /* HTTP: matches to the item callback */
    riak_pb_index_ctx x;  /* Protocol Buffers */
    char *continuation;
} index_stream_state;

static void index_stream_free(void *state) {
    index_stream_state *s = state;
    riak_mpstream_free(s->hs.mp); /* a parser left behind by a cancelled stream */
    free(s->continuation);
    free(s);
}

static void index_stream_bind(void *state, riak_response *resp) {
    ((index_stream_state *)state)->hs.resp = resp;
}

static riak_status take_continuation(index_stream_state *s, riak_async_result *res) {
    res->continuation = s->continuation;
    s->continuation = NULL;
    return RIAK_OK;
}

static riak_status index_stream_http(riak_client *c, void *state, riak_response *resp,
                                     riak_async_result *res) {
    (void)c;
    (void)resp;
    index_stream_state *s = state;
    riak_status st = riak_http_stream_end(&s->hs, RIAK_OK);
    return st == RIAK_OK ? take_continuation(s, res) : st;
}

static riak_status index_stream_http_error(riak_client *c, void *state, riak_status st) {
    (void)c;
    return riak_http_stream_end(&((index_stream_state *)state)->hs, st);
}

static riak_status index_stream_pb(riak_client *c, void *state, riak_pb_response *resp,
                                   riak_async_result *res) {
    (void)c;
    (void)resp;
    return take_continuation(state, res);
}

static const riak_aop_def index_stream_def = {.kind = RIAK_ASYNC_INDEX_STREAM,
                                              .finish_http = index_stream_http,
                                              .finish_pb = index_stream_pb,
                                              .on_error = index_stream_http_error,
                                              .bind_http = index_stream_bind,
                                              .free_state = index_stream_free};
static const riak_aop_def index_stream_pb_def = {
    .kind = RIAK_ASYNC_INDEX_STREAM, .finish_pb = index_stream_pb, .free_state = index_stream_free};

riak_status riak_async_index_query_stream(riak_async *a, const char *bucket,
                                          const riak_index_query *q, riak_index_cb item,
                                          void *item_ctx, riak_async_cb cb, void *ctx) {
    if (a == NULL || cb == NULL || item == NULL) return RIAK_E_INVALID_ARG;
    riak_client *c = riak_async_client(a);
    riak_status st = index_names(c, bucket, q);
    if (st != RIAK_OK) return st;
    index_stream_state *s = new_state(a, sizeof *s);
    if (s == NULL) return RIAK_E_NOMEM;
    riak_prepared hp = {0};
    riak_pb_prepared pp = {0};
    bool pb = riak_async_pb_engine(a) != NULL;
    if (pb) {
        s->x = (riak_pb_index_ctx){.cb = item, .ctx = item_ctx, .continuation = &s->continuation};
        st = riak_pb_index_prepare(c, bucket, q, &s->x, &pp);
    } else {
        s->is = (riak_index_stream){.cb = item, .ctx = item_ctx, .continuation = &s->continuation};
        s->hs = (riak_http_stream){.c = c, .fn = riak_index_chunk, .ctx = &s->is};
        st = riak_index_http_prepare(c, bucket, q, true, &hp);
        if (st == RIAK_OK) riak_http_stream_attach(&s->hs, &hp.req);
    }
    if (st != RIAK_OK) {
        index_stream_free(s);
        s = NULL;
    }
    return submit(a, pb ? &index_stream_pb_def : &index_stream_def, s, st, &hp, &pp, cb, ctx);
}

/* ---- Riak Search ----------------------------------------------------------- */

static riak_status search_http(riak_client *c, void *state, riak_response *resp,
                               riak_async_result *res) {
    (void)state;
    return riak_search_http_finish(c, resp, &res->search);
}

static riak_status search_pb(riak_client *c, void *state, riak_pb_response *resp,
                             riak_async_result *res) {
    (void)state;
    return riak_pb_search_finish(c, resp, &res->search);
}

static const riak_aop_def search_def = {
    .kind = RIAK_ASYNC_SEARCH, .finish_http = search_http, .finish_pb = search_pb};

riak_status riak_async_search(riak_async *a, const char *index, const riak_search_query *q,
                              riak_async_cb cb, void *ctx) {
    if (a == NULL || cb == NULL || q == NULL) return RIAK_E_INVALID_ARG;
    riak_client *c = riak_async_client(a);
    riak_status st;
    if ((st = riak_check_name(c, index, "index")) != RIAK_OK ||
        (st = riak_check_name(c, q->q, "query")) != RIAK_OK) {
        return st;
    }
    riak_prepared hp = {0};
    riak_pb_prepared pp = {0};
    st = riak_async_pb_engine(a) ? riak_pb_search_prepare(c, index, q, &pp)
                                 : riak_search_http_prepare(c, index, q, &hp);
    return submit(a, &search_def, NULL, st, &hp, &pp, cb, ctx);
}

/* ---- MapReduce ------------------------------------------------------------- */

typedef struct {
    riak_http_stream hs;      /* HTTP streams */
    riak_mapred_stream_ctx m; /* HTTP streams */
    riak_pb_mapred_ctx x;     /* Protocol Buffers */
} mapred_state;

static void mapred_state_free(void *state) {
    mapred_state *s = state;
    riak_mpstream_free(s->hs.mp);
    riak_pb_mapred_ctx_free(&s->x);
    free(s);
}

static void mapred_bind(void *state, riak_response *resp) {
    ((mapred_state *)state)->hs.resp = resp;
}

static riak_status mapred_http(riak_client *c, void *state, riak_response *resp,
                               riak_async_result *res) {
    (void)state;
    return riak_mapred_http_finish(c, resp, &res->json);
}

static riak_status mapred_pb(riak_client *c, void *state, riak_pb_response *resp,
                             riak_async_result *res) {
    (void)resp;
    return riak_pb_mapred_result(c, &((mapred_state *)state)->x, &res->json);
}

static riak_status mapred_stream_http(riak_client *c, void *state, riak_response *resp,
                                      riak_async_result *res) {
    (void)c;
    (void)resp;
    (void)res;
    return riak_http_stream_end(&((mapred_state *)state)->hs, RIAK_OK);
}

static riak_status mapred_stream_http_error(riak_client *c, void *state, riak_status st) {
    (void)c;
    return riak_http_stream_end(&((mapred_state *)state)->hs, st);
}

static const riak_aop_def mapred_def = {.kind = RIAK_ASYNC_MAPRED,
                                        .finish_http = mapred_http,
                                        .finish_pb = mapred_pb,
                                        .free_state = mapred_state_free};
static const riak_aop_def mapred_stream_def = {.kind = RIAK_ASYNC_MAPRED_STREAM,
                                               .finish_http = mapred_stream_http,
                                               .on_error = mapred_stream_http_error,
                                               .bind_http = mapred_bind,
                                               .free_state = mapred_state_free};
static const riak_aop_def mapred_stream_pb_def = {.kind = RIAK_ASYNC_MAPRED_STREAM,
                                                  .free_state = mapred_state_free};

static riak_status mapred(riak_async *a, const char *job, riak_mapred_cb item, void *item_ctx,
                          riak_async_cb cb, void *ctx) {
    if (a == NULL || cb == NULL || job == NULL) return RIAK_E_INVALID_ARG;
    riak_client *c = riak_async_client(a);
    mapred_state *s = new_state(a, sizeof *s);
    if (s == NULL) return RIAK_E_NOMEM;
    riak_prepared hp = {0};
    riak_pb_prepared pp = {0};
    riak_status st;
    bool pb = riak_async_pb_engine(a) != NULL;
    if (pb) {
        s->x = (riak_pb_mapred_ctx){.cb = item, .ctx = item_ctx};
        st = riak_pb_mapred_prepare(c, job, &s->x, &pp);
    } else {
        st = riak_mapred_http_prepare(c, job, item != NULL, &hp);
        if (st == RIAK_OK && item) {
            s->m = (riak_mapred_stream_ctx){.cb = item, .ctx = item_ctx};
            s->hs = (riak_http_stream){.c = c, .fn = riak_mapred_chunk, .ctx = &s->m};
            riak_http_stream_attach(&s->hs, &hp.req);
        }
    }
    const riak_aop_def *def = item == NULL ? &mapred_def
                              : pb         ? &mapred_stream_pb_def
                                           : &mapred_stream_def;
    if (st != RIAK_OK) {
        mapred_state_free(s);
        s = NULL;
    }
    return submit(a, def, s, st, &hp, &pp, cb, ctx);
}

riak_status riak_async_mapred(riak_async *a, const char *job, riak_async_cb cb, void *ctx) {
    return mapred(a, job, NULL, NULL, cb, ctx);
}

riak_status riak_async_mapred_stream(riak_async *a, const char *job, riak_mapred_cb item,
                                     void *item_ctx, riak_async_cb cb, void *ctx) {
    if (item == NULL) return RIAK_E_INVALID_ARG;
    return mapred(a, job, item, item_ctx, cb, ctx);
}

/* ---- bucket properties ------------------------------------------------------- */

static riak_status bucket_names(riak_client *c, const char *type, const char *bucket) {
    riak_status st;
    if ((type && (st = riak_check_name(c, type, "bucket type")) != RIAK_OK) ||
        (st = riak_check_name(c, bucket, "bucket")) != RIAK_OK) {
        return st;
    }
    return RIAK_OK;
}

typedef struct {
    char *bucket; /* named in the document Protocol Buffers results are mapped to */
} props_state;

static void props_state_free(void *state) {
    props_state *s = state;
    if (s) free(s->bucket);
    free(s);
}

static riak_status props_get_http(riak_client *c, void *state, riak_response *resp,
                                  riak_async_result *res) {
    (void)state;
    return riak_body_take(c, resp, RIAK_BODY_JSON_OBJECT, &res->json);
}

static riak_status props_get_pb(riak_client *c, void *state, riak_pb_response *resp,
                                riak_async_result *res) {
    return riak_pb_props_finish_get(c, resp, ((props_state *)state)->bucket, &res->json);
}

static riak_status props_set_http(riak_client *c, void *state, riak_response *resp,
                                  riak_async_result *res) {
    (void)state;
    (void)res;
    return riak_props_http_finish_set(c, resp);
}

static const riak_aop_def props_get_def = {.kind = RIAK_ASYNC_GET_BUCKET_PROPS,
                                           .finish_http = props_get_http,
                                           .finish_pb = props_get_pb,
                                           .free_state = props_state_free};
static const riak_aop_def props_set_def = {.kind = RIAK_ASYNC_SET_BUCKET_PROPS,
                                           .finish_http = props_set_http};

riak_status riak_async_get_bucket_props(riak_async *a, const char *type, const char *bucket,
                                        riak_async_cb cb, void *ctx) {
    if (a == NULL || cb == NULL) return RIAK_E_INVALID_ARG;
    riak_client *c = riak_async_client(a);
    riak_status st = bucket_names(c, type, bucket);
    if (st != RIAK_OK) return st;
    props_state *s = new_state(a, sizeof *s);
    if (s == NULL) return RIAK_E_NOMEM;
    if ((s->bucket = riak_strdup(bucket)) == NULL) {
        free(s);
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    riak_prepared hp = {0};
    riak_pb_prepared pp = {0};
    st = riak_async_pb_engine(a) ? riak_pb_props_prepare_get(c, type, bucket, &pp)
                                 : riak_props_http_prepare(c, type, bucket, NULL, &hp);
    if (st != RIAK_OK) {
        props_state_free(s);
        s = NULL;
    }
    return submit(a, &props_get_def, s, st, &hp, &pp, cb, ctx);
}

riak_status riak_async_set_bucket_props(riak_async *a, const char *type, const char *bucket,
                                        const char *json, riak_async_cb cb, void *ctx) {
    if (a == NULL || cb == NULL || json == NULL) return RIAK_E_INVALID_ARG;
    riak_client *c = riak_async_client(a);
    riak_status st = bucket_names(c, type, bucket);
    if (st != RIAK_OK) return st;
    riak_prepared hp = {0};
    riak_pb_prepared pp = {0};
    st = riak_async_pb_engine(a) ? riak_pb_props_prepare_set(c, type, bucket, json, &pp)
                                 : riak_props_http_prepare(c, type, bucket, json, &hp);
    return submit(a, &props_set_def, NULL, st, &hp, &pp, cb, ctx);
}

/* ---- key and bucket listings ------------------------------------------------- */

typedef struct {
    riak_name_stream ns; /* HTTP */
    riak_pb_list_ctx x;  /* Protocol Buffers */
} list_state;

static void list_state_free(void *state) {
    list_state *s = state;
    riak_keystream_free(s->ns.ks); /* a parser left behind by a cancelled listing */
    free(s);
}

static riak_status list_http(riak_client *c, void *state, riak_response *resp,
                             riak_async_result *res) {
    (void)res;
    return riak_list_http_end(c, &((list_state *)state)->ns, RIAK_OK, resp);
}

static riak_status list_http_error(riak_client *c, void *state, riak_status st) {
    return riak_list_http_end(c, &((list_state *)state)->ns, st, NULL);
}

static const riak_aop_def list_keys_def = {.kind = RIAK_ASYNC_LIST_KEYS,
                                           .finish_http = list_http,
                                           .on_error = list_http_error,
                                           .free_state = list_state_free};
static const riak_aop_def list_buckets_def = {.kind = RIAK_ASYNC_LIST_BUCKETS,
                                              .finish_http = list_http,
                                              .on_error = list_http_error,
                                              .free_state = list_state_free};
static const riak_aop_def list_keys_pb_def = {.kind = RIAK_ASYNC_LIST_KEYS,
                                              .free_state = list_state_free};
static const riak_aop_def list_buckets_pb_def = {.kind = RIAK_ASYNC_LIST_BUCKETS,
                                                 .free_state = list_state_free};

static riak_status list(riak_async *a, const char *type, const char *bucket, riak_name_cb item,
                        void *item_ctx, riak_async_cb cb, void *ctx) {
    riak_client *c = riak_async_client(a);
    list_state *s = new_state(a, sizeof *s);
    if (s == NULL) return RIAK_E_NOMEM;
    riak_prepared hp = {0};
    riak_pb_prepared pp = {0};
    riak_status st;
    bool pb = riak_async_pb_engine(a) != NULL;
    if (pb) {
        s->x = (riak_pb_list_ctx){.cb = item, .ctx = item_ctx};
        st = riak_pb_list_prepare(c, type, bucket, &s->x, &pp);
    } else {
        s->ns = (riak_name_stream){.cb = item, .ctx = item_ctx};
        st = riak_list_http_prepare(c, type, bucket, &s->ns, &hp);
    }
    const riak_aop_def *def = bucket ? (pb ? &list_keys_pb_def : &list_keys_def)
                                     : (pb ? &list_buckets_pb_def : &list_buckets_def);
    if (st != RIAK_OK) {
        list_state_free(s);
        s = NULL;
    }
    return submit(a, def, s, st, &hp, &pp, cb, ctx);
}

riak_status riak_async_list_keys_stream(riak_async *a, const char *type, const char *bucket,
                                        riak_name_cb item, void *item_ctx, riak_async_cb cb,
                                        void *ctx) {
    if (a == NULL || cb == NULL || item == NULL) return RIAK_E_INVALID_ARG;
    riak_status st = bucket_names(riak_async_client(a), type, bucket);
    return st == RIAK_OK ? list(a, type, bucket, item, item_ctx, cb, ctx) : st;
}

riak_status riak_async_list_buckets_stream(riak_async *a, const char *type, riak_name_cb item,
                                           void *item_ctx, riak_async_cb cb, void *ctx) {
    if (a == NULL || cb == NULL || item == NULL) return RIAK_E_INVALID_ARG;
    riak_client *c = riak_async_client(a);
    riak_status st = type ? riak_check_name(c, type, "bucket type") : RIAK_OK;
    return st == RIAK_OK ? list(a, type, NULL, item, item_ctx, cb, ctx) : st;
}
