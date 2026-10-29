/*
 File:   pbconn.c
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later

 One Protocol Buffers connection as a non-blocking state machine: connect,
 optional STARTTLS + TLS handshake + authentication, client id, then one
 request at a time. Frames are a 4-byte big-endian length (code included),
 a 1-byte message code and the packed message. The blocking API drives a
 connection with poll(); the async API drives many from its event loop.
 */

#include "pb.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/x509v3.h>

#define RBUF_SIZE 16384
/* libcurl's default connect timeout, used when none is configured */
#define DEFAULT_CONNECT_TIMEOUT_MS 300000L

/* strerror() may share a static buffer between threads; the XSI strerror_r()
 * (selected by _POSIX_C_SOURCE without _GNU_SOURCE) writes into the caller's. */
typedef struct {
    char text[128];
} errno_text;

static errno_text sys_error(int err) {
    errno_text t;
    if (strerror_r(err, t.text, sizeof t.text) != 0) {
        snprintf(t.text, sizeof t.text, "error %d", err);
    }
    return t;
}

static void fail(riak_pbconn *k, riak_status st, const char *fmt, ...) RIAK_PRINTF(3, 4);

static void fail(riak_pbconn *k, riak_status st, const char *fmt, ...) {
    if (k->finished) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(k->errmsg, sizeof k->errmsg, fmt, ap);
    va_end(ap);
    k->st = st;
    if (!k->sent) {
        k->retry = RIAK_RETRY_ANY; /* the request never left: always safe to repeat */
    } else if (st == RIAK_E_TRANSPORT && !k->delivered) {
        k->retry = RIAK_RETRY_IDEMPOTENT;
    } else {
        k->retry = RIAK_RETRY_NONE;
    }
    if (st == RIAK_E_TLS || st == RIAK_E_UNAUTHORIZED || st == RIAK_E_NOMEM ||
        st == RIAK_E_INVALID_ARG) {
        k->retry = RIAK_RETRY_NONE; /* another node will not do better */
    }
    k->finished = true;
    k->state = RIAK_PBC_BROKEN;
}

static const char *node_name(const riak_pbconn *k) {
    return k->c->nodes[k->node].host;
}

static unsigned node_port(const riak_pbconn *k) {
    return k->c->nodes[k->node].port;
}

/* ---- framing -------------------------------------------------------- */

static bool queue_frame(riak_pbconn *k, uint8_t code, const uint8_t *payload, size_t len) {
    if (len > UINT32_MAX - 1) {
        fail(k, RIAK_E_INVALID_ARG, "request too large");
        return false;
    }
    uint32_t n = (uint32_t)len + 1;
    uint8_t hdr[5] = {(uint8_t)(n >> 24U), (uint8_t)(n >> 16U), (uint8_t)(n >> 8U), (uint8_t)n,
                      code};
    k->out.len = 0;
    k->out_off = 0;
    if (riak_buf_append(&k->out, hdr, sizeof hdr) != 0 ||
        (len && riak_buf_append(&k->out, payload, len) != 0)) {
        fail(k, RIAK_E_NOMEM, "out of memory");
        return false;
    }
    return true;
}

/* ---- raw I/O: >0 bytes, 0 would block (k->want set), -1 failed ------- */

static void tls_fail(riak_pbconn *k, const char *what) {
    unsigned long e = ERR_peek_last_error();
    long vr = k->ssl ? SSL_get_verify_result(k->ssl) : X509_V_OK;
    if (vr != X509_V_OK) {
        fail(k, RIAK_E_TLS, "%s: certificate verification failed: %s", what,
             X509_verify_cert_error_string(vr));
#ifdef SSL_R_TLSV13_ALERT_CERTIFICATE_REQUIRED
    } else if (ERR_GET_REASON(e) == SSL_R_TLSV13_ALERT_CERTIFICATE_REQUIRED ||
               (ERR_GET_REASON(e) == SSL_R_SSLV3_ALERT_HANDSHAKE_FAILURE &&
                k->c->cert_file == NULL && k->state == RIAK_PBC_HANDSHAKE)) {
#else
    } else if (ERR_GET_REASON(e) == SSL_R_SSLV3_ALERT_HANDSHAKE_FAILURE &&
               k->c->cert_file == NULL && k->state == RIAK_PBC_HANDSHAKE) {
#endif
        fail(k, RIAK_E_TLS, "server requires a client certificate");
    } else if (e != 0) {
        char buf[200];
        ERR_error_string_n(e, buf, sizeof buf);
        fail(k, RIAK_E_TLS, "%s: %s", what, buf);
    } else {
        fail(k, RIAK_E_TLS, "%s failed", what);
    }
    ERR_clear_error();
}

/* Outcome of an SSL call that returned ret <= 0. */
static ssize_t ssl_result(riak_pbconn *k, int ret, const char *what) {
    switch (SSL_get_error(k->ssl, ret)) {
    case SSL_ERROR_WANT_READ:
        k->want = RIAK_PBC_WANT_READ;
        return 0;
    case SSL_ERROR_WANT_WRITE:
        k->want = RIAK_PBC_WANT_WRITE;
        return 0;
    case SSL_ERROR_ZERO_RETURN:
        fail(k, RIAK_E_TRANSPORT, "connection closed by %s:%u", node_name(k), node_port(k));
        return -1;
    case SSL_ERROR_SYSCALL:
        if (ERR_peek_last_error() != 0) {
            tls_fail(k, what);
        } else if (k->state == RIAK_PBC_HANDSHAKE) {
            /* old TLS servers drop the connection on a ClientHello they cannot parse */
            fail(k, RIAK_E_TLS, "%s: connection closed by %s:%u%s", what, node_name(k),
                 node_port(k), k->c->cfg.tls_legacy ? "" : LEGACY_HINT);
        } else if (errno != 0 && ret < 0) {
            fail(k, RIAK_E_TRANSPORT, "%s:%u: %s", node_name(k), node_port(k),
                 sys_error(errno).text);
        } else {
            fail(k, RIAK_E_TRANSPORT, "connection closed by %s:%u", node_name(k), node_port(k));
        }
        return -1;
    default:
        tls_fail(k, what);
        return -1;
    }
}

static ssize_t io_write(riak_pbconn *k, const uint8_t *p, size_t n) {
    if (k->ssl) {
        ERR_clear_error();
        errno = 0;
        int ret = SSL_write(k->ssl, p, n > INT32_MAX ? INT32_MAX : (int)n);
        return ret > 0 ? ret : ssl_result(k, ret, "TLS write");
    }
    for (;;) {
        ssize_t w = send(k->fd, p, n, MSG_NOSIGNAL);
        if (w >= 0) {
            return w;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            k->want = RIAK_PBC_WANT_WRITE;
            return 0;
        }
        fail(k, RIAK_E_TRANSPORT, "%s:%u: %s", node_name(k), node_port(k), sys_error(errno).text);
        return -1;
    }
}

static ssize_t io_read(riak_pbconn *k, uint8_t *p, size_t n) {
    if (k->ssl) {
        ERR_clear_error();
        errno = 0;
        int ret = SSL_read(k->ssl, p, n > INT32_MAX ? INT32_MAX : (int)n);
        return ret > 0 ? ret : ssl_result(k, ret, "TLS read");
    }
    for (;;) {
        ssize_t r = recv(k->fd, p, n, 0);
        if (r > 0) {
            return r;
        }
        if (r == 0) {
            fail(k, RIAK_E_TRANSPORT, "connection closed by %s:%u", node_name(k), node_port(k));
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            k->want = RIAK_PBC_WANT_READ;
            return 0;
        }
        fail(k, RIAK_E_TRANSPORT, "%s:%u: %s", node_name(k), node_port(k), sys_error(errno).text);
        return -1;
    }
}

/* The pending frame is written and one response frame read: 1 when a frame
 * is complete (code in hdr[4], payload in `in`), 0 would block, -1 failed. */
static int pump(riak_pbconn *k) {
    while (k->out_off < k->out.len) {
        ssize_t w = io_write(k, (const uint8_t *)k->out.data + k->out_off, k->out.len - k->out_off);
        if (w <= 0) {
            return (int)w;
        }
        k->out_off += (size_t)w;
        if (k->state == RIAK_PBC_BUSY) {
            k->sent = true;
        }
    }
    for (;;) {
        if (k->rpos == k->rlen) {
            k->rpos = k->rlen = 0;
            ssize_t r = io_read(k, k->rbuf, RBUF_SIZE);
            if (r <= 0) {
                return (int)r;
            }
            k->rlen = (size_t)r;
        }
        while (k->hdr_len < sizeof k->hdr && k->rpos < k->rlen) {
            k->hdr[k->hdr_len++] = k->rbuf[k->rpos++];
            if (k->hdr_len == sizeof k->hdr) {
                uint32_t n = ((uint32_t)k->hdr[0] << 24U) | ((uint32_t)k->hdr[1] << 16U) |
                             ((uint32_t)k->hdr[2] << 8U) | k->hdr[3];
                if (n == 0) {
                    fail(k, RIAK_E_PARSE, "malformed message frame");
                    return -1;
                }
                k->frame_len = n - 1;
                if (k->frame_len > k->c->cfg.max_body_bytes) {
                    fail(k, RIAK_E_HTTP, "response exceeds max_body_bytes");
                    return -1;
                }
                k->in.len = 0;
            }
        }
        if (k->hdr_len < sizeof k->hdr) {
            continue;
        }
        size_t need = k->frame_len - k->in.len;
        size_t take = k->rlen - k->rpos < need ? k->rlen - k->rpos : need;
        if (take > 0) {
            if (riak_buf_append(&k->in, k->rbuf + k->rpos, take) != 0) {
                fail(k, RIAK_E_NOMEM, "out of memory while reading the response");
                return -1;
            }
            k->rpos += take;
        }
        if (k->in.len == k->frame_len) {
            k->hdr_len = 0;
            k->received += k->frame_len;
            return 1;
        }
    }
}

/* ---- connection setup ----------------------------------------------- */

static void close_fd(riak_pbconn *k) {
    if (k->fd >= 0) {
        close(k->fd);
        k->fd = -1;
    }
}

/* Connects to the next candidate address: 1 connected, 0 in progress, -1 none left. */
static int connect_next(riak_pbconn *k) {
    int last_errno = ECONNREFUSED;
    while (k->next_addr < k->n_addrs) {
        size_t i = k->next_addr++;
        close_fd(k);
        k->fd = socket(k->addrs[i].ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (k->fd < 0) {
            last_errno = errno;
            continue;
        }
        int one = 1;
        (void)setsockopt(k->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        (void)setsockopt(k->fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
        if (connect(k->fd, (const struct sockaddr *)&k->addrs[i], k->addr_lens[i]) == 0) {
            return 1;
        }
        if (errno == EINPROGRESS || errno == EINTR) {
            k->want = RIAK_PBC_WANT_WRITE;
            return 0;
        }
        last_errno = errno;
    }
    close_fd(k);
    fail(k, RIAK_E_TRANSPORT, "cannot connect to %s:%u: %s", node_name(k), node_port(k),
         sys_error(last_errno).text);
    return -1;
}

static bool is_ip(const char *host) {
    unsigned char buf[sizeof(struct in6_addr)];
    return inet_pton(AF_INET, host, buf) == 1 || inet_pton(AF_INET6, host, buf) == 1;
}

static bool start_tls(riak_pbconn *k) {
    riak_client *c = k->c;
    char err[256];
    SSL_CTX *ctx = riak_pb_ssl_ctx(c, err, sizeof err);
    if (ctx == NULL) {
        fail(k, RIAK_E_TLS, "%s", err);
        return false;
    }
    BIO *bio = NULL;
    k->ssl = SSL_new(ctx);
    if (k->ssl == NULL || (bio = BIO_new(riak_pb_bio_method(c))) == NULL) {
        BIO_free(bio);
        fail(k, RIAK_E_NOMEM, "cannot create TLS session");
        return false;
    }
    BIO_set_data(bio, &k->fd);
    BIO_set_init(bio, 1);
    SSL_set_bio(k->ssl, bio, bio);
    const char *host = node_name(k);
    bool ip = is_ip(host);
    if (!ip) {
        /* the macro casts the name to void *, so the node's own copy is passed */
        (void)SSL_set_tlsext_host_name(k->ssl, c->nodes[k->node].host);
    }
    if (c->cfg.tls_verify) {
        int ok = ip ? X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(k->ssl), host)
                    : SSL_set1_host(k->ssl, host);
        if (ok != 1) {
            fail(k, RIAK_E_NOMEM, "cannot set the TLS peer name");
            return false;
        }
    }
    SSL_set_connect_state(k->ssl);
    return true;
}

/* After the TCP connection (and TLS + auth when configured): the client id
 * is sent, or the connection is ready. */
static void after_auth(riak_pbconn *k) {
    const char *id = k->c->client_id;
    if (id == NULL) {
        k->state = RIAK_PBC_READY;
        return;
    }
    RpbSetClientIdReq m = RPB_SET_CLIENT_ID_REQ__INIT;
    m.client_id = riak_pb_bin(id);
    size_t n = rpb_set_client_id_req__get_packed_size(&m);
    uint8_t *buf = malloc(n ? n : 1);
    if (buf == NULL) {
        fail(k, RIAK_E_NOMEM, "out of memory");
        return;
    }
    rpb_set_client_id_req__pack(&m, buf);
    if (queue_frame(k, RIAK_PB_SET_CLIENT_ID_REQ, buf, n)) {
        k->state = RIAK_PBC_CLIENT_ID;
    }
    free(buf);
}

static void after_handshake(riak_pbconn *k) {
    riak_client *c = k->c;
    if (c->username == NULL) {
        after_auth(k);
        return;
    }
    RpbAuthReq m = RPB_AUTH_REQ__INIT;
    m.user = riak_pb_bin(c->username);
    m.password = riak_pb_bin(c->password ? c->password : "");
    size_t n = rpb_auth_req__get_packed_size(&m);
    uint8_t *buf = malloc(n ? n : 1);
    if (buf == NULL) {
        fail(k, RIAK_E_NOMEM, "out of memory");
        return;
    }
    rpb_auth_req__pack(&m, buf);
    bool ok = queue_frame(k, RIAK_PB_AUTH_REQ, buf, n);
    OPENSSL_cleanse(buf, n); /* the password is in there */
    free(buf);
    if (ok) {
        k->state = RIAK_PBC_AUTH;
    }
}

static void after_connect(riak_pbconn *k) {
    if (k->c->cfg.tls) {
        if (queue_frame(k, RIAK_PB_START_TLS, NULL, 0)) {
            k->state = RIAK_PBC_STARTTLS;
        }
    } else {
        after_auth(k);
    }
}

/* A setup reply is checked: true when it has the expected code. */
static bool setup_reply(riak_pbconn *k, uint8_t want, const char *what) {
    uint8_t code = k->hdr[4];
    if (code == want) {
        return true;
    }
    if (code == RIAK_PB_ERROR_RESP) {
        char msg[200];
        enum riak_retry retry;
        riak_status st =
            riak_pb_server_error((const uint8_t *)k->in.data, k->in.len, msg, sizeof msg, &retry);
        if (want == RIAK_PB_AUTH_RESP) {
            st = RIAK_E_UNAUTHORIZED;
        } else if (want == RIAK_PB_START_TLS) {
            st = RIAK_E_TLS; /* e.g. security is not enabled on the node */
        }
        fail(k, st, "%s refused: %s", what, msg);
        return false;
    }
    fail(k, RIAK_E_PARSE, "unexpected reply to %s (message code %u)", what, code);
    return false;
}

/* ---- the request ---------------------------------------------------- */

static void start_request(riak_pbconn *k) {
    if (queue_frame(k, k->req->code, k->req->payload, k->req->len)) {
        k->state = RIAK_PBC_BUSY;
        k->started_ms = riak_now_ms();
    }
}

static void handle_response(riak_pbconn *k) {
    uint8_t code = k->hdr[4];
    const uint8_t *data = (const uint8_t *)k->in.data;
    size_t len = k->in.len;
    if (code == RIAK_PB_ERROR_RESP) {
        enum riak_retry retry = RIAK_RETRY_NONE;
        k->st = riak_pb_server_error(data, len, k->errmsg, sizeof k->errmsg, &retry);
        k->retry = retry;
        k->finished = true;
        /* a request the server's handler crashed on: Riak closes the
         * connection right after this error, so it is not reused */
        k->state = strstr(k->errmsg, "Error processing incoming message") ? RIAK_PBC_BROKEN
                                                                          : RIAK_PBC_READY;
        return;
    }
    if (code != k->req->resp_code) {
        fail(k, RIAK_E_PARSE, "unexpected response message code %u", code);
        return;
    }
    if (k->req->sink == NULL) {
        riak_buf_free(&k->resp->body);
        k->resp->body = k->in;
        memset(&k->in, 0, sizeof k->in);
        if (k->resp->body.data == NULL && riak_buf_append(&k->resp->body, "", 0) != 0) {
            fail(k, RIAK_E_NOMEM, "out of memory");
            return;
        }
        k->st = RIAK_OK;
        k->finished = true;
        k->state = RIAK_PBC_READY;
        return;
    }
    bool done = false;
    riak_status err = RIAK_OK;
    k->delivered = true;
    if (!k->req->sink(k->req->sink_ctx, data, len, &done, &err)) {
        if (err == RIAK_OK) {
            /* the caller is done; unread messages make the connection unusable */
            k->resp->stopped = true;
            k->st = RIAK_OK;
            k->finished = true;
            k->state = RIAK_PBC_BROKEN;
            return;
        }
        fail(k, err,
             err == RIAK_E_NOMEM ? "out of memory while reading the response"
                                 : "malformed response message");
        return;
    }
    if (done) {
        k->st = RIAK_OK;
        k->finished = true;
        k->state = RIAK_PBC_READY;
    }
}

/* ---- public engine API ---------------------------------------------- */

riak_pbconn *riak_pbconn_open(riak_client *c, size_t node) {
    riak_pbconn *k = calloc(1, sizeof *k);
    if (k == NULL || (k->rbuf = malloc(RBUF_SIZE)) == NULL) {
        free(k);
        riak_set_error(c, RIAK_E_NOMEM, "out of memory");
        return NULL;
    }
    k->c = c;
    k->node = node;
    k->fd = -1;
    k->state = RIAK_PBC_CONNECTING;
    long ct =
        c->cfg.connect_timeout_ms > 0 ? c->cfg.connect_timeout_ms : DEFAULT_CONNECT_TIMEOUT_MS;
    k->connect_deadline = riak_now_ms() + ct;
    riak_status st = riak_pb_resolve(c, node, &k->addrs, &k->addr_lens, &k->n_addrs);
    if (st != RIAK_OK) {
        /* resolution failed: reported when the request runs */
        (void)snprintf(k->errmsg, sizeof k->errmsg, "%.200s", c->errmsg);
        k->st = st;
        k->state = RIAK_PBC_BROKEN;
    }
    return k;
}

void riak_pbconn_free(riak_pbconn *k) {
    if (k == NULL) {
        return;
    }
    if (k->ssl) {
        SSL_free(k->ssl); /* no close_notify: the socket is simply closed */
    }
    close_fd(k);
    riak_buf_free(&k->out);
    riak_buf_free(&k->in);
    free(k->rbuf);
    free(k->addrs);
    free(k->addr_lens);
    free(k);
}

bool riak_pbconn_reusable(riak_pbconn *k) {
    if (k->state != RIAK_PBC_READY || k->fd < 0 || k->rpos != k->rlen || k->hdr_len != 0 ||
        (k->ssl && SSL_pending(k->ssl) > 0)) {
        return false;
    }
    /* an idle connection must have nothing to read: data or EOF means the
     * server closed it or the stream is out of step */
    struct pollfd p = {.fd = k->fd, .events = POLLIN};
    return poll(&p, 1, 0) == 0;
}

void riak_pbconn_begin(riak_pbconn *k, const riak_pb_request *req, riak_pb_response *resp,
                       unsigned attempt) {
    k->req = req;
    k->resp = resp;
    k->attempt = attempt;
    k->started_ms = riak_now_ms();
    k->sent = false;
    k->delivered = false;
    k->received = 0;
    k->finished = false;
    k->st = RIAK_OK;
    k->retry = RIAK_RETRY_NONE;
    k->errmsg[0] = '\0';
    k->want = 0;
    k->request_deadline = k->c->cfg.timeout_ms > 0 ? k->started_ms + k->c->cfg.timeout_ms : 0;
    if (k->state == RIAK_PBC_BROKEN) {
        /* resolution failed in riak_pbconn_open() */
        riak_status st = k->st != RIAK_OK ? k->st : RIAK_E_TRANSPORT;
        char msg[sizeof k->errmsg];
        (void)snprintf(msg, sizeof msg, "%s", k->errmsg[0] ? k->errmsg : "connection unusable");
        k->state = RIAK_PBC_CONNECTING; /* lets fail() record the outcome */
        fail(k, st, "%s", msg);
    }
}

int riak_pbconn_step(riak_pbconn *k) {
    for (;;) {
        if (k->finished) {
            k->want = 0;
            return 0;
        }
        int r;
        switch (k->state) {
        case RIAK_PBC_CONNECTING:
            if (k->fd < 0) {
                r = connect_next(k);
            } else {
                /* a connect in progress: writable means done or failed */
                struct pollfd p = {.fd = k->fd, .events = POLLOUT};
                if (poll(&p, 1, 0) == 0) {
                    k->want = RIAK_PBC_WANT_WRITE;
                    return k->want;
                }
                int err = 0;
                socklen_t len = sizeof err;
                if (getsockopt(k->fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
                    err = errno;
                }
                r = err == 0 ? 1 : connect_next(k);
            }
            if (r == 0) {
                return k->want;
            }
            if (r > 0) {
                after_connect(k);
            }
            break;
        case RIAK_PBC_STARTTLS:
            r = pump(k);
            if (r == 0) {
                return k->want;
            }
            if (r > 0 && setup_reply(k, RIAK_PB_START_TLS, "STARTTLS") && start_tls(k)) {
                k->state = RIAK_PBC_HANDSHAKE;
            }
            break;
        case RIAK_PBC_HANDSHAKE: {
            ERR_clear_error();
            errno = 0;
            int ret = SSL_do_handshake(k->ssl);
            if (ret == 1) {
                after_handshake(k);
            } else if (ssl_result(k, ret, "TLS handshake") == 0) {
                return k->want;
            }
            break;
        }
        case RIAK_PBC_AUTH:
            r = pump(k);
            if (r == 0) {
                return k->want;
            }
            if (r > 0 && setup_reply(k, RIAK_PB_AUTH_RESP, "authentication")) {
                after_auth(k);
            }
            break;
        case RIAK_PBC_CLIENT_ID:
            r = pump(k);
            if (r == 0) {
                return k->want;
            }
            if (r > 0 && setup_reply(k, RIAK_PB_SET_CLIENT_ID_RESP, "client id")) {
                k->state = RIAK_PBC_READY;
            }
            break;
        case RIAK_PBC_READY:
            if (k->req == NULL) {
                k->want = 0;
                return 0;
            }
            start_request(k);
            break;
        case RIAK_PBC_BUSY:
            r = pump(k);
            if (r == 0) {
                return k->want;
            }
            if (r > 0) {
                handle_response(k);
            }
            break;
        case RIAK_PBC_BROKEN:
            fail(k, RIAK_E_TRANSPORT, "connection unusable");
            break;
        }
    }
}

long long riak_pbconn_deadline(const riak_pbconn *k) {
    long long d = k->request_deadline;
    if (k->state < RIAK_PBC_READY && (d == 0 || k->connect_deadline < d)) {
        d = k->connect_deadline;
    }
    return d;
}

void riak_pbconn_timeout(riak_pbconn *k) {
    bool connecting = k->state < RIAK_PBC_READY;
    bool sent = k->sent;
    if (connecting) {
        /* like curl: a connect timeout is not a refused connection */
        k->sent = true;
    }
    fail(k, RIAK_E_TRANSPORT, "%s to %s:%u timed out", connecting ? "connection" : "request",
         node_name(k), node_port(k));
    k->sent = sent;
}

riak_status riak_pbconn_finish(riak_pbconn *k, enum riak_retry *retry) {
    riak_client *c = k->c;
    const riak_pb_request *req = k->req;
    c->last_node = k->node;
    c->http_status = 0;
    riak_clear_error(c);
    riak_status st = k->st;
    if (st != RIAK_OK) {
        riak_set_error(c, st, "%s", k->errmsg);
    }
    *retry = k->retry;
    riak_node_result(c, k->node, *retry == RIAK_RETRY_NONE);
    bool again = req->pin == 0 && riak_retry_allowed(c, req->idempotent, *retry, k->attempt + 1);
    riak_attempt_info info = {.method = req->name,
                              .path = req->path,
                              .node = k->node,
                              .attempt = k->attempt,
                              .status = st,
                              .started_ms = k->started_ms,
                              .received = k->received,
                              .retry = *retry,
                              .will_retry = again,
                              .failure = "server overloaded",
                              .transport = RIAK_TRANSPORT_PB};
    riak_attempt_report(c, &info);
    k->req = NULL;
    k->resp = NULL;
    k->finished = false;
    return st;
}
