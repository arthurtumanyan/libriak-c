/*
 File:   multipart.c
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "multipart.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

char *riak_multipart_boundary(const char *content_type) {
    if (content_type == NULL || strncasecmp(content_type, "multipart/", 10) != 0) {
        return NULL;
    }
    /* find a "boundary=" parameter (case-insensitive) after a ';' */
    for (const char *p = strchr(content_type, ';'); p; p = strchr(p + 1, ';')) {
        const char *q = p + 1;
        while (*q == ' ' || *q == '\t')
            q++;
        if (strncasecmp(q, "boundary=", 9) != 0) {
            continue;
        }
        q += 9;
        size_t len;
        if (*q == '"') {
            q++;
            const char *end = strchr(q, '"');
            if (end == NULL) {
                return NULL;
            }
            len = (size_t)(end - q);
        } else {
            len = strcspn(q, "; \t");
        }
        /* RFC 2046: 1 to 70 characters */
        if (len == 0 || len > 70) {
            return NULL;
        }
        return riak_strndup(q, len);
    }
    return NULL;
}

static const char *find(const char *hay, size_t hlen, const char *needle, size_t nlen) {
    if (nlen == 0 || hlen < nlen) {
        return NULL;
    }
    const char *last = hay + hlen - nlen;
    for (const char *p = hay; p <= last; p++) {
        p = memchr(p, needle[0], (size_t)(last - p) + 1);
        if (p == NULL) {
            return NULL;
        }
        if (memcmp(p, needle, nlen) == 0) {
            return p;
        }
    }
    return NULL;
}

void riak_parts_free(riak_part *parts, size_t n) {
    for (size_t i = 0; i < n; i++) {
        riak_headerlist_clear(&parts[i].headers);
        free(parts[i].body);
    }
    free(parts);
}

/* Parse the header block [p, end); lines end with CRLF. */
static riak_status parse_headers(riak_headerlist *h, const char *p, const char *end) {
    while (p < end) {
        const char *eol = find(p, (size_t)(end - p), "\r\n", 2);
        const char *line_end = eol ? eol : end;
        if (line_end > p && riak_headerlist_add_line(h, p, (size_t)(line_end - p)) < 0) {
            return RIAK_E_NOMEM;
        }
        p = eol ? eol + 2 : end;
    }
    return RIAK_OK;
}

riak_status riak_multipart_parse(const char *boundary, const char *body, size_t len,
                                 riak_part **parts, size_t *n) {
    *parts = NULL;
    *n = 0;
    size_t blen = boundary ? strlen(boundary) : 0;
    if (blen == 0 || blen > 70) {
        return RIAK_E_PARSE;
    }
    /* "\r\n--" + boundary; the first delimiter may also start the body */
    char delim[2 + 2 + 70 + 1];
    memcpy(delim, "\r\n--", 4);
    memcpy(delim + 4, boundary, blen);
    size_t dlen = 4 + blen;
    delim[dlen] = '\0';

    const char *end = body + len;
    const char *p;
    if (len >= dlen - 2 && memcmp(body, delim + 2, dlen - 2) == 0) {
        p = body + dlen - 2;
    } else {
        p = find(body, len, delim, dlen);
        if (p == NULL) {
            return RIAK_E_PARSE;
        }
        p += dlen;
    }

    riak_part *list = NULL;
    size_t count = 0, cap = 0;
    riak_status st = RIAK_E_PARSE;
    for (;;) {
        /* p is just past a delimiter */
        if (end - p >= 2 && p[0] == '-' && p[1] == '-') {
            st = RIAK_OK; /* close delimiter; anything after is epilogue */
            break;
        }
        /* transport padding, then CRLF */
        while (p < end && (*p == ' ' || *p == '\t'))
            p++;
        if (end - p < 2 || p[0] != '\r' || p[1] != '\n') {
            break;
        }
        p += 2;
        const char *next = find(p, (size_t)(end - p), delim, dlen);
        if (next == NULL) {
            break; /* no close delimiter */
        }
        const char *hdr_end;
        const char *data;
        if (next == p) {
            hdr_end = p; /* empty part */
            data = p;
        } else if (p[0] == '\r' && p[1] == '\n') {
            hdr_end = p; /* no headers */
            data = p + 2;
        } else {
            const char *sep = find(p, (size_t)(next - p), "\r\n\r\n", 4);
            hdr_end = sep ? sep : next;
            data = sep ? sep + 4 : next;
        }
        if (count == cap) {
            size_t ncap = cap ? cap * 2 : 4;
            if (ncap > SIZE_MAX / sizeof(riak_part)) {
                st = RIAK_E_NOMEM;
                break;
            }
            riak_part *grown = realloc(list, ncap * sizeof(riak_part));
            if (grown == NULL) {
                st = RIAK_E_NOMEM;
                break;
            }
            list = grown;
            cap = ncap;
        }
        riak_part *part = &list[count++];
        memset(part, 0, sizeof *part);
        part->len = (size_t)(next - data);
        part->body = riak_strndup(data, part->len);
        if (part->body == NULL || parse_headers(&part->headers, p, hdr_end) != RIAK_OK) {
            st = RIAK_E_NOMEM;
            break;
        }
        p = next + dlen;
    }
    if (st != RIAK_OK) {
        riak_parts_free(list, count);
        return st;
    }
    *parts = list;
    *n = count;
    return RIAK_OK;
}
