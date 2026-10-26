/*
 File:   multipart.h
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later

 multipart/mixed parsing (RFC 2046) for sibling responses. Independent of
 libcurl so it can be unit-tested and fuzzed.
 */

#ifndef RIAK_MULTIPART_H
#define RIAK_MULTIPART_H

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

/* Split body into parts. On success *parts holds *n parts (n may be 0). */
riak_status riak_multipart_parse(const char *boundary, const char *body, size_t len,
                                 riak_part **parts, size_t *n);
void riak_parts_free(riak_part *parts, size_t n);

#endif /* RIAK_MULTIPART_H */
