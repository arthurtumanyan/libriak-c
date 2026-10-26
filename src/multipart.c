/*
 File:   multipart.c
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later

 One incremental parser serves both complete bodies (siblings) and
 streamed responses (2i ?stream=true, MapReduce ?chunked=true).
 */

#include "multipart.h"

#include <stdint.h>
#include <stdio.h>
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

enum mp_state { MP_PREAMBLE, MP_AFTER_DELIM, MP_PART, MP_DONE };

struct riak_mpstream {
    char delim[4 + 70 + 1]; /* "\r\n--" + boundary */
    size_t dlen;
    size_t max_part;
    riak_part_fn fn;
    void *ctx;
    riak_buf buf;
    size_t pos;   /* consumed prefix of buf */
    bool skipped; /* preamble bytes were dropped: no longer at body start */
    enum mp_state state;
};

riak_mpstream *riak_mpstream_new(const char *boundary, size_t max_part, riak_part_fn fn,
                                 void *ctx) {
    size_t blen = boundary ? strlen(boundary) : 0;
    if (blen == 0 || blen > 70) {
        return NULL;
    }
    riak_mpstream *s = calloc(1, sizeof *s);
    if (s == NULL) {
        return NULL;
    }
    (void)snprintf(s->delim, sizeof s->delim, "\r\n--%s", boundary);
    s->dlen = 4 + blen;
    s->max_part = max_part ? max_part : SIZE_MAX;
    s->fn = fn;
    s->ctx = ctx;
    return s;
}

void riak_mpstream_free(riak_mpstream *s) {
    if (s) {
        riak_buf_free(&s->buf);
        free(s);
    }
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

/* Build a part from [p, next) and hand it to the callback. */
static riak_status emit(riak_mpstream *s, const char *p, const char *next) {
    const char *hdr_end;
    const char *data;
    if (next == p) {
        hdr_end = data = p; /* empty part */
    } else if (p[0] == '\r' && p[1] == '\n') {
        hdr_end = p; /* no headers */
        data = p + 2;
    } else {
        const char *sep = find(p, (size_t)(next - p), "\r\n\r\n", 4);
        hdr_end = sep ? sep : next;
        data = sep ? sep + 4 : next;
    }
    riak_part part = {0};
    part.len = (size_t)(next - data);
    part.body = riak_strndup(data, part.len);
    riak_status st = part.body ? parse_headers(&part.headers, p, hdr_end) : RIAK_E_NOMEM;
    if (st == RIAK_OK && s->fn && !s->fn(s->ctx, &part, &st)) {
        if (st == RIAK_OK) {
            st = RIAK_E_PARSE;
        }
    }
    riak_headerlist_clear(&part.headers);
    free(part.body);
    return st;
}

static riak_status step(riak_mpstream *s) {
    for (;;) {
        const char *base = s->buf.data ? s->buf.data + s->pos : "";
        size_t avail = s->buf.len - s->pos;
        switch (s->state) {
        case MP_PREAMBLE: {
            /* the first delimiter may also start the body (no leading CRLF) */
            if (!s->skipped && avail >= s->dlen - 2 &&
                memcmp(base, s->delim + 2, s->dlen - 2) == 0) {
                s->pos += s->dlen - 2;
                s->state = MP_AFTER_DELIM;
                break;
            }
            if (!s->skipped && avail < s->dlen - 2 && memcmp(base, s->delim + 2, avail) == 0) {
                return RIAK_OK; /* could still become a leading delimiter */
            }
            const char *d = find(base, avail, s->delim, s->dlen);
            if (d == NULL) {
                /* drop preamble but keep a possible partial delimiter */
                if (avail >= s->dlen) {
                    s->pos += avail - (s->dlen - 1);
                    s->skipped = true;
                }
                return RIAK_OK;
            }
            s->pos += (size_t)(d - base) + s->dlen;
            s->state = MP_AFTER_DELIM;
            break;
        }
        case MP_AFTER_DELIM: {
            size_t i = 0;
            if (avail >= 2 && base[0] == '-' && base[1] == '-') {
                s->state = MP_DONE;
                return RIAK_OK;
            }
            if (avail == 1 && base[0] == '-') {
                return RIAK_OK;
            }
            while (i < avail && (base[i] == ' ' || base[i] == '\t'))
                i++;
            if (avail - i < 2) {
                if (avail - i == 1 && base[i] != '\r') {
                    return RIAK_E_PARSE;
                }
                return RIAK_OK;
            }
            if (base[i] != '\r' || base[i + 1] != '\n') {
                return RIAK_E_PARSE;
            }
            s->pos += i + 2;
            s->state = MP_PART;
            break;
        }
        case MP_PART: {
            const char *next = find(base, avail, s->delim, s->dlen);
            if (next == NULL) {
                return avail > s->max_part ? RIAK_E_HTTP : RIAK_OK;
            }
            riak_status st = emit(s, base, next);
            if (st != RIAK_OK) {
                return st;
            }
            s->pos += (size_t)(next - base) + s->dlen;
            s->state = MP_AFTER_DELIM;
            break;
        }
        case MP_DONE:
            return RIAK_OK; /* epilogue is ignored */
        }
    }
}

riak_status riak_mpstream_feed(riak_mpstream *s, const char *data, size_t len) {
    if (s->state == MP_DONE) {
        return RIAK_OK;
    }
    if (riak_buf_append(&s->buf, data, len) != 0) {
        return RIAK_E_NOMEM;
    }
    riak_status st = step(s);
    /* compact once the consumed prefix dominates the buffer */
    if (s->pos > 0 && s->pos >= s->buf.len / 2) {
        memmove(s->buf.data, s->buf.data + s->pos, s->buf.len - s->pos);
        s->buf.len -= s->pos;
        s->buf.data[s->buf.len] = '\0';
        s->pos = 0;
    }
    return st;
}

riak_status riak_mpstream_finish(riak_mpstream *s) {
    return s->state == MP_DONE ? RIAK_OK : RIAK_E_PARSE;
}

typedef struct {
    riak_part *list;
    size_t count;
    size_t cap;
} collector;

static bool collect(void *ctx, riak_part *part, riak_status *err) {
    collector *c = ctx;
    if (c->count == c->cap) {
        size_t ncap = c->cap ? c->cap * 2 : 4;
        riak_part *grown = ncap <= SIZE_MAX / sizeof(riak_part)
                               ? realloc(c->list, ncap * sizeof(riak_part))
                               : NULL;
        if (grown == NULL) {
            *err = RIAK_E_NOMEM;
            return false;
        }
        c->list = grown;
        c->cap = ncap;
    }
    c->list[c->count++] = *part; /* take ownership */
    memset(part, 0, sizeof *part);
    return true;
}

riak_status riak_multipart_parse(const char *boundary, const char *body, size_t len,
                                 riak_part **parts, size_t *n) {
    *parts = NULL;
    *n = 0;
    collector c = {0};
    riak_mpstream *s = riak_mpstream_new(boundary, 0, collect, &c);
    if (s == NULL) {
        return boundary && *boundary && strlen(boundary) <= 70 ? RIAK_E_NOMEM : RIAK_E_PARSE;
    }
    riak_status st = riak_mpstream_feed(s, body, len);
    if (st == RIAK_OK) {
        st = riak_mpstream_finish(s);
    }
    riak_mpstream_free(s);
    if (st != RIAK_OK) {
        riak_parts_free(c.list, c.count);
        return st;
    }
    *parts = c.list;
    *n = c.count;
    return RIAK_OK;
}
