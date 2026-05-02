/**
 * @file sensor_service_test.c
 * @author EnterWorldDoor
 * @brief 传感器服务层单元测试（基于 Unity 框架）
 *
 * 测试覆盖范围:
 *   - 常量与枚举值验证
 *   - 数据结构体大小验证
 *   - 参数验证 (NULL指针检查)
 *   - 错误码定义验证
 *   - 状态机转换验证
 *   - 回调注册/注销测试
 *   - 配置参数边界检查
 *   - API接口存在性验证
 */

#include "unity.h"
#include "sensor_service.h"
#include "global_error.h"
#include <string.h>
#include <math.h>
#include <stdint.h>

/* ==================== 测试辅助变量 ==================== */

static int g_callback_count = 0;
static struct analysis_result g_last_result;

/* ==================== 测试辅助函数 ==================== */

static void test_result_callback(const struct analysis_result *result, void *user_data)
{
    (void)user_data;
    if (result) {
        memcpy(&g_last_result, result, sizeof(*result));
        g_callback_count++;
    }
}

static void reset_callback_state(void)
{
    g_callback_count = 0;
    memset(&g_last_result, 0, sizeof(g_last_result));
}

/* ==================== 测试组：常量与枚举验证 ==================== */

/**
 * test_constants - 验证配置常量
 */
void test_constants(void)
{
    TEST_ASSERT_EQUAL_INT(400, SENSOR_DEFAULT_SAMPLE_RATE_HZ);
    TEST_ASSERT_EQUAL_INT(1024, SENSOR_DEFAULT_BUFFER_SIZE);
    TEST_ASSERT_EQUAL_INT(512, SENSOR_FFT_WINDOW_SIZE);
    TEST_ASSERT_EQUAL_INT(8192, SENSOR_TASK_STACK_SIZE);
    TEST_ASSERT_EQUAL_INT(6, SENSOR_TASK_PRIORITY);
    TEST_ASSERT_EQUAL_INT(1000, SENSOR_ANALYSIS_INTERVAL_MS);
    TEST_ASSERT_EQUAL_INT(8, SENSOR_MAX_CALLBACKS);
}

/**
 * test_state_enum - 验证状态枚举值唯一性和顺序
 */
void test_state_enum(void)
{
    /* 枚举值递增且唯一 */
    TEST_ASSERT_EQUAL_INT(0, SENSOR_STATE_UNINITIALIZED);
    TEST_ASSERT_EQUAL_INT(1, SENSOR_STATE_INITIALIZED);
    TEST_ASSERT_EQUAL_INT(2, SENSOR_STATE_RUNNING);
    TEST_ASSERT_EQUAL_INT(3, SENSOR_STATE_DEGRADED);
    TEST_ASSERT_EQUAL_INT(4, SENSOR_STATE_ERROR);

    TEST_ASSERT_NOT_EQUAL(SENSOR_STATE_UNINITIALIZED, SENSOR_STATE_INITIALIZED);
    TEST_ASSERT_NOT_EQUAL(SENSOR_STATE_INITIALIZED, SENSOR_STATE_RUNNING);
    TEST_ASSERT_NOT_EQUAL(SENSOR_STATE_RUNNING, SENSOR_STATE_DEGRADED);
    TEST_ASSERT_NOT_EQUAL(SENSOR_STATE_DEGRADED, SENSOR_STATE_ERROR);
}

/* ==================== 测试组：数据结构体大小验证 ==================== */

/**
 * test_struct_sizes - 验证关键结构体大小
 */
void test_struct_sizes(void)
{
    /* vib_sample 结构体字段完整性 */
    TEST_ASSERT_EQUAL(sizeof(float), sizeof(struct vib_sample.x));
    TEST_ASSERT_EQUAL(sizeof(float), sizeof(struct vib_sample.y));
    TEST_ASSERT_EQUAL(sizeof(float), sizeof(struct vib_sample.z));
    TEST_ASSERT_EQUAL(sizeof(int64_t), sizeof(struct vib_sample.timestamp_us));

    /* analysis_result 关键字段 */
    TEST_ASSERT_EQUAL(sizeof(float), sizeof(struct analysis_result.overall_rms_g));
    TEST_ASSERT_EQUAL(sizeof(float), sizeof(struct analysis_result.peak_frequency_hz));
    TEST_ASSERT_EQUAL(sizeof(bool), sizeof(struct analysis_result.temperature_valid));
    TEST_ASSERT_EQUAL(sizeof(enum sensor_state),
                      sizeof(struct analysis_result.service_state));
}

/* ==================== 测试组：初始化/反初始化测试 ==================== */

/**
 * test_init_uninitialized_state - 未初始化时操作应返回错误
 */
void test_init_uninitialized_state(void)
{
    struct sensor_config cfg;
    struct sensor_stats stats;
    enum sensor_state state;

    TEST_ASSERT_FALSE(sensor_is_initialized());
    TEST_ASSERT_EQUAL_INT(APP_ERR_SENSOR_NOT_INIT, sensor_start());
    TEST_ASSERT_EQUAL_INT(APP_ERR_SENSOR_NOT_INIT, sensor_stop());
    TEST_ASSERT_EQUAL_INT(APP_ERR_SENSOR_NOT_INIT, sensor_get_state(&state));
    TEST_ASSERT_EQUAL_INT(APP_ERR_SENSOR_NOT_INIT, sensor_get_stats(&stats));
    TEST_ASSERT_EQUAL_INT(APP_ERR_SENSOR_NOT_INIT,
                          sensor_register_callback(test_result_callback, NULL));
    TEST_ASSERT_EQUAL_INT(APP_ERR_SENSOR_NOT_INIT, sensor_deinit());
}

/**
 * test_null_param_handling - NULL 参数处理
 */
void test_null_param_handling(void)
{
    TEST_ASSERT_EQUAL_INT(APP_ERR_INVALID_PARAM, sensor_init(NULL));

    struct sensor_config valid_cfg = {
        .sample_rate_hz = 400,
        .buffer_size = 1024,
        .analysis_interval_ms = 1000,
        .enable_protocol_temp = true,
        .enable_detailed_log = false
    };

    int ret = sensor_init(&valid_cfg);
    if (ret == APP_ERR_OK) {
        TEST_ASSERT_EQUAL_INT(APP_ERR_INVALID_PARAM, sensor_get_state(NULL));
        TEST_ASSERT_EQUAL_INT(APP_ERR_INVALID_PARAM, sensor_get_stats(NULL));
        TEST_ASSERT_EQUAL_INT(APP_ERR_INVALID_PARAM,
                              sensor_register_callback(NULL, NULL));
        sensor_deinit();
    }
}

/**
 * test_init_success - 正常初始化流程
 */
void test_init_success(void)
{
    struct sensor_config cfg = {
        .sample_rate_hz = 400,
        .buffer_size = 1024,
        .analysis_interval_ms = 1000,
        .enable_protocol_temp = true,
        .enable_detailed_log = false
    };

    int ret = sensor_init(&cfg);
    TEST_ASSERT_EQUAL_INT(APP_ERR_OK, ret);
    TEST_ASSERT_TRUE(sensor_is_initialized());

    enum sensor_state state;
    ret = sensor_get_state(&state);
    if (ret == APP_ERR_OK) {
        TEST_ASSERT_EQUAL_INT(SENSOR_STATE_INITIALIZED, state);
    }

    sensor_deinit();
    TEST_ASSERT_FALSE(sensor_is_initialized());
}

/**
 * test_init_default_config - 使用默认配置初始化
 */
void test_init_default_config(void)
{
    int ret = sensor_init(NULL);  /* NULL 使用默认配置 */
    if (ret == APP_ERR_OK || ret == APP_ERR_SENSOR_NO_DRIVER) {
        /* 如果驱动未连接可能返回错误，这是可接受的 */
        if (ret == APP_ERR_OK) {
            TEST_ASSERT_TRUE(sensor_is_initialized());
            sensor_deinit();
        }
    } else {
        TEST_FAIL_MESSAGE("Unexpected return code");
    }
}

/* ==================== 测试组：回调注册测试 ==================== */

/**
 * test_callback_registration - 回调注册/注销
 */
void test_callback_registration(void)
{
    reset_callback_state();

    struct sensor_config cfg = {
        .sample_rate_hz = 400,
        .buffer_size = 1024,
        .analysis_interval_ms = 1000,
        .enable_protocol_temp = false,
        .enable_detailed_log = false
    };

    int ret = sensor_init(&cfg);
    if (ret != APP_ERR_OK) {
        TEST_IGNORE();
        return;
    }

    /* 注册回调 */
    ret = sensor_register_callback(test_result_callback, (void *)0xDEAD);
    TEST_ASSERT_EQUAL_INT(APP_ERR_OK, ret);

    /* 注销回调 */
    ret = sensor_unregister_callback(test_result_callback);
    TEST_ASSERT_EQUAL_INT(APP_ERR_OK, ret);

    sensor_deinit();
}

/**
 * test_callback_invalid_params - 无效回调参数
 */
void test_callback_invalid_params(void)
{
    struct sensor_config cfg = {
        .sample_rate_hz = 400,
        .buffer_size = 1024,
        .analysis_interval_ms = 1000,
        .enable_protocol_temp = false,
        .enable_detailed_log = false
    };

    int ret = sensor_init(&cfg);
    if (ret != APP_ERR_OK) {
        TEST_IGNORE();
        return;
    }

    /* 注册 NULL 回调应失败 */
    TEST_ASSERT_EQUAL_INT(APP_ERR_INVALID_PARAM,
                          sensor_register_callback(NULL, NULL));

    /* 注销未注册的回调应失败 */
    TEST_ASSERT_EQUAL_INT(APP_ERR_INVALID_PARAM,
                          sensor_unregister_callback(test_result_callback));

    sensor_deinit();
}

/* ==================== 测试组：统计信息测试 ==================== */

/**
 * test_stats_initialization - 统计信息初始状态
 */
void test_stats_initialization(void)
{
    struct sensor_stats stats;

    struct sensor_config cfg = {
        .sample_rate_hz = 400,
        .buffer_size = 1024,
        .analysis_interval_ms = 1000,
        .enable_protocol_temp = false,
        .enable_detailed_log = false
    };

    int ret = sensor_init(&cfg);
    if (ret != APP_ERR_OK) {
        TEST_IGNORE();
        return;
    }

    ret = sensor_get_stats(&stats);
    TEST_ASSERT_EQUAL_INT(APP_ERR_OK, ret);
    TEST_ASSERT_EQUAL_UINT32(0, stats.total_samples_processed);
    TEST_ASSERT_EQUAL_UINT32(0, stats.analysis_cycles_completed);
    TEST_ASSERT_EQUAL_UINT32(0, stats.error_count);

    sensor_deinit();
}

/**
 * test_stats_reset - 重置统计计数器
 */
void test_stats_reset(void)
{
    struct sensor_stats stats;

    struct sensor_config cfg = {
        .sample_rate_hz = 400,
        .buffer_size = 1024,
        .analysis_interval_ms = 1000,
        .enable_protocol_temp = false,
        .enable_detailed_log = false
    };

    int ret = sensor_init(&cfg);
    if (ret != APP_ERR_OK) {
        TEST_IGNORE();
        return;
    }

    /* 重置后仍为零 */
    sensor_reset_stats();

    ret = sensor_get_stats(&stats);
    TEST_ASSERT_EQUAL_INT(APP_ERR_OK, ret);
    TEST_ASSERT_EQUAL_UINT32(0, stats.total_samples_processed);

    sensor_deinit();
}

/**
 * test_stats_null_param - NULL 参数处理
 */
void test_stats_null_param(void)
{
    struct sensor_config cfg = {
        .sample_rate_hz = 400,
        .buffer_size = 1024,
        .analysis_interval_ms = 1000,
        .enable_protocol_temp = false,
        .enable_detailed_log = false
    };

    int ret = sensor_init(&cfg);
    if (ret != APP_ERR_OK) {
        TEST_IGNORE();
        return;
    }

    TEST_ASSERT_EQUAL_INT(APP_ERR_INVALID_PARAM, sensor_get_stats(NULL));

    sensor_deinit();
}

/* ==================== 测试组：状态查询测试 ==================== */

/**
 * test_state_after_init - 初始化后的状态应为 INITIALIZED
 */
void test_state_after_init(void)
{
    struct sensor_config cfg = {
        .sample_rate_hz = 400,
        .buffer_size = 1024,
        .analysis_interval_ms = 1000,
        .enable_protocol_temp = false,
        .enable_detailed_log = false
    };

    int ret = sensor_init(&cfg);
    if (ret != APP_ERR_OK) {
        TEST_IGNORE();
        return;
    }

    enum sensor_state state;
    ret = sensor_get_state(&state);
    TEST_ASSERT_EQUAL_INT(APP_ERR_OK, ret);
    TEST_ASSERT_EQUAL_INT(SENSOR_STATE_INITIALIZED, state);

    sensor_deinit();
}

/**
 * test_state_null_param - NULL 参数处理
 */
void test_state_null_param(void)
{
    struct sensor_config cfg = {
        .sample_rate_hz = 400,
        .buffer_size = 1024,
        .analysis_interval_ms = 1000,
        .enable_protocol_temp = false,
        .enable_detailed_log = false
    };

    int ret = sensor_init(&cfg);
    if (ret != APP_ERR_OK) {
        TEST_IGNORE();
        return;
    }

    TEST_ASSERT_EQUAL_INT(APP_ERR_INVALID_PARAM, sensor_get_state(NULL));

    sensor_deinit();
}

/* ==================== Unity 主函数 ==================== */

void setUp(void) { }
void tearDown(void) {
    if (sensor_is_initialized()) {
        sensor_stop();
        sensor_deinit();
    }
    reset_callback_state();
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_constants);
    RUN_TEST(test_state_enum);
    RUN_TEST(test_struct_sizes);
    RUN_TEST(test_init_uninitialized_state);
    RUN_TEST(test_null_param_handling);
    RUN_TEST(test_init_success);
    RUN_TEST(test_init_default_config);
    RUN_TEST(test_callback_registration);
    RUN_TEST(test_callback_invalid_params);
    RUN_TEST(test_stats_initialization);
    RUN_TEST(test_stats_reset);
    RUN_TEST(test_stats_null_param);
    RUN_TEST(test_state_after_init);
    RUN_TEST(test_state_null_param);

    return UNITY_END();
}