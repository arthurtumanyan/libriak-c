/*
 File:   multipart.h
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later

 multipart/mixed parsing (RFC 2046) for sibling responses. Independent of
 libcurl so it can be unit-tested and fuzzed.
 */

#ifndef RIAK_MULTIPART_H
#define RIAK_MULTIPART_H

#include <stdbool.h>
#include <stddef.h>

#include "riak/riak.h"
#include "util.h"

typedef struct {
    riak_headerlist headers;
    char *body; /* NUL-terminated */
    size_t len;
} riak_part;

/* Boundary parameter of a Content-Type value, or NULL (malformed/ENOMEM). */
char *riak_multipart_boundary(const char *content_type);

/* Incremental parser. The callback gets each complete part; it may take
 * ownership of part->body / part->headers by clearing them. Returning false
 * stops parsing with *err (RIAK_E_PARSE if left as RIAK_OK). */
typedef bool (*riak_part_fn)(void *ctx, riak_part *part, riak_status *err);
typedef struct riak_mpstream riak_mpstream;

/* max_part bounds the bytes buffered for one part (0 = unlimited); a larger
 * part fails the feed with RIAK_E_HTTP. NULL on bad boundary or ENOMEM. */
riak_mpstream *riak_mpstream_new(const char *boundary, size_t max_part, riak_part_fn fn, void *ctx);
riak_status riak_mpstream_feed(riak_mpstream *s, const char *data, size_t len);
/* RIAK_E_PARSE unless the close delimiter was seen. */
riak_status riak_mpstream_finish(riak_mpstream *s);
void riak_mpstream_free(riak_mpstream *s);

/* Split a complete body into parts. On success *parts holds *n parts. */
riak_status riak_multipart_parse(const char *boundary, const char *body, size_t len,
                                 riak_part **parts, size_t *n);
void riak_parts_free(riak_part *parts, size_t n);

#endif /* RIAK_MULTIPART_H */
