/*
 File:   test_tls.c
 SPDX-License-Identifier: GPL-3.0-or-later

 TLS verification, client certificates and Basic authentication against
 `stub_riak.py --tls <certs>`: node 0 wants user "riak" / "s3cret", node 1
 wants a client certificate, node 2 plain TLS. RIAK_TEST_CERTS names the
 certificate directory made by gen_certs.sh.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "riak/riak.h"

static unsigned ports[3];
static const char *certs;

static char *cert(const char *name) {
    static char paths[8][512];
    static int next;
    char *p = paths[next++ % 8];
    snprintf(p, sizeof paths[0], "%s/%s", certs, name);
    return p;
}

static riak_config tls_config(unsigned port) {
    riak_config cfg;
    riak_config_init(&cfg);
    cfg.transport = RIAK_TRANSPORT_HTTP;
    cfg.port = port;
    cfg.tls = true;
    cfg.ca_file = cert("ca.pem");
    cfg.max_retries = 0;
    return cfg;
}

static riak_status ping_with(const riak_config *cfg, riak_client **keep) {
    riak_client *c = NULL;
    riak_status st = riak_client_new(cfg, &c);
    if (st != RIAK_OK) {
        return st;
    }
    st = riak_ping(c);
    if (keep) {
        *keep = c;
    } else {
        riak_client_free(c);
    }
    return st;
}

static void test_verification(void) {
    riak_config cfg = tls_config(ports[2]);
    CHECK_EQ_INT(ping_with(&cfg, NULL), RIAK_OK);

    cfg.ca_file = cert("other.pem"); /* a CA that did not sign the server */
    riak_client *c = NULL;
    CHECK_EQ_INT(ping_with(&cfg, &c), RIAK_E_TLS);
    CHECK(riak_client_errmsg(c)[0] != '\0');
    riak_client_free(c);

    cfg.tls_verify = false; /* explicitly disabled: any certificate passes */
    CHECK_EQ_INT(ping_with(&cfg, NULL), RIAK_OK);

    cfg = tls_config(ports[2]);
    cfg.host = "localhost"; /* the certificate also names localhost */
    CHECK_EQ_INT(ping_with(&cfg, NULL), RIAK_OK);
}

static void test_basic_auth(void) {
    riak_config cfg = tls_config(ports[0]);
    CHECK_EQ_INT(ping_with(&cfg, NULL), RIAK_E_UNAUTHORIZED);
    cfg.username = "riak";
    cfg.password = "wrong";
    CHECK_EQ_INT(ping_with(&cfg, NULL), RIAK_E_UNAUTHORIZED);
    cfg.password = "s3cret";
    riak_client *c = NULL;
    CHECK_EQ_INT(ping_with(&cfg, &c), RIAK_OK);
    CHECK_EQ_INT(riak_put(c, "secure", "k", "v", 1, NULL, NULL, NULL), RIAK_OK);
    riak_client_free(c);

    /* credentials are never sent without TLS */
    riak_config plain;
    riak_config_init(&plain);
    plain.transport = RIAK_TRANSPORT_HTTP;
    plain.port = ports[0];
    plain.username = "riak";
    plain.password = "s3cret";
    CHECK_EQ_INT(riak_client_new(&plain, &c), RIAK_E_INVALID_ARG);
    riak_config nouser = tls_config(ports[0]);
    nouser.password = "s3cret";
    CHECK_EQ_INT(riak_client_new(&nouser, &c), RIAK_E_INVALID_ARG);
}

static void test_client_certificate(void) {
    riak_config cfg = tls_config(ports[1]);
    riak_client *c = NULL;
    CHECK_EQ_INT(ping_with(&cfg, &c), RIAK_E_TLS); /* the server wants a certificate */
    CHECK(strstr(riak_client_errmsg(c), "certificate") != NULL);
    riak_client_free(c);
    cfg.tls_cert_file = cert("client.pem");
    cfg.tls_key_file = cert("client.key");
    CHECK_EQ_INT(ping_with(&cfg, NULL), RIAK_OK);

    cfg.tls_key_file = cert("client-enc.key");
    cfg.tls_key_password = "secret";
    CHECK_EQ_INT(ping_with(&cfg, NULL), RIAK_OK);
    cfg.tls_key_password = "nope";
    CHECK_EQ_INT(ping_with(&cfg, NULL), RIAK_E_TLS);
}

static void record(riak_async_result *res, void *ctx) {
    *(riak_status *)ctx = res->status;
}

static void test_async_tls(void) {
    riak_config cfg = tls_config(ports[0]);
    cfg.username = "riak";
    cfg.password = "s3cret";
    riak_async *a = NULL;
    CHECK_EQ_INT(riak_async_new(&cfg, &a), RIAK_OK);
    riak_status got = RIAK_E_HTTP;
    CHECK_EQ_INT(riak_async_ping(a, record, &got), RIAK_OK);
    CHECK_EQ_INT(riak_async_run(a, 5000), 0);
    CHECK_EQ_INT(got, RIAK_OK);
    riak_async_free(a);
}

int main(void) {
    const char *ps = getenv("RIAK_TEST_PORTS");
    certs = getenv("RIAK_TEST_CERTS");
    if (ps == NULL || certs == NULL ||
        sscanf(ps, "%u,%u,%u", &ports[0], &ports[1], &ports[2]) != 3) {
        fprintf(stderr, "run through stub_riak.py --tls (RIAK_TEST_PORTS/RIAK_TEST_CERTS)\n");
        return 77;
    }
    CHECK_EQ_INT(riak_global_init(), RIAK_OK);
    RUN(test_verification);
    RUN(test_basic_auth);
    RUN(test_client_certificate);
    RUN(test_async_tls);
    riak_global_cleanup();
    return CHECK_DONE();
}
