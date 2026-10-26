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

/* {"type":"...","value":...,"context":"..."} from a fetch or returnbody. */
riak_status riak_dt_parse(const char *json, size_t len, riak_datatype *out);

/* Serialize an operation (plus optional context) as the update document.
 * RIAK_E_INVALID_ARG for an empty or misused operation. */
riak_status riak_dt_op_to_json(const riak_dt_op *op, const char *context, char **json);

#endif /* RIAK_DATATYPE_H */
