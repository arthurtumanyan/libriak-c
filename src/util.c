/*
 File:   util.c
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "util.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int buf_reserve(riak_buf *b, size_t extra) {
    if (extra > SIZE_MAX - b->len - 1) {
        return -1;
    }
    size_t need = b->len + extra + 1;
    if (need <= b->cap) {
        return 0;
    }
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need) {
        cap = (cap > SIZE_MAX / 2) ? need : cap * 2;
    }
    char *p = realloc(b->data, cap);
    if (p == NULL) {
        return -1;
    }
    b->data = p;
    b->cap = cap;
    return 0;
}

int riak_buf_append(riak_buf *b, const void *data, size_t len) {
    if (buf_reserve(b, len) != 0 || b->data == NULL) {
        return -1;
    }
    if (len > 0) {
        memcpy(b->data + b->len, data, len);
    }
    b->len += len;
    b->data[b->len] = '\0';
    return 0;
}

int riak_buf_append_str(riak_buf *b, const char *s) {
    return riak_buf_append(b, s, strlen(s));
}

char *riak_buf_steal(riak_buf *b, size_t *len) {
    if (b->data == NULL && riak_buf_append(b, "", 0) != 0) {
        return NULL;
    }
    char *p = b->data;
    if (len) {
        *len = b->len;
    }
    b->data = NULL;
    b->len = b->cap = 0;
    return p;
}

void riak_buf_free(riak_buf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

char *riak_strndup(const char *s, size_t n) {
    char *p = malloc(n + 1);
    if (p == NULL) {
        return NULL;
    }
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

char *riak_strdup(const char *s) {
    return s ? riak_strndup(s, strlen(s)) : NULL;
}

char *riak_vstrprintf(const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) {
        return NULL;
    }
    char *p = malloc((size_t)n + 1);
    if (p == NULL) {
        return NULL;
    }
    (void)vsnprintf(p, (size_t)n + 1, fmt, ap);
    return p;
}

char *riak_strprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *p = riak_vstrprintf(fmt, ap);
    va_end(ap);
    return p;
}

static bool is_unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '.' || c == '_' || c == '~';
}

int riak_url_append_escaped(riak_buf *b, const char *s) {
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (is_unreserved(*p)) {
            if (riak_buf_append(b, p, 1) != 0) {
                return -1;
            }
        } else {
            char enc[3] = {'%', hex[*p >> 4U], hex[*p & 0x0FU]};
            if (riak_buf_append(b, enc, sizeof enc) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

int riak_url_append_query(riak_buf *b, const char *name, const char *value) {
    const char *sep = (b->data && strchr(b->data, '?')) ? "&" : "?";
    if (riak_buf_append_str(b, sep) != 0 || riak_url_append_escaped(b, name) != 0 ||
        riak_buf_append_str(b, "=") != 0 || riak_url_append_escaped(b, value) != 0) {
        return -1;
    }
    return 0;
}

int riak_url_append_quorum(riak_buf *b, const char *name, int q) {
    char num[16];
    const char *v;
    switch (q) {
    case RIAK_Q_DEFAULT:
        return 0;
    case RIAK_Q_ONE:
        v = "one";
        break;
    case RIAK_Q_QUORUM:
        v = "quorum";
        break;
    case RIAK_Q_ALL:
        v = "all";
        break;
    default:
        if (q < 0) {
            return -1;
        }
        snprintf(num, sizeof num, "%d", q);
        v = num;
        break;
    }
    return riak_url_append_query(b, name, v);
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char *riak_url_unescape(const char *s, size_t len) {
    char *out = malloc(len + 1);
    if (out == NULL) {
        return NULL;
    }
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '%') {
            int hi = (i + 2 < len) ? hexval(s[i + 1]) : -1;
            int lo = (i + 2 < len) ? hexval(s[i + 2]) : -1;
            if (hi < 0 || lo < 0) {
                free(out);
                return NULL;
            }
            out[o++] = (char)(((unsigned)hi << 4U) | (unsigned)lo);
            i += 2;
        } else {
            out[o++] = s[i];
        }
    }
    out[o] = '\0';
    return out;
}

bool riak_header_value_ok(const char *v) {
    return v != NULL && strpbrk(v, "\r\n") == NULL;
}

bool riak_header_name_ok(const char *n) {
    if (n == NULL || *n == '\0') {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)n; *p; p++) {
        if (!is_unreserved(*p) && strchr("!#$%&'*+^`|", *p) == NULL) {
            return false;
        }
    }
    return true;
}

int riak_strlist_push(riak_strlist *l, size_t *cap, const char *s, size_t len) {
    /* keep room for the terminating NULL */
    if (l->count + 1 >= *cap) {
        size_t ncap = *cap ? *cap * 2 : 16;
        if (ncap > SIZE_MAX / sizeof(char *)) {
            return -1;
        }
        char **items = (char **)realloc((void *)l->items, ncap * sizeof(char *));
        if (items == NULL) {
            return -1;
        }
        l->items = items;
        *cap = ncap;
    }
    char *copy = riak_strndup(s, len);
    if (copy == NULL) {
        return -1;
    }
    l->items[l->count++] = copy;
    l->items[l->count] = NULL;
    return 0;
}

void riak_strlist_free(riak_strlist *list) {
    if (list == NULL) {
        return;
    }
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i]);
    }
    free((void *)list->items);
    list->items = NULL;
    list->count = 0;
}

static void free_headers(riak_header *h, size_t n) {
    for (size_t i = 0; i < n; i++) {
        free(h[i].name);
        free(h[i].value);
    }
    free(h);
}

void riak_object_free(riak_object *obj) {
    if (obj == NULL) {
        return;
    }
    free(obj->data);
    free(obj->content_type);
    free(obj->vclock);
    free(obj->etag);
    free(obj->last_modified);
    free_headers(obj->meta, obj->n_meta);
    free_headers(obj->indexes, obj->n_indexes);
    free(obj->links);
    memset(obj, 0, sizeof *obj);
}

void riak_object_set_free(riak_object_set *set) {
    if (set == NULL) {
        return;
    }
    for (size_t i = 0; i < set->count; i++) {
        riak_object_free(&set->items[i]);
    }
    free(set->items);
    free(set->vclock);
    memset(set, 0, sizeof *set);
}

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

int riak_headerlist_add_line(riak_headerlist *l, const char *line, size_t len) {
    const char *colon = memchr(line, ':', len);
    if (colon == NULL || colon == line) {
        return 1;
    }
    const char *v = colon + 1;
    const char *end = line + len;
    while (v < end && is_space(*v))
        v++;
    while (end > v && is_space(end[-1]))
        end--;
    size_t nlen = (size_t)(colon - line);
    while (nlen > 0 && is_space(line[nlen - 1]))
        nlen--;
    if (nlen == 0) {
        return 1;
    }
    if (l->count == l->cap) {
        size_t ncap = l->cap ? l->cap * 2 : 16;
        if (ncap > SIZE_MAX / sizeof(riak_header)) {
            return -1;
        }
        riak_header *items = realloc(l->items, ncap * sizeof(riak_header));
        if (items == NULL) {
            return -1;
        }
        l->items = items;
        l->cap = ncap;
    }
    char *name = riak_strndup(line, nlen);
    char *value = riak_strndup(v, (size_t)(end - v));
    if (name == NULL || value == NULL) {
        free(name);
        free(value);
        return -1;
    }
    l->items[l->count].name = name;
    l->items[l->count].value = value;
    l->count++;
    return 0;
}

const char *riak_headerlist_get(const riak_headerlist *l, const char *name) {
    for (size_t i = 0; i < l->count; i++) {
        if (strcasecmp(l->items[i].name, name) == 0) {
            return l->items[i].value;
        }
    }
    return NULL;
}

void riak_headerlist_clear(riak_headerlist *l) {
    free_headers(l->items, l->count);
    l->items = NULL;
    l->count = l->cap = 0;
}

static int copy_header(char **dst, const riak_headerlist *h, const char *name) {
    const char *v = riak_headerlist_get(h, name);
    if (v == NULL) {
        return 0;
    }
    *dst = riak_strdup(v);
    return *dst ? 0 : -1;
}

/* Collect headers named <prefix><rest> as {rest, value}. */
static int collect_prefixed(const riak_headerlist *h, const char *prefix, riak_header **out,
                            size_t *n) {
    size_t plen = strlen(prefix);
    size_t count = 0;
    for (size_t i = 0; i < h->count; i++) {
        if (strncasecmp(h->items[i].name, prefix, plen) == 0 && h->items[i].name[plen]) {
            count++;
        }
    }
    if (count == 0) {
        return 0;
    }
    riak_header *items = calloc(count, sizeof *items);
    if (items == NULL) {
        return -1;
    }
    *out = items;
    size_t j = 0;
    for (size_t i = 0; i < h->count && j < count; i++) {
        const riak_header *src = &h->items[i];
        if (strncasecmp(src->name, prefix, plen) != 0 || src->name[plen] == '\0') {
            continue;
        }
        items[j].name = riak_strdup(src->name + plen);
        items[j].value = riak_strdup(src->value);
        *n = ++j;
        if (items[j - 1].name == NULL || items[j - 1].value == NULL) {
            return -1;
        }
    }
    return 0;
}

static int join_links(const riak_headerlist *h, char **out) {
    riak_buf b = {0};
    for (size_t i = 0; i < h->count; i++) {
        if (strcasecmp(h->items[i].name, "Link") != 0) {
            continue;
        }
        if ((b.len && riak_buf_append_str(&b, ", ") != 0) ||
            riak_buf_append_str(&b, h->items[i].value) != 0) {
            riak_buf_free(&b);
            return -1;
        }
    }
    *out = b.data;
    return 0;
}

int riak_object_from_headers(const riak_headerlist *h, const char *vclock, riak_object *obj) {
    if (copy_header(&obj->content_type, h, "Content-Type") != 0 ||
        copy_header(&obj->etag, h, "ETag") != 0 ||
        copy_header(&obj->last_modified, h, "Last-Modified") != 0 ||
        collect_prefixed(h, "X-Riak-Meta-", &obj->meta, &obj->n_meta) != 0 ||
        collect_prefixed(h, "X-Riak-Index-", &obj->indexes, &obj->n_indexes) != 0 ||
        join_links(h, &obj->links) != 0) {
        return -1;
    }
    if (vclock) {
        obj->vclock = riak_strdup(vclock);
        if (obj->vclock == NULL) {
            return -1;
        }
    } else if (copy_header(&obj->vclock, h, "X-Riak-Vclock") != 0) {
        return -1;
    }
    const char *del = riak_headerlist_get(h, "X-Riak-Deleted");
    obj->deleted = del != NULL && strcasecmp(del, "true") == 0;
    return 0;
}

void riak_free(void *ptr) {
    free(ptr);
}
