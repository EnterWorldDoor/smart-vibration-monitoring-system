/**
 * @file test_utils.h
 * @brief OPC UA Server — minimal unit test framework
 *
 * Copied from rs232-gateway tests/test_utils.h.
 * Provides Unity-style assertion macros with zero external dependencies.
 */

#ifndef TEST_UTILS_H
#define TEST_UTILS_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

static int _tests_run = 0;
static int _tests_failed = 0;
static const char *_current_test = NULL;

#define RUN_TEST(func) do { \
    _current_test = #func; \
    printf("  RUN  %s\n", _current_test); \
    _tests_run++; \
    func(); \
} while(0)

#define TEST_ASSERT_EQUAL(expected, actual) do { \
    int _e = (int)(expected); \
    int _a = (int)(actual); \
    if (_e != _a) { \
        printf("  FAIL %s:%d: expected %d, got %d\n", \
               _current_test, __LINE__, _e, _a); \
        _tests_failed++; \
        return; \
    } \
} while(0)

#define TEST_ASSERT_EQUAL_STRING(expected, actual) do { \
    const char *_e = (expected); \
    const char *_a = (actual); \
    if (!_e || !_a || strcmp(_e, _a) != 0) { \
        printf("  FAIL %s:%d: expected '%s', got '%s'\n", \
               _current_test, __LINE__, _e ? _e : "NULL", _a ? _a : "NULL"); \
        _tests_failed++; \
        return; \
    } \
} while(0)

#define TEST_ASSERT_TRUE(condition) do { \
    if (!(condition)) { \
        printf("  FAIL %s:%d: expected TRUE, got FALSE\n", \
               _current_test, __LINE__); \
        _tests_failed++; \
        return; \
    } \
} while(0)

#define TEST_ASSERT_FALSE(condition) do { \
    if (condition) { \
        printf("  FAIL %s:%d: expected FALSE, got TRUE\n", \
               _current_test, __LINE__); \
        _tests_failed++; \
        return; \
    } \
} while(0)

#define TEST_ASSERT_NULL(ptr) do { \
    if ((ptr) != NULL) { \
        printf("  FAIL %s:%d: expected NULL\n", _current_test, __LINE__); \
        _tests_failed++; \
        return; \
    } \
} while(0)

#define TEST_ASSERT_NOT_NULL(ptr) do { \
    if ((ptr) == NULL) { \
        printf("  FAIL %s:%d: expected non-NULL\n", _current_test, __LINE__); \
        _tests_failed++; \
        return; \
    } \
} while(0)

static inline int test_summary(void)
{
    printf("\n=== %d tests run, %d failed ===\n", _tests_run, _tests_failed);
    return _tests_failed > 0 ? 1 : 0;
}

#endif /* TEST_UTILS_H */
