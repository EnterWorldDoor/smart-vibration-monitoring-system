/**
 * @file adxl345.h
 * @author EnterWorldDoor
 * @brief 企业级 ADXL345 三轴加速度计驱动（SPI模式、FIFO中断驱动、滑动窗口滤波）
 *
 * 功能特性:
 *   - SPI 高速通信 (最高 5MHz，工业可靠性优于 I2C)
 *   - FIFO + INT1 中断驱动架构 (32级 FIFO 缓冲，减轻 CPU 负载)
 *   - 双缓冲 RingBuf 管道 (ADXL345 FIFO → ESP32 RingBuf → 原始数据输出)
 *   - 16 点滑动窗口移动平均滤波 (抑制高频噪声)
 *   - DC 基础偏移去除 (消除静态偏移，温度补偿由独立模块处理)
 *   - 读写重试与总线复位逻辑
 *   - 传感器健康状态自检与监控
 *   - 引脚可配置 (不在驱动中硬编码)
 *
 * 设计原则:
 *   - 纯驱动层: 只负责硬件抽象和数据采集
 *   - 无业务逻辑: RMS/FFT/趋势分析由 DSP 模块处理
 *   - 温度补偿: 由 temperature_compensation 模块处理
 */

#ifndef ADXL345_H
#define ADXL345_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/spi_master.h"
#include "global_error.h"
#include "ringbuf.h"

/* ==================== 枚举定义 ==================== */

/** 加速度量程范围 */
enum adxl345_range {
    ADXL345_RANGE_2G  = 0,  /**< ±2g (默认，10位分辨率) */
    ADXL345_RANGE_4G  = 1,  /**< ±4g */
    ADXL345_RANGE_8G  = 2,  /**< ±8g */
    ADXL345_RANGE_16G = 3,  /**< ±16g */
};

/** 数据输出速率 (ODR) - 工业场景推荐 3200Hz */
enum adxl345_data_rate {
    ADXL345_RATE_3200 = 0x0F,  /**< 3200 Hz (最高，适合振动分析) */
    ADXL345_RATE_1600 = 0x0E,  /**< 1600 Hz */
    ADXL345_RATE_800  = 0x0D,  /**< 800 Hz */
    ADXL345_RATE_400  = 0x0C,  /**< 400 Hz */
    ADXL345_RATE_200  = 0x0B,  /**< 200 Hz */
    ADXL345_RATE_100  = 0x0A,  /**< 100 Hz */
    ADXL345_RATE_50   = 0x09,  /**< 50 Hz */
    ADXL345_RATE_25   = 0x08,  /**< 25 Hz */
    ADXL345_RATE_12_5 = 0x07,  /**< 12.5 Hz */
    ADXL345_RATE_6_25 = 0x06,  /**< 6.25 Hz */
};

/** FIFO 工作模式 */
enum adxl345_fifo_mode {
    ADXL345_FIFO_BYPASS  = 0x00,  /**< Bypass 模式 (FIFO关闭) */
    ADXL345_FIFO_FIFO    = 0x40,  /**< FIFO 模式 (满后停止) */
    ADXL345_FIFO_STREAM  = 0x80,  /**< Stream 模式 (循环覆盖，推荐) */
    ADXL345_FIFO_TRIGGER = 0xC0,  /**< Trigger 模式 (INT1触发后停止) */
};

/** 总线类型 */
enum adxl345_bus_type {
    ADXL345_BUS_SPI = 0,  /**< SPI 总线 (工业推荐) */
    ADXL345_BUS_I2C = 1,  /**< I2C 总线 (兼容保留) */
};

/** 传感器健康等级 */
enum adxl345_health_level {
    ADXL345_HEALTH_GOOD      = 0,  /**< 正常 */
    ADXL345_HEALTH_WARNING   = 1,  /**< 轻微异常 (偶发通信错误) */
    ADXL345_HEALTH_DEGRADED  = 2,  /**< 性能下降 (频繁错误/漂移) */
    ADXL345_HEALTH_FAULT     = 3,  /**< 故障 (自检失败/无响应) */
};

/* ==================== 配置结构体 ==================== */

/** SPI 总线配置 (引脚在 main() 初始化，不在此硬编码) */
struct adxl345_spi_config {
    spi_host_device_t host_id;       /**< SPI 主机号 (HSPI/VSPI) */
    int gpio_cs;                     /**< CS 片选引脚 */
    int gpio_miso;                   /**< MISO 引脚 */
    int gpio_mosi;                   /**< MOSI 引脚 */
    int gpio_sclk;                   /**< SCLK 时钟引脚 */
    uint32_t clock_speed_hz;         /**< SPI 时钟频率 (建议 ≤5MHz) */
};

/** I2C 总线配置 (兼容保留) */
struct adxl345_i2c_config {
    int port;                        /**< I2C 端口号 */
    uint8_t addr;                    /**< I2C 地址 (0x53 或 0x1D) */
};

/** 中断配置 (TIM1/TIM2 对应 INT1/INT2) */
struct adxl345_int_config {
    bool enable_int1;                /**< 使能 INT1 引脚 */
    bool enable_int2;                /**< 使能 INT2 引脚 */
    int gpio_int1;                   /**< INT1 GPIO 引脚号 */
    int gpio_int2;                   /**< INT2 GPIO 引脚号 */
    uint8_t int1_sources;            /**< INT1 中断源映射 (见 ADXL345_INT_* 宏) */
    uint8_t int2_sources;            /**< INT2 中断源映射 */
};

/** DC 偏移校准值 */
struct adxl345_dc_offset {
    float x_offset;                  /**< X轴 DC 偏移 (g) */
    float y_offset;                  /**< Y轴 DC 偏移 (g) */
    float z_offset;                  /**< Z轴 DC 偏移 (g) */
    bool calibrated;                 /**< 是否已校准 */
};

/** 预处理后的加速度数据 (含时间戳，供 sensor_service 使用) */
struct adxl345_accel_data {
    float x_g;                       /**< X轴加速度 (g)，已去DC偏移和滤波 */
    float y_g;                       /**< Y轴加速度 (g) */
    float z_g;                       /**< Z轴加速度 (g) */
    uint32_t timestamp_us;           /**< 时间戳 (微秒) */
};

/** 批量原始样本 (供 DSP 模块进行 FFT/RMS 处理) */
struct adxl345_batch_data {
    struct adxl345_raw_sample *samples;  /**< 原始样本数组指针 */
    uint16_t count;                     /**< 样本数量 */
    uint32_t first_timestamp_us;        /**< 第一个样本时间戳 */
    uint32_t last_timestamp_us;         /**< 最后一个样本时间戳 */
};

/** 原始采样数据 (从 FIFO 批量读取) */
struct adxl345_raw_sample {
    int16_t x;                       /**< X轴原始值 */
    int16_t y;                       /**< Y轴原始值 */
    int16_t z;                       /**< Z轴原始值 */
    uint32_t timestamp_us;           /**< 采样时间戳 */
};

/** 传感器健康状态 */
struct adxl345_health_info {
    enum adxl345_health_level level; /**< 健康等级 */
    uint32_t total_samples;          /**< 累计采集样本数 */
    uint32_t comm_errors;            /**< 通信错误次数 */
    uint32_t retry_count;            /**< 重试次数 */
    uint32_t fifo_overflow_count;    /**< FIFO 溢出次数 */
    uint32_t dropped_samples;        /**< 丢弃样本数 */
    bool selftest_passed;            /**< 自检是否通过 */
};

/** 运行时统计信息 */
struct adxl345_stats {
    uint32_t samples_pushed;          /**< 推入 RingBuf 的样本数 */
    uint32_t samples_processed;       /**< 已预处理样本数 */
    uint32_t fifo_reads;             /**< FIFO 读取次数 */
    uint32_t int_fired_count;        /**< 中断触发次数 */
};

/* ==================== 回调函数类型 ==================== */

/**
 * 数据就绪回调 (预处理完成后调用)
 */
typedef void (*adxl345_data_callback)(const struct adxl345_accel_data *data,
                                       void *user_data);

/* ==================== 不透明设备句柄 ==================== */

struct adxl345_dev;

/* ==================== 公开 API ==================== */

/**
 * adxl345_init_spi - 初始化 ADXL345 (SPI 模式，推荐工业场景)
 * @spi_cfg: SPI 总线配置 (引脚、时钟等，调用者负责 SPI 主机初始化)
 * @range: 加速度量程
 * @rate: 数据输出速率 (ODR)
 * @fifo_mode: FIFO 工作模式
 * @int_cfg: 中断配置 (可为 NULL 表示轮询模式)
 * @ringbuf: 外部提供的环形缓冲区 (调用者分配)
 *
 * 注意: 引脚在 main() 或业务层初始化，不在此处硬编码
 *
 * Return: 设备句柄 on success, NULL on error
 */
struct adxl345_dev *adxl345_init_spi(
    const struct adxl345_spi_config *spi_cfg,
    enum adxl345_range range,
    enum adxl345_data_rate rate,
    enum adxl345_fifo_mode fifo_mode,
    const struct adxl345_int_config *int_cfg,
    struct ringbuf *ringbuf);

/**
 * adxl345_deinit - 反初始化设备，释放所有资源
 * @dev: 设备句柄
 */
void adxl345_deinit(struct adxl345_dev *dev);

/**
 * adxl345_start - 启动数据采集 (创建 FreeRTOS 任务)
 * @dev: 设备句柄
 * @task_priority: 采集任务优先级 (推荐 configMAX_PRIORITIES-1)
 * @task_stack_size: 任务栈大小 (推荐 2048~4096)
 *
 * Return: APP_ERR_OK or error code
 */
int adxl345_start(struct adxl345_dev *dev,
                  int task_priority,
                  uint32_t task_stack_size);

/**
 * adxl345_stop - 停止数据采集
 * @dev: 设备句柄
 *
 * Return: APP_ERR_OK or error code
 */
int adxl345_stop(struct adxl345_dev *dev);

/**
 * adxl345_fetch - 从 RingBuf 获取一个预处理后的样本
 * @dev: 设备句柄
 * @out: 输出数据
 * @timeout_ms: 超时时间 (ms)，0 为非阻塞
 *
 * Return: APP_ERR_OK, APP_ERR_TIMEOUT, or error code
 */
int adxl345_fetch(struct adxl345_dev *dev,
                  struct adxl345_accel_data *out,
                  uint32_t timeout_ms);

/**
 * adxl345_calibrate_dc_offset - 执行 DC 基础偏移校准 (不含温度补偿)
 * @dev: 设备句柄
 * @samples: 校准采样数 (默认 ADXL345_DC_CALIBRATION_SAMPLES)
 *
 * 在静止状态下采集多组样本计算平均值作为 DC 基础偏移。
 * 温度相关的漂移补偿由 temperature_compensation 模块处理。
 * 校准期间传感器应保持水平静止。
 *
 * Return: APP_ERR_OK or error code
 */
int adxl345_calibrate_dc_offset(struct adxl345_dev *dev, uint16_t samples);

/**
 * adxl345_register_data_callback - 注册数据就绪回调
 * @dev: 设备句柄
 * @cb: 回调函数
 * @user_data: 用户上下文
 */
void adxl345_register_data_callback(struct adxl345_dev *dev,
                                     adxl345_data_callback cb,
                                     void *user_data);

/**
 * adxl345_fetch_batch - 从 RingBuf 批量获取原始样本 (供 DSP 模块使用)
 * @dev: 设备句柄
 * @batch: 输出批量数据结构体
 * @max_count: 最大获取样本数量
 * @timeout_ms: 超时时间 (ms), 0 为非阻塞
 *
 * 用于 DSP 模块进行 FFT/RMS 批量处理。
 * 注意: batch->samples 的内存由调用者提供或内部静态分配。
 *
 * Return: 实际获取的样本数, 0 表示无数据或超时, 负数表示错误
 */
int adxl345_fetch_batch(struct adxl345_dev *dev,
                         struct adxl345_batch_data *batch,
                         uint16_t max_count,
                         uint32_t timeout_ms);

/**
 * adxl345_get_health - 获取传感器健康状态
 * @dev: 设备句柄
 * @health: 输出健康信息
 *
 * Return: APP_ERR_OK or error code
 */
int adxl345_get_health(struct adxl345_dev *dev,
                       struct adxl345_health_info *health);

/**
 * adxl345_get_stats - 获取运行统计
 * @dev: 设备句柄
 * @stats: 输出统计信息
 *
 * Return: APP_ERR_OK or error code
 */
int adxl345_get_stats(struct adxl345_dev *dev,
                      struct adxl345_stats *stats);

/**
 * adxl345_self_test - 执行传感器自检
 * @dev: 设备句柄
 *
 * 自检内容: 设备ID验证、寄存器读写、DC偏移合理性检查
 *
 * Return: APP_ERR_OK or error code
 */
int adxl345_self_test(struct adxl345_dev *dev);

/**
 * adxl345_reset_bus - 复位 SPI/I2C 总线 (故障恢复)
 * @dev: 设备句柄
 *
 * Return: APP_ERR_OK or error code
 */
int adxl345_reset_bus(struct adxl345_dev *dev);

/* ==================== 常量定义 ==================== */

#define ADXL345_MA_WINDOW_SIZE        16    /**< 移动平均滤波窗口大小 */
#define ADXL345_DC_CALIBRATION_SAMPLES 64   /**< DC偏移校准默认采样数 */
#define ADXL345_BATCH_MAX_SIZE       512   /**< 批量获取最大样本数 (DSP FFT 用) */
#define ADXL345_SPI_RETRY_MAX         3     /**< SPI 通信最大重试次数 */
#define ADXL345_SPI_TIMEOUT_MS        50    /**< SPI 通信超时 (ms) */
#define ADXL345_FIFO_WATERMARK        16    /**< FIFO 默认水位点 */
#define ADXL345_HEALTH_ERROR_THRESH  10     /**< 健康降级阈值 (连续错误次数) */

#endif /* ADXL345_H */