/*
 File:   conn.c
 SPDX-License-Identifier: GPL-3.0-or-later

 Node URLs and client creation. A URL list is a comma-separated list of
 scheme://host[:port] entries sharing one scheme: pb (Protocol Buffers), pbs
 (Protocol Buffers with STARTTLS), http or https. IPv6 literals are written
 in brackets; an entry without a scheme takes the scheme of the first one.
 */

#include <stdlib.h>
#include <string.h>

#include "riakctl.h"

static void log_stderr(riak_log_level level, const char *message, void *ctx) {
    static const char *const names[] = {"debug", "info", "warn", "error"};
    (void)ctx;
    fprintf(stderr, "riakctl: [%s] %s\n", names[level], message);
}

/* Parses one host[:port] entry into n; the host is allocated into *owned. */
static bool parse_host(const char *s, size_t len, riak_node *n, char **owned) {
    const char *end = s + len;
    const char *host = s;
    const char *host_end;
    const char *p;
    if (len > 0 && *s == '[') {
        host = s + 1;
        host_end = memchr(host, ']', (size_t)(end - host));
        if (host_end == NULL) {
            return false;
        }
        p = host_end + 1;
    } else {
        host_end = memchr(s, ':', len);
        if (host_end == NULL) {
            host_end = end;
        }
        p = host_end;
    }
    if (host_end == host) {
        return false;
    }
    unsigned long long port = 0;
    if (p < end) {
        if (*p != ':') {
            return false;
        }
        char buf[8];
        size_t plen = (size_t)(end - p - 1);
        if (plen == 0 || plen >= sizeof buf) {
            return false;
        }
        memcpy(buf, p + 1, plen);
        buf[plen] = '\0';
        if (!rc_parse_uint(buf, 65535, &port) || port == 0) {
            return false;
        }
    }
    char *h = strndup(host, (size_t)(host_end - host));
    if (h == NULL) {
        return false;
    }
    *owned = h;
    n->host = h;
    n->port = (unsigned)port;
    n->http_port = 0;
    return true;
}

int rc_conn_setup(rc_conn *c, const char *urls) {
    const char *scheme = NULL;
    size_t scheme_len = 0;
    size_t cap = 0;
    const char *s = urls;
    while (*s) {
        const char *comma = strchr(s, ',');
        size_t len = comma ? (size_t)(comma - s) : strlen(s);
        while (len > 0 && (*s == ' ' || *s == '\t')) {
            s++;
            len--;
        }
        const char *sep = len >= 3 ? strstr(s, "://") : NULL;
        if (sep && sep < s + len) {
            size_t sl = (size_t)(sep - s);
            if (scheme && (sl != scheme_len || strncmp(scheme, s, sl) != 0)) {
                fprintf(stderr, "riakctl: all URLs must use the same scheme\n");
                return RC_USAGE;
            }
            scheme = s;
            scheme_len = sl;
            len -= sl + 3;
            s = sep + 3;
        }
        /* a path after the host is ignored */
        const char *slash = memchr(s, '/', len);
        size_t hlen = slash ? (size_t)(slash - s) : len;
        if (c->n_nodes == cap) {
            cap = cap ? cap * 2 : 4;
            riak_node *nn = realloc(c->nodes, cap * sizeof *nn);
            if (nn) {
                c->nodes = nn;
            }
            char **nh = realloc(c->hosts, cap * sizeof *nh);
            if (nh) {
                c->hosts = nh;
            }
            if (nn == NULL || nh == NULL) {
                fprintf(stderr, "riakctl: out of memory\n");
                return RC_ERROR;
            }
        }
        if (!parse_host(s, hlen, &c->nodes[c->n_nodes], &c->hosts[c->n_nodes])) {
            fprintf(stderr, "riakctl: invalid node address in '%s'\n", urls);
            return RC_USAGE;
        }
        c->n_nodes++;
        s += len;
        if (*s == ',') {
            s++;
        }
    }
    if (c->n_nodes == 0) {
        fprintf(stderr, "riakctl: no node URL given\n");
        return RC_USAGE;
    }
    riak_config *cfg = &c->cfg;
    if (scheme == NULL || (scheme_len == 2 && strncmp(scheme, "pb", 2) == 0)) {
        cfg->transport = RIAK_TRANSPORT_PB;
    } else if (scheme_len == 3 && strncmp(scheme, "pbs", 3) == 0) {
        cfg->transport = RIAK_TRANSPORT_PB;
        cfg->tls = true;
    } else if (scheme_len == 4 && strncmp(scheme, "http", 4) == 0) {
        cfg->transport = RIAK_TRANSPORT_HTTP;
    } else if (scheme_len == 5 && strncmp(scheme, "https", 5) == 0) {
        cfg->transport = RIAK_TRANSPORT_HTTP;
        cfg->tls = true;
    } else {
        fprintf(stderr, "riakctl: unknown scheme '%.*s' (pb, pbs, http or https)\n",
                (int)scheme_len, scheme);
        return RC_USAGE;
    }
    cfg->nodes = c->nodes;
    cfg->n_nodes = c->n_nodes;
    if (c->verbose) {
        cfg->log_fn = log_stderr;
        cfg->log_level = RIAK_LOG_DEBUG;
    }
    return RC_OK;
}

void rc_conn_free(rc_conn *c) {
    for (size_t i = 0; i < c->n_nodes; i++) {
        free(c->hosts[i]);
    }
    free(c->hosts);
    c->hosts = NULL;
    free(c->nodes);
    c->nodes = NULL;
    c->n_nodes = 0;
    if (c->password) {
        volatile char *p = c->password;
        while (*p) {
            *p++ = '\0';
        }
        free(c->password);
        c->password = NULL;
    }
}

riak_client *rc_client_new(rc_ctx *x) {
    riak_client *c = NULL;
    riak_status st = riak_client_new(&x->conn.cfg, &c);
    if (st != RIAK_OK) {
        fprintf(stderr, "riakctl: cannot create client: %s%s\n", riak_strerror(st),
                st == RIAK_E_INVALID_ARG && x->conn.cfg.username && !x->conn.cfg.tls
                    ? " (a user name needs pbs:// or https://)"
                    : "");
        return NULL;
    }
    return c;
}

riak_client *rc_client(rc_ctx *x) {
    if (x->client == NULL) {
        x->client = rc_client_new(x);
    }
    return x->client;
}

char *rc_read_password_file(const char *path) {
    size_t len;
    char *s = rc_read_file(path, &len);
    if (s == NULL) {
        return NULL;
    }
    /* the first line is the password */
    s[strcspn(s, "\r\n")] = '\0';
    return s;
}
