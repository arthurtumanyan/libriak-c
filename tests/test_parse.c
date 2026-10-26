/*
 File:   test_parse.c
 SPDX-License-Identifier: GPL-3.0-or-later
 */

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

int main(void) {
    RUN(test_array);
    RUN(test_empty_array);
    RUN(test_bad_input);
    RUN(test_stream_split_everywhere);
    RUN(test_stream_truncated);
    RUN(test_stream_garbage);
    RUN(test_stream_empty);
    return CHECK_DONE();
}
