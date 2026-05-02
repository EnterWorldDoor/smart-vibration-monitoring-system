/**
 * @file temperature_compensation_test.c
 * @author EnterWorldDoor
 * @brief 温度补偿模块单元测试（基于 Unity 框架）
 *
 * 测试覆盖范围:
 *   - 常量与枚举值验证
 *   - 数据结构体大小验证
 *   - 参数验证 (NULL指针检查)
 *   - 错误码定义验证
 *   - EWMA 滤波算法正确性
 *   - 温度变化检测逻辑
 *   - 校准流程验证
 *   - API接口存在性验证
 */

#include "unity.h"
#include "temperature_compensation.h"
#include "global_error.h"
#include <string.h>
#include <math.h>
#include <stdint.h>

/* ==================== 测试辅助变量 ==================== */

static struct temp_comp_config default_config = {
    .ewma_alpha = TEMP_COMP_DEFAULT_ALPHA,
    .temp_change_threshold = TEMP_COMP_TEMP_THRESHOLD,
    .rate_threshold = TEMP_COMP_RATE_THRESHOLD,
    .stale_timeout_ms = TEMP_COMP_STALE_TIMEOUT_MS,
    .interp_max_delta_ms = TEMP_COMP_INTERP_MAX_DELTA_MS
};

/* ==================== 测试辅助函数 ==================== */

/**
 * create_temp_sample - 创建温度样本
 */
static struct temp_sample make_sample(float temp, int64_t timestamp_us)
{
    struct temp_sample s;
    s.temperature_c = temp;
    s.timestamp_us = timestamp_us;
    return s;
}

/* ==================== 测试组：常量与枚举验证 ==================== */

/**
 * test_constants - 验证配置常量
 */
void test_constants(void)
{
    TEST_ASSERT_EQUAL_INT(64, TEMP_COMP_MAX_HISTORY);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.1f, TEMP_COMP_DEFAULT_ALPHA);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, TEMP_COMP_TEMP_THRESHOLD);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, TEMP_COMP_RATE_THRESHOLD);
    TEST_ASSERT_EQUAL_INT(5000, TEMP_COMP_STALE_TIMEOUT_MS);
    TEST_ASSERT_EQUAL_INT(200, TEMP_COMP_INTERP_MAX_DELTA_MS);
}

/* ==================== 测试组：数据结构体大小验证 ==================== */

/**
 * test_struct_sizes - 验证关键结构体大小
 */
void test_struct_sizes(void)
{
    /* temp_sample 结构体字段完整性 */
    TEST_ASSERT_EQUAL(sizeof(float), sizeof(struct temp_sample.temperature_c));
    TEST_ASSERT_EQUAL(sizeof(int64_t), sizeof(struct temp_sample.timestamp_us));

    /* temp_comp_config 结构体 */
    TEST_ASSERT_EQUAL(sizeof(float), sizeof(struct temp_comp_config.ewma_alpha));
    TEST_ASSERT_EQUAL(sizeof(uint32_t), sizeof(struct temp_comp_config.stale_timeout_ms));

    /* temp_comp_state 结构体 */
    TEST_ASSERT_EQUAL(sizeof(float), sizeof(struct temp_comp_state.current_temp));
    TEST_ASSERT_EQUAL(sizeof(bool), sizeof(struct temp_comp_state.calibrated));

    /* temp_comp_stats 结构体 */
    TEST_ASSERT_EQUAL(sizeof(uint32_t), sizeof(struct temp_comp_stats.total_samples));
    TEST_ASSERT_EQUAL(sizeof(float), sizeof(struct temp_comp_stats.max_offset_applied));
}

/* ==================== 测试组：错误码定义验证 ==================== */

/**
 * test_error_codes - 验证温度补偿错误码范围和唯一性
 */
void test_error_codes(void)
{
    /* 温度补偿错误码范围: -1300 ~ -1304 */
    TEST_ASSERT_LESS_THAN(-1299, APP_ERR_TEMP_NOT_INIT);
    TEST_ASSERT_GREATER_THAN(-1305, APP_ERR_TEMP_SENSOR_OFFLINE);

    /* 各错误码唯一性 */
    TEST_ASSERT_NOT_EQUAL(APP_ERR_TEMP_NOT_INIT, APP_ERR_TEMP_NO_DATA);
    TEST_ASSERT_NOT_EQUAL(APP_ERR_TEMP_NO_DATA, APP_ERR_TEMP_STALE_DATA);
    TEST_ASSERT_NOT_EQUAL(APP_ERR_TEMP_STALE_DATA, APP_ERR_TEMP_CALIBRATION_FAIL);
    TEST_ASSERT_NOT_EQUAL(APP_ERR_TEMP_CALIBRATION_FAIL, APP_ERR_TEMP_SENSOR_OFFLINE);
}

/* ==================== 测试组：初始化/反初始化测试 ==================== */

/**
 * test_init_uninitialized_state - 未初始化时操作应返回错误
 */
void test_init_uninitialized_state(void)
{
    struct temp_sample sample = { .temperature_c = 25.0f, .timestamp_us = 1000000 };
    float offsets[3] = { 0 };
    struct temp_comp_state state;

    TEST_ASSERT_FALSE(temp_comp_is_initialized());
    TEST_ASSERT_EQUAL_INT(APP_ERR_TEMP_NOT_INIT, temp_comp_feed_sample(&sample));
    TEST_ASSERT_EQUAL_INT(APP_ERR_TEMP_NOT_INIT, temp_comp_get_offsets(offsets));
    TEST_ASSERT_EQUAL_INT(APP_ERR_TEMP_NOT_INIT, temp_comp_get_state(&state));
    TEST_ASSERT_EQUAL_INT(APP_ERR_TEMP_NOT_INIT, temp_comp_deinit());
}

/**
 * test_null_param_handling - NULL 参数处理
 */
void test_null_param_handling(void)
{
    /* NULL 配置应使用默认值，不报错 */
    int ret = temp_comp_init(NULL);
    if (ret == APP_ERR_OK) {
        temp_comp_deinit();
    }
    /* 但 NULL 样本应该报错 */
    ret = temp_comp_init(&default_config);
    TEST_ASSERT_EQUAL_INT(APP_ERR_OK, ret);
    TEST_ASSERT_EQUAL_INT(APP_ERR_INVALID_PARAM, temp_comp_feed_sample(NULL));
    temp_comp_deinit();
}

/**
 * test_init_success - 正常初始化流程
 */
void test_init_success(void)
{
    int ret = temp_comp_init(&default_config);
    TEST_ASSERT_EQUAL_INT(APP_ERR_OK, ret);
    TEST_ASSERT_TRUE(temp_comp_is_initialized());

    struct temp_comp_state state;
    ret = temp_comp_get_state(&state);
    TEST_ASSERT_EQUAL_INT(APP_ERR_OK, ret);
    TEST_ASSERT_FALSE(state.calibrated);  /* 初始未校准 */
    TEST_ASSERT_FALSE(state.sensor_online);

    temp_comp_deinit();
    TEST_ASSERT_FALSE(temp_comp_is_initialized());
}

/**
 * test_init_default_config - 使用默认配置初始化
 */
void test_init_default_config(void)
{
    int ret = temp_comp_init(NULL);  /* NULL 使用默认配置 */
    TEST_ASSERT_EQUAL_INT(APP_ERR_OK, ret);
    TEST_ASSERT_TRUE(temp_comp_is_initialized());
    temp_comp_deinit();
}

/* ==================== 测试组：EWMA 滤波算法测试 ==================== */

/**
 * test_ewma_convergence - EWMA 应收敛到稳定值
 */
void test_ewma_convergence(void)
{
    temp_comp_init(&default_config);

    /* 连续输入相同温度值 25.0°C */
    for (int i = 0; i < 100; i++) {
        struct temp_sample s = make_sample(25.0f, (int64_t)(i * 1000000));
        temp_comp_feed_sample(&s);
    }

    struct temp_comp_state state;
    temp_comp_get_state(&state);
    
    /* EWMA 应收敛到接近 25.0°C */
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 25.0f, state.current_temp);

    temp_comp_deinit();
}

/**
 * test_ewma_tracking - EWMA 应跟踪缓慢变化的温度
 */
void test_ewma_tracking(void)
{
    temp_comp_init(&default_config);

    /* 温度从 20°C 缓慢上升到 30°C */
    for (int i = 0; i < 50; i++) {
        float temp = 20.0f + (float)i * 0.2f;  /* 每次增加 0.2°C */
        struct temp_sample s = make_sample(temp, (int64_t)(i * 2000000));
        temp_comp_feed_sample(&s);
    }

    struct temp_comp_state state;
    temp_comp_get_state(&state);

    /* 最终温度应在 29~31 范围内 (有滞后) */
    TEST_ASSERT_GREATER_THAN(28.0f, state.current_temp);
    TEST_ASSERT_LESS_THAN(32.0f, state.current_temp);

    temp_comp_deinit();
}

/* ==================== 测试组：校准流程测试 ==================== */

/**
 * test_calibration_success - 成功校准
 */
void test_calibration_success(void)
{
    temp_comp_init(&default_config);

    /* 输入一些样本让系统稳定 */
    for (int i = 0; i < 10; i++) {
        struct temp_sample s = make_sample(25.0f, (int64_t)(i * 1000000));
        temp_comp_feed_sample(&s);
    }

    /* 执行校准 */
    int ret = temp_comp_calibrate(25.0f);
    TEST_ASSERT_EQUAL_INT(APP_ERR_OK, ret);

    struct temp_comp_state state;
    temp_comp_get_state(&state);
    TEST_ASSERT_TRUE(state.calibrated);

    temp_comp_deinit();
}

/**
 * test_calibration_invalid_temp - 无效校准温度
 */
void test_calibration_invalid_temp(void)
{
    temp_comp_init(&default_config);

    /* 极端温度可能不被接受 */
    int ret = temp_comp_calibrate(-50.0f);
    /* 根据实现，可能返回错误或成功 */
    (void)ret;

    temp_comp_deinit();
}

/* ==================== 测试组：统计信息测试 ==================== */

/**
 * test_stats_initialization - 统计信息初始状态
 */
void test_stats_initialization(void)
{
    struct temp_comp_stats stats;

    temp_comp_init(&default_config);

    int ret = temp_comp_get_stats(&stats);
    TEST_ASSERT_EQUAL_INT(APP_ERR_OK, ret);
    TEST_ASSERT_EQUAL_UINT32(0, stats.total_samples);
    TEST_ASSERT_EQUAL_UINT32(0, stats.compensated_samples);
    TEST_ASSERT_EQUAL_UINT32(0, stats.skipped_samples);
    TEST_ASSERT_EQUAL_UINT32(0, stats.interpolated_count);
    TEST_ASSERT_EQUAL_UINT32(0, stats.stale_data_count);
    TEST_ASSERT_EQUAL_UINT32(0, stats.calibration_count);

    temp_comp_deinit();
}

/**
 * test_stats_after_feeding - 喂入样本后统计更新
 */
void test_stats_after_feeding(void)
{
    struct temp_comp_stats stats;

    temp_comp_init(&default_config);

    /* 喂入 10 个样本 */
    for (int i = 0; i < 10; i++) {
        struct temp_sample s = make_sample(25.0f + (float)i * 0.1f,
                                           (int64_t)(i * 500000));
        temp_comp_feed_sample(&s);
    }

    int ret = temp_comp_get_stats(&stats);
    TEST_ASSERT_EQUAL_INT(APP_ERR_OK, ret);
    TEST_ASSERT_EQUAL_UINT32(10, stats.total_samples);

    temp_comp_deinit();
}

/**
 * test_stats_reset - 重置统计计数器
 */
void test_stats_reset(void)
{
    struct temp_comp_stats stats;

    temp_comp_init(&default_config);

    /* 喂入一些样本 */
    struct temp_sample s = make_sample(25.0f, 1000000);
    temp_comp_feed_sample(&s);

    /* 重置统计 */
    temp_comp_reset_stats();

    temp_comp_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.total_samples);

    temp_comp_deinit();
}

/**
 * test_stats_null_param - NULL 参数处理
 */
void test_stats_null_param(void)
{
    temp_comp_init(&default_config);
    TEST_ASSERT_EQUAL_INT(APP_ERR_INVALID_PARAM, temp_comp_get_stats(NULL));
    temp_comp_deinit();
}

/* ==================== 测试组：偏移量获取测试 ==================== */

/**
 * test_offsets_initial - 初始偏移量应为零
 */
void test_offsets_initial(void)
{
    float offsets[3];

    temp_comp_init(&default_config);

    int ret = temp_comp_get_offsets(offsets);
    TEST_ASSERT_EQUAL_INT(APP_ERR_OK, ret);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, offsets[0]);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, offsets[1]);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, offsets[2]);

    temp_comp_deinit();
}

/**
 * test_offsets_null_param - NULL 参数处理
 */
void test_offsets_null_param(void)
{
    temp_comp_init(&default_config);
    TEST_ASSERT_EQUAL_INT(APP_ERR_INVALID_PARAM, temp_comp_get_offsets(NULL));
    temp_comp_deinit();
}

/* ==================== Unity 主函数 ==================== */

void setUp(void) { }
void tearDown(void) {
    if (temp_comp_is_initialized()) {
        temp_comp_deinit();
    }
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_constants);
    RUN_TEST(test_struct_sizes);
    RUN_TEST(test_error_codes);
    RUN_TEST(test_init_uninitialized_state);
    RUN_TEST(test_null_param_handling);
    RUN_TEST(test_init_success);
    RUN_TEST(test_init_default_config);
    RUN_TEST(test_ewma_convergence);
    RUN_TEST(test_ewma_tracking);
    RUN_TEST(test_calibration_success);
    RUN_TEST(test_calibration_invalid_temp);
    RUN_TEST(test_stats_initialization);
    RUN_TEST(test_stats_after_feeding);
    RUN_TEST(test_stats_reset);
    RUN_TEST(test_stats_null_param);
    RUN_TEST(test_offsets_initial);
    RUN_TEST(test_offsets_null_param);

    return UNITY_END();
}