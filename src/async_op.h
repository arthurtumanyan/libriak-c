/*
 File:   async_op.h
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later

 What an asynchronous request is to the two engines (async.c over HTTP,
 async_pb.c over Protocol Buffers): a prepared request, built by the same
 prepare functions as the blocking call, and an operation definition that
 turns the final response into a riak_async_result with the same finish
 functions. Streams carry their sink in the prepared request; its context
 lives in the operation's state.
 */

#ifndef RIAK_ASYNC_OP_H
#define RIAK_ASYNC_OP_H

#include "internal.h"
#include "pb.h"

typedef struct {
    riak_async_kind kind;
    /* the request succeeded: the response becomes the result's members */
    riak_status (*finish_http)(riak_client *c, void *state, riak_response *resp,
                               riak_async_result *res);
    riak_status (*finish_pb)(riak_client *c, void *state, riak_pb_response *resp,
                             riak_async_result *res);
    /* the request failed with st: the status reported (NULL: st itself) */
    riak_status (*on_error)(riak_client *c, void *state, riak_status st);
    /* HTTP streams: the response the sink reads headers from */
    void (*bind_http)(void *state, riak_response *resp);
    void (*free_state)(void *state);
} riak_aop_def;

/* Every member a callback did not take is freed. */
void riak_async_result_clear(riak_async_result *res);

/* The Protocol Buffers engine of a handle, or NULL for HTTP. */
riak_pb_async *riak_async_pb_engine(riak_async *a);

/* A prepared request is submitted with its definition and state; both the
 * request and the state are owned by the engine from then on, also when the
 * submission fails. */
riak_status riak_async_submit_http(riak_async *a, const riak_aop_def *def, void *state,
                                   riak_prepared *p, riak_async_cb cb, void *ctx);
riak_status riak_pb_async_submit_op(riak_pb_async *a, const riak_aop_def *def, void *state,
                                    riak_pb_prepared *p, riak_async_cb cb, void *ctx);

#endif /* RIAK_ASYNC_OP_H */
