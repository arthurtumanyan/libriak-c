/*
 File:   test_parse.c
 SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "parse.h"

static riak_status parse(const char *json, riak_strlist *out) {
    return riak_parse_string_array(json, strlen(json), "buckets", out);
}

static void test_array(void) {
    riak_strlist l;
    CHECK_EQ_INT(parse("{\"buckets\":[\"a\",\"bb\",\"\\u0561\"]}", &l), RIAK_OK);
    CHECK_EQ_INT(l.count, 3);
    CHECK_EQ_STR(l.items[0], "a");
    CHECK_EQ_STR(l.items[1], "bb");
    CHECK_EQ_STR(l.items[2], "\xD5\xA1");
    CHECK(l.items[3] == NULL);
    riak_strlist_free(&l);
}

static void test_empty_array(void) {
    riak_strlist l;
    CHECK_EQ_INT(parse("{\"buckets\":[]}", &l), RIAK_OK);
    CHECK_EQ_INT(l.count, 0);
    CHECK(l.items != NULL && l.items[0] == NULL);
    riak_strlist_free(&l);
}

static void test_bad_input(void) {
    riak_strlist l;
    CHECK_EQ_INT(parse("{\"buckets\":\"nope\"}", &l), RIAK_E_PARSE);
    CHECK(l.items == NULL);
    CHECK_EQ_INT(parse("{\"other\":[]}", &l), RIAK_E_PARSE);
    CHECK_EQ_INT(parse("{\"buckets\":[1,2]}", &l), RIAK_E_PARSE);
    CHECK_EQ_INT(parse("{\"buckets\":[\"a\"", &l), RIAK_E_PARSE);
    CHECK_EQ_INT(parse("", &l), RIAK_E_PARSE);
    CHECK_EQ_INT(parse("[]", &l), RIAK_E_PARSE);
}

static void test_stream_split_everywhere(void) {
    const char *body = "{\"keys\":[]}{\"keys\":[\"k1\",\"k2\"]}\r\n"
                       "{\"keys\":[\"k3\"]} {\"keys\":[]}";
    size_t n = strlen(body);
    /* feed in every possible chunk size, so objects split at each byte */
    for (size_t step = 1; step <= n; step++) {
        riak_keystream *ks = riak_keystream_new();
        CHECK(ks != NULL);
        for (size_t off = 0; off < n; off += step) {
            size_t len = n - off < step ? n - off : step;
            CHECK_EQ_INT(riak_keystream_feed(ks, body + off, len), RIAK_OK);
        }
        riak_strlist l;
        CHECK_EQ_INT(riak_keystream_finish(ks, &l), RIAK_OK);
        CHECK_EQ_INT(l.count, 3);
        if (l.count == 3) {
            CHECK_EQ_STR(l.items[0], "k1");
            CHECK_EQ_STR(l.items[2], "k3");
            CHECK(l.items[3] == NULL);
        }
        riak_strlist_free(&l);
        riak_keystream_free(ks);
    }
}

static void test_stream_truncated(void) {
    riak_keystream *ks = riak_keystream_new();
    CHECK_EQ_INT(riak_keystream_feed(ks, "{\"keys\":[\"a\"]}{\"keys\":[", 23), RIAK_OK);
    riak_strlist l;
    CHECK_EQ_INT(riak_keystream_finish(ks, &l), RIAK_E_PARSE);
    riak_keystream_free(ks);
}

static void test_stream_garbage(void) {
    riak_keystream *ks = riak_keystream_new();
    CHECK_EQ_INT(riak_keystream_feed(ks, "{\"keys\":[\"a\"]}xyz", 17), RIAK_E_PARSE);
    riak_keystream_free(ks);

    ks = riak_keystream_new();
    CHECK_EQ_INT(riak_keystream_feed(ks, "{\"error\":\"timeout\"}", 19), RIAK_E_PARSE);
    riak_keystream_free(ks);
}

static void test_stream_empty(void) {
    riak_keystream *ks = riak_keystream_new();
    riak_strlist l;
    CHECK_EQ_INT(riak_keystream_finish(ks, &l), RIAK_OK);
    CHECK_EQ_INT(l.count, 0);
    CHECK(l.items != NULL && l.items[0] == NULL);
    riak_strlist_free(&l);
    riak_keystream_free(ks);
}

typedef struct {
    char keys[8][16];
    char terms[8][16];
    size_t n;
    size_t stop_at;
} visited;

static bool visit(const char *key, const char *term, void *ctx) {
    visited *v = ctx;
    if (v->n < 8) {
        snprintf(v->keys[v->n], sizeof v->keys[0], "%s", key);
        snprintf(v->terms[v->n], sizeof v->terms[0], "%s", term ? term : "-");
    }
    v->n++;
    return v->stop_at == 0 || v->n < v->stop_at;
}

static riak_status index_json(const char *json, visited *v, char **cont, bool *stopped) {
    return riak_parse_index_json(json, strlen(json), visit, v, cont, stopped);
}

static void test_index_json(void) {
    visited v = {0};
    char *cont = NULL;
    bool stopped;
    CHECK_EQ_INT(index_json("{\"keys\":[\"b\",\"a\"]}", &v, &cont, &stopped), RIAK_OK);
    CHECK(v.n == 2 && strcmp(v.keys[0], "b") == 0 && strcmp(v.terms[0], "-") == 0);
    CHECK(cont == NULL && !stopped);

    memset(&v, 0, sizeof v);
    CHECK_EQ_INT(index_json("{\"results\":[{\"na\":\"a\"},{\"nb\":\"b\"}],"
                            "\"continuation\":\"g2gC\"}",
                            &v, &cont, &stopped),
                 RIAK_OK);
    CHECK(v.n == 2 && strcmp(v.terms[1], "nb") == 0 && strcmp(v.keys[1], "b") == 0);
    CHECK_EQ_STR(cont, "g2gC");

    /* a page without continuation keeps the old value; a new one replaces it */
    CHECK_EQ_INT(index_json("{\"continuation\":\"next\"}", &v, &cont, &stopped), RIAK_OK);
    CHECK_EQ_STR(cont, "next");
    free(cont);
    cont = NULL;

    memset(&v, 0, sizeof v);
    v.stop_at = 1;
    CHECK_EQ_INT(index_json("{\"keys\":[\"x\",\"y\"]}", &v, &cont, &stopped), RIAK_OK);
    CHECK(stopped && v.n == 1);

    const char *bad[] = {"[]",
                         "{\"keys\":\"x\"}",
                         "{\"keys\":[1]}",
                         "{\"results\":[{\"a\":\"b\",\"c\":\"d\"}]}",
                         "{\"results\":[{\"a\":1}]}",
                         "{\"continuation\":3}",
                         "{\"keys\":["};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        memset(&v, 0, sizeof v);
        CHECK_EQ_INT(index_json(bad[i], &v, &cont, &stopped), RIAK_E_PARSE);
    }
    CHECK(cont == NULL);
}

static void test_search_json(void) {
    const char *json =
        "{\"responseHeader\":{\"status\":0},\"response\":{\"numFound\":7,\"start\":0,"
        "\"maxScore\":1.5,\"docs\":[{\"age_i\":30,\"name_s\":\"alice\",\"tags_ss\":[\"a\",\"b\"],"
        "\"_yz_rk\":\"u1\",\"_yz_rt\":\"default\",\"_yz_rb\":\"users\",\"score\":1.5,"
        "\"none\":null}]}}";
    riak_search_result r = {0};
    CHECK_EQ_INT(riak_parse_search(json, strlen(json), &r), RIAK_OK);
    CHECK_EQ_INT(r.num_found, 7);
    CHECK(r.max_score > 1.4 && r.max_score < 1.6);
    CHECK_EQ_INT(r.count, 1);
    if (r.count == 1) {
        riak_search_doc *d = &r.docs[0];
        CHECK_EQ_STR(d->key, "u1");
        CHECK_EQ_STR(d->bucket, "users");
        CHECK_EQ_STR(d->bucket_type, "default");
        CHECK(d->score > 1.4);
        size_t tags = 0;
        for (size_t i = 0; i < d->n_fields; i++) {
            if (strcmp(d->fields[i].name, "tags_ss") == 0) tags++;
            if (strcmp(d->fields[i].name, "age_i") == 0) CHECK_EQ_STR(d->fields[i].value, "30");
            CHECK(strcmp(d->fields[i].name, "none") != 0 &&
                  strcmp(d->fields[i].name, "score") != 0);
        }
        CHECK_EQ_INT(tags, 2);
    }
    riak_search_result_free(&r);

    const char *bad[] = {"{}", "{\"response\":[]}", "{\"response\":{\"docs\":{}}}",
                         "{\"response\":{\"docs\":[1]}}", "nope"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        memset(&r, 0, sizeof r);
        CHECK_EQ_INT(riak_parse_search(bad[i], strlen(bad[i]), &r), RIAK_E_PARSE);
        riak_search_result_free(&r);
    }
    riak_search_result_free(NULL);
}

static void test_mapred_chunk(void) {
    unsigned phase = 9;
    char *data = NULL;
    const char *ok = "{\"phase\":1,\"data\":[2, {\"a\": \"b\"}]}";
    CHECK_EQ_INT(riak_parse_mapred_chunk(ok, strlen(ok), &phase, &data), RIAK_OK);
    CHECK_EQ_INT(phase, 1);
    CHECK_EQ_STR(data, "[2,{\"a\":\"b\"}]");
    free(data);
    const char *bad[] = {"{\"data\":[]}",
                         "{\"phase\":-1,\"data\":[]}",
                         "{\"phase\":\"0\",\"data\":[]}",
                         "{\"phase\":0}",
                         "[]",
                         ""};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK_EQ_INT(riak_parse_mapred_chunk(bad[i], strlen(bad[i]), &phase, &data), RIAK_E_PARSE);
        CHECK(data == NULL);
    }
}

static void test_server_info_json(void) {
    char *node = NULL, *version = NULL;
    const char *ok = "{\"nodename\":\"riak@n1\",\"riak_kv_version\":\"3.2.0\",\"x\":1}";
    CHECK_EQ_INT(riak_parse_server_info(ok, strlen(ok), &node, &version), RIAK_OK);
    CHECK_EQ_STR(node, "riak@n1");
    CHECK_EQ_STR(version, "3.2.0");
    free(node);
    free(version);
    node = version = NULL;
    const char *bad[] = {"{}", "[]", "{\"nodename\":1}", "{\"nodename\":\"n\"}", "{"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(riak_parse_server_info(bad[i], strlen(bad[i]), &node, &version) != RIAK_OK);
        free(node);
        free(version);
        node = version = NULL;
    }
}

int main(void) {
    RUN(test_array);
    RUN(test_empty_array);
    RUN(test_bad_input);
    RUN(test_stream_split_everywhere);
    RUN(test_stream_truncated);
    RUN(test_stream_garbage);
    RUN(test_stream_empty);
    RUN(test_index_json);
    RUN(test_search_json);
    RUN(test_mapred_chunk);
    RUN(test_server_info_json);
    return CHECK_DONE();
}
