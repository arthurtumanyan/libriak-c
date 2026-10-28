/*
 File:   basic.c
 SPDX-License-Identifier: GPL-3.0-or-later

 Usage: riak_example [host] [port] [pb|http]
 The Protocol Buffers transport (port 8087) is used unless "http" is given
 (port 8098).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <riak/riak.h>

static int fail(riak_client *c, const char *what, riak_status st) {
    fprintf(stderr, "%s: %s (%s)\n", what, riak_strerror(st), riak_client_errmsg(c));
    return EXIT_FAILURE;
}

int main(int argc, char **argv) {
    riak_config cfg;
    riak_config_init(&cfg);
    if (argc > 1) cfg.host = argv[1];
    if (argc > 2) cfg.port = (unsigned)strtoul(argv[2], NULL, 10);
    if (argc > 3 && strcmp(argv[3], "http") == 0) cfg.transport = RIAK_TRANSPORT_HTTP;
    cfg.timeout_ms = 5000;

    riak_global_init();
    riak_client *c = NULL;
    riak_status st = riak_client_new(&cfg, &c);
    if (st != RIAK_OK) {
        fprintf(stderr, "client: %s\n", riak_strerror(st));
        return EXIT_FAILURE;
    }

    int rc = EXIT_FAILURE;
    if ((st = riak_ping(c)) != RIAK_OK) {
        rc = fail(c, "ping", st);
        goto out;
    }

    riak_opts put = {.content_type = "text/plain", .w = RIAK_Q_QUORUM};
    const char *value = "hello riak";
    if ((st = riak_put(c, "test", "doc", value, strlen(value), &put, NULL, NULL)) != RIAK_OK) {
        rc = fail(c, "put", st);
        goto out;
    }

    riak_object obj;
    if ((st = riak_get(c, "test", "doc", NULL, &obj)) != RIAK_OK) {
        rc = fail(c, "get", st);
        goto out;
    }
    printf("test/doc = %.*s (%s)\n", (int)obj.len, obj.data, obj.content_type);
    riak_object_free(&obj);

    riak_strlist keys;
    if ((st = riak_list_keys(c, "test", &keys)) != RIAK_OK) {
        rc = fail(c, "list keys", st);
        goto out;
    }
    for (char **k = keys.items; *k; k++) {
        printf("key: %s\n", *k);
    }
    riak_strlist_free(&keys);

    if ((st = riak_delete(c, "test", "doc", NULL)) != RIAK_OK) {
        rc = fail(c, "delete", st);
        goto out;
    }
    rc = EXIT_SUCCESS;
out:
    riak_client_free(c);
    riak_global_cleanup();
    return rc;
}
