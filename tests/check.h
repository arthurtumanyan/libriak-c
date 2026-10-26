/*
 File:   check.h
 SPDX-License-Identifier: GPL-3.0-or-later

 Minimal test assertions; no external framework needed.
 */

#ifndef RIAK_TEST_CHECK_H
#define RIAK_TEST_CHECK_H

#include <stdio.h>
#include <string.h>

static int check_failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            check_failures++;                                                                      \
        }                                                                                          \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                                         \
    do {                                                                                           \
        long long check_a_ = (long long)(a), check_b_ = (long long)(b);                            \
        if (check_a_ != check_b_) {                                                                \
            fprintf(stderr, "%s:%d: %s == %s failed: %lld != %lld\n", __FILE__, __LINE__, #a, #b,  \
                    check_a_, check_b_);                                                           \
            check_failures++;                                                                      \
        }                                                                                          \
    } while (0)

#define CHECK_EQ_STR(a, b)                                                                         \
    do {                                                                                           \
        const char *check_a_ = (a), *check_b_ = (b);                                               \
        if (check_a_ == NULL || check_b_ == NULL || strcmp(check_a_, check_b_) != 0) {             \
            fprintf(stderr, "%s:%d: %s == %s failed: \"%s\" != \"%s\"\n", __FILE__, __LINE__, #a,  \
                    #b, check_a_ ? check_a_ : "(null)", check_b_ ? check_b_ : "(null)");           \
            check_failures++;                                                                      \
        }                                                                                          \
    } while (0)

#define RUN(test)                                                                                  \
    do {                                                                                           \
        int check_before_ = check_failures;                                                        \
        test();                                                                                    \
        fprintf(stderr, "%s %s\n", check_failures == check_before_ ? "ok  " : "FAIL", #test);      \
    } while (0)

#define CHECK_DONE() (check_failures == 0 ? 0 : 1)

#endif /* RIAK_TEST_CHECK_H */
