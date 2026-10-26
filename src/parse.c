/*
 File:   parse.c
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "parse.h"

#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "util.h"

struct riak_keystream {
    json_tokener *tok;
    riak_strlist list;
    size_t cap;
    bool pending; /* an object has started but not finished */
};

/* Append every string of obj[field] to out. */
static riak_status collect(json_object *obj, const char *field, riak_strlist *out, size_t *cap) {
    json_object *arr;
    if (!json_object_is_type(obj, json_type_object) ||
        !json_object_object_get_ex(obj, field, &arr) ||
        !json_object_is_type(arr, json_type_array)) {
        return RIAK_E_PARSE;
    }
    size_t n = json_object_array_length(arr);
    for (size_t i = 0; i < n; i++) {
        json_object *item = json_object_array_get_idx(arr, i);
        if (!json_object_is_type(item, json_type_string)) {
            return RIAK_E_PARSE;
        }
        const char *s = json_object_get_string(item);
        size_t len = (size_t)json_object_get_string_len(item);
        if (riak_strlist_push(out, cap, s, len) != 0) {
            return RIAK_E_NOMEM;
        }
    }
    return RIAK_OK;
}

static riak_status ensure_terminated(riak_strlist *out, size_t *cap) {
    if (out->items != NULL) {
        return RIAK_OK;
    }
    out->items = (char **)calloc(1, sizeof(char *));
    *cap = 1;
    return out->items ? RIAK_OK : RIAK_E_NOMEM;
}

riak_status riak_parse_string_array(const char *json, size_t len, const char *field,
                                    riak_strlist *out) {
    memset(out, 0, sizeof *out);
    if (len > INT_MAX) {
        return RIAK_E_PARSE;
    }
    json_tokener *tok = json_tokener_new();
    if (tok == NULL) {
        return RIAK_E_NOMEM;
    }
    json_object *obj = json_tokener_parse_ex(tok, json, (int)len);
    riak_status st = RIAK_E_PARSE;
    size_t cap = 0;
    if (obj != NULL && json_tokener_get_error(tok) == json_tokener_success) {
        st = collect(obj, field, out, &cap);
        if (st == RIAK_OK) {
            st = ensure_terminated(out, &cap);
        }
    }
    json_object_put(obj);
    json_tokener_free(tok);
    if (st != RIAK_OK) {
        riak_strlist_free(out);
    }
    return st;
}

riak_keystream *riak_keystream_new(void) {
    riak_keystream *ks = calloc(1, sizeof *ks);
    if (ks == NULL) {
        return NULL;
    }
    ks->tok = json_tokener_new();
    if (ks->tok == NULL) {
        free(ks);
        return NULL;
    }
    return ks;
}

static bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

riak_status riak_keystream_feed(riak_keystream *ks, const char *data, size_t len) {
    while (len > 0) {
        if (!ks->pending) {
            while (len > 0 && is_ws(*data)) {
                data++;
                len--;
            }
            if (len == 0) {
                break;
            }
        }
        int slice = len > INT_MAX ? INT_MAX : (int)len;
        json_object *obj = json_tokener_parse_ex(ks->tok, data, slice);
        enum json_tokener_error err = json_tokener_get_error(ks->tok);
        if (err == json_tokener_continue) {
            ks->pending = true;
            data += slice;
            len -= (size_t)slice;
            continue;
        }
        if (err != json_tokener_success || obj == NULL) {
            json_object_put(obj);
            return RIAK_E_PARSE;
        }
        size_t used = json_tokener_get_parse_end(ks->tok);
        json_tokener_reset(ks->tok);
        ks->pending = false;
        riak_status st = collect(obj, "keys", &ks->list, &ks->cap);
        json_object_put(obj);
        if (st != RIAK_OK) {
            return st;
        }
        data += used;
        len -= used;
    }
    return RIAK_OK;
}

riak_status riak_keystream_finish(riak_keystream *ks, riak_strlist *out) {
    if (ks->pending) {
        return RIAK_E_PARSE;
    }
    riak_status st = ensure_terminated(&ks->list, &ks->cap);
    if (st != RIAK_OK) {
        return st;
    }
    *out = ks->list;
    memset(&ks->list, 0, sizeof ks->list);
    ks->cap = 0;
    return RIAK_OK;
}

void riak_keystream_free(riak_keystream *ks) {
    if (ks == NULL) {
        return;
    }
    json_tokener_free(ks->tok);
    riak_strlist_free(&ks->list);
    free(ks);
}
