/*
 File:   fuzz_parse.c
 SPDX-License-Identifier: GPL-3.0-or-later

 libFuzzer target for the response parsers and URL decoding:
   cmake -B build-fuzz -DCMAKE_C_COMPILER=clang -DRIAK_FUZZ=ON
   build-fuzz/tests/fuzz_parse -max_total_time=300
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "multipart.h"
#include "parse.h"
#include "util.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    const char *s = (const char *)data;

    riak_strlist l;
    if (riak_parse_string_array(s, size, "keys", &l) == RIAK_OK) {
        if (l.items[l.count] != NULL) {
            abort();
        }
        riak_strlist_free(&l);
    }

    /* first byte picks the chunk size so objects get split everywhere */
    size_t step = size ? (size_t)data[0] % 17 + 1 : 1;
    riak_keystream *ks = riak_keystream_new();
    if (ks != NULL) {
        riak_status st = RIAK_OK;
        for (size_t off = 0; st == RIAK_OK && off < size; off += step) {
            size_t n = size - off < step ? size - off : step;
            st = riak_keystream_feed(ks, s + off, n);
        }
        if (st == RIAK_OK && riak_keystream_finish(ks, &l) == RIAK_OK) {
            if (l.items[l.count] != NULL) {
                abort();
            }
            riak_strlist_free(&l);
        }
        riak_keystream_free(ks);
    }

    free(riak_url_unescape(s, size));

    /* multipart: boundary from the first line, body from the rest */
    const char *nl = memchr(s, '\n', size);
    if (nl != NULL) {
        char *ctype = riak_strndup(s, (size_t)(nl - s));
        char *boundary = ctype ? riak_multipart_boundary(ctype) : NULL;
        riak_part *parts;
        size_t n;
        if (boundary && riak_multipart_parse(boundary, nl + 1, size - (size_t)(nl - s) - 1, &parts,
                                             &n) == RIAK_OK) {
            for (size_t i = 0; i < n; i++) {
                riak_object obj = {0};
                if (parts[i].body[parts[i].len] != '\0' ||
                    riak_object_from_headers(&parts[i].headers, NULL, &obj) != 0) {
                    abort();
                }
                riak_object_free(&obj);
            }
            riak_parts_free(parts, n);
        }
        free(boundary);
        free(ctype);
    }

    /* escaping then unescaping must round-trip */
    char *z = riak_strndup(s, size);
    if (z != NULL) {
        riak_buf b = {0};
        if (riak_url_append_escaped(&b, z) == 0) {
            char *back = riak_url_unescape(b.data ? b.data : "", b.len);
            if (back == NULL || strcmp(back, z) != 0) {
                abort();
            }
            free(back);
        }
        riak_buf_free(&b);
        free(z);
    }
    return 0;
}
