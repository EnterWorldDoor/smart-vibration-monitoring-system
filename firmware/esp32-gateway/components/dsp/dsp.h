/**
 * @file dsp.h
 * @author EnterWorldDoor
 * @brief 企业级数字信号处理模块 (基于 ESP-DSP 库)
 *
 * 功能特性:
 *   - 使用 ESP-IDF 内置 esp-dsp 库 (高性能优化的 DSP 算法)
 *   - FFT 频谱分析 (基2-Radix, 支持汉宁/汉明/布莱克曼窗)
 *   - RMS 有效值计算 (单轴/三轴向量合成)
 *   - 频域特征提取 (峰值频率、总谐波失真、频带能量)
 *   - 三轴批量处理 (ADXL345 加速度计专用优化)
 *   - 内存预分配 (避免运行时动态分配)
 *   - 企业级错误处理与统计监控
 *
 * 适用场景:
 *   - 工业振动监测与分析
 *   - 电机故障诊断 (不平衡、不对中、轴承故障)
 *   - 结构健康监测 (SHM)
 *   - tinyML 特征提取前置处理
 *
 * 设计原则:
 *   - 无业务逻辑: 只负责数学运算
 *   - 输入原始数据: 来自 ADXL345 驱动层
 *   - 输出标准化结果: 供 AI/MQTT 模块使用
 */

#ifndef DSP_H
#define DSP_H
#include <stdint.h>
#include <stdbool.h>
#include "global_error.h"

/* ==================== 配置常量 ==================== */

#define DSP_FFT_MAX_SIZE               1024    /**< 最大 FFT 点数 (2^10) */
#define DSP_FFT_DEFAULT_SIZE           512     /**< 默认 FFT 点数 (2^9) */
#define DSP_MAX_WINDOW_COEFF           2048    /**< 最大窗函数系数缓存 */
#define DSP_NUM_PEAKS                  10      /**< 返回的最大峰值数量 */
#define DSP_NUM_FREQUENCY_BANDS        8       /**< 频带数量 (用于频带能量分析) */

/* ==================== 窗函数类型 ==================== */

enum dsp_window_type {
    DSP_WINDOW_RECTANGLE = 0,          /**< 矩形窗 (无窗) */
    DSP_WINDOW_HANN = 1,               /**< 汉宁窗 (Hann, 推荐) */
    DSP_WINDOW_HAMMING = 2,            /**< 汉明窗 (Hamming) */
    DSP_WINDOW_BLACKMAN = 3,           /**< 布莱克曼窗 (Blackman) */
    DSP_WINDOW_FLATTOP = 4             /**< 平顶窗 (Flat Top, 幅度精确) */
};

/* ==================== 数据结构 ==================== */

/**
 * struct dsp_config - DSP 模块配置参数
 */
struct dsp_config {
    uint16_t default_fft_size;         /**< 默认 FFT 点数 (必须为 2 的幂次方) */
    enum dsp_window_type window_type;  /**< 默认窗函数类型 */
    bool enable_dc_removal;            /**< 是否去除直流分量 */
};

/**
 * struct rms_result - RMS 计算结果
 */
struct rms_result {
    float value;                       /**< RMS 有效值 */
    float mean;                        /**< 均值 (直流分量) */
    float std_dev;                     /**< 标准差 (AC 分量) */
    float peak;                        /**< 峰值 (绝对值最大) */
    float peak_to_peak;                /**< 峰峰值 */
    float crest_factor;                /**< 波峰因子 (peak/rms) */
    float skewness;                    /**< 偏度 (不对称性度量) */
    float kurtosis;                    /**< 峰度 (尖锐程度) */
};

/**
 * struct freq_peak - 单个频率峰值点
 */
struct freq_peak {
    float frequency_hz;                /**< 频率 (Hz) */
    float amplitude;                   /**< 幅值 */
    float phase_rad;                   /**< 相位 (弧度) */
};

/**
 * struct fft_result - FFT 频谱分析结果
 */
struct fft_result {
    float *spectrum_magnitude;         /**< 幅值谱指针 (长度=fft_size/2+1) */
    float *spectrum_phase;             /**< 相位谱指针 (可选, 长度=fft_size/2+1) */
    float peak_freq;                   /**< 主峰值频率 (Hz) */
    float peak_amp;                    /**< 主峰值幅度 */
    struct freq_peak peaks[DSP_NUM_PEAKS]; /**< 前 N 个峰值点 */
    uint8_t peak_count;                /**< 实际检测到的峰数量 */
    uint16_t fft_size;                 /**< 实际 FFT 点数 */
    float sampling_rate;               /**< 采样率 (Hz) */
    float frequency_resolution;        /**< 频率分辨率 (Hz/bin) */
    float total_harmonic_distortion;   /**< 总谐波失真 THD (%) */
    float band_energy[DSP_NUM_FREQUENCY_BANDS]; /**< 各频带能量占比 */
};

/**
 * struct dsp_3axis_result - 三轴综合分析结果
 */
struct dsp_3axis_result {
    struct rms_result x_rms;           /**< X 轴 RMS */
    struct rms_result y_rms;           /**< Y 轴 RMS */
    struct rms_result z_rms;           /**< Z 轴 RMS */
    struct rms_result vector_rms;      /**< 三轴向量合成 RMS */
    struct fft_result x_fft;           /**< X 轴 FFT */
    struct fft_result y_fft;           /**< Y 轴 FFT */
    struct fft_result z_fft;           /**< Z 轴 FFT */
    float overall_vibration_level;     /**< 整体振动烈度 (mm/s 或 g) */
    uint16_t sample_count;             /**< 分析样本数 */
    uint32_t analysis_timestamp_us;    /**< 分析时间戳 (微秒) */
};

/**
 * struct dsp_stats - DSP 模块统计信息
 */
struct dsp_stats {
    uint32_t total_fft_computes;       /**< FFT 计算总次数 */
    uint32_t total_rms_computes;      /**< RMS 计算总次数 */
    uint32_t total_3axis_computes;    /**< 三轴批量计算次数 */
    uint32_t window_apply_count;       /**< 窗函数应用次数 */
    uint64_t compute_time_us_total;   /**< 累计计算时间 (微秒) */
    uint32_t max_fft_size_used;        /**< 使用的最大 FFT 点数 */
};

/* ==================== 生命周期 API ==================== */

/**
 * dsp_init - 初始化 DSP 模块 (预分配工作缓冲区和窗函数系数)
 * @config: 配置参数 (NULL 使用默认配置)
 *
 * 内部预分配:
 *   - FFT 工作区 (复数数组)
 *   - 窗函数系数表
 *   - 位反转索引表
 *
 * Return: APP_ERR_OK or error code
 */
int dsp_init(const struct dsp_config *config);

/**
 * dsp_deinit - 反初始化 DSP 模块 (释放所有预分配资源)
 *
 * Return: APP_ERR_OK or error code
 */
int dsp_deinit(void);

/**
 * dsp_is_initialized - 查询是否已初始化
 *
 * Return: true 已初始化, false 未初始化
 */
bool dsp_is_initialized(void);

/**
 * dsp_reset_stats - 重置统计计数器
 */
void dsp_reset_stats(void);

/* ==================== RMS 计算 API ==================== */

/**
 * dsp_rms_compute - 计算单组数据的 RMS 统计特性
 * @input: 输入浮点数据数组
 * @len: 数据长度
 * @out: 输出结果结构体 (含 RMS/均值/标准差/峰峰值等)
 *
 * Return: APP_ERR_OK or error code
 */
int dsp_rms_compute(const float *input, int len, struct rms_result *out);

/**
 * dsp_rms_compute_vector - 计算三轴向量合成 RMS
 * @x: X 轴数据数组
 * @y: Y 轴数据数组
 * @z: Z 轴数据数组
 * @len: 每轴数据长度
 * @out: 输出向量 RMS 结果
 *
 * 公式: RMS_vector = sqrt(mean(x² + y² + z²))
 *
 * Return: APP_ERR_OK or error code
 */
int dsp_rms_compute_vector(const float *x, const float *y, const float *z,
                            int len, struct rms_result *out);

/* ==================== FFT 频谱分析 API ==================== */

/**
 * dsp_fft_compute - 计算单通道 FFT 频谱 (使用 ESP-DSP 库)
 * @input: 输入时域信号数组 (长度必须 >= fft_size, 会被修改)
 * @fft_size: FFT 点数 (必须为 2 的幂次方, <= DSP_FFT_MAX_SIZE)
 * @sampling_rate: 采样率 (Hz), 用于计算实际频率
 * @window_type: 窗函数类型 (DSP_WINDOW_HANN 推荐)
 * @out: 输出完整频谱分析结果
 *
 * 功能:
 *   1. 可选 DC 去除
 *   2. 窗函数加窗 (减少频谱泄漏)
 *   3. FFT 变换 (基2-Radix, ESP-DSP 优化)
 *   4. 幅值/相位谱计算
 *   5. 峰值检测 (前 N 个最大峰值)
 *   6. THD 和频带能量计算
 *
 * 注意: input 数组会被原地修改!
 *
 * Return: APP_ERR_OK or error code
 */
int dsp_fft_compute(float *input, uint16_t fft_size, float sampling_rate,
                    enum dsp_window_type window_type, struct fft_result *out);

/**
 * dsp_fft_compute_3axis - 三轴同步 FFT 批量处理 (振动分析专用)
 * @x: X 轴时域信号 (会被修改)
 * @y: Y 轴时域信号 (会被修改)
 * @z: Z 轴时域信号 (会被修改)
 * @len: 每轴数据长度 (必须为 2 的幂次方)
 * @sampling_rate: 采样率 (Hz)
 * @window_type: 窗函数类型
 * @out: 输出三轴综合分析结果
 *
 * 优化:
 *   - 复用窗函数系数 (只计算一次)
 *   - 并行化三轴处理逻辑
 *   - 自动计算整体振动烈度
 *
 * Return: APP_ERR_OK or error code
 */
int dsp_fft_compute_3axis(float *x, float *y, float *z, uint16_t len,
                           float sampling_rate, enum dsp_window_type window_type,
                           struct dsp_3axis_result *out);

/* ==================== 窗函数 API ==================== */

/**
 * dsp_generate_window - 生成窗函数系数
 * @window_type: 窗函数类型
 * @length: 窗函数长度
 * @output: 输出系数数组 (需预先分配 length*sizeof(float))
 *
 * Return: APP_ERR_OK or error code
 */
int dsp_generate_window(enum dsp_window_type window_type, uint16_t length,
                         float *output);

/**
 * dsp_apply_window - 对数据应用窗函数 (原地操作)
 * @data: 输入/输出数据数组 (原地修改)
 * @length: 数据长度
 * @window_type: 窗函数类型
 *
 * Return: APP_ERR_OK or error code
 */
int dsp_apply_window(float *data, uint16_t length, enum dsp_window_type window_type);

/* ==================== 查询 API ==================== */

/**
 * dsp_get_stats - 获取 DSP 模块统计信息
 * @stats: 输出统计结构体指针
 *
 * Return: APP_ERR_OK or error code
 */
int dsp_get_stats(struct dsp_stats *stats);

/**
 * dsp_get_max_fft_size - 获取当前允许的最大 FFT 点数
 *
 * Return: 最大 FFT 点数
 */
uint16_t dsp_get_max_fft_size(void);

#endif /* DSP_H */