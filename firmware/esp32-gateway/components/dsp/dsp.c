/**
 * @file dsp.c
 * @author EnterWorldDoor
 * @brief DSP 算法实现(使用 ARM CMSIS-DSP 库或简易实现)
 * 注意: 这里的实现是简化版本，实际项目中建议使用高效的 DSP 库(如 CMSIS-DSP)以获得更好的性能和准确性
 */

 #include "dsp.h"
 #include "global_log.h"
 #include "global_error.h"
 #include <math.h>
 #include <string.h>
 #include <stdlib.h>

 /* 简易复数结构 */
 typedef struct {
    float real;
    float imag;
 } complex_t;

 /* 静态工作区(避免重复分配) */
 static complex_t *g_fft_work = NULL;
 static uint16_t g_fft_max_size = 0;
 
 /* 私有: 位反转重排 */
 static void bit_reverse(complex_t *x, uint16_t n)
 {
    uint16_t i, j, k;
    for (i = 0, j = 0; i < n - 1; i++) {
        if (i < j) {
            complex_t temp = x[i];
            x[i] = x[j];
            x[j] = temp;
        }
        k = n >> 1;
        while (k <= j) {
            j -= k;
            k >>= 1;
        }
        j += k;
    }
 }

 /* 私有: FFT 蝶形运算 (基2, 实现输入直接使用实部) */
 static void fft_compute(complex_t *x, uint16_t n, int invert)
 {
    bit_reverse(x,n);
    for (uint16_t len = 2; len <= n; len <<= 1) {
        float angle = 2 * M_PI / len * (invert ? -1 : 1);
        complex_t w,wn = {cosf(angle), sinf(angle)};
        for (uint16_t i = 0; i < n; i += len) {
            w.real = 1.0f;
            w.imag = 0.0f;
            for (uint16_t j = 0; j < len / 2; j++) {
                complex_t u = x[i + j];
                complex_t v = {x[i + j + len / 2].real * w.real - x[i + j + len / 2].imag * w.imag,
                               x[i + j + len / 2].real * w.imag + x[i + j + len / 2].imag * w.real};
                x[i + j].real = u.real + v.real;
                x[i + j].imag = u.imag + v.imag;
                x[i + j + len / 2].real = u.real - v.real;
                x[i + j + len / 2].imag = u.imag - v.imag;
                float w_real_temp = w.real;
                w.imag = w.real * wn.imag + w.imag * wn.real;
                w.real = w_real_temp * wn.real - w.imag * wn.imag;
            }
        }
    }

    if (invert) {
        for (uint16_t i = 0; i < n; i++) {
            x[i].real /= n;
            x[i].imag /= n;
        }
    }
 }

 int dsp_init(void)
 {
    /* 预分配最大 FFT 工作区 */
    g_fft_max_size = DSP_FFT_MAX_SIZE;
    g_fft_work = malloc(sizeof(complex_t) * g_fft_max_size);
    if (!g_fft_work) {
        LOG_ERROR("Failed to allocate FFT work buffer");
        return ERR_NO_MEM;
    }
    LOG_INFO("DSP module initialized with FFT max size: %d", g_fft_max_size);
    return ERR_OK;
 }

 int dsp_rms_compute(const float *input, int len, struct rms_result *out)
 {
    if (!input || !out || len <= 0) return ERR_INVALID_PARAM;

    double sum = 0.0, sum_sq = 0.0;
    float max_val = -1e9f, min_val = 1e9f;
    for (int i = 0; i < len; i++) {
        sum += input[i];
        sum_sq += input[i] * input[i];
        if (input[i] > max_val) max_val = input[i];
        if (input[i] < min_val) min_val = input[i];
    }
    out->mean = (float)(sum / len);
    out->value = (float)sqrt(sum_sq / len);
    out->peak = (max_val > -min_val) ? max_val : min_val; /* 峰值取绝对值较大的那个 */
    return ERR_OK;
 }

 int dsp_fft_compute(float *input, uint16_t fft_size, float sampling_rate, struct fft_result *out)
 {
    if (!input || !out || fft_size == 0 || (fft_size & (fft_size - 1)) != 0) {
        return ERR_INVALID_PARAM;
    }  
    if (fft_size > DSP_FFT_MAX_SIZE) return ERR_INVALID_PARAM;

    /* 准备复数数组 */
    for (uint16_t i = 0; i < fft_size; i++) {
        g_fft_work[i].real = input[i];
        g_fft_work[i].imag = 0.0f;
    }

    /* 执行 FFT */
    fft_compute(g_fft_work, fft_size, 0);

    /* 计算幅值谱(单边谱, 幅度放大 2/N, 直流分量除外) */
    float n_float = (float)fft_size;
    out->fft_size = fft_size;
    out->sampling_rate = sampling_rate;
    out->peak_freq = 0.0f;

    uint16_t num_bins = fft_size / 2;
    for (uint16_t i = 0; i < num_bins; i++) {
        float mag = sqrtf(g_fft_work[i].real * g_fft_work[i].real + g_fft_work[i].imag * g_fft_work[i].imag);
        if (i == 0) {
            mag /= n_float;             /* 直流分量 */
        } else {
            mag *= 2.0f / n_float;      /* 交流分量单边谱 */
        }
        out->spectrum[i] = mag;

        if (i > 0 && mag > out->peak_amp) {
            out->peak_amp = mag;
            out->peak_freq = (float)i * sampling_rate / n_float;
        }
    }

    return ERR_OK;
 }

 int dsp_fft_compute_3axis(const float *x_samples, const float *y_samples, const float *z_samples, uint16_t len,
                           float smapling_rate, struct fft_result *out_x, struct fft_result *out_y, struct fft_result *out_z)
 {
    if (!x_samples || !y_samples || !z_samples) return ERR_INVALID_PARAM;

    /* 复制输入以避免修改原数据(因为 FFT 会被破坏输入) */
    float *buf = malloc(sizeof(float) * len);
    if (!buf) return ERR_NO_MEM;

    int ret;
    if (out_x) {
        memcpy(buf, x_samples, len * sizeof(float));
        ret = dsp_fft_compute(buf, len, smapling_rate, out_x);
        if (ret != ERR_OK) goto cleanup;
    }

    if (out_y) {
        memcpy(buf, y_samples, len * sizeof(float));
        ret = dsp_fft_compute(buf, len, smapling_rate, out_y);
        if (ret != ERR_OK) goto cleanup;
    }

    if (out_z) {
        memcpy(buf, z_samples, len * sizeof(float));
        ret = dsp_fft_compute(buf, len, smapling_rate, out_z);
        if (ret != ERR_OK) goto cleanup;
    }
    ret = ERR_OK;

    cleanup:
        free(buf);
        return ret;
 }                          