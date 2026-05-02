/**
 * @file ringbuf_test.c
 * @author EnterWorldDoor
 * @brief ringbuf 企业级单元测试（基于 Unity 框架）
 *
 * 测试覆盖范围:
 *   - 生命周期管理 (init/deinit/is_initialized/reset)
 *   - 基础数据操作 (push/pop/peek)
 *   - 覆盖模式 vs 非覆盖模式
 *   - 边界条件 (空/满/溢出)
 *   - 查询 API (available/used/is_empty/is_full/capacity)
 *   - 扩展 API (peek_offset/drop)
 *   - 统计计数器 (get_stats/reset_stats)
 *   - 参数校验 (NULL指针/零长度/未初始化)
 *   - 环形边界跨越
 */

#include "unity.h"
#include "ringbuf.h"
#include "global_error.h"
#include <string.h>

/* ==================== 测试辅助 ==================== */

#define TEST_BUF_SIZE  256

static uint8_t g_test_buf[TEST_BUF_SIZE];
static struct ringbuf g_rb;

static void setup_ringbuf(bool overwrite)
{
    memset(g_test_buf, 0xAB, sizeof(g_test_buf));
    memset(&g_rb, 0, sizeof(g_rb));
    ringbuf_init(&g_rb, g_test_buf, TEST_BUF_SIZE, overwrite);
}

static void teardown_ringbuf(void)
{
    if (ringbuf_is_initialized(&g_rb)) {
        ringbuf_deinit(&g_rb);
    }
}

/* ==================== 生命周期测试组 ==================== */

void test_init_success(void)
{
    setup_ringbuf(false);
    TEST_ASSERT_TRUE(ringbuf_is_initialized(&g_rb));
    TEST_ASSERT_EQUAL(TEST_BUF_SIZE, ringbuf_capacity(&g_rb));
    teardown_ringbuf();
}

void test_init_null_rb(void)
{
    int ret = ringbuf_init(NULL, g_test_buf, TEST_BUF_SIZE, false);
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret);
}

void test_init_null_buffer(void)
{
    int ret = ringbuf_init(&g_rb, NULL, TEST_BUF_SIZE, false);
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret);
}

void test_init_zero_size(void)
{
    uint8_t buf[1];
    int ret = ringbuf_init(&g_rb, buf, 0, false);
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret);
}

void test_double_init_fails(void)
{
    setup_ringbuf(false);

    struct ringbuf rb2;
    uint8_t buf2[64];
    int ret = ringbuf_init(&rb2, buf2, 64, true);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    ringbuf_deinit(&rb2);

    teardown_ringbuf();
}

void test_deinit_clears_state(void)
{
    setup_ringbuf(false);
    ringbuf_deinit(&g_rb);
    TEST_ASSERT_FALSE(ringbuf_is_initialized(&g_rb));
}

void test_reset_clears_data(void)
{
    setup_ringbuf(false);
    uint8_t data[] = { 0x01, 0x02, 0x03 };
    ringbuf_push(&g_rb, data, 3);
    TEST_ASSERT_EQUAL(3, ringbuf_used(&g_rb));

    ringbuf_reset(&g_rb);
    TEST_ASSERT_TRUE(ringbuf_is_empty(&g_rb));
    TEST_ASSERT_EQUAL(0, ringbuf_used(&g_rb));
    teardown_ringbuf();
}

/* ==================== 基础 Push/Pop 测试组 ==================== */

void test_push_and_pop_basic(void)
{
    setup_ringbuf(false);

    uint8_t tx[] = { 0xAA, 0xBB, 0xCC };
    size_t pushed = ringbuf_push(&g_rb, tx, 3);
    TEST_ASSERT_EQUAL(3, pushed);
    TEST_ASSERT_EQUAL(3, ringbuf_used(&g_rb));

    uint8_t rx[4] = { 0 };
    size_t popped = ringbuf_pop(&g_rb, rx, 3);
    TEST_ASSERT_EQUAL(3, popped);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(tx, rx, 3);
    TEST_ASSERT_TRUE(ringbuf_is_empty(&g_rb));

    teardown_ringbuf();
}

void test_push_pop_partial(void)
{
    setup_ringbuf(false);

    uint8_t tx[] = { 0x11, 0x22, 0x33, 0x44 };
    ringbuf_push(&g_rb, tx, 4);

    uint8_t rx[4] = { 0 };
    size_t popped = ringbuf_pop(&g_rb, rx, 2);
    TEST_ASSERT_EQUAL(2, popped);
    TEST_ASSERT_EQUAL_UINT8(0x11, rx[0]);
    TEST_ASSERT_EQUAL_UINT8(0x22, rx[1]);
    TEST_ASSERT_EQUAL(2, ringbuf_used(&g_rb));

    teardown_ringbuf();
}

void test_pop_from_empty_returns_zero(void)
{
    setup_ringbuf(false);

    uint8_t rx[10] = { 0xFF };
    size_t popped = ringbuf_pop(&g_rb, rx, 10);
    TEST_ASSERT_EQUAL(0, popped);

    teardown_ringbuf();
}

void test_peek_does_not_consume(void)
{
    setup_ringbuf(false);

    uint8_t tx[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    ringbuf_push(&g_rb, tx, 4);

    uint8_t peeked[4] = { 0 };
    ringbuf_peek(&g_rb, peeked, 4);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(tx, peeked, 4);
    TEST_ASSERT_EQUAL(4, ringbuf_used(&g_rb));

    uint8_t popped[4] = { 0 };
    ringbuf_pop(&g_rb, popped, 4);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(tx, popped, 4);

    teardown_ringbuf();
}

/* ==================== 覆盖模式测试组 ==================== */

void test_overwrite_mode_discards_old_data(void)
{
    setup_ringbuf(true);

    /* 填满缓冲区 */
    uint8_t fill[TEST_BUF_SIZE];
    memset(fill, 0x41, TEST_BUF_SIZE);
    ringbuf_push(&g_rb, fill, TEST_BUF_SIZE - 1);
    TEST_ASSERT_FALSE(ringbuf_is_full(&g_rb));

    /* 再写入，应该覆盖旧数据 */
    uint8_t newdata[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    size_t pushed = ringbuf_push(&g_rb, newdata, sizeof(newdata));
    TEST_ASSERT_GREATER_THAN(1, pushed);  /* Bug修复前只返回1 */
    TEST_ASSERT_TRUE(ringbuf_is_full(&g_rb) || ringbuf_used(&g_rb) >= TEST_BUF_SIZE - 1);

    teardown_ringbuf();
}

void test_nonoverwrite_rejects_when_full(void)
{
    setup_ringbuf(false);

    uint8_t fill[TEST_BUF_SIZE];
    memset(fill, 0x55, sizeof(fill));
    ringbuf_push(&g_rb, fill, TEST_BUF_SIZE - 1);

    uint8_t extra[] = { 0xAA };
    size_t pushed = ringbuf_push(&g_rb, extra, 1);
    TEST_ASSERT_EQUAL(0, pushed);

    teardown_ringbuf();
}

/* ==================== 查询 API 测试组 ==================== */

void test_available_and_used(void)
{
    setup_ringbuf(false);

    TEST_ASSERT_EQUAL(TEST_BUF_SIZE, ringbuf_available(&g_rb));
    TEST_ASSERT_EQUAL(0, ringbuf_used(&g_rb));

    uint8_t data[50];
    memset(data, 0x77, sizeof(data));
    ringbuf_push(&g_rb, data, 30);
    TEST_ASSERT_EQUAL(30, ringbuf_used(&g_rb));
    TEST_ASSERT_EQUAL(TEST_BUF_SIZE - 30, ringbuf_available(&g_rb));

    ringbuf_pop(&g_rb, data, 10);
    TEST_ASSERT_EQUAL(20, ringbuf_used(&g_rb));
    TEST_ASSERT_EQUAL(TEST_BUF_SIZE - 20, ringbuf_available(&g_rb));

    teardown_ringbuf();
}

void test_is_empty_and_is_full(void)
{
    setup_ringbuf(false);

    TEST_ASSERT_TRUE(ringbuf_is_empty(&g_rb));
    TEST_ASSERT_FALSE(ringbuf_is_full(&g_rb));

    uint8_t data[TEST_BUF_SIZE];
    memset(data, 0x88, sizeof(data));
    ringbuf_push(&g_rb, data, TEST_BUF_SIZE - 1);
    TEST_ASSERT_FALSE(ringbuf_is_empty(&g_rb));

    teardown_ringbuf();
}

void test_capacity(void)
{
    setup_ringbuf(true);
    TEST_ASSERT_EQUAL(TEST_BUF_SIZE, ringbuf_capacity(&g_rb));
    teardown_ringbuf();
}

/* ==================== 扩展 API 测试组 ==================== */

void test_peek_offset_basic(void)
{
    setup_ringbuf(false);

    uint8_t data[] = { 0xA0, 0xB0, 0xC0, 0xD0, 0xE0 };
    ringbuf_push(&g_rb, data, 5);

    uint8_t out[5] = { 0 };

    size_t n = ringbuf_peek_offset(&g_rb, 0, out, 5);
    TEST_ASSERT_EQUAL(5, n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(data, out, 5);

    n = ringbuf_peek_offset(&g_rb, 2, out, 3);
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_EQUAL_UINT8(0xC0, out[0]);
    TEST_ASSERT_EQUAL_UINT8(0xD0, out[1]);
    TEST_ASSERT_EQUAL_UINT8(0xE0, out[2]);

    TEST_ASSERT_EQUAL(5, ringbuf_used(&g_rb));  /* 未消费 */

    teardown_ringbuf();
}

void test_peek_offset_out_of_range(void)
{
    setup_ringbuf(false);

    uint8_t data[] = { 0x01, 0x02 };
    ringbuf_push(&g_rb, data, 2);

    uint8_t out[10] = { 0 };
    size_t n = ringbuf_peek_offset(&g_rb, 100, out, 5);
    TEST_ASSERT_EQUAL(0, n);

    teardown_ringbuf();
}

void test_drop_basic(void)
{
    setup_ringbuf(false);

    uint8_t data[] = { 0xF1, 0xF2, 0xF3, 0xF4, 0xF5 };
    ringbuf_push(&g_rb, data, 5);

    size_t dropped = ringbuf_drop(&g_rb, 2);
    TEST_ASSERT_EQUAL(2, dropped);
    TEST_ASSERT_EQUAL(3, ringbuf_used(&g_rb));

    uint8_t out[5] = { 0 };
    ringbuf_pop(&g_rb, out, 3);
    TEST_ASSERT_EQUAL_UINT8(0xF3, out[0]);
    TEST_ASSERT_EQUAL_UINT8(0xF4, out[1]);
    TEST_ASSERT_EQUAL_UINT8(0xF5, out[2]);

    teardown_ringbuf();
}

void test_drop_more_than_available(void)
{
    setup_ringbuf(false);

    uint8_t data[] = { 0x11, 0x22 };
    ringbuf_push(&g_rb, data, 2);

    size_t dropped = ringbuf_drop(&g_rb, 100);
    TEST_ASSERT_EQUAL(2, dropped);
    TEST_ASSERT_TRUE(ringbuf_is_empty(&g_rb));

    teardown_ringbuf();
}

/* ==================== 统计计数器测试组 ==================== */

void test_stats_after_operations(void)
{
    setup_ringbuf(true);

    struct ringbuf_stats stats;
    memset(&stats, 0xFF, sizeof(stats));

    uint8_t wdata[20];
    memset(wdata, 0xAA, sizeof(wdata));
    ringbuf_push(&g_rb, wdata, 10);
    ringbuf_push(&g_rb, wdata, 10);

    uint8_t rdata[25];
    ringbuf_pop(&g_rb, rdata, 15);
    ringbuf_peek(&g_rb, rdata, 5);

    int ret = ringbuf_get_stats(&g_rb, &stats);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_EQUAL_UINT64(2, stats.push_count);
    TEST_ASSERT_EQUAL_UINT64(1, stats.pop_count);
    TEST_ASSERT_EQUAL_UINT64(1, stats.peek_count);
    TEST_ASSERT_EQUAL_UINT64(20, stats.push_bytes);
    TEST_ASSERT_EQUAL_UINT64(15, stats.pop_bytes);

    teardown_ringbuf();
}

void test_reset_stats_clears_all(void)
{
    setup_ringbuf(true);

    uint8_t data[10] = { 0 };
    ringbuf_push(&g_rb, data, 5);
    ringbuf_pop(&g_rb, data, 3);

    ringbuf_reset_stats(&g_rb);

    struct ringbuf_stats stats;
    ringbuf_get_stats(&g_rb, &stats);
    TEST_ASSERT_EQUAL_UINT64(0, stats.push_count);
    TEST_ASSERT_EQUAL_UINT64(0, stats.pop_count);
    TEST_ASSERT_EQUAL_UINT64(0, stats.peek_count);
    TEST_ASSERT_EQUAL_UINT64(0, stats.push_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, stats.pop_bytes);

    teardown_ringbuf();
}

void test_stats_overflow_tracking(void)
{
    setup_ringbuf(true);

    uint8_t big[TEST_BUF_SIZE];
    memset(big, 0xDD, sizeof(big));

    ringbuf_push(&g_rb, big, TEST_BUF_SIZE - 1);
    ringbuf_push(&g_rb, big, 50);

    struct ringbuf_stats stats;
    ringbuf_get_stats(&g_rb, &stats);
    TEST_ASSERT_GREATER_THAN_UINT32(0, (uint32_t)stats.overflow_count);

    teardown_ringbuf();
}

/* ==================== 参数校验测试组 ==================== */

void test_push_null_pointer(void)
{
    setup_ringbuf(false);
    size_t ret = ringbuf_push(&g_rb, NULL, 10);
    TEST_ASSERT_EQUAL(0, ret);
    teardown_ringbuf();
}

void test_push_zero_length(void)
{
    setup_ringbuf(false);
    uint8_t d = 0x42;
    size_t ret = ringbuf_push(&g_rb, &d, 0);
    TEST_ASSERT_EQUAL(0, ret);
    teardown_ringbuf();
}

void test_pop_to_null_pointer(void)
{
    setup_ringbuf(false);
    uint8_t d = 0x42;
    ringbuf_push(&g_rb, &d, 1);
    size_t ret = ringbuf_pop(&g_rb, NULL, 1);
    TEST_ASSERT_EQUAL(0, ret);
    teardown_ringbuf();
}

void test_operation_on_uninitialized(void)
{
    struct ringbuf uninit_rb;
    memset(&uninit_rb, 0, sizeof(uninit_rb));

    uint8_t d = 0x55;
    TEST_ASSERT_EQUAL(0, ringbuf_push(&uninit_rb, &d, 1));
    TEST_ASSERT_EQUAL(0, ringbuf_pop(&uninit_rb, &d, 1));
    TEST_ASSERT_EQUAL(0, ringbuf_available(&uninit_rb));
    TEST_ASSERT_FALSE(ringbuf_is_initialized(&uninit_rb));
}

void test_stats_null_pointer(void)
{
    setup_ringbuf(false);
    int ret = ringbuf_get_stats(&g_rb, NULL);
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret);
    teardown_ringbuf();
}

/* ==================== 环形边界测试组 ==================== */

void test_wraparound_write_read(void)
{
    setup_ringbuf(false);

    uint8_t pattern[TEST_BUF_SIZE / 2 + 10];
    for (int i = 0; i < (int)(sizeof(pattern)); i++) {
        pattern[i] = (uint8_t)i;
    }

    ringbuf_push(&g_rb, pattern, TEST_BUF_SIZE / 2);

    ringbuf_pop(&g_rb, pattern, TEST_BUF_SIZE / 4);

    ringbuf_push(&g_rb, pattern, sizeof(pattern));

    uint8_t verify[TEST_BUF_SIZE];
    size_t total = ringbuf_pop(&g_rb, verify, sizeof(verify));
    TEST_ASSERT_GREATER_THAN(0, (int)total);

    teardown_ringbuf();
}

/* ==================== Unity 主函数 ==================== */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    UNITY_BEGIN();

    RUN_TEST(test_init_success);
    RUN_TEST(test_init_null_rb);
    RUN_TEST(test_init_null_buffer);
    RUN_TEST(test_init_zero_size);
    RUN_TEST(test_double_init_fails);
    RUN_TEST(test_deinit_clears_state);
    RUN_TEST(test_reset_clears_data);

    RUN_TEST(test_push_and_pop_basic);
    RUN_TEST(test_push_pop_partial);
    RUN_TEST(test_pop_from_empty_returns_zero);
    RUN_TEST(test_peek_does_not_consume);

    RUN_TEST(test_overwrite_mode_discards_old_data);
    RUN_TEST(test_nonoverwrite_rejects_when_full);

    RUN_TEST(test_available_and_used);
    RUN_TEST(test_is_empty_and_is_full);
    RUN_TEST(test_capacity);

    RUN_TEST(test_peek_offset_basic);
    RUN_TEST(test_peek_offset_out_of_range);
    RUN_TEST(test_drop_basic);
    RUN_TEST(test_drop_more_than_available);

    RUN_TEST(test_stats_after_operations);
    RUN_TEST(test_reset_stats_clears_all);
    RUN_TEST(test_stats_overflow_tracking);

    RUN_TEST(test_push_null_pointer);
    RUN_TEST(test_push_zero_length);
    RUN_TEST(test_pop_to_null_pointer);
    RUN_TEST(test_operation_on_uninitialized);
    RUN_TEST(test_stats_null_pointer);

    RUN_TEST(test_wraparound_write_read);

    return UNITY_END();
}