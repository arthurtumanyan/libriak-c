/*
 File:   fuzz_pb.c
 SPDX-License-Identifier: GPL-3.0-or-later

 libFuzzer target for the decoding of Protocol Buffers responses into
 library results (objects, siblings, put replies, error messages) and for
 the base64 decoding of vector clocks and contexts:
   cmake -B build-fuzz -DCMAKE_C_COMPILER=clang -DRIAK_FUZZ=ON
   build-fuzz/tests/fuzz_pb -max_total_time=300
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pb.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static size_t pick_first(const riak_object *siblings, size_t count, void *ctx) {
    (void)siblings;
    (void)count;
    (void)ctx;
    return 0;
}

static riak_client *fuzz_client(void) {
    static riak_client *c;
    if (c == NULL) {
        riak_config cfg;
        riak_config_init(&cfg);
        if (riak_client_new(&cfg, &c) != RIAK_OK) {
            abort();
        }
    }
    return c;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) {
        return 0;
    }
    riak_client *c = fuzz_client();
    riak_pb_response resp = {0};
    if (riak_buf_append(&resp.body, data + 1, size - 1) != 0) {
        return 0;
    }
    switch (data[0] % 4) {
    case 0: {
        riak_opts o = {.resolver = (data[0] & 4U) ? pick_first : NULL};
        riak_object obj = {0};
        if (riak_pb_finish_get(c, &o, &resp, &obj) == RIAK_OK && obj.data[obj.len] != '\0') {
            abort();
        }
        riak_object_free(&obj);
        break;
    }
    case 1: {
        riak_object obj = {0};
        char *key = NULL;
        (void)riak_pb_finish_put(c, &resp, &obj, (data[0] & 4U) ? &key : NULL);
        riak_object_free(&obj);
        free(key);
        break;
    }
    case 2: {
        char msg[256];
        enum riak_retry retry;
        (void)riak_pb_server_error(data + 1, size - 1, msg, sizeof msg, &retry);
        if (memchr(msg, '\0', sizeof msg) == NULL) {
            abort();
        }
        break;
    }
    default: {
        ProtobufCBinaryData out;
        if (riak_pb_b64_decode(resp.body.data, &out)) {
            char *again = riak_pb_b64_encode(out);
            if (again == NULL || strlen(again) != strlen(resp.body.data)) {
                abort(); /* decoded data re-encodes to the same length */
            }
            free(again);
            free(out.data);
        }
        break;
    }
    }
    riak_buf_free(&resp.body);
    return 0;
}
