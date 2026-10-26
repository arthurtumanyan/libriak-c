/*
 File:   test_datatype.c
 SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "datatype.h"

static void expect_json(riak_dt_op *op, const char *context, const char *want) {
    char *json = NULL;
    CHECK_EQ_INT(riak_dt_op_to_json(op, context, &json), RIAK_OK);
    CHECK_EQ_STR(json, want);
    free(json);
}

static void expect_invalid(riak_dt_op *op) {
    char *json = (char *)"unchanged";
    CHECK_EQ_INT(riak_dt_op_to_json(op, NULL, &json), RIAK_E_INVALID_ARG);
    CHECK(json == NULL);
}

static void test_counter_and_sets(void) {
    riak_dt_op *c = riak_dt_op_new(RIAK_DT_COUNTER);
    riak_dt_op_increment(c, 5);
    riak_dt_op_increment(c, -7);
    expect_json(c, NULL, "{\"increment\":-2}");
    riak_dt_op_free(c);

    riak_dt_op *s = riak_dt_op_new(RIAK_DT_SET);
    riak_dt_op_add(s, "a");
    riak_dt_op_add(s, "\"quoted\"");
    riak_dt_op_remove(s, "b");
    expect_json(s, "ctx==",
                "{\"add_all\":[\"a\",\"\\\"quoted\\\"\"],\"remove_all\":[\"b\"],"
                "\"context\":\"ctx==\"}");
    riak_dt_op_free(s);

    riak_dt_op *h = riak_dt_op_new(RIAK_DT_HLL);
    riak_dt_op_add(h, "x");
    expect_json(h, NULL, "{\"add_all\":[\"x\"]}");
    riak_dt_op_free(h);

    riak_dt_op *g = riak_dt_op_new(RIAK_DT_GSET);
    riak_dt_op_add(g, "y");
    expect_json(g, NULL, "{\"add_all\":[\"y\"]}");
    riak_dt_op_free(g);
}

static void test_map(void) {
    riak_dt_op *m = riak_dt_op_new(RIAK_DT_MAP);
    riak_dt_op_map_register(m, "name", "bob");
    riak_dt_op_map_register(m, "name", "alice"); /* last write wins */
    riak_dt_op_map_flag(m, "admin", true);
    riak_dt_op_map_counter(m, "visits", 2);
    riak_dt_op_map_counter(m, "visits", 1); /* deltas accumulate */
    riak_dt_op *tags = riak_dt_op_map_set(m, "tags");
    riak_dt_op_add(tags, "x");
    CHECK(riak_dt_op_map_set(m, "tags") == tags); /* same nested op */
    riak_dt_op *addr = riak_dt_op_map_map(m, "addr");
    riak_dt_op_map_register(addr, "city", "Yerevan");
    riak_dt_op_map_flag(addr, "verified", false);
    riak_dt_op_map_remove(m, "old", RIAK_DT_FLAG);
    expect_json(m, "c1",
                "{\"update\":{\"name_register\":\"alice\",\"admin_flag\":\"enable\","
                "\"visits_counter\":{\"increment\":3},\"tags_set\":{\"add_all\":[\"x\"]},"
                "\"addr_map\":{\"update\":{\"city_register\":\"Yerevan\","
                "\"verified_flag\":\"disable\"}}},\"remove\":[\"old_flag\"],\"context\":\"c1\"}");
    riak_dt_op_free(m);

    riak_dt_op *only_remove = riak_dt_op_new(RIAK_DT_MAP);
    riak_dt_op_map_remove(only_remove, "a", RIAK_DT_COUNTER);
    riak_dt_op_map_remove(only_remove, "b", RIAK_DT_MAP);
    expect_json(only_remove, NULL, "{\"remove\":[\"a_counter\",\"b_map\"]}");
    riak_dt_op_free(only_remove);
}

static void test_misuse(void) {
    CHECK(riak_dt_op_new(RIAK_DT_REGISTER) == NULL);
    CHECK(riak_dt_op_new(RIAK_DT_FLAG) == NULL);
    CHECK(riak_dt_op_new((riak_dt_type)99) == NULL);

    riak_dt_op *empty[] = {riak_dt_op_new(RIAK_DT_SET), riak_dt_op_new(RIAK_DT_MAP),
                           riak_dt_op_new(RIAK_DT_HLL)};
    for (size_t i = 0; i < 3; i++) {
        expect_invalid(empty[i]);
        riak_dt_op_free(empty[i]);
    }

    riak_dt_op *s = riak_dt_op_new(RIAK_DT_SET);
    riak_dt_op_add(s, "a");
    riak_dt_op_increment(s, 1); /* not a counter */
    expect_invalid(s);
    riak_dt_op_free(s);

    riak_dt_op *h = riak_dt_op_new(RIAK_DT_HLL);
    riak_dt_op_add(h, "a");
    riak_dt_op_remove(h, "a"); /* HLLs cannot remove */
    expect_invalid(h);
    riak_dt_op_free(h);

    riak_dt_op *c = riak_dt_op_new(RIAK_DT_COUNTER);
    riak_dt_op_increment(c, INT64_MAX);
    riak_dt_op_increment(c, 1); /* overflow */
    expect_invalid(c);
    riak_dt_op_free(c);

    riak_dt_op *m = riak_dt_op_new(RIAK_DT_MAP);
    CHECK(riak_dt_op_map_set(NULL, "x") == NULL);
    riak_dt_op_map_register(m, "r", NULL);
    expect_invalid(m);
    riak_dt_op_free(m);

    m = riak_dt_op_new(RIAK_DT_MAP);
    riak_dt_op_map_counter(m, "", 1); /* empty field name */
    expect_invalid(m);
    riak_dt_op_free(m);

    m = riak_dt_op_new(RIAK_DT_MAP);
    riak_dt_op_map_remove(m, "x", RIAK_DT_HLL); /* HLLs cannot live in maps */
    expect_invalid(m);
    riak_dt_op_free(m);

    /* a bad nested op makes the whole update invalid */
    m = riak_dt_op_new(RIAK_DT_MAP);
    riak_dt_op *inner = riak_dt_op_map_set(m, "s");
    riak_dt_op_increment(inner, 1);
    expect_invalid(m);
    riak_dt_op_free(m);

    /* nested maps stop at 32 levels */
    m = riak_dt_op_new(RIAK_DT_MAP);
    riak_dt_op *cur = m;
    int levels = 0;
    while (cur && levels < 100) {
        riak_dt_op *next_level = riak_dt_op_map_map(cur, "m");
        if (next_level == NULL) break;
        cur = next_level;
        levels++;
    }
    CHECK_EQ_INT(levels, 32);
    expect_invalid(m);
    riak_dt_op_free(m);

    s = riak_dt_op_new(RIAK_DT_SET);
    riak_dt_op_map_counter(s, "x", 1); /* map call on a set */
    expect_invalid(s);
    riak_dt_op_free(s);

    char *json = NULL;
    CHECK_EQ_INT(riak_dt_op_to_json(NULL, NULL, &json), RIAK_E_INVALID_ARG);
    riak_dt_op_free(NULL);
    riak_dt_op_increment(NULL, 1);
    riak_dt_op_add(NULL, "x");
    riak_datatype_free(NULL);
}

static riak_status parse(const char *json, riak_datatype *dt) {
    return riak_dt_parse(json, strlen(json), dt);
}

static void test_parse_values(void) {
    riak_datatype dt;
    CHECK_EQ_INT(parse("{\"type\":\"counter\",\"value\":-3}", &dt), RIAK_OK);
    CHECK(dt.value.type == RIAK_DT_COUNTER && dt.value.counter == -3 && dt.context == NULL);
    riak_datatype_free(&dt);

    CHECK_EQ_INT(parse("{\"type\":\"set\",\"value\":[\"a\",\"b\"],\"context\":\"g2w=\"}", &dt),
                 RIAK_OK);
    CHECK(dt.value.n_elements == 2 && dt.value.elements[2] == NULL);
    CHECK_EQ_STR(dt.context, "g2w=");
    riak_datatype_free(&dt);

    CHECK_EQ_INT(parse("{\"type\":\"gset\",\"value\":[]}", &dt), RIAK_OK);
    CHECK(dt.value.type == RIAK_DT_GSET && dt.value.n_elements == 0 && dt.value.elements != NULL);
    riak_datatype_free(&dt);

    CHECK_EQ_INT(parse("{\"type\":\"hll\",\"value\":42}", &dt), RIAK_OK);
    CHECK(dt.value.type == RIAK_DT_HLL && dt.value.counter == 42);
    riak_datatype_free(&dt);

    CHECK_EQ_INT(parse("{\"type\":\"map\",\"value\":{\"addr_map\":{\"city_register\":\"Yerevan\"},"
                       "\"admin_flag\":true,\"first_name_register\":\"alice\","
                       "\"tags_set\":[\"x\"],\"visits_counter\":5},\"context\":\"c\"}",
                       &dt),
                 RIAK_OK);
    CHECK_EQ_INT(dt.value.n_entries, 5);
    const riak_dt_value *v = riak_dt_map_get(&dt.value, "first_name", RIAK_DT_REGISTER);
    CHECK(v && strcmp(v->reg, "alice") == 0); /* names may contain '_' */
    CHECK(riak_dt_map_get(&dt.value, "admin", RIAK_DT_FLAG)->flag);
    CHECK(riak_dt_map_get(&dt.value, "visits", RIAK_DT_COUNTER)->counter == 5);
    CHECK(riak_dt_map_get(&dt.value, "visits", RIAK_DT_SET) == NULL);
    const riak_dt_value *addr = riak_dt_map_get(&dt.value, "addr", RIAK_DT_MAP);
    CHECK(addr && strcmp(riak_dt_map_get(addr, "city", RIAK_DT_REGISTER)->reg, "Yerevan") == 0);
    CHECK(riak_dt_map_get(NULL, "x", RIAK_DT_MAP) == NULL);
    CHECK(riak_dt_map_get(v, "x", RIAK_DT_MAP) == NULL); /* not a map */
    riak_datatype_free(&dt);
}

static void test_parse_errors(void) {
    const char *bad[] = {
        "",
        "[]",
        "{\"value\":1}",
        "{\"type\":\"widget\",\"value\":1}",
        "{\"type\":\"register\",\"value\":\"x\"}",
        "{\"type\":\"counter\"}",
        "{\"type\":\"counter\",\"value\":\"1\"}",
        "{\"type\":\"set\",\"value\":[1]}",
        "{\"type\":\"set\",\"value\":\"a\"}",
        "{\"type\":\"map\",\"value\":{\"nosuffix\":1}}",
        "{\"type\":\"map\",\"value\":{\"_counter\":1}}",
        "{\"type\":\"map\",\"value\":{\"h_hll\":1}}",
        "{\"type\":\"map\",\"value\":{\"f_flag\":\"yes\"}}",
        "{\"type\":\"map\",\"value\":{\"r_register\":3}}",
        "{\"type\":\"map\",\"value\":[]}",
        "{\"type\":\"counter\",\"value\":1,\"context\":5}",
        "{\"type\":\"counter\",\"value\":1",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        riak_datatype dt;
        CHECK_EQ_INT(parse(bad[i], &dt), RIAK_E_PARSE);
        CHECK(dt.value.entries == NULL && dt.context == NULL);
    }

    /* nesting deeper than 32 maps is refused */
    char deep[2048] = "{\"type\":\"map\",\"value\":";
    for (int i = 0; i < 40; i++)
        strcat(deep, "{\"m_map\":");
    strcat(deep, "{}");
    for (int i = 0; i < 40; i++)
        strcat(deep, "}");
    strcat(deep, "}");
    riak_datatype dt;
    CHECK_EQ_INT(parse(deep, &dt), RIAK_E_PARSE);
}

int main(void) {
    RUN(test_counter_and_sets);
    RUN(test_map);
    RUN(test_misuse);
    RUN(test_parse_values);
    RUN(test_parse_errors);
    return CHECK_DONE();
}
