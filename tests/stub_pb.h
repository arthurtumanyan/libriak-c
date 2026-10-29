/*
 File:   stub_pb.h
 SPDX-License-Identifier: GPL-3.0-or-later

 An in-process Riak Protocol Buffers stub for the tests: one thread accepts
 connections, one thread serves each. Replies are canned per message code
 and steered by the key or bucket name (see stub_handle()); what the client
 sent is recorded in `stub` for the tests to check. With stub_start_tls()
 the server also answers STARTTLS and authentication.
 */

#ifndef RIAK_TEST_STUB_PB_H
#define RIAK_TEST_STUB_PB_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <zlib.h>

#include "riak.pb-c.h"
#include "riak_dt.pb-c.h"
#include "riak_kv.pb-c.h"
#include "riak_repl.pb-c.h"
#include "riak_search.pb-c.h"
#include "riak_yokozuna.pb-c.h"

/* What the client sent, and knobs for the next replies. */
static struct {
    atomic_int accepts;     /* connections accepted */
    atomic_int requests;    /* request messages served */
    atomic_int overload;    /* reply "overload" to this many gets first */
    atomic_int drop;        /* close the connection on this many gets first */
    atomic_int auth_ok;     /* successful authentications */
    atomic_int tls_drop;    /* answer STARTTLS, then close like an old TLS server */
    atomic_int tls_version; /* protocol of the latest TLS session, e.g. TLS1_3_VERSION */
    char client_id[64];
    /* last put */
    char put_value[256];
    char put_ctype[64];
    char put_charset[32];
    char put_key[64];
    size_t put_meta, put_indexes, put_links;
    char put_link_bucket[64], put_link_key[64], put_link_tag[64];
    uint8_t put_vclock[16];
    size_t put_vclock_len;
    int put_if_none_match, put_if_not_modified, put_return_body;
    unsigned put_w;
    /* last get */
    unsigned get_r;
    int get_head, get_sloppy;
    unsigned get_n_val;
    /* last set bucket */
    unsigned set_n_val;
    int set_allow_mult;
    size_t set_precommit;
    /* last index query */
    uint8_t cover[16];
    size_t cover_len;
    /* last data type update */
    int dt_kind; /* 1 counter, 2 set, 3 map, 4 hll, 5 gset */
    int64_t dt_increment;
    size_t dt_adds, dt_removes, dt_map_updates, dt_map_removes;
    uint8_t dt_context[16];
    size_t dt_context_len;
} stub;

/* What the client sent with the newer requests. */
static struct {
    atomic_int old_node; /* answer newer codes like Riak 2.2: "Unknown message code" */
    atomic_int dropped;  /* connections closed after a handler crash */
    unsigned node_confirms, put_sync, put_asis, put_return_head, get_deletedvclock;
    int index_pagination_sort;
    unsigned dt_node_confirms, dt_return_body, dt_fetches;
    char type_props_type[32];
    int type_notfound_ok;
    int64_t counter_amount;
    int counter_returnvalue;
    char fold_start[32], fold_end[32];
    int fold_start_incl, fold_end_incl;
    char repl_cluster[32];
    size_t push_keys, push_vclock_len;
    unsigned aae_n_val, aae_ids, aae_tree_size, aae_change, aae_job, aae_finder, aae_limit;
    int aae_key_range, aae_seg_filter, aae_mod_range, aae_prehash;
    unsigned aae_iv, aae_mod_start, aae_mod_end;
    char aae_queue[32];
    /* range requests: what selects the objects */
    char aae_type[32], aae_bucket[32], aae_start[32], aae_end[32];
    unsigned aae_segs[4], aae_filter_size; /* filter_tree_size + 1, 0 when absent */
    /* object folds */
    unsigned fold_max;
    char fold_cont[32], fold_type[32], fold_bucket[32];
    /* counter reads */
    unsigned counter_r;
    int counter_basic_quorum, counter_notfound_ok;
    /* queue fetches and pushes */
    char fetch_queue[32], fetch_encoding[32];
    char push_type[2][16], push_bucket[2][16], push_key[2][16];
    /* bucket props sent with RpbSetBucketReq */
    unsigned props_sync, props_node_confirms; /* + 1, 0 when absent */
    int props_aae_exclude;
    /* the next request with this code is read and its connection closed
     * without an answer; received counts every request per code */
    atomic_int drop_code;
    unsigned received[256];
    atomic_int bad_tree_size; /* trees report a size Riak does not have */
} ext;

typedef struct {
    int fd;
    SSL *ssl;
} stub_conn;

static int stub_crash(stub_conn *c, const char *what);

static SSL_CTX *stub_tls_ctx;

static const uint8_t STUB_VCLOCK[] = {0x6b, 0xce, 0x00, 0x61, 0x60, 0x01};
static const uint8_t STUB_COVER[] = {0x00, 0x01, 0x63, 0x63};
static const uint8_t STUB_DT_CTX[] = {0x00, 0x63, 0x74, 0x78};

static int stub_io(stub_conn *c, void *buf, size_t n, int write) {
    size_t done = 0;
    while (done < n) {
        ssize_t r;
        if (c->ssl) {
            r = write ? SSL_write(c->ssl, (char *)buf + done, (int)(n - done))
                      : SSL_read(c->ssl, (char *)buf + done, (int)(n - done));
        } else {
            r = write ? send(c->fd, (char *)buf + done, n - done, MSG_NOSIGNAL)
                      : recv(c->fd, (char *)buf + done, n - done, 0);
        }
        if (r <= 0) return -1;
        done += (size_t)r;
    }
    return 0;
}

static int stub_send(stub_conn *c, uint8_t code, const ProtobufCMessage *m) {
    size_t len = m ? protobuf_c_message_get_packed_size(m) : 0;
    uint8_t *buf = malloc(len + 5);
    uint32_t n = (uint32_t)len + 1;
    buf[0] = (uint8_t)(n >> 24);
    buf[1] = (uint8_t)(n >> 16);
    buf[2] = (uint8_t)(n >> 8);
    buf[3] = (uint8_t)n;
    buf[4] = code;
    if (m) protobuf_c_message_pack(m, buf + 5);
    int rc = stub_io(c, buf, len + 5, 1);
    free(buf);
    return rc;
}

static int stub_error(stub_conn *c, const char *msg) {
    RpbErrorResp e = RPB_ERROR_RESP__INIT;
    e.errmsg.data = (uint8_t *)(uintptr_t)msg;
    e.errmsg.len = strlen(msg);
    return stub_send(c, 0, &e.base);
}

static ProtobufCBinaryData stub_bin(const char *s) {
    ProtobufCBinaryData b = {strlen(s), (uint8_t *)(uintptr_t)s};
    return b;
}

static void stub_copy(char *dst, size_t cap, ProtobufCBinaryData b) {
    size_t n = b.len < cap - 1 ? b.len : cap - 1;
    memcpy(dst, b.data, n);
    dst[n] = '\0';
}

static int stub_is(ProtobufCBinaryData b, const char *s) {
    return b.len == strlen(s) && memcmp(b.data, s, b.len) == 0;
}

static void stub_sleep_ms(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* A content with metadata, an index entry and a link. */
static void stub_content(RpbContent *ct, const char *value, RpbPair *meta, RpbPair *idx,
                         RpbLink *link, RpbPair **metap, RpbPair **idxp, RpbLink **linkp) {
    rpb_content__init(ct);
    ct->value = stub_bin(value);
    ct->has_content_type = 1;
    ct->content_type = stub_bin("text/plain");
    ct->has_charset = 1;
    ct->charset = stub_bin("utf-8");
    ct->has_vtag = 1;
    ct->vtag = stub_bin("vt1");
    ct->has_last_mod = 1;
    ct->last_mod = 1700000000;
    rpb_pair__init(meta);
    meta->key = stub_bin("color");
    meta->has_value = 1;
    meta->value = stub_bin("blue");
    rpb_pair__init(idx);
    idx->key = stub_bin("email_bin");
    idx->has_value = 1;
    idx->value = stub_bin("a@b.c");
    rpb_link__init(link);
    link->has_bucket = link->has_key = link->has_tag = 1;
    link->bucket = stub_bin("people");
    link->key = stub_bin("bob smith");
    link->tag = stub_bin("friend");
    *metap = meta;
    *idxp = idx;
    *linkp = link;
    ct->n_usermeta = 1;
    ct->usermeta = metap;
    ct->n_indexes = 1;
    ct->indexes = idxp;
    ct->n_links = 1;
    ct->links = linkp;
}

static int stub_get(stub_conn *c, const uint8_t *buf, size_t len) {
    RpbGetReq *q = rpb_get_req__unpack(NULL, len, buf);
    if (q == NULL) return -1;
    stub.get_r = q->has_r ? q->r : 0;
    stub.get_head = q->has_head && q->head;
    stub.get_sloppy = q->has_sloppy_quorum && q->sloppy_quorum;
    stub.get_n_val = q->has_n_val ? q->n_val : 0;
    ext.node_confirms = q->has_node_confirms ? q->node_confirms : 0;
    ext.get_deletedvclock = q->has_deletedvclock && q->deletedvclock;
    int rc = 0;
    RpbGetResp r = RPB_GET_RESP__INIT;
    RpbContent ct[2];
    RpbContent *cts[2] = {&ct[0], &ct[1]};
    RpbPair meta[2], idx[2];
    RpbLink link[2];
    RpbPair *metap[2], *idxp[2];
    RpbLink *linkp[2];
    r.has_vclock = 1;
    r.vclock.data = (uint8_t *)(uintptr_t)STUB_VCLOCK;
    r.vclock.len = sizeof STUB_VCLOCK;
    if (stub_is(q->key, "drop") && atomic_load(&stub.drop) > 0) {
        atomic_fetch_sub(&stub.drop, 1);
        rc = -1; /* the connection is closed without a reply */
    } else if (stub_is(q->key, "overload") && atomic_load(&stub.overload) > 0) {
        atomic_fetch_sub(&stub.overload, 1);
        rc = stub_error(c, "overload");
    } else if (stub_is(q->key, "slow")) {
        stub_sleep_ms(1500);
        rc = stub_send(c, 10, &r.base);
    } else if (stub_is(q->key, "bye")) {
        /* reply, then close the connection as an idle timeout would */
        RpbGetResp empty = RPB_GET_RESP__INIT;
        (void)stub_send(c, 10, &empty.base);
        rc = -1;
    } else if (stub_is(q->key, "crash")) {
        rc = stub_crash(c, "riak_kv_pb_object,process");
    } else if (stub_is(q->key, "tombdv")) {
        /* a tombstone: with deletedvclock only its clock comes back */
        RpbGetResp tomb = RPB_GET_RESP__INIT;
        if (q->has_deletedvclock && q->deletedvclock) {
            tomb.has_vclock = 1;
            tomb.vclock.data = (uint8_t *)(uintptr_t)STUB_VCLOCK;
            tomb.vclock.len = sizeof STUB_VCLOCK;
        }
        rc = stub_send(c, 10, &tomb.base);
    } else if (stub_is(q->key, "missing")) {
        RpbGetResp empty = RPB_GET_RESP__INIT;
        rc = stub_send(c, 10, &empty.base);
    } else if (stub_is(q->key, "unchanged")) {
        RpbGetResp un = RPB_GET_RESP__INIT;
        un.has_unchanged = 1;
        un.unchanged = q->has_if_modified;
        rc = stub_send(c, 10, &un.base);
    } else if (stub_is(q->key, "forbidden")) {
        rc = stub_error(c, "Permission denied");
    } else if (stub_is(q->key, "big")) {
        char *v = malloc(70000);
        memset(v, 'x', 69999);
        v[69999] = '\0';
        stub_content(&ct[0], v, &meta[0], &idx[0], &link[0], &metap[0], &idxp[0], &linkp[0]);
        r.n_content = 1;
        r.content = cts;
        rc = stub_send(c, 10, &r.base);
        free(v);
    } else {
        int sibs = stub_is(q->key, "sibs");
        stub_content(&ct[0],
                     sibs                       ? "one"
                     : (q->has_head && q->head) ? ""
                                                : "hello",
                     &meta[0], &idx[0], &link[0], &metap[0], &idxp[0], &linkp[0]);
        stub_content(&ct[1], "two", &meta[1], &idx[1], &link[1], &metap[1], &idxp[1], &linkp[1]);
        if (stub_is(q->key, "tomb")) {
            ct[0].has_deleted = 1;
            ct[0].deleted = 1;
        }
        r.n_content = sibs ? 2 : 1;
        r.content = cts;
        rc = stub_send(c, 10, &r.base);
    }
    rpb_get_req__free_unpacked(q, NULL);
    return rc;
}

static int stub_put(stub_conn *c, const uint8_t *buf, size_t len) {
    RpbPutReq *q = rpb_put_req__unpack(NULL, len, buf);
    if (q == NULL) return -1;
    stub_copy(stub.put_value, sizeof stub.put_value, q->content->value);
    stub_copy(stub.put_ctype, sizeof stub.put_ctype, q->content->content_type);
    stub.put_charset[0] = '\0';
    if (q->content->has_charset)
        stub_copy(stub.put_charset, sizeof stub.put_charset, q->content->charset);
    stub.put_key[0] = '\0';
    if (q->has_key) stub_copy(stub.put_key, sizeof stub.put_key, q->key);
    stub.put_meta = q->content->n_usermeta;
    stub.put_indexes = q->content->n_indexes;
    stub.put_links = q->content->n_links;
    if (q->content->n_links) {
        stub_copy(stub.put_link_bucket, 64, q->content->links[0]->bucket);
        stub_copy(stub.put_link_key, 64, q->content->links[0]->key);
        stub_copy(stub.put_link_tag, 64, q->content->links[0]->tag);
    }
    stub.put_vclock_len = q->has_vclock ? q->vclock.len : 0;
    if (q->has_vclock && q->vclock.len <= sizeof stub.put_vclock)
        memcpy(stub.put_vclock, q->vclock.data, q->vclock.len);
    stub.put_if_none_match = q->has_if_none_match && q->if_none_match;
    stub.put_if_not_modified = q->has_if_not_modified && q->if_not_modified;
    stub.put_return_body = q->has_return_body && q->return_body;
    stub.put_w = q->has_w ? q->w : 0;
    ext.node_confirms = q->has_node_confirms ? q->node_confirms : 0;
    ext.put_sync = q->has_sync_on_write ? (unsigned)q->sync_on_write + 1 : 0;
    ext.put_asis = q->has_asis && q->asis;
    ext.put_return_head = q->has_return_head && q->return_head;
    int rc;
    if (q->has_key && stub_is(q->key, "exists") && stub.put_if_none_match) {
        rc = stub_error(c, "match_found");
    } else if (stub.put_if_not_modified &&
               (q->vclock.len != sizeof STUB_VCLOCK ||
                memcmp(q->vclock.data, STUB_VCLOCK, sizeof STUB_VCLOCK) != 0)) {
        rc = stub_error(c, "modified");
    } else {
        RpbPutResp r = RPB_PUT_RESP__INIT;
        if (!q->has_key) {
            r.has_key = 1;
            r.key = stub_bin("genkey");
        }
        RpbContent *cts[1] = {q->content};
        ProtobufCBinaryData value = q->content->value;
        if (ext.put_return_head) {
            q->content->value.len = 0; /* metadata only */
        }
        if (stub.put_return_body || ext.put_return_head) {
            r.n_content = 1;
            r.content = cts;
            r.has_vclock = 1;
            r.vclock.data = (uint8_t *)(uintptr_t)STUB_VCLOCK;
            r.vclock.len = sizeof STUB_VCLOCK;
        }
        rc = stub_send(c, 12, &r.base);
        q->content->value = value;
    }
    rpb_put_req__free_unpacked(q, NULL);
    return rc;
}

static int stub_names(stub_conn *c, uint8_t code, int keys) {
    const char *batches[3][2] = {{"n1", "n2"}, {"n3", NULL}, {NULL, NULL}};
    for (int i = 0; i < 3; i++) {
        ProtobufCBinaryData names[2];
        size_t n = 0;
        for (int j = 0; j < 2 && batches[i][j]; j++)
            names[n++] = stub_bin(batches[i][j]);
        int rc;
        if (keys) {
            RpbListKeysResp r = RPB_LIST_KEYS_RESP__INIT;
            r.n_keys = n;
            r.keys = names;
            r.has_done = i == 2;
            r.done = i == 2;
            rc = stub_send(c, code, &r.base);
        } else {
            RpbListBucketsResp r = RPB_LIST_BUCKETS_RESP__INIT;
            r.n_buckets = n;
            r.buckets = names;
            r.has_done = i == 2;
            r.done = i == 2;
            rc = stub_send(c, code, &r.base);
        }
        if (rc != 0) return rc;
    }
    return 0;
}

static int stub_props(stub_conn *c) {
    RpbBucketProps p = RPB_BUCKET_PROPS__INIT;
    p.has_n_val = 1;
    p.n_val = 3;
    p.has_allow_mult = 1;
    p.allow_mult = 1;
    p.has_r = 1;
    p.r = 4294967293u; /* quorum */
    p.has_datatype = 1;
    p.datatype = stub_bin("map");
    p.has_node_confirms = 1;
    p.node_confirms = 2;
    p.has_sync_on_write = 1;
    p.sync_on_write = RPB_BUCKET_PROPS__SYNC_ON_WRITE__sync_one;
    p.has_aae_tree_exclude = 1;
    p.aae_tree_exclude = 1;
    RpbCommitHook hook = RPB_COMMIT_HOOK__INIT;
    hook.has_name = 1;
    hook.name = stub_bin("validate");
    RpbCommitHook *hooks[1] = {&hook};
    p.n_precommit = 1;
    p.precommit = hooks;
    RpbModFun mf = RPB_MOD_FUN__INIT;
    mf.module = stub_bin("riak_core_util");
    mf.function = stub_bin("chash_std_keyfun");
    p.chash_keyfun = &mf;
    RpbGetBucketResp r = RPB_GET_BUCKET_RESP__INIT;
    r.props = &p;
    return stub_send(c, 20, &r.base);
}

static int stub_index(stub_conn *c, const uint8_t *buf, size_t len) {
    RpbIndexReq *q = rpb_index_req__unpack(NULL, len, buf);
    if (q == NULL) return -1;
    stub.cover_len = q->has_cover_context ? q->cover_context.len : 0;
    if (q->has_cover_context && q->cover_context.len <= sizeof stub.cover)
        memcpy(stub.cover, q->cover_context.data, q->cover_context.len);
    int terms = q->has_return_terms && q->return_terms;
    ext.index_pagination_sort = q->has_pagination_sort && q->pagination_sort;
    int rc = 0;
    for (int i = 0; rc == 0 && i < 2; i++) {
        RpbIndexResp r = RPB_INDEX_RESP__INIT;
        ProtobufCBinaryData keys[1] = {stub_bin(i ? "k2" : "k1")};
        RpbPair pair = RPB_PAIR__INIT;
        RpbPair *pairs[1] = {&pair};
        if (terms) {
            pair.key = stub_bin(i ? "t2" : "t1");
            pair.has_value = 1;
            pair.value = keys[0];
            r.n_results = 1;
            r.results = pairs;
        } else {
            r.n_keys = 1;
            r.keys = keys;
        }
        if (i == 1) {
            r.has_continuation = 1;
            r.continuation = stub_bin("c2");
            r.has_done = r.done = 1;
        }
        rc = stub_send(c, 26, &r.base);
    }
    rpb_index_req__free_unpacked(q, NULL);
    return rc;
}

static int stub_search(stub_conn *c) {
    RpbPair f[4];
    RpbPair *fp[4];
    const char *kv[4][2] = {
        {"_yz_rk", "alice"}, {"_yz_rb", "people"}, {"tag_ss", "a"}, {"tag_ss", "b"}};
    for (int i = 0; i < 4; i++) {
        rpb_pair__init(&f[i]);
        f[i].key = stub_bin(kv[i][0]);
        f[i].has_value = 1;
        f[i].value = stub_bin(kv[i][1]);
        fp[i] = &f[i];
    }
    RpbPair score = RPB_PAIR__INIT;
    score.key = stub_bin("score");
    score.has_value = 1;
    score.value = stub_bin("1.5");
    RpbPair *all[5] = {fp[0], fp[1], fp[2], fp[3], &score};
    RpbSearchDoc doc = RPB_SEARCH_DOC__INIT;
    doc.n_fields = 5;
    doc.fields = all;
    RpbSearchDoc *docs[1] = {&doc};
    RpbSearchQueryResp r = RPB_SEARCH_QUERY_RESP__INIT;
    r.n_docs = 1;
    r.docs = docs;
    r.has_num_found = 1;
    r.num_found = 7;
    r.has_max_score = 1;
    r.max_score = 1.5f;
    return stub_send(c, 28, &r.base);
}

static int stub_mapred(stub_conn *c) {
    const char *parts[3][2] = {{"0", "[1,2]"}, {"0", "[3]"}, {"1", "[\"x\"]"}};
    for (int i = 0; i < 4; i++) {
        RpbMapRedResp r = RPB_MAP_RED_RESP__INIT;
        if (i < 3) {
            r.has_phase = 1;
            r.phase = (uint32_t)atoi(parts[i][0]);
            r.has_response = 1;
            r.response = stub_bin(parts[i][1]);
        } else {
            r.has_done = r.done = 1;
        }
        if (stub_send(c, 24, &r.base) != 0) return -1;
    }
    return 0;
}

/* MapReduce link phases: alice links to bob (friend) and carol (boss);
 * the reply lists [bucket, key, tag] per kept phase. */
static int stub_links(stub_conn *c, const char *job) {
    const char *b = strstr(job, "\"inputs\":[[\"");
    char bucket[64] = "lb";
    if (b) {
        b += 12;
        size_t n = strcspn(b, "\"");
        snprintf(bucket, sizeof bucket, "%.*s", (int)(n < 63 ? n : 63), b);
    }
    char p0[256], p1[256];
    int two = strstr(job, "\"boss\"") != NULL;
    if (strcmp(bucket, "oddlinks") == 0) {
        /* entries that are not [bucket, key, tag] lists */
        snprintf(p0, sizeof p0, "[\"x\",[\"oddlinks\"]]");
    } else if (strcmp(bucket, "numlinks") == 0) {
        snprintf(p0, sizeof p0, "[[1,\"bob\",\"friend\"]]");
    } else if (strcmp(bucket, "numkeys") == 0) {
        snprintf(p0, sizeof p0, "[[\"numkeys\",2,\"friend\"]]");
    } else if (two) {
        snprintf(p0, sizeof p0, "[[\"%s\",\"bob\",\"friend\"],[\"%s\",\"carol\",\"boss\"]]", bucket,
                 bucket);
        snprintf(p1, sizeof p1, "[[\"%s\",\"carol\",\"boss\"],[\"%s\",\"carol\",\"boss\"]]", bucket,
                 bucket);
    } else {
        snprintf(p0, sizeof p0, "[[\"%s\",\"bob\",\"friend\"],[\"%s\",\"missing\",\"friend\"]]",
                 bucket, bucket);
    }
    for (int i = 0; i < (two ? 3 : 2); i++) {
        RpbMapRedResp r = RPB_MAP_RED_RESP__INIT;
        if ((two && i < 2) || (!two && i < 1)) {
            r.has_phase = 1;
            r.phase = (uint32_t)i;
            r.has_response = 1;
            r.response = stub_bin(i == 0 ? p0 : p1);
        } else {
            r.has_done = r.done = 1;
        }
        if (stub_send(c, 24, &r.base) != 0) return -1;
    }
    return 0;
}

static int stub_dt_fetch(stub_conn *c, const uint8_t *buf, size_t len) {
    DtFetchReq *q = dt_fetch_req__unpack(NULL, len, buf);
    if (q == NULL) return -1;
    ext.dt_fetches++;
    ext.dt_node_confirms = q->has_node_confirms ? q->node_confirms : 0;
    DtFetchResp r = DT_FETCH_RESP__INIT;
    DtValue v = DT_VALUE__INIT;
    int rc;
    if (stub_is(q->key, "missing")) {
        r.type = DT_FETCH_RESP__DATA_TYPE__COUNTER;
        rc = stub_send(c, 81, &r.base);
    } else if (stub_is(q->type, "counters")) {
        r.type = DT_FETCH_RESP__DATA_TYPE__COUNTER;
        v.has_counter_value = 1;
        v.counter_value = -5;
        r.value = &v;
        rc = stub_send(c, 81, &r.base);
    } else {
        MapField fc = MAP_FIELD__INIT, fs = MAP_FIELD__INIT, fr = MAP_FIELD__INIT,
                 ff = MAP_FIELD__INIT, fm = MAP_FIELD__INIT, fn = MAP_FIELD__INIT;
        MapEntry ec = MAP_ENTRY__INIT, es = MAP_ENTRY__INIT, er = MAP_ENTRY__INIT,
                 ef = MAP_ENTRY__INIT, em = MAP_ENTRY__INIT, en = MAP_ENTRY__INIT;
        fc.name = stub_bin("visits");
        fc.type = MAP_FIELD__MAP_FIELD_TYPE__COUNTER;
        ec.field = &fc;
        ec.has_counter_value = 1;
        ec.counter_value = 5;
        ProtobufCBinaryData members[2] = {stub_bin("a"), stub_bin("b")};
        fs.name = stub_bin("tags");
        fs.type = MAP_FIELD__MAP_FIELD_TYPE__SET;
        es.field = &fs;
        es.n_set_value = 2;
        es.set_value = members;
        fr.name = stub_bin("name");
        fr.type = MAP_FIELD__MAP_FIELD_TYPE__REGISTER;
        er.field = &fr;
        er.has_register_value = 1;
        er.register_value = stub_bin("hi");
        ff.name = stub_bin("active");
        ff.type = MAP_FIELD__MAP_FIELD_TYPE__FLAG;
        ef.field = &ff;
        ef.has_flag_value = 1;
        ef.flag_value = 1;
        fn.name = stub_bin("n");
        fn.type = MAP_FIELD__MAP_FIELD_TYPE__COUNTER;
        en.field = &fn;
        en.has_counter_value = 1;
        en.counter_value = 1;
        MapEntry *inner[1] = {&en};
        fm.name = stub_bin("sub");
        fm.type = MAP_FIELD__MAP_FIELD_TYPE__MAP;
        em.field = &fm;
        em.n_map_value = 1;
        em.map_value = inner;
        MapEntry *entries[5] = {&ec, &es, &er, &ef, &em};
        r.type = DT_FETCH_RESP__DATA_TYPE__MAP;
        v.n_map_value = 5;
        v.map_value = entries;
        r.value = &v;
        r.has_context = 1;
        r.context.data = (uint8_t *)(uintptr_t)STUB_DT_CTX;
        r.context.len = sizeof STUB_DT_CTX;
        rc = stub_send(c, 81, &r.base);
    }
    dt_fetch_req__free_unpacked(q, NULL);
    return rc;
}

static int stub_dt_update(stub_conn *c, const uint8_t *buf, size_t len) {
    DtUpdateReq *q = dt_update_req__unpack(NULL, len, buf);
    if (q == NULL) return -1;
    const DtOp *op = q->op;
    ext.dt_node_confirms = q->has_node_confirms ? q->node_confirms : 0;
    ext.dt_return_body = q->has_return_body && q->return_body;
    if (op->gset_op && ext.dt_return_body) {
        /* Riak 3.4 cannot encode a gset in an update response */
        dt_update_req__free_unpacked(q, NULL);
        return stub_crash(c, "riak_pb_dt_codec,encode_update_response");
    }
    stub.dt_kind = op->counter_op ? 1 : op->set_op ? 2 : op->map_op ? 3 : op->hll_op ? 4 : 5;
    stub.dt_increment = op->counter_op ? op->counter_op->increment : 0;
    stub.dt_adds = op->set_op    ? op->set_op->n_adds
                   : op->hll_op  ? op->hll_op->n_adds
                   : op->gset_op ? op->gset_op->n_adds
                                 : 0;
    stub.dt_removes = op->set_op ? op->set_op->n_removes : 0;
    stub.dt_map_updates = op->map_op ? op->map_op->n_updates : 0;
    stub.dt_map_removes = op->map_op ? op->map_op->n_removes : 0;
    stub.dt_context_len = q->has_context ? q->context.len : 0;
    if (q->has_context && q->context.len <= sizeof stub.dt_context)
        memcpy(stub.dt_context, q->context.data, q->context.len);
    int rc;
    if (stub.dt_removes && !q->has_context) {
        rc = stub_error(c, "{precondition,{not_present,<<\"zz\">>}}");
    } else {
        DtUpdateResp r = DT_UPDATE_RESP__INIT;
        if (!q->has_key) {
            r.has_key = 1;
            r.key = stub_bin("dtkey");
        }
        if (q->has_return_body && q->return_body) {
            r.has_counter_value = 1;
            r.counter_value = 7;
            r.has_context = 1;
            r.context.data = (uint8_t *)(uintptr_t)STUB_DT_CTX;
            r.context.len = sizeof STUB_DT_CTX;
        }
        rc = stub_send(c, 83, &r.base);
    }
    dt_update_req__free_unpacked(q, NULL);
    return rc;
}

/* ---- Riak 2.9+/3.x requests --------------------------------------------- */

static const uint8_t STUB_CLIENT_ID[] = {0x01, 0x02, 0x03};

/* A crash of the server's handler: Riak sends the error, then closes. */
static int stub_crash(stub_conn *c, const char *what) {
    char msg[160];
    snprintf(msg, sizeof msg, "Error processing incoming message: error:function_clause:[{%s}]",
             what);
    stub_error(c, msg);
    atomic_fetch_add(&ext.dropped, 1);
    /* the close follows the error with a delay, so a client that reused the
     * connection would send its next request into it */
    stub_sleep_ms(200);
    return -1;
}

/* <<Id:32/big, zlib(Seg)>> as Riak sends tree branches. */
static ProtobufCBinaryData stub_branch(uint32_t id, uint8_t fill, size_t seg_len) {
    uint8_t *seg = malloc(seg_len);
    memset(seg, fill, seg_len);
    uLongf zlen = compressBound(seg_len);
    uint8_t *out = malloc(4 + zlen);
    out[0] = (uint8_t)(id >> 24U);
    out[1] = (uint8_t)(id >> 16U);
    out[2] = (uint8_t)(id >> 8U);
    out[3] = (uint8_t)id;
    compress(out + 4, &zlen, seg, seg_len);
    free(seg);
    ProtobufCBinaryData b = {4 + zlen, out};
    return b;
}

static int stub_tree(stub_conn *c, int size, size_t level1_len, const uint32_t *ids, size_t n) {
    RpbAaeFoldTreeResp r = RPB_AAE_FOLD_TREE_RESP__INIT;
    if (atomic_load(&ext.bad_tree_size)) size = 9;
    r.size = (RpbAaeFoldTreeResp__RpbTreeSize)size;
    uint8_t *l1 = calloc(level1_len ? level1_len : 1, 1);
    if (level1_len) {
        l1[0] = 0xAB; /* a non-empty first hash */
        r.has_level_one = 1;
        r.level_one.data = l1;
        r.level_one.len = level1_len;
    }
    ProtobufCBinaryData l2[4];
    for (size_t i = 0; i < n; i++)
        l2[i] = stub_branch(ids[i], (uint8_t)(0x10 + i), 256 * 4);
    r.n_level_two = n;
    r.level_two = l2;
    int rc = stub_send(c, 220, &r.base);
    for (size_t i = 0; i < n; i++)
        free(l2[i].data);
    free(l1);
    return rc;
}

static int stub_keys_clocks(stub_conn *c, int typed) {
    RpbKeysValue kv[2];
    RpbKeysValue *p[2] = {&kv[0], &kv[1]};
    const char *keys[2] = {"k1", "k2"};
    for (int i = 0; i < 2; i++) {
        rpb_keys_value__init(&kv[i]);
        if (typed) {
            kv[i].has_type = 1;
            kv[i].type = stub_bin("maps");
        }
        kv[i].bucket = stub_bin("aaeb");
        kv[i].key = stub_bin(keys[i]);
        kv[i].value.data = (uint8_t *)(uintptr_t)STUB_VCLOCK;
        kv[i].value.len = sizeof STUB_VCLOCK;
    }
    RpbAaeFoldKeyValueResp r = RPB_AAE_FOLD_KEY_VALUE_RESP__INIT;
    r.response_type = stub_bin("clock");
    r.n_keys_value = 2;
    r.keys_value = p;
    return stub_send(c, 221, &r.base);
}

/* The count comes second, after an entry the client must skip. */
static int stub_count(stub_conn *c, const char *type, uint32_t n) {
    RpbKeysCount other = RPB_KEYS_COUNT__INIT, kc = RPB_KEYS_COUNT__INIT;
    RpbKeysCount *p[2] = {&other, &kc};
    other.tag = stub_bin("other_count");
    other.count = 999;
    kc.tag = stub_bin("dispatched_count");
    kc.count = n;
    RpbAaeFoldKeyCountResp r = RPB_AAE_FOLD_KEY_COUNT_RESP__INIT;
    r.response_type = stub_bin(type);
    r.n_keys_count = 2;
    r.keys_count = p;
    return stub_send(c, 222, &r.base);
}

/* Fields every range request carries; the macro reads them from any. */
#define STUB_RANGE(q)                                                                              \
    do {                                                                                           \
        ext.aae_type[0] = '\0';                                                                    \
        if ((q)->has_type) stub_copy(ext.aae_type, sizeof ext.aae_type, (q)->type);                \
        stub_copy(ext.aae_bucket, sizeof ext.aae_bucket, (q)->bucket);                             \
        ext.aae_start[0] = ext.aae_end[0] = '\0';                                                  \
        if ((q)->has_start_key) stub_copy(ext.aae_start, sizeof ext.aae_start, (q)->start_key);    \
        if ((q)->has_end_key) stub_copy(ext.aae_end, sizeof ext.aae_end, (q)->end_key);            \
        ext.aae_key_range = (q)->key_range;                                                        \
        ext.aae_mod_range = (q)->modified_range;                                                   \
        ext.aae_mod_start = (q)->last_mod_start;                                                   \
        ext.aae_mod_end = (q)->last_mod_end;                                                       \
    } while (0)

/* Segment filter fields of the two requests that carry them. */
#define STUB_SEGS(q)                                                                               \
    do {                                                                                           \
        memset(ext.aae_segs, 0, sizeof ext.aae_segs);                                              \
        for (size_t s_ = 0; s_ < (q)->n_id_filter && s_ < 4; s_++)                                 \
            ext.aae_segs[s_] = (q)->id_filter[s_];                                                 \
        ext.aae_filter_size = (q)->has_filter_tree_size ? (unsigned)(q)->filter_tree_size + 1 : 0; \
    } while (0)

/* -1 closes the connection, 0 served, 1 not an extended request. */
static int stub_handle_ext(stub_conn *c, uint8_t code, const uint8_t *buf, size_t len) {
    ext.received[code]++;
    int drop = code;
    if (atomic_compare_exchange_strong(&ext.drop_code, &drop, 0)) {
        return -1; /* read, maybe applied, never answered */
    }
    if (atomic_load(&ext.old_node) && (code == 3 || code == 40 || code == 50 || code == 52 ||
                                       code == 128 || code == 129 || code >= 202)) {
        char msg[48];
        snprintf(msg, sizeof msg, "Unknown message code: %u", code);
        return stub_error(c, msg);
    }
    switch (code) {
    case 3: {
        RpbGetClientIdResp r = RPB_GET_CLIENT_ID_RESP__INIT;
        r.client_id.data = (uint8_t *)(uintptr_t)STUB_CLIENT_ID;
        r.client_id.len = sizeof STUB_CLIENT_ID;
        return stub_send(c, 4, &r.base);
    }
    case 32: {
        RpbSetBucketTypeReq *q = rpb_set_bucket_type_req__unpack(NULL, len, buf);
        if (q == NULL) return -1;
        stub_copy(ext.type_props_type, sizeof ext.type_props_type, q->type);
        ext.type_notfound_ok = q->props->has_notfound_ok ? (int)q->props->notfound_ok : -1;
        rpb_set_bucket_type_req__free_unpacked(q, NULL);
        return stub_send(c, 22, NULL);
    }
    case 40: {
        RpbCSBucketReq *q = rpb_csbucket_req__unpack(NULL, len, buf);
        if (q == NULL) return -1;
        int fail = stub_is(q->bucket, "foldfail");
        stub_copy(ext.fold_start, sizeof ext.fold_start, q->start_key);
        stub_copy(ext.fold_bucket, sizeof ext.fold_bucket, q->bucket);
        ext.fold_max = q->has_max_results ? q->max_results : 0;
        ext.fold_cont[0] = ext.fold_type[0] = '\0';
        if (q->has_continuation) stub_copy(ext.fold_cont, sizeof ext.fold_cont, q->continuation);
        if (q->has_type) stub_copy(ext.fold_type, sizeof ext.fold_type, q->type);
        ext.fold_end[0] = '\0';
        if (q->has_end_key) stub_copy(ext.fold_end, sizeof ext.fold_end, q->end_key);
        ext.fold_start_incl = q->start_incl;
        ext.fold_end_incl = q->end_incl;
        rpb_csbucket_req__free_unpacked(q, NULL);
        int rc = 0;
        for (int batch = 0; rc == 0 && batch < 2; batch++) {
            RpbCSBucketResp r = RPB_CSBUCKET_RESP__INIT;
            RpbContent ct;
            RpbPair meta, idx;
            RpbLink link;
            RpbPair *metap, *idxp;
            RpbLink *linkp;
            stub_content(&ct, batch ? "fold-2" : "fold-1", &meta, &idx, &link, &metap, &idxp,
                         &linkp);
            RpbContent *cts[1] = {&ct};
            RpbGetResp g = RPB_GET_RESP__INIT;
            g.n_content = 1;
            g.content = cts;
            g.has_vclock = 1;
            g.vclock.data = (uint8_t *)(uintptr_t)STUB_VCLOCK;
            g.vclock.len = sizeof STUB_VCLOCK;
            RpbIndexObject io = RPB_INDEX_OBJECT__INIT;
            io.key = stub_bin(batch ? "f2" : "f1");
            io.object = &g;
            RpbIndexObject *ios[1] = {&io};
            r.n_objects = 1;
            r.objects = ios;
            if (batch == 1 || fail) {
                r.has_continuation = 1;
                r.continuation = stub_bin("next-page");
                r.has_done = r.done = !fail;
            }
            rc = stub_send(c, 41, &r.base);
            if (rc == 0 && fail) return stub_error(c, "fold failed"); /* after the continuation */
        }
        return rc;
    }
    case 50: {
        RpbCounterUpdateReq *q = rpb_counter_update_req__unpack(NULL, len, buf);
        if (q == NULL) return -1;
        ext.counter_amount = q->amount;
        ext.counter_returnvalue = q->has_returnvalue && q->returnvalue;
        ext.node_confirms = q->has_node_confirms ? q->node_confirms : 0;
        RpbCounterUpdateResp r = RPB_COUNTER_UPDATE_RESP__INIT;
        if (ext.counter_returnvalue) {
            r.has_value = 1;
            r.value = 40 + q->amount;
        }
        rpb_counter_update_req__free_unpacked(q, NULL);
        return stub_send(c, 51, &r.base);
    }
    case 52: {
        RpbCounterGetReq *q = rpb_counter_get_req__unpack(NULL, len, buf);
        if (q == NULL) return -1;
        ext.counter_r = q->has_r ? q->r : 0;
        ext.counter_basic_quorum = q->has_basic_quorum ? (int)q->basic_quorum : -1;
        ext.counter_notfound_ok = q->has_notfound_ok ? (int)q->notfound_ok : -1;
        RpbCounterGetResp r = RPB_COUNTER_GET_RESP__INIT;
        if (!stub_is(q->key, "missing")) {
            r.has_value = 1;
            r.value = 42;
        }
        rpb_counter_get_req__free_unpacked(q, NULL);
        return stub_send(c, 53, &r.base);
    }
    case 128: {
        /* riak_repl: the payload repeats the code ("tunnelled") */
        if (len < 1 || buf[0] != 128) return stub_crash(c, "riak_repl_pb_get,process");
        RpbReplGetReq *q = rpb_repl_get_req__unpack(NULL, len - 1, buf + 1);
        if (q == NULL) return -1;
        stub_copy(ext.repl_cluster, sizeof ext.repl_cluster, q->cluster_id);
        int missing = stub_is(q->key, "missing");
        rpb_repl_get_req__free_unpacked(q, NULL);
        RpbGetResp r = RPB_GET_RESP__INIT;
        RpbContent ct;
        RpbPair meta, idx;
        RpbLink link;
        RpbPair *metap, *idxp;
        RpbLink *linkp;
        stub_content(&ct, "proxied", &meta, &idx, &link, &metap, &idxp, &linkp);
        RpbContent *cts[1] = {&ct};
        if (!missing) {
            r.n_content = 1;
            r.content = cts;
        }
        return stub_send(c, 10, &r.base);
    }
    case 129: {
        if (len != 1 || buf[0] != 129) return stub_crash(c, "riak_repl_pb_get,process");
        RpbReplGetClusterIdResp r = RPB_REPL_GET_CLUSTER_ID_RESP__INIT;
        r.cluster_id = stub_bin("{'riak@stub',{1,2,3}}");
        return stub_send(c, 130, &r.base);
    }
    case 202: {
        RpbFetchReq *q = rpb_fetch_req__unpack(NULL, len, buf);
        if (q == NULL) return -1;
        stub_copy(ext.fetch_queue, sizeof ext.fetch_queue, q->queuename);
        ext.fetch_encoding[0] = '\0';
        if (q->has_encoding) stub_copy(ext.fetch_encoding, sizeof ext.fetch_encoding, q->encoding);
        RpbFetchResp r = RPB_FETCH_RESP__INIT;
        static const uint8_t obj[] = {0x83, 0x68, 0x02, 0x01, 0x02, 0x03};
        int rc;
        if (stub_is(q->queuename, "gone")) {
            rc = stub_error(c, "notfound");
        } else if (stub_is(q->queuename, "empty")) {
            r.queue_empty = 1;
            rc = stub_send(c, 203, &r.base);
        } else {
            r.queue_empty = 0;
            r.has_replencoded_object = 1;
            r.replencoded_object.data = (uint8_t *)(uintptr_t)obj;
            r.replencoded_object.len = sizeof obj;
            r.has_crc_check = 1;
            r.crc_check = 0xDEADBEEF;
            r.has_segment_id = r.has_segment_hash = 1;
            r.segment_id = 77;
            r.segment_hash = 88;
            if (stub_is(q->queuename, "tomb")) {
                r.has_deleted = r.deleted = 1;
                r.has_deleted_vclock = 1;
                r.deleted_vclock.data = (uint8_t *)(uintptr_t)STUB_VCLOCK;
                r.deleted_vclock.len = sizeof STUB_VCLOCK;
            }
            rc = stub_send(c, 203, &r.base);
        }
        rpb_fetch_req__free_unpacked(q, NULL);
        return rc;
    }
    case 204: {
        RpbPushReq *q = rpb_push_req__unpack(NULL, len, buf);
        if (q == NULL) return -1;
        ext.push_keys = q->n_keys_value;
        ext.push_vclock_len = q->n_keys_value ? q->keys_value[0]->value.len : 0;
        for (size_t i = 0; i < 2; i++) {
            ext.push_type[i][0] = ext.push_bucket[i][0] = ext.push_key[i][0] = '\0';
            if (i >= q->n_keys_value) continue;
            const RpbKeysValue *kv = q->keys_value[i];
            if (kv->has_type) stub_copy(ext.push_type[i], sizeof ext.push_type[i], kv->type);
            stub_copy(ext.push_bucket[i], sizeof ext.push_bucket[i], kv->bucket);
            stub_copy(ext.push_key[i], sizeof ext.push_key[i], kv->key);
        }
        rpb_push_req__free_unpacked(q, NULL);
        RpbPushResp r = RPB_PUSH_RESP__INIT;
        r.queue_exists = 1;
        r.has_foldq_length = r.has_fsync_length = r.has_realt_length = 1;
        r.foldq_length = 1;
        r.fsync_length = 2;
        r.realt_length = 3;
        return stub_send(c, 205, &r.base);
    }
    case 206: {
        /* Riak 3.4 only accepts the request without a body */
        if (len > 0) return stub_crash(c, "riak_kv_pb_object,process");
        RpbClusterMemberEntry e = RPB_CLUSTER_MEMBER_ENTRY__INIT;
        e.ip = stub_bin("10.0.0.7");
        e.port = 8087;
        RpbClusterMemberEntry *all[1] = {&e};
        RpbMembershipResp r = RPB_MEMBERSHIP_RESP__INIT;
        r.n_up_nodes = 1;
        r.up_nodes = all;
        return stub_send(c, 207, &r.base);
    }
    case 210: {
        RpbAaeFoldMergeRootNValReq *q = rpb_aae_fold_merge_root_nval_req__unpack(NULL, len, buf);
        if (q == NULL) return -1;
        ext.aae_n_val = q->n_val;
        rpb_aae_fold_merge_root_nval_req__free_unpacked(q, NULL);
        return stub_tree(c, 4, 4096 * 4, NULL, 0);
    }
    case 211: {
        RpbAaeFoldMergeBranchNValReq *q =
            rpb_aae_fold_merge_branch_nval_req__unpack(NULL, len, buf);
        if (q == NULL) return -1;
        ext.aae_n_val = q->n_val;
        ext.aae_ids = (unsigned)q->n_id_filter;
        uint32_t ids[2] = {q->n_id_filter ? q->id_filter[0] : 0, 9};
        rpb_aae_fold_merge_branch_nval_req__free_unpacked(q, NULL);
        return stub_tree(c, 4, 0, ids, 2);
    }
    case 212: {
        RpbAaeFoldFetchClocksNValReq *q =
            rpb_aae_fold_fetch_clocks_nval_req__unpack(NULL, len, buf);
        if (q == NULL) return -1;
        ext.aae_n_val = q->n_val;
        ext.aae_ids = (unsigned)q->n_id_filter;
        rpb_aae_fold_fetch_clocks_nval_req__free_unpacked(q, NULL);
        return stub_keys_clocks(c, 0);
    }
    case 213: {
        RpbAaeFoldMergeTreesRangeReq *q =
            rpb_aae_fold_merge_trees_range_req__unpack(NULL, len, buf);
        if (q == NULL) return -1;
        STUB_RANGE(q);
        STUB_SEGS(q);
        ext.aae_tree_size = q->tree_size;
        ext.aae_seg_filter = q->segment_filter;
        ext.aae_ids = (unsigned)q->n_id_filter;
        ext.aae_prehash = q->use_prehash;
        ext.aae_iv = q->init_vector;
        int size = q->tree_size;
        rpb_aae_fold_merge_trees_range_req__free_unpacked(q, NULL);
        uint32_t ids[2] = {0, 1};
        return stub_tree(c, size, 16 * 4, ids, 2);
    }
    case 214: {
        RpbAaeFoldFetchClocksRangeReq *q =
            rpb_aae_fold_fetch_clocks_range_req__unpack(NULL, len, buf);
        if (q == NULL) return -1;
        STUB_RANGE(q);
        STUB_SEGS(q);
        ext.aae_seg_filter = q->segment_filter;
        int typed = q->has_type;
        rpb_aae_fold_fetch_clocks_range_req__free_unpacked(q, NULL);
        return stub_keys_clocks(c, typed);
    }
    case 215: {
        RpbAaeFoldFindKeysReq *q = rpb_aae_fold_find_keys_req__unpack(NULL, len, buf);
        if (q == NULL) return -1;
        STUB_RANGE(q);
        ext.aae_finder = q->finder;
        ext.aae_limit = q->find_limit;
        rpb_aae_fold_find_keys_req__free_unpacked(q, NULL);
        RpbKeysCount kc = RPB_KEYS_COUNT__INIT;
        RpbKeysCount *p[1] = {&kc};
        kc.tag = stub_bin("big-key");
        kc.count = 4096;
        RpbAaeFoldKeyCountResp r = RPB_AAE_FOLD_KEY_COUNT_RESP__INIT;
        r.response_type = stub_bin("find_keys");
        r.n_keys_count = 1;
        r.keys_count = p;
        return stub_send(c, 222, &r.base);
    }
    case 216: {
        RpbAaeFoldObjectStatsReq *q = rpb_aae_fold_object_stats_req__unpack(NULL, len, buf);
        if (q == NULL) return -1;
        STUB_RANGE(q);
        rpb_aae_fold_object_stats_req__free_unpacked(q, NULL);
        RpbKeysCount kc[5];
        RpbKeysCount *p[5];
        const char *tags[5] = {"total_count", "total_size", "sizes", "sizes", "siblings"};
        unsigned orders[5] = {0, 0, 2, 3, 1}, counts[5] = {12, 3400, 10, 2, 12};
        for (int i = 0; i < 5; i++) {
            rpb_keys_count__init(&kc[i]);
            kc[i].tag = stub_bin(tags[i]);
            kc[i].has_order = i >= 2;
            kc[i].order = orders[i];
            kc[i].count = counts[i];
            p[i] = &kc[i];
        }
        RpbAaeFoldKeyCountResp r = RPB_AAE_FOLD_KEY_COUNT_RESP__INIT;
        r.response_type = stub_bin("stats");
        r.n_keys_count = 5;
        r.keys_count = p;
        return stub_send(c, 222, &r.base);
    }
    case 217: {
        RpbAaeFoldReplKeysReq *q = rpb_aae_fold_repl_keys_req__unpack(NULL, len, buf);
        if (q == NULL) return -1;
        STUB_RANGE(q);
        stub_copy(ext.aae_queue, sizeof ext.aae_queue, q->queuename);
        rpb_aae_fold_repl_keys_req__free_unpacked(q, NULL);
        return stub_count(c, "repl_keys", 5);
    }
    case 218: {
        RpbAaeFoldFindTombsReq *q = rpb_aae_fold_find_tombs_req__unpack(NULL, len, buf);
        if (q == NULL) return -1;
        STUB_RANGE(q);
        rpb_aae_fold_find_tombs_req__free_unpacked(q, NULL);
        return stub_keys_clocks(c, 0);
    }
    case 219: {
        RpbAaeFoldRepairKeysReq *q = rpb_aae_fold_repair_keys_req__unpack(NULL, len, buf);
        if (q == NULL) return -1;
        STUB_RANGE(q);
        rpb_aae_fold_repair_keys_req__free_unpacked(q, NULL);
        return stub_count(c, "repair_keys", 6);
    }
    case 223: {
        RpbAaeFoldListBucketsReq *q = rpb_aae_fold_list_buckets_req__unpack(NULL, len, buf);
        if (q == NULL) return -1;
        ext.aae_n_val = q->n_val;
        rpb_aae_fold_list_buckets_req__free_unpacked(q, NULL);
        RpbAaeFoldBucket a = RPB_AAE_FOLD_BUCKET__INIT, b = RPB_AAE_FOLD_BUCKET__INIT;
        a.bucket = stub_bin("plain");
        b.has_type = 1;
        b.type = stub_bin("maps");
        b.bucket = stub_bin("typed");
        RpbAaeFoldBucket *all[2] = {&a, &b};
        RpbAaeFoldListBucketsResp r = RPB_AAE_FOLD_LIST_BUCKETS_RESP__INIT;
        r.n_bucket_list = 2;
        r.bucket_list = all;
        return stub_send(c, 224, &r.base);
    }
    case 230: {
        RpbAaeFoldReapTombsReq *q = rpb_aae_fold_reap_tombs_req__unpack(NULL, len, buf);
        if (q == NULL) return -1;
        STUB_RANGE(q);
        ext.aae_change = q->change_method;
        ext.aae_job = q->job_id;
        rpb_aae_fold_reap_tombs_req__free_unpacked(q, NULL);
        return stub_count(c, "reap_tombs", 7);
    }
    case 231: {
        RpbAaeFoldEraseKeysReq *q = rpb_aae_fold_erase_keys_req__unpack(NULL, len, buf);
        if (q == NULL) return -1;
        STUB_RANGE(q);
        ext.aae_change = q->change_method;
        ext.aae_job = q->job_id;
        rpb_aae_fold_erase_keys_req__free_unpacked(q, NULL);
        return stub_count(c, "erase_keys", 8);
    }
    default:
        return 1;
    }
}

/* One request message; -1 closes the connection. */
static int stub_handle(stub_conn *c, uint8_t code, const uint8_t *buf, size_t len) {
    atomic_fetch_add(&stub.requests, 1);
    int ext_rc = stub_handle_ext(c, code, buf, len);
    if (ext_rc <= 0) {
        return ext_rc;
    }
    switch (code) {
    case 1:
        return stub_send(c, 2, NULL);
    case 5: {
        RpbSetClientIdReq *q = rpb_set_client_id_req__unpack(NULL, len, buf);
        if (q == NULL) return -1;
        stub_copy(stub.client_id, sizeof stub.client_id, q->client_id);
        rpb_set_client_id_req__free_unpacked(q, NULL);
        return stub_send(c, 6, NULL);
    }
    case 7: {
        RpbGetServerInfoResp r = RPB_GET_SERVER_INFO_RESP__INIT;
        r.has_node = r.has_server_version = 1;
        r.node = stub_bin("riak@stub");
        r.server_version = stub_bin("3.2.0");
        return stub_send(c, 8, &r.base);
    }
    case 9:
        return stub_get(c, buf, len);
    case 11:
        return stub_put(c, buf, len);
    case 13:
        return stub_send(c, 14, NULL);
    case 15:
        return stub_names(c, 16, 0);
    case 17:
        return stub_names(c, 18, 1);
    case 19:
    case 31:
        return stub_props(c);
    case 21: {
        RpbSetBucketReq *q = rpb_set_bucket_req__unpack(NULL, len, buf);
        if (q == NULL) return -1;
        stub.set_n_val = q->props->has_n_val ? q->props->n_val : 0;
        stub.set_allow_mult = q->props->has_allow_mult && q->props->allow_mult;
        stub.set_precommit = q->props->n_precommit;
        ext.props_sync = q->props->has_sync_on_write ? (unsigned)q->props->sync_on_write + 1 : 0;
        ext.props_node_confirms = q->props->has_node_confirms ? q->props->node_confirms + 1 : 0;
        ext.props_aae_exclude =
            q->props->has_aae_tree_exclude ? (int)q->props->aae_tree_exclude : -1;
        rpb_set_bucket_req__free_unpacked(q, NULL);
        return stub_send(c, 22, NULL);
    }
    case 23: {
        RpbMapRedReq *q = rpb_map_red_req__unpack(NULL, len, buf);
        if (q == NULL) return -1;
        char job[512];
        stub_copy(job, sizeof job, q->request);
        rpb_map_red_req__free_unpacked(q, NULL);
        return strstr(job, "\"link\"") ? stub_links(c, job) : stub_mapred(c);
    }
    case 25:
        return stub_index(c, buf, len);
    case 27:
        return stub_search(c);
    case 29:
        return stub_send(c, 30, NULL);
    case 33: {
        RpbBucketKeyPreflistItem a = RPB_BUCKET_KEY_PREFLIST_ITEM__INIT;
        RpbBucketKeyPreflistItem b = RPB_BUCKET_KEY_PREFLIST_ITEM__INIT;
        a.partition = 1;
        a.node = stub_bin("riak@a");
        a.primary = 1;
        b.partition = 2;
        b.node = stub_bin("riak@b");
        RpbBucketKeyPreflistItem *items[2] = {&a, &b};
        RpbGetBucketKeyPreflistResp r = RPB_GET_BUCKET_KEY_PREFLIST_RESP__INIT;
        r.n_preflist = 2;
        r.preflist = items;
        return stub_send(c, 34, &r.base);
    }
    case 54: {
        RpbYokozunaIndexGetReq *q = rpb_yokozuna_index_get_req__unpack(NULL, len, buf);
        if (q == NULL) return -1;
        int missing = q->has_name && stub_is(q->name, "missing");
        rpb_yokozuna_index_get_req__free_unpacked(q, NULL);
        if (missing) return stub_error(c, "notfound");
        RpbYokozunaIndex idx = RPB_YOKOZUNA_INDEX__INIT;
        idx.name = stub_bin("people");
        idx.has_schema = 1;
        idx.schema = stub_bin("_yz_default");
        idx.has_n_val = 1;
        idx.n_val = 3;
        RpbYokozunaIndex *all[1] = {&idx};
        RpbYokozunaIndexGetResp r = RPB_YOKOZUNA_INDEX_GET_RESP__INIT;
        r.n_index = 1;
        r.index = all;
        return stub_send(c, 55, &r.base);
    }
    case 56:
    case 60:
        return stub_send(c, 12, NULL);
    case 57:
        return stub_send(c, 14, NULL);
    case 58: {
        RpbYokozunaSchema s = RPB_YOKOZUNA_SCHEMA__INIT;
        s.name = stub_bin("sch");
        s.has_content = 1;
        s.content = stub_bin("<schema/>");
        RpbYokozunaSchemaGetResp r = RPB_YOKOZUNA_SCHEMA_GET_RESP__INIT;
        r.schema = &s;
        return stub_send(c, 59, &r.base);
    }
    case 70: {
        RpbCoverageEntry e = RPB_COVERAGE_ENTRY__INIT;
        e.ip = stub_bin("10.0.0.1");
        e.port = 8087;
        e.has_keyspace_desc = 1;
        e.keyspace_desc = stub_bin("partition 0");
        e.cover_context.data = (uint8_t *)(uintptr_t)STUB_COVER;
        e.cover_context.len = sizeof STUB_COVER;
        RpbCoverageEntry *all[1] = {&e};
        RpbCoverageResp r = RPB_COVERAGE_RESP__INIT;
        r.n_entries = 1;
        r.entries = all;
        return stub_send(c, 71, &r.base);
    }
    case 80:
        return stub_dt_fetch(c, buf, len);
    case 82:
        return stub_dt_update(c, buf, len);
    case 253: {
        RpbAuthReq *q = rpb_auth_req__unpack(NULL, len, buf);
        if (q == NULL) return -1;
        int ok = stub_is(q->user, "alice") && stub_is(q->password, "secret");
        rpb_auth_req__free_unpacked(q, NULL);
        if (!ok) return stub_error(c, "Authentication failed");
        atomic_fetch_add(&stub.auth_ok, 1);
        return stub_send(c, 254, NULL);
    }
    case 255:
        if (stub_tls_ctx == NULL) return stub_error(c, "Security not enabled");
        if (stub_send(c, 255, NULL) != 0 || atomic_load(&stub.tls_drop)) return -1;
        c->ssl = SSL_new(stub_tls_ctx);
        SSL_set_fd(c->ssl, c->fd);
        if (SSL_accept(c->ssl) != 1) return -1;
        atomic_store(&stub.tls_version, SSL_version(c->ssl));
        return 0;
    default:
        return stub_error(c, "unknown message");
    }
}

static void *stub_serve(void *arg) {
    stub_conn c = {.fd = (int)(intptr_t)arg};
    for (;;) {
        uint8_t hdr[5];
        if (stub_io(&c, hdr, 5, 0) != 0) break;
        uint32_t n =
            ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) | hdr[3];
        if (n == 0 || n > (1u << 24)) break;
        uint8_t *buf = malloc(n);
        if (n > 1 && stub_io(&c, buf, n - 1, 0) != 0) {
            free(buf);
            break;
        }
        int rc = stub_handle(&c, hdr[4], buf, n - 1);
        free(buf);
        if (rc != 0) break;
    }
    if (c.ssl) SSL_free(c.ssl);
    close(c.fd);
    return NULL;
}

static void *stub_accept(void *arg) {
    int lfd = (int)(intptr_t)arg;
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) continue;
        atomic_fetch_add(&stub.accepts, 1);
        pthread_t t;
        pthread_create(&t, NULL, stub_serve, (void *)(intptr_t)fd);
        pthread_detach(t);
    }
    return NULL;
}

/* A stub listening on 127.0.0.1; returns its port. */
static unsigned stub_start(void) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(lfd, (struct sockaddr *)&a, sizeof a);
    listen(lfd, 64);
    socklen_t len = sizeof a;
    getsockname(lfd, (struct sockaddr *)&a, &len);
    pthread_t t;
    pthread_create(&t, NULL, stub_accept, (void *)(intptr_t)lfd);
    pthread_detach(t);
    return ntohs(a.sin_port);
}

/* STARTTLS is answered with this certificate; a client certificate is
 * requested (and verified against ca) but not required. */
static int stub_start_tls(const char *cert, const char *key, const char *ca) {
    /* sessions still open hold their own reference to the old context */
    SSL_CTX_free(stub_tls_ctx);
    stub_tls_ctx = SSL_CTX_new(TLS_server_method());
    if (stub_tls_ctx == NULL || SSL_CTX_use_certificate_chain_file(stub_tls_ctx, cert) != 1 ||
        SSL_CTX_use_PrivateKey_file(stub_tls_ctx, key, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_load_verify_locations(stub_tls_ctx, ca, NULL) != 1) {
        return -1;
    }
    SSL_CTX_set_verify(stub_tls_ctx, SSL_VERIFY_PEER, NULL);
    return 0;
}

#endif /* RIAK_TEST_STUB_PB_H */
