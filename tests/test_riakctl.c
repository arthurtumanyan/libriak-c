/*
 File:   test_riakctl.c
 SPDX-License-Identifier: GPL-3.0-or-later

 Unit tests of riakctl's internals: node URLs, base64, UTF-8 detection,
 argument parsing, exit codes, the JSON form of objects and data types,
 and the command-line front end (usage errors, without a node).
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "riakctl.h"

/* ---- node URLs ---------------------------------------------------------- */

static int setup(rc_conn *c, const char *urls) {
    memset(c, 0, sizeof *c);
    riak_config_init(&c->cfg);
    return rc_conn_setup(c, urls);
}

static void test_urls(void) {
    rc_conn c;
    CHECK_EQ_INT(setup(&c, "pb://riak1"), RC_OK);
    CHECK_EQ_INT(c.cfg.transport, RIAK_TRANSPORT_PB);
    CHECK(!c.cfg.tls);
    CHECK_EQ_INT(c.n_nodes, 1);
    CHECK_EQ_STR(c.nodes[0].host, "riak1");
    CHECK_EQ_INT(c.nodes[0].port, 0);
    CHECK(c.cfg.nodes == c.nodes && c.cfg.n_nodes == 1);
    rc_conn_free(&c);

    CHECK_EQ_INT(setup(&c, "pbs://10.0.0.1:18087"), RC_OK);
    CHECK_EQ_INT(c.cfg.transport, RIAK_TRANSPORT_PB);
    CHECK(c.cfg.tls);
    CHECK_EQ_STR(c.nodes[0].host, "10.0.0.1");
    CHECK_EQ_INT(c.nodes[0].port, 18087);
    rc_conn_free(&c);

    CHECK_EQ_INT(setup(&c, "http://[::1]:8098/ignored/path"), RC_OK);
    CHECK_EQ_INT(c.cfg.transport, RIAK_TRANSPORT_HTTP);
    CHECK_EQ_STR(c.nodes[0].host, "::1");
    CHECK_EQ_INT(c.nodes[0].port, 8098);
    rc_conn_free(&c);

    /* entries without a scheme take the first one; spaces are skipped */
    CHECK_EQ_INT(setup(&c, "https://a.example:8443, b.example,[fe80::1]:9"), RC_OK);
    CHECK_EQ_INT(c.cfg.transport, RIAK_TRANSPORT_HTTP);
    CHECK(c.cfg.tls);
    CHECK_EQ_INT(c.n_nodes, 3);
    CHECK_EQ_STR(c.nodes[0].host, "a.example");
    CHECK_EQ_INT(c.nodes[0].port, 8443);
    CHECK_EQ_STR(c.nodes[1].host, "b.example");
    CHECK_EQ_INT(c.nodes[1].port, 0);
    CHECK_EQ_STR(c.nodes[2].host, "fe80::1");
    CHECK_EQ_INT(c.nodes[2].port, 9);
    rc_conn_free(&c);

    /* no scheme at all: Protocol Buffers */
    CHECK_EQ_INT(setup(&c, "h1:1,h2:2"), RC_OK);
    CHECK_EQ_INT(c.cfg.transport, RIAK_TRANSPORT_PB);
    CHECK_EQ_INT(c.n_nodes, 2);
    CHECK_EQ_INT(c.nodes[1].port, 2);
    rc_conn_free(&c);

    /* many nodes: the arrays grow */
    char many[2048] = "pb://n0:1";
    for (int i = 1; i < 40; i++) {
        size_t len = strlen(many);
        snprintf(many + len, sizeof many - len, ",n%d:%d", i, i + 1);
    }
    CHECK_EQ_INT(setup(&c, many), RC_OK);
    CHECK_EQ_INT(c.n_nodes, 40);
    CHECK_EQ_STR(c.nodes[39].host, "n39");
    CHECK_EQ_INT(c.nodes[39].port, 40);
    rc_conn_free(&c);

    static const char *const bad[] = {
        "",                    /* no node */
        "pb://",               /* empty host */
        "pb://h:",             /* empty port */
        "pb://h:0",            /* port 0 */
        "pb://h:65536",        /* port out of range */
        "pb://h:80x",          /* trailing garbage */
        "pb://h:-1",           /* sign */
        "pb://h:1234567",      /* too long */
        "pb://[::1",           /* unterminated bracket */
        "pb://[]:1",           /* empty IPv6 literal */
        "pb://[::1]x",         /* garbage after the bracket */
        "ftp://h",             /* unknown scheme */
        "PB://h",              /* schemes are lower case */
        "pb://a:1,http://b:2", /* mixed schemes */
        "pb://a,,b",           /* empty entry */
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        int rc = setup(&c, bad[i]);
        if (rc != RC_USAGE) {
            fprintf(stderr, "  URL '%s' accepted (%d)\n", bad[i], rc);
        }
        CHECK_EQ_INT(rc, RC_USAGE);
        rc_conn_free(&c);
    }
}

/* ---- base64 ------------------------------------------------------------- */

static void test_base64(void) {
    /* RFC 4648, section 10 */
    static const char *const vec[][2] = {{"", ""},
                                         {"f", "Zg=="},
                                         {"fo", "Zm8="},
                                         {"foo", "Zm9v"},
                                         {"foob", "Zm9vYg=="},
                                         {"fooba", "Zm9vYmE="},
                                         {"foobar", "Zm9vYmFy"}};
    for (size_t i = 0; i < sizeof vec / sizeof *vec; i++) {
        char *enc = rc_base64_encode((const unsigned char *)vec[i][0], strlen(vec[i][0]));
        CHECK_EQ_STR(enc, vec[i][1]);
        free(enc);
        size_t len = 99;
        unsigned char *dec = rc_base64_decode(vec[i][1], &len);
        CHECK(dec != NULL);
        CHECK_EQ_INT(len, strlen(vec[i][0]));
        CHECK(dec && memcmp(dec, vec[i][0], len) == 0);
        free(dec);
    }
    /* every byte value, at every length modulo 3 */
    unsigned char all[258];
    for (size_t i = 0; i < sizeof all; i++) {
        all[i] = (unsigned char)(255 - i % 256);
    }
    for (size_t n = 253; n <= sizeof all; n++) {
        char *enc = rc_base64_encode(all, n);
        CHECK(enc != NULL);
        CHECK_EQ_INT(strlen(enc), (n + 2) / 3 * 4);
        size_t len = 0;
        unsigned char *dec = rc_base64_decode(enc, &len);
        CHECK_EQ_INT(len, n);
        CHECK(dec && memcmp(dec, all, n) == 0);
        free(dec);
        free(enc);
    }
    size_t len = 0;
    unsigned char *dec = rc_base64_decode("Zg", &len); /* padding is optional */
    CHECK(dec && len == 1 && dec[0] == 'f');
    free(dec);
    CHECK(rc_base64_decode("Zm9v!", &len) == NULL);
    CHECK(rc_base64_decode("Zm 9v", &len) == NULL);
    CHECK(rc_base64_decode("Zm9v\xc3\xa9", &len) == NULL);
}

/* ---- values: UTF-8 text or base64 --------------------------------------- */

/* "text" or "b64": how rc_value_json() stores the bytes. */
static const char *value_kind(const char *data, size_t len) {
    json_object *o = rc_value_json(data, len);
    const char *kind = json_object_object_get_ex(o, "value", NULL)       ? "text"
                       : json_object_object_get_ex(o, "value_b64", NULL) ? "b64"
                                                                         : "none";
    json_object_put(o);
    return kind;
}

#define KIND(lit, want) CHECK_EQ_STR(value_kind(lit, sizeof(lit) - 1), want)

static void test_values(void) {
    KIND("", "text");
    KIND("plain ASCII", "text");
    KIND("\xd4\xb5\xd6\x80\xd6\x87\xd5\xa1\xd5\xb6", "text"); /* Armenian */
    KIND("\xe2\x82\xac", "text");                             /* U+20AC */
    KIND("\xf0\x9f\x98\x80", "text");                         /* U+1F600 */
    KIND("\xf4\x8f\xbf\xbf", "text");                         /* U+10FFFF */
    KIND("a\0b", "b64");                                      /* NUL */
    KIND("\x80", "b64");                                      /* lone continuation */
    KIND("\xc0\x80", "b64");                                  /* overlong NUL */
    KIND("\xe0\x80\xaf", "b64");                              /* overlong '/' */
    KIND("\xed\xa0\x80", "b64");                              /* surrogate U+D800 */
    KIND("\xed\xaf\xbf", "b64");                              /* surrogate U+DBFF */
    KIND("\xed\xbf\xbf", "b64");                              /* surrogate U+DFFF */
    KIND("\xed\x9f\xbf", "text");                             /* U+D7FF, just below */
    KIND("\xee\x80\x80", "text");                             /* U+E000, just above */
    KIND("\xc2\x80", "text");                                 /* U+0080, smallest 2-byte form */
    KIND("\xc1\xbf", "b64");                                  /* overlong U+007F */
    KIND("\xf0\x8f\xbf\xbf", "b64");                          /* overlong U+FFFF */
    KIND("\xf4\x90\x80\x80", "b64");                          /* above U+10FFFF */
    KIND("\xe2\x82", "b64");                                  /* truncated */
    KIND("\xf8\x88\x80\x80\x80", "b64");                      /* 5-byte form */
    KIND("\xff", "b64");

    /* NULL data is an empty value whatever the length says */
    json_object *o = rc_value_json(NULL, 5);
    json_object *v;
    CHECK(json_object_object_get_ex(o, "value", &v) && json_object_get_string_len(v) == 0);
    json_object_put(o);

    char *data = NULL;
    size_t len = 0;
    o = json_tokener_parse("{\"value_b64\":\"AQL/\"}");
    CHECK(rc_value_from_json(o, &data, &len));
    CHECK(len == 3 && memcmp(data, "\x01\x02\xff", 3) == 0);
    free(data);
    json_object_put(o);
    o = json_tokener_parse("{\"value\":\"x\\u0000y\"}");
    CHECK(rc_value_from_json(o, &data, &len));
    CHECK(len == 3 && memcmp(data, "x\0y", 3) == 0);
    free(data);
    json_object_put(o);
    o = json_tokener_parse("{\"value_b64\":\"***\"}");
    CHECK(!rc_value_from_json(o, &data, &len));
    json_object_put(o);
    o = json_tokener_parse("{}");
    CHECK(rc_value_from_json(o, &data, &len));
    CHECK(data == NULL && len == 0);
    json_object_put(o);
}

/* ---- argument helpers ---------------------------------------------------- */

static void test_args(void) {
    int q = 99;
    CHECK(rc_parse_quorum("one", &q) && q == RIAK_Q_ONE);
    CHECK(rc_parse_quorum("quorum", &q) && q == RIAK_Q_QUORUM);
    CHECK(rc_parse_quorum("all", &q) && q == RIAK_Q_ALL);
    CHECK(rc_parse_quorum("default", &q) && q == RIAK_Q_DEFAULT);
    CHECK(rc_parse_quorum("3", &q) && q == 3);
    CHECK(rc_parse_quorum("1000", &q) && q == 1000);
    q = 42;
    CHECK(!rc_parse_quorum("0", &q));
    CHECK(!rc_parse_quorum("1001", &q));
    CHECK(!rc_parse_quorum("-1", &q));
    CHECK(!rc_parse_quorum("", &q));
    CHECK(!rc_parse_quorum("3x", &q));
    CHECK(!rc_parse_quorum("ONE", &q));
    CHECK_EQ_INT(q, 42); /* untouched on failure */

    unsigned long long v = 7;
    CHECK(rc_parse_uint("0", 10, &v) && v == 0);
    CHECK(rc_parse_uint("10", 10, &v) && v == 10);
    CHECK(rc_parse_uint("18446744073709551615", ULLONG_MAX, &v) && v == ULLONG_MAX);
    v = 7;
    CHECK(!rc_parse_uint("11", 10, &v));
    CHECK(!rc_parse_uint("18446744073709551616", ULLONG_MAX, &v)); /* overflow */
    CHECK(!rc_parse_uint("+1", 10, &v));
    CHECK(!rc_parse_uint(" 1", 10, &v));
    CHECK(!rc_parse_uint("1 ", 10, &v));
    CHECK(!rc_parse_uint("", 10, &v));
    CHECK(!rc_parse_uint(NULL, 10, &v));
    CHECK_EQ_INT(v, 7);

    char *n = NULL, *val = NULL;
    CHECK(rc_split_pair("a=b", &n, &val));
    CHECK_EQ_STR(n, "a");
    CHECK_EQ_STR(val, "b");
    free(n);
    free(val);
    CHECK(rc_split_pair("k=", &n, &val));
    CHECK_EQ_STR(val, "");
    free(n);
    free(val);
    CHECK(rc_split_pair("expr=a=b", &n, &val));
    CHECK_EQ_STR(n, "expr");
    CHECK_EQ_STR(val, "a=b");
    free(n);
    free(val);
    CHECK(!rc_split_pair("=b", &n, &val));
    CHECK(!rc_split_pair("ab", &n, &val));

    rc_strv sv = {0};
    for (int i = 0; i < 100; i++) {
        CHECK(rc_strv_push(&sv, i % 2 ? "odd" : "even"));
    }
    CHECK_EQ_INT(sv.count, 100);
    CHECK_EQ_STR(sv.items[99], "odd");
    rc_strv_free(&sv);
    CHECK(sv.items == NULL && sv.count == 0);
}

static void test_exit_codes(void) {
    CHECK_EQ_INT(rc_exit_code(RIAK_OK), 0);
    CHECK_EQ_INT(rc_exit_code(RIAK_E_INVALID_ARG), 2);
    CHECK_EQ_INT(rc_exit_code(RIAK_E_NOT_FOUND), 3);
    CHECK_EQ_INT(rc_exit_code(RIAK_E_SIBLINGS), 4);
    CHECK_EQ_INT(rc_exit_code(RIAK_E_PRECONDITION), 5);
    CHECK_EQ_INT(rc_exit_code(RIAK_E_UNAUTHORIZED), 6);
    CHECK_EQ_INT(rc_exit_code(RIAK_E_TRANSPORT), 7);
    CHECK_EQ_INT(rc_exit_code(RIAK_E_TLS), 7);
    CHECK_EQ_INT(rc_exit_code(RIAK_E_UNSUPPORTED), 8);
    CHECK_EQ_INT(rc_exit_code(RIAK_E_NOMEM), 1);
    CHECK_EQ_INT(rc_exit_code(RIAK_E_HTTP), 1);
    CHECK_EQ_INT(rc_exit_code(RIAK_E_PARSE), 1);
    CHECK_EQ_INT(rc_exit_code(RIAK_E_NOT_MODIFIED), 1);
    CHECK_EQ_INT(rc_exit_code(RIAK_E_CANCELLED), 1);
    CHECK_EQ_INT(rc_exit_code(RIAK_E_SERVER), 1);
}

/* ---- JSON forms ---------------------------------------------------------- */

static void check_json(json_object *o, const char *want) {
    /* the flags riakctl prints with */
    CHECK_EQ_STR(
        json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE),
        want);
    json_object_put(o);
}

static void test_json(void) {
    char data[] = "hi";
    char ct[] = "text/plain";
    char lm[] = "Mon, 02 Nov 2026 10:00:00 GMT";
    char m1[] = "owner", m2[] = "alice";
    char i1[] = "tag_bin", i2[] = "a", i3[] = "b", i4[] = "n_int", i5[] = "5";
    riak_header meta[] = {{m1, m2}};
    riak_header ix[] = {{i1, i2}, {i4, i5}, {i1, i3}};
    riak_object obj = {.data = data,
                       .len = 2,
                       .content_type = ct,
                       .last_modified = lm,
                       .meta = meta,
                       .n_meta = 1,
                       .indexes = ix,
                       .n_indexes = 3};
    check_json(rc_object_json(&obj, true),
               "{\"value\":\"hi\",\"content_type\":\"text/plain\",\"last_modified\":\"Mon, 02 Nov "
               "2026 10:00:00 GMT\",\"meta\":{\"owner\":\"alice\"},\"indexes\":{\"tag_bin\":[\"a\","
               "\"b\"],\"n_int\":[\"5\"]}}");
    obj.deleted = true;
    obj.n_meta = obj.n_indexes = 0;
    obj.last_modified = NULL;
    check_json(rc_object_json(&obj, false), "{\"content_type\":\"text/plain\",\"deleted\":true}");

    /* a map with every field type, nested */
    char e1[] = "x", e2[] = "y", reg[] = "Yerevan";
    char *elems[] = {e1, e2, NULL};
    char f_city[] = "city", f_n[] = "visits", f_tags[] = "tags", f_on[] = "on", f_addr[] = "addr";
    riak_dt_entry inner[] = {{f_city, {.type = RIAK_DT_REGISTER, .reg = reg}}};
    riak_dt_entry fields[] = {
        {f_n, {.type = RIAK_DT_COUNTER, .counter = -3}},
        {f_tags, {.type = RIAK_DT_SET, .elements = elems, .n_elements = 2}},
        {f_on, {.type = RIAK_DT_FLAG, .flag = true}},
        {f_addr, {.type = RIAK_DT_MAP, .entries = inner, .n_entries = 1}},
    };
    riak_dt_value map = {.type = RIAK_DT_MAP, .entries = fields, .n_entries = 4};
    check_json(rc_dt_json(&map), "{\"visits_counter\":-3,\"tags_set\":[\"x\",\"y\"],\"on_flag\":"
                                 "true,\"addr_map\":{\"city_register\":\"Yerevan\"}}");
    riak_dt_value hll = {.type = RIAK_DT_HLL, .counter = 12345};
    check_json(rc_dt_json(&hll), "12345");
    riak_dt_value empty_set = {.type = RIAK_DT_GSET};
    check_json(rc_dt_json(&empty_set), "[]");

    riak_dt_type t;
    static const char *const names[] = {"counter", "set", "map", "hll", "gset", "register", "flag"};
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        CHECK(rc_dt_from_name(names[i], &t) && (size_t)t == i);
        CHECK_EQ_STR(rc_dt_name(t), names[i]);
    }
    CHECK(!rc_dt_from_name("Counter", &t));
    CHECK(!rc_dt_from_name("", &t));
    CHECK_EQ_STR(rc_dt_name((riak_dt_type)99), "unknown");
}

/* ---- the front end, without a node --------------------------------------- */

static int run(const char *const *args) {
    char *argv[16];
    int argc = 0;
    for (; args[argc]; argc++) {
        argv[argc] = (char *)(uintptr_t)args[argc];
    }
    argv[argc] = NULL;
    return rc_main(argc, argv);
}

#define RUN_RC(want, ...)                                                                          \
    do {                                                                                           \
        static const char *const a_[] = {"riakctl", __VA_ARGS__, NULL};                            \
        CHECK_EQ_INT(run(a_), want);                                                               \
    } while (0)

/* ---- line reader -------------------------------------------------------- */

static FILE *input(const char *data, size_t len) {
    FILE *f = tmpfile();
    CHECK(f != NULL);
    if (f) {
        CHECK_EQ_INT(fwrite(data, 1, len, f), len);
        rewind(f);
    }
    return f;
}

static void test_read_line(void) {
    /* lines up to the limit come back whole, with their newline; a NUL
     * byte is kept and counted */
    static const char text[] = "ab\n\nxyz\0q\n12345678\n123456789\nlast";
    FILE *f = input(text, sizeof text - 1);
    if (f == NULL) {
        return;
    }
    char *buf = NULL;
    size_t cap = 0;
    bool too_long = true;
    CHECK_EQ_INT(rc_read_line(f, &buf, &cap, 9, &too_long), 3);
    CHECK(!too_long && strcmp(buf, "ab\n") == 0);
    CHECK_EQ_INT(rc_read_line(f, &buf, &cap, 9, &too_long), 1);
    CHECK(!too_long && strcmp(buf, "\n") == 0);
    CHECK_EQ_INT(rc_read_line(f, &buf, &cap, 9, &too_long), 6);
    CHECK(!too_long && memcmp(buf, "xyz\0q\n", 7) == 0);
    /* exactly the limit, newline included */
    CHECK_EQ_INT(rc_read_line(f, &buf, &cap, 9, &too_long), 9);
    CHECK(!too_long && strcmp(buf, "12345678\n") == 0);
    /* one byte over: the line is consumed, its full length returned */
    CHECK_EQ_INT(rc_read_line(f, &buf, &cap, 9, &too_long), 10);
    CHECK(too_long && strncmp(buf, "123456789", 9) == 0 && buf[9] == '\0');
    /* the next line is read normally: nothing of the long one is left */
    CHECK_EQ_INT(rc_read_line(f, &buf, &cap, 9, &too_long), 4);
    CHECK(!too_long && strcmp(buf, "last") == 0);
    CHECK_EQ_INT(rc_read_line(f, &buf, &cap, 9, &too_long), -1);
    CHECK_EQ_INT(rc_read_line(f, &buf, &cap, 9, &too_long), -1);
    (void)fclose(f);
    free(buf);

    /* a line far above the limit keeps memory at the limit */
    size_t big = 1 << 20;
    char *data = malloc(big + 1);
    CHECK(data != NULL);
    if (data == NULL) {
        return;
    }
    memset(data, 'x', big);
    data[big] = '\n';
    f = input(data, big + 1);
    free(data);
    buf = NULL;
    cap = 0;
    CHECK_EQ_INT(rc_read_line(f, &buf, &cap, 4096, &too_long), (long)big + 1);
    CHECK(too_long && cap <= 8192 && strlen(buf) == 4096);
    CHECK_EQ_INT(rc_read_line(f, &buf, &cap, 4096, &too_long), -1);
    (void)fclose(f);
    free(buf);

    /* a limit of 0 keeps nothing but still ends the string */
    f = input("abc\n", 4);
    buf = NULL;
    cap = 0;
    CHECK_EQ_INT(rc_read_line(f, &buf, &cap, 0, &too_long), 4);
    CHECK(too_long && buf && buf[0] == '\0');
    (void)fclose(f);
    free(buf);
}

static void test_front_end(void) {
    RUN_RC(0, "--version");
    RUN_RC(0, "help");
    RUN_RC(0, "help", "backup");
    RUN_RC(0, "get", "--help");
    RUN_RC(2, "help", "nope");
    RUN_RC(2, "nope");
    RUN_RC(2, "--bogus-option", "ping");
    RUN_RC(2, "get");
    RUN_RC(2, "get", "only-bucket");
    RUN_RC(2, "get", "b", "k", "extra");
    RUN_RC(2, "put", "--w", "lots", "b", "k");
    RUN_RC(2, "put", "--value", "x", "--file", "f", "b", "k");
    RUN_RC(2, "put", "--meta", "novalue", "--value", "x", "b", "k");
    RUN_RC(2, "list", "things");
    RUN_RC(2, "backup", "--method", "magic", "b");
    RUN_RC(2, "restore", "a", "b");
    RUN_RC(2, "dt", "get", "b", "k"); /* no bucket type */
    RUN_RC(2, "-t", "maps", "dt", "frobnicate", "b", "k");
    RUN_RC(2, "aae", "tree", "b", "--size", "huge");
    RUN_RC(2, "aae", "root", "x");
    RUN_RC(2, "aae", "find", "b");    /* needs --siblings or --object-size */
    RUN_RC(2, "aae", "compare", "b"); /* needs a peer */
    RUN_RC(2, "aae", "keys", "b", "--modified-start", "10", "--modified-end", "5");
    RUN_RC(2, "aae", "keys", "b", "--segments", "1,x");
    RUN_RC(2, "repl", "push");
    RUN_RC(2, "search", "query", "idx");
    RUN_RC(2, "props", "explode", "b");
    RUN_RC(2, "counter", "get", "b");
    RUN_RC(2, "--timeout", "soon", "ping");
    RUN_RC(2, "--retries", "1000", "ping");
    RUN_RC(2, "--http-port", "70000", "ping");
    RUN_RC(2, "-u", "ftp://h", "ping");
    RUN_RC(2, "-u", "pb://h:0", "ping");
    RUN_RC(2, "--user", "alice", "ping"); /* no password */
    /* the password file must exist */
    RUN_RC(1, "--user", "alice", "--password-file", "/nonexistent/riakctl-test", "ping");
    /* closed port: unreachable */
    RUN_RC(7, "-u", "pb://127.0.0.1:1", "--connect-timeout", "500", "ping");
    RUN_RC(7, "-u", "http://127.0.0.1:1", "--connect-timeout", "500", "--retries", "0", "get", "b",
           "k");
}

int main(void) {
    /* rc_main() calls riak_global_init() itself; the unit tests need it too */
    if (riak_global_init() != RIAK_OK) {
        return 1;
    }
    RUN(test_urls);
    RUN(test_base64);
    RUN(test_values);
    RUN(test_args);
    RUN(test_exit_codes);
    RUN(test_json);
    RUN(test_read_line);
    RUN(test_front_end);
    riak_global_cleanup();
    return CHECK_DONE();
}
