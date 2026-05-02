/**
 * @file temperature_compensation.h
 * @author EnterWorldDoor
 * @brief 企业级自适应温度漂移补偿模块 (EWMA + 时间戳对齐 + 变化率检测)
 *
 * 核心算法:
 *   1. 时间戳对齐: 使用 time_sync 统一时基，解决 ADXL345 与温度传感器的时间差
 *   2. 线性插值: 当时间差 > 阈值时，对温度数据进行插值
 *   3. EWMA滤波: 指数加权移动平均，抑制温度噪声
 *   4. 变化率检测: 只在温度显著变化时更新偏移量，避免频繁调整
 *
 * 适用场景:
 *   - ADXL345 长期振动监测中的零点温漂补偿
 *   - STM32 通过 UART 上报温度数据，ESP32 侧进行实时补偿
 *   - 工业环境温度变化缓慢场景 (0.1~2°C/min)
 */

#ifndef TEMPERATURE_COMPENSATION_H
#define TEMPERATURE_COMPENSATION_H

#include <stdint.h>
#include <stdbool.h>
#include "global_error.h"

/* ==================== 配置常量 ==================== */

#define TEMP_COMP_MAX_HISTORY         64      /**< 温度历史记录最大长度 */
#define TEMP_COMP_DEFAULT_ALPHA       0.1f    /**< EWMA 默认平滑系数 (0~1, 越小越平滑) */
#define TEMP_COMP_TEMP_THRESHOLD      0.5f    /**< 温度变化阈值 (°C), 超过此值才更新补偿 */
#define TEMP_COMP_RATE_THRESHOLD      0.5f    /**< 温度变化率阈值 (°C/s) */
#define TEMP_COMP_STALE_TIMEOUT_MS    5000    /**< 数据过期超时 (ms), 超时认为温度传感器离线 */
#define TEMP_COMP_INTERP_MAX_DELTA_MS 200     /**< 最大插值时间差 (ms), 超过此值标记数据为陈旧 */

/* ==================== 数据结构 ==================== */

/**
 * struct temp_sample - 带时间戳的温度样本
 */
struct temp_sample {
    float temperature_c;             /**< 温度值 (°C) */
    int64_t timestamp_us;            /**< 采样时间戳 (微秒, 基于 time_sync) */
};

/**
 * struct temp_comp_config - 温度补偿配置参数
 */
struct temp_comp_config {
    float ewma_alpha;                /**< EWMA 平滑系数 (推荐 0.05~0.2) */
    float temp_change_threshold;     /**< 触发更新的最小温度变化 (°C) */
    float rate_threshold;            /**< 温度变化率阈值 (°C/s) */
    uint32_t stale_timeout_ms;       /**< 数据过期时间 (ms) */
    uint32_t interp_max_delta_ms;    /**< 最大允许插值时间差 (ms) */
};

/**
 * struct temp_comp_state - 温度补偿运行状态
 */
struct temp_comp_state {
    float current_temp;              /**< 当前滤波后温度 (°C) */
    float last_compensated_temp;     /**< 上次执行补偿时的温度 (°C) */
    float offset_x;                  /**< X轴当前偏移量 (g) */
    float offset_y;                  /**< Y轴当前偏移量 (g) */
    float offset_z;                  /**< Z轴当前偏移量 (g) */
    float sensitivity_coeff_x;      /**< X轴温度灵敏度系数 (g/°C) */
    float sensitivity_coeff_y;      /**< Y轴温度灵敏度系数 (g/°C) */
    float sensitivity_coeff_z;      /**< Z轴温度灵敏度系数 (g/°C) */
    int64_t last_update_time_us;     /**< 上次更新补偿的时间戳 */
    bool calibrated;                 /**< 是否已完成校准 */
    bool sensor_online;             /**< 温度传感器是否在线 */
    uint32_t stale_count;           /**< 连续陈旧数据计数 */
};

/**
 * struct temp_comp_stats - 温度补偿统计信息
 */
struct temp_comp_stats {
    uint32_t total_samples;          /**< 总接收样本数 */
    uint32_t compensated_samples;    /**< 已补偿样本数 */
    uint32_t skipped_samples;        /**< 跳过的样本数 (未达阈值) */
    uint32_t interpolated_count;     /**< 插值次数 */
    uint32_t stale_data_count;       /**< 陈旧数据次数 */
    uint32_t calibration_count;      /**< 校准次数 */
    float max_offset_applied;        /**< 应用过的最大偏移量 */
    float avg_compensation_rate;     /**< 平均补偿频率 (Hz) */
};

/* ==================== 生命周期 API ==================== */

/**
 * temp_comp_init - 初始化温度补偿模块
 * @config: 配置参数 (NULL 使用默认配置)
 *
 * Return: APP_ERR_OK or error code
 */
int temp_comp_init(const struct temp_comp_config *config);

/**
 * temp_comp_deinit - 反初始化温度补偿模块
 *
 * Return: APP_ERR_OK or error code
 */
int temp_comp_deinit(void);

/**
 * temp_comp_reset - 重置补偿状态 (保留配置)
 */
void temp_comp_reset(void);

/* ==================== 数据输入 API ==================== */

/**
 * temp_comp_push_temperature - 推入新的温度样本 (由 protocol 回调调用)
 * @temperature: 温度值 (°C)
 * @timestamp_us: 采样时间戳 (微秒, 来自 time_sync 或 STM32)
 *
 * 内部自动进行 EWMA 滤波和时间戳对齐。
 *
 * Return: APP_ERR_OK or error code
 */
int temp_comp_push_temperature(float temperature, int64_t timestamp_us);

/**
 * temp_comp_is_sensor_online - 查询温度传感器是否在线
 *
 * 基于最近数据接收时间和陈旧数据判断。
 *
 * Return: true 在线, false 离线/数据过期
 */
bool temp_comp_is_sensor_online(void);

/* ==================== 补偿计算 API ==================== */

/**
 * temp_comp_compensate - 对加速度数据进行温度漂移补偿
 * @x: X轴加速度输入/输出 (g)
 * @y: Y轴加速度输入/输出 (g)
 * @z: Z轴加速度输入/输出 (g)
 * @current_timestamp_us: 当前加速度数据的时间戳 (微秒)
 *
 * 算法流程:
 *   1. 检查温度数据时效性
 *   2. 如有时间差，进行线性插值
 *   3. 计算温度变化率，判断是否需要更新偏移
 *   4. 应用偏移补偿到三轴加速度
 *
 * Return: APP_ERR_OK 成功, APP_ERR_TEMP_NO_DATA 无温度数据,
 *         APP_ERR_TEMP_STALE_DATA 数据陈旧, 其他错误码
 */
int temp_comp_compensate(float *x, float *y, float *z,
                         int64_t current_timestamp_us);

/**
 * temp_comp_get_current_offset - 获取当前的温度偏移量
 * @offset_x: 输出 X轴偏移 (g)
 * @offset_y: 输出 Y轴偏移 (g)
 * @offset_z: 输出 Z轴偏移 (g)
 *
 * Return: APP_ERR_OK or error code
 */
int temp_comp_get_current_offset(float *offset_x, float *offset_y,
                                  float *offset_z);

/* ==================== 校准 API ==================== */

/**
 * temp_comp_calibrate - 执行温度灵敏度校准
 * @temp_low: 低温点温度 (°C)
 * @offset_low_x/y/z: 低温点时的零点偏移
 * @temp_high: 高温点温度 (°C)
 * @offset_high_x/y/z: 高温点时的零点偏移
 *
 * 两点校准法:
 *   sensitivity = (offset_high - offset_low) / (temp_high - temp_low)
 *
 * 注意: 校准期间设备应静止，且温度稳定。
 *
 * Return: APP_ERR_OK or error code
 */
int temp_comp_calibrate(float temp_low,
                        float offset_low_x, float offset_low_y, float offset_low_z,
                        float temp_high,
                        float offset_high_x, float offset_high_y, float offset_high_z);

/**
 * temp_comp_set_sensitivity - 手动设置温度灵敏度系数
 * @sens_x: X轴灵敏度 (g/°C)
 * @sens_y: Y轴灵敏度 (g/°C)
 * @sens_z: Z轴灵敏度 (g/°C)
 *
 * 用于已知传感器特性或使用出厂标定值的场景。
 *
 * Return: APP_ERR_OK or error code
 */
int temp_comp_set_sensitivity(float sens_x, float sens_y, float sens_z);

/* ==================== 查询 API ==================== */

/**
 * temp_comp_get_state - 获取当前补偿状态快照
 * @state: 输出状态结构体指针
 *
 * Return: APP_ERR_OK or error code
 */
int temp_comp_get_state(struct temp_comp_state *state);

/**
 * temp_comp_get_stats - 获取统计信息
 * @stats: 输出统计结构体指针
 *
 * Return: APP_ERR_OK or error code
 */
int temp_comp_get_stats(struct temp_comp_stats *stats);

/**
 * temp_comp_get_filtered_temp - 获取当前 EWMA 滤波后的温度值
 *
 * Return: 温度值 (°C), 若无数据返回 NAN
 */
float temp_comp_get_filtered_temp(void);

/**
 * temp_comp_reset_stats - 重置统计计数器
 */
void temp_comp_reset_stats(void);

#endif /* TEMPERATURE_COMPENSATION_H */