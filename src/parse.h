/*
 File:   parse.h
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later

 JSON response parsing. Independent of libcurl so it can be unit-tested.
 */

#ifndef RIAK_PARSE_H
#define RIAK_PARSE_H

#include <stddef.h>

#include "riak/riak.h"

/* Parse {"<field>": ["a", "b", ...]} from a complete body. */
riak_status riak_parse_string_array(const char *json, size_t len, const char *field,
                                    riak_strlist *out);

/* Incremental parser for ?keys=stream responses, which are a sequence of
 * concatenated {"keys":[...]} objects split arbitrarily across chunks. */
typedef struct riak_keystream riak_keystream;

riak_keystream *riak_keystream_new(void);
riak_status riak_keystream_feed(riak_keystream *ks, const char *data, size_t len);
/* Move the collected keys into out; fails if the input ended mid-object. */
riak_status riak_keystream_finish(riak_keystream *ks, riak_strlist *out);
void riak_keystream_free(riak_keystream *ks);

#endif /* RIAK_PARSE_H */
