/*
 File:   test_util.c
 SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdlib.h>

#include "check.h"
#include "util.h"

static void test_buf_growth(void) {
    riak_buf b = {0};
    for (int i = 0; i < 1000; i++) {
        CHECK_EQ_INT(riak_buf_append(&b, "abc", 3), 0);
    }
    CHECK_EQ_INT(b.len, 3000);
    CHECK_EQ_INT(b.data[b.len], '\0');
    size_t len;
    char *s = riak_buf_steal(&b, &len);
    CHECK_EQ_INT(len, 3000);
    CHECK(b.data == NULL && b.len == 0);
    free(s);
}

static void test_buf_steal_empty(void) {
    riak_buf b = {0};
    char *s = riak_buf_steal(&b, NULL);
    CHECK_EQ_STR(s, "");
    free(s);
}

static void test_escape(void) {
    riak_buf b = {0};
    CHECK_EQ_INT(riak_url_append_escaped(&b, "a/b?c#d e%~._-"), 0);
    CHECK_EQ_STR(b.data, "a%2Fb%3Fc%23d%20e%25~._-");
    riak_buf_free(&b);

    CHECK_EQ_INT(riak_url_append_escaped(&b, "\xD5\xA1"), 0); /* UTF-8 */
    CHECK_EQ_STR(b.data, "%D5%A1");
    riak_buf_free(&b);
}

static void test_unescape(void) {
    char *s = riak_url_unescape("auto%20key%2F1", 14);
    CHECK_EQ_STR(s, "auto key/1");
    free(s);
    CHECK(riak_url_unescape("bad%2", 5) == NULL);
    CHECK(riak_url_unescape("bad%zz", 6) == NULL);
}

static void test_query(void) {
    riak_buf b = {0};
    riak_buf_append_str(&b, "http://h:1/buckets/x/keys/y");
    CHECK_EQ_INT(riak_url_append_quorum(&b, "r", RIAK_Q_DEFAULT), 0);
    CHECK_EQ_INT(riak_url_append_quorum(&b, "r", RIAK_Q_QUORUM), 0);
    CHECK_EQ_INT(riak_url_append_quorum(&b, "w", 3), 0);
    CHECK_EQ_INT(riak_url_append_query(&b, "returnbody", "true"), 0);
    CHECK_EQ_STR(b.data, "http://h:1/buckets/x/keys/y?r=quorum&w=3&returnbody=true");
    CHECK_EQ_INT(riak_url_append_quorum(&b, "pr", RIAK_Q_ONE), 0);
    CHECK_EQ_INT(riak_url_append_quorum(&b, "pw", RIAK_Q_ALL), 0);
    CHECK_EQ_STR(b.data, "http://h:1/buckets/x/keys/y?r=quorum&w=3&returnbody=true&pr=one&pw=all");
    CHECK_EQ_INT(riak_url_append_quorum(&b, "w", -42), -1);
    riak_buf_free(&b);
}

static void test_headers(void) {
    CHECK(riak_header_value_ok("text/plain; charset=utf-8"));
    CHECK(!riak_header_value_ok("text/plain\r\nX-Evil: 1"));
    CHECK(!riak_header_value_ok("a\nb"));
    CHECK(!riak_header_value_ok(NULL));
    CHECK(riak_header_name_ok("color_bin"));
    CHECK(!riak_header_name_ok("bad name"));
    CHECK(!riak_header_name_ok("bad:name"));
    CHECK(!riak_header_name_ok(""));
}

static void test_strprintf(void) {
    char *s = riak_strprintf("%s-%d", "x", 42);
    CHECK_EQ_STR(s, "x-42");
    free(s);
}

static void test_strlist(void) {
    riak_strlist l = {0};
    size_t cap = 0;
    for (int i = 0; i < 100; i++) {
        CHECK_EQ_INT(riak_strlist_push(&l, &cap, "key", 3), 0);
        CHECK(l.items[l.count] == NULL);
    }
    CHECK_EQ_INT(l.count, 100);
    riak_strlist_free(&l);
    CHECK(l.items == NULL && l.count == 0);
}

static void test_headerlist(void) {
    riak_headerlist h = {0};
    CHECK_EQ_INT(riak_headerlist_add_line(&h, "Content-Type:  text/plain \r\n", 28), 0);
    CHECK_EQ_INT(riak_headerlist_add_line(&h, "X-Riak-Index-age_int: 42", 24), 0);
    CHECK_EQ_INT(riak_headerlist_add_line(&h, "X-Riak-Index-name_bin: a", 24), 0);
    CHECK_EQ_INT(riak_headerlist_add_line(&h, "Link: <a>", 9), 0);
    CHECK_EQ_INT(riak_headerlist_add_line(&h, "Link: <b>", 9), 0);
    CHECK_EQ_INT(riak_headerlist_add_line(&h, "Empty:", 6), 0);
    CHECK_EQ_INT(riak_headerlist_add_line(&h, "\r\n", 2), 1);
    CHECK_EQ_INT(riak_headerlist_add_line(&h, ": novalue", 9), 1);
    CHECK_EQ_INT(riak_headerlist_add_line(&h, "HTTP/1.1 200 OK", 15), 1);
    CHECK_EQ_INT(h.count, 6);
    CHECK_EQ_STR(riak_headerlist_get(&h, "content-type"), "text/plain");
    CHECK_EQ_STR(riak_headerlist_get(&h, "Empty"), "");
    CHECK(riak_headerlist_get(&h, "missing") == NULL);

    riak_object obj = {0};
    CHECK_EQ_INT(riak_object_from_headers(&h, NULL, &obj), 0);
    CHECK_EQ_INT(obj.n_indexes, 2);
    if (obj.n_indexes == 2) {
        CHECK_EQ_STR(obj.indexes[0].name, "age_int");
        CHECK_EQ_STR(obj.indexes[1].value, "a");
    }
    CHECK_EQ_STR(obj.links, "<a>, <b>");
    CHECK(obj.meta == NULL && obj.n_meta == 0);
    riak_object_free(&obj);
    riak_headerlist_clear(&h);
    CHECK(h.items == NULL && h.count == 0);
}

int main(void) {
    RUN(test_buf_growth);
    RUN(test_buf_steal_empty);
    RUN(test_escape);
    RUN(test_unescape);
    RUN(test_query);
    RUN(test_headers);
    RUN(test_strprintf);
    RUN(test_strlist);
    RUN(test_headerlist);
    return CHECK_DONE();
}
