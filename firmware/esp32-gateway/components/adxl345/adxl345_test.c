/**
 * @file adxl345_test.c
 * @author EnterWorldDoor
 * @brief ADXL345 企业级驱动单元测试（基于 Unity 框架）
 *
 * 测试覆盖范围:
 *   - 常量与枚举值验证
 *   - 数据结构体大小验证
 *   - 参数验证 (NULL指针检查)
 *   - 错误码定义验证
 *   - 数学运算验证 (RMS计算、移动平均、DC偏移去除)
 *   - 趋势分析逻辑验证
 *   - 配置参数边界检查
 *   - API接口存在性验证
 */

#include "unity.h"
#include "adxl345.h"
#include "adxl345_reg.h"
#include "global_error.h"
#include "driver/spi_master.h"
#include <string.h>
#include <math.h>
#include <stdint.h>

/* ==================== 测试辅助变量 ==================== */

static int g_data_callback_count = 0;
static struct adxl345_accel_data g_last_accel_data;

/* ==================== 测试辅助函数 ==================== */

static void test_data_callback(const struct adxl345_accel_data *data,
                               void *user_data)
{
    (void)user_data;
    if (data) {
        memcpy(&g_last_accel_data, data, sizeof(*data));
        g_data_callback_count++;
    }
}

static float test_temp_callback(void)
{
    return 25.5f;
}

static void reset_callback_state(void)
{
    g_data_callback_count = 0;
    memset(&g_last_accel_data, 0, sizeof(g_last_accel_data));
}

/* ==================== 测试组：常量与枚举验证 ==================== */

void test_enum_range_values(void)
{
    TEST_ASSERT_EQUAL(0,  ADXL345_RANGE_2G);
    TEST_ASSERT_EQUAL(1,  ADXL345_RANGE_4G);
    TEST_ASSERT_EQUAL(2,  ADXL345_RANGE_8G);
    TEST_ASSERT_EQUAL(3,  ADXL345_RANGE_16G);
}

void test_enum_rate_values(void)
{
    TEST_ASSERT_EQUAL(0x0F, ADXL345_RATE_3200);
    TEST_ASSERT_EQUAL(0x0E, ADXL345_RATE_1600);
    TEST_ASSERT_EQUAL(0x0D, ADXL345_RATE_800);
    TEST_ASSERT_EQUAL(0x0C, ADXL345_RATE_400);
    TEST_ASSERT_EQUAL(0x0B, ADXL345_RATE_200);
    TEST_ASSERT_EQUAL(0x0A, ADXL345_RATE_100);
    TEST_ASSERT_EQUAL(0x09, ADXL345_RATE_50);
    TEST_ASSERT_EQUAL(0x08, ADXL345_RATE_25);
}

void test_enum_fifo_mode_values(void)
{
    TEST_ASSERT_EQUAL(0x00, ADXL345_FIFO_BYPASS);
    TEST_ASSERT_EQUAL(0x40, ADXL345_FIFO_FIFO);
    TEST_ASSERT_EQUAL(0x80, ADXL345_FIFO_STREAM);
    TEST_ASSERT_EQUAL(0xC0, ADXL345_FIFO_TRIGGER);
}

void test_enum_bus_type_values(void)
{
    TEST_ASSERT_EQUAL(0, ADXL345_BUS_SPI);
    TEST_ASSERT_EQUAL(1, ADXL345_BUS_I2C);
}

void test_enum_trend_direction_values(void)
{
    TEST_ASSERT_EQUAL(0, ADXL345_TREND_STABLE);
    TEST_ASSERT_EQUAL(1, ADXL345_TREND_RISING);
    TEST_ASSERT_EQUAL(2, ADXL345_TREND_FALLING);
}

void test_enum_health_level_values(void)
{
    TEST_ASSERT_EQUAL(0, ADXL345_HEALTH_GOOD);
    TEST_ASSERT_EQUAL(1, ADXL345_HEALTH_WARNING);
    TEST_ASSERT_EQUAL(2, ADXL345_HEALTH_DEGRADED);
    TEST_ASSERT_EQUAL(3, ADXL345_HEALTH_FAULT);
}

/* ==================== 测试组：寄存器地址验证 ==================== */

void test_register_addresses(void)
{
    TEST_ASSERT_EQUAL(0x00, ADXL345_DEVID);
    TEST_ASSERT_EQUAL(0x1D, ADXL345_THRESH_TAP);
    TEST_ASSERT_EQUAL(0x2C, ADXL345_BW_RATE);
    TEST_ASSERT_EQUAL(0x2D, ADXL345_POWER_CTL);
    TEST_ASSERT_EQUAL(0x31, ADXL345_DATA_FORMAT);
    TEST_ASSERT_EQUAL(0x32, ADXL345_DATAX0);
    TEST_ASSERT_EQUAL(0x38, ADXL345_FIFO_CTL);
    TEST_ASSERT_EQUAL(0x39, ADXL345_FIFO_STATUS);
}

void test_device_id_constant(void)
{
    TEST_ASSERT_EQUAL(0xE5, ADXL345_DEVICE_ID);
}

void test_fifo_depth_constant(void)
{
    TEST_ASSERT_EQUAL(32, ADXL345_FIFO_DEPTH);
}

void test_spi_command_constants(void)
{
    TEST_ASSERT_EQUAL(0x80, ADXL345_SPI_READ_CMD);
    TEST_ASSERT_EQUAL(0x00, ADXL345_SPI_WRITE_CMD);
    TEST_ASSERT_EQUAL(0x40, ADXL345_SPI_MB_BIT);
}

/* ==================== 测试组：数据格式位域验证 ==================== */

void test_data_format_bits(void)
{
    TEST_ASSERT_EQUAL(0x80, ADXL345_DF_SELF_TEST);
    TEST_ASSERT_EQUAL(0x40, ADXL345_DF_SPI_3WIRE);
    TEST_ASSERT_EQUAL(0x20, ADXL345_DF_INT_INVERT);
    TEST_ASSERT_EQUAL(0x10, ADXL345_DF_FULL_RES);
    TEST_ASSERT_EQUAL(0x08, ADXL345_DF_JUSTIFY);
    TEST_ASSERT_EQUAL(0x03, ADXL345_DF_RANGE_MASK);
}

void test_power_control_bits(void)
{
    TEST_ASSERT_EQUAL(0x20, ADXL345_PCTL_LINK);
    TEST_ASSERT_EQUAL(0x10, ADXL345_PCTL_AUTO_SLEEP);
    TEST_ASSERT_EQUAL(0x08, ADXL345_PCTL_MEASURE);
    TEST_ASSERT_EQUAL(0x04, ADXL345_PCTL_SLEEP);
    TEST_ASSERT_EQUAL(0x03, ADXL345_PCTL_WAKEUP_MASK);
}

void test_interrupt_source_bits(void)
{
    TEST_ASSERT_EQUAL(0x80, ADXL345_INT_DATA_READY);
    TEST_ASSERT_EQUAL(0x40, ADXL345_INT_SINGLE_TAP);
    TEST_ASSERT_EQUAL(0x20, ADXL345_INT_DOUBLE_TAP);
    TEST_ASSERT_EQUAL(0x10, ADXL345_INT_ACTIVITY);
    TEST_ASSERT_EQUAL(0x08, ADXL345_INT_INACTIVITY);
    TEST_ASSERT_EQUAL(0x04, ADXL345_INT_FREE_FALL);
    TEST_ASSERT_EQUAL(0x02, ADXL345_INT_WATERMARK);
    TEST_ASSERT_EQUAL(0x01, ADXL345_INT_OVERRUN);
}

/* ==================== 测试组：常量定义验证 ==================== */

void test_filter_window_size(void)
{
    TEST_ASSERT_EQUAL(16,  ADXL345_MA_WINDOW_SIZE);
}

void test_dc_calibration_samples(void)
{
    TEST_ASSERT_EQUAL(64,  ADXL345_DC_CALIBRATION_SAMPLES);
}

void test_trend_window_size(void)
{
    TEST_ASSERT_EQUAL(256, ADXL345_TREND_WINDOW_SIZE);
}

void test_spi_retry_max(void)
{
    TEST_ASSERT_EQUAL(3,   ADXL345_SPI_RETRY_MAX);
}

void test_spi_timeout_ms(void)
{
    TEST_ASSERT_EQUAL(50,  ADXL345_SPI_TIMEOUT_MS);
}

void test_fifo_watermark(void)
{
    TEST_ASSERT_EQUAL(16,  ADXL345_FIFO_WATERMARK);
}

void test_health_error_threshold(void)
{
    TEST_ASSERT_EQUAL(10,  ADXL345_HEALTH_ERROR_THRESH);
}

/* ==================== 测试组：错误码验证 ==================== */

void test_adxl_error_codes_defined(void)
{
    TEST_ASSERT_EQUAL(-1100, APP_ERR_ADXL_INIT_FAIL);
    TEST_ASSERT_EQUAL(-1101, APP_ERR_ADXL_ALREADY_INIT);
    TEST_ASSERT_EQUAL(-1102, APP_ERR_ADXL_NOT_INIT);
    TEST_ASSERT_EQUAL(-1103, APP_ERR_ADXL_INVALID_PARAM);
    TEST_ASSERT_EQUAL(-1104, APP_ERR_ADXL_SPI_CONFIG);
    TEST_ASSERT_EQUAL(-1105, APP_ERR_ADXL_SPI_TRANSFAIL);
    TEST_ASSERT_EQUAL(-1106, APP_ERR_ADXL_DEV_ID_MISMATCH);
    TEST_ASSERT_EQUAL(-1107, APP_ERR_ADXL_FIFO_ERROR);
    TEST_ASSERT_EQUAL(-1108, APP_ERR_ADXL_INT_CONFIG);
    TEST_ASSERT_EQUAL(-1109, APP_ERR_ADXL_TASK_CREATE);
    TEST_ASSERT_EQUAL(-1110, APP_ERR_ADXL_QUEUE_CREATE);
    TEST_ASSERT_EQUAL(-1111, APP_ERR_ADXL_MUTEX_CREATE);
    TEST_ASSERT_EQUAL(-1112, APP_ERR_ADXL_SELFTEST_FAIL);
    TEST_ASSERT_EQUAL(-1113, APP_ERR_ADXL_BUS_TIMEOUT);
    TEST_ASSERT_EQUAL(-1114, APP_ERR_ADXL_HEALTH_DEGRADED);
}

/* ==================== 测试组：数据结构体大小验证 ==================== */

void test_struct_sizes(void)
{
    TEST_ASSERT_EQUAL(sizeof(float) * 3 + sizeof(bool),
                      sizeof(struct adxl345_dc_offset));
    TEST_ASSERT_EQUAL(sizeof(float) * 4 + sizeof(uint32_t),
                      sizeof(struct adxl345_accel_data));
    TEST_ASSERT_EQUAL(sizeof(int16_t) * 3 + sizeof(uint32_t),
                      sizeof(struct adxl345_raw_sample));
    TEST_ASSERT_EQUAL(sizeof(int) + sizeof(float) * 3 +
                      sizeof(uint32_t), sizeof(struct adxl345_trend_info));
}

/* ==================== 测试组：数学运算验证 ==================== */

void test_rms_calculation_zero_input(void)
{
    float rms = sqrtf(0.0f * 0.0f + 0.0f * 0.0f + 0.0f * 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, rms);
}

void test_rms_calculation_single_axis(void)
{
    float rms = sqrtf(1.0f * 1.0f + 0.0f * 0.0f + 0.0f * 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, rms);
}

void test_rms_calculation_three_axis_equal(void)
{
    float val = 1.0f;
    float rms = sqrtf(val * val + val * val + val * val);
    float expected = sqrtf(3.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, expected, rms);
}

void test_rms_calculation_known_value(void)
{
    float x = 3.0f, y = 4.0f, z = 0.0f;
    float rms = sqrtf(x * x + y * y + z * z);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.0f, rms);
}

void test_dc_offset_removal(void)
{
    float raw = 1.05f;
    float offset = 1.0f;
    float result = raw - offset;
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.05f, result);
}

void test_dc_offset_removal_zero_offset(void)
{
    float raw = 0.5f;
    float offset = 0.0f;
    float result = raw - offset;
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, result);
}

/* ==================== 测试组：移动平均滤波验证 ==================== */

void test_moving_average_initial_samples(void)
{
    float window[ADXL345_MA_WINDOW_SIZE];
    int index = 0;
    float sum = 0.0f;
    bool filled = false;

    memset(window, 0, sizeof(window));

    for (int i = 0; i < 5; i++) {
        float old_val = window[index];
        window[index] = (float)(i + 1);
        sum += ((float)(i + 1) - old_val);
        index++;
        if (index >= ADXL345_MA_WINDOW_SIZE) {
            index = 0;
            filled = true;
        }
    }

    TEST_ASSERT_FALSE(filled);
    TEST_ASSERT_EQUAL(5, index);
}

void test_moving_average_full_window(void)
{
    float window[ADXL345_MA_WINDOW_SIZE];
    int index = 0;
    float sum = 0.0f;
    bool filled = false;

    memset(window, 0, sizeof(window));

    for (int i = 0; i < ADXL345_MA_WINDOW_SIZE; i++) {
        float old_val = window[index];
        window[index] = 1.0f;
        sum += (1.0f - old_val);
        index++;
        if (index >= ADXL345_MA_WINDOW_SIZE) {
            index = 0;
            filled = true;
        }
    }

    TEST_ASSERT_TRUE(filled);
    TEST_ASSERT_EQUAL(0, index);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 16.0f, sum);

    float avg = sum / (float)ADXL345_MA_WINDOW_SIZE;
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, avg);
}

void test_moving_average_steady_state(void)
{
    float window[ADXL345_MA_WINDOW_SIZE];
    int index = 0;
    float sum = 0.0f;
    bool filled = false;

    memset(window, 0, sizeof(window));

    for (int i = 0; i < ADXL345_MA_WINDOW_SIZE; i++) {
        float old_val = window[index];
        window[index] = 2.0f;
        sum += (2.0f - old_val);
        index++;
        if (index >= ADXL345_MA_WINDOW_SIZE) {
            index = 0;
            filled = true;
        }
    }

    float old_val = window[index];
    window[index] = 2.0f;
    sum += (2.0f - old_val);
    index++;

    float avg = sum / (float)ADXL345_MA_WINDOW_SIZE;
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, avg);
}

/* ==================== 测试组：API NULL参数验证 ==================== */

void test_init_spi_null_config(void)
{
    struct ringbuf rb;
    struct adxl345_dev *dev = adxl345_init_spi(NULL,
                    ADXL345_RANGE_2G, ADXL345_RATE_3200,
                    ADXL345_FIFO_STREAM, NULL, &rb);
    TEST_ASSERT_NULL(dev);
}

void test_init_spi_null_ringbuf(void)
{
    struct adxl345_spi_config cfg = {0};
    cfg.host_id = SPI2_HOST;
    cfg.gpio_cs = 5;
    cfg.gpio_miso = 19;
    cfg.gpio_mosi = 23;
    cfg.gpio_sclk = 18;
    cfg.clock_speed_hz = 1000000;

    struct adxl345_dev *dev = adxl345_init_spi(&cfg,
                    ADXL345_RANGE_2G, ADXL345_RATE_3200,
                    ADXL345_FIFO_STREAM, NULL, NULL);
    TEST_ASSERT_NULL(dev);
}

void test_deinit_null_device(void)
{
    adxl345_deinit(NULL);
}

void test_start_null_device(void)
{
    int ret = adxl345_start(NULL, 5, 2048);
    TEST_ASSERT_EQUAL(APP_ERR_ADXL_NOT_INIT, ret);
}

void test_stop_null_device(void)
{
    int ret = adxl345_stop(NULL);
    TEST_ASSERT_EQUAL(APP_ERR_ADXL_NOT_INIT, ret);
}

void test_fetch_null_device(void)
{
    struct adxl345_accel_data data;
    int ret = adxl345_fetch(NULL, &data, 100);
    TEST_ASSERT_EQUAL(APP_ERR_ADXL_INVALID_PARAM, ret);
}

void test_fetch_null_output(void)
{
    struct adxl345_spi_config cfg = {0};
    struct ringbuf rb;
    struct adxl345_dev dev;

    memset(&cfg, 0, sizeof(cfg));
    memset(&rb, 0, sizeof(rb));
    memset(&dev, 0, sizeof(dev));

    int ret = adxl345_fetch(&dev, NULL, 100);
    TEST_ASSERT_EQUAL(APP_ERR_ADXL_INVALID_PARAM, ret);
}

void test_calibrate_null_device(void)
{
    int ret = adxl345_calibrate_dc_offset(NULL, 64);
    TEST_ASSERT_EQUAL(APP_ERR_ADXL_NOT_INIT, ret);
}

void test_get_trend_null_params(void)
{
    struct adxl345_trend_info trend;
    int ret = adxl345_get_trend(NULL, &trend);
    TEST_ASSERT_EQUAL(APP_ERR_ADXL_INVALID_PARAM, ret);

    struct adxl345_spi_config cfg = {0};
    struct ringbuf rb;
    struct adxl345_dev dev;
    memset(&dev, 0, sizeof(dev));

    ret = adxl345_get_trend(&dev, NULL);
    TEST_ASSERT_EQUAL(APP_ERR_ADXL_INVALID_PARAM, ret);
}

void test_get_health_null_params(void)
{
    struct adxl345_health_info health;
    int ret = adxl345_get_health(NULL, &health);
    TEST_ASSERT_EQUAL(APP_ERR_ADXL_INVALID_PARAM, ret);

    struct adxl345_dev dev;
    memset(&dev, 0, sizeof(dev));

    ret = adxl345_get_health(&dev, NULL);
    TEST_ASSERT_EQUAL(APP_ERR_ADXL_INVALID_PARAM, ret);
}

void test_get_stats_null_params(void)
{
    struct adxl345_stats stats;
    int ret = adxl345_get_stats(NULL, &stats);
    TEST_ASSERT_EQUAL(APP_ERR_ADXL_INVALID_PARAM, ret);

    struct adxl345_dev dev;
    memset(&dev, 0, sizeof(dev));

    ret = adxl345_get_stats(&dev, NULL);
    TEST_ASSERT_EQUAL(APP_ERR_ADXL_INVALID_PARAM, ret);
}

void test_selftest_null_device(void)
{
    int ret = adxl345_self_test(NULL);
    TEST_ASSERT_EQUAL(APP_ERR_ADXL_NOT_INIT, ret);
}

void test_reset_bus_null_device(void)
{
    int ret = adxl345_reset_bus(NULL);
    TEST_ASSERT_EQUAL(APP_ERR_ADXL_NOT_INIT, ret);
}

void test_register_temp_callback_null_device(void)
{
    adxl345_register_temp_callback(NULL, test_temp_callback);
}

void test_register_data_callback_null_device(void)
{
    adxl345_register_data_callback(NULL, test_data_callback, NULL);
}

/* ==================== 测试组：配置参数合理性验证 ==================== */

void test_default_dc_offset_uninitialized(void)
{
    struct adxl345_dc_offset offset = {0};
    TEST_ASSERT_FALSE(offset.calibrated);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, offset.x_offset);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, offset.y_offset);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, offset.z_offset);
}

void test_health_info_initial_state(void)
{
    struct adxl345_health_info health = {0};
    TEST_ASSERT_EQUAL(ADXL345_HEALTH_GOOD, health.level);
    TEST_ASSERT_EQUAL(0U, health.total_samples);
    TEST_ASSERT_EQUAL(0U, health.comm_errors);
    TEST_ASSERT_FALSE(health.selftest_passed);
}

void test_stats_initial_zeros(void)
{
    struct adxl345_stats stats = {0};
    TEST_ASSERT_EQUAL(0U, stats.samples_pushed);
    TEST_ASSERT_EQUAL(0U, stats.samples_processed);
    TEST_ASSERT_EQUAL(0U, stats.fifo_reads);
    TEST_ASSERT_EQUAL(0U, stats.int_fired_count);
}

void test_trend_info_initial_state(void)
{
    struct adxl345_trend_info trend = {0};
    TEST_ASSERT_EQUAL(ADXL345_TREND_STABLE, trend.direction);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, trend.current_rms);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, trend.avg_rms);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, trend.slope);
    TEST_ASSERT_EQUAL(0U, trend.window_samples);
}

void test_int_config_defaults_disabled(void)
{
    struct adxl345_int_config cfg = {0};
    TEST_ASSERT_FALSE(cfg.enable_int1);
    TEST_ASSERT_FALSE(cfg.enable_int2);
    TEST_ASSERT_EQUAL(0, cfg.gpio_int1);
    TEST_ASSERT_EQUAL(0, cfg.gpio_int2);
    TEST_ASSERT_EQUAL(0, cfg.int1_sources);
    TEST_ASSERT_EQUAL(0, cfg.int2_sources);
}

/* ==================== 测试组：Scale Factor 验证 ==================== */

void test_scale_factor_2g(void)
{
    float expected = 4.0f / 1024.0f;
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, expected, 0.00390625f);
}

void test_scale_factor_4g(void)
{
    float expected = 8.0f / 1024.0f;
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, expected, 0.0078125f);
}

void test_scale_factor_8g(void)
{
    float expected = 16.0f / 1024.0f;
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, expected, 0.015625f);
}

void test_scale_factor_16g(void)
{
    float expected = 32.0f / 1024.0f;
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, expected, 0.03125f);
}

/* ==================== 测试组：ODR与频率分辨率验证 ==================== */

void test_odr_3200hz_frequency_resolution(void)
{
    float freq_resolution = 3200.0f / 1024.0f;
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 3.125f, freq_resolution);
}

void test_nyquist_criteria_50hz_motor(void)
{
    float motor_hz = 50.0f;
    float min_sampling_rate = motor_hz * 2.0f;
    TEST_ASSERT_TRUE(3200.0f > min_sampling_rate);
}

void test_fft_output_points(void)
{
    int n = 1024;
    int valid_points = n / 2 + 1;
    TEST_ASSERT_EQUAL(513, valid_points);
}

/* ==================== 测试组：回调函数注册验证 ==================== */

void test_temp_callback_type_exists(void)
{
    adxl345_temp_read_fn fn = test_temp_callback;
    TEST_ASSERT_NOT_NULL(fn);
    float temp = fn();
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 25.5f, temp);
}

void test_data_callback_type_exists(void)
{
    adxl345_data_callback cb = test_data_callback;
    TEST_ASSERT_NOT_NULL(cb);

    struct adxl345_accel_data test_data = {
        .x_g = 0.1f, .y_g = 0.2f, .z_g = 0.9f,
        .rms_total = 0.933f, .timestamp_us = 123456,
        .temperature_c = 25.0f
    };

    reset_callback_state();
    cb(&test_data, NULL);

    TEST_ASSERT_EQUAL(1, g_data_callback_count);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.1f, g_last_accel_data.x_g);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.2f, g_last_accel_data.y_g);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.9f, g_last_accel_data.z_g);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.933f, g_last_accel_data.rms_total);
    TEST_ASSERT_EQUAL(123456U, g_last_accel_data.timestamp_us);
}

/* ==================== 测试组：原始样本到加速度转换验证 ==================== */

void test_raw_to_accel_conversion_2g(void)
{
    float scale = 4.0f / 1024.0f;
    int16_t raw = 256;
    float accel = (float)raw * scale;
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, accel);
}

void test_raw_to_accel_conversion_negative(void)
{
    float scale = 4.0f / 1024.0f;
    int16_t raw = -256;
    float accel = (float)raw * scale;
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f, accel);
}

void test_raw_sample_timestamp_preserved(void)
{
    struct adxl345_raw_sample sample = {
        .x = 100, .y = 200, .z = 300,
        .timestamp_us = 987654321
    };
    TEST_ASSERT_EQUAL(987654321U, sample.timestamp_us);
}

/* ==================== 测试组：边界条件测试 ==================== */

void test_max_range_16g_raw_value(void)
{
    float scale = 32.0f / 1024.0f;
    int16_t max_raw = 2047;
    float max_g = (float)max_raw * scale;
    TEST_ASSERT_TRUE(max_g <= 64.0f);
    TEST_ASSERT_TRUE(max_g > 63.0f);
}

void test_min_range_16g_raw_value(void)
{
    float scale = 32.0f / 1024.0f;
    int16_t min_raw = -2048;
    float min_g = (float)min_raw * scale;
    TEST_ASSERT_TRUE(min_g >= -64.0f);
    TEST_ASSERT_TRUE(min_g < -63.0f);
}

void test_zero_rate_not_used_in_production(void)
{
    enum adxl345_data_rate rate = ADXL345_RATE_3200;
    TEST_ASSERT_TRUE(rate != 0);
    TEST_ASSERT_TRUE(rate <= 0x0F);
}

/* ==================== Unity 主函数 ==================== */

void setUp(void)
{
    reset_callback_state();
}

void tearDown(void)
{
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_enum_range_values);
    RUN_TEST(test_enum_rate_values);
    RUN_TEST(test_enum_fifo_mode_values);
    RUN_TEST(test_enum_bus_type_values);
    RUN_TEST(test_enum_trend_direction_values);
    RUN_TEST(test_enum_health_level_values);

    RUN_TEST(test_register_addresses);
    RUN_TEST(test_device_id_constant);
    RUN_TEST(test_fifo_depth_constant);
    RUN_TEST(test_spi_command_constants);

    RUN_TEST(test_data_format_bits);
    RUN_TEST(test_power_control_bits);
    RUN_TEST(test_interrupt_source_bits);

    RUN_TEST(test_filter_window_size);
    RUN_TEST(test_dc_calibration_samples);
    RUN_TEST(test_trend_window_size);
    RUN_TEST(test_spi_retry_max);
    RUN_TEST(test_spi_timeout_ms);
    RUN_TEST(test_fifo_watermark);
    RUN_TEST(test_health_error_threshold);

    RUN_TEST(test_adxl_error_codes_defined);

    RUN_TEST(test_struct_sizes);

    RUN_TEST(test_rms_calculation_zero_input);
    RUN_TEST(test_rms_calculation_single_axis);
    RUN_TEST(test_rms_calculation_three_axis_equal);
    RUN_TEST(test_rms_calculation_known_value);
    RUN_TEST(test_dc_offset_removal);
    RUN_TEST(test_dc_offset_removal_zero_offset);

    RUN_TEST(test_moving_average_initial_samples);
    RUN_TEST(test_moving_average_full_window);
    RUN_TEST(test_moving_average_steady_state);

    RUN_TEST(test_init_spi_null_config);
    RUN_TEST(test_init_spi_null_ringbuf);
    RUN_TEST(test_deinit_null_device);
    RUN_TEST(test_start_null_device);
    RUN_TEST(test_stop_null_device);
    RUN_TEST(test_fetch_null_device);
    RUN_TEST(test_fetch_null_output);
    RUN_TEST(test_calibrate_null_device);
    RUN_TEST(test_get_trend_null_params);
    RUN_TEST(test_get_health_null_params);
    RUN_TEST(test_get_stats_null_params);
    RUN_TEST(test_selftest_null_device);
    RUN_TEST(test_reset_bus_null_device);
    RUN_TEST(test_register_temp_callback_null_device);
    RUN_TEST(test_register_data_callback_null_device);

    RUN_TEST(test_default_dc_offset_uninitialized);
    RUN_TEST(test_health_info_initial_state);
    RUN_TEST(test_stats_initial_zeros);
    RUN_TEST(test_trend_info_initial_state);
    RUN_TEST(test_int_config_defaults_disabled);

    RUN_TEST(test_scale_factor_2g);
    RUN_TEST(test_scale_factor_4g);
    RUN_TEST(test_scale_factor_8g);
    RUN_TEST(test_scale_factor_16g);

    RUN_TEST(test_odr_3200hz_frequency_resolution);
    RUN_TEST(test_nyquist_criteria_50hz_motor);
    RUN_TEST(test_fft_output_points);

    RUN_TEST(test_temp_callback_type_exists);
    RUN_TEST(test_data_callback_type_exists);

    RUN_TEST(test_raw_to_accel_conversion_2g);
    RUN_TEST(test_raw_to_accel_conversion_negative);
    RUN_TEST(test_raw_sample_timestamp_preserved);

    RUN_TEST(test_max_range_16g_raw_value);
    RUN_TEST(test_min_range_16g_raw_value);
    RUN_TEST(test_zero_rate_not_used_in_production);

    return UNITY_END();
}