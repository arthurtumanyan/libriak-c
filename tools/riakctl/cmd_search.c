/*
 File:   cmd_search.c
 SPDX-License-Identifier: GPL-3.0-or-later

 search: Riak Search (Solr) queries and index and schema administration
 (Riak 2.x; Riak Search was removed in Riak 3.0).
   search query INDEX QUERY [options]
   search index-list | index-get NAME | index-create NAME | index-delete NAME
   search schema-get NAME | schema-put NAME [FILE]
 */

#include <stdlib.h>
#include <string.h>

#include "riakctl.h"

enum {
    OPT_FILTER = 256,
    OPT_SORT,
    OPT_FIELDS,
    OPT_DF,
    OPT_OP,
    OPT_START,
    OPT_ROWS,
    OPT_SCHEMA,
    OPT_NVAL,
    OPT_RAW
};

static json_object *doc_json(const riak_search_doc *d) {
    json_object *o = json_object_new_object();
    if (d->bucket_type) {
        json_object_object_add(o, "type", json_object_new_string(d->bucket_type));
    }
    if (d->bucket) {
        json_object_object_add(o, "bucket", json_object_new_string(d->bucket));
    }
    if (d->key) {
        json_object_object_add(o, "key", json_object_new_string(d->key));
    }
    json_object_object_add(o, "score", json_object_new_double(d->score));
    /* multi-valued fields repeat in the result; they become arrays */
    json_object *fields = json_object_new_object();
    for (size_t i = 0; i < d->n_fields; i++) {
        const riak_header *f = &d->fields[i];
        json_object *prev;
        json_object *v = json_object_new_string(f->value);
        if (!json_object_object_get_ex(fields, f->name, &prev)) {
            json_object_object_add(fields, f->name, v);
        } else if (json_object_is_type(prev, json_type_array)) {
            json_object_array_add(prev, v);
        } else {
            json_object *a = json_object_new_array();
            json_object_array_add(a, json_object_get(prev));
            json_object_array_add(a, v);
            json_object_object_add(fields, f->name, a);
        }
    }
    json_object_object_add(o, "fields", fields);
    return o;
}

static int print_json_text(rc_ctx *x, riak_client *c, riak_status st, char *text,
                           const char *what) {
    if (st != RIAK_OK) {
        return rc_fail(c, what, st);
    }
    json_object *o = rc_parse_json(text, what);
    riak_free(text);
    return o ? rc_print(x, o) : RC_ERROR;
}

int rc_cmd_search(rc_ctx *x, int argc, char **argv) {
    static const struct option opts[] = {{"filter", required_argument, NULL, OPT_FILTER},
                                         {"sort", required_argument, NULL, OPT_SORT},
                                         {"fields", required_argument, NULL, OPT_FIELDS},
                                         {"df", required_argument, NULL, OPT_DF},
                                         {"op", required_argument, NULL, OPT_OP},
                                         {"start", required_argument, NULL, OPT_START},
                                         {"rows", required_argument, NULL, OPT_ROWS},
                                         {"schema", required_argument, NULL, OPT_SCHEMA},
                                         {"n-val", required_argument, NULL, OPT_NVAL},
                                         {"raw", no_argument, NULL, OPT_RAW},
                                         {NULL, 0, NULL, 0}};
    riak_search_query q = {0};
    const char *schema = NULL;
    unsigned long long n, nval = 0;
    bool raw = false;
    int ch;
    while ((ch = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        switch (ch) {
        case OPT_FILTER:
            q.filter = optarg;
            break;
        case OPT_SORT:
            q.sort = optarg;
            break;
        case OPT_FIELDS:
            q.fields = optarg;
            break;
        case OPT_DF:
            q.default_field = optarg;
            break;
        case OPT_OP:
            q.op = optarg;
            break;
        case OPT_START:
        case OPT_ROWS:
        case OPT_NVAL:
            if (!rc_parse_uint(optarg, UINT32_MAX, &n)) {
                return rc_usage_error("search", "invalid number '%s'", optarg);
            }
            if (ch == OPT_START) {
                q.start = (unsigned)n;
            } else if (ch == OPT_ROWS) {
                q.rows = (unsigned)n;
            } else {
                nval = n;
            }
            break;
        case OPT_SCHEMA:
            schema = optarg;
            break;
        case OPT_RAW:
            raw = true;
            break;
        default:
            return rc_usage_error("search", "unknown option");
        }
    }
    int rest = argc - optind;
    if (rest < 1) {
        return rc_usage_error("search", "ACTION expected");
    }
    const char *action = argv[optind];
    char **args = argv + optind + 1;
    rest--;
    static const struct {
        const char *name;
        int min, max;
    } actions[] = {{"query", 2, 2},        {"index-list", 0, 0},   {"index-get", 1, 1},
                   {"index-create", 1, 1}, {"index-delete", 1, 1}, {"schema-get", 1, 1},
                   {"schema-put", 1, 2}};
    bool known = false;
    for (size_t i = 0; i < sizeof actions / sizeof *actions; i++) {
        if (strcmp(action, actions[i].name) == 0) {
            if (rest < actions[i].min || rest > actions[i].max) {
                return rc_usage_error("search", "wrong number of arguments for '%s'", action);
            }
            known = true;
        }
    }
    if (!known) {
        return rc_usage_error("search", "unknown action '%s'", action);
    }
    riak_client *c = rc_client(x);
    if (c == NULL) {
        return RC_ERROR;
    }
    riak_status st;
    char *text = NULL;
    if (strcmp(action, "query") == 0) {
        q.q = args[1];
        riak_search_result r = {0};
        if ((st = riak_search(c, args[0], &q, &r)) != RIAK_OK) {
            return rc_fail(c, "search", st);
        }
        json_object *o;
        if (raw) {
            o = rc_parse_json(r.raw ? r.raw : "null", "search response");
        } else {
            o = json_object_new_object();
            json_object_object_add(o, "num_found", json_object_new_int64((int64_t)r.num_found));
            json_object_object_add(o, "max_score", json_object_new_double(r.max_score));
            json_object *docs = json_object_new_array();
            for (size_t i = 0; i < r.count; i++) {
                json_object_array_add(docs, doc_json(&r.docs[i]));
            }
            json_object_object_add(o, "docs", docs);
        }
        riak_search_result_free(&r);
        return o ? rc_print(x, o) : RC_ERROR;
    }
    if (strcmp(action, "index-list") == 0) {
        st = riak_search_index_list(c, &text);
        return print_json_text(x, c, st, text, "search index-list");
    }
    if (strcmp(action, "index-get") == 0) {
        st = riak_search_index_get(c, args[0], &text);
        return print_json_text(x, c, st, text, "search index-get");
    }
    if (strcmp(action, "index-create") == 0) {
        st = riak_search_index_create(c, args[0], schema, (unsigned)nval);
        return st == RIAK_OK ? RC_OK : rc_fail(c, "search index-create", st);
    }
    if (strcmp(action, "index-delete") == 0) {
        st = riak_search_index_delete(c, args[0]);
        return st == RIAK_OK ? RC_OK : rc_fail(c, "search index-delete", st);
    }
    if (strcmp(action, "schema-get") == 0) {
        if ((st = riak_search_schema_get(c, args[0], &text)) != RIAK_OK) {
            return rc_fail(c, "search schema-get", st);
        }
        int rc = fputs(text, stdout) >= 0 ? RC_OK : RC_ERROR;
        riak_free(text);
        return rc;
    }
    size_t len;
    char *xml = rc_read_file(rest == 2 ? args[1] : "-", &len);
    if (xml == NULL) {
        return RC_ERROR;
    }
    st = riak_search_schema_put(c, args[0], xml);
    free(xml);
    return st == RIAK_OK ? RC_OK : rc_fail(c, "search schema-put", st);
}
