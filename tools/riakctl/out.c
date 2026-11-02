/*
 File:   out.c
 SPDX-License-Identifier: GPL-3.0-or-later

 Output, error reporting and conversions between library results and JSON.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "riakctl.h"

int rc_exit_code(riak_status st) {
    switch (st) {
    case RIAK_OK:
        return RC_OK;
    case RIAK_E_INVALID_ARG:
        return RC_USAGE;
    case RIAK_E_NOT_FOUND:
        return RC_NOT_FOUND;
    case RIAK_E_SIBLINGS:
        return RC_SIBLINGS;
    case RIAK_E_PRECONDITION:
        return RC_PRECONDITION;
    case RIAK_E_UNAUTHORIZED:
        return RC_UNAUTHORIZED;
    case RIAK_E_TRANSPORT:
    case RIAK_E_TLS:
        return RC_UNREACHABLE;
    case RIAK_E_UNSUPPORTED:
        return RC_UNSUPPORTED;
    default:
        return RC_ERROR;
    }
}

int rc_fail(riak_client *c, const char *what, riak_status st) {
    /* the detail may end with a server response body: one line is kept */
    char detail[512];
    snprintf(detail, sizeof detail, "%s", c ? riak_client_errmsg(c) : "");
    for (char *p = detail; *p; p++) {
        if (*p == '\n' || *p == '\r' || *p == '\t') {
            *p = ' ';
        }
    }
    size_t dl = strlen(detail);
    while (dl > 0 && detail[dl - 1] == ' ') {
        detail[--dl] = '\0';
    }
    if (dl && strcmp(detail, riak_strerror(st)) != 0) {
        fprintf(stderr, "riakctl: %s: %s (%s)\n", what, riak_strerror(st), detail);
    } else {
        fprintf(stderr, "riakctl: %s: %s\n", what, riak_strerror(st));
    }
    if (st == RIAK_E_SIBLINGS) {
        fprintf(stderr, "riakctl: the siblings are shown with get --all\n");
    }
    return rc_exit_code(st);
}

int rc_usage_error(const char *cmd, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)fprintf(stderr, "riakctl %s: ", cmd);
    (void)vfprintf(stderr, fmt, ap);
    (void)fprintf(stderr, "\nriakctl help %s shows the usage\n", cmd);
    va_end(ap);
    return RC_USAGE;
}

void rc_warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)fputs("riakctl: ", stderr);
    (void)vfprintf(stderr, fmt, ap);
    (void)fputc('\n', stderr);
    va_end(ap);
}

/* json-c's flags are int macros */
static int print_flags(int extra) {
    unsigned f = (unsigned)extra;
#ifdef JSON_C_TO_STRING_NOSLASHESCAPE
    f |= (unsigned)JSON_C_TO_STRING_NOSLASHESCAPE;
#endif
    return (int)f;
}

static int put_json(json_object *o, int flags) {
    const char *s = json_object_to_json_string_ext(o, print_flags(flags));
    int rc = s && fputs(s, stdout) >= 0 && fputc('\n', stdout) != EOF ? RC_OK : RC_ERROR;
    json_object_put(o);
    if (rc != RC_OK) {
        fprintf(stderr, "riakctl: write error: %s\n", strerror(errno));
    }
    return rc;
}

int rc_print(rc_ctx *x, json_object *o) {
    if (o == NULL) {
        fprintf(stderr, "riakctl: out of memory\n");
        return RC_ERROR;
    }
    unsigned pretty = (unsigned)JSON_C_TO_STRING_PRETTY | (unsigned)JSON_C_TO_STRING_SPACED;
    return put_json(o, x->pretty ? (int)pretty : JSON_C_TO_STRING_PLAIN);
}

int rc_print_line(json_object *o) {
    if (o == NULL) {
        fprintf(stderr, "riakctl: out of memory\n");
        return RC_ERROR;
    }
    return put_json(o, JSON_C_TO_STRING_PLAIN);
}

json_object *rc_parse_json(const char *text, const char *what) {
    enum json_tokener_error err = json_tokener_success;
    json_object *o = json_tokener_parse_verbose(text, &err);
    if (o == NULL) {
        fprintf(stderr, "riakctl: %s: invalid JSON (%s)\n", what, json_tokener_error_desc(err));
    }
    return o;
}

/* ---- base64 ------------------------------------------------------------- */

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char b64_char(uint32_t v) {
    return b64[v & 63U];
}

char *rc_base64_encode(const unsigned char *data, size_t len) {
    char *out = malloc((len + 2) / 3 * 4 + 1);
    if (out == NULL) {
        return NULL;
    }
    char *p = out;
    for (size_t i = 0; i < len; i += 3) {
        size_t left = len - i;
        uint32_t v = (uint32_t)data[i] << 16U;
        if (left > 1) {
            v |= (uint32_t)data[i + 1] << 8U;
        }
        if (left > 2) {
            v |= data[i + 2];
        }
        *p++ = b64_char(v >> 18U);
        *p++ = b64_char(v >> 12U);
        *p++ = left > 1 ? b64_char(v >> 6U) : '=';
        *p++ = left > 2 ? b64_char(v) : '=';
    }
    *p = '\0';
    return out;
}

/* Value of a base64 character, -1 for other characters. */
static int b64_value(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A';
    }
    if (ch >= 'a' && ch <= 'z') {
        return ch - 'a' + 26;
    }
    if (ch >= '0' && ch <= '9') {
        return ch - '0' + 52;
    }
    return ch == '+' ? 62 : ch == '/' ? 63 : -1;
}

unsigned char *rc_base64_decode(const char *text, size_t *len) {
    size_t n = strlen(text);
    size_t cap = n / 4 * 3 + 3;
    unsigned char *out = malloc(cap);
    if (out == NULL) {
        return NULL;
    }
    uint32_t acc = 0;
    unsigned bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < n && text[i] != '='; i++) {
        int v = b64_value(text[i]);
        if (v < 0 || o >= cap) {
            free(out);
            return NULL;
        }
        acc = (acc << 6U) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (unsigned char)((acc >> bits) & 0xFFU);
        }
    }
    *len = o;
    return out;
}

/* ---- values and objects ------------------------------------------------- */

/* Length of the UTF-8 sequence starting with c (0: invalid lead byte), and
 * the payload bits of c. */
static size_t utf8_lead(unsigned c, uint32_t *cp) {
    if (c >= 0x01U && c < 0x80U) {
        *cp = c;
        return 1;
    }
    if ((c & 0xE0U) == 0xC0U) {
        *cp = c & 0x1FU;
        return 2;
    }
    if ((c & 0xF0U) == 0xE0U) {
        *cp = c & 0x0FU;
        return 3;
    }
    if ((c & 0xF8U) == 0xF0U) {
        *cp = c & 0x07U;
        return 4;
    }
    return 0; /* NUL bytes are rejected too: values with them go to base64 */
}

static bool valid_utf8(const unsigned char *s, size_t len) {
    static const uint32_t min_cp[] = {0, 0, 0x80, 0x800, 0x10000};
    size_t i = 0;
    while (i < len) {
        uint32_t cp;
        size_t n = utf8_lead(s[i], &cp);
        if (n == 0 || n > len - i) {
            return false;
        }
        for (size_t k = 1; k < n; k++) {
            unsigned c = s[i + k];
            if ((c & 0xC0U) != 0x80U) {
                return false;
            }
            cp = (cp << 6U) | (c & 0x3FU);
        }
        if (cp < min_cp[n] || cp > 0x10FFFFU || (cp >= 0xD800U && cp <= 0xDFFFU)) {
            return false;
        }
        i += n;
    }
    return true;
}

/* {"value": text} for UTF-8 data, {"value_b64": base64} otherwise; the
 * caller adds the members of the returned object to its own. */
json_object *rc_value_json(const char *data, size_t len) {
    json_object *o = json_object_new_object();
    if (o == NULL) {
        return NULL;
    }
    if (data == NULL) {
        data = "";
        len = 0;
    }
    if (valid_utf8((const unsigned char *)data, len)) {
        json_object_object_add(o, "value", json_object_new_string_len(data, (int)len));
    } else {
        char *enc = rc_base64_encode((const unsigned char *)data, len);
        if (enc == NULL) {
            json_object_put(o);
            return NULL;
        }
        json_object_object_add(o, "value_b64", json_object_new_string(enc));
        free(enc);
    }
    return o;
}

bool rc_value_from_json(json_object *o, char **data, size_t *len) {
    json_object *v;
    if (json_object_object_get_ex(o, "value_b64", &v) && json_object_is_type(v, json_type_string)) {
        unsigned char *d = rc_base64_decode(json_object_get_string(v), len);
        *data = (char *)d;
        return d != NULL;
    }
    if (json_object_object_get_ex(o, "value", &v) && json_object_is_type(v, json_type_string)) {
        *len = (size_t)json_object_get_string_len(v);
        *data = malloc(*len + 1);
        if (*data == NULL) {
            return false;
        }
        memcpy(*data, json_object_get_string(v), *len + 1);
        return true;
    }
    *data = NULL;
    *len = 0;
    return true;
}

static void add_str(json_object *o, const char *name, const char *value) {
    if (value) {
        json_object_object_add(o, name, json_object_new_string(value));
    }
}

json_object *rc_object_json(const riak_object *obj, bool with_value) {
    json_object *o = with_value ? rc_value_json(obj->data, obj->len) : json_object_new_object();
    if (o == NULL) {
        return NULL;
    }
    add_str(o, "content_type", obj->content_type);
    add_str(o, "last_modified", obj->last_modified);
    if (obj->deleted) {
        json_object_object_add(o, "deleted", json_object_new_boolean(1));
    }
    if (obj->n_meta) {
        json_object *m = json_object_new_object();
        for (size_t i = 0; i < obj->n_meta; i++) {
            json_object_object_add(m, obj->meta[i].name,
                                   json_object_new_string(obj->meta[i].value));
        }
        json_object_object_add(o, "meta", m);
    }
    if (obj->n_indexes) {
        /* {"name_bin": ["v1", "v2"]}: an index may hold several values */
        json_object *ix = json_object_new_object();
        for (size_t i = 0; i < obj->n_indexes; i++) {
            json_object *vals;
            if (!json_object_object_get_ex(ix, obj->indexes[i].name, &vals)) {
                vals = json_object_new_array();
                json_object_object_add(ix, obj->indexes[i].name, vals);
            }
            json_object_array_add(vals, json_object_new_string(obj->indexes[i].value));
        }
        json_object_object_add(o, "indexes", ix);
    }
    add_str(o, "links", obj->links);
    return o;
}

static const char *const dt_names[] = {"counter", "set", "map", "hll", "gset", "register", "flag"};

const char *rc_dt_name(riak_dt_type t) {
    return (unsigned)t < sizeof dt_names / sizeof *dt_names ? dt_names[t] : "unknown";
}

bool rc_dt_from_name(const char *name, riak_dt_type *t) {
    for (size_t i = 0; i < sizeof dt_names / sizeof *dt_names; i++) {
        if (strcmp(name, dt_names[i]) == 0) {
            *t = (riak_dt_type)i;
            return true;
        }
    }
    return false;
}

/* Counters and HyperLogLogs become numbers, sets arrays, registers strings,
 * flags booleans; map fields are named "<field>_<type>", as in Riak's HTTP API. */
json_object *rc_dt_json(const riak_dt_value *v) {
    switch (v->type) {
    case RIAK_DT_COUNTER:
    case RIAK_DT_HLL:
        return json_object_new_int64(v->counter);
    case RIAK_DT_SET:
    case RIAK_DT_GSET: {
        json_object *a = json_object_new_array();
        for (size_t i = 0; a && i < v->n_elements; i++) {
            json_object_array_add(a, json_object_new_string(v->elements[i]));
        }
        return a;
    }
    case RIAK_DT_REGISTER:
        return json_object_new_string(v->reg ? v->reg : "");
    case RIAK_DT_FLAG:
        return json_object_new_boolean(v->flag);
    case RIAK_DT_MAP: {
        json_object *m = json_object_new_object();
        for (size_t i = 0; m && i < v->n_entries; i++) {
            const riak_dt_entry *e = &v->entries[i];
            char name[1024];
            snprintf(name, sizeof name, "%s_%s", e->name, rc_dt_name(e->value.type));
            json_object_object_add(m, name, rc_dt_json(&e->value));
        }
        return m;
    }
    }
    return NULL;
}

/* ---- input -------------------------------------------------------------- */

bool rc_read_all(FILE *f, char **data, size_t *len) {
    size_t cap = 65536;
    size_t n = 0;
    char *buf = malloc(cap);
    if (buf == NULL) {
        return false;
    }
    for (;;) {
        if (cap - n < 2) {
            char *nb = realloc(buf, cap * 2);
            if (nb == NULL) {
                free(buf);
                return false;
            }
            buf = nb;
            cap *= 2;
        }
        size_t want = cap - n - 1;
        size_t r = fread(buf + n, 1, want, f);
        n += r;
        if (r < want) {
            break; /* end of input or an error */
        }
    }
    if (ferror(f)) {
        free(buf);
        return false;
    }
    buf[n] = '\0';
    *data = buf;
    *len = n;
    return true;
}

char *rc_read_file(const char *path, size_t *len) {
    FILE *f = strcmp(path, "-") == 0 ? stdin : fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "riakctl: %s: %s\n", path, strerror(errno));
        return NULL;
    }
    char *data = NULL;
    if (!rc_read_all(f, &data, len)) {
        fprintf(stderr, "riakctl: cannot read %s\n", path);
    }
    if (f != stdin) {
        (void)fclose(f); /* read-only: nothing is lost */
    }
    return data;
}
