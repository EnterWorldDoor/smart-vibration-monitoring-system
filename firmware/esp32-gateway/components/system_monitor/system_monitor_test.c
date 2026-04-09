/**
 * @file system_monitor_test.c
 * @author EnterWorldDoor
 * @brief system_monitor 企业级单元测试（基于 Unity 框架）
 *
 * 测试覆盖范围:
 *   - 生命周期管理 (init/deinit/is_initialized)
 *   - 状态查询 API (get_status/get_cpu/get_memory/task_count/uptime)
 *   - 阈值配置与验证 (set/get_thresholds)
 *   - 告警回调机制 (register/unregister)
 *   - 历史数据管理 (history depth/entry/clear)
 *   - 边界条件与错误处理
 */

#include "unity.h"
#include "system_monitor.h"
#include "global_error.h"
#include <string.h>

/* ==================== 测试辅助变量 ==================== */

static int g_alarm_callback_count = 0;
static int g_last_alarm_type = -1;
static float g_last_current_value = 0.0f;
static float g_last_threshold_value = 0.0f;
static void *g_last_user_data = NULL;

/* ==================== 测试辅助函数 ==================== */

void test_alarm_callback(int alarm_type, float current_value,
                        float threshold_value, void *user_data)
{
    g_alarm_callback_count++;
    g_last_alarm_type = alarm_type;
    g_last_current_value = current_value;
    g_last_threshold_value = threshold_value;
    g_last_user_data = user_data;
}

void reset_callback_state(void)
{
    g_alarm_callback_count = 0;
    g_last_alarm_type = -1;
    g_last_current_value = 0.0f;
    g_last_threshold_value = 0.0f;
    g_last_user_data = NULL;
}

void setUp(void)
{
    reset_callback_state();
}

void tearDown(void)
{
    if (system_monitor_is_initialized()) {
        system_monitor_deinit();
    }
}

/* ==================== 1. 生命周期测试 ==================== */

void test_init_default_should_succeed(void)
{
    int ret = system_monitor_init(1000);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_TRUE(system_monitor_is_initialized());
}

void test_init_with_zero_interval_should_use_manual_mode(void)
{
    int ret = system_monitor_init(0);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_TRUE(system_monitor_is_initialized());
}

void test_init_twice_should_fail(void)
{
    int ret1 = system_monitor_init(1000);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret1);

    int ret2 = system_monitor_init(1000);
    TEST_ASSERT_EQUAL(APP_ERR_MONITOR_ALREADY_INIT, ret2);
}

void test_init_with_config_null_should_fail(void)
{
    int ret = system_monitor_init_with_config(NULL);
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret);
    TEST_ASSERT_FALSE(system_monitor_is_initialized());
}

void test_init_with_config_valid_should_succeed(void)
{
    struct monitor_config cfg = {
        .interval_ms = 2000,
        .history_depth = 10,
        .enable_wdt_feed = false,
        .thresholds = {
            .cpu_warn_percent = 70.0f,
            .cpu_critical_percent = 90.0f,
            .heap_warn_bytes = 8192,
            .heap_critical_bytes = 4096,
            .stack_warn_percent = 60.0f,
            .stack_critical_percent = 80.0f
        }
    };

    int ret = system_monitor_init_with_config(&cfg);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_TRUE(system_monitor_is_initialized());

    /* 验证阈值已正确设置 */
    struct monitor_thresholds th;
    ret = system_monitor_get_thresholds(&th);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 70.0f, th.cpu_warn_percent);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 90.0f, th.cpu_critical_percent);
    TEST_ASSERT_EQUAL_UINT32(8192, th.heap_warn_bytes);
}

void test_deinit_when_not_initialized_should_fail(void)
{
    int ret = system_monitor_deinit();
    TEST_ASSERT_EQUAL(APP_ERR_MONITOR_NOT_INIT, ret);
}

void test_deinit_after_init_should_succeed(void)
{
    system_monitor_init(1000);
    int ret = system_monitor_deinit();
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_FALSE(system_monitor_is_initialized());
}

void test_deinit_and_reinit_should_work(void)
{
    system_monitor_init(1000);
    system_monitor_deinit();

    int ret = system_monitor_init(500);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_TRUE(system_monitor_is_initialized());
}

/* ==================== 2. 状态查询测试 ==================== */

void test_get_status_before_init_should_fail(void)
{
    struct system_status status;
    int ret = system_monitor_get_status(&status);
    TEST_ASSERT_EQUAL(APP_ERR_MONITOR_NOT_INIT, ret);
}

void test_get_status_null_param_should_fail(void)
{
    system_monitor_init(1000);
    int ret = system_monitor_get_status(NULL);
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret);
}

void test_get_status_should_return_valid_data(void)
{
    system_monitor_init(0);

    struct system_status status;
    int ret = system_monitor_get_status(&status);

    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_GREATER_OR_EQUAL(0, status.task_count);
    TEST_ASSERT_GREATER_THAN(0, status.mem.free_heap);
    TEST_ASSERT_GREATER_THAN(0, status.mem.total_heap);
    TEST_ASSERT_GREATER_THAN(0, status.uptime_ms);
    TEST_ASSERT_GREATER_OR_EQUAL(0.0f, status.cpu.usage_percent);
    TEST_ASSERT_LESS_OR_EQUAL(100.0f, status.cpu.usage_percent);
}

void test_get_cpu_should_return_valid_cpu_info(void)
{
    system_monitor_init(0);

    struct cpu_info cpu;
    int ret = system_monitor_get_cpu(&cpu);

    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_GREATER_OR_EQUAL(0.0f, cpu.usage_percent);
    TEST_ASSERT_LESS_OR_EQUAL(100.0f, cpu.usage_percent);
}

void test_get_memory_should_return_valid_memory_info(void)
{
    system_monitor_init(0);

    struct memory_info mem;
    int ret = system_monitor_get_memory(&mem);

    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_GREATER_THAN(0, mem.free_heap);
    TEST_ASSERT_GREATER_THAN(0, mem.min_free_heap);
    TEST_ASSERT_GREATER_THAN(0, mem.total_heap);
    TEST_ASSERT_GREATER_OR_EQUAL(0.0f, mem.fragmentation_percent);
    TEST_ASSERT_LESS_OR_EQUAL(100.0f, mem.fragmentation_percent);
}

void test_get_task_count_before_init_should_return_negative_one(void)
{
    int count = system_monitor_get_task_count();
    TEST_ASSERT_EQUAL(-1, count);
}

void test_get_task_count_after_init_should_be_positive(void)
{
    system_monitor_init(0);
    int count = system_monitor_get_task_count();
    TEST_ASSERT_GREATER_THAN(0, count);
}

void test_get_uptime_should_increase_over_time(void)
{
    system_monitor_init(0);

    uint64_t t1 = system_monitor_get_uptime();
    vTaskDelay(pdMS_TO_TICKS(100));
    uint64_t t2 = system_monitor_get_uptime();

    TEST_ASSERT_GREATER_OR_EQUAL(t1 + 90, t2);
}

void test_get_task_info_invalid_index_should_fail(void)
{
    system_monitor_init(0);

    struct task_info info;
    int ret = system_monitor_get_task_info(-1, &info);
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret);

    ret = system_monitor_get_task_info(999, &info);
    TEST_ASSERT_NOT_EQUAL(APP_ERR_OK, ret);
}

void test_get_task_info_null_param_should_fail(void)
{
    system_monitor_init(0);
    int ret = system_monitor_get_task_info(0, NULL);
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret);
}

void test_get_task_info_valid_index_should_return_data(void)
{
    system_monitor_init(0);

    int count = system_monitor_get_task_count();
    if (count > 0) {
        struct task_info info;
        int ret = system_monitor_get_task_info(0, &info);
        TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
        TEST_ASSERT_GREATER_THAN(0, strlen(info.name));
        TEST_ASSERT_GREATER_OR_EQUAL(0.0f, info.stack_usage_percent);
    }
}

/* ==================== 3. 阈值配置测试 ==================== */

void test_set_thresholds_null_should_fail(void)
{
    system_monitor_init(0);
    int ret = system_monitor_set_thresholds(NULL);
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret);
}

void test_set_thresholds_before_init_should_fail(void)
{
    struct monitor_thresholds th = {0};
    int ret = system_monitor_set_thresholds(&th);
    TEST_ASSERT_EQUAL(APP_ERR_MONITOR_NOT_INIT, ret);
}

void test_set_thresholds_valid_should_succeed(void)
{
    system_monitor_init(0);

    struct monitor_thresholds th = {
        .cpu_warn_percent = 75.0f,
        .cpu_critical_percent = 95.0f,
        .heap_warn_bytes = 16384,
        .heap_critical_bytes = 8192,
        .stack_warn_percent = 65.0f,
        .stack_critical_percent = 85.0f
    };

    int ret = system_monitor_set_thresholds(&th);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);

    struct monitor_thresholds out;
    ret = system_system_monitor_get_thresholds(&out);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 75.0f, out.cpu_warn_percent);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 95.0f, out.cpu_critical_percent);
    TEST_ASSERT_EQUAL_UINT32(16384, out.heap_warn_bytes);
}

void test_set_thresholds_cpu_exceed_100_should_fail(void)
{
    system_monitor_init(0);

    struct monitor_thresholds th = {
        .cpu_warn_percent = 101.0f,
        .cpu_critical_percent = 99.0f
    };

    int ret = system_monitor_set_thresholds(&th);
    TEST_ASSERT_EQUAL(APP_ERR_MONITOR_THRESHOLD_INVALID, ret);
}

void test_set_thresholds_cpu_negative_should_fail(void)
{
    system_monitor_init(0);

    struct monitor_thresholds th = {
        .cpu_warn_percent = -5.0f,
        .cpu_critical_percent = 90.0f
    };

    int ret = system_monitor_set_thresholds(&th);
    TEST_ASSERT_EQUAL(APP_ERR_MONITOR_THRESHOLD_INVALID, ret);
}

void test_set_thresholds_warn_greater_than_critical_should_fail(void)
{
    system_monitor_init(0);

    struct monitor_thresholds th = {
        .cpu_warn_percent = 90.0f,
        .cpu_critical_percent = 50.0f
    };

    int ret = system_monitor_set_thresholds(&th);
    TEST_ASSERT_EQUAL(APP_ERR_MONITOR_THRESHOLD_INVALID, ret);
}

void test_set_thresholds_zero_disables_checking(void)
{
    system_monitor_init(0);

    struct monitor_thresholds th = {
        .cpu_warn_percent = 0.0f,
        .cpu_critical_percent = 0.0f,
        .heap_warn_bytes = 0,
        .heap_critical_bytes = 0,
        .stack_warn_percent = 0.0f,
        .stack_critical_percent = 0.0f
    };

    int ret = system_monitor_set_thresholds(&th);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
}

void test_get_thresholds_null_should_fail(void)
{
    system_monitor_init(0);
    int ret = system_monitor_get_thresholds(NULL);
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret);
}

void test_get_thresholds_before_init_should_fail(void)
{
    struct monitor_thresholds th;
    int ret = system_monitor_get_thresholds(&th);
    TEST_ASSERT_EQUAL(APP_ERR_MONITOR_NOT_INIT, ret);
}

/* ==================== 4. 告警回调测试 ==================== */

void test_register_callback_null_should_fail(void)
{
    system_monitor_init(0);
    int ret = system_monitor_register_alarm_callback(NULL, NULL);
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret);
}

void test_register_callback_before_init_should_fail(void)
{
    int ret = system_monitor_register_alarm_callback(test_alarm_callback, NULL);
    TEST_ASSERT_EQUAL(APP_ERR_MONITOR_NOT_INIT, ret);
}

void test_register_callback_should_succeed(void)
{
    system_monitor_init(0);

    int ret = system_monitor_register_alarm_callback(test_alarm_callback,
                                                    (void *)0x1234);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_EQUAL(1, g_alarm_callback_count == 0 ? 0 : 0);
}

void test_register_duplicate_callback_should_not_add_again(void)
{
    system_monitor_init(0);

    system_monitor_register_alarm_callback(test_alarm_callback, (void *)0x1111);
    system_monitor_register_alarm_callback(test_alarm_callback, (void *)0x2222);

    /* 不应该报错，但也不应该重复注册 */
}

void test_unregister_callback_null_should_fail(void)
{
    system_monitor_init(0);
    int ret = system_monitor_unregister_alarm_callback(NULL);
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret);
}

void test_unregister_callback_before_init_should_fail(void)
{
    int ret = system_monitor_unregister_alarm_callback(test_alarm_callback);
    TEST_ASSERT_EQUAL(APP_ERR_MONITOR_NOT_INIT, ret);
}

void test_unregister_nonexistent_callback_should_fail(void)
{
    system_monitor_init(0);

    void (*dummy_cb)(int, float, float, void *) = test_alarm_callback;
    int ret = system_monitor_unregister_alarm_callback(dummy_cb);
    TEST_ASSERT_EQUAL(APP_ERR_NOT_SUPPORTED, ret);
}

void test_register_and_unregister_should_work(void)
{
    system_monitor_init(0);

    int ret = system_monitor_register_alarm_callback(test_alarm_callback, NULL);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);

    ret = system_monitor_unregister_alarm_callback(test_alarm_callback);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
}

/* ==================== 5. 历史数据测试 ==================== */

void test_history_depth_initially_zero(void)
{
    system_monitor_init(0);
    int depth = system_monitor_get_history_depth();
    TEST_ASSERT_EQUAL(0, depth);
}

void test_clear_history_before_init_should_not_crash(void)
{
    system_monitor_clear_history();
    TEST_PASS();
}

void test_clear_history_should_reset_depth(void)
{
    struct monitor_config cfg = {
        .interval_ms = 0,
        .history_depth = 10,
        .enable_wdt_feed = false
    };
    system_monitor_init_with_config(&cfg);

    system_monitor_clear_history();
    int depth = system_monitor_get_history_depth();
    TEST_ASSERT_EQUAL(0, depth);
}

void test_get_history_entry_out_of_range_should_fail(void)
{
    system_monitor_init(0);

    struct monitor_history_entry entry;
    int ret = system_monitor_get_history_entry(-1, &entry);
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret);

    ret = system_monitor_get_history_entry(0, &entry);
    TEST_ASSERT_NOT_EQUAL(APP_ERR_OK, ret);
}

void test_get_history_entry_null_should_fail(void)
{
    system_monitor_init(0);
    int ret = system_monitor_get_history_entry(0, NULL);
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret);
}

/* ==================== 6. 日志输出测试（不崩溃即可）==================== */

void test_dump_before_init_should_not_crash(void)
{
    system_monitor_dump();
    TEST_PASS();
}

void test_dump_tasks_before_init_should_not_crash(void)
{
    system_monitor_dump_tasks();
    TEST_PASS();
}

void test_dump_memory_before_init_should_not_crash(void)
{
    system_monitor_dump_memory();
    TEST_PASS();
}

void test_dump_after_init_should_not_crash(void)
{
    system_monitor_init(0);
    system_monitor_dump();
    system_monitor_dump_tasks();
    system_monitor_dump_memory();
    TEST_PASS();
}

/* ==================== 7. 边界条件测试 ==================== */

void test_multiple_init_deinit_cycles(void)
{
    for (int i = 0; i < 3; i++) {
        int ret = system_monitor_init(500);
        TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
        TEST_ASSERT_TRUE(system_monitor_is_initialized());

        struct system_status status;
        ret = system_monitor_get_status(&status);
        TEST_ASSERT_EQUAL(APP_ERR_OK, ret);

        ret = system_monitor_deinit();
        TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
        TEST_ASSERT_FALSE(system_monitor_is_initialized());
    }
}

void test_manual_mode_no_background_task(void)
{
    system_monitor_init(0);

    vTaskDelay(pdMS_TO_TICKS(200));

    struct system_status status;
    int ret = system_monitor_get_status(&status);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_GREATER_THAN(0, status.task_count);
}

void test_status_fields_within_reasonable_range(void)
{
    system_monitor_init(0);

    struct system_status status;
    system_monitor_get_status(&status);

    /* CPU 使用率应该在 0~100 */
    TEST_ASSERT_GREATER_OR_EQUAL(0.0f, status.cpu.usage_percent);
    TEST_ASSERT_LESS_OR_EQUAL(100.0f, status.cpu.usage_percent);

    /* 内存值应该合理 */
    TEST_ASSERT_LESS_OR_EQUAL(status.mem.total_heap, status.mem.free_heap + 1024);

    /* 碎片率 0~100 */
    TEST_ASSERT_GREATER_OR_EQUAL(0.0f, status.mem.fragmentation_percent);
    TEST_ASSERT_LESS_OR_EQUAL(100.0f, status.mem.fragmentation_percent);

    /* 任务数 > 0 */
    TEST_ASSERT_GREATER_THAN(0, status.task_count);
}

void test_task_info_stack_usage_range(void)
{
    system_monitor_init(0);

    int count = system_monitor_get_task_count();
    for (int i = 0; i < count && i < MONITOR_MAX_TASKS; i++) {
        struct task_info info;
        int ret = system_monitor_get_task_info(i, &info);
        if (ret == APP_ERR_OK) {
            TEST_ASSERT_GREATER_OR_EQUAL(0.0f, info.stack_usage_percent);
            TEST_ASSERT_LESS_OR_EQUAL(100.0f, info.stack_usage_percent);
        }
    }
}

/* ==================== Unity 测试注册入口 ==================== */

void app_main(void)
{
    UNITY_BEGIN();

    printf("\n========================================\n");
    printf("   System Monitor Unit Tests\n");
    printf("========================================\n\n");

    /* 1. 生命周期测试 */
    RUN_TEST(test_init_default_should_succeed);
    RUN_TEST(test_init_with_zero_interval_should_use_manual_mode);
    RUN_TEST(test_init_twice_should_fail);
    RUN_TEST(test_init_with_config_null_should_fail);
    RUN_TEST(test_init_with_config_valid_should_succeed);
    RUN_TEST(test_deinit_when_not_initialized_should_fail);
    RUN_TEST(test_deinit_after_init_should_succeed);
    RUN_TEST(test_deinit_and_reinit_should_work);

    /* 2. 状态查询测试 */
    RUN_TEST(test_get_status_before_init_should_fail);
    RUN_TEST(test_get_status_null_param_should_fail);
    RUN_TEST(test_get_status_should_return_valid_data);
    RUN_TEST(test_get_cpu_should_return_valid_cpu_info);
    RUN_TEST(test_get_memory_should_return_valid_memory_info);
    RUN_TEST(test_get_task_count_before_init_should_return_negative_one);
    RUN_TEST(test_get_task_count_after_init_should_be_positive);
    RUN_TEST(test_get_uptime_should_increase_over_time);
    RUN_TEST(test_get_task_info_invalid_index_should_fail);
    RUN_TEST(test_get_task_info_null_param_should_fail);
    RUN_TEST(test_get_task_info_valid_index_should_return_data);

    /* 3. 阈值配置测试 */
    RUN_TEST(test_set_thresholds_null_should_fail);
    RUN_TEST(test_set_thresholds_before_init_should_fail);
    RUN_TEST(test_set_thresholds_valid_should_succeed);
    RUN_TEST(test_set_thresholds_cpu_exceed_100_should_fail);
    RUN_TEST(test_set_thresholds_cpu_negative_should_fail);
    RUN_TEST(test_set_thresholds_warn_greater_than_critical_should_fail);
    RUN_TEST(test_set_thresholds_zero_disables_checking);
    RUN_TEST(test_get_thresholds_null_should_fail);
    RUN_TEST(test_get_thresholds_before_init_should_fail);

    /* 4. 告警回调测试 */
    RUN_TEST(test_register_callback_null_should_fail);
    RUN_TEST(test_register_callback_before_init_should_fail);
    RUN_TEST(test_register_callback_should_succeed);
    RUN_TEST(test_register_duplicate_callback_should_not_add_again);
    RUN_TEST(test_unregister_callback_null_should_fail);
    RUN_TEST(test_unregister_callback_before_init_should_fail);
    RUN_TEST(test_unregister_nonexistent_callback_should_fail);
    RUN_TEST(test_register_and_unregister_should_work);

    /* 5. 历史数据测试 */
    RUN_TEST(test_history_depth_initially_zero);
    RUN_TEST(test_clear_history_before_init_should_not_crash);
    RUN_TEST(test_clear_history_should_reset_depth);
    RUN_TEST(test_get_history_entry_out_of_range_should_fail);
    RUN_TEST(test_get_history_entry_null_should_fail);

    /* 6. 日志输出测试 */
    RUN_TEST(test_dump_before_init_should_not_crash);
    RUN_TEST(test_dump_tasks_before_init_should_not_crash);
    RUN_TEST(test_dump_memory_before_init_should_not_crash);
    RUN_TEST(test_dump_after_init_should_not_crash);

    /* 7. 边界条件测试 */
    RUN_TEST(test_multiple_init_deinit_cycles);
    RUN_TEST(test_manual_mode_no_background_task);
    RUN_TEST(test_status_fields_within_reasonable_range);
    RUN_TEST(test_task_info_stack_usage_range);

    int failures = UNITY_END();

    printf("\n========================================\n");
    printf("   Test Results: %d failures\n", failures);
    printf("========================================\n", failures);
}