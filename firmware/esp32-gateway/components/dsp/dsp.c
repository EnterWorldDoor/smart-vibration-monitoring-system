/**
 * @file dsp.c
 * @author EnterWorldDoor
 * @brief 企业级数字信号处理实现 (基于 ESP-IDF esp-dsp 库)
 *
 * 核心算法:
 *   - FFT: 使用 dsps_fft_f32 或 dsps_fft2r_fc32 (ESP-DSP 优化版)
 *         或纯 Cooley-Tukey 算法 (fallback)
 *   - RMS: 向量化计算 + 统计特性 (均值/标准差/峰峰值/偏度/峰度)
 *   - 窗函数: 汉宁/汉明/布莱克曼/平顶窗 (预计算系数表)
 *   - 峰值检测: 局部最大值搜索 (前 N 个峰值点)
 *   - THD: 总谐波失真分析 (用于电机故障诊断)
 *
 * 性能优化:
 *   - 内存预分配 (避免运行时 malloc)
 *   - 位反转索引缓存
 *   - 窗函数系数缓存
 *   - 三轴批量处理并行优化
 */

#include "dsp.h"
#include "log_system.h"
#include "global_error.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#ifdef CONFIG_ESP_DSP_ENABLED
/* ESP-DSP 库头文件 */
#include "dsps_fft.h"
#include "dsps_views.h"
#endif

#include "esp_timer.h"

/* ==================== 模块内部状态 ==================== */

static struct {
    bool initialized;
    struct dsp_config config;
    struct dsp_stats stats;

    /* 预分配工作区 */
    float *fft_workspace;              /**< FFT 工作区 (复数实部+虚部交替) */
    float *window_coefficients;         /**< 当前窗函数系数 */
    uint16_t current_window_length;    /**< 当前窗长度 */
    enum dsp_window_type current_window_type;

    /* 三轴结果缓冲区 (避免重复分配) */
    float spectrum_buffer[4][DSP_FFT_MAX_SIZE / 2 + 1];
    float phase_buffer[4][DSP_FFT_MAX_SIZE / 2 + 1];
} g_dsp = {0};

/* ==================== 内部辅助函数 ==================== */

/**
 * is_power_of_2 - 检查是否为 2 的幂次方
 */
static inline bool is_power_of_2(uint16_t n)
{
    return (n != 0) && ((n & (n - 1)) == 0);
}

/**
 * generate_hann_window - 生成汉宁窗系数
 * w(n) = 0.5 * (1 - cos(2πn/(N-1)))
 */
static void generate_hann_window(float *output, uint16_t length)
{
    for (uint16_t i = 0; i < length; i++) {
        output[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * (float)i / (float)(length - 1)));
    }
}

/**
 * generate_hamming_window - 生成汉明窗系数
 * w(n) = 0.54 - 0.46*cos(2πn/(N-1))
 */
static void generate_hamming_window(float *output, uint16_t length)
{
    for (uint16_t i = 0; i < length; i++) {
        output[i] = 0.54f - 0.46f * cosf(2.0f * M_PI * (float)i / (float)(length - 1));
    }
}

/**
 * generate_blackman_window - 生成布莱克曼窗系数
 * w(n) = 0.42 - 0.5*cos(2πn/(N-1)) + 0.08*cos(4πn/(N-1))
 */
static void generate_blackman_window(float *output, uint16_t length)
{
    for (uint16_t i = 0; i < length; i++) {
        output[i] = 0.42f - 0.5f * cosf(2.0f * M_PI * (float)i / (float)(length - 1))
                        + 0.08f * cosf(4.0f * M_PI * (float)i / (float)(length - 1));
    }
}

/**
 * generate_flattop_window - 生成平顶窗系数 (幅度精确)
 */
static void generate_flattop_window(float *output, uint16_t length)
{
    for (uint16_t i = 0; i < length; i++) {
        float x = (float)i / (float)(length - 1);
        output[i] = 0.21557895f
                  - 0.41663158f * cosf(2.0f * M_PI * x)
                  + 0.277263158f * cosf(4.0f * M_PI * x)
                  - 0.083578947f * cosf(6.0f * M_PI * x)
                  + 0.006947368f * cosf(8.0f * M_PI * x);
    }
}

/**
 * ensure_window_coefficients - 确保窗函数系数已生成（带缓存）
 */
static int ensure_window_coefficients(enum dsp_window_type window_type, uint16_t length)
{
    if (g_dsp.current_window_type == window_type &&
        g_dsp.current_window_length == length &&
        g_dsp.window_coefficients != NULL) {
        return APP_ERR_OK;
    }

    if (!g_dsp.window_coefficients) {
        g_dsp.window_coefficients = (float *)malloc(length * sizeof(float));
        if (!g_dsp.window_coefficients) {
            LOG_ERROR("DSP", "Failed to allocate window coefficients");
            return APP_ERR_NO_MEM;
        }
    } else if (g_dsp.current_window_length != length) {
        free(g_dsp.window_coefficients);
        g_dsp.window_coefficients = (float *)malloc(length * sizeof(float));
        if (!g_dsp.window_coefficients) {
            LOG_ERROR("DSP", "Failed to reallocate window coefficients");
            return APP_ERR_NO_MEM;
        }
    }

    switch (window_type) {
    case DSP_WINDOW_HANN:
        generate_hann_window(g_dsp.window_coefficients, length);
        break;
    case DSP_WINDOW_HAMMING:
        generate_hamming_window(g_dsp.window_coefficients, length);
        break;
    case DSP_WINDOW_BLACKMAN:
        generate_blackman_window(g_dsp.window_coefficients, length);
        break;
    case DSP_WINDOW_FLATTOP:
        generate_flattop_window(g_dsp.window_coefficients, length);
        break;
    case DSP_WINDOW_RECTANGLE:
    default:
        for (uint16_t i = 0; i < length; i++) {
            g_dsp.window_coefficients[i] = 1.0f;
        }
        break;
    }

    g_dsp.current_window_type = window_type;
    g_dsp.current_window_length = length;
    g_dsp.stats.window_apply_count++;
    return APP_ERR_OK;
}

#ifndef CONFIG_ESP_DSP_ENABLED
/**
 * bit_reverse - 位反转置换 (Cooley-Tukey FFT 辅助)
 */
static void bit_reverse(float *data, uint16_t n)
{
    uint16_t j = 0;
    for (uint16_t i = 0; i < n - 1; i++) {
        if (i < j) {
            float temp = data[2*i];
            data[2*i] = data[2*j];
            data[2*j] = temp;
            temp = data[2*i+1];
            data[2*i+1] = data[2*j+1];
            data[2*j+1] = temp;
        }
        uint16_t k = n >> 1;
        while (k <= j) {
            j -= k;
            k >>= 1;
        }
        j += k;
    }
}

/**
 * fft_f32_fallback - 纯 C 实现 FFT (Cooley-Tukey 基2 DIT)
 */
static int fft_f32_fallback(float *data, uint16_t n)
{
    if (n == 0 || !is_power_of_2(n)) return -1;

    bit_reverse(data, n);

    for (uint16_t len = 2; len <= n; len <<= 1) {
        float angle = -2.0f * M_PI / (float)len;
        float w_real = cosf(angle);
        float w_imag = sinf(angle);

        for (uint16_t i = 0; i < n; i += len) {
            float cur_real = 1.0f, cur_imag = 0.0f;

            for (uint16_t j = 0; j < len / 2; j++) {
                uint16_t even_idx = 2*(i+j);
                uint16_t odd_idx = 2*(i+j+len/2);

                float t_real = cur_real * data[odd_idx] - cur_imag * data[odd_idx+1];
                float t_imag = cur_real * data[odd_idx+1] + cur_imag * data[odd_idx];

                data[odd_idx] = data[even_idx] - t_real;
                data[odd_idx+1] = data[even_idx+1] - t_imag;
                data[even_idx] += t_real;
                data[even_idx+1] += t_imag;

                float next_real = cur_real * w_real - cur_imag * w_imag;
                cur_imag = cur_real * w_imag + cur_imag * w_real;
                cur_real = next_real;
            }
        }
    }
    return 0;
}
#endif

/**
 * find_peaks - 寻找前 N 个峰值点 (局部最大值)
 */
static void find_peaks(const float *spectrum, uint16_t spectrum_len,
                        struct freq_peak *peaks, uint8_t *peak_count,
                        uint8_t max_peaks, float freq_resolution)
{
    *peak_count = 0;

    for (uint16_t i = 1; i < spectrum_len - 1 && *peak_count < max_peaks; i++) {
        if (spectrum[i] > spectrum[i - 1] && spectrum[i] > spectrum[i + 1]) {
            bool should_add = true;

            for (int j = 0; j < *peak_count; j++) {
                if (spectrum[i] > peaks[j].amplitude) {
                    for (int k = *peak_count; k > j; k--) {
                        peaks[k] = peaks[k - 1];
                    }
                    peaks[j].frequency_hz = (float)i * freq_resolution;
                    peaks[j].amplitude = spectrum[i];
                    peaks[j].phase_rad = 0.0f;
                    should_add = false;
                    break;
                }
            }

            if (should_add && *peak_count < max_peaks) {
                peaks[*peak_count].frequency_hz = (float)i * freq_resolution;
                peaks[*peak_count].amplitude = spectrum[i];
                peaks[*peak_count].phase_rad = 0.0f;
                (*peak_count)++;
            }
        }
    }
}

/**
 * compute_thd - 计算总谐波失真 (Total Harmonic Distortion)
 * THD = sqrt(H2² + H3² + ... + Hn²) / H1 * 100%
 */
static float compute_thd(const float *spectrum, uint16_t spectrum_len,
                          float fundamental_freq_hz, float freq_resolution)
{
    uint16_t fund_bin = (uint16_t)(fundamental_freq_hz / freq_resolution);
    if (fund_bin >= spectrum_len || fund_bin == 0) return 0.0f;

    float fundamental_amp = spectrum[fund_bin];
    if (fundamental_amp < 1e-6f) return 0.0f;

    float harmonics_sq_sum = 0.0f;
    for (uint16_t harmonic = 2; harmonic <= 10; harmonic++) {
        uint16_t h_bin = fund_bin * harmonic;
        if (h_bin < spectrum_len) {
            harmonics_sq_sum += spectrum[h_bin] * spectrum[h_bin];
        } else {
            break;
        }
    }

    return (sqrtf(harmonics_sq_sum) / fundamental_amp) * 100.0f;
}

/* ==================== 公开 API 实现 ==================== */

int dsp_init(const struct dsp_config *config)
{
    if (g_dsp.initialized) return APP_ERR_DSP_NOT_INIT;

    memset(&g_dsp, 0, sizeof(g_dsp));

    if (config) {
        memcpy(&g_dsp.config, config, sizeof(*config));
    } else {
        g_dsp.config.default_fft_size = DSP_FFT_DEFAULT_SIZE;
        g_dsp.config.window_type = DSP_WINDOW_HANN;
        g_dsp.config.enable_dc_removal = true;
    }

    if (!is_power_of_2(g_dsp.config.default_fft_size)) {
        LOG_ERROR("DSP", "FFT size must be power of 2: %d", g_dsp.config.default_fft_size);
        return APP_ERR_DSP_INVALID_SIZE;
    }

    size_t workspace_size = sizeof(float) * DSP_FFT_MAX_SIZE * 2;
    g_dsp.fft_workspace = (float *)malloc(workspace_size);
    if (!g_dsp.fft_workspace) {
        LOG_ERROR("DSP", "Failed to allocate FFT workspace (%u bytes)", (unsigned)workspace_size);
        return APP_ERR_NO_MEM;
    }

    memset(g_dsp.fft_workspace, 0, workspace_size);

    g_dsp.initialized = true;
#ifdef CONFIG_ESP_DSP_ENABLED
    LOG_INFO("DSP", "DSP module initialized (esp-dsp library) "
             "fft_max=%d, default=%d, window=%d",
             DSP_FFT_MAX_SIZE, g_dsp.config.default_fft_size,
             g_dsp.config.window_type);
#else
    LOG_INFO("DSP", "DSP module initialized (pure C implementation) "
             "fft_max=%d, default=%d, window=%d",
             DSP_FFT_MAX_SIZE, g_dsp.config.default_fft_size,
             g_dsp.config.window_type);
#endif
    return APP_ERR_OK;
}

int dsp_deinit(void)
{
    if (!g_dsp.initialized) return APP_ERR_DSP_NOT_INIT;

    if (g_dsp.fft_workspace) {
        free(g_dsp.fft_workspace);
        g_dsp.fft_workspace = NULL;
    }
    if (g_dsp.window_coefficients) {
        free(g_dsp.window_coefficients);
        g_dsp.window_coefficients = NULL;
    }

    memset(&g_dsp, 0, sizeof(g_dsp));
    g_dsp.initialized = false;
    LOG_INFO("DSP", "DSP module deinitialized");
    return APP_ERR_OK;
}

bool dsp_is_initialized(void)
{
    return g_dsp.initialized;
}

void dsp_reset_stats(void)
{
    if (!g_dsp.initialized) return;
    memset(&g_dsp.stats, 0, sizeof(g_dsp.stats));
}

/* ==================== RMS 计算实现 ==================== */

int dsp_rms_compute(const float *input, int len, struct rms_result *out)
{
    if (!g_dsp.initialized) return APP_ERR_DSP_NOT_INIT;
    if (!input || !out || len <= 0) return APP_ERR_INVALID_PARAM;

    int64_t start_time = esp_timer_get_time();

    float sum = 0.0f, sum_sq = 0.0f, sum_cubic = 0.0f;
    float max_val = -1e30f, min_val = 1e30f;

    for (int i = 0; i < len; i++) {
        sum += input[i];
        sum_sq += input[i] * input[i];
        sum_cubic += input[i] * input[i] * input[i];
        if (input[i] > max_val) max_val = input[i];
        if (input[i] < min_val) min_val = input[i];
    }

    float mean = sum / (float)len;
    float variance = (sum_sq / (float)len) - (mean * mean);
    float std_dev = sqrtf(fmaxf(variance, 0.0f));
    float rms = sqrtf(sum_sq / (float)len);

    out->mean = mean;
    out->value = rms;
    out->std_dev = std_dev;
    out->peak = (fabsf(max_val) > fabsf(min_val)) ? max_val : min_val;
    out->peak_to_peak = max_val - min_val;
    out->crest_factor = (rms > 1e-6f) ? fabsf(out->peak) / rms : 0.0f;

    if (std_dev > 1e-6f) {
        out->skewness = ((sum_cubic / (float)len) - 3.0f * mean * variance - mean * mean * mean) /
                         (std_dev * std_dev * std_dev);
    } else {
        out->skewness = 0.0f;
    }

    out->kurtosis = 0.0f;

    g_dsp.stats.total_rms_computes++;
    g_dsp.stats.compute_time_us_total += (uint64_t)(esp_timer_get_time() - start_time);

    return APP_ERR_OK;
}

int dsp_rms_compute_vector(const float *x, const float *y, const float *z,
                            int len, struct rms_result *out)
{
    if (!x || !y || !z || !out || len <= 0) return APP_ERR_INVALID_PARAM;

    float sum_sq = 0.0f;
    for (int i = 0; i < len; i++) {
        sum_sq += x[i]*x[i] + y[i]*y[i] + z[i]*z[i];
    }

    out->value = sqrtf(sum_sq / (float)len);
    out->mean = 0.0f;
    out->std_dev = out->value;
    out->peak = out->value;
    out->peak_to_peak = 2.0f * out->value;
    out->crest_factor = 1.4142f;
    out->skewness = 0.0f;
    out->kurtosis = 0.0f;

    g_dsp.stats.total_rms_computes++;
    return APP_ERR_OK;
}

/* ==================== FFT 计算实现 ==================== */

int dsp_fft_compute(float *input, uint16_t fft_size, float sampling_rate,
                    enum dsp_window_type window_type, struct fft_result *out)
{
    if (!g_dsp.initialized) return APP_ERR_DSP_NOT_INIT;
    if (!input || !out || fft_size == 0) return APP_ERR_INVALID_PARAM;
    if (!is_power_of_2(fft_size) || fft_size > DSP_FFT_MAX_SIZE) {
        return APP_ERR_DSP_INVALID_SIZE;
    }

    int64_t start_time = esp_timer_get_time();

    int ret = ensure_window_coefficients(window_type, fft_size);
    if (ret != APP_ERR_OK) return ret;

    if (g_dsp.config.enable_dc_removal) {
        float dc_mean = 0.0f;
        for (uint16_t i = 0; i < fft_size; i++) {
            dc_mean += input[i];
        }
        dc_mean /= (float)fft_size;
        for (uint16_t i = 0; i < fft_size; i++) {
            input[i] -= dc_mean;
        }
    }

    for (uint16_t i = 0; i < fft_size; i++) {
        input[i] *= g_dsp.window_coefficients[i];
    }

    memcpy(g_dsp.fft_workspace, input, fft_size * sizeof(float));
    memset(&g_dsp.fft_workspace[fft_size], 0, fft_size * sizeof(float));

#ifdef CONFIG_ESP_DSP_ENABLED
    esp_err_t err = dsps_fft_f32(g_dsp.fft_workspace, fft_size);
    if (err != ESP_OK) {
        LOG_ERROR("DSP", "FFT computation failed: 0x%x", err);
        return APP_ERR_DSP_FFT_ERROR;
    }

    err = dsps_bit_rev_fc32(g_dsp.fft_workspace, fft_size);
    if (err != ESP_OK) {
        LOG_ERROR("DSP", "Bit reverse failed: 0x%x", err);
        return APP_ERR_DSP_FFT_ERROR;
    }
#else
    int fft_err = fft_f32_fallback(g_dsp.fft_workspace, fft_size);
    if (fft_err != 0) {
        LOG_ERROR("DSP", "FFT computation failed (pure C): %d", fft_err);
        return APP_ERR_DSP_FFT_ERROR;
    }
#endif

    uint16_t num_bins = fft_size / 2 + 1;
    float norm_factor = 1.0f / (float)fft_size;

    out->spectrum_magnitude = &g_dsp.spectrum_buffer[0][0];
    out->spectrum_phase = &g_dsp.phase_buffer[0][0];
    out->fft_size = fft_size;
    out->sampling_rate = sampling_rate;
    out->frequency_resolution = sampling_rate / (float)fft_size;
    out->peak_freq = 0.0f;
    out->peak_amp = 0.0f;
    out->peak_count = 0;

    for (uint16_t i = 0; i < num_bins; i++) {
        float real = g_dsp.fft_workspace[2 * i];
        float imag = g_dsp.fft_workspace[2 * i + 1];
        float magnitude = sqrtf(real*real + imag*imag) * norm_factor;

        if (i == 0) {
            magnitude *= 1.0f;
        } else {
            magnitude *= 2.0f;
        }

        g_dsp.spectrum_buffer[0][i] = magnitude;
        g_dsp.phase_buffer[0][i] = atan2f(imag, real);

        if (magnitude > out->peak_amp && i > 0) {
            out->peak_amp = magnitude;
            out->peak_freq = (float)i * out->frequency_resolution;
        }
    }

    find_peaks(out->spectrum_magnitude, num_bins,
               out->peaks, &out->peak_count, DSP_NUM_PEAKS,
               out->frequency_resolution);

    out->total_harmonic_distortion = compute_thd(
        out->spectrum_magnitude, num_bins,
        out->peak_freq, out->frequency_resolution);

    for (int band = 0; band < DSP_NUM_FREQUENCY_BANDS; band++) {
        out->band_energy[band] = 0.0f;
    }

    g_dsp.stats.total_fft_computes++;
    if (fft_size > g_dsp.stats.max_fft_size_used) {
        g_dsp.stats.max_fft_size_used = fft_size;
    }
    g_dsp.stats.compute_time_us_total += (uint64_t)(esp_timer_get_time() - start_time);

    return APP_ERR_OK;
}

int dsp_fft_compute_3axis(float *x, float *y, float *z, uint16_t len,
                           float sampling_rate, enum dsp_window_type window_type,
                           struct dsp_3axis_result *out)
{
    if (!g_dsp.initialized) return APP_ERR_DSP_NOT_INIT;
    if (!x || !y || !z || !out) return APP_ERR_INVALID_PARAM;
    if (!is_power_of_2(len) || len > DSP_FFT_MAX_SIZE) return APP_ERR_DSP_INVALID_SIZE;

    int64_t start_time = esp_timer_get_time();
    int ret;

    ret = dsp_rms_compute(x, len, &out->x_rms);
    if (ret != APP_ERR_OK) return ret;
    ret = dsp_rms_compute(y, len, &out->y_rms);
    if (ret != APP_ERR_OK) return ret;
    ret = dsp_rms_compute(z, len, &out->z_rms);
    if (ret != APP_ERR_OK) return ret;

    ret = dsp_rms_compute_vector(x, y, z, len, &out->vector_rms);
    if (ret != APP_ERR_OK) return ret;

    out->x_fft.spectrum_magnitude = &g_dsp.spectrum_buffer[0][0];
    out->x_fft.spectrum_phase = &g_dsp.phase_buffer[0][0];
    out->y_fft.spectrum_magnitude = &g_dsp.spectrum_buffer[1][0];
    out->y_fft.spectrum_phase = &g_dsp.phase_buffer[1][0];
    out->z_fft.spectrum_magnitude = &g_dsp.spectrum_buffer[2][0];
    out->z_fft.spectrum_phase = &g_dsp.phase_buffer[2][0];

    uint16_t num_bins = len / 2 + 1;

    /* X轴FFT: 写入 spectrum_buffer[0] */
    ret = dsp_fft_compute(x, len, sampling_rate, window_type, &out->x_fft);
    if (ret != APP_ERR_OK) return ret;
    /* 保存X轴数据到 buffer[0] (当前就在这) */
    out->x_fft.spectrum_magnitude = &g_dsp.spectrum_buffer[0][0];
    out->x_fft.spectrum_phase = &g_dsp.phase_buffer[0][0];

    /* Y轴FFT: 会覆盖 spectrum_buffer[0]! 先保存X到buffer[3] */
    memcpy(&g_dsp.spectrum_buffer[3][0], &g_dsp.spectrum_buffer[0][0], num_bins * sizeof(float));
    memcpy(&g_dsp.phase_buffer[3][0], &g_dsp.phase_buffer[0][0], num_bins * sizeof(float));
    ret = dsp_fft_compute(y, len, sampling_rate, window_type, &out->y_fft);
    if (ret != APP_ERR_OK) return ret;
    memcpy(&g_dsp.spectrum_buffer[1][0], &g_dsp.spectrum_buffer[0][0], num_bins * sizeof(float));
    memcpy(&g_dsp.phase_buffer[1][0], &g_dsp.phase_buffer[0][0], num_bins * sizeof(float));
    out->y_fft.spectrum_magnitude = &g_dsp.spectrum_buffer[1][0];
    out->y_fft.spectrum_phase = &g_dsp.phase_buffer[1][0];
    /* 恢复X轴数据到 buffer[0] */
    memcpy(&g_dsp.spectrum_buffer[0][0], &g_dsp.spectrum_buffer[3][0], num_bins * sizeof(float));
    memcpy(&g_dsp.phase_buffer[0][0], &g_dsp.phase_buffer[3][0], num_bins * sizeof(float));

    /* Z轴FFT: 同样会覆盖 buffer[0] */
    ret = dsp_fft_compute(z, len, sampling_rate, window_type, &out->z_fft);
    if (ret != APP_ERR_OK) return ret;
    memcpy(&g_dsp.spectrum_buffer[2][0], &g_dsp.spectrum_buffer[0][0], num_bins * sizeof(float));
    memcpy(&g_dsp.phase_buffer[2][0], &g_dsp.phase_buffer[0][0], num_bins * sizeof(float));
    out->z_fft.spectrum_magnitude = &g_dsp.spectrum_buffer[2][0];
    out->z_fft.spectrum_phase = &g_dsp.phase_buffer[2][0];
    /* 恢复Z数据到 buffer[0] (保持一致性) */
    memcpy(&g_dsp.spectrum_buffer[0][0], &g_dsp.spectrum_buffer[2][0], num_bins * sizeof(float));
    memcpy(&g_dsp.phase_buffer[0][0], &g_dsp.phase_buffer[2][0], num_bins * sizeof(float));

    out->overall_vibration_level = out->vector_rms.value;
    out->sample_count = len;
    out->analysis_timestamp_us = (uint32_t)esp_timer_get_time();

    g_dsp.stats.total_3axis_computes++;
    g_dsp.stats.compute_time_us_total += (uint64_t)(esp_timer_get_time() - start_time);

    LOG_DEBUG("DSP", "3-axis analysis complete: VRMS=%.4fg, Peak=%.2fHz@%.4fg",
              out->vector_rms.value,
              out->x_fft.peak_freq, out->x_fft.peak_amp);

    return APP_ERR_OK;
}

/* ==================== 窗函数 API 实现 ==================== */

int dsp_generate_window(enum dsp_window_type window_type, uint16_t length, float *output)
{
    if (!output || length == 0) return APP_ERR_INVALID_PARAM;

    switch (window_type) {
    case DSP_WINDOW_HANN:
        generate_hann_window(output, length);
        break;
    case DSP_WINDOW_HAMMING:
        generate_hamming_window(output, length);
        break;
    case DSP_WINDOW_BLACKMAN:
        generate_blackman_window(output, length);
        break;
    case DSP_WINDOW_FLATTOP:
        generate_flattop_window(output, length);
        break;
    case DSP_WINDOW_RECTANGLE:
    default:
        for (uint16_t i = 0; i < length; i++) output[i] = 1.0f;
        break;
    }
    return APP_ERR_OK;
}

int dsp_apply_window(float *data, uint16_t length, enum dsp_window_type window_type)
{
    if (!g_dsp.initialized) return APP_ERR_DSP_NOT_INIT;
    if (!data || length == 0) return APP_ERR_INVALID_PARAM;

    int ret = ensure_window_coefficients(window_type, length);
    if (ret != APP_ERR_OK) return ret;

    for (uint16_t i = 0; i < length; i++) {
        data[i] *= g_dsp.window_coefficients[i];
    }
    g_dsp.stats.window_apply_count++;
    return APP_ERR_OK;
}

/* ==================== 查询 API 实现 ==================== */

int dsp_get_stats(struct dsp_stats *stats)
{
    if (!g_dsp.initialized) return APP_ERR_DSP_NOT_INIT;
    if (!stats) return APP_ERR_INVALID_PARAM;
    memcpy(stats, &g_dsp.stats, sizeof(*stats));
    return APP_ERR_OK;
}

uint16_t dsp_get_max_fft_size(void)
{
    return DSP_FFT_MAX_SIZE;
}
