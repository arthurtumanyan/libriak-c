/*
 File:   parse.h
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later

 JSON response parsing. Independent of libcurl so it can be unit-tested.
 */

#ifndef RIAK_PARSE_H
#define RIAK_PARSE_H

#include <stdbool.h>
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

/* One page (or streamed chunk) of a 2i response: {"keys":[...]} or
 * {"results":[{"<term>":"<key>"},...]}, optionally with "continuation".
 * visit() returns false to stop (the call then returns RIAK_OK with
 * *stopped set). *continuation is replaced when the page carries one. */
typedef bool (*riak_index_visit_fn)(const char *key, const char *term, void *ctx);
riak_status riak_parse_index_json(const char *json, size_t len, riak_index_visit_fn visit,
                                  void *ctx, char **continuation, bool *stopped);

/* Solr JSON response (wt=json) into a riak_search_result; raw is not set. */
riak_status riak_parse_search(const char *json, size_t len, riak_search_result *out);

/* One MapReduce chunk {"phase":N,"data":<json>}; *data is newly allocated. */
riak_status riak_parse_mapred_chunk(const char *json, size_t len, unsigned *phase, char **data);

#endif /* RIAK_PARSE_H */
