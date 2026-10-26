/*
 File:   util.h
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later

 Growable buffers and string helpers. No libcurl/json-c dependency so they
 can be unit-tested in isolation.
 */

#ifndef RIAK_UTIL_H
#define RIAK_UTIL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#include "riak/riak.h"

#if defined(__GNUC__)
#define RIAK_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))
#else
#define RIAK_PRINTF(fmt, args)
#endif

typedef struct {
    char *data; /* always NUL-terminated once anything was appended */
    size_t len;
    size_t cap;
} riak_buf;

/* Return 0 on success, -1 on allocation failure or size overflow. On failure
 * the buffer keeps its previous contents. */
int riak_buf_append(riak_buf *b, const void *data, size_t len);
int riak_buf_append_str(riak_buf *b, const char *s);
/* Hand the contents to the caller (never NULL on success; "" when empty)
 * and reset the buffer. Returns NULL only on allocation failure. */
char *riak_buf_steal(riak_buf *b, size_t *len);
void riak_buf_free(riak_buf *b);

char *riak_strdup(const char *s);
char *riak_strndup(const char *s, size_t n);
char *riak_vstrprintf(const char *fmt, va_list ap) RIAK_PRINTF(1, 0);
char *riak_strprintf(const char *fmt, ...) RIAK_PRINTF(1, 2);

/* Append a percent-encoded path segment / query component (RFC 3986:
 * everything except unreserved characters is encoded). */
int riak_url_append_escaped(riak_buf *b, const char *s);
/* Append "?name=value" or "&name=value" depending on what is already there. */
int riak_url_append_query(riak_buf *b, const char *name, const char *value);
/* Append a quorum parameter; RIAK_Q_DEFAULT appends nothing. */
int riak_url_append_quorum(riak_buf *b, const char *name, int q);
/* Decode %XX sequences; returns NULL on malformed input or ENOMEM. */
char *riak_url_unescape(const char *s, size_t len);

/* Header values must not contain CR, LF or NUL; names must be RFC 7230
 * tokens. */
bool riak_header_value_ok(const char *v);
bool riak_header_name_ok(const char *n);

/* String list builder. */
int riak_strlist_push(riak_strlist *l, size_t *cap, const char *s, size_t len);

#endif /* RIAK_UTIL_H */
