/*
 File:   test_client.c
 SPDX-License-Identifier: GPL-3.0-or-later

 End-to-end tests of the public API against tests/stub_riak.py, which sets
 RIAK_TEST_PORT. Only the public header is used for the library itself.
 */

#define _POSIX_C_SOURCE 200809L /* clock_gettime, nanosleep */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <curl/curl.h>
#include <json-c/json.h>

#include "check.h"
#include "riak/riak.h"

static unsigned port;
static riak_client *rc;

static size_t collect(char *d, size_t s, size_t n, void *u) {
    size_t len = s * n;
    char **buf = u;
    size_t old = *buf ? strlen(*buf) : 0;
    char *p = realloc(*buf, old + len + 1);
    if (p == NULL) return 0;
    memcpy(p + old, d, len);
    p[old + len] = '\0';
    *buf = p;
    return len;
}

static unsigned ports[3];

/* GET a stub control endpoint on one node; returns the body (caller frees). */
static char *stub_get(unsigned on_port, const char *path) {
    char url[96];
    char *body = NULL;
    snprintf(url, sizeof url, "http://127.0.0.1:%u%s", on_port, path);
    CURL *h = curl_easy_init();
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);
    curl_easy_perform(h);
    curl_easy_cleanup(h);
    return body;
}

static void set_mode(int node, const char *mode) {
    char path[32];
    snprintf(path, sizeof path, "/_mode?set=%s", mode);
    free(stub_get(ports[node], path));
}

static long hits(int node) {
    char *body = stub_get(ports[node], "/_hits");
    long n = -1;
    if (body) sscanf(body, "{\"hits\": %ld}", &n);
    free(body);
    return n;
}

/* Previous request as recorded by the stub; caller puts the result. */
static json_object *last_request(void) {
    char url[64];
    char *body = NULL;
    snprintf(url, sizeof url, "http://127.0.0.1:%u/_last", port);
    CURL *h = curl_easy_init();
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);
    curl_easy_perform(h);
    curl_easy_cleanup(h);
    json_object *o = json_tokener_parse(body ? body : "{}");
    free(body);
    return o;
}

static const char *jstr(json_object *o, const char *k) {
    json_object *v;
    return json_object_object_get_ex(o, k, &v) ? json_object_get_string(v) : NULL;
}

static const char *last_header(json_object *o, const char *name) {
    json_object *h;
    return json_object_object_get_ex(o, "headers", &h) ? jstr(h, name) : NULL;
}

static void test_ping(void) {
    CHECK_EQ_INT(riak_ping(rc), RIAK_OK);
}

static void test_put_get_roundtrip_with_escaping(void) {
    riak_kv_pair meta[] = {{"color", "blue"}};
    riak_kv_pair idx[] = {{"age_int", "42"}};
    riak_opts o = {.content_type = "text/plain",
                   .meta = meta,
                   .n_meta = 1,
                   .indexes = idx,
                   .n_indexes = 1,
                   .w = RIAK_Q_QUORUM};
    const char *key = "a/b?c d";
    CHECK_EQ_INT(riak_put(rc, "test", key, "hello", 5, &o, NULL, NULL), RIAK_OK);
    CHECK_EQ_INT(riak_client_http_status(rc), 204);

    json_object *last = last_request();
    CHECK_EQ_STR(jstr(last, "method"), "PUT");
    CHECK_EQ_STR(jstr(last, "path"), "/buckets/test/keys/a%2Fb%3Fc%20d?w=quorum");
    CHECK_EQ_STR(last_header(last, "Content-Type"), "text/plain");
    CHECK_EQ_STR(last_header(last, "X-Riak-Meta-color"), "blue");
    CHECK_EQ_STR(last_header(last, "X-Riak-Index-age_int"), "42");
    json_object_put(last);

    riak_object obj;
    CHECK_EQ_INT(riak_get(rc, "test", key, NULL, &obj), RIAK_OK);
    CHECK_EQ_INT(obj.len, 5);
    CHECK_EQ_STR(obj.data, "hello");
    CHECK_EQ_STR(obj.content_type, "text/plain");
    CHECK(obj.vclock != NULL && strncmp(obj.vclock, "vc-", 3) == 0);
    CHECK(obj.etag != NULL && obj.etag[0] == '"');
    CHECK_EQ_INT(obj.n_meta, 1);
    if (obj.n_meta == 1) {
        CHECK_EQ_STR(obj.meta[0].name, "color");
        CHECK_EQ_STR(obj.meta[0].value, "blue");
    }
    CHECK_EQ_INT(obj.n_indexes, 1);
    CHECK(obj.last_modified != NULL);
    riak_object_free(&obj);
}

/* Regression: the old library left CURLOPT_CUSTOMREQUEST=DELETE on the
 * handle, so the next "get" deleted data. */
static void test_get_after_delete_is_get(void) {
    CHECK_EQ_INT(riak_put(rc, "test", "gone", "x", 1, NULL, NULL, NULL), RIAK_OK);
    CHECK_EQ_INT(riak_delete(rc, "test", "gone", NULL), RIAK_OK);
    riak_object obj;
    CHECK_EQ_INT(riak_get(rc, "test", "gone", NULL, &obj), RIAK_E_NOT_FOUND);
    json_object *last = last_request();
    CHECK_EQ_STR(jstr(last, "method"), "GET");
    json_object_put(last);
    CHECK(obj.data == NULL);
    /* deleting a missing key is fine */
    CHECK_EQ_INT(riak_delete(rc, "test", "gone", NULL), RIAK_OK);
}

static void test_put_return_body(void) {
    riak_opts o = {.content_type = "application/json", .return_body = true};
    riak_object obj;
    CHECK_EQ_INT(riak_put(rc, "test", "doc", "{}", 2, &o, &obj, NULL), RIAK_OK);
    CHECK_EQ_STR(obj.data, "{}");
    CHECK_EQ_STR(obj.content_type, "application/json");
    riak_object_free(&obj);
}

static void test_autokey(void) {
    char *key = NULL;
    CHECK_EQ_INT(riak_put(rc, "test", NULL, "v", 1, NULL, NULL, &key), RIAK_OK);
    CHECK_EQ_STR(key, "auto key");
    riak_free(key);
    json_object *last = last_request();
    CHECK_EQ_STR(jstr(last, "method"), "POST");
    json_object_put(last);
}

static void test_status_mapping(void) {
    riak_object obj;
    CHECK_EQ_INT(riak_get(rc, "siblings", "k", NULL, &obj), RIAK_E_SIBLINGS);
    CHECK(strstr(riak_client_errmsg(rc), "300") != NULL);
    riak_opts o = {.vclock = "stale"};
    CHECK_EQ_INT(riak_put(rc, "test", "k", "v", 1, &o, NULL, NULL), RIAK_E_PRECONDITION);
    /* plain HTTP against a node with security enabled */
    CHECK_EQ_INT(riak_get(rc, "needs-https", "k", NULL, &obj), RIAK_E_UNAUTHORIZED);
    CHECK_EQ_INT(riak_client_http_status(rc), 426);
}

static void test_list_keys_stream(void) {
    riak_strlist l;
    CHECK_EQ_INT(riak_list_keys(rc, "many", &l), RIAK_OK);
    CHECK_EQ_INT(l.count, 1000);
    if (l.count == 1000) {
        CHECK_EQ_STR(l.items[0], "key-0");
        CHECK_EQ_STR(l.items[999], "key-999");
        CHECK(l.items[1000] == NULL);
    }
    riak_strlist_free(&l);
}

static void test_list_buckets(void) {
    riak_strlist l;
    CHECK_EQ_INT(riak_list_buckets(rc, &l), RIAK_OK);
    bool found = false;
    for (char **p = l.items; p && *p; p++) {
        found = found || strcmp(*p, "test") == 0;
    }
    CHECK(found);
    riak_strlist_free(&l);
}

static void test_bucket_props(void) {
    CHECK_EQ_INT(riak_set_bucket_props(rc, "test", "{\"props\":{\"n_val\":5}}"), RIAK_OK);
    char *json = NULL;
    CHECK_EQ_INT(riak_get_bucket_props(rc, "test", &json), RIAK_OK);
    CHECK(json != NULL && strstr(json, "\"n_val\": 5") != NULL);
    riak_free(json);
    CHECK_EQ_INT(riak_reset_bucket_props(rc, "test"), RIAK_OK);
}

static void test_header_injection_rejected(void) {
    riak_opts o = {.content_type = "text/plain\r\nX-Evil: 1"};
    CHECK_EQ_INT(riak_put(rc, "test", "k", "v", 1, &o, NULL, NULL), RIAK_E_INVALID_ARG);
    riak_kv_pair bad[] = {{"bad name", "v"}};
    riak_opts o2 = {.meta = bad, .n_meta = 1};
    CHECK_EQ_INT(riak_put(rc, "test", "k", "v", 1, &o2, NULL, NULL), RIAK_E_INVALID_ARG);
}

static void test_invalid_args(void) {
    riak_object obj;
    riak_strlist l;
    CHECK_EQ_INT(riak_get(rc, "", "k", NULL, &obj), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_get(rc, "b", NULL, NULL, &obj), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_get(NULL, "b", "k", NULL, &obj), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_list_keys(rc, NULL, &l), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_put(rc, "b", "k", NULL, 3, NULL, NULL, NULL), RIAK_E_INVALID_ARG);
    riak_opts o = {.r = -99};
    CHECK_EQ_INT(riak_get(rc, "b", "k", &o, &obj), RIAK_E_INVALID_ARG);
}

static void test_transport_error(void) {
    riak_config cfg;
    riak_client *c = NULL;
    riak_config_init(&cfg);
    cfg.transport = RIAK_TRANSPORT_HTTP;
    cfg.port = 1; /* nothing listens here */
    cfg.connect_timeout_ms = 2000;
    CHECK_EQ_INT(riak_client_new(&cfg, &c), RIAK_OK);
    CHECK_EQ_INT(riak_ping(c), RIAK_E_TRANSPORT);
    CHECK(riak_client_errmsg(c)[0] != '\0');
    riak_client_free(c);
}

static void test_server_errors(void) {
    riak_object obj;
    CHECK_EQ_INT(riak_get(rc, "unavailable", "k", NULL, &obj), RIAK_E_HTTP);
    CHECK_EQ_INT(riak_client_http_status(rc), 503);
    CHECK(strstr(riak_client_errmsg(rc), "503") != NULL);

    char *key = NULL;
    CHECK_EQ_INT(riak_put(rc, "nolocation", NULL, "v", 1, NULL, NULL, &key), RIAK_E_PARSE);
    CHECK(key == NULL);
}

static void test_body_limit(void) {
    riak_config cfg;
    riak_client *c = NULL;
    riak_config_init(&cfg);
    cfg.transport = RIAK_TRANSPORT_HTTP;
    cfg.port = port;
    cfg.max_body_bytes = 100;
    CHECK_EQ_INT(riak_client_new(&cfg, &c), RIAK_OK);
    riak_strlist l;
    CHECK_EQ_INT(riak_list_keys(c, "many", &l), RIAK_E_HTTP);
    CHECK(strstr(riak_client_errmsg(c), "max_body_bytes") != NULL);
    CHECK(l.items == NULL);
    /* the same client keeps working for small responses */
    CHECK_EQ_INT(riak_ping(c), RIAK_OK);
    riak_client_free(c);
}

static void test_bucket_error_paths(void) {
    riak_strlist l;
    CHECK_EQ_INT(riak_list_keys(rc, "unavailable", &l), RIAK_E_HTTP);
    CHECK_EQ_INT(riak_list_keys(rc, "truncated", &l), RIAK_E_PARSE);
    CHECK(l.items == NULL);
    char *json = NULL;
    CHECK_EQ_INT(riak_get_bucket_props(rc, "unavailable", &json), RIAK_E_HTTP);
    CHECK_EQ_INT(riak_get_bucket_props(rc, "notjson", &json), RIAK_E_PARSE);
    CHECK(json == NULL);
    CHECK_EQ_INT(riak_set_bucket_props(rc, "unavailable", "{}"), RIAK_E_HTTP);
    CHECK_EQ_INT(riak_reset_bucket_props(rc, "unavailable"), RIAK_E_HTTP);
}

static void test_null_arguments(void) {
    riak_object obj;
    riak_strlist l;
    char *json;
    CHECK_EQ_INT(riak_ping(NULL), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_get(rc, "b", "k", NULL, NULL), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_put(NULL, "b", "k", "v", 1, NULL, NULL, NULL), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_put(rc, "", "k", "v", 1, NULL, NULL, NULL), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_put(rc, "b", "", "v", 1, NULL, NULL, NULL), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_delete(NULL, "b", "k", NULL), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_delete(rc, "b", "", NULL), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_list_buckets(NULL, &l), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_list_keys(NULL, "b", &l), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_get_bucket_props(rc, "b", NULL), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_get_bucket_props(rc, "", &json), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_set_bucket_props(rc, "b", NULL), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_set_bucket_props(rc, "", "{}"), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_reset_bucket_props(NULL, "b"), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_reset_bucket_props(rc, ""), RIAK_E_INVALID_ARG);
    (void)obj;
}

static void test_bucket_types(void) {
    riak_opts o = {.bucket_type = "t1", .content_type = "text/plain"};
    CHECK_EQ_INT(riak_put(rc, "tb", "k1", "typed", 5, &o, NULL, NULL), RIAK_OK);
    json_object *last = last_request();
    CHECK_EQ_STR(jstr(last, "path"), "/types/t1/buckets/tb/keys/k1");
    json_object_put(last);

    riak_object obj;
    CHECK_EQ_INT(riak_get(rc, "tb", "k1", &o, &obj), RIAK_OK);
    CHECK_EQ_STR(obj.data, "typed");
    riak_object_free(&obj);
    /* the same bucket name in the default type is a different bucket */
    CHECK_EQ_INT(riak_get(rc, "tb", "k1", NULL, &obj), RIAK_E_NOT_FOUND);

    riak_strlist l;
    CHECK_EQ_INT(riak_type_list_keys(rc, "t1", "tb", &l), RIAK_OK);
    CHECK_EQ_INT(l.count, 1);
    riak_strlist_free(&l);
    CHECK_EQ_INT(riak_type_list_buckets(rc, "t1", &l), RIAK_OK);
    CHECK(l.count == 1 && strcmp(l.items[0], "tb") == 0);
    riak_strlist_free(&l);

    char *json = NULL;
    CHECK_EQ_INT(riak_type_set_bucket_props(rc, "t1", "tb", "{\"props\":{\"n_val\":1}}"), RIAK_OK);
    CHECK_EQ_INT(riak_type_get_bucket_props(rc, "t1", "tb", &json), RIAK_OK);
    CHECK(json && strstr(json, "\"n_val\": 1"));
    riak_free(json);
    CHECK_EQ_INT(riak_type_reset_bucket_props(rc, "t1", "tb"), RIAK_OK);
    CHECK_EQ_INT(riak_get_type_props(rc, "multi", &json), RIAK_OK);
    CHECK(json && strstr(json, "\"allow_mult\": true"));
    riak_free(json);
    CHECK_EQ_INT(riak_get_type_props(rc, "nosuchtype", &json), RIAK_E_NOT_FOUND);

    char *key = NULL;
    CHECK_EQ_INT(riak_put(rc, "tb", NULL, "a", 1, &o, NULL, &key), RIAK_OK);
    CHECK_EQ_STR(key, "auto key");
    riak_free(key);
    CHECK_EQ_INT(riak_delete(rc, "tb", "k1", &o), RIAK_OK);
    last = last_request();
    CHECK_EQ_STR(jstr(last, "path"), "/types/t1/buckets/tb/keys/k1");
    json_object_put(last);

    riak_opts bad = {.bucket_type = ""};
    CHECK_EQ_INT(riak_get(rc, "tb", "k1", &bad, &obj), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_type_list_keys(rc, "", "tb", &l), RIAK_E_INVALID_ARG);
}

static size_t pick_longest(const riak_object *s, size_t n, void *ctx) {
    (void)ctx;
    size_t best = 0;
    for (size_t i = 1; i < n; i++) {
        if (s[i].len > s[best].len) best = i;
    }
    return best;
}

static size_t give_up(const riak_object *s, size_t n, void *ctx) {
    (void)s;
    *(size_t *)ctx = n;
    return n;
}

static void test_siblings(void) {
    riak_opts o = {.bucket_type = "multi", .content_type = "text/plain"};
    CHECK_EQ_INT(riak_put(rc, "mb", "k", "short", 5, &o, NULL, NULL), RIAK_OK);
    CHECK_EQ_INT(riak_put(rc, "mb", "k", "longer value", 12, &o, NULL, NULL), RIAK_OK);

    riak_object obj;
    CHECK_EQ_INT(riak_get(rc, "mb", "k", &o, &obj), RIAK_E_SIBLINGS);

    riak_object_set set;
    CHECK_EQ_INT(riak_get_siblings(rc, "mb", "k", &o, &set), RIAK_OK);
    CHECK_EQ_INT(set.count, 2);
    CHECK(set.vclock != NULL);
    if (set.count == 2) {
        CHECK_EQ_STR(set.items[0].data, "short");
        CHECK_EQ_STR(set.items[1].data, "longer value");
        CHECK_EQ_STR(set.items[1].content_type, "text/plain");
        CHECK_EQ_STR(set.items[1].vclock, set.vclock);
    }
    riak_object_set_free(&set);

    size_t seen = 0;
    riak_opts declined = o;
    declined.resolver = give_up;
    declined.resolver_ctx = &seen;
    CHECK_EQ_INT(riak_get(rc, "mb", "k", &declined, &obj), RIAK_E_SIBLINGS);
    CHECK_EQ_INT(seen, 2);

    riak_opts res = o;
    res.resolver = pick_longest;
    CHECK_EQ_INT(riak_get(rc, "mb", "k", &res, &obj), RIAK_OK);
    CHECK_EQ_STR(obj.data, "longer value");
    CHECK(obj.vclock != NULL);

    /* writing the resolved value back with its vclock collapses the siblings */
    riak_opts wb = o;
    wb.vclock = obj.vclock;
    CHECK_EQ_INT(riak_put(rc, "mb", "k", obj.data, obj.len, &wb, NULL, NULL), RIAK_OK);
    riak_object_free(&obj);
    CHECK_EQ_INT(riak_get_siblings(rc, "mb", "k", &o, &set), RIAK_OK);
    CHECK_EQ_INT(set.count, 1);
    riak_object_set_free(&set);
    CHECK_EQ_INT(riak_get(rc, "mb", "k", &o, &obj), RIAK_OK);
    riak_object_free(&obj);

    /* 300 without a multipart body cannot be resolved */
    CHECK_EQ_INT(riak_get(rc, "siblings", "k", &res, &obj), RIAK_E_PARSE);
    CHECK_EQ_INT(riak_get_siblings(rc, "nothing", "here", NULL, &set), RIAK_E_NOT_FOUND);
    CHECK(set.items == NULL);
}

static void test_conditional(void) {
    riak_opts o = {.content_type = "text/plain"};
    CHECK_EQ_INT(riak_put(rc, "cond", "k", "v1", 2, &o, NULL, NULL), RIAK_OK);
    riak_object obj;
    CHECK_EQ_INT(riak_get(rc, "cond", "k", NULL, &obj), RIAK_OK);

    riak_opts inm = {.if_none_match = obj.etag};
    riak_object again;
    CHECK_EQ_INT(riak_get(rc, "cond", "k", &inm, &again), RIAK_E_NOT_MODIFIED);
    CHECK(again.data == NULL);
    json_object *last = last_request();
    CHECK_EQ_STR(last_header(last, "If-None-Match"), obj.etag);
    json_object_put(last);

    riak_opts create = {.if_none_match = "*"};
    CHECK_EQ_INT(riak_put(rc, "cond", "k", "v2", 2, &create, NULL, NULL), RIAK_E_PRECONDITION);
    CHECK_EQ_INT(riak_put(rc, "cond", "fresh", "v", 1, &create, NULL, NULL), RIAK_OK);

    riak_opts match = {.if_match = "\"wrong\""};
    CHECK_EQ_INT(riak_put(rc, "cond", "k", "v2", 2, &match, NULL, NULL), RIAK_E_PRECONDITION);
    match.if_match = obj.etag;
    CHECK_EQ_INT(riak_put(rc, "cond", "k", "v2", 2, &match, NULL, NULL), RIAK_OK);

    riak_opts dates = {.if_modified_since = "Mon, 26 Oct 2026 10:00:00 GMT",
                       .if_unmodified_since = "Tue, 27 Oct 2026 10:00:00 GMT"};
    CHECK_EQ_INT(riak_get(rc, "cond", "k", &dates, &again), RIAK_OK);
    last = last_request();
    CHECK_EQ_STR(last_header(last, "If-Modified-Since"), dates.if_modified_since);
    CHECK_EQ_STR(last_header(last, "If-Unmodified-Since"), dates.if_unmodified_since);
    json_object_put(last);
    riak_object_free(&again);
    riak_object_free(&obj);

    riak_opts inj = {.if_match = "x\r\nX-Evil: 1"};
    CHECK_EQ_INT(riak_put(rc, "cond", "k", "v", 1, &inj, NULL, NULL), RIAK_E_INVALID_ARG);
}

static void test_server_timeout_and_stats(void) {
    riak_opts o = {.server_timeout_ms = 1500, .r = RIAK_Q_ONE};
    riak_object obj;
    CHECK_EQ_INT(riak_get(rc, "cond", "k", &o, &obj), RIAK_OK);
    riak_object_free(&obj);
    json_object *last = last_request();
    CHECK_EQ_STR(jstr(last, "path"), "/buckets/cond/keys/k?r=one&timeout=1500");
    json_object_put(last);

    char *json = NULL;
    CHECK_EQ_INT(riak_stats(rc, &json), RIAK_OK);
    CHECK(json && strstr(json, "vnode_gets"));
    riak_free(json);
    CHECK_EQ_INT(riak_stats(rc, NULL), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_stats(NULL, &json), RIAK_E_INVALID_ARG);
}

typedef struct {
    size_t n;
    size_t stop_at;
    char last_key[32];
    char last_term[32];
} index_seen;

static bool on_index(const char *key, const char *term, void *ctx) {
    index_seen *s = ctx;
    s->n++;
    snprintf(s->last_key, sizeof s->last_key, "%s", key);
    snprintf(s->last_term, sizeof s->last_term, "%s", term ? term : "-");
    return s->stop_at == 0 || s->n < s->stop_at;
}

static void test_secondary_indexes(void) {
    for (int i = 0; i < 5; i++) {
        char key[8], age[8];
        snprintf(key, sizeof key, "p%d", i);
        snprintf(age, sizeof age, "%d", 20 + i * 5);
        riak_kv_pair idx[] = {{"age_int", age}, {"group_bin", i % 2 ? "odd" : "even"}};
        riak_opts o = {.indexes = idx, .n_indexes = 2};
        CHECK_EQ_INT(riak_put(rc, "people", key, "x", 1, &o, NULL, NULL), RIAK_OK);
    }
    riak_index_result r;
    riak_index_query exact = {.index = "group_bin", .value = "odd"};
    CHECK_EQ_INT(riak_index_query_run(rc, "people", &exact, &r), RIAK_OK);
    CHECK(r.count == 2 && r.terms == NULL && r.keys[2] == NULL && r.continuation == NULL);
    riak_index_result_free(&r);
    json_object *last = last_request();
    CHECK_EQ_STR(jstr(last, "path"), "/buckets/people/index/group_bin/odd");
    json_object_put(last);

    /* integer range with terms, paged two at a time */
    riak_index_query range = {.index = "age_int",
                              .range_min = "21",
                              .range_max = "40",
                              .return_terms = true,
                              .max_results = 2,
                              .server_timeout_ms = 900};
    CHECK_EQ_INT(riak_index_query_run(rc, "people", &range, &r), RIAK_OK);
    CHECK_EQ_INT(r.count, 2);
    if (r.count == 2 && r.terms) {
        CHECK_EQ_STR(r.keys[0], "p1");
        CHECK_EQ_STR(r.terms[0], "25");
        CHECK(r.terms[2] == NULL);
    }
    CHECK(r.continuation != NULL);
    last = last_request();
    CHECK_EQ_STR(jstr(last, "path"),
                 "/buckets/people/index/age_int/21/40?max_results=2&return_terms=true&timeout=900");
    json_object_put(last);
    riak_index_query next = range;
    next.continuation = r.continuation;
    riak_index_result r2;
    CHECK_EQ_INT(riak_index_query_run(rc, "people", &next, &r2), RIAK_OK);
    /* ages 25..40 match p1..p4: the second page is p3, p4 and the last one */
    CHECK(r2.count == 2 && strcmp(r2.keys[0], "p3") == 0 && strcmp(r2.keys[1], "p4") == 0);
    CHECK(r2.continuation == NULL);
    riak_index_result_free(&r2);
    riak_index_result_free(&r);

    /* streaming, including the continuation and an early stop */
    index_seen seen = {0};
    char *cont = NULL;
    CHECK_EQ_INT(riak_index_query_stream(rc, "people", &range, on_index, &seen, &cont), RIAK_OK);
    CHECK_EQ_INT(seen.n, 2);
    CHECK_EQ_STR(seen.last_term, "30");
    CHECK(cont != NULL);
    riak_free(cont);
    memset(&seen, 0, sizeof seen);
    riak_index_query keys = {.index = "$key", .range_min = "p0", .range_max = "p9"};
    seen.stop_at = 3;
    CHECK_EQ_INT(riak_index_query_stream(rc, "people", &keys, on_index, &seen, NULL), RIAK_OK);
    CHECK_EQ_INT(seen.n, 3);
    CHECK_EQ_STR(seen.last_key, "p2");
    CHECK_EQ_INT(riak_ping(rc), RIAK_OK); /* the client is usable after an aborted stream */

    riak_index_query bkt = {.index = "$bucket", .value = "people"};
    CHECK_EQ_INT(riak_index_query_run(rc, "people", &bkt, &r), RIAK_OK);
    CHECK_EQ_INT(r.count, 5);
    riak_index_result_free(&r);

    riak_index_query typed = {.index = "x_bin", .value = "v", .bucket_type = "t1"};
    CHECK_EQ_INT(riak_index_query_run(rc, "people", &typed, &r), RIAK_OK);
    CHECK(r.count == 0 && r.keys != NULL && r.keys[0] == NULL);
    riak_index_result_free(&r);
    last = last_request();
    CHECK_EQ_STR(jstr(last, "path"), "/types/t1/buckets/people/index/x_bin/v");
    json_object_put(last);

    riak_index_query both = {.index = "a_bin", .value = "x", .range_min = "a", .range_max = "b"};
    riak_index_query none = {.index = "a_bin"};
    riak_index_query half = {.index = "a_bin", .range_min = "a"};
    CHECK_EQ_INT(riak_index_query_run(rc, "people", &both, &r), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_index_query_run(rc, "people", &none, &r), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_index_query_run(rc, "people", &half, &r), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_index_query_run(rc, "people", NULL, &r), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_index_query_stream(rc, "people", &exact, NULL, NULL, NULL),
                 RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_index_query_run(rc, "unavailable", &exact, &r), RIAK_E_HTTP);
    CHECK_EQ_INT(riak_index_query_stream(rc, "unavailable", &exact, on_index, &seen, &cont),
                 RIAK_E_HTTP);
    CHECK(cont == NULL);
}

static void test_search(void) {
    CHECK_EQ_INT(riak_search_index_create(rc, "users_idx", "_yz_default", 2), RIAK_OK);
    json_object *last = last_request();
    CHECK_EQ_STR(jstr(last, "method"), "PUT");
    CHECK_EQ_STR(jstr(last, "path"), "/search/index/users_idx");
    json_object_put(last);
    CHECK_EQ_INT(riak_search_index_create(rc, "plain_idx", NULL, 0), RIAK_OK);
    CHECK_EQ_INT(riak_search_index_create(rc, "bad", "x\"y", 0), RIAK_E_INVALID_ARG);

    char *json = NULL;
    CHECK_EQ_INT(riak_search_index_get(rc, "users_idx", &json), RIAK_OK);
    CHECK(json && strstr(json, "\"n_val\": 2"));
    riak_free(json);
    CHECK_EQ_INT(riak_search_index_list(rc, &json), RIAK_OK);
    CHECK(json && json[0] == '[' && strstr(json, "plain_idx"));
    riak_free(json);
    CHECK_EQ_INT(riak_search_index_get(rc, "missing", &json), RIAK_E_NOT_FOUND);

    CHECK_EQ_INT(riak_search_schema_put(rc, "mine", "<schema name=\"mine\"/>"), RIAK_OK);
    CHECK_EQ_INT(riak_search_schema_get(rc, "mine", &json), RIAK_OK);
    CHECK_EQ_STR(json, "<schema name=\"mine\"/>");
    riak_free(json);

    CHECK_EQ_INT(riak_set_bucket_props(rc, "users", "{\"props\":{\"search_index\":\"users_idx\"}}"),
                 RIAK_OK);
    riak_opts o = {.content_type = "application/json"};
    const char *docs[] = {"{\"name_s\":\"alice\",\"age_i\":30,\"tags_ss\":[\"a\",\"b\"]}",
                          "{\"name_s\":\"bob\",\"age_i\":40}"};
    CHECK_EQ_INT(riak_put(rc, "users", "u1", docs[0], strlen(docs[0]), &o, NULL, NULL), RIAK_OK);
    CHECK_EQ_INT(riak_put(rc, "users", "u2", docs[1], strlen(docs[1]), &o, NULL, NULL), RIAK_OK);

    riak_search_query q = {.q = "name_s:alice"};
    riak_search_result res;
    CHECK_EQ_INT(riak_search(rc, "users_idx", &q, &res), RIAK_OK);
    CHECK_EQ_INT(res.num_found, 1);
    if (res.count == 1) {
        CHECK_EQ_STR(res.docs[0].key, "u1");
        CHECK_EQ_STR(res.docs[0].bucket, "users");
        CHECK_EQ_STR(res.docs[0].bucket_type, "default");
        size_t tags = 0;
        for (size_t i = 0; i < res.docs[0].n_fields; i++) {
            tags += strcmp(res.docs[0].fields[i].name, "tags_ss") == 0;
        }
        CHECK_EQ_INT(tags, 2);
    }
    CHECK(res.raw && strstr(res.raw, "numFound"));
    riak_search_result_free(&res);

    riak_search_query all = {.q = "*:*",
                             .sort = "age_i desc",
                             .rows = 1,
                             .start = 0,
                             .filter = "age_i:[1 TO 99]",
                             .fields = "name_s,age_i",
                             .default_field = "name_s",
                             .op = "AND"};
    CHECK_EQ_INT(riak_search(rc, "users_idx", &all, &res), RIAK_OK);
    CHECK(res.num_found == 2 && res.count == 1 && strcmp(res.docs[0].key, "u2") == 0);
    riak_search_result_free(&res);
    last = last_request();
    CHECK_EQ_STR(jstr(last, "path"),
                 "/search/query/users_idx?wt=json&q=%2A%3A%2A&fq=age_i%3A%5B1%20TO%2099%5D"
                 "&sort=age_i%20desc&fl=name_s%2Cage_i&df=name_s&q.op=AND&rows=1");
    json_object_put(last);

    CHECK_EQ_INT(riak_search(rc, "missing", &q, &res), RIAK_E_NOT_FOUND);
    riak_search_query empty = {0};
    CHECK_EQ_INT(riak_search(rc, "users_idx", &empty, &res), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_search(rc, "users_idx", NULL, &res), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_search_index_delete(rc, "plain_idx"), RIAK_OK);
    CHECK_EQ_INT(riak_search_index_delete(rc, "plain_idx"), RIAK_E_NOT_FOUND);
    CHECK_EQ_INT(riak_search_index_delete(rc, ""), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_search_schema_put(rc, "mine", NULL), RIAK_E_INVALID_ARG);
}

static bool on_phase(unsigned phase, const char *data, void *ctx) {
    size_t *count = ctx;
    count[phase < 2 ? phase : 2]++;
    if (phase == 1) {
        CHECK_EQ_STR(data, "[2]");
    }
    return count[0] < 100;
}

static bool stop_now(unsigned phase, const char *data, void *ctx) {
    (void)phase;
    (void)data;
    (*(size_t *)ctx)++;
    return false;
}

static void test_mapreduce(void) {
    CHECK_EQ_INT(riak_put(rc, "mr", "a", "\"va\"", 4, NULL, NULL, NULL), RIAK_OK);
    CHECK_EQ_INT(riak_put(rc, "mr", "b", "\"vb\"", 4, NULL, NULL, NULL), RIAK_OK);
    const char *job = "{\"inputs\":[[\"mr\",\"a\"],[\"mr\",\"b\"]],\"query\":["
                      "{\"map\":{\"language\":\"erlang\",\"module\":\"riak_kv_mapreduce\","
                      "\"function\":\"map_object_value\"}},"
                      "{\"reduce\":{\"language\":\"erlang\",\"module\":\"riak_kv_mapreduce\","
                      "\"function\":\"reduce_count_inputs\"}}]}";
    char *result = NULL;
    CHECK_EQ_INT(riak_mapred(rc, job, &result), RIAK_OK);
    CHECK_EQ_STR(result, "[2]");
    riak_free(result);
    json_object *last = last_request();
    CHECK_EQ_STR(jstr(last, "method"), "POST");
    CHECK_EQ_STR(last_header(last, "Content-Type"), "application/json");
    json_object_put(last);

    size_t count[3] = {0, 0, 0};
    CHECK_EQ_INT(riak_mapred_stream(rc, job, on_phase, count), RIAK_OK);
    CHECK_EQ_INT(count[0], 2);
    CHECK_EQ_INT(count[1], 1);
    last = last_request();
    CHECK_EQ_STR(jstr(last, "path"), "/mapred?chunked=true");
    json_object_put(last);

    size_t calls = 0;
    CHECK_EQ_INT(riak_mapred_stream(rc, job, stop_now, &calls), RIAK_OK);
    CHECK_EQ_INT(calls, 1);

    CHECK_EQ_INT(riak_mapred(rc, "{\"inputs\":", &result), RIAK_E_HTTP);
    CHECK(result == NULL);
    CHECK_EQ_INT(riak_mapred(rc, NULL, &result), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_mapred(rc, job, NULL), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_mapred_stream(rc, job, NULL, NULL), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_mapred_stream(NULL, job, stop_now, &calls), RIAK_E_INVALID_ARG);
}

static void test_datatypes(void) {
    riak_dt_opts rb = {.return_body = true, .w = RIAK_Q_QUORUM};
    riak_datatype dt;

    riak_dt_op *c = riak_dt_op_new(RIAK_DT_COUNTER);
    riak_dt_op_increment(c, 4);
    CHECK_EQ_INT(riak_dt_update(rc, "counters", "dtb", "c", c, NULL, &rb, &dt, NULL), RIAK_OK);
    CHECK(dt.value.type == RIAK_DT_COUNTER && dt.value.counter == 4);
    riak_datatype_free(&dt);
    json_object *last = last_request();
    CHECK_EQ_STR(jstr(last, "method"), "POST");
    CHECK_EQ_STR(jstr(last, "path"),
                 "/types/counters/buckets/dtb/datatypes/c?w=quorum&returnbody=true");
    CHECK_EQ_STR(jstr(last, "body"), "{\"increment\":4}");
    CHECK_EQ_STR(last_header(last, "Content-Type"), "application/json");
    json_object_put(last);

    char *key = NULL;
    CHECK_EQ_INT(riak_dt_update(rc, "counters", "dtb", NULL, c, NULL, NULL, NULL, &key), RIAK_OK);
    CHECK(key && strncmp(key, "gen", 3) == 0);
    riak_dt_opts fo = {.r = RIAK_Q_ONE, .server_timeout_ms = 700};
    CHECK_EQ_INT(riak_dt_fetch(rc, "counters", "dtb", key, &fo, &dt), RIAK_OK);
    CHECK_EQ_INT(dt.value.counter, 4);
    riak_datatype_free(&dt);
    last = last_request();
    CHECK(strstr(jstr(last, "path"), "?r=one&timeout=700") != NULL);
    json_object_put(last);
    riak_free(key);
    riak_dt_op_free(c);
    CHECK_EQ_INT(riak_dt_fetch(rc, "counters", "dtb", "absent", NULL, &dt), RIAK_E_NOT_FOUND);

    riak_dt_op *s = riak_dt_op_new(RIAK_DT_SET);
    riak_dt_op_add(s, "a");
    riak_dt_op_add(s, "b");
    CHECK_EQ_INT(riak_dt_update(rc, "sets", "dtb", "s", s, NULL, NULL, NULL, NULL), RIAK_OK);
    riak_dt_op_free(s);
    CHECK_EQ_INT(riak_dt_fetch(rc, "sets", "dtb", "s", NULL, &dt), RIAK_OK);
    CHECK(dt.value.n_elements == 2 && dt.context != NULL);
    s = riak_dt_op_new(RIAK_DT_SET);
    riak_dt_op_remove(s, "a");
    CHECK_EQ_INT(riak_dt_update(rc, "sets", "dtb", "s", s, dt.context, NULL, NULL, NULL), RIAK_OK);
    last = last_request();
    CHECK(strstr(jstr(last, "body"), "\"context\":\"") != NULL);
    json_object_put(last);
    riak_dt_op_free(s);
    riak_datatype_free(&dt);
    s = riak_dt_op_new(RIAK_DT_SET);
    riak_dt_op_remove(s, "missing");
    CHECK_EQ_INT(riak_dt_update(rc, "sets", "dtb", "s", s, NULL, NULL, NULL, NULL),
                 RIAK_E_PRECONDITION);
    riak_dt_op_free(s);
    riak_dt_opts noctx = {.no_context = true};
    CHECK_EQ_INT(riak_dt_fetch(rc, "sets", "dtb", "s", &noctx, &dt), RIAK_OK);
    CHECK(dt.context == NULL && dt.value.n_elements == 1);
    riak_datatype_free(&dt);

    riak_dt_op *m = riak_dt_op_new(RIAK_DT_MAP);
    riak_dt_op_map_register(m, "name", "alice");
    riak_dt_op_map_flag(m, "admin", true);
    riak_dt_op_map_counter(m, "visits", 2);
    riak_dt_op_add(riak_dt_op_map_set(m, "tags"), "x");
    riak_dt_op_map_counter(riak_dt_op_map_map(m, "stats"), "logins", 1);
    CHECK_EQ_INT(riak_dt_update(rc, "maps", "dtb", "m", m, NULL, &rb, &dt, NULL), RIAK_OK);
    riak_dt_op_free(m);
    CHECK_EQ_INT(dt.value.n_entries, 5);
    const riak_dt_value *stats = riak_dt_map_get(&dt.value, "stats", RIAK_DT_MAP);
    CHECK(stats && riak_dt_map_get(stats, "logins", RIAK_DT_COUNTER)->counter == 1);
    m = riak_dt_op_new(RIAK_DT_MAP);
    riak_dt_op_map_remove(m, "admin", RIAK_DT_FLAG);
    riak_datatype after;
    CHECK_EQ_INT(riak_dt_update(rc, "maps", "dtb", "m", m, dt.context, &rb, &after, NULL), RIAK_OK);
    CHECK(after.value.n_entries == 4 &&
          riak_dt_map_get(&after.value, "admin", RIAK_DT_FLAG) == NULL);
    riak_datatype_free(&after);
    riak_datatype_free(&dt);
    riak_dt_op_free(m);

    riak_dt_op *h = riak_dt_op_new(RIAK_DT_HLL);
    riak_dt_op_add(h, "a");
    riak_dt_op_add(h, "a");
    riak_dt_op_add(h, "b");
    CHECK_EQ_INT(riak_dt_update(rc, "hlls", "dtb", "h", h, NULL, &rb, &dt, NULL), RIAK_OK);
    CHECK(dt.value.type == RIAK_DT_HLL && dt.value.counter == 2);
    riak_datatype_free(&dt);
    riak_dt_op_free(h);

    riak_dt_op *g = riak_dt_op_new(RIAK_DT_GSET);
    riak_dt_op_add(g, "z");
    CHECK_EQ_INT(riak_dt_update(rc, "gsets", "dtb", "g", g, NULL, &rb, &dt, NULL), RIAK_OK);
    CHECK(dt.value.type == RIAK_DT_GSET && dt.value.n_elements == 1);
    riak_datatype_free(&dt);

    /* errors */
    CHECK_EQ_INT(riak_dt_update(rc, "counters", "dtb", "c", g, NULL, NULL, NULL, NULL),
                 RIAK_E_HTTP);
    CHECK_EQ_INT(riak_client_http_status(rc), 400);
    riak_dt_op_free(g);
    riak_dt_op *bad = riak_dt_op_new(RIAK_DT_SET);
    CHECK_EQ_INT(riak_dt_update(rc, "sets", "dtb", "s", bad, NULL, NULL, NULL, NULL),
                 RIAK_E_INVALID_ARG); /* empty operation, nothing sent */
    riak_dt_op_add(bad, "x");
    CHECK_EQ_INT(riak_dt_update(rc, "", "dtb", "s", bad, NULL, NULL, NULL, NULL),
                 RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_dt_update(rc, "sets", "dtb", "s", NULL, NULL, NULL, NULL, NULL),
                 RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_dt_update(rc, "default", "dtb", "s", bad, NULL, NULL, NULL, NULL),
                 RIAK_E_NOT_FOUND);
    riak_dt_op_free(bad);
    CHECK_EQ_INT(riak_dt_fetch(rc, "sets", "dtb", NULL, NULL, &dt), RIAK_E_INVALID_ARG);
    CHECK_EQ_INT(riak_dt_fetch(rc, "sets", "dtb", "s", NULL, NULL), RIAK_E_INVALID_ARG);
    riak_dt_opts badq = {.r = -9};
    CHECK_EQ_INT(riak_dt_fetch(rc, "sets", "dtb", "s", &badq, &dt), RIAK_E_INVALID_ARG);
}

static riak_client *cluster(const riak_node *nodes, size_t n, riak_balance balance,
                            unsigned retries) {
    riak_config cfg;
    riak_config_init(&cfg);
    cfg.transport = RIAK_TRANSPORT_HTTP;
    cfg.nodes = nodes;
    cfg.n_nodes = n;
    cfg.balance = balance;
    cfg.max_retries = retries;
    cfg.retry_base_ms = 10;
    cfg.fail_threshold = 2;
    cfg.eject_ms = 300;
    cfg.connect_timeout_ms = 2000;
    riak_client *c = NULL;
    CHECK_EQ_INT(riak_client_new(&cfg, &c), RIAK_OK);
    return c;
}

static double elapsed_ms(const struct timespec *t0) {
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (double)(t1.tv_sec - t0->tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0->tv_nsec) / 1e6;
}

static void sleep_ms(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static void test_cluster_round_robin(void) {
    riak_node nodes[3] = {
        {"127.0.0.1", ports[0], 0}, {"127.0.0.1", ports[1], 0}, {"127.0.0.1", ports[2], 0}};
    for (int i = 0; i < 3; i++)
        hits(i);
    riak_client *c = cluster(nodes, 3, RIAK_BALANCE_ROUND_ROBIN, 0);
    CHECK_EQ_INT(riak_client_node_count(c), 3);
    for (int i = 0; i < 6; i++)
        CHECK_EQ_INT(riak_ping(c), RIAK_OK);
    for (int i = 0; i < 3; i++)
        CHECK_EQ_INT(hits(i), 2);

    /* writes through one node are visible through another (shared store) */
    CHECK_EQ_INT(riak_put(c, "cl", "k", "v", 1, NULL, NULL, NULL), RIAK_OK);
    riak_object obj;
    CHECK_EQ_INT(riak_get(c, "cl", "k", NULL, &obj), RIAK_OK);
    riak_object_free(&obj);
    riak_node_info info;
    CHECK_EQ_INT(riak_client_node_info(c, 2, &info), RIAK_OK);
    CHECK(info.port == ports[2] && info.healthy && info.requests == 2);
    CHECK_EQ_INT(riak_client_node_info(c, 3, &info), RIAK_E_INVALID_ARG);
    riak_client_free(c);
}

static void test_cluster_failover(void) {
    /* port 1: nothing listens, connections are refused */
    riak_node nodes[2] = {{"127.0.0.1", 1, 0}, {"127.0.0.1", ports[0], 0}};
    hits(0);
    riak_client *c = cluster(nodes, 2, RIAK_BALANCE_FAILOVER, 1);
    CHECK_EQ_INT(riak_ping(c), RIAK_OK); /* refused, then retried on the next node */
    CHECK_EQ_INT(riak_ping(c), RIAK_OK);
    riak_node_info dead;
    CHECK_EQ_INT(riak_client_node_info(c, 0, &dead), RIAK_OK);
    CHECK(!dead.healthy && dead.consecutive_failures == 2); /* threshold reached: ejected */
    unsigned long long before = dead.requests;
    CHECK_EQ_INT(riak_ping(c), RIAK_OK);
    CHECK_EQ_INT(riak_client_node_info(c, 0, &dead), RIAK_OK);
    CHECK_EQ_INT(dead.requests, before); /* skipped while ejected */
    sleep_ms(350);                       /* eject_ms elapsed: tried again */
    CHECK_EQ_INT(riak_ping(c), RIAK_OK);
    CHECK_EQ_INT(riak_client_node_info(c, 0, &dead), RIAK_OK);
    CHECK_EQ_INT(dead.requests, before + 1);
    CHECK_EQ_INT(hits(0), 4);

    /* a POST that never reached a node is safe to retry */
    riak_dt_op *inc = riak_dt_op_new(RIAK_DT_COUNTER);
    riak_dt_op_increment(inc, 1);
    sleep_ms(350);
    CHECK_EQ_INT(riak_dt_update(c, "counters", "cl", "once", inc, NULL, NULL, NULL, NULL), RIAK_OK);
    riak_datatype dt;
    CHECK_EQ_INT(riak_dt_fetch(c, "counters", "cl", "once", NULL, &dt), RIAK_OK);
    CHECK_EQ_INT(dt.value.counter, 1);
    riak_datatype_free(&dt);
    riak_client_free(c);

    /* 503 on a GET is retried elsewhere; on a POST it is not */
    riak_node two[2] = {{"127.0.0.1", ports[1], 0}, {"127.0.0.1", ports[2], 0}};
    c = cluster(two, 2, RIAK_BALANCE_FAILOVER, 2);
    set_mode(1, "503");
    hits(1);
    hits(2);
    riak_object obj;
    CHECK_EQ_INT(riak_get(c, "cl", "k", NULL, &obj), RIAK_OK);
    riak_object_free(&obj);
    CHECK_EQ_INT(hits(1), 1);
    CHECK_EQ_INT(hits(2), 1);
    CHECK_EQ_INT(riak_dt_update(c, "counters", "cl", "once", inc, NULL, NULL, NULL, NULL),
                 RIAK_E_HTTP);
    CHECK_EQ_INT(riak_client_http_status(c), 503);
    CHECK_EQ_INT(hits(2), 0); /* the increment was not repeated on another node */
    CHECK(strstr(riak_client_errmsg(c), "127.0.0.1:") != NULL);

    /* connections closed without a reply count as transport errors */
    set_mode(1, "close");
    CHECK_EQ_INT(riak_ping(c), RIAK_OK);
    CHECK_EQ_INT(riak_client_check_nodes(c), 1);
    set_mode(1, "ok");
    CHECK_EQ_INT(riak_client_check_nodes(c), 2);
    riak_node_info info;
    CHECK_EQ_INT(riak_client_node_info(c, 0, &info), RIAK_OK);
    CHECK(info.healthy && info.consecutive_failures == 0 && info.failures >= 2);
    riak_dt_op_free(inc);
    riak_client_free(c);
}

static void test_cluster_backoff_and_limits(void) {
    riak_node dead[2] = {{"127.0.0.1", 1, 0}, {"127.0.0.1", 1, 0}};
    riak_client *c = cluster(dead, 2, RIAK_BALANCE_ROUND_ROBIN, 2);
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CHECK_EQ_INT(riak_ping(c), RIAK_E_TRANSPORT);
    /* backoffs of 10 and 20 ms, at least half of each */
    CHECK(elapsed_ms(&t0) >= 14.0);
    riak_client_free(c);

    riak_node one[1] = {{"127.0.0.1", 1, 0}};
    c = cluster(one, 1, RIAK_BALANCE_ROUND_ROBIN, 0);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CHECK_EQ_INT(riak_ping(c), RIAK_E_TRANSPORT);
    riak_node_info info;
    CHECK_EQ_INT(riak_client_node_info(c, 0, &info), RIAK_OK);
    CHECK_EQ_INT(info.requests, 1); /* max_retries = 0: a single attempt */
    riak_client_free(c);

    riak_config cfg;
    riak_config_init(&cfg);
    cfg.transport = RIAK_TRANSPORT_HTTP;
    riak_client *bad = NULL;
    riak_node badhost[1] = {{"no/slash", 0, 0}};
    cfg.nodes = badhost;
    cfg.n_nodes = 1;
    CHECK_EQ_INT(riak_client_new(&cfg, &bad), RIAK_E_INVALID_ARG);
    cfg.nodes = NULL;
    CHECK_EQ_INT(riak_client_new(&cfg, &bad), RIAK_E_INVALID_ARG);
    cfg.n_nodes = 0;
    cfg.balance = (riak_balance)7;
    CHECK_EQ_INT(riak_client_new(&cfg, &bad), RIAK_E_INVALID_ARG);
    cfg.balance = RIAK_BALANCE_ROUND_ROBIN;
    cfg.retry_base_ms = -1;
    CHECK_EQ_INT(riak_client_new(&cfg, &bad), RIAK_E_INVALID_ARG);
    CHECK(bad == NULL);
    CHECK_EQ_INT(riak_client_node_count(NULL), 0);
    CHECK_EQ_INT(riak_client_check_nodes(NULL), 0);
}

typedef struct {
    riak_request_metrics m[8];
    char paths[8][64];
    char hosts[8][32];
    int n;
} metrics_log;

static void on_metrics(const riak_request_metrics *m, void *ctx) {
    metrics_log *l = ctx;
    if (l->n < 8) {
        l->m[l->n] = *m;
        snprintf(l->paths[l->n], sizeof l->paths[0], "%s", m->path);
        snprintf(l->hosts[l->n], sizeof l->hosts[0], "%s", m->host);
    }
    l->n++;
}

typedef struct {
    int counts[4];
    char last_warn[256];
} log_sink;

static void on_log(riak_log_level level, const char *msg, void *ctx) {
    log_sink *l = ctx;
    l->counts[level]++;
    if (level == RIAK_LOG_WARN) snprintf(l->last_warn, sizeof l->last_warn, "%s", msg);
}

static void test_observability(void) {
    riak_node nodes[2] = {{"127.0.0.1", 1, 0}, {"127.0.0.1", ports[0], 0}};
    metrics_log ml = {0};
    log_sink ls;
    memset(&ls, 0, sizeof ls);
    riak_config cfg;
    riak_config_init(&cfg);
    cfg.transport = RIAK_TRANSPORT_HTTP;
    cfg.nodes = nodes;
    cfg.n_nodes = 2;
    cfg.balance = RIAK_BALANCE_FAILOVER;
    cfg.max_retries = 1;
    cfg.retry_base_ms = 5;
    cfg.metrics_fn = on_metrics;
    cfg.metrics_ctx = &ml;
    cfg.log_fn = on_log;
    cfg.log_ctx = &ls; /* default level: WARN and above */
    riak_client *c = NULL;
    CHECK_EQ_INT(riak_client_new(&cfg, &c), RIAK_OK);
    CHECK_EQ_INT(riak_ping(c), RIAK_OK);
    CHECK_EQ_INT(ml.n, 2);
    if (ml.n == 2) {
        CHECK(ml.m[0].status == RIAK_E_TRANSPORT && ml.m[0].will_retry && ml.m[0].port == 1);
        CHECK(ml.m[0].http_status == 0 && ml.m[0].attempt == 0);
        CHECK(ml.m[1].status == RIAK_OK && ml.m[1].http_status == 200 && !ml.m[1].will_retry);
        CHECK(ml.m[1].attempt == 1 && ml.m[1].port == ports[0] && ml.m[1].bytes_received == 2);
        CHECK_EQ_STR(ml.paths[1], "/ping");
        CHECK_EQ_STR(ml.hosts[1], "127.0.0.1");
        CHECK_EQ_STR(ml.m[1].method, "GET");
        CHECK(ml.m[1].duration_ms >= 0.0);
    }
    CHECK_EQ_INT(ls.counts[RIAK_LOG_DEBUG], 0);
    CHECK_EQ_INT(ls.counts[RIAK_LOG_WARN], 1);
    CHECK(strstr(ls.last_warn, "retrying") != NULL && strstr(ls.last_warn, "127.0.0.1:1") != NULL);
    riak_client_free(c);

    /* DEBUG shows every attempt; ERROR when the last retry fails */
    memset(&ls, 0, sizeof ls);
    cfg.log_level = RIAK_LOG_DEBUG;
    cfg.metrics_fn = NULL;
    riak_node dead[1] = {{"127.0.0.1", 1, 0}};
    cfg.nodes = dead;
    cfg.n_nodes = 1;
    CHECK_EQ_INT(riak_client_new(&cfg, &c), RIAK_OK);
    CHECK_EQ_INT(riak_ping(c), RIAK_E_TRANSPORT);
    CHECK_EQ_INT(ls.counts[RIAK_LOG_DEBUG], 2);
    CHECK_EQ_INT(ls.counts[RIAK_LOG_ERROR], 1);
    riak_client_free(c);
    cfg.log_level = (riak_log_level)9;
    CHECK_EQ_INT(riak_client_new(&cfg, &c), RIAK_E_INVALID_ARG);
}

static void test_client_id_header(void) {
    riak_config cfg;
    riak_client *c = NULL;
    riak_config_init(&cfg);
    cfg.transport = RIAK_TRANSPORT_HTTP;
    cfg.port = port;
    cfg.client_id = "client-7";
    CHECK_EQ_INT(riak_client_new(&cfg, &c), RIAK_OK);
    CHECK_EQ_INT(riak_ping(c), RIAK_OK);
    json_object *last = last_request();
    CHECK_EQ_STR(last_header(last, "X-Riak-ClientId"), "client-7");
    json_object_put(last);
    riak_client_free(c);
}

static bool count_name(const char *name, size_t len, void *ctx) {
    size_t *n = ctx;
    (*n)++;
    return len > 0 && name[len] == '\0' && *n < 5000;
}

static bool reenter_name(const char *name, size_t len, void *ctx) {
    size_t *n = ctx;
    (void)name;
    (void)len;
    if ((*n)++ == 0) {
        /* refused: the request in progress owns the client's connection */
        CHECK_EQ_INT(riak_ping(rc), RIAK_E_INVALID_ARG);
        CHECK(strstr(riak_client_errmsg(rc), "busy") != NULL);
    }
    return true;
}

static bool stop_after_three(const char *name, size_t len, void *ctx) {
    (void)name;
    (void)len;
    return ++*(size_t *)ctx < 3;
}

static void test_info_preflist_streams(void) {
    riak_server_info info;
    CHECK_EQ_INT(riak_get_server_info(rc, &info), RIAK_OK);
    CHECK_EQ_STR(info.node, "riak@stub");
    CHECK_EQ_STR(info.version, "2.2.3");
    riak_server_info_free(&info);
    CHECK_EQ_INT(riak_get_server_info(rc, NULL), RIAK_E_INVALID_ARG);

    riak_preflist pl;
    CHECK_EQ_INT(riak_get_preflist(rc, NULL, "b", "k", &pl), RIAK_OK);
    CHECK_EQ_INT(pl.count, 2);
    CHECK_EQ_STR(pl.items[1].node, "riak@b");
    CHECK(pl.items[0].primary && !pl.items[1].primary);
    riak_preflist_free(&pl);
    json_object *last = last_request();
    CHECK_EQ_STR(jstr(last, "path"), "/types/default/buckets/b/keys/k/preflist");
    json_object_put(last);
    CHECK_EQ_INT(riak_get_preflist(rc, NULL, "", "k", &pl), RIAK_E_INVALID_ARG);

    size_t n = 0;
    CHECK_EQ_INT(riak_list_keys_stream(rc, NULL, "many", count_name, &n), RIAK_OK);
    CHECK_EQ_INT(n, 1000);
    n = 0;
    CHECK_EQ_INT(riak_list_keys_stream(rc, NULL, "many", stop_after_three, &n), RIAK_OK);
    CHECK_EQ_INT(n, 3);
    n = 0;
    CHECK_EQ_INT(riak_list_keys_stream(rc, NULL, "many", reenter_name, &n), RIAK_OK);
    CHECK_EQ_INT(n, 1000);
    CHECK_EQ_INT(riak_ping(rc), RIAK_OK);
    CHECK_EQ_INT(riak_list_keys_stream(rc, NULL, "truncated", count_name, &n), RIAK_E_PARSE);
    CHECK_EQ_INT(riak_list_keys_stream(rc, NULL, "unavailable", count_name, &n), RIAK_E_HTTP);
    CHECK_EQ_INT(riak_list_keys_stream(rc, NULL, "b", NULL, &n), RIAK_E_INVALID_ARG);
    n = 0;
    CHECK_EQ_INT(riak_list_buckets_stream(rc, NULL, count_name, &n), RIAK_OK);
    CHECK(n >= 1);
    CHECK_EQ_INT(riak_list_buckets_stream(rc, "", count_name, &n), RIAK_E_INVALID_ARG);

    riak_coverage_plan plan;
    CHECK_EQ_INT(riak_get_coverage(rc, NULL, "b", 0, &plan), RIAK_E_UNSUPPORTED);
    CHECK_EQ_INT(riak_get_coverage(rc, NULL, "b", 0, NULL), RIAK_E_INVALID_ARG);
    riak_coverage_free(&plan);
}

static void test_read_flags_and_head(void) {
    riak_opts put = {.content_type = "text/plain"};
    CHECK_EQ_INT(riak_put(rc, "flags", "k", "value", 5, &put, NULL, NULL), RIAK_OK);
    riak_object obj;
    riak_opts flags = {.basic_quorum = true, .notfound_ok = true};
    CHECK_EQ_INT(riak_get(rc, "flags", "k", &flags, &obj), RIAK_OK);
    riak_object_free(&obj);
    json_object *last = last_request();
    const char *path = jstr(last, "path");
    CHECK(path && strstr(path, "basic_quorum=true") && strstr(path, "notfound_ok=true"));
    json_object_put(last);

    riak_opts head = {.head = true};
    CHECK_EQ_INT(riak_get(rc, "flags", "k", &head, &obj), RIAK_OK);
    CHECK_EQ_INT(obj.len, 0);
    CHECK_EQ_STR(obj.content_type, "text/plain");
    riak_object_free(&obj);

    riak_opts pb_only[] = {
        {.sloppy_quorum = true}, {.n_val = 3}, {.if_modified = true}, {.if_not_modified = true}};
    for (size_t i = 0; i < 4; i++) {
        CHECK_EQ_INT(riak_get(rc, "flags", "k", &pb_only[i], &obj), RIAK_E_UNSUPPORTED);
        CHECK_EQ_INT(riak_put(rc, "flags", "k", "v", 1, &pb_only[i], NULL, NULL),
                     RIAK_E_UNSUPPORTED);
        CHECK_EQ_INT(riak_delete(rc, "flags", "k", &pb_only[i]), RIAK_E_UNSUPPORTED);
    }
    riak_index_query q = {.index = "$bucket", .value = "flags", .cover_context = "AAAA"};
    riak_index_result r;
    CHECK_EQ_INT(riak_index_query_run(rc, "flags", &q, &r), RIAK_E_UNSUPPORTED);
    CHECK_EQ_INT(riak_delete(rc, "flags", "k", NULL), RIAK_OK);
}

int main(void) {
    const char *p = getenv("RIAK_TEST_PORT");
    if (p == NULL) {
        fprintf(stderr, "run through stub_riak.py (RIAK_TEST_PORT not set)\n");
        return 77; /* CTest "skipped" */
    }
    port = (unsigned)strtoul(p, NULL, 10);
    const char *ps = getenv("RIAK_TEST_PORTS");
    if (ps == NULL || sscanf(ps, "%u,%u,%u", &ports[0], &ports[1], &ports[2]) != 3) {
        fprintf(stderr, "RIAK_TEST_PORTS must list three stub nodes\n");
        return 1;
    }

    CHECK_EQ_INT(riak_global_init(), RIAK_OK);
    riak_config cfg;
    riak_config_init(&cfg);
    cfg.transport = RIAK_TRANSPORT_HTTP;
    cfg.port = port;
    CHECK_EQ_INT(riak_client_new(&cfg, &rc), RIAK_OK);
    if (rc == NULL) {
        return 1;
    }

    RUN(test_ping);
    RUN(test_put_get_roundtrip_with_escaping);
    RUN(test_get_after_delete_is_get);
    RUN(test_put_return_body);
    RUN(test_autokey);
    RUN(test_status_mapping);
    RUN(test_list_keys_stream);
    RUN(test_list_buckets);
    RUN(test_bucket_props);
    RUN(test_header_injection_rejected);
    RUN(test_invalid_args);
    RUN(test_transport_error);
    RUN(test_server_errors);
    RUN(test_body_limit);
    RUN(test_bucket_error_paths);
    RUN(test_null_arguments);
    RUN(test_bucket_types);
    RUN(test_siblings);
    RUN(test_conditional);
    RUN(test_server_timeout_and_stats);
    RUN(test_secondary_indexes);
    RUN(test_search);
    RUN(test_mapreduce);
    RUN(test_datatypes);
    RUN(test_cluster_round_robin);
    RUN(test_cluster_failover);
    RUN(test_cluster_backoff_and_limits);
    RUN(test_observability);
    RUN(test_client_id_header);
    RUN(test_info_preflist_streams);
    RUN(test_read_flags_and_head);

    riak_client_free(rc);
    riak_global_cleanup();
    return CHECK_DONE();
}
