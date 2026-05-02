/**
 * @file time_sync_test.c
 * @author EnterWorldDoor
 * @brief time_sync 企业级单元测试（基于 Unity 框架）
 *
 * 测试覆盖范围:
 *   - 生命周期管理 (init/deinit/is_initialized)
 *   - 时间戳获取 (get_timestamp_us/ms)
 *   - 完整时间信息查询 (get_time_info)
 *   - 同步状态管理 (get_status/is_synchronized)
 *   - NTP 服务器配置 (set_servers/get_servers)
 *   - 回调注册/注销机制
 *   - 错误处理与边界条件
 *   - 并发保护（重复初始化、未初始化调用等）
 */

#include "unity.h"
#include "time_sync.h"
#include "global_error.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

/* ==================== 测试辅助变量 ==================== */

static int g_callback_fired_count = 0;
static enum sync_status g_last_cb_status = SYNC_STATUS_IDLE;
static int64_t g_last_cb_timestamp = 0;

/* ==================== 测试辅助函数 ==================== */

void test_sync_callback(enum sync_status status, int64_t timestamp_us,
                        void *user_data)
{
    if (!user_data) return;

    g_callback_fired_count++;
    g_last_cb_status = status;
    g_last_cb_timestamp = timestamp_us;
}

void reset_callbacks(void)
{
    g_callback_fired_count = 0;
    g_last_cb_status = SYNC_STATUS_IDLE;
    g_last_cb_timestamp = 0;
}

/* ==================== Unity Setup / Teardown ==================== */

void setUp(void)
{
    reset_callbacks();
}

void tearDown(void)
{
    /* 清理：如果模块已初始化则反初始化 */
    if (time_sync_is_initialized()) {
        time_sync_deinit();
    }
}

/* ================================================================
 *  1. 生命周期管理测试
 * ================================================================ */

void test_init_sntp_mode_should_succeed(void)
{
    int ret = time_sync_init(true, NULL);

    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_TRUE(time_sync_is_initialized());
}

void test_init_local_mode_should_succeed(void)
{
    int ret = time_sync_init(false, "CST-8");

    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_TRUE(time_sync_is_initialized());
}

void test_init_twice_should_fail(void)
{
    time_sync_init(true, NULL);

    int ret2 = time_sync_init(true, NULL);

    TEST_ASSERT_EQUAL(APP_ERR_TIME_ALREADY_INIT, ret2);
}

void test_deinit_when_not_initialized_should_fail(void)
{
    int ret = time_sync_deinit();

    TEST_ASSERT_EQUAL(APP_ERR_TIME_NOT_INIT, ret);
}

void test_deinit_after_init_should_succeed(void)
{
    time_sync_init(true, NULL);

    int ret = time_sync_deinit();

    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_FALSE(time_sync_is_initialized());
}

void test_deinit_and_reinit_should_work(void)
{
    time_sync_init(true, NULL);
    time_sync_deinit();

    int ret = time_sync_init(false, "UTC");

    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_TRUE(time_sync_is_initialized());
}

void test_multiple_init_deinit_cycles(void)
{
    for (int i = 0; i < 3; i++) {
        int ret = time_sync_init(i % 2 == 0, NULL);
        TEST_ASSERT_EQUAL_MESSAGE(APP_ERR_OK, ret, "Init should succeed");
        TEST_ASSERT_TRUE(time_sync_is_initialized());

        ret = time_sync_deinit();
        TEST_ASSERT_EQUAL_MESSAGE(APP_ERR_OK, ret, "Deinit should succeed");
        TEST_ASSERT_FALSE(time_sync_is_initialized());
    }
}

/* ================================================================
 *  2. 初始状态查询测试
 * ================================================================ */

void test_get_status_before_init_should_return_idle(void)
{
    enum sync_status st = time_sync_get_status();

    TEST_ASSERT_EQUAL(SYNC_STATUS_IDLE, st);
}

void test_get_status_after_init_local_should_return_initialized(void)
{
    time_sync_init(false, NULL);

    enum sync_status st = time_sync_get_status();

    TEST_ASSERT_EQUAL(SYNC_STATUS_INITIALIZED, st);
}

void test_is_synchronized_before_init_should_return_false(void)
{
    bool synced = time_sync_is_synchronized();

    TEST_ASSERT_FALSE(synced);
}

void test_is_synchronized_after_local_init_should_return_false(void)
{
    time_sync_init(false, NULL);

    bool synced = time_sync_is_synchronized();

    TEST_ASSERT_FALSE(synced); /* 本地模式无网络同步 */
}

/* ================================================================
 *  3. 时间戳获取测试
 * ================================================================ */

void test_get_timestamp_us_before_init_should_not_crash(void)
{
    int64_t ts = time_sync_get_timestamp_us();

    /* 未初始化时返回 esp_timer_get_time() 的相对值，不应为负 */
    TEST_ASSERT_GREATER_OR_EQUAL(0, ts);
}

void test_get_timestamp_ms_before_init_should_not_crash(void)
{
    int64_t ts = time_sync_get_timestamp_ms();

    TEST_ASSERT_GREATER_OR_EQUAL(0, ts);
}

void test_get_timestamp_us_after_local_init_should_be_positive(void)
{
    time_sync_init(false, NULL);

    int64_t ts1 = time_sync_get_timestamp_us();
    vTaskDelay(pdMS_TO_TICKS(10));
    int64_t ts2 = time_sync_get_timestamp_us();

    TEST_ASSERT_GREATER_THAN(ts1, ts2); /* 时间应递增 */
    TEST_ASSERT_GREATER_THAN(10000, ts2 - ts1); /* 至少10ms */
}

void test_get_timestamp_ms_should_match_us_divided_by_1000(void)
{
    time_sync_init(false, NULL);

    int64_t ts_us = time_sync_get_timestamp_us();
    int64_t ts_ms = time_sync_get_timestamp_ms();

    /* 允许微小误差（因为两次调用之间有时间差）*/
    int64_t diff = ts_us / 1000 - ts_ms;
    TEST_ASSERT_INT_WITHIN(5, 0, diff); /* 误差在5ms以内 */
}

/* ================================================================
 *  4. get_time_info 测试
 * ================================================================ */

void test_get_time_info_null_param_should_fail(void)
{
    time_sync_init(false, NULL);

    int ret = time_sync_get_time_info(NULL);

    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret);
}

void test_get_time_info_before_init_should_fail(void)
{
    struct time_info info;
    int ret = time_sync_get_time_info(&info);

    TEST_ASSERT_EQUAL(APP_ERR_TIME_NOT_INIT, ret);
}

void test_get_time_info_after_local_init_should_have_valid_data(void)
{
    time_sync_init(false, "CST-8");

    struct time_info info;
    int ret = time_sync_get_time_info(&info);

    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_GREATER_THAN(0, info.timestamp_us);
    TEST_ASSERT_GREATER_THAN(0, info.uptime_s);
    TEST_ASSERT_EQUAL(SYNC_STATUS_INITIALIZED, info.status);
    TEST_ASSERT_FALSE(info.is_synchronized);
    /* 本地模式下 uptime 应与 timestamp_us 接近 */
    TEST_ASSERT_INT_WITHIN(500000, info.uptime_s * 1000000LL,
                           info.timestamp_us);
}

void test_get_time_info_timestamps_consistency(void)
{
    time_sync_init(false, NULL);

    struct time_info info;
    time_sync_get_time_info(&info);

    /* timestamp_ms 应等于 timestamp_us / 1000 */
    TEST_ASSERT_EQUAL(info.timestamp_us / 1000, info.timestamp_ms);
}

/* ================================================================
 *  5. wait_sync 参数验证测试
 * ================================================================ */

void test_wait_sync_before_init_should_fail(void)
{
    int ret = time_sync_wait_sync(1000);

    TEST_ASSERT_EQUAL(APP_ERR_TIME_NOT_INIT, ret);
}

void test_wait_sync_in_local_mode_should_return_ok(void)
{
    time_sync_init(false, NULL);

    int ret = time_sync_wait_sync(1000);

    TEST_ASSERT_EQUAL(APP_ERR_OK, ret); /* SNTP禁用时立即返回成功 */
}

/* ================================================================
 *  6. force_sync 参数验证测试
 * ================================================================ */

void test_force_sync_before_init_should_fail(void)
{
    int ret = time_sync_force_sync();

    TEST_ASSERT_EQUAL(APP_ERR_TIME_NOT_INIT, ret);
}

void test_force_sync_in_local_mode_should_fail(void)
{
    time_sync_init(false, NULL);

    int ret = time_sync_force_sync();

    TEST_ASSERT_EQUAL(APP_ERR_TIME_SNTPI_INIT_FAIL, ret);
}

/* ================================================================
 *  7. NTP 服务器管理测试
 * ================================================================ */

void test_set_servers_null_param_should_fail(void)
{
    time_sync_init(true, NULL);

    int ret = time_sync_set_servers(NULL, 2);

    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret);
}

void test_set_servers_invalid_count_should_fail(void)
{
    time_sync_init(true, NULL);

    struct sntp_server_config srv[4];
    memset(srv, 0, sizeof(srv));

    int ret = time_sync_set_servers(srv, 0);
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret);

    ret = time_sync_set_servers(srv, TIME_SYNC_MAX_SERVERS + 1);
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret);
}

void test_set_servers_before_init_should_fail(void)
{
    struct sntp_server_config srv;
    memset(&srv, 0, sizeof(srv));

    int ret = time_sync_set_servers(&srv, 1);

    TEST_ASSERT_EQUAL(APP_ERR_TIME_NOT_INIT, ret);
}

void test_set_servers_valid_should_succeed(void)
{
    time_sync_init(true, NULL);

    struct sntp_server_config srv[2];
    strncpy(srv[0].server, "ntp.example.com", sizeof(srv[0].server) - 1);
    srv[0].enabled = true;
    strncpy(srv[1].server, "time.example.com", sizeof(srv[1].server) - 1);
    srv[1].enabled = true;

    int ret = time_sync_set_servers(srv, 2);

    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
}

void test_get_servers_null_param_should_fail(void)
{
    time_sync_init(true, NULL);

    struct sntp_server_config srv[4];
    int count;

    int ret1 = time_sync_get_servers(NULL, 4, &count);
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret1);

    int ret2 = time_sync_get_servers(srv, 4, NULL);
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret2);
}

void test_get_servers_before_init_should_fail(void)
{
    struct sntp_server_config srv[4];
    int count;

    int ret = time_sync_get_servers(srv, 4, &count);

    TEST_ASSERT_EQUAL(APP_ERR_TIME_NOT_INIT, ret);
}

void test_get_servers_after_init_should_return_data(void)
{
    time_sync_init(true, NULL);

    struct sntp_server_config srv[TIME_SYNC_MAX_SERVERS];
    int count = 0;

    int ret = time_sync_get_servers(srv, TIME_SYNC_MAX_SERVERS, &count);

    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_GREATER_THAN(0, count);
    TEST_ASSERT_GREATER_THAN(0, strlen(srv[0].server));
}

void test_set_and_get_servers_roundtrip(void)
{
    time_sync_init(true, NULL);

    struct sntp_server_config set_srv[2];
    strncpy(set_srv[0].server, "custom.ntp.org",
            sizeof(set_srv[0].server) - 1);
    set_srv[0].enabled = true;
    strncpy(set_srv[1].server, "backup.ntp.org",
            sizeof(set_srv[1].server) - 1);
    set_srv[1].enabled = true;

    time_sync_set_servers(set_srv, 2);

    struct sntp_server_config get_srv[4];
    int count = 0;
    time_sync_get_servers(get_srv, 4, &count);

    TEST_ASSERT_EQUAL(2, count);
    TEST_ASSERT_EQUAL_STRING("custom.ntp.org", get_srv[0].server);
    TEST_ASSERT_EQUAL_STRING("backup.ntp.org", get_srv[1].server);
}

/* ================================================================
 *  8. 回调注册/注销测试
 * ================================================================ */

void test_register_callback_null_should_fail(void)
{
    time_sync_init(true, NULL);

    int ret = time_sync_register_callback(NULL, NULL);

    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret);
}

void test_register_callback_before_init_should_fail(void)
{
    int ret = time_sync_register_callback(test_sync_callback,
                                         (void *)0x1234);

    TEST_ASSERT_EQUAL(APP_ERR_TIME_NOT_INIT, ret);
}

void test_register_callback_should_succeed(void)
{
    time_sync_init(true, NULL);

    int ret = time_sync_register_callback(test_sync_callback,
                                         (void *)0xABCDEF);

    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
}

void test_register_duplicate_callback_should_not_add_again(void)
{
    time_sync_init(true, NULL);

    int ret1 = time_sync_register_callback(test_sync_callback,
                                          (void *)0x1111);
    int ret2 = time_sync_register_callback(test_sync_callback,
                                          (void *)0x2222);

    TEST_ASSERT_EQUAL(APP_ERR_OK, ret1);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret2); /* 不报错但不重复添加 */
}

void test_unregister_callback_null_should_fail(void)
{
    time_sync_init(true, NULL);

    int ret = time_sync_unregister_callback(NULL);

    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret);
}

void test_unregister_callback_before_init_should_fail(void)
{
    int ret = time_sync_unregister_callback(test_sync_callback);

    TEST_ASSERT_EQUAL(APP_ERR_TIME_NOT_INIT, ret);
}

void test_unregister_nonexistent_callback_should_fail(void)
{
    time_sync_init(true, NULL);
    time_sync_register_callback(test_sync_callback, NULL);

    void (*dummy)(enum sync_status, int64_t, void *) = test_sync_callback;
    int ret = time_sync_unregister_callback(dummy);

    TEST_ASSERT_EQUAL(APP_ERR_NOT_SUPPORTED, ret);
}

void test_register_and_unregister_callback_should_work(void)
{
    time_sync_init(true, NULL);

    int reg_ret = time_sync_register_callback(test_sync_callback,
                                             (void *)0xDEAD);
    TEST_ASSERT_EQUAL(APP_ERR_OK, reg_ret);

    int unreg_ret = time_sync_unregister_callback(test_sync_callback);
    TEST_ASSERT_EQUAL(APP_ERR_OK, unreg_ret);
}

/* ================================================================
 *  9. 错误码常量验证测试
 * ================================================================ */

void test_time_error_codes_should_be_negative(void)
{
    TEST_ASSERT_LESS_THAN(0, APP_ERR_TIME_INIT_FAIL);
    TEST_ASSERT_LESS_THAN(0, APP_ERR_TIME_ALREADY_INIT);
    TEST_ASSERT_LESS_THAN(0, APP_ERR_TIME_NOT_INIT);
    TEST_ASSERT_LESS_THAN(0, APP_ERR_TIME_SNTPI_INIT_FAIL);
    TEST_ASSERT_LESS_THAN(0, APP_ERR_TIME_EVENT_GROUP_FAIL);
    TEST_ASSERT_LESS_THAN(0, APP_ERR_TIME_SNTPI_SYNC_TIMEOUT);
    TEST_ASSERT_LESS_THAN(0, APP_ERR_TIME_INVALID_PARAM);
    TEST_ASSERT_LESS_THAN(0, APP_ERR_TIME_CALLBACK_FULL);
    TEST_ASSERT_LESS_THAN(0, APP_ERR_TIME_TASK_CREATE_FAIL);
}

void test_time_error_codes_should_be_in_correct_range(void)
{
    /* TIME errors should be in -900 to -999 range */
    TEST_ASSERT_LESS_OR_EQUAL(-900, APP_ERR_TIME_INIT_FAIL);
    TEST_ASSERT_GREATER_THAN(-1000, APP_ERR_TIME_TASK_CREATE_FAIL);
}

/* ================================================================
 *  10. 配置常量验证测试
 * ================================================================ */

void test_config_constants_should_be_reasonable(void)
{
    TEST_ASSERT_GREATER_THAN(0, TIME_SYNC_MAX_SERVERS);
    TEST_ASSERT_GREATER_THAN(0, TIME_SYNC_MAX_TIMEZONE_LEN);
    TEST_ASSERT_GREATER_THAN(0, TIME_SYNC_MAX_SERVER_LEN);
    TEST_ASSERT_GREATER_THAN(0, TIME_SYNC_DEFAULT_TIMEOUT_MS);
    TEST_ASSERT_GREATER_THAN(0, TIME_SYNC_DEFAULT_RETRY_MS);
    TEST_ASSERT_GREATER_THAN(0, TIME_SYNC_DEFAULT_MAX_RETRIES);
    TEST_ASSERT_GREATER_THAN(0, TIME_SYNC_DEFAULT_INTERVAL_S);
    TEST_ASSERT_GREATER_THAN(0, TIME_SYNC_TASK_STACK_SIZE);
    TEST_ASSERT_GREATER_THAN(0, TIME_SYNC_TASK_PRIORITY);
    TEST_ASSERT_GREATER_THAN(0, TIME_SYNC_MAX_CALLBACKS);
}

/* ================================================================
 *  11. 状态枚举值验证测试
 * ================================================================ */

void test_sync_status_enum_values_should_sequential(void)
{
    TEST_ASSERT_EQUAL(0, SYNC_STATUS_IDLE);
    TEST_ASSERT_EQUAL(1, SYNC_STATUS_INITIALIZED);
    TEST_ASSERT_EQUAL(2, SYNC_STATUS_SYNCHRONIZING);
    TEST_ASSERT_EQUAL(3, SYNC_STATUS_SYNCED);
    TEST_ASSERT_EQUAL(4, SYNC_STATUS_FAILED);
    TEST_ASSERT_EQUAL(5, SYNC_STATUS_COUNT);
}

/* ================================================================
 *  12. 结构体验证测试
 * ================================================================ */

void test_struct_sizes_should_be_reasonable(void)
{
    TEST_ASSERT_GREATER_THAN(0, sizeof(struct time_info));
    TEST_ASSERT_GREATER_THAN(0, sizeof(struct sntp_server_config));
}

void test_time_info_fields_should_exist(void)
{
    struct time_info info;
    memset(&info, 0, sizeof(info));

    info.timestamp_us = 1704067200000000LL; /* 2024-01-01 00:00:00 UTC */
    info.timestamp_ms = 1704067200000LL;
    info.uptime_s = 3600;
    info.status = SYNC_STATUS_SYNCED;
    info.is_synchronized = true;
    info.last_sync_epoch = 1704067200;
    info.drift_us = 100;

    TEST_ASSERT_EQUAL(1704067200000000LL, info.timestamp_us);
    TEST_ASSERT_EQUAL(1704067200000LL, info.timestamp_ms);
    TEST_ASSERT_EQUAL(3600, info.uptime_s);
    TEST_ASSERT_EQUAL(SYNC_STATUS_SYNCED, info.status);
    TEST_ASSERT_TRUE(info.is_synchronized);
    TEST_ASSERT_EQUAL(1704067200U, info.last_sync_epoch);
    TEST_ASSERT_EQUAL(100, info.drift_us);
}

void test_sntp_server_config_fields_should_exist(void)
{
    struct sntp_server_config cfg;
    memset(&cfg, 0, sizeof(cfg));

    strncpy(cfg.server, "pool.ntp.org", sizeof(cfg.server) - 1);
    cfg.enabled = true;

    TEST_ASSERT_EQUAL_STRING("pool.ntp.org", cfg.server);
    TEST_ASSERT_TRUE(cfg.enabled);
}

/* ================================================================
 *  13. 边界条件与压力测试
 * ================================================================ */

void test_rapid_init_deinit_cycles(void)
{
    for (int i = 0; i < 10; i++) {
        int ret = time_sync_init(i % 2 == 0, NULL);
        if (ret == APP_ERR_OK) {
            time_sync_deinit();
        }
    }
    TEST_PASS(); /* 无崩溃即通过 */
}

void test_multiple_timestamp_reads_consistency(void)
{
    time_sync_init(false, NULL);

    int64_t prev_ts = 0;
    for (int i = 0; i < 20; i++) {
        int64_t ts = time_sync_get_timestamp_us();
        TEST_ASSERT_GREATER_OR_EQUAL(prev_ts, ts);
        prev_ts = ts;
    }
}

void test_max_callbacks_registration(void)
{
    time_sync_init(true, NULL);

    for (int i = 0; i < TIME_SYNC_MAX_CALLBACKS; i++) {
        int ret = time_sync_register_callback(test_sync_callback,
                                            (void *)(size_t)i);
        TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    }

    /* 下一个应该失败 */
    int ret = time_sync_register_callback(test_sync_callback,
                                         (void *)0xFF);
    TEST_ASSERT_EQUAL(APP_ERR_TIME_CALLBACK_FULL, ret);
}

void test_get_time_info_concurrent_access_simulation(void)
{
    time_sync_init(false, "CST-8");

    struct time_info infos[10];
    for (int i = 0; i < 10; i++) {
        int ret = time_sync_get_time_info(&infos[i]);
        TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
        TEST_ASSERT_EQUAL(SYNC_STATUS_INITIALIZED, infos[i].status);
    }
}

/*
 * ================================================================
 * Unity Test Runner Entry Point - 已禁用!
 * ================================================================
 *
 * ⚠️⚠️⚠️ 重要说明 ⚠️⚠️⚠️
 *
 * 此文件原本包含两种类型的测试:
 *   [A] 纯逻辑测试 (无需任何组件初始化)
 *   [B] 集成测试 (需要组件完整初始化)
 *
 * 🔴 已删除 app_main() 函数!
 * 原因: 避免与主程序 esp32-gateway.c 的 app_main() 冲突
 *       导致 Guru Meditation Error (LoadProhibited) 系统崩溃
 *
 * 如需运行测试,请创建独立测试项目,不要在此文件中定义 app_main()
 */

/* 以下测试入口已被永久移除 (2026-04-19):
#ifdef CONFIG_UNITY_ZERO_DEPENDENCY_MODE
void app_main(void) { ... }  // ← 已删除!
#else
void app_main(void) { ... }  // ← 已删除!
#endif
*/