/*
 File:   async.c
 SPDX-License-Identifier: GPL-3.0-or-later

 Write and read back many keys concurrently.
 Usage: riak_async_example [host] [port] [pb|http]
 The Protocol Buffers transport (port 8087) is used unless "http" is given
 (port 8098).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <riak/riak.h>

typedef struct {
    int done, failed;
} progress;

static void on_done(riak_async_result *res, void *ctx) {
    progress *p = ctx;
    p->done++;
    if (res->status != RIAK_OK) {
        p->failed++;
        fprintf(stderr, "request failed: %s (%s)\n", riak_strerror(res->status), res->errmsg);
    }
}

int main(int argc, char **argv) {
    riak_config cfg;
    riak_config_init(&cfg);
    if (argc > 1) cfg.host = argv[1];
    if (argc > 2) cfg.port = (unsigned)strtoul(argv[2], NULL, 10);
    if (argc > 3 && strcmp(argv[3], "http") == 0) cfg.transport = RIAK_TRANSPORT_HTTP;

    riak_global_init();
    riak_async *a = NULL;
    if (riak_async_new(&cfg, &a) != RIAK_OK) {
        fprintf(stderr, "cannot create client\n");
        return EXIT_FAILURE;
    }
    progress p = {0};
    riak_opts o = {.content_type = "text/plain"};
    char key[32];
    for (int i = 0; i < 100; i++) {
        snprintf(key, sizeof key, "key-%d", i);
        riak_async_put(a, "async-example", key, key, strlen(key), &o, on_done, &p);
    }
    size_t left = riak_async_run(a, 10000);
    printf("%d requests done, %d failed, %zu still pending\n", p.done, p.failed, left);
    riak_async_free(a);
    riak_global_cleanup();
    return p.failed || left ? EXIT_FAILURE : EXIT_SUCCESS;
}
