/*
 File:   cmd_repl.c
 SPDX-License-Identifier: GPL-3.0-or-later

 repl: nextgenrepl queues (Riak 2.9+) and riak_repl.
   repl fetch QUEUE [--max N]   takes changes off a queue (JSON Lines)
   repl push QUEUE              queues the keys read from stdin, JSON Lines
                                {"type", "bucket", "key", "vclock"} as
                                printed by aae keys
   repl members | repl cluster-id | repl get CLUSTER_ID BUCKET KEY
 */

#include <stdlib.h>
#include <string.h>

#include "riakctl.h"

#define PUSH_BATCH 1000

enum { OPT_MAX = 256 };

static int repl_fetch(rc_ctx *x, riak_client *c, const char *queue, unsigned long long max) {
    unsigned long long taken = 0;
    while (max == 0 || taken < max) {
        riak_repl_fetch_result r = {0};
        riak_status st = riak_repl_fetch(c, queue, &r);
        if (st == RIAK_E_NOT_FOUND) {
            continue; /* the queued key no longer exists; the entry is gone */
        }
        if (st != RIAK_OK) {
            return rc_fail(c, "repl fetch", st);
        }
        if (r.queue_empty) {
            riak_repl_fetch_result_free(&r);
            break;
        }
        json_object *o = json_object_new_object();
        json_object_object_add(o, "deleted", json_object_new_boolean(r.deleted));
        if (r.object) {
            char *b = rc_base64_encode(r.object, r.object_len);
            json_object_object_add(o, "object_b64", json_object_new_string(b ? b : ""));
            free(b);
        }
        if (r.deleted_vclock) {
            json_object_object_add(o, "deleted_vclock", json_object_new_string(r.deleted_vclock));
        }
        if (r.has_crc) {
            json_object_object_add(o, "crc", json_object_new_int64(r.crc));
        }
        json_object_object_add(o, "segment_id", json_object_new_int64(r.segment_id));
        json_object_object_add(o, "segment_hash", json_object_new_int64(r.segment_hash));
        riak_repl_fetch_result_free(&r);
        if (rc_print_line(o) != RC_OK) {
            return RC_ERROR;
        }
        taken++;
    }
    (void)x;
    fprintf(stderr, "riakctl: %llu changes taken from %s\n", taken, queue);
    return RC_OK;
}

static int push_batch(riak_client *c, const char *queue, riak_repl_key *keys, size_t n,
                      riak_repl_push_result *res) {
    if (n == 0) {
        return RC_OK;
    }
    riak_status st = riak_repl_push(c, queue, keys, n, res);
    return st == RIAK_OK ? RC_OK : rc_fail(c, "repl push", st);
}

static int repl_push(rc_ctx *x, riak_client *c, const char *queue) {
    riak_repl_key keys[PUSH_BATCH];
    json_object *held[PUSH_BATCH];
    size_t n = 0;
    unsigned long long total = 0;
    riak_repl_push_result res = {0};
    char *line = NULL;
    size_t cap = 0;
    int rc = RC_OK;
    while (rc == RC_OK && getline(&line, &cap, stdin) > 0) {
        if (line[0] == '\n') {
            continue;
        }
        json_object *o = rc_parse_json(line, "repl push input");
        json_object *v;
        if (o == NULL) {
            rc = RC_USAGE;
            break;
        }
        riak_repl_key *k = &keys[n];
        memset(k, 0, sizeof *k);
        if (json_object_object_get_ex(o, "type", &v)) {
            k->bucket_type = json_object_get_string(v);
        }
        if (json_object_object_get_ex(o, "bucket", &v)) {
            k->bucket = json_object_get_string(v);
        }
        if (json_object_object_get_ex(o, "key", &v)) {
            k->key = json_object_get_string(v);
        }
        if (json_object_object_get_ex(o, "vclock", &v)) {
            k->vclock = json_object_get_string(v);
        }
        if (k->bucket == NULL || k->key == NULL || k->vclock == NULL) {
            fprintf(stderr, "riakctl: repl push: bucket, key and vclock expected per line\n");
            json_object_put(o);
            rc = RC_USAGE;
            break;
        }
        held[n++] = o;
        if (n == PUSH_BATCH) {
            rc = push_batch(c, queue, keys, n, &res);
            total += n;
            while (n > 0) {
                json_object_put(held[--n]);
            }
        }
    }
    if (rc == RC_OK) {
        rc = push_batch(c, queue, keys, n, &res);
        total += n;
    }
    while (n > 0) {
        json_object_put(held[--n]);
    }
    free(line);
    if (rc != RC_OK) {
        return rc;
    }
    json_object *o = json_object_new_object();
    json_object_object_add(o, "pushed", json_object_new_int64((int64_t)total));
    if (total) {
        json_object_object_add(o, "queue_exists", json_object_new_boolean(res.queue_exists));
        json_object_object_add(o, "fold_length", json_object_new_int64(res.fold_length));
        json_object_object_add(o, "fsync_length", json_object_new_int64(res.fsync_length));
        json_object_object_add(o, "realtime_length", json_object_new_int64(res.realtime_length));
    }
    return rc_print(x, o);
}

int rc_cmd_repl(rc_ctx *x, int argc, char **argv) {
    static const struct option opts[] = {{"max", required_argument, NULL, OPT_MAX},
                                         {"type", required_argument, NULL, 't'},
                                         {NULL, 0, NULL, 0}};
    unsigned long long max = 0;
    int ch;
    while ((ch = getopt_long(argc, argv, "t:", opts, NULL)) != -1) {
        if (ch == 't') {
            x->type = optarg;
        } else if (ch != OPT_MAX || !rc_parse_uint(optarg, UINT32_MAX, &max)) {
            return rc_usage_error("repl", "invalid option");
        }
    }
    int rest = argc - optind;
    if (rest < 1) {
        return rc_usage_error("repl", "ACTION expected");
    }
    const char *action = argv[optind];
    char **args = argv + optind + 1;
    rest--;
    static const struct {
        const char *name;
        int nargs;
    } actions[] = {{"fetch", 1}, {"push", 1}, {"members", 0}, {"cluster-id", 0}, {"get", 3}};
    bool known = false;
    for (size_t i = 0; i < sizeof actions / sizeof *actions; i++) {
        if (strcmp(action, actions[i].name) == 0) {
            if (rest != actions[i].nargs) {
                return rc_usage_error("repl", "wrong number of arguments for '%s'", action);
            }
            known = true;
        }
    }
    if (!known) {
        return rc_usage_error("repl", "unknown action '%s'", action);
    }
    riak_client *c = rc_client(x);
    if (c == NULL) {
        return RC_ERROR;
    }
    riak_status st;
    if (strcmp(action, "fetch") == 0) {
        return repl_fetch(x, c, args[0], max);
    }
    if (strcmp(action, "push") == 0) {
        return repl_push(x, c, args[0]);
    }
    if (strcmp(action, "members") == 0) {
        riak_members m = {0};
        if ((st = riak_cluster_membership(c, &m)) != RIAK_OK) {
            return rc_fail(c, "repl members", st);
        }
        json_object *a = json_object_new_array();
        for (size_t i = 0; i < m.count; i++) {
            json_object *o = json_object_new_object();
            json_object_object_add(o, "ip", json_object_new_string(m.items[i].ip));
            json_object_object_add(o, "port", json_object_new_int((int)m.items[i].port));
            json_object_array_add(a, o);
        }
        riak_members_free(&m);
        return rc_print(x, a);
    }
    if (strcmp(action, "cluster-id") == 0) {
        char *id = NULL;
        if ((st = riak_get_cluster_id(c, &id)) != RIAK_OK) {
            return rc_fail(c, "repl cluster-id", st);
        }
        json_object *o = json_object_new_object();
        json_object_object_add(o, "cluster_id", json_object_new_string(id));
        riak_free(id);
        return rc_print(x, o);
    }
    riak_object obj = {0};
    if ((st = riak_repl_get(c, args[0], args[1], args[2], NULL, &obj)) != RIAK_OK) {
        return rc_fail(c, "repl get", st);
    }
    json_object *o = rc_object_json(&obj, true);
    if (o && obj.vclock) {
        json_object_object_add(o, "vclock", json_object_new_string(obj.vclock));
    }
    riak_object_free(&obj);
    return rc_print(x, o);
}
