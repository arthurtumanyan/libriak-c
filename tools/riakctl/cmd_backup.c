/*
 File:   cmd_backup.c
 SPDX-License-Identifier: GPL-3.0-or-later

 backup and restore. A backup is a JSON Lines file:
   {"format":"riakctl-backup","version":1,"bucket":...,"type":...,...}   header
   {"key":...,"vclock":...,"siblings":[{...},...]}                       object
   {"key":...,"datatype":"map","value":{...}}                            data type
   {"end":true,"count":N}                                                trailer
 Siblings use the layout of rc_object_json(); tombstones are not saved.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "riakctl.h"

#define BACKUP_FORMAT "riakctl-backup"
#define BACKUP_VERSION 1
#define FOLD_PAGE 1000

enum { OPT_METHOD = 256 };

typedef enum { M_AUTO, M_FOLD, M_AAE, M_KEYS } bk_method;

typedef struct {
    rc_ctx *x;
    const char *bucket;
    const char *datatype; /* NULL for plain objects */
    riak_client *fetch;   /* client used for per-key fetches */
    unsigned long long count;
    riak_status err;
    char *err_key;
    bool write_failed;
    /* fold state: siblings of one key arrive in consecutive callbacks */
    char *cur_key;
    json_object *cur;
} bk_ctx;

static bool emit(bk_ctx *b, json_object *entry) {
    if (rc_print_line(entry) != RC_OK) {
        b->write_failed = true;
        return false;
    }
    b->count++;
    return true;
}

static json_object *new_entry(const char *key, const char *vclock) {
    json_object *e = json_object_new_object();
    json_object_object_add(e, "key", json_object_new_string(key));
    if (vclock) {
        json_object_object_add(e, "vclock", json_object_new_string(vclock));
    }
    json_object_object_add(e, "siblings", json_object_new_array());
    return e;
}

static void add_sibling(json_object *entry, const riak_object *obj) {
    json_object *sibs;
    if (!obj->deleted && json_object_object_get_ex(entry, "siblings", &sibs)) {
        json_object_array_add(sibs, rc_object_json(obj, true));
    }
}

/* Writes an entry unless every sibling was a tombstone. */
static bool emit_entry(bk_ctx *b, json_object *entry) {
    json_object *sibs;
    if (json_object_object_get_ex(entry, "siblings", &sibs) &&
        json_object_array_length(sibs) == 0) {
        json_object_put(entry);
        return true;
    }
    return emit(b, entry);
}

static bool flush_fold(bk_ctx *b) {
    bool ok = true;
    if (b->cur) {
        ok = emit_entry(b, b->cur);
        b->cur = NULL;
    }
    free(b->cur_key);
    b->cur_key = NULL;
    return ok;
}

static bool fold_cb(const char *key, riak_object *obj, void *ctx) {
    bk_ctx *b = ctx;
    if (b->cur_key && strcmp(b->cur_key, key) != 0 && !flush_fold(b)) {
        return false;
    }
    if (b->cur == NULL) {
        b->cur = new_entry(key, obj->vclock);
        b->cur_key = strdup(key);
        if (b->cur == NULL || b->cur_key == NULL) {
            b->err = RIAK_E_NOMEM;
            return false;
        }
    }
    add_sibling(b->cur, obj);
    return true;
}

static riak_status run_fold(bk_ctx *b, riak_client *c) {
    riak_fold_query q = {0};
    /* the smallest key a C string can hold */
    q.start_key = "\x01";
    q.max_results = FOLD_PAGE;
    q.bucket_type = b->x->type;
    char *cont = NULL;
    riak_status st;
    do {
        q.continuation = cont;
        char *next = NULL;
        st = riak_fold_objects(c, b->bucket, &q, fold_cb, b, &next);
        free(cont);
        cont = next;
        if (st == RIAK_OK && (b->err != RIAK_OK || b->write_failed)) {
            st = b->err != RIAK_OK ? b->err : RIAK_E_TRANSPORT;
        }
    } while (st == RIAK_OK && cont && *cont);
    free(cont);
    if (st == RIAK_OK && !flush_fold(b)) {
        st = RIAK_E_TRANSPORT;
    }
    json_object_put(b->cur);
    b->cur = NULL;
    free(b->cur_key);
    b->cur_key = NULL;
    return st;
}

/* Fetches one key with b->fetch and writes it; false stops the walk. */
static bool backup_key(bk_ctx *b, const char *key) {
    riak_status st;
    if (b->datatype) {
        riak_dt_opts o = {0};
        o.no_context = true;
        riak_datatype dt = {0};
        st = riak_dt_fetch(b->fetch, b->x->type, b->bucket, key, &o, &dt);
        if (st == RIAK_OK) {
            json_object *e = json_object_new_object();
            json_object_object_add(e, "key", json_object_new_string(key));
            json_object_object_add(e, "datatype",
                                   json_object_new_string(rc_dt_name(dt.value.type)));
            json_object_object_add(e, "value", rc_dt_json(&dt.value));
            riak_datatype_free(&dt);
            return emit(b, e);
        }
    } else {
        riak_opts o = {0};
        o.bucket_type = b->x->type;
        riak_object_set set = {0};
        st = riak_get_siblings(b->fetch, b->bucket, key, &o, &set);
        if (st == RIAK_OK) {
            json_object *e = new_entry(key, set.vclock);
            for (size_t i = 0; i < set.count; i++) {
                add_sibling(e, &set.items[i]);
            }
            riak_object_set_free(&set);
            return emit_entry(b, e);
        }
    }
    if (st == RIAK_E_NOT_FOUND) {
        return true; /* deleted since it was listed */
    }
    b->err = st;
    b->err_key = strdup(key);
    return false;
}

static riak_status run_aae(bk_ctx *b, riak_client *c) {
    riak_aae_filter f = {0};
    f.bucket_type = b->x->type;
    f.bucket = b->bucket;
    riak_aae_keys keys = {0};
    riak_status st = riak_aae_fetch_clocks_range(c, &f, &keys);
    if (st != RIAK_OK) {
        return st;
    }
    b->fetch = c;
    for (size_t i = 0; i < keys.count; i++) {
        if (!backup_key(b, keys.items[i].key)) {
            break;
        }
    }
    riak_aae_keys_free(&keys);
    return b->write_failed ? RIAK_E_TRANSPORT : b->err;
}

static bool keys_cb(const char *name, size_t len, void *ctx) {
    bk_ctx *b = ctx;
    char *key = strndup(name, len);
    if (key == NULL) {
        b->err = RIAK_E_NOMEM;
        return false;
    }
    bool more = backup_key(b, key);
    free(key);
    return more;
}

static riak_status run_keys(bk_ctx *b, riak_client *c) {
    /* the listing streams on c; the objects are fetched on a second client */
    b->fetch = rc_client_new(b->x);
    if (b->fetch == NULL) {
        return RIAK_E_INVALID_ARG;
    }
    riak_status st = riak_list_keys_stream(c, b->x->type, b->bucket, keys_cb, b);
    if (b->err != RIAK_OK || b->write_failed) {
        st = b->write_failed ? RIAK_E_TRANSPORT : b->err;
    }
    riak_client_free(b->fetch);
    b->fetch = NULL;
    return st;
}

/* Failures after which auto mode moves on to the next method. */
static bool method_unavailable(riak_status st) {
    return st == RIAK_E_UNSUPPORTED || st == RIAK_E_SERVER || st == RIAK_E_HTTP ||
           st == RIAK_E_NOT_FOUND;
}

static char *bucket_datatype(riak_client *c, const char *type, const char *bucket,
                             riak_status *st) {
    char *json = NULL;
    *st = riak_type_get_bucket_props(c, type, bucket, &json);
    if (*st != RIAK_OK) {
        return NULL;
    }
    json_object *o = json_tokener_parse(json);
    riak_free(json);
    json_object *props, *dt;
    char *name = NULL;
    if (o && json_object_object_get_ex(o, "props", &props) &&
        json_object_object_get_ex(props, "datatype", &dt) &&
        json_object_is_type(dt, json_type_string)) {
        name = strdup(json_object_get_string(dt));
    }
    json_object_put(o);
    return name;
}

int rc_cmd_backup(rc_ctx *x, int argc, char **argv) {
    static const struct option opts[] = {{"type", required_argument, NULL, 't'},
                                         {"output", required_argument, NULL, 'o'},
                                         {"method", required_argument, NULL, OPT_METHOD},
                                         {NULL, 0, NULL, 0}};
    const char *output = NULL;
    bk_method method = M_AUTO;
    int ch;
    while ((ch = getopt_long(argc, argv, "t:o:", opts, NULL)) != -1) {
        switch (ch) {
        case 't':
            x->type = optarg;
            break;
        case 'o':
            output = optarg;
            break;
        case OPT_METHOD:
            if (strcmp(optarg, "auto") == 0) {
                method = M_AUTO;
            } else if (strcmp(optarg, "fold") == 0) {
                method = M_FOLD;
            } else if (strcmp(optarg, "aae") == 0) {
                method = M_AAE;
            } else if (strcmp(optarg, "keys") == 0) {
                method = M_KEYS;
            } else {
                return rc_usage_error("backup", "unknown method '%s'", optarg);
            }
            break;
        default:
            return rc_usage_error("backup", "unknown option");
        }
    }
    if (argc - optind != 1) {
        return rc_usage_error("backup", "BUCKET expected");
    }
    riak_client *c = rc_client(x);
    if (c == NULL) {
        return RC_ERROR;
    }
    bk_ctx b = {.x = x, .bucket = argv[optind]};
    riak_status st;
    char *datatype = bucket_datatype(c, x->type, b.bucket, &st);
    if (st != RIAK_OK) {
        return rc_fail(c, "bucket properties", st);
    }
    b.datatype = datatype;
    if (datatype && method == M_FOLD) {
        free(datatype);
        return rc_usage_error("backup", "a data type bucket cannot be backed up with a fold");
    }
    riak_server_info si = {0};
    riak_get_server_info(c, &si);

    if (output && freopen(output, "w", stdout) == NULL) {
        fprintf(stderr, "riakctl: %s: %s\n", output, strerror(errno));
        free(datatype);
        riak_server_info_free(&si);
        return RC_ERROR;
    }
    char created[32];
    time_t now = time(NULL);
    struct tm tm;
    strftime(created, sizeof created, "%Y-%m-%dT%H:%M:%SZ", gmtime_r(&now, &tm));
    json_object *h = json_object_new_object();
    json_object_object_add(h, "format", json_object_new_string(BACKUP_FORMAT));
    json_object_object_add(h, "version", json_object_new_int(BACKUP_VERSION));
    json_object_object_add(h, "bucket", json_object_new_string(b.bucket));
    if (x->type) {
        json_object_object_add(h, "type", json_object_new_string(x->type));
    }
    if (datatype) {
        json_object_object_add(h, "datatype", json_object_new_string(datatype));
    }
    json_object_object_add(h, "created", json_object_new_string(created));
    if (si.version) {
        json_object_object_add(h, "riak_version", json_object_new_string(si.version));
    }
    riak_server_info_free(&si);
    int rc = rc_print_line(h);
    if (rc != RC_OK) {
        free(datatype);
        return rc;
    }

    static const char *const names[] = {"auto", "fold", "aae", "keys"};
    bk_method first = method == M_AUTO ? (datatype ? M_AAE : M_FOLD) : method;
    bk_method last = method == M_AUTO ? M_KEYS : method;
    for (bk_method m = first; m <= last; m++) {
        b.err = RIAK_OK;
        st = m == M_FOLD ? run_fold(&b, c) : m == M_AAE ? run_aae(&b, c) : run_keys(&b, c);
        if (st == RIAK_OK || b.count > 0 || b.err_key || b.write_failed ||
            !method_unavailable(st) || m == last) {
            break;
        }
        if (x->conn.verbose) {
            rc_warn("backup: %s method not available (%s), trying the next one", names[m],
                    riak_strerror(st));
        }
    }
    free(datatype);
    if (st != RIAK_OK || b.write_failed) {
        if (b.write_failed) {
            rc_warn("backup: write error");
            rc = RC_ERROR;
        } else {
            char what[300];
            snprintf(what, sizeof what, "backup%s%.256s", b.err_key ? " of key " : "",
                     b.err_key ? b.err_key : "");
            rc = rc_fail(b.fetch ? b.fetch : c, what, st);
        }
        free(b.err_key);
        return rc;
    }
    json_object *t = json_object_new_object();
    json_object_object_add(t, "end", json_object_new_boolean(1));
    json_object_object_add(t, "count", json_object_new_int64((int64_t)b.count));
    rc = rc_print_line(t);
    if (rc == RC_OK && output) {
        fprintf(stderr, "riakctl: %llu entries written to %s\n", b.count, output);
    }
    return rc;
}

/* ---- restore ------------------------------------------------------------ */

typedef struct {
    rc_ctx *x;
    riak_client *c;
    const char *type;
    const char *bucket;
    bool dry_run;
    unsigned long long restored, unchanged, skipped, failed;
} rs_ctx;

/* A sibling of a backup entry, decoded. */
typedef struct {
    char *data;
    size_t len;
    const char *content_type;
    riak_kv_pair *meta;
    size_t n_meta;
    riak_kv_pair *indexes;
    size_t n_indexes;
    const char *links;
} rs_sib;

static void sib_free(rs_sib *s) {
    free(s->data);
    free(s->meta);
    free(s->indexes);
}

static bool sib_decode(json_object *o, rs_sib *s) {
    memset(s, 0, sizeof *s);
    if (!json_object_is_type(o, json_type_object) || !rc_value_from_json(o, &s->data, &s->len)) {
        return false;
    }
    json_object *v;
    if (json_object_object_get_ex(o, "content_type", &v)) {
        s->content_type = json_object_get_string(v);
    }
    if (json_object_object_get_ex(o, "links", &v)) {
        s->links = json_object_get_string(v);
    }
    if (json_object_object_get_ex(o, "meta", &v) && json_object_is_type(v, json_type_object)) {
        s->meta = calloc((size_t)json_object_object_length(v) + 1, sizeof *s->meta);
        if (s->meta == NULL) {
            return false;
        }
        json_object_object_foreach(v, name, val) {
            s->meta[s->n_meta].name = name;
            s->meta[s->n_meta++].value = json_object_get_string(val);
        }
    }
    if (json_object_object_get_ex(o, "indexes", &v) && json_object_is_type(v, json_type_object)) {
        size_t n = 0;
        json_object_object_foreach(v, name, vals) {
            (void)name;
            n += json_object_is_type(vals, json_type_array) ? json_object_array_length(vals) : 1;
        }
        s->indexes = calloc(n + 1, sizeof *s->indexes);
        if (s->indexes == NULL) {
            return false;
        }
        json_object_object_foreach(v, iname, ivals) {
            size_t k =
                json_object_is_type(ivals, json_type_array) ? json_object_array_length(ivals) : 1;
            for (size_t i = 0; i < k; i++) {
                json_object *e = json_object_is_type(ivals, json_type_array)
                                     ? json_object_array_get_idx(ivals, i)
                                     : ivals;
                s->indexes[s->n_indexes].name = iname;
                s->indexes[s->n_indexes++].value = json_object_get_string(e);
            }
        }
    }
    return true;
}

static bool str_eq(const char *a, const char *b) {
    return strcmp(a ? a : "", b ? b : "") == 0;
}

/* Every pair of a is in b (names compared without case) and the counts match. */
static bool pairs_eq(const riak_kv_pair *a, size_t na, const riak_header *b, size_t nb) {
    if (na != nb) {
        return false;
    }
    for (size_t i = 0; i < na; i++) {
        bool found = false;
        for (size_t j = 0; j < nb && !found; j++) {
            found = strcasecmp(a[i].name, b[j].name) == 0 && strcmp(a[i].value, b[j].value) == 0;
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

static bool sib_eq(const rs_sib *s, const riak_object *o) {
    return !o->deleted && s->len == o->len &&
           (s->len == 0 || memcmp(s->data, o->data, s->len) == 0) &&
           str_eq(s->content_type, o->content_type) && str_eq(s->links, o->links) &&
           pairs_eq(s->meta, s->n_meta, o->meta, o->n_meta) &&
           pairs_eq(s->indexes, s->n_indexes, o->indexes, o->n_indexes);
}

static riak_status put_sib(rs_ctx *r, const char *key, const rs_sib *s, const char *vclock,
                           bool asis) {
    riak_opts o = {0};
    o.bucket_type = r->type;
    o.content_type = s->content_type;
    o.meta = s->meta;
    o.n_meta = s->n_meta;
    o.indexes = s->indexes;
    o.n_indexes = s->n_indexes;
    o.links = s->links ? &s->links : NULL;
    o.n_links = s->links ? 1 : 0;
    o.vclock = vclock;
    o.asis = asis;
    return riak_put(r->c, r->bucket, key, s->data, s->len, &o, NULL, NULL);
}

/* Restores one object; returns false after a failure. */
static bool restore_object(rs_ctx *r, const char *key, json_object *e) {
    json_object *sibs, *v;
    const char *vclock = NULL;
    if (!json_object_object_get_ex(e, "siblings", &sibs) ||
        !json_object_is_type(sibs, json_type_array)) {
        rc_warn("restore: key %s: no siblings in the backup", key);
        return false;
    }
    if (json_object_object_get_ex(e, "vclock", &v)) {
        vclock = json_object_get_string(v);
    }
    size_t n = json_object_array_length(sibs);
    if (n == 0) {
        r->skipped++;
        return true;
    }
    rs_sib *s = calloc(n, sizeof *s);
    bool *missing = calloc(n, sizeof *missing);
    bool ok = s && missing;
    for (size_t i = 0; ok && i < n; i++) {
        ok = sib_decode(json_object_array_get_idx(sibs, i), &s[i]);
        if (!ok) {
            rc_warn("restore: key %s: invalid sibling %zu", key, i);
        }
    }
    riak_object_set cur = {0};
    riak_opts go = {0};
    go.bucket_type = r->type;
    riak_status st = ok ? riak_get_siblings(r->c, r->bucket, key, &go, &cur) : RIAK_OK;
    if (ok && st != RIAK_OK && st != RIAK_E_NOT_FOUND) {
        rc_fail(r->c, key, st);
        ok = false;
    }
    size_t n_missing = 0;
    for (size_t i = 0; ok && i < n; i++) {
        missing[i] = true;
        for (size_t j = 0; j < cur.count && missing[i]; j++) {
            missing[i] = !sib_eq(&s[i], &cur.items[j]);
        }
        n_missing += missing[i];
    }
    if (ok && n_missing == 0) {
        r->unchanged++;
    } else if (ok && r->dry_run) {
        r->restored++;
    } else if (ok && n == 1) {
        /* stored as given, with the backup's vector clock (as replication
         * does); a node without "asis" support takes a normal update */
        st = put_sib(r, key, &s[0], vclock, vclock != NULL);
        if (st != RIAK_OK && st != RIAK_E_TRANSPORT && st != RIAK_E_UNAUTHORIZED) {
            st = put_sib(r, key, &s[0], cur.vclock, false);
        }
        ok = st == RIAK_OK;
        if (!ok) {
            rc_fail(r->c, key, st);
        }
        r->restored += ok;
    } else if (ok) {
        /* one sibling per put; each write without a vector clock becomes a
         * sibling of what is stored (in allow_mult buckets) */
        for (size_t i = 0; ok && i < n; i++) {
            if (missing[i]) {
                st = put_sib(r, key, &s[i], NULL, false);
                ok = st == RIAK_OK;
                if (!ok) {
                    rc_fail(r->c, key, st);
                }
            }
        }
        r->restored += ok;
    }
    riak_object_set_free(&cur);
    for (size_t i = 0; s && i < n; i++) {
        sib_free(&s[i]);
    }
    free(s);
    free(missing);
    return ok;
}

/* ---- data types --------------------------------------------------------- */

typedef struct lazy_map {
    struct lazy_map *up;
    const char *field;
    riak_dt_op *op;
} lazy_map;

/* Nested map operations are created only when a field below them changes. */
static riak_dt_op *lazy_op(lazy_map *l) {
    if (l->op == NULL && l->up) {
        riak_dt_op *parent = lazy_op(l->up);
        l->op = parent ? riak_dt_op_map_map(parent, l->field) : NULL;
    }
    return l->op;
}

static bool has_element(const riak_dt_value *cur, const char *el) {
    for (size_t i = 0; cur && i < cur->n_elements; i++) {
        if (strcmp(cur->elements[i], el) == 0) {
            return true;
        }
    }
    return false;
}

/* Adds the elements of a missing from cur; op is created through get(). */
static bool set_adds(json_object *a, const riak_dt_value *cur, riak_dt_op *(*get)(void *),
                     void *arg, size_t *changes) {
    if (!json_object_is_type(a, json_type_array)) {
        return false;
    }
    for (size_t i = 0; i < json_object_array_length(a); i++) {
        const char *el = json_object_get_string(json_object_array_get_idx(a, i));
        if (!has_element(cur, el)) {
            riak_dt_op_add(get(arg), el);
            (*changes)++;
        }
    }
    return true;
}

typedef struct {
    lazy_map *map;
    const char *field;
} map_set_arg;

static riak_dt_op *get_map_set(void *arg) {
    map_set_arg *a = arg;
    return riak_dt_op_map_set(lazy_op(a->map), a->field);
}

static riak_dt_op *get_op(void *arg) {
    return arg;
}

static bool map_restore(json_object *backup, const riak_dt_value *cur, lazy_map *l,
                        size_t *changes) {
    if (!json_object_is_type(backup, json_type_object)) {
        return false;
    }
    json_object_object_foreach(backup, name, val) {
        const char *us = strrchr(name, '_');
        riak_dt_type t;
        if (us == NULL || !rc_dt_from_name(us + 1, &t) || (size_t)(us - name) >= 1024) {
            rc_warn("restore: map field '%s' has no type suffix", name);
            return false;
        }
        char field[1024];
        memcpy(field, name, (size_t)(us - name));
        field[us - name] = '\0';
        const riak_dt_value *cf = cur ? riak_dt_map_get(cur, field, t) : NULL;
        switch (t) {
        case RIAK_DT_COUNTER: {
            int64_t delta = json_object_get_int64(val) - (cf ? cf->counter : 0);
            if (delta) {
                riak_dt_op_map_counter(lazy_op(l), field, delta);
                (*changes)++;
            }
            break;
        }
        case RIAK_DT_REGISTER:
            if (cf == NULL || !str_eq(cf->reg, json_object_get_string(val))) {
                riak_dt_op_map_register(lazy_op(l), field, json_object_get_string(val));
                (*changes)++;
            }
            break;
        case RIAK_DT_FLAG:
            if ((cf ? cf->flag : false) != (bool)json_object_get_boolean(val)) {
                riak_dt_op_map_flag(lazy_op(l), field, json_object_get_boolean(val));
                (*changes)++;
            }
            break;
        case RIAK_DT_SET: {
            map_set_arg a = {l, field};
            if (!set_adds(val, cf, get_map_set, &a, changes)) {
                return false;
            }
            break;
        }
        case RIAK_DT_MAP: {
            lazy_map child = {l, field, NULL};
            if (!map_restore(val, cf, &child, changes)) {
                return false;
            }
            break;
        }
        default:
            rc_warn("restore: map field '%s' has an unsupported type", name);
            return false;
        }
    }
    return true;
}

static bool restore_datatype(rs_ctx *r, const char *key, json_object *e) {
    json_object *jt, *val;
    riak_dt_type t;
    if (!json_object_object_get_ex(e, "datatype", &jt) ||
        !rc_dt_from_name(json_object_get_string(jt), &t) ||
        !json_object_object_get_ex(e, "value", &val)) {
        rc_warn("restore: key %s: invalid data type entry", key);
        return false;
    }
    if (t == RIAK_DT_HLL) {
        rc_warn("restore: key %s: a HyperLogLog cannot be restored (only its estimate is saved)",
                key);
        r->skipped++;
        return true;
    }
    if (r->type == NULL) {
        rc_warn("restore: key %s: data types need a bucket type (--type)", key);
        return false;
    }
    riak_datatype cur = {0};
    riak_status st = riak_dt_fetch(r->c, r->type, r->bucket, key, NULL, &cur);
    if (st != RIAK_OK && st != RIAK_E_NOT_FOUND) {
        rc_fail(r->c, key, st);
        return false;
    }
    const riak_dt_value *cv = st == RIAK_OK ? &cur.value : NULL;
    riak_dt_op *op = riak_dt_op_new(t);
    size_t changes = 0;
    bool ok = op != NULL;
    if (ok && cv && cv->type != t) {
        rc_warn("restore: key %s: stored as a %s, backup holds a %s", key, rc_dt_name(cv->type),
                rc_dt_name(t));
        ok = false;
    } else if (ok && t == RIAK_DT_COUNTER) {
        int64_t delta = json_object_get_int64(val) - (cv ? cv->counter : 0);
        if (delta) {
            riak_dt_op_increment(op, delta);
            changes++;
        }
    } else if (ok && (t == RIAK_DT_SET || t == RIAK_DT_GSET)) {
        ok = set_adds(val, cv, get_op, op, &changes);
    } else if (ok && t == RIAK_DT_MAP) {
        lazy_map root = {NULL, NULL, op};
        ok = map_restore(val, cv, &root, &changes);
    }
    if (ok && changes == 0) {
        r->unchanged++;
    } else if (ok && r->dry_run) {
        r->restored++;
    } else if (ok) {
        st = riak_dt_update(r->c, r->type, r->bucket, key, op, cv ? cur.context : NULL, NULL, NULL,
                            NULL);
        ok = st == RIAK_OK;
        if (!ok) {
            rc_fail(r->c, key, st);
        }
        r->restored += ok;
    }
    riak_dt_op_free(op);
    riak_datatype_free(&cur);
    return ok;
}

int rc_cmd_restore(rc_ctx *x, int argc, char **argv) {
    static const struct option opts[] = {{"type", required_argument, NULL, 't'},
                                         {"bucket", required_argument, NULL, 'b'},
                                         {"dry-run", no_argument, NULL, 'n'},
                                         {NULL, 0, NULL, 0}};
    rs_ctx r = {.x = x};
    const char *bucket = NULL;
    int ch;
    while ((ch = getopt_long(argc, argv, "t:b:n", opts, NULL)) != -1) {
        switch (ch) {
        case 't':
            x->type = optarg;
            break;
        case 'b':
            bucket = optarg;
            break;
        case 'n':
            r.dry_run = true;
            break;
        default:
            return rc_usage_error("restore", "unknown option");
        }
    }
    if (argc - optind > 1) {
        return rc_usage_error("restore", "at most one FILE expected");
    }
    const char *path = argc - optind == 1 ? argv[optind] : "-";
    FILE *f = strcmp(path, "-") == 0 ? stdin : fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "riakctl: %s: %s\n", path, strerror(errno));
        return RC_ERROR;
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    unsigned long long lineno = 0;
    json_object *header = NULL;
    bool complete = false;
    int rc = RC_OK;
    while ((n = getline(&line, &cap, f)) > 0) {
        lineno++;
        if (n == 1 && line[0] == '\n') {
            continue;
        }
        char what[64];
        snprintf(what, sizeof what, "%s line %llu", path, lineno);
        json_object *e = rc_parse_json(line, what);
        if (e == NULL) {
            rc = RC_ERROR;
            break;
        }
        json_object *v;
        if (header == NULL) {
            json_object *fmt, *ver;
            if (!json_object_object_get_ex(e, "format", &fmt) ||
                strcmp(json_object_get_string(fmt), BACKUP_FORMAT) != 0 ||
                !json_object_object_get_ex(e, "version", &ver) ||
                json_object_get_int(ver) != BACKUP_VERSION) {
                fprintf(stderr, "riakctl: %s is not a riakctl backup (version %d)\n", path,
                        BACKUP_VERSION);
                json_object_put(e);
                rc = RC_ERROR;
                break;
            }
            header = e;
            r.type = x->type;
            if (r.type == NULL && json_object_object_get_ex(header, "type", &v)) {
                r.type = json_object_get_string(v);
            }
            r.bucket = bucket;
            if (r.bucket == NULL && json_object_object_get_ex(header, "bucket", &v)) {
                r.bucket = json_object_get_string(v);
            }
            if (r.bucket == NULL || (r.c = rc_client(x)) == NULL) {
                rc = RC_ERROR;
                break;
            }
            continue;
        }
        if (json_object_object_get_ex(e, "end", &v)) {
            complete = true;
            json_object_put(e);
            continue;
        }
        if (!json_object_object_get_ex(e, "key", &v)) {
            rc_warn("%s: no key, entry skipped", what);
            r.skipped++;
            json_object_put(e);
            continue;
        }
        const char *key = json_object_get_string(v);
        bool ok = json_object_object_get_ex(e, "datatype", NULL) ? restore_datatype(&r, key, e)
                                                                 : restore_object(&r, key, e);
        r.failed += !ok;
        json_object_put(e);
        if (!ok && riak_client_check_nodes(r.c) == 0) {
            rc_warn("restore: no node answers, stopping");
            rc = RC_UNREACHABLE;
            break;
        }
    }
    if (ferror(f)) {
        fprintf(stderr, "riakctl: %s: read error\n", path);
        rc = RC_ERROR;
    }
    free(line);
    if (f != stdin) {
        fclose(f);
    }
    if (header == NULL) {
        if (rc == RC_OK) {
            fprintf(stderr, "riakctl: %s: empty backup\n", path);
        }
        return RC_ERROR;
    }
    json_object *o = json_object_new_object();
    json_object_object_add(o, "bucket", json_object_new_string(r.bucket ? r.bucket : ""));
    if (r.type) {
        json_object_object_add(o, "type", json_object_new_string(r.type));
    }
    json_object_object_add(o, r.dry_run ? "would_restore" : "restored",
                           json_object_new_int64((int64_t)r.restored));
    json_object_object_add(o, "unchanged", json_object_new_int64((int64_t)r.unchanged));
    json_object_object_add(o, "skipped", json_object_new_int64((int64_t)r.skipped));
    json_object_object_add(o, "failed", json_object_new_int64((int64_t)r.failed));
    json_object_object_add(o, "complete", json_object_new_boolean(complete));
    int prc = rc_print(x, o);
    json_object_put(header);
    if (rc == RC_OK && !complete) {
        rc_warn("restore: %s has no trailer; the backup is incomplete", path);
        rc = RC_ERROR;
    }
    if (rc == RC_OK && r.failed) {
        rc = RC_ERROR;
    }
    return rc != RC_OK ? rc : prc;
}
