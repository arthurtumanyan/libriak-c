/*
 File:   test_multipart.c
 SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
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

typedef struct {
    size_t n;
    char bodies[8][64];
} seen_parts;

static bool remember(void *ctx, riak_part *part, riak_status *err) {
    (void)err;
    seen_parts *s = ctx;
    if (s->n < 8) {
        snprintf(s->bodies[s->n], sizeof s->bodies[0], "%s", part->body);
    }
    s->n++;
    return true;
}

static void test_stream_matches_batch(void) {
    /* feed in every chunk size: the parts must equal the one-shot parse */
    size_t len = sizeof riak_body - 1;
    for (size_t step = 1; step <= len; step++) {
        seen_parts seen = {0};
        riak_mpstream *s = riak_mpstream_new("B", 0, remember, &seen);
        CHECK(s != NULL);
        riak_status st = RIAK_OK;
        for (size_t off = 0; st == RIAK_OK && off < len; off += step) {
            st = riak_mpstream_feed(s, riak_body + off, len - off < step ? len - off : step);
        }
        CHECK_EQ_INT(st, RIAK_OK);
        CHECK_EQ_INT(riak_mpstream_finish(s), RIAK_OK);
        CHECK_EQ_INT(seen.n, 3);
        CHECK_EQ_STR(seen.bodies[0], "first");
        CHECK_EQ_STR(seen.bodies[2], "no headers\r\n");
        riak_mpstream_free(s);
    }
}

static bool stop_after_one(void *ctx, riak_part *part, riak_status *err) {
    (void)part;
    (void)err;
    (*(size_t *)ctx)++;
    return false;
}

static void test_stream_limits_and_stop(void) {
    /* a part larger than max_part fails the feed */
    riak_mpstream *s = riak_mpstream_new("B", 8, NULL, NULL);
    const char big[] = "--B\r\n\r\n0123456789abcdef";
    CHECK_EQ_INT(riak_mpstream_feed(s, big, sizeof big - 1), RIAK_E_HTTP);
    riak_mpstream_free(s);

    /* the callback can stop parsing */
    size_t calls = 0;
    s = riak_mpstream_new("B", 0, stop_after_one, &calls);
    CHECK_EQ_INT(riak_mpstream_feed(s, riak_body, sizeof riak_body - 1), RIAK_E_PARSE);
    CHECK_EQ_INT(calls, 1);
    riak_mpstream_free(s);

    /* a preamble longer than the delimiter, split across feeds */
    seen_parts seen = {0};
    s = riak_mpstream_new("B", 0, remember, &seen);
    const char *pre = "lots of preamble text -";
    const char *rest = "-B is not it\r\n--B\r\n\r\nx\r\n--B--";
    CHECK_EQ_INT(riak_mpstream_feed(s, pre, strlen(pre)), RIAK_OK);
    CHECK_EQ_INT(riak_mpstream_feed(s, rest, strlen(rest)), RIAK_OK);
    CHECK_EQ_INT(riak_mpstream_finish(s), RIAK_OK);
    CHECK_EQ_INT(seen.n, 1);
    CHECK_EQ_STR(seen.bodies[0], "x");
    riak_mpstream_free(s);

    CHECK(riak_mpstream_new("", 0, NULL, NULL) == NULL);
    CHECK(riak_mpstream_new(NULL, 0, NULL, NULL) == NULL);
    char *b = riak_multipart_boundary("multipart/mixed;boundary=DXmffoAWkavejCOV020OyUCJib");
    CHECK_EQ_STR(b, "DXmffoAWkavejCOV020OyUCJib"); /* Riak's 2i/mapred form, no space */
    free(b);
}

int main(void) {
    RUN(test_boundary);
    RUN(test_parse_riak_shape);
    RUN(test_parse_binary_and_preamble);
    RUN(test_parse_errors);
    RUN(test_object_from_part);
    RUN(test_stream_matches_batch);
    RUN(test_stream_limits_and_stop);
    return CHECK_DONE();
}
