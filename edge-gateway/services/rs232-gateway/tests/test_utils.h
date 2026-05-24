/**
 * @file test_utils.h
 * @brief RS232 Gateway — 最小单元测试框架
 *
 * 提供类似 Unity 的断言宏，无需外部依赖。
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

#define TEST_ASSERT_EQUAL_UINT8(expected, actual) do { \
    unsigned _e = (unsigned)(expected); \
    unsigned _a = (unsigned)(actual); \
    if (_e != _a) { \
        printf("  FAIL %s:%d: expected %u, got %u\n", \
               _current_test, __LINE__, _e, _a); \
        _tests_failed++; \
        return; \
    } \
} while(0)

#define TEST_ASSERT_EQUAL_UINT16(expected, actual) do { \
    unsigned _e = (unsigned)(expected); \
    unsigned _a = (unsigned)(actual); \
    if (_e != _a) { \
        printf("  FAIL %s:%d: expected %u, got %u\n", \
               _current_test, __LINE__, _e, _a); \
        _tests_failed++; \
        return; \
    } \
} while(0)

#define TEST_ASSERT_EQUAL_UINT32(expected, actual) do { \
    uint32_t _e = (uint32_t)(expected); \
    uint32_t _a = (uint32_t)(actual); \
    if (_e != _a) { \
        printf("  FAIL %s:%d: expected %u, got %u\n", \
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
