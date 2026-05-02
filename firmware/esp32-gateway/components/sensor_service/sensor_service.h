/**
 * @file sensor_service.h
 * @author EnterWorldDoor
 * @brief 企业级传感器服务层 (业务逻辑核心)
 *
 * 架构设计:
 *   ┌─────────────────────────────────────────────┐
 *   │           Sensor Service (业务层)           │
 *   │                                             │
 *   │  ADXL345 ──┐                                  │
 *   │  (振动)    ├──▶ 数据融合管道 ──▶ AI/MQTT 输出│
 *   │            │     ↓                           │
 *   │  Protocol──┤  温度补偿 → DSP处理(RMS/FFT)      │
 *   │  (温湿度)  │                                  │
 *   └─────────────────────────────────────────────┘
 *
 * 核心职责:
 *   - 数据采集调度 (ADXL345 批量原始数据)
 *   - 温度漂移补偿 (对接 Protocol 温度数据)
 *   - 数字信号处理 (RMS/FFT 频谱分析)
 *   - 结果标准化输出 (供 AI/MQTT 使用)
 *   - 系统健康监控与故障恢复
 *
 * 设计原则:
 *   - 业务逻辑层: 不直接操作硬件
 *   - 模块解耦: 通过回调/接口与其他模块交互
 *   - 企业级: 错误处理、日志、统计、线程安全
 */

#ifndef SENSOR_SERVICE_H
#define SENSOR_SERVICE_H

#include <stdint.h>
#include <stdbool.h>
#include "global_error.h"
#include "adxl345.h"          /* ADXL345 驱动层 */
#include "dsp.h"              /* ESP-DSP 处理模块 */
#include "protocol.h"         /* UART 协议 (温度数据) */
#include "temperature_compensation.h"  /* 温度补偿模块 */

/* ==================== 配置常量 ==================== */

#define SENSOR_DEFAULT_SAMPLE_RATE_HZ   400      /**< 默认采样率 (Hz), 匹配 ADXL345_RATE_400 */
#define SENSOR_DEFAULT_BUFFER_SIZE      256      /**< 默认环形缓冲区大小 (样本数) */
#define SENSOR_FFT_WINDOW_SIZE          512      /**< FFT 窗口大小 (必须为2的幂次方) */
#define SENSOR_TASK_STACK_SIZE          6144     /**< 服务任务栈大小 */
#define SENSOR_TASK_PRIORITY            6        /**< 服务任务优先级 */
#define SENSOR_ANALYSIS_INTERVAL_MS     2000     /**< 分析周期 (ms), 匹配fetch_batch 1500ms超时 */
#define SENSOR_MAX_CALLBACKS            8        /**< 最大注册回调数 */

/* ==================== 数据结构 ==================== */

/**
 * enum sensor_state - 传感器服务状态枚举
 */
enum sensor_state {
    SENSOR_STATE_UNINITIALIZED = 0,    /**< 未初始化 */
    SENSOR_STATE_INITIALIZED,          /**< 已初始化, 未启动 */
    SENSOR_STATE_RUNNING,              /**< 运行中 */
    SENSOR_STATE_DEGRADED,             /**< 降级运行 (部分功能异常) */
    SENSOR_STATE_ERROR                 /**< 错误状态 */
};

/**
 * struct vib_sample - 三轴振动样本 (基础单位)
 */
struct vib_sample {
    float x;                            /**< X轴加速度 (g), 已温度补偿 */
    float y;                            /**< Y轴加速度 (g), 已温度补偿 */
    float z;                            /**< Z轴加速度 (g), 已温度补偿 */
    int64_t timestamp_us;              /**< 时间戳 (微秒) */
};

/**
 * struct analysis_result - 完整分析结果 (供 AI/MQTT 使用)
 *
 * 这是 Sensor Service 的主要输出格式,
 * 包含振动特征、频谱信息、环境数据和系统状态。
 */
struct analysis_result {
    /* ---- 振动特征 ---- */
    struct dsp_3axis_result vibration; /**< 三轴振动分析结果 (RMS+FFT) */
    float overall_rms_g;               /**< 整体振动烈度 (g) */
    float peak_frequency_hz;           /**< 主峰值频率 (Hz) */
    float peak_amplitude_g;            /**< 主峰值幅度 (g) */

    /* ---- 环境数据 ---- */
    float temperature_c;               /**< 温度 (°C) [来自 STM32] */
    float humidity_rh;                 /**< 相对湿度 (%RH) [来自 STM32] */
    bool temperature_valid;            /**< 温度数据是否有效 */

    /* ---- 补偿状态 ---- */
    bool temp_compensation_active;     /**< 温度补偿是否激活 */
    float temp_offset_x;               /**< X轴温度偏移量 (g) */
    float temp_offset_y;               /**< Y轴温度偏移量 (g) */
    float temp_offset_z;               /**< Z轴温度偏移量 (g) */

    /* ---- 系统状态 ---- */
    enum sensor_state service_state;   /**< 服务状态 */
    uint32_t analysis_timestamp_us;    /**< 分析时间戳 (微秒) */
    uint32_t samples_analyzed;         /**< 本次分析的样本数 */
    uint32_t total_analyses;           /**< 累计分析次数 */

    /* ---- 健康指标 ---- */
    struct adxl345_health_info adxl_health; /**< ADXL345 健康状态 */
    struct proto_stats protocol_stats;      /**< 协议统计 */
    struct dsp_stats dsp_stats;             /**< DSP 统计 */
};

/**
 * struct sensor_config - 传感器服务配置参数
 */
struct sensor_config {
    int sample_rate_hz;                /**< 采样率 (Hz) */
    int ring_buffer_size;              /**< 环形缓冲区大小 (样本数) */
    uint16_t fft_size;                 /**< FFT 点数 (必须为2的幂次方) */
    enum dsp_window_type window_type;  /**< DSP 窗函数类型 */
    uint32_t analysis_interval_ms;     /**< 分析间隔 (ms) */
    bool enable_temp_compensation;     /**< 是否启用温度补偿 */
    bool enable_protocol_temp;         /**< 是否从协议接收温度 */
    bool enable_detailed_logging;      /**< 是否启用详细日志 */
};

/**
 * struct sensor_stats - 传感器服务统计信息
 */
struct sensor_stats {
    uint32_t total_samples_acquired;   /**< 累计采集样本数 */
    uint32_t total_analyses_performed; /**< 累计分析次数 */
    uint32_t successful_analyses;      /**< 成功分析次数 */
    uint32_t failed_analyses;          /**< 失败分析次数 */
    uint32_t temp_data_received;       /**< 收到温度数据次数 */
    uint32_t temp_comp_applied;        /**< 温度补偿应用次数 */
    uint64_t total_compute_time_us;    /**< 累计计算时间 (微秒) */
    uint32_t max_compute_time_us;      /**< 单次最大计算时间 (微秒) */
};

/* ==================== 回调函数类型 ==================== */

/**
 * analysis_ready_callback_t - 分析完成回调
 * @result: 完整分析结果指针 (只读, 不要保存指针)
 * @user_data: 用户上下文
 *
 * 用途:
 *   - AI 模块接收数据进行 tinyML 推理
 *   - MQTT 模块发送到云端
 *   - 本地存储/日志记录
 *
 * 注意:
 *   - 此回调在 Sensor Service 任务中调用, 不应阻塞
 *   - result 指针的生命周期仅限于本次回调
 *   - 如需持久化请 memcpy 到自己的缓冲区
 */
typedef void (*analysis_ready_callback_t)(const struct analysis_result *result,
                                          void *user_data);

/**
 * sensor_error_callback_t - 传感器错误回调
 * @error_code: 错误码
 * @context: 错误描述
 * @user_data: 用户上下文
 */
typedef void (*sensor_error_callback_t)(int error_code,
                                         const char *context,
                                         void *user_data);

/* ==================== 生命周期 API ==================== */

/**
 * sensor_service_init - 初始化传感器服务 (配置所有子模块)
 * @config: 配置参数 (NULL 使用默认配置)
 *
 * 内部初始化顺序:
 *   1. ADXL345 驱动 (SPI/FIFO/中断)
 *   2. DSP 模块 (预分配工作区)
 *   3. Ring Buffer (环形缓冲区)
 *   4. 温度补偿模块 (如启用)
 *   5. Protocol 回调注册 (如启用)
 *
 * Return: APP_ERR_OK or error code
 */
int sensor_service_init(const struct sensor_config *config);

/**
 * sensor_service_deinit - 反初始化并释放所有资源
 *
 * Return: APP_ERR_OK or error code
 */
int sensor_service_deinit(void);

/**
 * sensor_service_is_initialized - 查询是否已初始化
 */
bool sensor_service_is_initialized(void);

/**
 * sensor_service_start - 启动采集和分析任务
 *
 * 创建 FreeRTOS 任务:
 *   - 数据采集循环 (从 ADXL345 获取批量数据)
 *   - 定时分析循环 (FFT/RMS + 温度补偿)
 *
 * Return: APP_ERR_OK or error code
 */
int sensor_service_start(void);

/**
 * sensor_service_stop - 停止所有后台任务
 *
 * Return: APP_ERR_OK or error code
 */
int sensor_service_stop(void);

/* ==================== 数据查询 API ==================== */

/**
 * sensor_service_get_latest_analysis - 获取最近一次分析结果 (同步查询)
 * @result: 输出结果结构体指针
 *
 * 用于轮询模式 (非回调模式)。
 *
 * Return: APP_ERR_OK 成功, APP_ERR_NO_DATA 暂无数据, 其他错误码
 */
int sensor_service_get_latest_analysis(struct analysis_result *result);

/**
 * sensor_service_fetch_sample - 获取单个实时振动样本 (非阻塞)
 * @sample: 输出样本结构体
 *
 * 从预处理后的环形缓冲区读取。
 *
 * Return: APP_ERR_OK or error code
 */
int sensor_service_fetch_sample(struct vib_sample *sample);

/**
 * sensor_service_get_current_temperature - 获取当前温度值
 * @temp_c: 输出温度 (°C)
 *
 * Return: APP_ERR_OK 成功, APP_ERR_TEMP_NO_DATA 无数据
 */
int sensor_service_get_current_temperature(float *temp_c);

/* ==================== 注册 API ==================== */

/**
 * sensor_service_register_analysis_callback - 注册分析完成回调
 * @cb: 回调函数
 * @user_data: 用户上下文
 *
 * 可同时注册多个回调 (AI + MQTT + 存储)。
 *
 * Return: APP_ERR_OK or error code
 */
int sensor_service_register_analysis_callback(analysis_ready_callback_t cb,
                                               void *user_data);

/**
 * sensor_service_unregister_analysis_callback - 注销分析完成回调
 * @cb: 待注销的回调
 */
int sensor_service_unregister_analysis_callback(analysis_ready_callback_t cb);

/**
 * sensor_service_register_error_callback - 注册错误回调
 * @cb: 错误回调函数
 * @user_data: 用户上下文
 */
int sensor_service_register_error_callback(sensor_error_callback_t cb,
                                            void *user_data);

/* ==================== 查询 API ==================== */

/**
 * sensor_service_get_state - 获取当前服务状态
 */
enum sensor_state sensor_service_get_state(void);

/**
 * sensor_service_get_stats - 获取统计信息
 * @stats: 输出统计结构体
 */
int sensor_service_get_stats(struct sensor_stats *stats);

/**
 * sensor_service_reset_stats - 重置统计计数器
 */
void sensor_service_reset_stats(void);

/**
 * sensor_service_force_analysis - 手动触发一次即时分析
 *
 * 用于调试或事件驱动模式。
 *
 * Return: APP_ERR_OK or error code
 */
int sensor_service_force_analysis(void);

#endif /* SENSOR_SERVICE_H */