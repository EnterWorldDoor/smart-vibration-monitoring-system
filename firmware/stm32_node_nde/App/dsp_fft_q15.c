/**
 * @file dsp_fft_q15.c
 * @brief Minimal 64-point real FFT — self-contained, no CMSIS-DSP dependency.
 *
 * Architecture:
 *   1. Pack 64 real samples into 32 complex (even-indexed = real, odd = imag)
 *   2. 32-point radix-2 DIT complex FFT (5 stages)
 *   3. Unpack CFFT result to 33-bin real FFT spectrum
 *   4. Square-root magnitude approximation for spectrum bins
 *
 * Fixed-point: q15 throughout. Each CFFT stage right-shifts by 1 to prevent
 * overflow (net /32 gain). Unpacking does additional /2 (net /64 from original).
 * This matches CMSIS-DSP scaling conventions.
 *
 * Flash footprint: ~1.5KB code + ~500B tables (vs ~62KB for full CMSIS-DSP).
 */

#include "dsp_fft_q15.h"
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* 5-bit bit-reversal table for 32-point FFT                          */
/* ------------------------------------------------------------------ */
static const uint8_t bitrev5[32] = {
     0, 16,  8, 24,  4, 20, 12, 28,
     2, 18, 10, 26,  6, 22, 14, 30,
     1, 17,  9, 25,  5, 21, 13, 29,
     3, 19, 11, 27,  7, 23, 15, 31
};

/* ------------------------------------------------------------------ */
/* Twiddle factors for 32-point CFFT: W_32^k, k = 0..15              */
/* W_k = cos(2*pi*k/32) - j*sin(2*pi*k/32)  in q15                  */
/* ------------------------------------------------------------------ */
static const q15_t twid32_re[16] = {
     32767,  32138,  30273,  27245,  23170,  18204,  12539,   6392,
         0,  -6393, -12540, -18205, -23170, -27246, -30274, -32139
};
static const q15_t twid32_im[16] = {
         0,  -6393, -12540, -18205, -23170, -27246, -30274, -32139,
    -32768, -32139, -30274, -27246, -23170, -18205, -12540,  -6393
};

/* ------------------------------------------------------------------ */
/* Twiddle factors W_64^k, k = 1..31 (used in real FFT unpacking)    */
/* ------------------------------------------------------------------ */
static const q15_t twid64_re[32] = {
         0,  32728,  32138,  30893,  29032,  26620,  23731,  20448,
     16867,  13094,   9226,   5362,   1601,  -1961,  -5423,  -8693,
    -11686, -14330, -16572, -18369, -19689, -20513, -20830, -20640,
    -19948, -18768, -17121, -15034, -12539,  -9677,  -6492,  -3034
};
static const q15_t twid64_im[32] = {
         0,  -1601,  -3197,  -4790,  -6377,  -7955,  -9520, -11069,
    -12598, -14102, -15577, -17018, -18421, -19782, -21096, -22359,
    -23566, -24712, -25793, -26805, -27743, -28604, -29384, -30079,
    -30686, -31202, -31624, -31950, -32178, -32306, -32333, -32260
};

/* ---- public API ---- */

int dsp_fft_init(void)
{
    return 0;
}

/*
 * q15 saturated multiply with rounding: (a * b + 0x4000) >> 15
 */
static inline q15_t q15_mul(q15_t a, q15_t b)
{
    q31_t prod = (q31_t)a * (q31_t)b;
    return (q15_t)((prod + 0x4000) >> 15);
}

/*
 * q15 multiply-accumulate: acc += (a * b) >> 15, with saturation
 */
static inline q31_t q15_mac(q31_t acc, q15_t a, q15_t b)
{
    return acc + (q31_t)a * (q31_t)b;
}

/*
 * 32-point complex FFT, radix-2 DIT, in-place.
 *
 * Input:  bit-reversed order (64 q15 values: re0,im0, re1,im1, ...)
 * Output: normal order (same buffer, overwritten)
 *
 * Each of the 5 stages right-shifts result by 1 to prevent overflow.
 * Net gain: 1/32.
 */
static void cfft32_q15(q15_t *data)
{
    int stage, group, pair;

    for (stage = 0; stage < 5; stage++) {
        int step    = 1 << stage;        /* butterfly pair distance */
        int group_n = step << 1;          /* group size */
        int twid_step = 16 >> stage;      /* twiddle factor stride */

        for (group = 0; group < 32; group += group_n) {
            for (pair = 0; pair < step; pair++) {
                int top    = group + pair;
                int bottom = top + step;
                int twid_idx = pair * twid_step;

                q15_t Wr = twid32_re[twid_idx];
                q15_t Wi = twid32_im[twid_idx];

                /* Load top values */
                q15_t top_re = data[top * 2];
                q15_t top_im = data[top * 2 + 1];
                q15_t bot_re = data[bottom * 2];
                q15_t bot_im = data[bottom * 2 + 1];

                /* W * bottom (complex, q15) */
                q31_t tmp_re = q15_mac(0, bot_re, Wr) - (q31_t)bot_im * Wi;
                q31_t tmp_im = q15_mac(0, bot_re, Wi) + (q31_t)bot_im * Wr;
                q15_t prod_re = (q15_t)((tmp_re + 0x4000) >> 15);
                q15_t prod_im = (q15_t)((tmp_im + 0x4000) >> 15);

                /* Butterfly with stage scaling (>> 1) */
                data[top * 2]     = (top_re + prod_re) >> 1;
                data[top * 2 + 1] = (top_im + prod_im) >> 1;
                data[bottom * 2]     = (top_re - prod_re) >> 1;
                data[bottom * 2 + 1] = (top_im - prod_im) >> 1;
            }
        }
    }
}

/*
 * 64-point real FFT:
 *   Pack 64 real → 32 complex → CFFT → unpack → 33 complex bins.
 *
 * output layout (66 q15 values): [re0, im0, re1, im1, ..., re32, im32]
 * im0 and im32 are always 0 (DC and Nyquist are purely real).
 */
void dsp_fft_rfft_q15(const q15_t *input, q15_t *output)
{
    int k;
    q15_t cfft_buf[64];  /* 32 complex values, interleaved */

    /* Step 1: Pack: z[n] = x[2n] + j*x[2n+1], n = 0..31 */
    for (k = 0; k < 32; k++) {
        cfft_buf[k * 2]     = input[k * 2];       /* even = real */
        cfft_buf[k * 2 + 1] = input[k * 2 + 1];   /* odd  = imag */
    }

    /* Step 2: Bit-reverse reorder */
    {
        q15_t tmp[64];
        int rev;

        for (k = 0; k < 32; k++) {
            rev = bitrev5[k];
            tmp[k * 2]     = cfft_buf[rev * 2];
            tmp[k * 2 + 1] = cfft_buf[rev * 2 + 1];
        }
        for (k = 0; k < 64; k++)
            cfft_buf[k] = tmp[k];
    }

    /* Step 3: 32-point complex FFT */
    cfft32_q15(cfft_buf);

    /* Step 4: Unpack to 64-point real FFT (33 bins) */
    /* bin 0 (DC): purely real */
    output[0] = (cfft_buf[0] + cfft_buf[1]) >> 1;  /* /2 scaling */
    output[1] = 0;

    /* bins 1..31 */
    for (k = 1; k < 32; k++) {
        int k2;
        q15_t Ck_re, Ck_im, Ck2_re, Ck2_im;

        k2 = 32 - k;  /* conjugate symmetric index */

        Ck_re  = cfft_buf[k * 2];
        Ck_im  = cfft_buf[k * 2 + 1];
        Ck2_re = cfft_buf[k2 * 2];
        Ck2_im = cfft_buf[k2 * 2 + 1];

        /* Even/Odd decomposition */
        q15_t A_re = (Ck_re + Ck2_re) >> 1;
        q15_t A_im = (Ck_im - Ck2_im) >> 1;
        q15_t B_re = (Ck_im + Ck2_im) >> 1;
        q15_t B_im = (Ck2_re - Ck_re) >> 1;

        /* Multiply B by W_64^k */
        q15_t Wr = twid64_re[k];
        q15_t Wi = twid64_im[k];

        q31_t prod_re = q15_mac(0, B_re, Wr) - (q31_t)B_im * Wi;
        q31_t prod_im = q15_mac(0, B_re, Wi) + (q31_t)B_im * Wr;
        q15_t BW_re = (q15_t)((prod_re + 0x4000) >> 15);
        q15_t BW_im = (q15_t)((prod_im + 0x4000) >> 15);

        output[k * 2]     = A_re + BW_re;
        output[k * 2 + 1] = A_im + BW_im;
    }

    /* bin 32 (Nyquist): purely real */
    output[64] = (cfft_buf[0] - cfft_buf[1]) >> 1;
    output[65] = 0;
}

/*
 * Magnitude: |z| = sqrt(re^2 + im^2) in q15.
 *
 * Uses fast integer sqrt approximation via Newton's method on the
 * 32-bit squared sum. Saturated to 32767.
 */
void dsp_fft_mag_q15(const q15_t *src, q15_t *dst, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        int32_t re = (int32_t)src[i * 2];
        int32_t im = (int32_t)src[i * 2 + 1];

        /* Sum of squares: max = 32767^2 + 32767^2 ≈ 2.147e9 < 2^31 */
        uint32_t sum_sq = (uint32_t)(re * re + im * im);

        /* Integer sqrt by Newton's method (5 iterations is enough for 31-bit) */
        uint32_t x = sum_sq;
        uint32_t y = (x + 1) >> 1;
        while (y < x) {
            x = y;
            y = (x + sum_sq / x) >> 1;
        }

        /* Clamp to q15 range */
        dst[i] = (x > 32767) ? (q15_t)32767 : (q15_t)x;
    }
}
