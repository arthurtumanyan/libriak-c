/*
 File:   test_client.c
 SPDX-License-Identifier: GPL-3.0-or-later

 End-to-end tests of the public API against tests/stub_riak.py, which sets
 RIAK_TEST_PORT. Only the public header is used for the library itself.
 */

#include <stdlib.h>
#include <string.h>

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
    CHECK_EQ_STR(obj.etag, "\"etag-1\"");
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

static void test_client_id_header(void) {
    riak_config cfg;
    riak_client *c = NULL;
    riak_config_init(&cfg);
    cfg.port = port;
    cfg.client_id = "client-7";
    CHECK_EQ_INT(riak_client_new(&cfg, &c), RIAK_OK);
    CHECK_EQ_INT(riak_ping(c), RIAK_OK);
    json_object *last = last_request();
    CHECK_EQ_STR(last_header(last, "X-Riak-ClientId"), "client-7");
    json_object_put(last);
    riak_client_free(c);
}

int main(void) {
    const char *p = getenv("RIAK_TEST_PORT");
    if (p == NULL) {
        fprintf(stderr, "run through stub_riak.py (RIAK_TEST_PORT not set)\n");
        return 77; /* CTest "skipped" */
    }
    port = (unsigned)strtoul(p, NULL, 10);

    CHECK_EQ_INT(riak_global_init(), RIAK_OK);
    riak_config cfg;
    riak_config_init(&cfg);
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
    RUN(test_client_id_header);

    riak_client_free(rc);
    riak_global_cleanup();
    return CHECK_DONE();
}
