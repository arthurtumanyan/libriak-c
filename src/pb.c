/*
 File:   pb.c
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later

 Protocol Buffers transport: per-client state (TLS context, cached node
 addresses, one kept connection per node for blocking calls), the blocking
 request loop with retries, and helpers shared by the operations.
 */

#include "pb.h"

#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/err.h>

/* Resolved addresses are kept this long. */
#define DNS_TTL_MS 60000

typedef struct {
    struct sockaddr_storage *addrs;
    socklen_t *lens;
    size_t n;
    long long resolved_ms;
} dns_entry;

struct riak_pb_state {
    SSL_CTX *ssl_ctx;
    BIO_METHOD *bio;
    riak_pbconn **conns; /* blocking calls: one kept connection per node */
    dns_entry *dns;
};

/* ---- BIO without SIGPIPE ------------------------------------------- */

/* The BIO data points at the connection's descriptor. */
static int bio_fd(BIO *b) {
    return *(const int *)BIO_get_data(b);
}

static int nosig_write(BIO *b, const char *data, int len) {
    BIO_clear_retry_flags(b);
    ssize_t n = send(bio_fd(b), data, (size_t)len, MSG_NOSIGNAL);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        BIO_set_retry_write(b);
    }
    return (int)n;
}

static int nosig_read(BIO *b, char *buf, int len) {
    BIO_clear_retry_flags(b);
    ssize_t n = recv(bio_fd(b), buf, (size_t)len, 0);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        BIO_set_retry_read(b);
    }
    return (int)n;
}

static int nosig_puts(BIO *b, const char *s) {
    return nosig_write(b, s, (int)strlen(s));
}

static long nosig_ctrl(BIO *b, int cmd, long num, void *ptr) {
    (void)b;
    (void)num;
    (void)ptr;
    return cmd == BIO_CTRL_FLUSH ? 1 : 0;
}

const BIO_METHOD *riak_pb_bio_method(riak_client *c) {
    return c->pb->bio;
}

/* ---- TLS context ----------------------------------------------------- */

static int key_password_cb(char *buf, int size, int rwflag, void *u) {
    (void)rwflag;
    const char *pw = u;
    if (pw == NULL || size <= 0) {
        return 0;
    }
    size_t n = strlen(pw);
    if (n > (size_t)size) {
        n = (size_t)size;
    }
    memcpy(buf, pw, n); /* NOLINT(bugprone-not-null-terminated-result): OpenSSL takes a length */
    return (int)n;
}

static void ssl_error(char *err, size_t cap, const char *what, const char *file) {
    unsigned long e = ERR_peek_last_error();
    char detail[200] = "unknown error";
    if (e != 0) {
        ERR_error_string_n(e, detail, sizeof detail);
    }
    (void)snprintf(err, cap, "%s%s%s: %s", what, file ? " " : "", file ? file : "", detail);
    ERR_clear_error();
}

SSL_CTX *riak_pb_ssl_ctx(riak_client *c, char *err, size_t cap) {
    riak_pb_state *s = c->pb;
    if (s->ssl_ctx != NULL) {
        return s->ssl_ctx;
    }
    ERR_clear_error();
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == NULL) {
        ssl_error(err, cap, "cannot create TLS context", NULL);
        return NULL;
    }
    (void)SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    if (c->cfg.tls_legacy && riak_tls_legacy_ctx(ctx) != 0) {
        ssl_error(err, cap, "cannot apply tls_legacy", NULL);
        SSL_CTX_free(ctx);
        return NULL;
    }
    SSL_CTX_set_verify(ctx, c->cfg.tls_verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);
    bool ok = c->ca_file ? SSL_CTX_load_verify_locations(ctx, c->ca_file, NULL) == 1
                         : SSL_CTX_set_default_verify_paths(ctx) == 1;
    if (!ok) {
        ssl_error(err, cap, "cannot load CA certificates", c->ca_file);
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (c->cert_file && SSL_CTX_use_certificate_chain_file(ctx, c->cert_file) != 1) {
        ssl_error(err, cap, "cannot load client certificate", c->cert_file);
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (c->key_file || c->cert_file) {
        const char *key = c->key_file ? c->key_file : c->cert_file;
        SSL_CTX_set_default_passwd_cb(ctx, key_password_cb);
        SSL_CTX_set_default_passwd_cb_userdata(ctx, c->key_password);
        if (SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(ctx) != 1) {
            ssl_error(err, cap, "cannot load client key", key);
            SSL_CTX_free(ctx);
            return NULL;
        }
        /* the passphrase is not needed after loading */
        SSL_CTX_set_default_passwd_cb_userdata(ctx, NULL);
    }
    s->ssl_ctx = ctx;
    return ctx;
}

/* Old Erlang/OTP TLS servers crash on RSA-PSS and EdDSA signature
 * algorithms, lack secure renegotiation, offer 1024-bit DHE groups and have
 * a broken ECDHE; this is what remains acceptable to both sides. */
int riak_tls_legacy_ctx(void *ssl_ctx) {
    SSL_CTX *ctx = ssl_ctx;
    (void)SSL_CTX_set_options(ctx, SSL_OP_LEGACY_SERVER_CONNECT);
    if (SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION) != 1 ||
        SSL_CTX_set1_sigalgs_list(ctx, "RSA+SHA256:RSA+SHA384:RSA+SHA512:ECDSA+SHA256:"
                                       "ECDSA+SHA384:ECDSA+SHA512:RSA+SHA1:ECDSA+SHA1") != 1 ||
        SSL_CTX_set_cipher_list(ctx, "DEFAULT:!kDHE") != 1) {
        return -1;
    }
    return 0;
}

/* ---- client state ----------------------------------------------------- */

riak_status riak_pb_client_init(riak_client *c) {
    riak_pb_state *s = calloc(1, sizeof *s);
    if (s == NULL) {
        return RIAK_E_NOMEM;
    }
    c->pb = s;
    s->conns = (riak_pbconn **)calloc(c->n_nodes, sizeof *s->conns);
    s->dns = calloc(c->n_nodes, sizeof *s->dns);
    s->bio = BIO_meth_new((int)((unsigned)BIO_get_new_index() | (unsigned)BIO_TYPE_SOURCE_SINK),
                          "riak socket");
    if (s->conns == NULL || s->dns == NULL || s->bio == NULL ||
        BIO_meth_set_write(s->bio, nosig_write) != 1 ||
        BIO_meth_set_read(s->bio, nosig_read) != 1 || BIO_meth_set_puts(s->bio, nosig_puts) != 1 ||
        BIO_meth_set_ctrl(s->bio, nosig_ctrl) != 1) {
        return RIAK_E_NOMEM;
    }
    return RIAK_OK;
}

static void dns_clear(dns_entry *d) {
    free(d->addrs);
    free(d->lens);
    memset(d, 0, sizeof *d);
}

void riak_pb_client_free(riak_client *c) {
    riak_pb_state *s = c->pb;
    if (s == NULL) {
        return;
    }
    for (size_t i = 0; i < c->n_nodes; i++) {
        if (s->conns) riak_pbconn_free(s->conns[i]);
        if (s->dns) dns_clear(&s->dns[i]);
    }
    free((void *)s->conns);
    free(s->dns);
    SSL_CTX_free(s->ssl_ctx);
    BIO_meth_free(s->bio);
    free(s);
    c->pb = NULL;
}

riak_status riak_pb_resolve(riak_client *c, size_t node, struct sockaddr_storage **addrs,
                            socklen_t **lens, size_t *n) {
    dns_entry *d = &c->pb->dns[node];
    long long now = riak_now_ms();
    if (d->n == 0 || now - d->resolved_ms > DNS_TTL_MS) {
        const riak_node_state *ns = &c->nodes[node];
        char port[8];
        (void)snprintf(port, sizeof port, "%u", ns->port);
        struct addrinfo hints = {
            .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_NUMERICSERV};
        struct addrinfo *res = NULL;
        int rc = getaddrinfo(ns->host, port, &hints, &res);
        if (rc != 0) {
            if (d->n > 0) {
                goto copy; /* keep using the stale addresses */
            }
            return riak_set_error(c, RIAK_E_TRANSPORT, "cannot resolve %s: %s", ns->host,
                                  gai_strerror(rc));
        }
        size_t count = 0;
        for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
            count++;
        }
        struct sockaddr_storage *a = calloc(count ? count : 1, sizeof *a);
        socklen_t *l = calloc(count ? count : 1, sizeof *l);
        if (a == NULL || l == NULL) {
            free(a);
            free(l);
            freeaddrinfo(res);
            return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
        }
        size_t i = 0;
        for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
            if (ai->ai_addrlen <= sizeof a[i]) {
                memcpy(&a[i], ai->ai_addr, ai->ai_addrlen);
                l[i++] = ai->ai_addrlen;
            }
        }
        freeaddrinfo(res);
        dns_clear(d);
        d->addrs = a;
        d->lens = l;
        d->n = i;
        d->resolved_ms = now;
    }
copy:
    *addrs = calloc(d->n ? d->n : 1, sizeof **addrs);
    *lens = calloc(d->n ? d->n : 1, sizeof **lens);
    if (*addrs == NULL || *lens == NULL) {
        free(*addrs);
        free(*lens);
        *addrs = NULL;
        *lens = NULL;
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    memcpy(*addrs, d->addrs, d->n * sizeof **addrs);
    memcpy(*lens, d->lens, d->n * sizeof **lens);
    *n = d->n;
    return RIAK_OK;
}

/* ---- blocking requests ---------------------------------------------- */

static riak_status attempt_once(riak_client *c, size_t node, unsigned attempt,
                                const riak_pb_request *req, riak_pb_response *resp,
                                enum riak_retry *retry) {
    riak_pb_state *s = c->pb;
    riak_pbconn *k = s->conns[node];
    if (k != NULL && !riak_pbconn_reusable(k)) {
        riak_pbconn_free(k);
        k = NULL;
    }
    if (k == NULL) {
        if ((k = riak_pbconn_open(c, node)) == NULL) {
            *retry = RIAK_RETRY_NONE;
            return RIAK_E_NOMEM;
        }
        s->conns[node] = k;
    }
    c->nodes[node].requests++;
    riak_log(c, RIAK_LOG_DEBUG, "%s %s on %s:%u (attempt %u)", req->name, req->path,
             c->nodes[node].host, c->nodes[node].port, attempt);
    riak_pbconn_begin(k, req, resp, attempt);
    for (;;) {
        int want = riak_pbconn_step(k);
        if (want == 0) {
            break;
        }
        int timeout = -1;
        long long deadline = riak_pbconn_deadline(k);
        if (deadline > 0) {
            long long left = deadline - riak_now_ms();
            if (left <= 0) {
                riak_pbconn_timeout(k);
                continue;
            }
            timeout = left > INT_MAX ? INT_MAX : (int)left;
        }
        unsigned w = (unsigned)want;
        struct pollfd p = {.fd = k->fd,
                           .events = (short)(((w & (unsigned)RIAK_PBC_WANT_READ) ? POLLIN : 0) +
                                             ((w & (unsigned)RIAK_PBC_WANT_WRITE) ? POLLOUT : 0))};
        (void)poll(&p, 1, timeout); /* EINTR and errors: the next step finds out */
    }
    riak_status st = riak_pbconn_finish(k, retry);
    if (k->state != RIAK_PBC_READY) {
        riak_pbconn_free(k);
        s->conns[node] = NULL;
    }
    return st;
}

static riak_status perform(riak_client *c, const riak_pb_request *req, riak_pb_response *resp) {
    size_t k = req->pin > 0 ? (req->pin - 1) % c->n_nodes : riak_pick_node(c, SIZE_MAX);
    for (unsigned attempt = 0;; attempt++) {
        enum riak_retry retry;
        riak_status st = attempt_once(c, k, attempt, req, resp, &retry);
        if (req->pin > 0 || !riak_retry_allowed(c, req->idempotent, retry, attempt + 1)) {
            riak_error_add_node(c, st);
            return st;
        }
        riak_buf_free(&resp->body);
        resp->stopped = false;
        riak_sleep_ms(riak_backoff_ms(c, attempt));
        k = riak_pick_node(c, k);
    }
}

riak_status riak_pb_perform(riak_client *c, const riak_pb_request *req, riak_pb_response *resp) {
    if (c->busy) {
        return riak_set_error(c, RIAK_E_INVALID_ARG, RIAK_CLIENT_BUSY_MSG);
    }
    c->busy = true;
    riak_status st = perform(c, req, resp);
    c->busy = false;
    return st;
}

static riak_status pack(riak_client *c, const ProtobufCMessage *msg, uint8_t **buf, size_t *len) {
    *buf = NULL;
    *len = 0;
    if (msg == NULL) {
        return RIAK_OK;
    }
    *len = protobuf_c_message_get_packed_size(msg);
    *buf = malloc(*len ? *len : 1);
    if (*buf == NULL) {
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    protobuf_c_message_pack(msg, *buf);
    return RIAK_OK;
}

static riak_status run(riak_client *c, const riak_pb_call *call, const ProtobufCMessage *msg,
                       riak_pb_sink_fn sink, void *ctx, riak_pb_response *resp) {
    uint8_t *buf;
    size_t len;
    riak_status st = pack(c, msg, &buf, &len);
    if (st != RIAK_OK) {
        return st;
    }
    riak_pb_request req = {.code = call->code,
                           .resp_code = call->resp_code,
                           .payload = buf,
                           .len = len,
                           .sink = sink,
                           .sink_ctx = ctx,
                           .idempotent = call->idempotent,
                           .pin = call->pin,
                           .name = call->name,
                           .path = call->path ? call->path : "/"};
    st = riak_pb_perform(c, &req, resp);
    free(buf);
    return st;
}

riak_status riak_pb_exec(riak_client *c, const riak_pb_call *call, const ProtobufCMessage *msg,
                         const ProtobufCMessageDescriptor *resp_desc, ProtobufCMessage **out) {
    if (out) {
        *out = NULL;
    }
    riak_pb_response resp = {0};
    riak_status st = run(c, call, msg, NULL, NULL, &resp);
    if (st == RIAK_OK && resp_desc && out) {
        *out = protobuf_c_message_unpack(resp_desc, NULL, resp.body.len,
                                         (const uint8_t *)resp.body.data);
        if (*out == NULL) {
            st = riak_set_error(c, RIAK_E_PARSE, "malformed %s", resp_desc->name);
        }
    }
    riak_buf_free(&resp.body);
    return st;
}

riak_status riak_pb_stream(riak_client *c, const riak_pb_call *call, const ProtobufCMessage *msg,
                           riak_pb_sink_fn sink, void *ctx) {
    riak_pb_response resp = {0};
    riak_status st = run(c, call, msg, sink, ctx, &resp);
    riak_buf_free(&resp.body);
    return st;
}

/* ---- helpers --------------------------------------------------------- */

static bool contains(const char *s, size_t n, const char *needle) {
    size_t m = strlen(needle);
    for (size_t i = 0; i + m <= n; i++) {
        if (memcmp(s + i, needle, m) == 0) {
            return true;
        }
    }
    return false;
}

riak_status riak_pb_server_error(const uint8_t *data, size_t len, char *msg, size_t cap,
                                 enum riak_retry *retry) {
    *retry = RIAK_RETRY_NONE;
    RpbErrorResp *e = rpb_error_resp__unpack(NULL, len, data);
    if (e == NULL) {
        (void)snprintf(msg, cap, "malformed error response");
        return RIAK_E_PARSE;
    }
    const char *m = (const char *)e->errmsg.data;
    size_t n = e->errmsg.len;
    riak_status st = RIAK_E_SERVER;
    if ((n == 8 && memcmp(m, "notfound", 8) == 0) || contains(m, n, "no_type") ||
        contains(m, n, "No bucket-type") || contains(m, n, "notfound")) {
        st = RIAK_E_NOT_FOUND;
    } else if ((n == 8 && memcmp(m, "modified", 8) == 0) || contains(m, n, "match_found") ||
               contains(m, n, "precondition")) {
        st = RIAK_E_PRECONDITION;
    } else if (contains(m, n, "Permission denied") || contains(m, n, "Authentication failed") ||
               contains(m, n, "Security is enabled") || contains(m, n, "unauthorized")) {
        st = RIAK_E_UNAUTHORIZED;
    } else if (contains(m, n, "Unknown message code")) {
        st = RIAK_E_UNSUPPORTED; /* the node does not implement this request */
    } else if (contains(m, n, "overload") || contains(m, n, "timeout")) {
        *retry = RIAK_RETRY_IDEMPOTENT; /* like HTTP 503 */
    }
    int shown = n > 300 ? 300 : (int)n;
    if (e->errcode != 0) {
        (void)snprintf(msg, cap, "server error %u: %.*s", e->errcode, shown, m);
    } else {
        (void)snprintf(msg, cap, "server error: %.*s", shown, m);
    }
    rpb_error_resp__free_unpacked(e, NULL);
    return st;
}

bool riak_pb_quorum(int q, protobuf_c_boolean *has, uint32_t *out) {
    switch (q) {
    case RIAK_Q_DEFAULT:
        *has = 0;
        return true;
    case RIAK_Q_ONE:
        *out = RIAK_PB_Q_ONE;
        break;
    case RIAK_Q_QUORUM:
        *out = RIAK_PB_Q_QUORUM;
        break;
    case RIAK_Q_ALL:
        *out = RIAK_PB_Q_ALL;
        break;
    default:
        if (q < 0) {
            return false;
        }
        *out = (uint32_t)q;
        break;
    }
    *has = 1;
    return true;
}

ProtobufCBinaryData riak_pb_bin(const char *s) {
    return riak_pb_bytes(s, s ? strlen(s) : 0);
}

ProtobufCBinaryData riak_pb_bytes(const void *p, size_t len) {
    /* protobuf-c only reads the data of a message being packed */
    ProtobufCBinaryData b = {.len = len, .data = NULL};
    memcpy((void *)&b.data, (const void *)&p, sizeof p);
    return b;
}

char *riak_pb_strdup(ProtobufCBinaryData b) {
    return riak_strndup(b.data ? (const char *)b.data : "", b.len);
}

char *riak_pb_path(riak_client *c, const char *type, const char *bucket, const char *suffix,
                   const char *key) {
    riak_buf b = {0};
    if (riak_url_bucket(&b, c, type, bucket, suffix, key) != 0) {
        riak_buf_free(&b);
        return NULL;
    }
    return riak_buf_steal(&b, NULL);
}
