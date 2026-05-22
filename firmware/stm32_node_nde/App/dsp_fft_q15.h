/**
 * @file dsp_fft_q15.h
 * @brief Minimal 64-point real FFT for STM32F103 (Cortex-M3, no FPU)
 *
 * Self-contained fixed-point (q15) radix-2 DIT FFT. Replaces CMSIS-DSP
 * which would consume ~62KB Flash for constant tables — unsuitable for
 * the F103C8T6's 64KB Flash budget.
 *
 * Size: ~2KB code + tables. 64-point RFFT ≈ 6000 cycles @ 72MHz ≈ 83µs.
 */

#ifndef __DSP_FFT_Q15_H
#define __DSP_FFT_Q15_H

#include <stdint.h>

#define DSP_FFT_SIZE    64
#define DSP_CFFT_SIZE   32   /* N/2 */
#define DSP_FFT_BINS    33   /* N/2 + 1 */

/* Fixed-point types compatible with CMSIS-DSP naming */
typedef int16_t  q15_t;
typedef int32_t  q31_t;
typedef uint16_t uq15_t;

/**
 * dsp_fft_init - Pre-compute nothing (tables are static const).
 *               Provided for symmetry with CMSIS-DSP API.
 *
 * Return: 0 always.
 */
int dsp_fft_init(void);

/**
 * dsp_fft_rfft_q15 - 64-point real FFT
 * @input:  64 q15 samples (time domain, real)
 * @output: 66 q15 values — 33 complex pairs (real, imag) interleaved.
 *          output[0..1] = bin 0 (DC), output[2..3] = bin 1, ...
 *          output[64..65] = bin 32 (Nyquist).
 *
 * Internally packs real input into 32-point complex FFT via the standard
 * even/odd trick, computes radix-2 DIT CFFT, then unpacks to 33 bins.
 * Each FFT stage right-shifts by 1 to prevent overflow (overall /32 scaling).
 */
void dsp_fft_rfft_q15(const q15_t *input, q15_t *output);

/**
 * dsp_fft_mag_q15 - Complex magnitude
 * @src:  N complex pairs (real, imag interleaved) in q15
 * @dst:  N q15 magnitudes (sqrt(re^2 + im^2), saturated to 32767)
 * @n:    Number of complex pairs
 */
void dsp_fft_mag_q15(const q15_t *src, q15_t *dst, int n);

#endif /* __DSP_FFT_Q15_H */
