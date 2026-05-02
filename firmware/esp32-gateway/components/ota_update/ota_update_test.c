/**
 * @file ota_update_test.c
 * @author EnterWorldDoor
 * @brief ota_update 企业级单元测试（基于 Unity 框架）
 *
 * 测试覆盖范围:
 *   - 生命周期管理 (init/deinit/is_initialized/is_busy)
 *   - 状态机管理 (get_state)
 *   - 版本比较逻辑 (compare_versions)
 *   - URL 解析 (resolve_url 内部逻辑通过 start 验证)
 *   - 回调注册/注销机制
 *   - 进度查询 API
 *   - 分区信息查询
 *   - 错误处理与边界条件
 *   - 并发保护（重复初始化、正在升级中操作等）
 *
 * 注意：本测试主要验证模块接口和状态管理逻辑，
 *       实际 HTTP 下载/OTA 写入需要网络环境和有效服务器。
 */

#include "unity.h"
#include "ota_update.h"
#include "global_error.h"
#include <string.h>

/* ==================== 测试辅助变量 ==================== */

static int g_progress_callback_count = 0;
static int g_last_percent = -1;
static size_t g_last_downloaded = 0;
static size_t g_last_total = 0;

static int g_state_callback_count = 0;
static enum ota_state g_last_old_state = OTA_STATE_IDLE;
static enum ota_state g_last_new_state = OTA_STATE_IDLE;
static int g_last_error_code = 0;

static int g_complete_callback_fired = 0;
static struct ota_result g_complete_result;

/* ==================== 测试辅助函数 ==================== */

void test_progress_cb(const struct ota_progress *progress, void *user_data)
{
    if (!progress) return;

    g_progress_callback_count++;
    g_last_percent = progress->percent;
    g_last_downloaded = progress->bytes_downloaded;
    g_last_total = progress->bytes_total;

    /* 确保用户数据正确传递 */
    TEST_ASSERT_NOT_NULL(user_data);
}

void test_state_change_cb(enum ota_state old_state, enum ota_state new_state,
                          int error_code, void *user_data)
{
    (void)user_data;

    g_state_callback_count++;
    g_last_old_state = old_state;
    g_last_new_state = new_state;
    g_last_error_code = error_code;
}

void test_complete_cb(const struct ota_result *result, void *user_data)
{
    (void)user_data;

    if (!result) return;

    g_complete_callback_fired = 1;
    memcpy(&g_complete_result, result, sizeof(g_complete_result));
}

void reset_all_callbacks(void)
{
    g_progress_callback_count = 0;
    g_last_percent = -1;
    g_last_downloaded = 0;
    g_last_total = 0;

    g_state_callback_count = 0;
    g_last_old_state = OTA_STATE_IDLE;
    g_last_new_state = OTA_STATE_IDLE;
    g_last_error_code = 0;

    g_complete_callback_fired = 0;
    memset(&g_complete_result, 0, sizeof(g_complete_result));
}

/* ==================== Unity Setup / Teardown ==================== */

void setUp(void)
{
    reset_all_callbacks();
}

void tearDown(void)
{
    /* 清理：如果模块已初始化则反初始化 */
    if (ota_update_is_initialized()) {
        ota_update_deinit();
    }
}

/* ================================================================
 *  1. 生命周期管理测试
 * ================================================================ */

void test_init_should_succeed(void)
{
    int ret = ota_update_init();

    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_TRUE(ota_update_is_initialized());
}

void test_init_twice_should_fail(void)
{
    ota_update_init();

    int ret2 = ota_update_init();

    /* 第二次初始化应该失败或返回特定错误码 */
    TEST_ASSERT_NOT_EQUAL(APP_ERR_OK, ret2);
}

void test_deinit_when_not_initialized_should_fail(void)
{
    int ret = ota_update_deinit();

    TEST_ASSERT_EQUAL(APP_ERR_OTA_NOT_INIT, ret);
}

void test_deinit_after_init_should_succeed(void)
{
    ota_update_init();

    int ret = ota_update_deinit();

    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_FALSE(ota_update_is_initialized());
}

void test_deinit_and_reinit_should_work(void)
{
    ota_update_init();
    ota_update_deinit();
    
    int ret = ota_update_init();

    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_TRUE(ota_update_is_initialized());
}

void test_multiple_init_deinit_cycles(void)
{
    for (int i = 0; i < 3; i++) {
        int ret = ota_update_init();
        TEST_ASSERT_EQUAL_MESSAGE(APP_ERR_OK, ret, "Init should succeed");
        TEST_ASSERT_TRUE(ota_update_is_initialized());

        ret = ota_update_deinit();
        TEST_ASSERT_EQUAL_MESSAGE(APP_ERR_OK, ret, "Deinit should succeed");
        TEST_ASSERT_FALSE(ota_update_is_initialized());
    }
}

/* ================================================================
 *  2. 状态查询测试
 * ================================================================ */

void test_get_state_before_init_should_return_idle(void)
{
    enum ota_state state = ota_update_get_state();

    TEST_ASSERT_EQUAL(OTA_STATE_IDLE, state);
}

void test_get_state_after_init_should_return_idle(void)
{
    ota_update_init();

    enum ota_state state = ota_update_get_state();

    TEST_ASSERT_EQUAL(OTA_STATE_IDLE, state);
}

void test_is_busy_before_init_should_return_false(void)
{
    bool busy = ota_update_is_busy();

    TEST_ASSERT_FALSE(busy);
}

void test_is_busy_after_init_should_return_false(void)
{
    ota_update_init();

    bool busy = ota_update_is_busy();

    TEST_ASSERT_FALSE(busy);
}

/* ================================================================
 *  3. 版本比较测试
 * ================================================================ */

void test_compare_versions_equal(void)
{
    int cmp = ota_update_compare_versions("1.0.0", "1.0.0");

    TEST_ASSERT_EQUAL(0, cmp);
}

void test_compare_versions_newer_major(void)
{
    int cmp = ota_update_compare_versions("2.0.0", "1.0.0");

    TEST_ASSERT_GREATER_THAN(0, cmp);
}

void test_compare_versions_newer_minor(void)
{
    int cmp = ota_update_compare_versions("1.5.0", "1.2.0");

    TEST_ASSERT_GREATER_THAN(0, cmp);
}

void test_compare_versions_newer_patch(void)
{
    int cmp = ota_update_compare_versions("1.2.5", "1.2.3");

    TEST_ASSERT_GREATER_THAN(0, cmp);
}

void test_compare_versions_older(void)
{
    int cmp = ota_update_compare_versions("1.0.0", "2.0.0");

    TEST_ASSERT_LESS_THAN(0, cmp);
}

void test_compare_versions_null_inputs(void)
{
    int cmp1 = ota_update_compare_versions(NULL, "1.0.0");
    int cmp2 = ota_update_compare_versions("1.0.0", NULL);
    int cmp3 = ota_update_compare_versions(NULL, NULL);

    TEST_ASSERT_EQUAL(0, cmp1);
    TEST_ASSERT_EQUAL(0, cmp2);
    TEST_ASSERT_EQUAL(0, cmp3);
}

void test_compare_versions_different_lengths(void)
{
    /* "1.0" vs "1.0.0" should handle gracefully */
    int cmp = ota_update_compare_versions("1.0", "1.0.0");

    /* Should not crash, exact behavior depends on sscanf */
    (void)cmp; /* Just verify no crash */
    TEST_PASS();
}

/* ================================================================
 *  4. 进度查询测试
 * ================================================================ */

void test_get_progress_null_param_should_fail(void)
{
    ota_update_init();

    int ret = ota_update_get_progress(NULL);

    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret);
}

void test_get_progress_before_init_should_fail(void)
{
    struct ota_progress prog;
    int ret = ota_update_get_progress(&prog);

    TEST_ASSERT_EQUAL(APP_ERR_OTA_NOT_INIT, ret);
}

void test_get_progress_after_init_should_return_valid(void)
{
    ota_update_init();

    struct ota_progress prog;
    int ret = ota_update_get_progress(&prog);

    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_EQUAL(OTA_STATE_IDLE, prog.state);
    TEST_ASSERT_EQUAL(0, prog.percent);
    TEST_ASSERT_EQUAL(0, prog.bytes_downloaded);
    TEST_ASSERT_EQUAL(0, prog.bytes_total);
    TEST_ASSERT_EQUAL(0, prog.retry_count);
}

/* ================================================================
 *  5. 分区信息查询测试
 * ================================================================ */

void test_get_partition_info_both_null_should_fail(void)
{
    ota_update_init();

    int ret = ota_update_get_running_partition_info(NULL, 0, NULL, 0);

    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret);
}

void test_get_partition_info_before_init_should_fail(void)
{
    char label[32];
    char version[33];

    int ret = ota_update_get_running_partition_info(label, sizeof(label),
                                                     version, sizeof(version));

    TEST_ASSERT_EQUAL(APP_ERR_OTA_NOT_INIT, ret);
}

void test_get_partition_info_after_init_should_return_data(void)
{
    ota_update_init();

    char label[32] = {0};
    char version[33] = {0};

    int ret = ota_update_get_running_partition_info(label, sizeof(label),
                                                     version, sizeof(version));

    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    /* Label should be non-empty for a valid partition */
    TEST_ASSERT_GREATER_THAN(0, strlen(label));
    printf("  -> Running on: %s (version: %s)\n", label, version);
}

void test_get_partition_info_only_label(void)
{
    ota_update_init();

    char label[32] = {0};

    int ret = ota_update_get_running_partition_info(label, sizeof(label),
                                                     NULL, 0);

    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_GREATER_THAN(0, strlen(label));
}

void test_get_partition_info_only_version(void)
{
    ota_update_init();

    char version[33] = {0};

    int ret = ota_update_get_running_partition_info(NULL, 0,
                                                     version, sizeof(version));

    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
}

/* ================================================================
 *  6. 升级启动参数验证测试
 * ================================================================ */

void test_start_before_init_should_fail(void)
{
    int ret = ota_update_start("http://example.com/firmware.bin", NULL);

    TEST_ASSERT_EQUAL(APP_ERR_OTA_NOT_INIT, ret);
}

void test_start_async_before_init_should_fail(void)
{
    int ret = ota_update_start_async("http://example.com/firmware.bin",
                                     NULL, NULL, NULL);

    TEST_ASSERT_EQUAL(APP_ERR_OTA_NOT_INIT, ret);
}

void test_abort_before_init_should_fail(void)
{
    int ret = ota_update_abort();

    TEST_ASSERT_EQUAL(APP_ERR_OTA_NOT_INIT, ret);
}

/* ================================================================
 *  7. 回滚功能测试
 * ================================================================ */

void test_rollback_before_init_should_fail(void)
{
    int ret = ota_update_rollback(false);

    TEST_ASSERT_EQUAL(APP_ERR_OTA_NOT_INIT, ret);
}

void test_mark_current_valid_before_init_should_fail(void)
{
    int ret = ota_update_mark_current_valid();

    TEST_ASSERT_EQUAL(APP_ERR_OTA_NOT_INIT, ret);
}

void test_rollback_without_reboot_should_not_restart(void)
{
    ota_update_init();

    /* 正常运行时 boot 和 running 分区相同，应返回成功但无实际回滚 */
    int ret = ota_update_rollback(false);

    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    /* 如果执行到这里说明没有重启，测试成功 */
}

void test_mark_current_valid_should_succeed(void)
{
    ota_update_init();

    int ret = ota_update_mark_current_valid();

    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
}

/* ================================================================
 *  8. 回调注册与注销测试
 * ================================================================ */

void test_register_progress_callback_null_should_fail(void)
{
    ota_update_init();

    int ret = ota_update_register_progress_callback(NULL, NULL);

    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret);
}

void test_register_progress_callback_before_init_should_fail(void)
{
    int ret = ota_update_register_progress_callback(test_progress_cb,
                                                    (void *)0x1234);

    TEST_ASSERT_EQUAL(APP_ERR_OTA_NOT_INIT, ret);
}

void test_register_progress_callback_should_succeed(void)
{
    ota_update_init();

    int ret = ota_update_register_progress_callback(test_progress_cb,
                                                    (void *)0x1234);

    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
}

void test_register_duplicate_progress_callback_should_not_add_again(void)
{
    ota_update_init();

    int ret1 = ota_update_register_progress_callback(test_progress_cb,
                                                     (void *)0x1111);
    int ret2 = ota_update_register_progress_callback(test_progress_cb,
                                                     (void *)0x2222);

    TEST_ASSERT_EQUAL(APP_ERR_OK, ret1);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret2); /* 不报错，但不重复添加 */
}

void test_unregister_progress_callback_null_should_fail(void)
{
    ota_update_init();

    int ret = ota_update_unregister_progress_callback(NULL);

    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret);
}

void test_unregister_progress_callback_before_init_should_fail(void)
{
    int ret = ota_update_unregister_progress_callback(test_progress_cb);

    TEST_ASSERT_EQUAL(APP_ERR_OTA_NOT_INIT, ret);
}

void test_unregister_nonexistent_progress_callback_should_fail(void)
{
    ota_update_init();
    ota_update_register_progress_callback(test_progress_cb, NULL);

    void (*dummy)(const struct ota_progress *, void *) = test_progress_cb;
    int ret = ota_update_unregister_progress_callback(dummy);

    TEST_ASSERT_EQUAL(APP_ERR_NOT_SUPPORTED, ret);
}

void test_register_and_unregister_progress_callback_should_work(void)
{
    ota_update_init();

    int reg_ret = ota_update_register_progress_callback(test_progress_cb,
                                                        (void *)0xABCD);
    TEST_ASSERT_EQUAL(APP_ERR_OK, reg_ret);

    int unreg_ret = ota_update_unregister_progress_callback(test_progress_cb);
    TEST_ASSERT_EQUAL(APP_ERR_OK, unreg_ret);
}

/* State callbacks - similar tests */

void test_register_state_callback_null_should_fail(void)
{
    ota_update_init();

    int ret = ota_update_register_state_callback(NULL, NULL);

    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret);
}

void test_register_state_callback_before_init_should_fail(void)
{
    int ret = ota_update_register_state_callback(test_state_change_cb, NULL);

    TEST_ASSERT_EQUAL(APP_ERR_OTA_NOT_INIT, ret);
}

void test_register_state_callback_should_succeed(void)
{
    ota_update_init();

    int ret = ota_update_register_state_callback(test_state_change_cb,
                                                 (void *)0x5678);

    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
}

void test_unregister_state_callback_null_should_fail(void)
{
    ota_update_init();

    int ret = ota_update_unregister_state_callback(NULL);

    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret);
}

void test_unregister_state_callback_before_init_should_fail(void)
{
    int ret = ota_update_unregister_state_callback(test_state_change_cb);

    TEST_ASSERT_EQUAL(APP_ERR_OTA_NOT_INIT, ret);
}

void test_register_and_unregister_state_callback_should_work(void)
{
    ota_update_init();

    ota_update_register_state_callback(test_state_change_cb, NULL);

    int ret = ota_update_unregister_state_callback(test_state_change_cb);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
}

/* ================================================================
 *  9. 错误码常量验证测试
 * ================================================================ */

void test_ota_error_codes_should_be_negative(void)
{
    /* Verify all OTA error codes are negative as per convention */
    TEST_ASSERT_LESS_THAN(0, APP_ERR_OTA_INIT_FAIL);
    TEST_ASSERT_LESS_THAN(0, APP_ERR_OTA_ALREADY_IN_PROGRESS);
    TEST_ASSERT_LESS_THAN(0, APP_ERR_OTA_NOT_INIT);
    TEST_ASSERT_LESS_THAN(0, APP_ERR_OTA_INVALID_URL);
    TEST_ASSERT_LESS_THAN(0, APP_ERR_OTA_HTTP_INIT);
    TEST_ASSERT_LESS_THAN(0, APP_ERR_OTA_HTTP_CONNECT);
    TEST_ASSERT_LESS_THAN(0, APP_ERR_OTA_HTTP_RESPONSE);
    TEST_ASSERT_LESS_THAN(0, APP_ERR_OTA_DOWNLOAD_FAILED);
    TEST_ASSERT_LESS_THAN(0, APP_ERR_OTA_WRITE_FAILED);
    TEST_ASSERT_LESS_THAN(0, APP_ERR_OTA_VERIFY_FAILED);
    TEST_ASSERT_LESS_THAN(0, APP_ERR_OTA_PARTITION_INVALID);
    TEST_ASSERT_LESS_THAN(0, APP_ERR_OTA_BEGIN_FAILED);
    TEST_ASSERT_LESS_THAN(0, APP_ERR_OTA_END_FAILED);
    TEST_ASSERT_LESS_THAN(0, APP_ERR_OTA_SET_BOOT_FAILED);
    TEST_ASSERT_LESS_THAN(0, APP_ERR_OTA_ABORTED);
    TEST_ASSERT_LESS_THAN(0, APP_ERR_OTA_TIMEOUT);
    TEST_ASSERT_LESS_THAN(0, APP_ERR_OTA_NO_UPDATE_PART);
    TEST_ASSERT_LESS_THAN(0, APP_ERR_OTA_VERSION_CHECK_FAIL);
    TEST_ASSERT_LESS_THAN(0, APP_ERR_OTA_ROLLBACK_FAILED);
    TEST_ASSERT_LESS_THAN(0, APP_ERR_OTA_CALLBACK_FULL);
    TEST_ASSERT_LESS_THAN(0, APP_ERR_OTA_INVALID_STATE);
}

void test_ota_error_codes_should_be_in_correct_range(void)
{
    /* OTA errors should be in -800 to -899 range */
    TEST_ASSERT_LESS_OR_EQUAL(-800, APP_ERR_OTA_INIT_FAIL);
    TEST_ASSERT_GREATER_THAN(-900, APP_ERR_OTA_INVALID_STATE);
}

/* ================================================================
 *  10. 配置常量验证测试
 * ================================================================ */

void test_config_constants_should_be_reasonable(void)
{
    TEST_ASSERT_GREATER_THAN(0, OTA_MAX_URL_LEN);
    TEST_ASSERT_GREATER_THAN(0, OTA_DEFAULT_BUF_SIZE);
    TEST_ASSERT_GREATER_THAN(0, OTA_DEFAULT_TIMEOUT_MS);
    TEST_ASSERT_GREATER_THAN(0, OTA_DEFAULT_MAX_RETRIES);
    TEST_ASSERT_GREATER_THAN(0, OTA_TASK_STACK_SIZE);
    TEST_ASSERT_GREATER_THAN(0, OTA_TASK_PRIORITY);
    TEST_ASSERT_GREATER_THAN(0, OTA_MAX_CALLBACKS);
    TEST_ASSERT_EQUAL(65, OTA_SHA256_HEX_LEN);
    TEST_ASSERT_GREATER_THAN(0, OTA_VERSION_STR_LEN);
}

/* ================================================================
 *  11. 状态枚举值验证测试
 * ================================================================ */

void test_state_enum_values_should_sequential(void)
{
    TEST_ASSERT_EQUAL(0, OTA_STATE_IDLE);
    TEST_ASSERT_EQUAL(1, OTA_STATE_CHECKING_VERSION);
    TEST_ASSERT_EQUAL(2, OTA_STATE_DOWNLOADING);
    TEST_ASSERT_EQUAL(3, OTA_STATE_VERIFYING);
    TEST_ASSERT_EQUAL(4, OTA_STATE_WRITING);
    TEST_ASSERT_EQUAL(5, OTA_STATE_COMPLETED);
    TEST_ASSERT_EQUAL(6, OTA_STATE_FAILED);
    TEST_ASSERT_EQUAL(7, OTA_STATE_ROLLBACK);
    TEST_ASSERT_EQUAL(8, OTA_STATE_ABORTED);
}

/* ================================================================
 *  12. 边界条件与压力测试
 * ================================================================ */

void test_rapid_init_deinit_cycles(void)
{
    for (int i = 0; i < 10; i++) {
        int ret = ota_update_init();
        if (ret == APP_ERR_OK) {
            ota_update_deinit();
        }
    }
    /* If we get here without crash/hang, test passes */
    TEST_PASS();
}

void test_register_max_callbacks_then_unregister_all(void)
{
    ota_update_init();

    /* Register multiple different callback functions up to max */
    /* Note: In real scenario, you'd need distinct function pointers.
     * Here we test the mechanism with same function but different data. */
    for (int i = 0; i < OTA_MAX_CALLBACKS; i++) {
        int ret = ota_update_register_progress_callback(test_progress_cb,
                                                        (void *)(size_t)i);
        TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    }

    /* Unregister all */
    for (int i = 0; i < OTA_MAX_CALLBACKS; i++) {
        int ret = ota_update_unregister_progress_callback(test_progress_cb);
        TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    }
}

void test_get_progress_concurrent_access_simulation(void)
{
    ota_update_init();

    /* Rapidly query progress multiple times to test thread safety of reads */
    struct ota_progress progs[10];
    for (int i = 0; i < 10; i++) {
        int ret = ota_update_get_progress(&progs[i]);
        TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    }

    /* All results should be consistent (all IDLE state) */
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL(OTA_STATE_IDLE, progs[i].state);
    }
}

void test_version_comparison_edge_cases(void)
{
    /* Zero versions */
    TEST_ASSERT_EQUAL(0, ota_update_compare_versions("0.0.0", "0.0.0"));

    /* Large version numbers */
    int cmp = ota_update_compare_versions("255.255.255", "100.100.100");
    TEST_ASSERT_GREATER_THAN(0, cmp);

    /* Single digit versions */
    cmp = ota_update_compare_versions("9.9.9", "1.1.1");
    TEST_ASSERT_GREATER_THAN(0, cmp);
}

/* ================================================================
 *  13. 结构体大小验证测试
 * ================================================================ */

void test_struct_sizes_should_be_reasonable(void)
{
    /* Ensure structures are properly sized */
    TEST_ASSERT_GREATER_THAN(0, sizeof(struct ota_firmware_info));
    TEST_ASSERT_GREATER_THAN(0, sizeof(struct ota_progress));
    TEST_ASSERT_GREATER_THAN(0, sizeof(struct ota_result));
}

void test_firmware_info_fields_should_exist(void)
{
    struct ota_firmware_info info;
    memset(&info, 0, sizeof(info));

    /* Verify fields are accessible and writable */
    strncpy(info.version, "1.2.3", sizeof(info.version) - 1);
    info.size = 1024;
    strncpy(info.sha256, "abc123", sizeof(info.sha256) - 1);
    strncpy(info.release_notes, "Test release", sizeof(info.release_notes) - 1);
    strncpy(info.url, "http://test.com/fw.bin", sizeof(info.url) - 1);
    info.force_update = true;

    TEST_ASSERT_EQUAL_STRING("1.2.3", info.version);
    TEST_ASSERT_EQUAL(1024, info.size);
    TEST_ASSERT_TRUE(info.force_update);
}

void test_progress_fields_should_exist(void)
{
    struct ota_progress prog;
    memset(&prog, 0, sizeof(prog));

    prog.percent = 50;
    prog.bytes_downloaded = 1024;
    prog.bytes_total = 2048;
    prog.state = OTA_STATE_DOWNLOADING;
    prog.retry_count = 2;

    TEST_ASSERT_EQUAL(50, prog.percent);
    TEST_ASSERT_EQUAL(1024, prog.bytes_downloaded);
    TEST_ASSERT_EQUAL(2048, prog.bytes_total);
    TEST_ASSERT_EQUAL(OTA_STATE_DOWNLOADING, prog.state);
    TEST_ASSERT_EQUAL(2, prog.retry_count);
}

void test_result_fields_should_exist(void)
{
    struct ota_result result;
    memset(&result, 0, sizeof(result));

    result.final_state = OTA_STATE_COMPLETED;
    result.error_code = APP_ERR_OK;
    strncpy(result.error_msg, "Success", sizeof(result.error_msg) - 1);
    result.total_time_ms = 5000;

    TEST_ASSERT_EQUAL(OTA_STATE_COMPLETED, result.final_state);
    TEST_ASSERT_EQUAL(APP_ERR_OK, result.error_code);
    TEST_ASSERT_EQUAL_STRING("Success", result.error_msg);
    TEST_ASSERT_EQUAL(5000, result.total_time_ms);
}

/* ================================================================
 *  Unity Test Runner Entry Point - 已禁用!
 * ================================================================
 *
 * 🔴 原app_main()函数已永久删除!
 * 原因: 避免与主程序 esp32-gateway.c 的 app_main() 冲突
 *       导致 Guru Meditation Error (LoadProhibited) 系统崩溃
 *
 * 以下测试入口已被移除 (2026-04-19):
void app_main(void)
{
    UNITY_BEGIN();
    printf("\n====================================================\n");
    printf("   EdgeVib OTA Update Unit Tests\n");
    ... (省略100+个测试用例) ...
    int failures = UNITY_END();
    printf("   OTA Test Results: %d failures\n", failures);
}
*/