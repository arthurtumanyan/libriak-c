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

static bool any_visit(const char *key, const char *term, void *ctx) {
    (void)term;
    (*(size_t *)ctx) += strlen(key) > 0;
    return true;
}

static bool count_part(void *ctx, riak_part *part, riak_status *err) {
    (void)err;
    if (part->body[part->len] != '\0') {
        abort();
    }
    (*(size_t *)ctx)++;
    return true;
}

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

    /* 2i pages, Search responses and MapReduce chunks */
    size_t visits = 0;
    char *cont = NULL;
    bool stopped;
    (void)riak_parse_index_json(s, size, any_visit, &visits, &cont, &stopped);
    free(cont);
    riak_search_result sr = {0};
    (void)riak_parse_search(s, size, &sr);
    riak_search_result_free(&sr);
    unsigned phase;
    char *mr = NULL;
    if (riak_parse_mapred_chunk(s, size, &phase, &mr) == RIAK_OK && mr == NULL) {
        abort();
    }
    free(mr);

    /* streamed multipart in odd chunks must agree with the one-shot parse */
    if (nl != NULL) {
        char *ctype = riak_strndup(s, (size_t)(nl - s));
        char *boundary = ctype ? riak_multipart_boundary(ctype) : NULL;
        if (boundary) {
            const char *body = nl + 1;
            size_t blen = size - (size_t)(nl - s) - 1;
            riak_part *parts;
            size_t n_batch = 0, n_stream = 0;
            riak_status batch = riak_multipart_parse(boundary, body, blen, &parts, &n_batch);
            if (batch == RIAK_OK) {
                riak_parts_free(parts, n_batch);
            }
            riak_mpstream *ms = riak_mpstream_new(boundary, 0, count_part, &n_stream);
            if (ms) {
                riak_status st = RIAK_OK;
                for (size_t off = 0; st == RIAK_OK && off < blen; off += step) {
                    st = riak_mpstream_feed(ms, body + off, blen - off < step ? blen - off : step);
                }
                if (st == RIAK_OK) {
                    st = riak_mpstream_finish(ms);
                }
                if ((st == RIAK_OK) != (batch == RIAK_OK) ||
                    (st == RIAK_OK && n_stream != n_batch)) {
                    abort();
                }
                riak_mpstream_free(ms);
            }
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
