/*
 File:   client.c
 Copyright (C) 2013-2026  Arthur Tumanyan <arthurtumanyan@gmail.com>
 SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RIAK_DEFAULT_HOST "127.0.0.1"
#define RIAK_DEFAULT_PORT 8098u
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
    cfg->host = RIAK_DEFAULT_HOST;
    cfg->port = RIAK_DEFAULT_PORT;
    cfg->tls_verify = true;
    cfg->max_body_bytes = RIAK_DEFAULT_MAX_BODY;
}

static bool host_ok(const char *h) {
    return *h != '\0' && strpbrk(h, "/?#@ \t\r\n") == NULL;
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
    const char *host = (cfg->host && *cfg->host) ? cfg->host : RIAK_DEFAULT_HOST;
    unsigned port = cfg->port ? cfg->port : RIAK_DEFAULT_PORT;
    if (!host_ok(host) || port > 65535 ||
        (cfg->client_id && !riak_header_value_ok(cfg->client_id))) {
        return RIAK_E_INVALID_ARG;
    }

    riak_client *c = calloc(1, sizeof *c);
    if (c == NULL) {
        return RIAK_E_NOMEM;
    }
    c->cfg = *cfg;
    c->host = riak_strdup(host);
    c->ca_file = riak_strdup(cfg->ca_file);
    c->client_id = riak_strdup(cfg->client_id);
    /* bracket bare IPv6 literals */
    bool v6 = strchr(host, ':') != NULL && host[0] != '[';
    c->base_url = riak_strprintf("%s://%s%s%s:%u/", cfg->tls ? "https" : "http", v6 ? "[" : "",
                                 host, v6 ? "]" : "", port);
    c->curl = curl_easy_init();
    if (c->host == NULL || c->base_url == NULL || c->curl == NULL ||
        (cfg->ca_file && c->ca_file == NULL) || (cfg->client_id && c->client_id == NULL)) {
        riak_client_free(c);
        return RIAK_E_NOMEM;
    }
    c->cfg.host = c->host;
    c->cfg.port = port;
    if (c->cfg.max_body_bytes == 0) {
        c->cfg.max_body_bytes = RIAK_DEFAULT_MAX_BODY;
    }
    c->cfg.ca_file = c->ca_file;
    c->cfg.client_id = c->client_id;
    *out = c;
    return RIAK_OK;
}

void riak_client_free(riak_client *client) {
    if (client == NULL) {
        return;
    }
    if (client->curl) {
        curl_easy_cleanup(client->curl);
    }
    free(client->base_url);
    free(client->host);
    free(client->ca_file);
    free(client->client_id);
    free(client);
}

riak_status riak_ping(riak_client *client) {
    if (client == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    riak_buf url = {0};
    if (riak_buf_append_str(&url, client->base_url) != 0 ||
        riak_buf_append_str(&url, "ping") != 0) {
        riak_buf_free(&url);
        return riak_set_error(client, RIAK_E_NOMEM, "out of memory");
    }
    riak_request req = {.method = "GET", .url = url.data};
    riak_response resp = {0};
    riak_status st = riak_perform(client, &req, &resp);
    riak_buf_free(&url);
    if (st != RIAK_OK) {
        return st;
    }
    if (resp.status != 200) {
        st = riak_http_error(client, &resp);
    } else if (resp.body.len < 2 || strncmp(resp.body.data, "OK", 2) != 0) {
        st = riak_set_error(client, RIAK_E_PARSE, "unexpected ping reply");
    }
    riak_response_free(&resp);
    return st;
}
