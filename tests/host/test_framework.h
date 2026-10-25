#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int g_test_assertions;
extern int g_test_failures;
extern const char *g_test_case;

#define TEST_CASE(name)            \
    do {                           \
        g_test_case = (name);      \
        printf("[ RUN      ] %s\n", name); \
    } while (0)

#define TEST_ASSERT(condition)                                                    \
    do {                                                                          \
        ++g_test_assertions;                                                      \
        if (!(condition)) {                                                       \
            ++g_test_failures;                                                    \
            printf("[   FAIL   ] %s:%d: %s\n", __FILE__, __LINE__, #condition);   \
        }                                                                         \
    } while (0)

#define TEST_ASSERT_MSG(condition, ...)                                           \
    do {                                                                          \
        ++g_test_assertions;                                                      \
        if (!(condition)) {                                                       \
            ++g_test_failures;                                                    \
            printf("[   FAIL   ] %s:%d: ", __FILE__, __LINE__);                   \
            printf(__VA_ARGS__);                                                  \
            printf("\n");                                                         \
        }                                                                         \
    } while (0)

#define TEST_EQ_INT(expected, actual)                                             \
    do {                                                                          \
        long long test_expected_ = (long long)(expected);                         \
        long long test_actual_ = (long long)(actual);                             \
        ++g_test_assertions;                                                      \
        if (test_expected_ != test_actual_) {                                     \
            ++g_test_failures;                                                    \
            printf("[   FAIL   ] %s:%d: expected %lld, got %lld\n", __FILE__,     \
                   __LINE__, test_expected_, test_actual_);                       \
        }                                                                         \
    } while (0)

#define TEST_EQ_STR(expected, actual)                                             \
    do {                                                                          \
        const char *test_expected_ = (expected);                                  \
        const char *test_actual_ = (actual);                                      \
        ++g_test_assertions;                                                      \
        if (test_expected_ == NULL || test_actual_ == NULL ||                     \
            strcmp(test_expected_, test_actual_) != 0) {                          \
            ++g_test_failures;                                                    \
            printf("[   FAIL   ] %s:%d: expected \"%s\", got \"%s\"\n", __FILE__, \
                   __LINE__, test_expected_ ? test_expected_ : "(null)",          \
                   test_actual_ ? test_actual_ : "(null)");                       \
        }                                                                         \
    } while (0)

#define TEST_EQ_FLOAT(expected, actual, epsilon)                                  \
    do {                                                                          \
        double test_expected_ = (double)(expected);                               \
        double test_actual_ = (double)(actual);                                   \
        double test_delta_ = test_expected_ - test_actual_;                       \
        if (test_delta_ < 0) {                                                    \
            test_delta_ = -test_delta_;                                           \
        }                                                                         \
        ++g_test_assertions;                                                      \
        if (test_delta_ > (epsilon)) {                                            \
            ++g_test_failures;                                                    \
            printf("[   FAIL   ] %s:%d: expected %.9f, got %.9f\n", __FILE__,     \
                   __LINE__, test_expected_, test_actual_);                       \
        }                                                                         \
    } while (0)
