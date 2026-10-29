/*
 File:   datatype.h
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later

 JSON encoding of Riak data types, independent of libcurl so it can be
 unit-tested and fuzzed.
 */

#ifndef RIAK_DATATYPE_H
#define RIAK_DATATYPE_H

#include <stddef.h>

#include "riak/riak.h"
#include "util.h"

/* Maps nest; parsing, building and serializing stop at this depth. */
#define RIAK_DT_MAX_DEPTH 32

/* An operation as built by the riak_dt_op_* calls; encoded as JSON for
 * HTTP and as DtOp for Protocol Buffers. */
typedef struct {
    char *name;
    riak_dt_type type;
    riak_dt_op *nested; /* SET / MAP fields */
    char *reg;
    bool flag;
    int64_t delta;
} map_field;

struct riak_dt_op {
    riak_dt_type type;
    int depth; /* nesting level inside a map operation */
    bool invalid;
    bool nomem;
    int64_t delta;
    riak_strlist adds, removes;
    size_t add_cap, remove_cap;
    map_field *fields; /* updates */
    size_t n_fields, field_cap;
    map_field *removed; /* removals: name + type only */
    size_t n_removed, removed_cap;
};

/* {"type":"...","value":...,"context":"..."} from a fetch or returnbody. */
riak_status riak_dt_parse(const char *json, size_t len, riak_datatype *out);

/* Serialize an operation (plus optional context) as the update document.
 * RIAK_E_INVALID_ARG for an empty or misused operation. */
riak_status riak_dt_op_to_json(const riak_dt_op *op, const char *context, char **json);

#endif /* RIAK_DATATYPE_H */
