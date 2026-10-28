/*
 File:   client.c
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "internal.h"

#include "parse.h"
#include "pb.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RIAK_DEFAULT_HOST "127.0.0.1"
#define RIAK_DEFAULT_HTTP_PORT 8098u
#define RIAK_DEFAULT_PB_PORT 8087u
#define RIAK_DEFAULT_MAX_BODY ((size_t)64 * 1024 * 1024)

riak_status riak_global_init(void) {
    return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK ? RIAK_OK : RIAK_E_TRANSPORT;
}

void riak_global_cleanup(void) {
    curl_global_cleanup();
}

const char *riak_strerror(riak_status status) {
    switch (status) {
    case RIAK_OK:
        return "success";
    case RIAK_E_INVALID_ARG:
        return "invalid argument";
    case RIAK_E_NOMEM:
        return "out of memory";
    case RIAK_E_TRANSPORT:
        return "transport error";
    case RIAK_E_TLS:
        return "TLS error";
    case RIAK_E_NOT_FOUND:
        return "not found";
    case RIAK_E_SIBLINGS:
        return "object has siblings";
    case RIAK_E_PRECONDITION:
        return "precondition failed";
    case RIAK_E_HTTP:
        return "unexpected HTTP status";
    case RIAK_E_PARSE:
        return "malformed response";
    case RIAK_E_NOT_MODIFIED:
        return "not modified";
    case RIAK_E_UNAUTHORIZED:
        return "unauthorized";
    case RIAK_E_CANCELLED:
        return "cancelled";
    case RIAK_E_UNSUPPORTED:
        return "not supported by the transport";
    case RIAK_E_SERVER:
        return "server error";
    }
    return "unknown error";
}

riak_status riak_set_error(riak_client *c, riak_status st, const char *fmt, ...) {
    if (c != NULL) {
        va_list ap;
        va_start(ap, fmt);
        (void)vsnprintf(c->errmsg, sizeof c->errmsg, fmt, ap);
        va_end(ap);
    }
    return st;
}

void riak_log(riak_client *c, riak_log_level level, const char *fmt, ...) {
    if (c->cfg.log_fn == NULL || level < c->cfg.log_level) {
        return;
    }
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    c->cfg.log_fn(level, msg, c->cfg.log_ctx);
}

void riak_clear_error(riak_client *c) {
    c->errmsg[0] = '\0';
}

const char *riak_client_errmsg(const riak_client *client) {
    return client ? client->errmsg : "no client";
}

long riak_client_http_status(const riak_client *client) {
    return client ? client->http_status : 0;
}

riak_status riak_check_name(riak_client *c, const char *s, const char *what) {
    if (s == NULL || *s == '\0') {
        return riak_set_error(c, RIAK_E_INVALID_ARG, "%s must not be empty", what);
    }
    return RIAK_OK;
}

void riak_config_init(riak_config *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->transport = RIAK_TRANSPORT_PB;
    cfg->host = RIAK_DEFAULT_HOST;
    cfg->port = 0; /* the transport's default */
    cfg->tls_verify = true;
    cfg->max_body_bytes = RIAK_DEFAULT_MAX_BODY;
    cfg->max_retries = 2;
    cfg->retry_base_ms = 50;
    cfg->retry_max_ms = 2000;
    cfg->fail_threshold = 3;
    cfg->eject_ms = 5000;
    cfg->log_level = RIAK_LOG_WARN;
}

/* A secret is overwritten before it is freed; volatile keeps the stores. */
static void free_secret(char *s) {
    if (s) {
        for (volatile char *p = s; *p; p++) {
            *p = '\0';
        }
        free(s);
    }
}

static bool copy_opt(char **dst, const char *src) {
    if (src == NULL) {
        return true;
    }
    *dst = riak_strdup(src);
    return *dst != NULL;
}

static bool host_ok(const char *h) {
    return h != NULL && *h != '\0' && strpbrk(h, "/?#@ \t\r\n") == NULL;
}

static riak_status add_node(riak_client *c, size_t i, const char *host, unsigned port,
                            unsigned http_port) {
    if (!host_ok(host) || port > 65535 || http_port > 65535) {
        return RIAK_E_INVALID_ARG;
    }
    bool pb = riak_is_pb(c);
    riak_node_state *n = &c->nodes[i];
    n->port = port ? port : pb ? RIAK_DEFAULT_PB_PORT : RIAK_DEFAULT_HTTP_PORT;
    n->host = riak_strdup(host);
    if (n->host == NULL) {
        return RIAK_E_NOMEM;
    }
    /* HTTP requests go to the node port, or under Protocol Buffers to the
     * optional HTTP port (riak_stats) */
    unsigned hp = pb ? http_port : n->port;
    if (hp == 0) {
        return RIAK_OK;
    }
    /* bracket bare IPv6 literals */
    bool v6 = strchr(host, ':') != NULL && host[0] != '[';
    n->base = riak_strprintf("%s://%s%s%s:%u", c->cfg.tls ? "https" : "http", v6 ? "[" : "", host,
                             v6 ? "]" : "", hp);
    return n->base ? RIAK_OK : RIAK_E_NOMEM;
}

riak_status riak_client_new(const riak_config *cfg, riak_client **out) {
    if (out == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    *out = NULL;

    riak_config defaults;
    riak_config_init(&defaults);
    if (cfg == NULL) {
        cfg = &defaults;
    }
    if ((cfg->client_id && !riak_header_value_ok(cfg->client_id)) ||
        (cfg->n_nodes > 0 && cfg->nodes == NULL) || cfg->n_nodes > 1024 ||
        (unsigned)cfg->balance > RIAK_BALANCE_FAILOVER ||
        (unsigned)cfg->transport > RIAK_TRANSPORT_HTTP || cfg->retry_base_ms < 0 ||
        cfg->retry_max_ms < 0 || cfg->eject_ms < 0 || (unsigned)cfg->log_level > RIAK_LOG_ERROR ||
        /* Riak only accepts credentials over TLS; never send them in clear */
        (cfg->username && !cfg->tls) || (cfg->password && cfg->username == NULL)) {
        return RIAK_E_INVALID_ARG;
    }

    riak_client *c = calloc(1, sizeof *c);
    if (c == NULL) {
        return RIAK_E_NOMEM;
    }
    c->cfg = *cfg;
    c->root = "/";
    c->n_nodes = cfg->n_nodes ? cfg->n_nodes : 1;
    c->nodes = calloc(c->n_nodes, sizeof *c->nodes);
    c->curl = curl_easy_init();
    riak_status st = RIAK_OK;
    if (c->nodes == NULL || c->curl == NULL || !copy_opt(&c->ca_file, cfg->ca_file) ||
        !copy_opt(&c->client_id, cfg->client_id) || !copy_opt(&c->username, cfg->username) ||
        !copy_opt(&c->password, cfg->password) || !copy_opt(&c->cert_file, cfg->tls_cert_file) ||
        !copy_opt(&c->key_file, cfg->tls_key_file) ||
        !copy_opt(&c->key_password, cfg->tls_key_password)) {
        st = RIAK_E_NOMEM;
    } else if (cfg->n_nodes == 0) {
        st = add_node(c, 0, (cfg->host && *cfg->host) ? cfg->host : RIAK_DEFAULT_HOST, cfg->port,
                      cfg->http_port);
    } else {
        for (size_t i = 0; st == RIAK_OK && i < cfg->n_nodes; i++) {
            st = add_node(c, i, cfg->nodes[i].host, cfg->nodes[i].port, cfg->nodes[i].http_port);
        }
    }
    if (st == RIAK_OK && riak_is_pb(c)) {
        st = riak_pb_client_init(c);
    }
    if (st != RIAK_OK) {
        riak_client_free(c);
        return st;
    }
    c->cfg.nodes = NULL; /* the caller's array is not kept */
    c->cfg.host = c->nodes[0].host;
    c->cfg.port = c->nodes[0].port;
    c->cfg.ca_file = c->ca_file;
    c->cfg.client_id = c->client_id;
    /* secrets live only in the client's own copies */
    c->cfg.username = c->username;
    c->cfg.password = NULL;
    c->cfg.tls_cert_file = c->cert_file;
    c->cfg.tls_key_file = c->key_file;
    c->cfg.tls_key_password = NULL;
    if (c->cfg.max_body_bytes == 0) c->cfg.max_body_bytes = RIAK_DEFAULT_MAX_BODY;
    if (c->cfg.retry_base_ms == 0) c->cfg.retry_base_ms = 50;
    if (c->cfg.retry_max_ms == 0) c->cfg.retry_max_ms = 2000;
    if (c->cfg.fail_threshold == 0) c->cfg.fail_threshold = 3;
    if (c->cfg.eject_ms == 0) c->cfg.eject_ms = 5000;
    /* seed the jitter generator; quality does not matter, only spread */
    c->rng =
        (unsigned long long)(uintptr_t)c ^ (unsigned long long)time(NULL) ^ 0x9E3779B97F4A7C15ULL;
    long maxconn = c->n_nodes > 5 ? (long)c->n_nodes : 5L;
    curl_easy_setopt(c->curl, CURLOPT_MAXCONNECTS, maxconn);
    *out = c;
    return RIAK_OK;
}

void riak_client_free(riak_client *client) {
    if (client == NULL) {
        return;
    }
    riak_pb_client_free(client);
    if (client->curl) {
        curl_easy_cleanup(client->curl);
    }
    for (size_t i = 0; client->nodes && i < client->n_nodes; i++) {
        free(client->nodes[i].host);
        free(client->nodes[i].base);
    }
    free(client->nodes);
    free(client->ca_file);
    free(client->client_id);
    free(client->username);
    free_secret(client->password);
    free(client->cert_file);
    free(client->key_file);
    free_secret(client->key_password);
    free(client);
}

size_t riak_client_node_count(const riak_client *client) {
    return client ? client->n_nodes : 0;
}

riak_status riak_client_node_info(const riak_client *client, size_t index, riak_node_info *info) {
    if (client == NULL || info == NULL || index >= client->n_nodes) {
        return RIAK_E_INVALID_ARG;
    }
    const riak_node_state *n = &client->nodes[index];
    info->host = n->host;
    info->port = n->port;
    info->healthy = n->ejected_until_ms == 0 || riak_now_ms() >= n->ejected_until_ms;
    info->consecutive_failures = n->consecutive_failures;
    info->requests = n->requests;
    info->failures = n->failures;
    return RIAK_OK;
}

size_t riak_client_check_nodes(riak_client *client) {
    if (client == NULL) {
        return 0;
    }
    size_t healthy = 0;
    for (size_t i = 0; i < client->n_nodes; i++) {
        if (riak_is_pb(client)) {
            healthy += riak_pb_ping(client, i + 1) == RIAK_OK;
            continue;
        }
        riak_request req = {.method = "GET", .url = "/ping", .pin = i + 1};
        riak_response resp = {0};
        riak_status st = riak_perform(client, &req, &resp);
        if (st == RIAK_OK && resp.status == 200) {
            healthy++;
        }
        riak_response_free(&resp);
    }
    return healthy;
}

riak_status riak_prepare_ping(riak_client *c, riak_prepared *p) {
    memset(p, 0, sizeof *p);
    if (riak_buf_append_str(&p->url, c->root) != 0 || riak_buf_append_str(&p->url, "ping") != 0) {
        riak_prepared_free(p);
        return riak_set_error(c, RIAK_E_NOMEM, "out of memory");
    }
    p->req = (riak_request){.method = "GET", .url = p->url.data};
    return RIAK_OK;
}

riak_status riak_finish_ping(riak_client *c, riak_response *resp) {
    if (resp->status != 200) {
        return riak_http_error(c, resp);
    }
    if (resp->body.len < 2 || strncmp(resp->body.data, "OK", 2) != 0) {
        return riak_set_error(c, RIAK_E_PARSE, "unexpected ping reply");
    }
    return RIAK_OK;
}

riak_status riak_ping(riak_client *client) {
    if (client == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    if (riak_is_pb(client)) {
        return riak_pb_ping(client, 0);
    }
    riak_prepared p;
    riak_response resp = {0};
    riak_status st = riak_prepare_ping(client, &p);
    if (st == RIAK_OK) {
        st = riak_perform(client, &p.req, &resp);
        riak_prepared_free(&p);
    }
    if (st == RIAK_OK) {
        st = riak_finish_ping(client, &resp);
    }
    riak_response_free(&resp);
    return st;
}

/* ---- server information ---------------------------------------------- */

void riak_server_info_free(riak_server_info *info) {
    if (info == NULL) {
        return;
    }
    free(info->node);
    free(info->version);
    memset(info, 0, sizeof *info);
}

riak_status riak_get_server_info(riak_client *client, riak_server_info *out) {
    if (client == NULL || out == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    if (riak_is_pb(client)) {
        return riak_pb_server_info(client, out);
    }
    char *json = NULL;
    riak_status st = riak_get_body(client, "/stats", RIAK_BODY_JSON_OBJECT, &json);
    if (st != RIAK_OK) {
        return st;
    }
    st = riak_parse_server_info(json, strlen(json), &out->node, &out->version);
    free(json);
    if (st != RIAK_OK) {
        riak_server_info_free(out);
        return riak_set_error(client, st, "cannot parse /stats");
    }
    return RIAK_OK;
}
