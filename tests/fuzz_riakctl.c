/*
 File:   fuzz_riakctl.c
 SPDX-License-Identifier: GPL-3.0-or-later

 libFuzzer target for the parsers of riakctl that see untrusted input:
 node URLs (command line, environment), the value encodings of backup
 files and the line reader of restore. Beyond the absence of crashes and
 sanitizer reports, round trips must hold: value -> JSON -> value and
 bytes -> base64 -> bytes, and the line reader accounts for every byte.
   cmake -B build-fuzz -DCMAKE_C_COMPILER=clang -DRIAK_FUZZ=ON
   build-fuzz/tests/fuzz_riakctl -max_total_time=300
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "riakctl.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void fuzz_urls(const char *s) {
    rc_conn c;
    memset(&c, 0, sizeof c);
    riak_config_init(&c.cfg);
    if (rc_conn_setup(&c, s) == RC_OK) {
        if (c.n_nodes == 0 || c.cfg.nodes != c.nodes || c.cfg.n_nodes != c.n_nodes) {
            abort();
        }
        for (size_t i = 0; i < c.n_nodes; i++) {
            if (c.nodes[i].host[0] == '\0' || c.nodes[i].port > 65535) {
                abort();
            }
        }
    }
    rc_conn_free(&c);
}

static void fuzz_base64_text(const char *s) {
    size_t len;
    unsigned char *dec = rc_base64_decode(s, &len);
    if (dec == NULL) {
        return;
    }
    /* whatever decoded must survive another round trip unchanged */
    char *enc = rc_base64_encode(dec, len);
    size_t len2;
    unsigned char *dec2 = enc ? rc_base64_decode(enc, &len2) : NULL;
    if (enc && (dec2 == NULL || len2 != len || memcmp(dec, dec2, len) != 0)) {
        abort();
    }
    free(dec2);
    free(enc);
    free(dec);
}

static void fuzz_bytes(const uint8_t *data, size_t size) {
    char *enc = rc_base64_encode(data, size);
    size_t len;
    unsigned char *dec = enc ? rc_base64_decode(enc, &len) : NULL;
    if (enc && (dec == NULL || len != size || (size && memcmp(dec, data, size) != 0))) {
        abort();
    }
    free(dec);
    free(enc);

    /* the form a backup stores a value in, text or base64, gives it back */
    json_object *o = rc_value_json((const char *)data, size);
    if (o) {
        const char *text = json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN);
        json_object *back = text ? json_tokener_parse(text) : NULL;
        char *v = NULL;
        size_t vlen = 0;
        if (back == NULL || !rc_value_from_json(back, &v, &vlen) || vlen != size ||
            (size && memcmp(v, data, size) != 0)) {
            abort();
        }
        free(v);
        json_object_put(back);
        json_object_put(o);
    }
}

static void fuzz_numbers(const char *s) {
    int q;
    unsigned long long v;
    char *n, *val;
    (void)rc_parse_quorum(s, &q);
    (void)rc_parse_uint(s, UINT64_MAX, &v);
    if (rc_split_pair(s, &n, &val)) {
        free(n);
        free(val);
    }
}

/* Lines read with a small limit cover the input exactly once; a line keeps
 * at most max bytes, and only a line longer than that is marked. */
static void fuzz_lines(const uint8_t *data, size_t size) {
    if (size < 1) {
        return;
    }
    size_t max = data[0] % 64;
    FILE *f = fmemopen((void *)(uintptr_t)(data + 1), size - 1, "r");
    if (f == NULL) {
        return;
    }
    char *buf = NULL;
    size_t cap = 0, total = 0;
    bool too_long;
    ssize_t n;
    while ((n = rc_read_line(f, &buf, &cap, max, &too_long)) > 0) {
        size_t kept = strlen(buf);
        if (too_long ? (size_t)n <= max || kept > max
                     : (size_t)n > max || memcmp(buf, data + 1 + total, (size_t)n) != 0) {
            abort();
        }
        total += (size_t)n;
    }
    if (total != size - 1) {
        abort();
    }
    free(buf);
    (void)fclose(f);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) {
        return 0;
    }
    uint8_t mode = data[0];
    data++;
    size--;
    char *s = malloc(size + 1);
    if (s == NULL) {
        return 0;
    }
    memcpy(s, data, size);
    s[size] = '\0';
    switch (mode % 5) {
    case 0:
        fuzz_urls(s);
        break;
    case 1:
        fuzz_base64_text(s);
        break;
    case 2:
        fuzz_bytes(data, size);
        break;
    case 3:
        fuzz_numbers(s);
        break;
    default:
        fuzz_lines(data, size);
        break;
    }
    free(s);
    return 0;
}
