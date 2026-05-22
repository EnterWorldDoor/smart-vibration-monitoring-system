/*
 * App/dsp_nde.c — NDE DSP管线: 64点RFFT → 24维特征提取
 *
 * 使用自写dsp_fft_q15 (无CMSIS-DSP). Cortex-M3, 无FPU.
 * 特征格式与ESP32 DE完全一致 (24 x float32).
 * 特征[23] = 0.0f (NDE无温度传感器).
 */

#include "dsp_nde.h"
#include "dsp_fft_q15.h"
#include "global_error.h"
#include <string.h>
#include <math.h>

/*
 * 8个频带 (Hz), fs=400Hz, 64点FFT (6.25Hz/bin):
 *   Band 0: 0-25,  1: 25-50,  2: 50-75,   3: 75-100,
 *         4: 100-125, 5: 125-150, 6: 150-175, 7: 175-200
 * Bin 0 = DC (跳过). 最大bin 32 = 200Hz (Nyquist)
 */
#define NUM_BANDS       8

static const uint8_t band_bins[NUM_BANDS + 1] = {
	0, 4, 8, 12, 16, 20, 24, 28, 33
};

/* ---- 静态工作缓冲区 ---- */
static q15_t s_window_coeff[DSP_NDE_FFT_SIZE];
static q15_t s_fft_input[DSP_NDE_FFT_SIZE];
static q15_t s_fft_output[DSP_FFT_BINS * 2];    /* 33个复数对 */
static q15_t s_magnitude[DSP_FFT_BINS];
static int8_t s_initialized;

int dsp_nde_init(void)
{
	int ret;
	int i;

	ret = dsp_fft_init();
	if (ret < 0)
		return ERR_DSP_INIT_FAIL;

	/* 生成Hann窗系数 (q15) */
	for (i = 0; i < DSP_NDE_FFT_SIZE; i++) {
		float w;

		w = 0.5f * (1.0f - cosf(2.0f * M_PI * i /
		      (DSP_NDE_FFT_SIZE - 1)));
		s_window_coeff[i] = (q15_t)(w * 32767.0f);
	}

	s_initialized = 1;
	return 0;
}

/*
 * 频谱特征提取: q15转换+FFT+幅值+峰值+频带能量.
 * 在q15域完成, 输出float用于特征向量组装.
 */
static void dsp_axis_spectral(const int16_t *raw, uint16_t count,
			      float *peak_freq, float *peak_amp,
			      float *band_energy)
{
	int i, bin, b;
	int n;
	float sum;
	float spec_mag[DSP_FFT_BINS];

	/* 1. int16→q15 + Hann窗 */
	n = (count < DSP_NDE_FFT_SIZE) ? count : DSP_NDE_FFT_SIZE;
	for (i = 0; i < n; i++) {
		q31_t prod;

		prod = (q31_t)raw[i] * s_window_coeff[i];
		s_fft_input[i] = (q15_t)((prod + 0x4000) >> 15);
	}
	for (; i < DSP_NDE_FFT_SIZE; i++)
		s_fft_input[i] = 0;

	/* 2. 64点实FFT → 33复数bin */
	dsp_fft_rfft_q15(s_fft_input, s_fft_output);

	/* 3. 幅值 (q15 → float) */
	dsp_fft_mag_q15(s_fft_output, s_magnitude, DSP_FFT_BINS);

	/* 4. 频谱分析 */
	sum = 0.0f;
	*peak_amp = 0.0f;
	*peak_freq = 0.0f;
	memset(band_energy, 0, NUM_BANDS * sizeof(float));

	for (bin = 0; bin < DSP_FFT_BINS; bin++) {
		spec_mag[bin] = (float)s_magnitude[bin] / 32768.0f;
		sum += spec_mag[bin];

		if (spec_mag[bin] > *peak_amp) {
			*peak_amp = spec_mag[bin];
			*peak_freq = bin * (400.0f / DSP_NDE_FFT_SIZE);
		}

		for (b = 0; b < NUM_BANDS; b++) {
			if (bin >= band_bins[b] && bin < band_bins[b + 1])
				band_energy[b] += spec_mag[bin];
		}
	}

	/* 频带能量归一化 */
	sum = (sum > 0.001f) ? sum : 1.0f;
	for (b = 0; b < NUM_BANDS; b++)
		band_energy[b] /= sum;
}

/*
 * 时域统计特征: RMS, 均值, 方差, 偏度, 峰度, 峰值, 波峰因子.
 * 全部软浮点计算 (M3无FPU).
 */
static void dsp_axis_temporal(const int16_t *raw, uint16_t count,
			      float *rms_val, float *peak_val,
			      float *skewness, float *kurtosis,
			      float *crest)
{
	int i;
	float sum_sq, sum_val, mean_val, variance, std_dev;
	float diff, abs_val;

	sum_sq = 0.0f;
	sum_val = 0.0f;
	*peak_val = 0.0f;

	for (i = 0; i < count; i++) {
		float fval;

		fval = (float)raw[i];
		sum_sq += fval * fval;
		sum_val += fval;
	}

	/* RMS */
	*rms_val = sqrtf(sum_sq / (float)count);

	/* 均值 */
	mean_val = sum_val / (float)count;

	/* 方差 + 偏度 + 峰度 + 峰值 */
	variance = 0.0f;
	*skewness = 0.0f;
	*kurtosis = 0.0f;

	for (i = 0; i < count; i++) {
		diff = (float)raw[i] - mean_val;
		variance += diff * diff;
		*skewness += diff * diff * diff;
		*kurtosis += diff * diff * diff * diff;

		abs_val = (diff > 0.0f) ? diff : -diff;
		if (abs_val > *peak_val)
			*peak_val = abs_val;
	}

	variance /= (float)count;
	std_dev = sqrtf(variance);

	if (std_dev > 0.001f) {
		*skewness = *skewness / (float)count /
			    (variance * std_dev);
		*kurtosis = *kurtosis / (float)count /
			    (variance * variance);
	} else {
		*skewness = 0.0f;
		*kurtosis = 0.0f;
	}

	/* 波峰因子 */
	*crest = (*rms_val > 0.001f) ? *peak_val / *rms_val : 1.0f;
}

/*
 * 单轴特征提取 (编排频谱+时域).
 * 输出14维: rms, peak_freq, peak_amp, skewness, kurtosis, crest, band[0..7]
 */
static void dsp_process_axis(const int16_t *raw, uint16_t count,
			     float *out_features, uint8_t feat_offset)
{
	float rms_val, peak_freq, peak_amp;
	float skewness, kurtosis, peak_val, crest;
	float band_energy[NUM_BANDS];

	if (!s_initialized)
		return;

	dsp_axis_spectral(raw, count, &peak_freq, &peak_amp, band_energy);
	dsp_axis_temporal(raw, count, &rms_val, &peak_val,
			  &skewness, &kurtosis, &crest);

	out_features[feat_offset + 0] = rms_val;
	out_features[feat_offset + 1] = peak_freq;
	out_features[feat_offset + 2] = peak_amp;
	out_features[feat_offset + 3] = skewness;
	out_features[feat_offset + 4] = kurtosis;
	out_features[feat_offset + 5] = crest;
	memcpy(&out_features[feat_offset + 6], band_energy,
	       NUM_BANDS * sizeof(float));
}

int dsp_nde_process(const int16_t *x, const int16_t *y, const int16_t *z,
		    uint16_t count, struct dsp_nde_result *out)
{
	int i;
	float x_feat[14], y_feat[14], z_feat[14];
	float overall;

	if (!x || !y || !z || !out || !s_initialized)
		return ERR_INVALID_PARAM;

	/* 每轴14维特征 */
	dsp_process_axis(x, count, x_feat, 0);
	dsp_process_axis(y, count, y_feat, 0);
	dsp_process_axis(z, count, z_feat, 0);

	/* 总体RMS (向量幅度) */
	overall = 0.0f;
	for (i = 0; i < count; i++) {
		overall += (float)x[i] * x[i]
			+ (float)y[i] * y[i]
			+ (float)z[i] * z[i];
	}
	overall = sqrtf(overall / (3.0f * count));

	/*
	 * 组装为ESP32格式:
	 * [0-3]:   rms_x, rms_y, rms_z, overall_rms
	 * [4-5]:   peak_freq_x, peak_amp_x
	 * [6-8]:   skewness_x, kurtosis_x, crest_factor_x
	 * [9-16]:  band_energy_x[0..7]
	 * [17-19]: peak_freq_y, peak_amp_y, crest_factor_y
	 * [20-22]: peak_freq_z, peak_amp_z, crest_factor_z
	 * [23]:    0.0f (无温度传感器)
	 */
	out->features[0]  = x_feat[0];   /* rms_x */
	out->features[1]  = y_feat[0];   /* rms_y */
	out->features[2]  = z_feat[0];   /* rms_z */
	out->features[3]  = overall;     /* overall_rms */
	out->features[4]  = x_feat[1];   /* peak_freq_x */
	out->features[5]  = x_feat[2];   /* peak_amp_x */
	out->features[6]  = x_feat[3];   /* skewness_x */
	out->features[7]  = x_feat[4];   /* kurtosis_x */
	out->features[8]  = x_feat[5];   /* crest_factor_x */
	for (i = 0; i < 8; i++)
		out->features[9 + i]  = x_feat[6 + i];  /* band_energy_x */
	out->features[17] = y_feat[1];   /* peak_freq_y */
	out->features[18] = y_feat[2];   /* peak_amp_y */
	out->features[19] = y_feat[5];   /* crest_factor_y */
	out->features[20] = z_feat[1];   /* peak_freq_z */
	out->features[21] = z_feat[2];   /* peak_amp_z */
	out->features[22] = z_feat[5];   /* crest_factor_z */
	out->features[23] = 0.0f;        /* 无温度传感器 */

	return 0;
}
