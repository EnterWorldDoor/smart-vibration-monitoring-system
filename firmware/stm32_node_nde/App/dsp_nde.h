/*
 * App/dsp_nde.h — NDE DSP接口: q15 FFT + 24维特征提取
 *
 * 使用自写dsp_fft_q15 (无CMSIS-DSP依赖).
 * 特征向量格式与ESP32 DE侧完全一致 (24 x float32).
 */

#ifndef __DSP_NDE_H
#define __DSP_NDE_H

#include <stdint.h>

#define DSP_NDE_FFT_SIZE    64
#define DSP_NDE_FEAT_DIM    24

struct dsp_nde_result {
	float features[DSP_NDE_FEAT_DIM];
	uint8_t window_idx;
};

int dsp_nde_init(void);
int dsp_nde_process(const int16_t *x, const int16_t *y, const int16_t *z,
		    uint16_t count, struct dsp_nde_result *out);

#endif /* __DSP_NDE_H */
