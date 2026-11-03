/*
 File:   cmd_aae.c
 SPDX-License-Identifier: GPL-3.0-or-later

 aae: AAE folds (Riak 2.9+ with tictac AAE). Range folds share the filter
 options of riak_aae_filter; reap and erase only count unless --local or
 --job is given. aae compare compares the range trees of a bucket on two
 clusters (or two buckets) and lists the keys whose clocks differ.
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "riakctl.h"

/* Segments per level-two branch, and bytes per segment hash, of a tictac tree. */
#define SEGMENTS_PER_BRANCH 256
#define HASH_BYTES 4
/* Segments per clock fold of aae compare. */
#define COMPARE_BATCH 128

enum {
    OPT_START_KEY = 256,
    OPT_END_KEY,
    OPT_SEGMENTS,
    OPT_TREE_SIZE,
    OPT_MODIFIED_START,
    OPT_MODIFIED_END,
    OPT_HASH_IV,
    OPT_SIZE,
    OPT_SIBLINGS,
    OPT_OBJECT_SIZE,
    OPT_LOCAL,
    OPT_JOB,
    OPT_PEER,
    OPT_PEER_BUCKET,
    OPT_CHUNK
};

/* Keys per side and range of a bucket against bucket compare. */
#define COMPARE_CHUNK 20000

static const char *const size_names[] = {"xxsmall", "xsmall", "small", "medium", "large", "xlarge"};

static bool parse_size(const char *s, riak_aae_tree_size *out) {
    for (size_t i = 0; i < sizeof size_names / sizeof *size_names; i++) {
        if (strcmp(s, size_names[i]) == 0) {
            *out = (riak_aae_tree_size)i;
            return true;
        }
    }
    return false;
}

/* Comma-separated unsigned numbers. */
static bool parse_ids(const char *s, uint32_t **ids, size_t *n) {
    size_t cap = 1;
    for (const char *p = s; *p; p++) {
        cap += *p == ',';
    }
    *ids = calloc(cap, sizeof **ids);
    if (*ids == NULL) {
        return false;
    }
    *n = 0;
    char *copy = strdup(s);
    char *save = NULL;
    bool ok = copy != NULL;
    for (char *t = ok ? strtok_r(copy, ",", &save) : NULL; ok && t && *n < cap;
         t = strtok_r(NULL, ",", &save)) {
        unsigned long long v;
        ok = rc_parse_uint(t, 0xFFFFFFFFu, &v);
        (*ids)[(*n)++] = (uint32_t)v;
    }
    free(copy);
    if (!ok || *n == 0) {
        free(*ids);
        *ids = NULL;
        return false;
    }
    return true;
}

static bool add_ids(char **args, int n, uint32_t **ids, size_t *count) {
    *ids = calloc((size_t)n + 1, sizeof **ids);
    if (*ids == NULL) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        unsigned long long v;
        if (!rc_parse_uint(args[i], 0xFFFFFFFFu, &v)) {
            free(*ids);
            *ids = NULL;
            return false;
        }
        (*ids)[i] = (uint32_t)v;
    }
    *count = (size_t)n;
    return true;
}

static json_object *bytes_json(const uint8_t *data, size_t len) {
    char *b = rc_base64_encode(data, len);
    json_object *o = json_object_new_string(b ? b : "");
    free(b);
    return o;
}

static int print_tree(rc_ctx *x, const riak_aae_tree *t) {
    json_object *o = json_object_new_object();
    json_object_object_add(o, "size", json_object_new_string(size_names[t->size]));
    if (t->level1) {
        json_object_object_add(o, "level1_b64", bytes_json(t->level1, t->level1_len));
    }
    json_object *br = json_object_new_array();
    for (size_t i = 0; i < t->n_branches; i++) {
        json_object *b = json_object_new_object();
        json_object_object_add(b, "id", json_object_new_int64(t->branches[i].id));
        json_object_object_add(b, "data_b64", bytes_json(t->branches[i].data, t->branches[i].len));
        json_object_array_add(br, b);
    }
    json_object_object_add(o, "branches", br);
    return rc_print(x, o);
}

static json_object *key_clock_json(const riak_aae_key_clock *k) {
    json_object *o = json_object_new_object();
    if (k->bucket_type) {
        json_object_object_add(o, "type", json_object_new_string(k->bucket_type));
    }
    json_object_object_add(o, "bucket", json_object_new_string(k->bucket));
    json_object_object_add(o, "key", json_object_new_string(k->key));
    json_object_object_add(o, "vclock", json_object_new_string(k->vclock ? k->vclock : ""));
    return o;
}

static int print_keys(const riak_aae_keys *k) {
    for (size_t i = 0; i < k->count; i++) {
        if (rc_print_line(key_clock_json(&k->items[i])) != RC_OK) {
            return RC_ERROR;
        }
    }
    return RC_OK;
}

static json_object *histogram_json(const riak_aae_histogram *h, size_t n, const char *name) {
    json_object *a = json_object_new_array();
    for (size_t i = 0; i < n; i++) {
        json_object *o = json_object_new_object();
        json_object_object_add(o, name, json_object_new_int64(h[i].order));
        json_object_object_add(o, "count", json_object_new_int64((int64_t)h[i].count));
        json_object_array_add(a, o);
    }
    return a;
}

/* ---- compare ------------------------------------------------------------ */

typedef struct {
    const riak_aae_key_clock *k;
    bool matched;
} clock_ref;

static int cmp_ref(const void *a, const void *b) {
    return strcmp(((const clock_ref *)a)->k->key, ((const clock_ref *)b)->k->key);
}

static int print_diff(const char *key, const char *status, const char *local, const char *peer) {
    json_object *o = json_object_new_object();
    json_object_object_add(o, "key", json_object_new_string(key));
    json_object_object_add(o, "status", json_object_new_string(status));
    if (local) {
        json_object_object_add(o, "vclock", json_object_new_string(local));
    }
    if (peer) {
        json_object_object_add(o, "peer_vclock", json_object_new_string(peer));
    }
    return rc_print_line(o);
}

/* Whether key lies in the window (after, upto]; a NULL bound is open. */
static bool in_window(const char *key, const char *after, const char *upto) {
    return (after == NULL || strcmp(key, after) > 0) && (upto == NULL || strcmp(key, upto) <= 0);
}

/* The keys of ka and kb inside the window, compared by vector clock. */
static int compare_lists(const riak_aae_keys *ka, const riak_aae_keys *kb, const char *after,
                         const char *upto, unsigned long long *diffs) {
    clock_ref *rb = calloc(kb->count + 1, sizeof *rb);
    if (rb == NULL) {
        fprintf(stderr, "riakctl: aae compare: out of memory\n");
        return RC_ERROR;
    }
    size_t nb = 0;
    for (size_t i = 0; i < kb->count; i++) {
        if (in_window(kb->items[i].key, after, upto)) {
            rb[nb++].k = &kb->items[i];
        }
    }
    qsort(rb, nb, sizeof *rb, cmp_ref);
    int rc = RC_OK;
    for (size_t i = 0; rc == RC_OK && i < ka->count; i++) {
        if (!in_window(ka->items[i].key, after, upto)) {
            continue;
        }
        clock_ref key = {&ka->items[i], false};
        clock_ref *m = bsearch(&key, rb, nb, sizeof *rb, cmp_ref);
        const char *va = ka->items[i].vclock ? ka->items[i].vclock : "";
        if (m == NULL) {
            rc = print_diff(ka->items[i].key, "missing_on_peer", va, NULL);
            (*diffs)++;
        } else {
            m->matched = true;
            const char *vb = m->k->vclock ? m->k->vclock : "";
            if (strcmp(va, vb) != 0) {
                rc = print_diff(ka->items[i].key, "differs", va, vb);
                (*diffs)++;
            }
        }
    }
    for (size_t i = 0; rc == RC_OK && i < nb; i++) {
        if (!rb[i].matched) {
            rc = print_diff(rb[i].k->key, "missing_locally", NULL, rb[i].k->vclock);
            (*diffs)++;
        }
    }
    free(rb);
    return rc;
}

/* The clocks of fa and fb (keys in the window) compared. */
static int compare_folds(riak_client *a, riak_client *b, const riak_aae_filter *fa,
                         const riak_aae_filter *fb, const char *after, const char *upto,
                         unsigned long long *diffs) {
    riak_aae_keys ka = {0}, kb = {0};
    riak_status st = riak_aae_fetch_clocks_range(a, fa, &ka);
    if (st != RIAK_OK) {
        return rc_fail(a, "aae compare", st);
    }
    if ((st = riak_aae_fetch_clocks_range(b, fb, &kb)) != RIAK_OK) {
        riak_aae_keys_free(&ka);
        return rc_fail(b, "aae compare (peer)", st);
    }
    int rc = compare_lists(&ka, &kb, after, upto, diffs);
    riak_aae_keys_free(&ka);
    riak_aae_keys_free(&kb);
    return rc;
}

/* Keys of segs on both sides, compared by vector clock. */
static int compare_segments(riak_client *a, riak_client *b, riak_aae_filter fa, riak_aae_filter fb,
                            const uint32_t *segs, size_t n, unsigned long long *diffs) {
    fa.segments = fb.segments = segs;
    fa.n_segments = fb.n_segments = n;
    return compare_folds(a, b, &fa, &fb, NULL, NULL, diffs);
}

/* ---- bucket against bucket ---------------------------------------------- */

/* The keys of one side in ascending order, a page at a time, from the
 * $bucket index (or $key over the --start-key/--end-key range). */
typedef struct {
    riak_client *c;
    const char *bucket;
    riak_index_query q;
    riak_index_result page;
    size_t pos;    /* first key of the page not yet compared */
    bool has_more; /* another page follows */
} key_pager;

static riak_status pager_next(key_pager *p) {
    char *cont = NULL;
    riak_status st = RIAK_OK;
    do {
        free(cont);
        cont = p->page.continuation;
        p->page.continuation = NULL;
        riak_index_result_free(&p->page);
        p->pos = 0;
        p->q.continuation = cont;
        st = riak_index_query_run(p->c, p->bucket, &p->q, &p->page);
        p->has_more = st == RIAK_OK && p->page.continuation != NULL;
        /* a page may come back empty with a continuation */
    } while (st == RIAK_OK && p->page.count == 0 && p->has_more);
    free(cont);
    p->q.continuation = NULL;
    return st;
}

static riak_status pager_start(key_pager *p, riak_client *c, const riak_aae_filter *f,
                               unsigned chunk) {
    memset(p, 0, sizeof *p);
    p->c = c;
    p->bucket = f->bucket;
    p->q.bucket_type = f->bucket_type;
    p->q.max_results = chunk;
    if (f->start_key) {
        p->q.index = "$key";
        p->q.range_min = f->start_key;
        p->q.range_max = f->end_key;
    } else {
        p->q.index = "$bucket";
        p->q.value = f->bucket;
    }
    return pager_next(p);
}

static const char *pager_last(const key_pager *p) {
    return p->page.count ? p->page.keys[p->page.count - 1] : NULL;
}

/* The keys up to upto are compared: the page moves on. */
static riak_status pager_consume(key_pager *p, const char *upto) {
    while (p->pos < p->page.count && strcmp(p->page.keys[p->pos], upto) <= 0) {
        p->pos++;
    }
    return p->pos == p->page.count && p->has_more ? pager_next(p) : RIAK_OK;
}

/* The node answered the index query with an error such as a backend
 * without secondary indexes (bitcask) gives. */
static bool no_index(riak_status st) {
    return st == RIAK_E_UNSUPPORTED || st == RIAK_E_SERVER || st == RIAK_E_HTTP;
}

/* Upper end of the last range, above every key that does not start with
 * it: 1024 bytes 0xFF over Protocol Buffers; over HTTP, where keys travel
 * as JSON strings, 256 times U+10FFFF, the largest valid UTF-8. */
#define CEILING_REPEAT ((size_t)256)

static char *key_ceiling(bool pb) {
    static const char top[] = "\xf4\x8f\xbf\xbf";
    char *s = malloc(CEILING_REPEAT * 4 + 1);
    for (size_t i = 0; s && i < CEILING_REPEAT; i++) {
        if (pb) {
            memset(s + i * 4, 0xff, 4);
        } else {
            memcpy(s + i * 4, top, 4);
        }
    }
    if (s) {
        s[CEILING_REPEAT * 4] = '\0';
    }
    return s;
}

/* Two buckets compared in windows of keys: each window ends at a key at
 * which one side's page ends, so a window holds at most a page of each
 * side's stored keys (plus their tombstones), and the clocks of a window
 * come from an AAE fold over its key range. */
static int compare_buckets(riak_client *a, riak_client *b, const riak_aae_filter *fa,
                           const riak_aae_filter *fb, bool pb_a, bool pb_b, unsigned chunk,
                           unsigned long long *diffs, unsigned long long *windows) {
    key_pager pa, pb;
    riak_status st = pager_start(&pa, a, fa, chunk);
    if (st != RIAK_OK) {
        riak_index_result_free(&pa.page);
        return no_index(st) ? -1 : rc_fail(a, "aae compare", st);
    }
    if ((st = pager_start(&pb, b, fb, chunk)) != RIAK_OK) {
        riak_index_result_free(&pa.page);
        riak_index_result_free(&pb.page);
        return no_index(st) ? -1 : rc_fail(b, "aae compare (peer)", st);
    }
    char *ceil_a = key_ceiling(pb_a), *ceil_b = key_ceiling(pb_b);
    char *after = NULL; /* the previous window ended here */
    int rc = ceil_a && ceil_b ? RC_OK : RC_ERROR;
    while (rc == RC_OK) {
        /* the window ends where the first unfinished side's page ends */
        const char *upto = NULL;
        if (pa.has_more) {
            upto = pager_last(&pa);
        }
        if (pb.has_more && (upto == NULL || strcmp(pager_last(&pb), upto) < 0)) {
            upto = pager_last(&pb);
        }
        char *hi = upto ? malloc(strlen(upto) + 2) : NULL;
        if (upto && hi == NULL) {
            rc = RC_ERROR;
            break;
        }
        if (hi) {
            /* the fold range reaches just past upto, whether or not the
             * node treats the end as inclusive */
            size_t n = strlen(upto);
            memcpy(hi, upto, n);
            hi[n] = '\x01';
            hi[n + 1] = '\0';
        }
        riak_aae_filter wa = *fa, wb = *fb;
        wa.start_key = wb.start_key = after ? after : fa->start_key ? fa->start_key : "\x01";
        wa.end_key = hi ? hi : fa->end_key ? fa->end_key : ceil_a;
        wb.end_key = hi ? hi : fa->end_key ? fa->end_key : ceil_b;
        rc = compare_folds(a, b, &wa, &wb, after, hi ? upto : fa->end_key, diffs);
        (*windows)++;
        if (rc != RC_OK || hi == NULL) {
            free(hi);
            break;
        }
        hi[strlen(upto)] = '\0';
        free(after);
        after = hi;
        if ((st = pager_consume(&pa, after)) != RIAK_OK) {
            rc = rc_fail(a, "aae compare", st);
        } else if ((st = pager_consume(&pb, after)) != RIAK_OK) {
            rc = rc_fail(b, "aae compare (peer)", st);
        }
        if (rc != RC_OK) {
            break;
        }
    }
    free(after);
    free(ceil_a);
    free(ceil_b);
    riak_index_result_free(&pa.page);
    riak_index_result_free(&pb.page);
    return rc;
}

static const riak_aae_branch *find_branch(const riak_aae_tree *t, uint32_t id) {
    for (size_t i = 0; t->branches && i < t->n_branches; i++) {
        if (t->branches[i].id == id) {
            return &t->branches[i];
        }
    }
    return NULL;
}

/* Names the peer left out (NUL bytes) make the comparison incomplete; the
 * local client's are reported when riakctl exits. */
static int left_out(riak_client *pc, int rc) {
    uint64_t n = riak_client_skipped_names(pc);
    if (n > 0) {
        rc_warn("aae compare: %" PRIu64 " key(s) of the peer with a NUL byte were not compared", n);
        return rc == RC_OK ? RC_ERROR : rc;
    }
    return rc;
}

static int aae_compare(rc_ctx *x, riak_client *c, riak_aae_filter f, riak_aae_tree_size size,
                       const char *peer_url, const char *peer_bucket, unsigned chunk) {
    rc_ctx px = *x;
    memset(&px.conn, 0, sizeof px.conn);
    px.client = NULL;
    px.conn.cfg = x->conn.cfg;
    px.conn.verbose = x->conn.verbose;
    int rc = peer_url ? rc_conn_setup(&px.conn, peer_url) : RC_OK;
    riak_client *pc = NULL;
    if (rc == RC_OK) {
        pc = peer_url ? rc_client_new(&px) : c;
        rc = pc ? RC_OK : RC_USAGE;
    }
    riak_aae_filter pf = f;
    if (peer_bucket) {
        pf.bucket = peer_bucket;
    }
    /* A key's segment depends on its bucket as well: trees are comparable
     * only for the same bucket. Two buckets are compared key by key. */
    if (rc == RC_OK && strcmp(f.bucket, pf.bucket) != 0) {
        unsigned long long diffs = 0, windows = 0;
        rc = compare_buckets(c, pc, &f, &pf, x->conn.cfg.transport == RIAK_TRANSPORT_PB,
                             px.conn.cfg.transport == RIAK_TRANSPORT_PB, chunk, &diffs, &windows);
        if (rc == -1) {
            /* without secondary indexes the keys cannot be paged in order */
            rc_warn("aae compare: the $bucket index is not available (%s); the key lists are "
                    "compared whole, in memory",
                    riak_client_errmsg(c));
            rc = compare_segments(c, pc, f, pf, NULL, 0, &diffs);
            windows = 1;
        }
        if (rc == RC_OK) {
            fprintf(stderr, "riakctl: aae compare: %llu keys differ (key by key, %llu ranges)\n",
                    diffs, windows);
        }
        if (pc != c) {
            rc = left_out(pc, rc);
            riak_client_free(pc);
        }
        rc_conn_free(&px.conn);
        return rc;
    }
    riak_aae_tree ta = {0}, tb = {0};
    riak_status st;
    if (rc == RC_OK && (st = riak_aae_merge_range(c, &f, size, &ta)) != RIAK_OK) {
        rc = rc_fail(c, "aae tree", st);
    }
    if (rc == RC_OK && (st = riak_aae_merge_range(pc, &pf, size, &tb)) != RIAK_OK) {
        rc = rc_fail(pc, "aae tree (peer)", st);
    }
    /* differing segments, from the level-two blocks */
    uint32_t *segs = NULL;
    size_t n_segs = 0, cap = 0;
    for (size_t i = 0; rc == RC_OK && i < ta.n_branches + tb.n_branches; i++) {
        const riak_aae_branch *ba = i < ta.n_branches ? &ta.branches[i] : NULL;
        const riak_aae_branch *bb;
        uint32_t id;
        if (ba) {
            id = ba->id;
            bb = find_branch(&tb, id);
        } else {
            bb = &tb.branches[i - ta.n_branches];
            id = bb->id;
            if (find_branch(&ta, id)) {
                continue; /* compared from the local side */
            }
        }
        /* at least one side holds the branch */
        const riak_aae_branch *ref = ba ? ba : bb;
        if (ref == NULL) {
            continue;
        }
        size_t len = ref->len;
        if (ba && bb && (ba->len != bb->len)) {
            fprintf(stderr, "riakctl: aae compare: trees of different shapes\n");
            rc = RC_ERROR;
            break;
        }
        if (ba && bb && (len == 0 || memcmp(ba->data, bb->data, len) == 0)) {
            continue;
        }
        for (size_t s = 0; s < len / HASH_BYTES; s++) {
            const uint8_t *ha = ba && ba->data ? ba->data + s * HASH_BYTES : NULL;
            const uint8_t *hb = bb && bb->data ? bb->data + s * HASH_BYTES : NULL;
            static const uint8_t zero[HASH_BYTES];
            if (memcmp(ha ? ha : zero, hb ? hb : zero, HASH_BYTES) == 0) {
                continue;
            }
            if (n_segs == cap) {
                cap = cap ? cap * 2 : 256;
                uint32_t *ns = realloc(segs, cap * sizeof *ns);
                if (ns == NULL) {
                    rc = RC_ERROR;
                    break;
                }
                segs = ns;
            }
            segs[n_segs++] = id * (uint32_t)(len / HASH_BYTES) + (uint32_t)s;
        }
    }
    riak_aae_tree_free(&ta);
    riak_aae_tree_free(&tb);
    unsigned long long diffs = 0;
    f.segment_tree_size = pf.segment_tree_size = size;
    for (size_t i = 0; rc == RC_OK && i < n_segs; i += COMPARE_BATCH) {
        size_t n = n_segs - i < COMPARE_BATCH ? n_segs - i : COMPARE_BATCH;
        rc = compare_segments(c, pc, f, pf, segs + i, n, &diffs);
    }
    free(segs);
    if (rc == RC_OK) {
        fprintf(stderr, "riakctl: aae compare: %zu segments and %llu keys differ\n", n_segs, diffs);
    }
    if (pc != c) {
        rc = left_out(pc, rc);
        riak_client_free(pc);
    }
    rc_conn_free(&px.conn);
    return rc;
}

/* ---- aae ---------------------------------------------------------------- */

int rc_cmd_aae(rc_ctx *x, int argc, char **argv) {
    static const struct option opts[] = {
        {"type", required_argument, NULL, 't'},
        {"start-key", required_argument, NULL, OPT_START_KEY},
        {"end-key", required_argument, NULL, OPT_END_KEY},
        {"segments", required_argument, NULL, OPT_SEGMENTS},
        {"tree-size", required_argument, NULL, OPT_TREE_SIZE},
        {"modified-start", required_argument, NULL, OPT_MODIFIED_START},
        {"modified-end", required_argument, NULL, OPT_MODIFIED_END},
        {"hash-iv", required_argument, NULL, OPT_HASH_IV},
        {"size", required_argument, NULL, OPT_SIZE},
        {"siblings", required_argument, NULL, OPT_SIBLINGS},
        {"object-size", required_argument, NULL, OPT_OBJECT_SIZE},
        {"local", no_argument, NULL, OPT_LOCAL},
        {"job", required_argument, NULL, OPT_JOB},
        {"peer", required_argument, NULL, OPT_PEER},
        {"peer-bucket", required_argument, NULL, OPT_PEER_BUCKET},
        {"chunk", required_argument, NULL, OPT_CHUNK},
        {NULL, 0, NULL, 0}};
    riak_aae_filter f = {0};
    riak_aae_tree_size size = RIAK_AAE_SMALL;
    uint32_t *segments = NULL;
    uint32_t *ids = NULL;
    riak_aae_find find = RIAK_AAE_SIBLING_COUNT;
    unsigned long long limit = 0, v;
    bool have_find = false;
    riak_aae_change how = RIAK_AAE_COUNT;
    uint32_t job = 0;
    const char *peer = NULL, *peer_bucket = NULL;
    unsigned long long chunk = COMPARE_CHUNK;
    int rc = RC_USAGE;
    int ch;
    while ((ch = getopt_long(argc, argv, "t:", opts, NULL)) != -1) {
        switch (ch) {
        case 't':
            x->type = optarg;
            break;
        case OPT_START_KEY:
            f.start_key = optarg;
            break;
        case OPT_END_KEY:
            f.end_key = optarg;
            break;
        case OPT_SEGMENTS:
            free(segments);
            if (!parse_ids(optarg, &segments, &f.n_segments)) {
                rc = rc_usage_error("aae", "invalid segment list '%s'", optarg);
                goto out;
            }
            f.segments = segments;
            break;
        case OPT_TREE_SIZE:
        case OPT_SIZE:
            if (!parse_size(optarg, ch == OPT_SIZE ? &size : &f.segment_tree_size)) {
                rc = rc_usage_error("aae", "unknown tree size '%s' (xxsmall ... xlarge)", optarg);
                goto out;
            }
            break;
        case OPT_MODIFIED_START:
        case OPT_MODIFIED_END:
        case OPT_HASH_IV:
        case OPT_JOB:
            if (!rc_parse_uint(optarg, 0xFFFFFFFFu, &v)) {
                rc = rc_usage_error("aae", "invalid number '%s'", optarg);
                goto out;
            }
            if (ch == OPT_MODIFIED_START) {
                f.modified_start = (uint32_t)v;
            } else if (ch == OPT_MODIFIED_END) {
                f.modified_end = (uint32_t)v;
            } else if (ch == OPT_HASH_IV) {
                f.use_hash_iv = true;
                f.hash_iv = (uint32_t)v;
            } else {
                how = RIAK_AAE_JOB;
                job = (uint32_t)v;
            }
            break;
        case OPT_SIBLINGS:
        case OPT_OBJECT_SIZE:
            if (!rc_parse_uint(optarg, 0xFFFFFFFFu, &limit)) {
                rc = rc_usage_error("aae", "invalid limit '%s'", optarg);
                goto out;
            }
            find = ch == OPT_SIBLINGS ? RIAK_AAE_SIBLING_COUNT : RIAK_AAE_OBJECT_SIZE;
            have_find = true;
            break;
        case OPT_LOCAL:
            how = RIAK_AAE_LOCAL;
            break;
        case OPT_PEER:
            peer = optarg;
            break;
        case OPT_PEER_BUCKET:
            peer_bucket = optarg;
            break;
        case OPT_CHUNK:
            if (!rc_parse_uint(optarg, 1000000, &chunk) || chunk < 10) {
                rc = rc_usage_error("aae", "--chunk takes 10 to 1000000 keys");
                goto out;
            }
            break;
        default:
            rc = rc_usage_error("aae", "unknown option");
            goto out;
        }
    }
    if ((f.modified_start || f.modified_end) && f.modified_end < f.modified_start) {
        rc = rc_usage_error("aae", "--modified-end is before --modified-start");
        goto out;
    }
    int rest = argc - optind;
    if (rest < 2) {
        rc = rc_usage_error("aae", "ACTION and its arguments expected");
        goto out;
    }
    const char *action = argv[optind];
    char **args = argv + optind + 1;
    rest--;
    /* actions on the cached n_val trees take an n_val, the others a bucket */
    bool by_nval = strcmp(action, "root") == 0 || strcmp(action, "branches") == 0 ||
                   strcmp(action, "clocks") == 0 || strcmp(action, "buckets") == 0;
    unsigned long long nval = 0;
    if (by_nval && !rc_parse_uint(args[0], 100, &nval)) {
        rc = rc_usage_error("aae", "invalid n_val '%s'", args[0]);
        goto out;
    }
    f.bucket_type = x->type;
    f.bucket = args[0];
    riak_client *c = rc_client(x);
    if (c == NULL) {
        rc = RC_ERROR;
        goto out;
    }
    riak_status st = RIAK_OK;
    size_t n_ids = 0;
    uint64_t count = 0;
    if (strcmp(action, "root") == 0 || strcmp(action, "branches") == 0 ||
        strcmp(action, "tree") == 0) {
        riak_aae_tree t = {0};
        if (action[0] == 'r') {
            st = riak_aae_merge_root(c, (unsigned)nval, &t);
        } else if (action[0] == 't') {
            st = riak_aae_merge_range(c, &f, size, &t);
        } else if (rest < 2 || !add_ids(args + 1, rest - 1, &ids, &n_ids)) {
            rc = rc_usage_error("aae", "branches NVAL ID... expected");
            goto out;
        } else {
            st = riak_aae_merge_branches(c, (unsigned)nval, ids, n_ids, &t);
        }
        rc = st == RIAK_OK ? print_tree(x, &t) : rc_fail(c, "aae", st);
        riak_aae_tree_free(&t);
    } else if (strcmp(action, "clocks") == 0 || strcmp(action, "keys") == 0 ||
               strcmp(action, "tombs") == 0) {
        riak_aae_keys k = {0};
        if (action[0] == 'k') {
            st = riak_aae_fetch_clocks_range(c, &f, &k);
        } else if (action[0] == 't') {
            st = riak_aae_find_tombs(c, &f, &k);
        } else if (rest < 2 || !add_ids(args + 1, rest - 1, &ids, &n_ids)) {
            rc = rc_usage_error("aae", "clocks NVAL SEGMENT... expected");
            goto out;
        } else {
            st = riak_aae_fetch_clocks_nval(c, (unsigned)nval, ids, n_ids, &k);
        }
        rc = st == RIAK_OK ? print_keys(&k) : rc_fail(c, "aae", st);
        riak_aae_keys_free(&k);
    } else if (strcmp(action, "find") == 0) {
        if (!have_find) {
            rc = rc_usage_error("aae", "find needs --siblings N or --object-size BYTES");
            goto out;
        }
        riak_aae_key_counts k = {0};
        st = riak_aae_find_keys(c, &f, find, (uint32_t)limit, &k);
        if (st != RIAK_OK) {
            rc = rc_fail(c, "aae find", st);
            goto out;
        }
        rc = RC_OK;
        for (size_t i = 0; rc == RC_OK && i < k.count; i++) {
            json_object *o = json_object_new_object();
            json_object_object_add(o, "key", json_object_new_string(k.items[i].key));
            json_object_object_add(o, find == RIAK_AAE_SIBLING_COUNT ? "siblings" : "size",
                                   json_object_new_int64((int64_t)k.items[i].value));
            rc = rc_print_line(o);
        }
        riak_aae_key_counts_free(&k);
    } else if (strcmp(action, "stats") == 0) {
        riak_aae_stats s = {0};
        if ((st = riak_aae_object_stats(c, &f, &s)) != RIAK_OK) {
            rc = rc_fail(c, "aae stats", st);
            goto out;
        }
        json_object *o = json_object_new_object();
        json_object_object_add(o, "total_count", json_object_new_int64((int64_t)s.total_count));
        json_object_object_add(o, "total_size", json_object_new_int64((int64_t)s.total_size));
        json_object_object_add(o, "sizes", histogram_json(s.sizes, s.n_sizes, "order"));
        json_object_object_add(o, "siblings", histogram_json(s.siblings, s.n_siblings, "siblings"));
        riak_aae_stats_free(&s);
        rc = rc_print(x, o);
    } else if (strcmp(action, "reap") == 0 || strcmp(action, "erase") == 0 ||
               strcmp(action, "repair") == 0 || strcmp(action, "repl") == 0) {
        const char *mode = "count";
        if (strcmp(action, "reap") == 0) {
            st = riak_aae_reap_tombs(c, &f, how, job, &count);
        } else if (strcmp(action, "erase") == 0) {
            st = riak_aae_erase_keys(c, &f, how, job, &count);
        } else if (strcmp(action, "repair") == 0) {
            st = riak_aae_repair_keys(c, &f, &count);
            mode = "repair";
        } else if (rest < 2) {
            rc = rc_usage_error("aae", "repl BUCKET QUEUE expected");
            goto out;
        } else {
            st = riak_aae_repl_keys(c, &f, args[1], &count);
            mode = "queued";
        }
        if (st != RIAK_OK) {
            rc = rc_fail(c, "aae", st);
            goto out;
        }
        if (strcmp(mode, "count") == 0) {
            mode = how == RIAK_AAE_COUNT ? "count" : how == RIAK_AAE_LOCAL ? "local" : "job";
        }
        json_object *o = json_object_new_object();
        json_object_object_add(o, "mode", json_object_new_string(mode));
        json_object_object_add(o, "count", json_object_new_int64((int64_t)count));
        rc = rc_print(x, o);
    } else if (strcmp(action, "buckets") == 0) {
        riak_aae_buckets b = {0};
        if ((st = riak_aae_list_buckets(c, (unsigned)nval, &b)) != RIAK_OK) {
            rc = rc_fail(c, "aae buckets", st);
            goto out;
        }
        rc = RC_OK;
        for (size_t i = 0; rc == RC_OK && i < b.count; i++) {
            json_object *o = json_object_new_object();
            if (b.items[i].bucket_type) {
                json_object_object_add(o, "type", json_object_new_string(b.items[i].bucket_type));
            }
            json_object_object_add(o, "bucket", json_object_new_string(b.items[i].bucket));
            rc = rc_print_line(o);
        }
        riak_aae_buckets_free(&b);
    } else if (strcmp(action, "compare") == 0) {
        if (peer == NULL && (peer_bucket == NULL || strcmp(peer_bucket, f.bucket) == 0)) {
            rc = rc_usage_error("aae", "compare needs --peer URL and/or another --peer-bucket");
            goto out;
        }
        rc = aae_compare(x, c, f, size, peer, peer_bucket, (unsigned)chunk);
    } else {
        rc = rc_usage_error("aae", "unknown action '%s'", action);
    }
out:
    free(segments);
    free(ids);
    return rc;
}
