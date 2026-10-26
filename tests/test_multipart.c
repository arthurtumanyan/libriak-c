/*
 File:   test_multipart.c
 SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "multipart.h"

static void test_boundary(void) {
    char *b = riak_multipart_boundary("multipart/mixed; boundary=YinLMzyUR9feB17okMytgKsylvh");
    CHECK_EQ_STR(b, "YinLMzyUR9feB17okMytgKsylvh");
    free(b);
    b = riak_multipart_boundary("Multipart/Mixed;charset=x; BOUNDARY=\"a b\"; x=y");
    CHECK_EQ_STR(b, "a b");
    free(b);
    CHECK(riak_multipart_boundary("text/plain; boundary=x") == NULL);
    CHECK(riak_multipart_boundary("multipart/mixed") == NULL);
    CHECK(riak_multipart_boundary("multipart/mixed; boundary=") == NULL);
    CHECK(riak_multipart_boundary("multipart/mixed; boundary=\"open") == NULL);
    CHECK(riak_multipart_boundary(NULL) == NULL);
}

/* Shape of a Riak 2.x siblings response. */
static const char riak_body[] = "\r\n--B\r\n"
                                "Content-Type: text/plain\r\n"
                                "Link: </buckets/test>; rel=\"up\"\r\n"
                                "Etag: 1\r\n"
                                "X-Riak-Meta-Color: red\r\n"
                                "\r\n"
                                "first\r\n"
                                "--B\r\n"
                                "Content-Type: text/plain\r\n"
                                "X-Riak-Deleted: true\r\n"
                                "\r\n"
                                "\r\n"
                                "--B\r\n"
                                "\r\n"
                                "no headers\r\n\r\n"
                                "--B--\r\n";

static void test_parse_riak_shape(void) {
    riak_part *parts;
    size_t n;
    CHECK_EQ_INT(riak_multipart_parse("B", riak_body, sizeof riak_body - 1, &parts, &n), RIAK_OK);
    CHECK_EQ_INT(n, 3);
    if (n == 3) {
        CHECK_EQ_STR(parts[0].body, "first");
        CHECK_EQ_INT(parts[0].len, 5);
        CHECK_EQ_STR(riak_headerlist_get(&parts[0].headers, "content-type"), "text/plain");
        CHECK_EQ_STR(riak_headerlist_get(&parts[0].headers, "ETag"), "1");
        CHECK_EQ_INT(parts[1].len, 0);
        CHECK_EQ_STR(riak_headerlist_get(&parts[1].headers, "X-Riak-Deleted"), "true");
        CHECK_EQ_INT(parts[2].headers.count, 0);
        CHECK_EQ_STR(parts[2].body, "no headers\r\n");
    }
    riak_parts_free(parts, n);
}

static void test_parse_binary_and_preamble(void) {
    const char body[] = "preamble\r\n--xyz\r\n\r\na\0b\r\n--xyz--";
    riak_part *parts;
    size_t n;
    CHECK_EQ_INT(riak_multipart_parse("xyz", body, sizeof body - 1, &parts, &n), RIAK_OK);
    CHECK_EQ_INT(n, 1);
    if (n == 1) {
        CHECK_EQ_INT(parts[0].len, 3);
        CHECK(memcmp(parts[0].body, "a\0b", 3) == 0);
    }
    riak_parts_free(parts, n);
}

static void test_parse_errors(void) {
    riak_part *parts;
    size_t n;
    const char *cases[] = {
        "",                          /* empty */
        "--B\r\n\r\nbody",           /* no close delimiter */
        "no delimiter at all",       /* boundary missing */
        "--B\r\n\r\nbody\r\n--B",    /* ends after delimiter */
        "--Bjunk\r\n\r\nx\r\n--B--", /* garbage after boundary */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        CHECK_EQ_INT(riak_multipart_parse("B", cases[i], strlen(cases[i]), &parts, &n),
                     RIAK_E_PARSE);
        CHECK(parts == NULL && n == 0);
    }
    CHECK_EQ_INT(riak_multipart_parse("", "x", 1, &parts, &n), RIAK_E_PARSE);
    CHECK_EQ_INT(riak_multipart_parse(NULL, "x", 1, &parts, &n), RIAK_E_PARSE);
}

static void test_object_from_part(void) {
    riak_part *parts;
    size_t n;
    CHECK_EQ_INT(riak_multipart_parse("B", riak_body, sizeof riak_body - 1, &parts, &n), RIAK_OK);
    riak_object obj = {0};
    CHECK_EQ_INT(riak_object_from_headers(&parts[0].headers, "vc-shared", &obj), 0);
    CHECK_EQ_STR(obj.content_type, "text/plain");
    CHECK_EQ_STR(obj.vclock, "vc-shared");
    CHECK_EQ_STR(obj.etag, "1");
    CHECK_EQ_STR(obj.links, "</buckets/test>; rel=\"up\"");
    CHECK_EQ_INT(obj.n_meta, 1);
    if (obj.n_meta == 1) {
        CHECK_EQ_STR(obj.meta[0].name, "Color");
        CHECK_EQ_STR(obj.meta[0].value, "red");
    }
    CHECK(!obj.deleted);
    riak_object_free(&obj);

    CHECK_EQ_INT(riak_object_from_headers(&parts[1].headers, NULL, &obj), 0);
    CHECK(obj.deleted);
    CHECK(obj.vclock == NULL);
    riak_object_free(&obj);
    riak_parts_free(parts, n);
}

int main(void) {
    RUN(test_boundary);
    RUN(test_parse_riak_shape);
    RUN(test_parse_binary_and_preamble);
    RUN(test_parse_errors);
    RUN(test_object_from_part);
    return CHECK_DONE();
}
