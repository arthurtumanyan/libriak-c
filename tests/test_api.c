/*
 File:   test_api.c
 SPDX-License-Identifier: GPL-3.0-or-later

 Public API checks that need no server.
 */

#include <string.h>

#include "check.h"
#include "riak/riak.h"

static void test_strerror(void) {
    for (int st = RIAK_OK; st <= RIAK_E_CANCELLED; st++) {
        const char *msg = riak_strerror((riak_status)st);
        CHECK(msg != NULL && strcmp(msg, "unknown error") != 0);
    }
    CHECK_EQ_STR(riak_strerror((riak_status)999), "unknown error");
}

static void test_free_null(void) {
    riak_strlist_free(NULL);
    riak_object_free(NULL);
    riak_free(NULL);
    riak_client_free(NULL);
    CHECK_EQ_STR(riak_client_errmsg(NULL), "no client");
    CHECK_EQ_INT(riak_client_http_status(NULL), 0);
}

static void test_client_config(void) {
    riak_client *c = NULL;
    CHECK_EQ_INT(riak_client_new(NULL, NULL), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_client_new(NULL, &c), RIAK_OK); /* 127.0.0.1:8098 */
    riak_client_free(c);

    riak_config cfg;
    riak_config_init(&cfg);
    CHECK_EQ_STR(cfg.host, "127.0.0.1");
    CHECK_EQ_INT(cfg.port, 8098);
    CHECK(cfg.tls_verify);
    cfg.host = "::1"; /* IPv6 literal gets bracketed */
    cfg.max_body_bytes = 0;
    CHECK_EQ_INT(riak_client_new(&cfg, &c), RIAK_OK);
    riak_client_free(c);

    c = NULL;
    cfg.port = 70000;
    CHECK_EQ_INT(riak_client_new(&cfg, &c), RIAK_E_INVALID_ARG);
    cfg.port = 8098;
    cfg.host = "evil/host";
    CHECK_EQ_INT(riak_client_new(&cfg, &c), RIAK_E_INVALID_ARG);
    cfg.host = "127.0.0.1";
    cfg.client_id = "x\ny";
    CHECK_EQ_INT(riak_client_new(&cfg, &c), RIAK_E_INVALID_ARG);
    CHECK(c == NULL);
}

int main(void) {
    RUN(test_strerror);
    RUN(test_free_null);
    RUN(test_client_config);
    return CHECK_DONE();
}
