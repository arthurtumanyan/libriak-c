/*
 File:   cmd_kv.c
 SPDX-License-Identifier: GPL-3.0-or-later

 ping, info, stats, get, put, delete and list.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "riakctl.h"

enum {
    OPT_R = 256,
    OPT_PR,
    OPT_W,
    OPT_DW,
    OPT_PW,
    OPT_RW,
    OPT_HEAD,
    OPT_BASIC_QUORUM,
    OPT_NOTFOUND_OK,
    OPT_VALUE,
    OPT_LINK,
    OPT_VCLOCK,
    OPT_CREATE,
    OPT_RETURN_BODY,
    OPT_ALL
};

/* Quorum options shared by get, put and delete; false for other options. */
static bool quorum_opt(int ch, const char *arg, riak_opts *o, bool *bad) {
    int *field;
    switch (ch) {
    case OPT_R:
        field = &o->r;
        break;
    case OPT_PR:
        field = &o->pr;
        break;
    case OPT_W:
        field = &o->w;
        break;
    case OPT_DW:
        field = &o->dw;
        break;
    case OPT_PW:
        field = &o->pw;
        break;
    case OPT_RW:
        field = &o->rw;
        break;
    default:
        return false;
    }
    *bad = !rc_parse_quorum(arg, field);
    return true;
}

int rc_cmd_ping(rc_ctx *x, int argc, char **argv) {
    static const struct option opts[] = {{"all", no_argument, NULL, OPT_ALL}, {NULL, 0, NULL, 0}};
    bool all = false;
    int ch;
    while ((ch = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        if (ch != OPT_ALL) {
            return rc_usage_error("ping", "unknown option");
        }
        all = true;
    }
    /* the configuration is checked once; each node is then pinged through
     * its own client, without retries */
    if (rc_client(x) == NULL) {
        return RC_USAGE;
    }
    rc_conn *cn = &x->conn;
    size_t total = cn->n_nodes;
    size_t up = 0;
    json_object *o = json_object_new_object();
    json_object *nodes = json_object_new_array();
    for (size_t i = 0; i < total; i++) {
        riak_config cfg = cn->cfg;
        cfg.nodes = &cn->nodes[i];
        cfg.n_nodes = 1;
        cfg.max_retries = 0;
        riak_client *c = NULL;
        riak_status st = riak_client_new(&cfg, &c);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (st == RIAK_OK) {
            st = riak_ping(c);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        json_object *n = json_object_new_object();
        json_object_object_add(n, "host", json_object_new_string(cn->nodes[i].host));
        json_object_object_add(n, "port", json_object_new_int((int)cn->nodes[i].port));
        json_object_object_add(n, "up", json_object_new_boolean(st == RIAK_OK));
        if (st == RIAK_OK) {
            double ms =
                (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
            char ms_text[32];
            snprintf(ms_text, sizeof ms_text, "%.2f", ms);
            json_object_object_add(n, "ms", json_object_new_double_s(ms, ms_text));
            up++;
        } else {
            json_object_object_add(n, "error", json_object_new_string(riak_strerror(st)));
        }
        json_object_array_add(nodes, n);
        riak_client_free(c);
    }
    json_object_object_add(o, "nodes", nodes);
    json_object_object_add(o, "up", json_object_new_int64((int64_t)up));
    json_object_object_add(o, "total", json_object_new_int64((int64_t)total));
    int rc = rc_print(x, o);
    if (rc != RC_OK) {
        return rc;
    }
    if (up == 0 || (all && up < total)) {
        fprintf(stderr, "riakctl: ping: %zu of %zu nodes answered\n", up, total);
        return RC_UNREACHABLE;
    }
    return RC_OK;
}

int rc_cmd_info(rc_ctx *x, int argc, char **argv) {
    (void)argv;
    if (argc > 1) {
        return rc_usage_error("info", "no arguments expected");
    }
    riak_client *c = rc_client(x);
    if (c == NULL) {
        return RC_ERROR;
    }
    riak_server_info si = {0};
    riak_status st = riak_get_server_info(c, &si);
    if (st != RIAK_OK) {
        return rc_fail(c, "server info", st);
    }
    json_object *o = json_object_new_object();
    json_object_object_add(o, "node", json_object_new_string(si.node ? si.node : ""));
    json_object_object_add(o, "version", json_object_new_string(si.version ? si.version : ""));
    riak_server_info_free(&si);
    char ver[32];
    snprintf(ver, sizeof ver, "%d.%d.%d", RIAK_VERSION_MAJOR, RIAK_VERSION_MINOR,
             RIAK_VERSION_PATCH);
    json_object_object_add(o, "client_version", json_object_new_string(ver));
    json_object_object_add(
        o, "transport",
        json_object_new_string(x->conn.cfg.transport == RIAK_TRANSPORT_PB ? "pb" : "http"));
    json_object_object_add(o, "tls", json_object_new_boolean(x->conn.cfg.tls));

    json_object *nodes = json_object_new_array();
    for (size_t i = 0; i < riak_client_node_count(c); i++) {
        riak_node_info ni;
        if (riak_client_node_info(c, i, &ni) != RIAK_OK) {
            continue;
        }
        json_object *n = json_object_new_object();
        json_object_object_add(n, "host", json_object_new_string(ni.host));
        json_object_object_add(n, "port", json_object_new_int((int)ni.port));
        json_object_object_add(n, "healthy", json_object_new_boolean(ni.healthy));
        json_object_object_add(n, "requests", json_object_new_int64((int64_t)ni.requests));
        json_object_object_add(n, "failures", json_object_new_int64((int64_t)ni.failures));
        json_object_array_add(nodes, n);
    }
    json_object_object_add(o, "client_nodes", nodes);

    /* members that are up (Riak 3.0+, Protocol Buffers); omitted elsewhere */
    riak_members m = {0};
    if (riak_cluster_membership(c, &m) == RIAK_OK) {
        json_object *members = json_object_new_array();
        for (size_t i = 0; i < m.count; i++) {
            json_object *n = json_object_new_object();
            json_object_object_add(n, "ip", json_object_new_string(m.items[i].ip));
            json_object_object_add(n, "port", json_object_new_int((int)m.items[i].port));
            json_object_array_add(members, n);
        }
        json_object_object_add(o, "members", members);
        riak_members_free(&m);
    }
    return rc_print(x, o);
}

static bool stat_selected(const char *name, int argc, char **argv) {
    if (argc <= 1) {
        return true;
    }
    for (int i = 1; i < argc; i++) {
        size_t len = strlen(argv[i]);
        if (len > 0 && argv[i][len - 1] == '*' ? strncmp(name, argv[i], len - 1) == 0
                                               : strcmp(name, argv[i]) == 0) {
            return true;
        }
    }
    return false;
}

int rc_cmd_stats(rc_ctx *x, int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            return rc_usage_error("stats", "unknown option '%s'", argv[i]);
        }
    }
    riak_client *c = rc_client(x);
    if (c == NULL) {
        return RC_ERROR;
    }
    char *json = NULL;
    riak_status st = riak_stats(c, &json);
    if (st != RIAK_OK) {
        if (st == RIAK_E_UNSUPPORTED) {
            rc_warn("stats need the HTTP port of the node (--http-port)");
        }
        return rc_fail(c, "stats", st);
    }
    json_object *all = rc_parse_json(json, "stats");
    riak_free(json);
    if (all == NULL) {
        return RC_ERROR;
    }
    if (argc <= 1) {
        return rc_print(x, all);
    }
    json_object *o = json_object_new_object();
    json_object_object_foreach(all, name, val) {
        if (stat_selected(name, argc, argv)) {
            json_object_object_add(o, name, json_object_get(val));
        }
    }
    json_object_put(all);
    if (json_object_object_length(o) == 0) {
        json_object_put(o);
        fprintf(stderr, "riakctl: stats: no matching statistics\n");
        return RC_NOT_FOUND;
    }
    return rc_print(x, o);
}

static int print_siblings(rc_ctx *x, const char *bucket, const char *key,
                          const riak_object_set *set, bool with_value) {
    json_object *o = json_object_new_object();
    json_object_object_add(o, "bucket", json_object_new_string(bucket));
    json_object_object_add(o, "key", json_object_new_string(key));
    if (x->type) {
        json_object_object_add(o, "type", json_object_new_string(x->type));
    }
    if (set->vclock) {
        json_object_object_add(o, "vclock", json_object_new_string(set->vclock));
    }
    json_object *sibs = json_object_new_array();
    for (size_t i = 0; i < set->count; i++) {
        json_object_array_add(sibs, rc_object_json(&set->items[i], with_value));
    }
    json_object_object_add(o, "siblings", sibs);
    return rc_print(x, o);
}

int rc_cmd_get(rc_ctx *x, int argc, char **argv) {
    static const struct option opts[] = {{"type", required_argument, NULL, 't'},
                                         {"json", no_argument, NULL, 'j'},
                                         {"all", no_argument, NULL, 'a'},
                                         {"head", no_argument, NULL, OPT_HEAD},
                                         {"r", required_argument, NULL, OPT_R},
                                         {"pr", required_argument, NULL, OPT_PR},
                                         {"basic-quorum", no_argument, NULL, OPT_BASIC_QUORUM},
                                         {"notfound-ok", no_argument, NULL, OPT_NOTFOUND_OK},
                                         {NULL, 0, NULL, 0}};
    riak_opts o = {0};
    bool json = false;
    bool all = false;
    bool bad = false;
    int ch;
    while ((ch = getopt_long(argc, argv, "t:ja", opts, NULL)) != -1) {
        if (quorum_opt(ch, optarg, &o, &bad)) {
            if (bad) {
                return rc_usage_error("get", "invalid quorum '%s'", optarg);
            }
            continue;
        }
        switch (ch) {
        case 't':
            x->type = optarg;
            break;
        case 'j':
            json = true;
            break;
        case 'a':
            all = true;
            break;
        case OPT_HEAD:
            o.head = true;
            json = true;
            break;
        case OPT_BASIC_QUORUM:
            o.basic_quorum = true;
            break;
        case OPT_NOTFOUND_OK:
            o.notfound_ok = true;
            break;
        default:
            return rc_usage_error("get", "unknown option");
        }
    }
    if (argc - optind != 2) {
        return rc_usage_error("get", "BUCKET and KEY expected");
    }
    const char *bucket = argv[optind];
    const char *key = argv[optind + 1];
    o.bucket_type = x->type;
    riak_client *c = rc_client(x);
    if (c == NULL) {
        return RC_ERROR;
    }
    riak_status st;
    if (all) {
        riak_object_set set = {0};
        if ((st = riak_get_siblings(c, bucket, key, &o, &set)) != RIAK_OK) {
            return rc_fail(c, "get", st);
        }
        int rc = print_siblings(x, bucket, key, &set, !o.head);
        riak_object_set_free(&set);
        return rc;
    }
    riak_object obj = {0};
    if ((st = riak_get(c, bucket, key, &o, &obj)) != RIAK_OK) {
        return rc_fail(c, "get", st);
    }
    int rc = RC_OK;
    if (json) {
        json_object *j = rc_object_json(&obj, !o.head);
        if (j && obj.vclock) {
            json_object_object_add(j, "vclock", json_object_new_string(obj.vclock));
        }
        rc = rc_print(x, j);
    } else if (obj.len && fwrite(obj.data, 1, obj.len, stdout) != obj.len) {
        rc_warn("write error");
        rc = RC_ERROR;
    }
    riak_object_free(&obj);
    return rc;
}

int rc_cmd_put(rc_ctx *x, int argc, char **argv) {
    static const struct option opts[] = {{"type", required_argument, NULL, 't'},
                                         {"content-type", required_argument, NULL, 'c'},
                                         {"value", required_argument, NULL, OPT_VALUE},
                                         {"file", required_argument, NULL, 'f'},
                                         {"meta", required_argument, NULL, 'm'},
                                         {"index", required_argument, NULL, 'i'},
                                         {"link", required_argument, NULL, OPT_LINK},
                                         {"vclock", required_argument, NULL, OPT_VCLOCK},
                                         {"create", no_argument, NULL, OPT_CREATE},
                                         {"return-body", no_argument, NULL, OPT_RETURN_BODY},
                                         {"w", required_argument, NULL, OPT_W},
                                         {"dw", required_argument, NULL, OPT_DW},
                                         {"pw", required_argument, NULL, OPT_PW},
                                         {NULL, 0, NULL, 0}};
    riak_opts o = {0};
    const char *value = NULL;
    const char *file = NULL;
    rc_strv meta = {0}, index = {0}, links = {0};
    riak_kv_pair *pairs = NULL;
    char **owned = NULL; /* names and values of pairs */
    char *data = NULL;
    size_t len = 0;
    int rc = RC_USAGE;
    bool bad = false;
    int ch;
    while ((ch = getopt_long(argc, argv, "t:c:f:m:i:", opts, NULL)) != -1) {
        if (quorum_opt(ch, optarg, &o, &bad)) {
            if (bad) {
                rc = rc_usage_error("put", "invalid quorum '%s'", optarg);
                goto out;
            }
            continue;
        }
        switch (ch) {
        case 't':
            x->type = optarg;
            break;
        case 'c':
            o.content_type = optarg;
            break;
        case OPT_VALUE:
            value = optarg;
            break;
        case 'f':
            file = optarg;
            break;
        case 'm':
        case 'i':
        case OPT_LINK:
            if (!rc_strv_push(ch == 'm' ? &meta : ch == 'i' ? &index : &links, optarg)) {
                rc = RC_ERROR;
                goto out;
            }
            break;
        case OPT_VCLOCK:
            o.vclock = optarg;
            break;
        case OPT_CREATE:
            o.if_none_match = "*";
            break;
        case OPT_RETURN_BODY:
            o.return_body = true;
            break;
        default:
            rc = rc_usage_error("put", "unknown option");
            goto out;
        }
    }
    if (argc - optind < 1 || argc - optind > 2) {
        rc = rc_usage_error("put", "BUCKET and an optional KEY expected");
        goto out;
    }
    if (value && file) {
        rc = rc_usage_error("put", "--value and --file exclude each other");
        goto out;
    }
    const char *bucket = argv[optind];
    const char *key = argc - optind == 2 ? argv[optind + 1] : NULL;

    /* metadata and index entries share one array: meta first, then indexes */
    pairs = calloc(meta.count + index.count + 1, sizeof *pairs);
    owned = calloc(2 * (meta.count + index.count) + 1, sizeof *owned);
    if (pairs == NULL || owned == NULL) {
        rc = RC_ERROR;
        goto out;
    }
    for (size_t i = 0; i < meta.count + index.count; i++) {
        const char *s = i < meta.count ? meta.items[i] : index.items[i - meta.count];
        char *n, *v;
        if (!rc_split_pair(s, &n, &v)) {
            rc = rc_usage_error("put", "NAME=VALUE expected, got '%s'", s);
            goto out;
        }
        owned[2 * i] = n;
        owned[2 * i + 1] = v;
        pairs[i].name = n;
        pairs[i].value = v;
    }
    o.meta = pairs;
    o.n_meta = meta.count;
    o.indexes = pairs + meta.count;
    o.n_indexes = index.count;
    o.links = links.items;
    o.n_links = links.count;
    o.bucket_type = x->type;

    if (value) {
        len = strlen(value);
        data = strdup(value);
    } else {
        data = rc_read_file(file ? file : "-", &len);
    }
    if (data == NULL) {
        rc = RC_ERROR;
        goto out;
    }
    riak_client *c = rc_client(x);
    if (c == NULL) {
        rc = RC_ERROR;
        goto out;
    }
    riak_object stored = {0};
    char *created = NULL;
    riak_status st = riak_put(c, bucket, key, data, len, &o, o.return_body ? &stored : NULL,
                              key ? NULL : &created);
    if (st != RIAK_OK) {
        rc = rc_fail(c, "put", st);
        goto out;
    }
    if (o.return_body || created) {
        json_object *j = o.return_body ? rc_object_json(&stored, true) : json_object_new_object();
        if (j && created) {
            json_object_object_add(j, "key", json_object_new_string(created));
        }
        if (j && stored.vclock) {
            json_object_object_add(j, "vclock", json_object_new_string(stored.vclock));
        }
        rc = rc_print(x, j);
    } else {
        rc = RC_OK;
    }
    riak_object_free(&stored);
    riak_free(created);
out:
    if (owned) {
        for (size_t i = 0; i < 2 * (meta.count + index.count); i++) {
            free(owned[i]);
        }
        free(owned);
    }
    free(pairs);
    free(data);
    rc_strv_free(&meta);
    rc_strv_free(&index);
    rc_strv_free(&links);
    return rc;
}

int rc_cmd_delete(rc_ctx *x, int argc, char **argv) {
    static const struct option opts[] = {{"type", required_argument, NULL, 't'},
                                         {"vclock", required_argument, NULL, OPT_VCLOCK},
                                         {"rw", required_argument, NULL, OPT_RW},
                                         {"w", required_argument, NULL, OPT_W},
                                         {"dw", required_argument, NULL, OPT_DW},
                                         {"pw", required_argument, NULL, OPT_PW},
                                         {"r", required_argument, NULL, OPT_R},
                                         {"pr", required_argument, NULL, OPT_PR},
                                         {NULL, 0, NULL, 0}};
    riak_opts o = {0};
    bool bad = false;
    int ch;
    while ((ch = getopt_long(argc, argv, "t:", opts, NULL)) != -1) {
        if (quorum_opt(ch, optarg, &o, &bad)) {
            if (bad) {
                return rc_usage_error("delete", "invalid quorum '%s'", optarg);
            }
            continue;
        }
        switch (ch) {
        case 't':
            x->type = optarg;
            break;
        case OPT_VCLOCK:
            o.vclock = optarg;
            break;
        default:
            return rc_usage_error("delete", "unknown option");
        }
    }
    if (argc - optind != 2) {
        return rc_usage_error("delete", "BUCKET and KEY expected");
    }
    o.bucket_type = x->type;
    riak_client *c = rc_client(x);
    if (c == NULL) {
        return RC_ERROR;
    }
    riak_status st = riak_delete(c, argv[optind], argv[optind + 1], &o);
    return st == RIAK_OK ? RC_OK : rc_fail(c, "delete", st);
}

typedef struct {
    bool plain;
    bool failed;
} list_ctx;

static bool list_name(const char *name, size_t len, void *ctx) {
    list_ctx *l = ctx;
    if (l->plain) {
        l->failed = fwrite(name, 1, len, stdout) != len || fputc('\n', stdout) == EOF;
    } else {
        l->failed = rc_print_line(json_object_new_string_len(name, (int)len)) != RC_OK;
    }
    return !l->failed;
}

int rc_cmd_list(rc_ctx *x, int argc, char **argv) {
    static const struct option opts[] = {{"type", required_argument, NULL, 't'},
                                         {"plain", no_argument, NULL, 'p'},
                                         {NULL, 0, NULL, 0}};
    list_ctx l = {0};
    int ch;
    while ((ch = getopt_long(argc, argv, "t:p", opts, NULL)) != -1) {
        switch (ch) {
        case 't':
            x->type = optarg;
            break;
        case 'p':
            l.plain = true;
            break;
        default:
            return rc_usage_error("list", "unknown option");
        }
    }
    int n = argc - optind;
    bool buckets = n == 1 && strcmp(argv[optind], "buckets") == 0;
    bool keys = n == 2 && strcmp(argv[optind], "keys") == 0;
    if (!buckets && !keys) {
        return rc_usage_error("list", "'buckets' or 'keys BUCKET' expected");
    }
    riak_client *c = rc_client(x);
    if (c == NULL) {
        return RC_ERROR;
    }
    riak_status st = buckets ? riak_list_buckets_stream(c, x->type, list_name, &l)
                             : riak_list_keys_stream(c, x->type, argv[optind + 1], list_name, &l);
    if (l.failed) {
        rc_warn("write error");
        return RC_ERROR;
    }
    return st == RIAK_OK ? RC_OK : rc_fail(c, "list", st);
}
