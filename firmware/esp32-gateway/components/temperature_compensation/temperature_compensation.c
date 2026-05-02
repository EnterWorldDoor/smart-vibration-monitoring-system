/**
 * @file temperature_compensation.c
 * @author EnterWorldDoor
 * @brief 自适应温度漂移补偿实现 (EWMA + 插值 + 变化率检测)
 *
 * 算法细节:
 *   - EWMA: T[n] = α * T_new + (1-α) * T[n-1]
 *   - 线性插值: T_interp = T1 + (T2-T1) * (t-t1)/(t2-t1)
 *   - 变化率: dT/dt = |T_now - T_last| / Δt
 *   - 偏移更新: offset += sens * (T_now - T_ref)
 */

#include "temperature_compensation.h"
#include "time_sync.h"
#include "log_system.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* ==================== 模块内部状态 ==================== */

static struct {
    /* 配置 */
    struct temp_comp_config config;

    /* 运行状态 */
    struct temp_comp_state state;
    struct temp_comp_stats stats;

    /* 温度历史环形缓冲区 (用于插值) */
    struct temp_sample history[TEMP_COMP_MAX_HISTORY];
    int history_head;
    int history_count;

    /* 线程安全 */
    SemaphoreHandle_t mutex;

    /* 初始化标志 */
    bool initialized;
} g_tc = {0};

/* ==================== 内部辅助函数 ==================== */

/**
 * get_elapsed_ms - 计算时间差 (毫秒)
 */
static inline int64_t get_elapsed_ms(int64_t t_start, int64_t t_end)
{
    return (t_end - t_start) / 1000;
}

/**
 * linear_interpolate - 线性插值
 * @t_target: 目标时间戳
 * @t1, t2: 两个采样点时间戳
 * @v1, v2: 两个采样点的值
 *
 * Return: 插值结果
 */
static float linear_interpolate(int64_t t_target,
                                 int64_t t1, float v1,
                                 int64_t t2, float v2)
{
    if (t2 == t1) return v1;

    float ratio = (float)(t_target - t1) / (float)(t2 - t1);
    return v1 + (v2 - v1) * ratio;
}

/**
 * find_surrounding_samples - 查找目标时间戳前后的两个历史样本
 * @target_ts: 目标时间戳
 * @out_earlier: 输出较早的样本 (可为 NULL)
 * @out_later: 输出较晚的样本 (可为 NULL)
 *
 * Return: true 找到配对, false 未找到
 */
static bool find_surrounding_samples(int64_t target_ts,
                                      const struct temp_sample **out_earlier,
                                      const struct temp_sample **out_later)
{
    if (g_tc.history_count < 2) return false;

    int idx = g_tc.history_head;
    const struct temp_sample *earlier = NULL;
    const struct temp_sample *later = NULL;

    for (int i = 0; i < g_tc.history_count; i++) {
        int actual_idx = (idx - 1 - i + TEMP_COMP_MAX_HISTORY) % TEMP_COMP_MAX_HISTORY;
        const struct temp_sample *sample = &g_tc.history[actual_idx];

        if (sample->timestamp_us <= target_ts) {
            earlier = sample;
        }
        if (later == NULL && sample->timestamp_us > target_ts) {
            later = sample;
        }

        if (earlier && later) break;
    }

    if (!earlier || !later) return false;

    if (out_earlier) *out_earlier = earlier;
    if (out_later) *out_later = later;
    return true;
}

/**
 * push_to_history - 将样本推入历史缓冲区
 */
static void push_to_history(const struct temp_sample *sample)
{
    g_tc.history[g_tc.history_head] = *sample;
    g_tc.history_head = (g_tc.history_head + 1) % TEMP_COMP_MAX_HISTORY;

    if (g_tc.history_count < TEMP_COMP_MAX_HISTORY) {
        g_tc.history_count++;
    }
}

/**
 * update_ewma - 更新 EWMA 滤波器
 */
static void update_ewma(float new_temp)
{
    if (!g_tc.state.calibrated) {
        g_tc.state.current_temp = new_temp;
        return;
    }

    float alpha = g_tc.config.ewma_alpha;
    g_tc.state.current_temp = alpha * new_temp +
                               (1.0f - alpha) * g_tc.state.current_temp;
}

/**
 * should_update_compensation - 判断是否需要更新偏移补偿
 *
 * 条件:
 *   1. 温度变化超过阈值 OR
 *   2. 温度变化率超过阈值 (快速温度波动)
 */
static bool should_update_compensation(float filtered_temp, int64_t now_us)
{
    float temp_diff = fabsf(filtered_temp - g_tc.state.last_compensated_temp);
    if (temp_diff >= g_tc.config.temp_change_threshold) {
        return true;
    }

    int64_t dt_us = now_us - g_tc.state.last_update_time_us;
    if (dt_us <= 0) dt_us = 1;

    float rate = temp_diff / ((float)dt_us / 1000000.0f);
    if (rate >= g_tc.config.rate_threshold) {
        LOG_DEBUG("TEMP_COMP", "High temp change rate: %.3f °C/s", rate);
        return true;
    }

    return false;
}

/**
 * apply_compensation - 应用偏移补偿到三轴加速度
 */
static void apply_compensation(float *x, float *y, float *z,
                                float delta_temp)
{
    float dx = g_tc.state.sensitivity_coeff_x * delta_temp;
    float dy = g_tc.state.sensitivity_coeff_y * delta_temp;
    float dz = g_tc.state.sensitivity_coeff_z * delta_temp;

    *x -= dx;
    *y -= dy;
    *z -= dz;

    g_tc.state.offset_x = dx;
    g_tc.state.offset_y = dy;
    g_tc.state.offset_z = dz;

    float abs_offset = sqrtf(dx*dx + dy*dy + dz*dz);
    if (abs_offset > g_tc.stats.max_offset_applied) {
        g_tc.stats.max_offset_applied = abs_offset;
    }
}

/* ==================== 公开 API 实现 ==================== */

int temp_comp_init(const struct temp_comp_config *config)
{
    if (g_tc.initialized) return APP_ERR_TEMP_NOT_INIT;

    memset(&g_tc, 0, sizeof(g_tc));

    g_tc.mutex = xSemaphoreCreateMutex();
    if (!g_tc.mutex) {
        LOG_ERROR("TEMP_COMP", "Failed to create mutex");
        return APP_ERR_NO_MEM;
    }

    /* 设置默认配置 */
    if (config) {
        memcpy(&g_tc.config, config, sizeof(*config));
    } else {
        g_tc.config.ewma_alpha = TEMP_COMP_DEFAULT_ALPHA;
        g_tc.config.temp_change_threshold = TEMP_COMP_TEMP_THRESHOLD;
        g_tc.config.rate_threshold = TEMP_COMP_RATE_THRESHOLD;
        g_tc.config.stale_timeout_ms = TEMP_COMP_STALE_TIMEOUT_MS;
        g_tc.config.interp_max_delta_ms = TEMP_COMP_INTERP_MAX_DELTA_MS;
    }

    /* 初始化状态 */
    g_tc.state.current_temp = NAN;
    g_tc.state.last_compensated_temp = NAN;
    g_tc.state.sensor_online = false;
    g_tc.state.calibrated = false;
    g_tc.state.sensitivity_coeff_x = 0.0f;
    g_tc.state.sensitivity_coeff_y = 0.0f;
    g_tc.state.sensitivity_coeff_z = 0.0f;

    g_tc.initialized = true;
    LOG_INFO("TEMP_COMP", "Temperature compensation initialized "
             "(alpha=%.2f, thresh=%.2f°C, rate_thresh=%.2f°C/s)",
             g_tc.config.ewma_alpha,
             g_tc.config.temp_change_threshold,
             g_tc.config.rate_threshold);
    return APP_ERR_OK;
}

int temp_comp_deinit(void)
{
    if (!g_tc.initialized) return APP_ERR_TEMP_NOT_INIT;

    if (g_tc.mutex) {
        vSemaphoreDelete(g_tc.mutex);
        g_tc.mutex = NULL;
    }

    memset(&g_tc, 0, sizeof(g_tc));
    g_tc.initialized = false;

    LOG_INFO("TEMP_COMP", "Temperature compensation deinitialized");
    return APP_ERR_OK;
}

void temp_comp_reset(void)
{
    if (!g_tc.initialized) return;

    xSemaphoreTake(g_tc.mutex, portMAX_DELAY);

    memset(&g_tc.state, 0, sizeof(g_tc.state));
    g_tc.state.current_temp = NAN;
    g_tc.state.last_compensated_temp = NAN;
    g_tc.state.calibrated = false;
    g_tc.history_head = 0;
    g_tc.history_count = 0;

    xSemaphoreGive(g_tc.mutex);
    LOG_INFO("TEMP_COMP", "Compensation state reset");
}

int temp_comp_push_temperature(float temperature, int64_t timestamp_us)
{
    if (!g_tc.initialized) return APP_ERR_TEMP_NOT_INIT;

    xSemaphoreTake(g_tc.mutex, portMAX_DELAY);

    struct temp_sample sample = {
        .temperature_c = temperature,
        .timestamp_us = timestamp_us
    };

    push_to_history(&sample);
    update_ewma(temperature);

    g_tc.state.sensor_online = true;
    g_tc.state.stale_count = 0;
    g_tc.stats.total_samples++;

    LOG_DEBUG("TEMP_COMP", "Pushed temp=%.2f°C ts=%lld us",
              temperature, (long long)timestamp_us);

    xSemaphoreGive(g_tc.mutex);
    return APP_ERR_OK;
}

bool temp_comp_is_sensor_online(void)
{
    if (!g_tc.initialized) return false;

    bool online;
    xSemaphoreTake(g_tc.mutex, portMAX_DELAY);
    online = g_tc.state.sensor_online;
    xSemaphoreGive(g_tc.mutex);
    return online;
}

int temp_comp_compensate(float *x, float *y, float *z,
                          int64_t current_timestamp_us)
{
    if (!g_tc.initialized) return APP_ERR_TEMP_NOT_INIT;
    if (!x || !y || !z) return APP_ERR_INVALID_PARAM;
    if (!g_tc.state.calibrated) {
        static uint32_t last_calib_warn = 0;
        uint32_t now_sec = (uint32_t)(current_timestamp_us / 1000000LL);
        if ((now_sec - last_calib_warn) >= 60) {
            LOG_DEBUG("TEMP_COMP", "Compensation not calibrated, passing through");
            last_calib_warn = now_sec;
        }
        g_tc.stats.skipped_samples++;
        return APP_ERR_OK;
    }

    xSemaphoreTake(g_tc.mutex, portMAX_DELAY);

    /* 检查是否有温度数据 */
    if (g_tc.history_count == 0 || isnan(g_tc.state.current_temp)) {
        g_tc.stats.skipped_samples++;
        xSemaphoreGive(g_tc.mutex);
        return APP_ERR_TEMP_NO_DATA;
    }

    /* 获取用于补偿的温度值 */
    float compensate_temp;
    int64_t latest_ts = g_tc.history[
        (g_tc.history_head - 1 + TEMP_COMP_MAX_HISTORY) % TEMP_COMP_MAX_HISTORY
    ].timestamp_us;

    int64_t time_delta_ms = get_elapsed_ms(latest_ts, current_timestamp_us);

    /* 检查数据是否陈旧 */
    if ((uint64_t)llabs(time_delta_ms) > g_tc.config.stale_timeout_ms) {
        g_tc.state.stale_count++;
        g_tc.stats.stale_data_count++;

        if (g_tc.state.stale_count > 10) {
            g_tc.state.sensor_online = false;
            static uint32_t last_offline_warn = 0;
            uint32_t now_sec = (uint32_t)(current_timestamp_us / 1000000LL);
            if ((now_sec - last_offline_warn) >= 60) {
                LOG_WARN("TEMP_COMP", "Temperature sensor appears offline "
                         "(stale for %llu ms)", (unsigned long long)llabs(time_delta_ms));
                last_offline_warn = now_sec;
            }
        }

        g_tc.stats.skipped_samples++;
        xSemaphoreGive(g_tc.mutex);
        return APP_ERR_TEMP_STALE_DATA;
    }

    g_tc.state.stale_count = 0;
    g_tc.state.sensor_online = true;

    /* 如果时间差较大，使用线性插值 */
    if ((uint64_t)llabs(time_delta_ms) > g_tc.config.interp_max_delta_ms &&
        g_tc.history_count >= 2) {

        const struct temp_sample *earlier = NULL;
        const struct temp_sample *later = NULL;

        if (find_surrounding_samples(current_timestamp_us, &earlier, &later)) {
            compensate_temp = linear_interpolate(
                current_timestamp_us,
                earlier->timestamp_us, earlier->temperature_c,
                later->timestamp_us, later->temperature_c
            );
            g_tc.stats.interpolated_count++;

            LOG_DEBUG("TEMP_COMP", "Interpolated temp=%.2f°C (Δt=%lld ms)",
                      compensate_temp, (long long)time_delta_ms);
        } else {
            compensate_temp = g_tc.state.current_temp;
        }
    } else {
        compensate_temp = g_tc.state.current_temp;
    }

    /* 判断是否需要更新补偿 */
    if (should_update_compensation(compensate_temp, current_timestamp_us)) {
        float delta_temp = compensate_temp - g_tc.state.last_compensated_temp;

        if (isnan(g_tc.state.last_compensated_temp)) {
            delta_temp = 0.0f;
        }

        apply_compensation(x, y, z, delta_temp);

        g_tc.state.last_compensated_temp = compensate_temp;
        g_tc.state.last_update_time_us = current_timestamp_us;
        g_tc.stats.compensated_samples++;

        LOG_DEBUG("TEMP_COMP", "Applied compensation: ΔT=%.3f°C "
                  "offset=(%.4f, %.4f, %.4f) g",
                  delta_temp,
                  g_tc.state.offset_x, g_tc.state.offset_y, g_tc.state.offset_z);
    } else {
        /* 使用当前已计算的偏移量 */
        *x -= g_tc.state.offset_x;
        *y -= g_tc.state.offset_y;
        *z -= g_tc.state.offset_z;
        g_tc.stats.compensated_samples++;
    }

    xSemaphoreGive(g_tc.mutex);
    return APP_ERR_OK;
}

int temp_comp_get_current_offset(float *offset_x, float *offset_y,
                                  float *offset_z)
{
    if (!g_tc.initialized) return APP_ERR_TEMP_NOT_INIT;
    if (!offset_x || !offset_y || !offset_z) return APP_ERR_INVALID_PARAM;

    xSemaphoreTake(g_tc.mutex, portMAX_DELAY);
    *offset_x = g_tc.state.offset_x;
    *offset_y = g_tc.state.offset_y;
    *offset_z = g_tc.state.offset_z;
    xSemaphoreGive(g_tc.mutex);

    return APP_ERR_OK;
}

int temp_comp_calibrate(float temp_low,
                         float offset_low_x, float offset_low_y, float offset_low_z,
                         float temp_high,
                         float offset_high_x, float offset_high_y, float offset_high_z)
{
    if (!g_tc.initialized) return APP_ERR_TEMP_NOT_INIT;

    float delta_temp = temp_high - temp_low;
    if (fabsf(delta_temp) < 1.0f) {
        LOG_ERROR("TEMP_COMP", "Calibration temperature range too small: %.2f°C",
                  delta_temp);
        return APP_ERR_TEMP_CALIBRATION_FAIL;
    }

    xSemaphoreTake(g_tc.mutex, portMAX_DELAY);

    g_tc.state.sensitivity_coeff_x =
        (offset_high_x - offset_low_x) / delta_temp;
    g_tc.state.sensitivity_coeff_y =
        (offset_high_y - offset_low_y) / delta_temp;
    g_tc.state.sensitivity_coeff_z =
        (offset_high_z - offset_low_z) / delta_temp;

    g_tc.state.calibrated = true;
    g_tc.state.last_compensated_temp = g_tc.state.current_temp;
    g_tc.stats.calibration_count++;

    LOG_INFO("TEMP_COMP", "Calibration complete: "
             "T_low=%.1f°C T_high=%.1f°C "
             "sens=(%.4f, %.4f, %.4f) g/°C",
             temp_low, temp_high,
             g_tc.state.sensitivity_coeff_x,
             g_tc.state.sensitivity_coeff_y,
             g_tc.state.sensitivity_coeff_z);

    xSemaphoreGive(g_tc.mutex);
    return APP_ERR_OK;
}

int temp_comp_set_sensitivity(float sens_x, float sens_y, float sens_z)
{
    if (!g_tc.initialized) return APP_ERR_TEMP_NOT_INIT;

    xSemaphoreTake(g_tc.mutex, portMAX_DELAY);
    g_tc.state.sensitivity_coeff_x = sens_x;
    g_tc.state.sensitivity_coeff_y = sens_y;
    g_tc.state.sensitivity_coeff_z = sens_z;
    g_tc.state.calibrated = true;
    g_tc.stats.calibration_count++;

    LOG_INFO("TEMP_COMP", "Sensitivity set manually: "
             "(%.4f, %.4f, %.4f) g/°C", sens_x, sens_y, sens_z);

    xSemaphoreGive(g_tc.mutex);
    return APP_ERR_OK;
}

int temp_comp_get_state(struct temp_comp_state *state)
{
    if (!g_tc.initialized) return APP_ERR_TEMP_NOT_INIT;
    if (!state) return APP_ERR_INVALID_PARAM;

    xSemaphoreTake(g_tc.mutex, portMAX_DELAY);
    memcpy(state, &g_tc.state, sizeof(*state));
    xSemaphoreGive(g_tc.mutex);

    return APP_ERR_OK;
}

int temp_comp_get_stats(struct temp_comp_stats *stats)
{
    if (!g_tc.initialized) return APP_ERR_TEMP_NOT_INIT;
    if (!stats) return APP_ERR_INVALID_PARAM;

    xSemaphoreTake(g_tc.mutex, portMAX_DELAY);
    memcpy(stats, &g_tc.stats, sizeof(*stats));
    xSemaphoreGive(g_tc.mutex);

    return APP_ERR_OK;
}

float temp_comp_get_filtered_temp(void)
{
    if (!g_tc.initialized) return NAN;

    float temp;
    xSemaphoreTake(g_tc.mutex, portMAX_DELAY);
    temp = g_tc.state.current_temp;
    xSemaphoreGive(g_tc.mutex);

    return temp;
}

void temp_comp_reset_stats(void)
{
    if (!g_tc.initialized) return;

    xSemaphoreTake(g_tc.mutex, portMAX_DELAY);
    memset(&g_tc.stats, 0, sizeof(g_tc.stats));
    xSemaphoreGive(g_tc.mutex);
}