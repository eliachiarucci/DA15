// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

/*
 * Minimal dependency-free test harness for host-side unit tests.
 */

#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdio.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        g_checks++;                                                       \
        if (!(cond)) {                                                    \
            g_failures++;                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                 \
    } while (0)

#define CHECK_EQ_I32(actual, expected)                                    \
    do {                                                                  \
        g_checks++;                                                       \
        long long a_ = (long long)(actual), e_ = (long long)(expected);   \
        if (a_ != e_) {                                                   \
            g_failures++;                                                 \
            printf("FAIL %s:%d: %s == %lld, expected %lld\n", __FILE__,   \
                   __LINE__, #actual, a_, e_);                            \
        }                                                                 \
    } while (0)

// Returns the process exit code: 0 on success, 1 if any check failed
static int test_summary(const char *suite) {
    printf("%s: %d checks, %d failures\n", suite, g_checks, g_failures);
    return g_failures ? 1 : 0;
}

#endif // TEST_UTIL_H
