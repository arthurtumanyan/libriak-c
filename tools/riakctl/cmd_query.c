/*
 File:   cmd_query.c
 SPDX-License-Identifier: GPL-3.0-or-later

 index (secondary indexes), query (Query API, Riak 3.4+), mapred,
 coverage and preflist.
 */

#include <stdlib.h>
#include <string.h>

#include "riakctl.h"

enum {
    OPT_MAX = 256,
    OPT_CONTINUATION,
    OPT_TERMS,
    OPT_REGEX,
    OPT_SORT,
    OPT_TIMEOUT,
    OPT_COVER,
    OPT_REQUEST,
    OPT_START,
    OPT_END,
    OPT_EVAL,
    OPT_FILTER,
    OPT_ACCUMULATE,
    OPT_TERM,
    OPT_SUBST,
    OPT_COLLECT,
    OPT_MIN
};

/* ---- index -------------------------------------------------------------- */

typedef struct {
    bool failed;
} index_ctx;

static json_object *match_json(const char *key, const char *term) {
    if (term == NULL) {
        return json_object_new_string(key);
    }
    json_object *o = json_object_new_object();
    json_object_object_add(o, "key", json_object_new_string(key));
    json_object_object_add(o, "term", json_object_new_string(term));
    return o;
}

static bool index_cb(const char *key, const char *term, void *ctx) {
    index_ctx *ic = ctx;
    ic->failed = rc_print_line(match_json(key, term)) != RC_OK;
    return !ic->failed;
}

int rc_cmd_index(rc_ctx *x, int argc, char **argv) {
    static const struct option opts[] = {
        {"type", required_argument, NULL, 't'},
        {"max", required_argument, NULL, OPT_MAX},
        {"continuation", required_argument, NULL, OPT_CONTINUATION},
        {"terms", no_argument, NULL, OPT_TERMS},
        {"regex", required_argument, NULL, OPT_REGEX},
        {"sort", no_argument, NULL, OPT_SORT},
        {"timeout", required_argument, NULL, OPT_TIMEOUT},
        {"cover", required_argument, NULL, OPT_COVER},
        {NULL, 0, NULL, 0}};
    riak_index_query q = {0};
    unsigned long long n;
    int ch;
    while ((ch = getopt_long(argc, argv, "t:", opts, NULL)) != -1) {
        switch (ch) {
        case 't':
            x->type = optarg;
            break;
        case OPT_MAX:
            if (!rc_parse_uint(optarg, 0xFFFFFFFFu, &n) || n == 0) {
                return rc_usage_error("index", "invalid page size '%s'", optarg);
            }
            q.max_results = (unsigned)n;
            break;
        case OPT_CONTINUATION:
            q.continuation = optarg;
            break;
        case OPT_TERMS:
            q.return_terms = true;
            break;
        case OPT_REGEX:
            q.term_regex = optarg;
            break;
        case OPT_SORT:
            q.pagination_sort = true;
            break;
        case OPT_TIMEOUT:
            if (!rc_parse_uint(optarg, 0xFFFFFFFFu, &n)) {
                return rc_usage_error("index", "invalid timeout '%s'", optarg);
            }
            q.server_timeout_ms = (unsigned)n;
            break;
        case OPT_COVER:
            q.cover_context = optarg;
            break;
        default:
            return rc_usage_error("index", "unknown option");
        }
    }
    int rest = argc - optind;
    if (rest != 3 && rest != 4) {
        return rc_usage_error("index", "BUCKET INDEX and VALUE, or BUCKET INDEX MIN MAX expected");
    }
    const char *bucket = argv[optind];
    q.index = argv[optind + 1];
    if (rest == 3) {
        q.value = argv[optind + 2];
    } else {
        q.range_min = argv[optind + 2];
        q.range_max = argv[optind + 3];
    }
    if ((q.return_terms || q.term_regex) && q.value) {
        return rc_usage_error("index", "--terms and --regex need a range query");
    }
    q.bucket_type = x->type;
    riak_client *c = rc_client(x);
    if (c == NULL) {
        return RC_ERROR;
    }
    riak_status st;
    if (q.max_results == 0) {
        index_ctx ic = {0};
        st = riak_index_query_stream(c, bucket, &q, index_cb, &ic, NULL);
        if (ic.failed) {
            return RC_ERROR;
        }
        return st == RIAK_OK ? RC_OK : rc_fail(c, "index", st);
    }
    /* one page as a document, with the continuation of the next one */
    riak_index_result res = {0};
    if ((st = riak_index_query_run(c, bucket, &q, &res)) != RIAK_OK) {
        return rc_fail(c, "index", st);
    }
    json_object *o = json_object_new_object();
    json_object *results = json_object_new_array();
    for (size_t i = 0; i < res.count; i++) {
        json_object_array_add(results, match_json(res.keys[i], res.terms ? res.terms[i] : NULL));
    }
    json_object_object_add(o, "results", results);
    json_object_object_add(o, "continuation",
                           res.continuation ? json_object_new_string(res.continuation) : NULL);
    riak_index_result_free(&res);
    return rc_print(x, o);
}

/* ---- query (Query API) -------------------------------------------------- */

static const char *jstr(json_object *o, const char *name) {
    json_object *v;
    return json_object_object_get_ex(o, name, &v) && !json_object_is_type(v, json_type_null)
               ? json_object_get_string(v)
               : NULL;
}

/* Builds q from a request document; the strings stay owned by req. */
static bool query_from_json(json_object *req, riak_query *q, riak_query_part **parts,
                            riak_kv_pair **subs) {
    json_object *jp, *js, *v;
    if (!json_object_object_get_ex(req, "parts", &jp) ||
        !json_object_is_type(jp, json_type_array) || json_object_array_length(jp) == 0) {
        fprintf(stderr, "riakctl: query: the request has no \"parts\"\n");
        return false;
    }
    size_t n = json_object_array_length(jp);
    *parts = calloc(n, sizeof **parts);
    if (*parts == NULL) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        json_object *p = json_object_array_get_idx(jp, i);
        riak_query_part *qp = &(*parts)[i];
        qp->aggregation_tag =
            json_object_object_get_ex(p, "tag", &v) ? json_object_get_int(v) : (int)i + 1;
        qp->index_name = jstr(p, "index");
        qp->start_term = jstr(p, "start");
        qp->end_term = jstr(p, "end");
        qp->evaluation_expression = jstr(p, "eval");
        qp->filter_expression = jstr(p, "filter");
        qp->regular_expression = jstr(p, "regex");
        if (qp->index_name == NULL || qp->start_term == NULL || qp->end_term == NULL) {
            fprintf(stderr, "riakctl: query: part %zu needs \"index\", \"start\" and \"end\"\n", i);
            return false;
        }
    }
    q->parts = *parts;
    q->n_parts = n;
    q->aggregation_expression = jstr(req, "aggregation");
    q->accumulation_option = jstr(req, "accumulate");
    q->accumulation_term = jstr(req, "term");
    q->continuation = jstr(req, "continuation");
    if (json_object_object_get_ex(req, "max_results", &v)) {
        q->max_results = (unsigned)json_object_get_int64(v);
    }
    if (json_object_object_get_ex(req, "timeout", &v)) {
        q->timeout_s = (unsigned)json_object_get_int64(v);
    }
    if (json_object_object_get_ex(req, "substitutions", &js) &&
        json_object_is_type(js, json_type_object)) {
        *subs = calloc((size_t)json_object_object_length(js) + 1, sizeof **subs);
        if (*subs == NULL) {
            return false;
        }
        json_object_object_foreach(js, name, val) {
            (*subs)[q->n_substitutions].name = name;
            (*subs)[q->n_substitutions++].value = json_object_get_string(val);
        }
        q->substitutions = *subs;
    }
    return true;
}

static int print_queue(rc_ctx *x, riak_client *c, const char *bucket, const char *queue,
                       unsigned batch) {
    for (;;) {
        riak_query_result r = {0};
        riak_status st = riak_query_fetch(c, x->type, bucket, queue, batch, &r);
        if (st != RIAK_OK) {
            return rc_fail(c, "query fetch", st);
        }
        for (size_t i = 0; i < r.count; i++) {
            if (rc_print_line(match_json(r.keys[i], r.terms ? r.terms[i] : NULL)) != RC_OK) {
                riak_query_result_free(&r);
                return RC_ERROR;
            }
        }
        bool done = r.complete;
        riak_query_result_free(&r);
        if (done) {
            return RC_OK;
        }
    }
}

int rc_cmd_query(rc_ctx *x, int argc, char **argv) {
    static const struct option opts[] = {
        {"type", required_argument, NULL, 't'},
        {"request", required_argument, NULL, OPT_REQUEST},
        {"start", required_argument, NULL, OPT_START},
        {"end", required_argument, NULL, OPT_END},
        {"eval", required_argument, NULL, OPT_EVAL},
        {"filter", required_argument, NULL, OPT_FILTER},
        {"regex", required_argument, NULL, OPT_REGEX},
        {"accumulate", required_argument, NULL, OPT_ACCUMULATE},
        {"term", required_argument, NULL, OPT_TERM},
        {"sub", required_argument, NULL, OPT_SUBST},
        {"max", required_argument, NULL, OPT_MAX},
        {"continuation", required_argument, NULL, OPT_CONTINUATION},
        {"timeout", required_argument, NULL, OPT_TIMEOUT},
        {NULL, 0, NULL, 0}};
    const char *request = NULL;
    /* the flags build the same document a --request file holds */
    json_object *part = json_object_new_object();
    json_object *req = json_object_new_object();
    json_object *subs = json_object_new_object();
    riak_query_part *parts = NULL;
    riak_kv_pair *pairs = NULL;
    char *text = NULL;
    int rc = RC_USAGE;
    unsigned long long n;
    int ch;
    while ((ch = getopt_long(argc, argv, "t:", opts, NULL)) != -1) {
        static const struct {
            int opt;
            bool in_part;
            const char *name;
        } map[] = {{OPT_START, true, "start"}, {OPT_END, true, "end"},
                   {OPT_EVAL, true, "eval"},   {OPT_FILTER, true, "filter"},
                   {OPT_REGEX, true, "regex"}, {OPT_ACCUMULATE, false, "accumulate"},
                   {OPT_TERM, false, "term"},  {OPT_CONTINUATION, false, "continuation"}};
        bool done = false;
        for (size_t i = 0; i < sizeof map / sizeof *map && !done; i++) {
            if (map[i].opt == ch) {
                json_object_object_add(map[i].in_part ? part : req, map[i].name,
                                       json_object_new_string(optarg));
                done = true;
            }
        }
        if (done) {
            continue;
        }
        switch (ch) {
        case 't':
            x->type = optarg;
            break;
        case OPT_REQUEST:
            request = optarg;
            break;
        case OPT_SUBST: {
            char *nm, *val;
            if (!rc_split_pair(optarg, &nm, &val)) {
                rc = rc_usage_error("query", "NAME=VALUE expected, got '%s'", optarg);
                goto out;
            }
            json_object_object_add(subs, nm, json_object_new_string(val));
            free(nm);
            free(val);
            break;
        }
        case OPT_MAX:
        case OPT_TIMEOUT:
            if (!rc_parse_uint(optarg, 0xFFFFFFFFu, &n)) {
                rc = rc_usage_error("query", "invalid number '%s'", optarg);
                goto out;
            }
            json_object_object_add(req, ch == OPT_MAX ? "max_results" : "timeout",
                                   json_object_new_int64((int64_t)n));
            break;
        default:
            rc = rc_usage_error("query", "unknown option");
            goto out;
        }
    }
    int rest = argc - optind;
    if (request ? rest != 1 : rest != 2) {
        rc = rc_usage_error("query", request ? "BUCKET expected with --request"
                                             : "BUCKET and INDEX expected (or --request FILE)");
        goto out;
    }
    const char *bucket = argv[optind];
    if (request) {
        size_t len;
        if ((text = rc_read_file(request, &len)) == NULL) {
            rc = RC_ERROR;
            goto out;
        }
        json_object *r = rc_parse_json(text, request);
        if (r == NULL) {
            rc = RC_ERROR;
            goto out;
        }
        /* command-line flags override the file */
        json_object_object_foreach(req, name, val) {
            json_object_object_add(r, name, json_object_get(val));
        }
        json_object_put(req);
        req = r;
    } else {
        json_object_object_add(part, "index", json_object_new_string(argv[optind + 1]));
        json_object *pa = json_object_new_array();
        json_object_array_add(pa, json_object_get(part));
        json_object_object_add(req, "parts", pa);
    }
    if (json_object_object_length(subs) > 0) {
        json_object_object_add(req, "substitutions", json_object_get(subs));
    }
    riak_query q = {0};
    if (!query_from_json(req, &q, &parts, &pairs)) {
        goto out;
    }
    q.bucket_type = x->type;
    riak_client *c = rc_client(x);
    if (c == NULL) {
        rc = RC_ERROR;
        goto out;
    }
    riak_query_result res = {0};
    riak_status st = riak_query_run(c, bucket, &q, &res);
    if (st != RIAK_OK) {
        rc = rc_fail(c, "query", st);
        goto out;
    }
    if (res.result_queue) {
        rc = print_queue(x, c, bucket, res.result_queue, q.max_results);
    } else {
        json_object *o = rc_parse_json(res.raw ? res.raw : "null", "query response");
        rc = o ? rc_print(x, o) : RC_ERROR;
    }
    riak_query_result_free(&res);
out:
    free(text);
    free(parts);
    free(pairs);
    json_object_put(part);
    json_object_put(req);
    json_object_put(subs);
    return rc;
}

/* ---- mapred ------------------------------------------------------------- */

static bool mapred_cb(unsigned phase, const char *data_json, void *ctx) {
    bool *failed = ctx;
    json_object *data = json_tokener_parse(data_json);
    json_object *o = json_object_new_object();
    json_object_object_add(o, "phase", json_object_new_int((int)phase));
    json_object_object_add(o, "data", data ? data : json_object_new_string(data_json));
    *failed = rc_print_line(o) != RC_OK;
    return !*failed;
}

int rc_cmd_mapred(rc_ctx *x, int argc, char **argv) {
    static const struct option opts[] = {{"collect", no_argument, NULL, OPT_COLLECT},
                                         {NULL, 0, NULL, 0}};
    bool collect = false;
    int ch;
    while ((ch = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        if (ch != OPT_COLLECT) {
            return rc_usage_error("mapred", "unknown option");
        }
        collect = true;
    }
    if (argc - optind > 1) {
        return rc_usage_error("mapred", "at most one FILE expected");
    }
    size_t len;
    char *job = rc_read_file(argc - optind == 1 ? argv[optind] : "-", &len);
    if (job == NULL) {
        return RC_ERROR;
    }
    riak_client *c = rc_client(x);
    int rc = RC_ERROR;
    riak_status st = RIAK_OK;
    if (c && collect) {
        char *result = NULL;
        st = riak_mapred(c, job, &result);
        if (st == RIAK_OK) {
            json_object *o = rc_parse_json(result, "MapReduce result");
            rc = o ? rc_print(x, o) : RC_ERROR;
        }
        riak_free(result);
    } else if (c) {
        bool failed = false;
        st = riak_mapred_stream(c, job, mapred_cb, &failed);
        rc = failed ? RC_ERROR : RC_OK;
    }
    free(job);
    if (c && st != RIAK_OK) {
        rc = rc_fail(c, "mapred", st);
    }
    return rc;
}

/* ---- coverage and preflist ---------------------------------------------- */

int rc_cmd_coverage(rc_ctx *x, int argc, char **argv) {
    static const struct option opts[] = {{"type", required_argument, NULL, 't'},
                                         {"min", required_argument, NULL, OPT_MIN},
                                         {NULL, 0, NULL, 0}};
    unsigned long long min = 0;
    int ch;
    while ((ch = getopt_long(argc, argv, "t:", opts, NULL)) != -1) {
        if (ch == 't') {
            x->type = optarg;
        } else if (ch != OPT_MIN || !rc_parse_uint(optarg, 0xFFFFFFFFu, &min)) {
            return rc_usage_error("coverage", "invalid option");
        }
    }
    if (argc - optind != 1) {
        return rc_usage_error("coverage", "BUCKET expected");
    }
    riak_client *c = rc_client(x);
    if (c == NULL) {
        return RC_ERROR;
    }
    riak_coverage_plan plan = {0};
    riak_status st = riak_get_coverage(c, x->type, argv[optind], (unsigned)min, &plan);
    if (st != RIAK_OK) {
        return rc_fail(c, "coverage", st);
    }
    json_object *a = json_object_new_array();
    for (size_t i = 0; i < plan.count; i++) {
        const riak_coverage_entry *e = &plan.entries[i];
        json_object *o = json_object_new_object();
        json_object_object_add(o, "host", json_object_new_string(e->host));
        json_object_object_add(o, "port", json_object_new_int((int)e->port));
        json_object_object_add(o, "description",
                               json_object_new_string(e->description ? e->description : ""));
        json_object_object_add(o, "cover_context", json_object_new_string(e->cover_context));
        json_object_array_add(a, o);
    }
    riak_coverage_free(&plan);
    return rc_print(x, a);
}

int rc_cmd_preflist(rc_ctx *x, int argc, char **argv) {
    static const struct option opts[] = {{"type", required_argument, NULL, 't'},
                                         {NULL, 0, NULL, 0}};
    int ch;
    while ((ch = getopt_long(argc, argv, "t:", opts, NULL)) != -1) {
        if (ch != 't') {
            return rc_usage_error("preflist", "unknown option");
        }
        x->type = optarg;
    }
    if (argc - optind != 2) {
        return rc_usage_error("preflist", "BUCKET and KEY expected");
    }
    riak_client *c = rc_client(x);
    if (c == NULL) {
        return RC_ERROR;
    }
    riak_preflist pl = {0};
    riak_status st = riak_get_preflist(c, x->type, argv[optind], argv[optind + 1], &pl);
    if (st != RIAK_OK) {
        return rc_fail(c, "preflist", st);
    }
    json_object *a = json_object_new_array();
    for (size_t i = 0; i < pl.count; i++) {
        json_object *o = json_object_new_object();
        json_object_object_add(o, "partition", json_object_new_int64(pl.items[i].partition));
        json_object_object_add(o, "node", json_object_new_string(pl.items[i].node));
        json_object_object_add(o, "primary", json_object_new_boolean(pl.items[i].primary));
        json_object_array_add(a, o);
    }
    riak_preflist_free(&pl);
    return rc_print(x, a);
}
