/**
 * @file dsp.h
 * @author EnterWorldDoor
 * @brief 数字信号处理模块: FFT 频谱分析、RMS 计算
 */

 #ifndef DSP_H
 #define DSP_H

 #include <stdint.h>
 #include "global_error.h"

 /* FFT 配置(使用基 2 长度) */
 #define DSP_FFT_MAX_SIZE               1024
 #define DSP_FFT_DEFAULT_SIZE           512
 
 /* FFT 结果结构体 */
 struct fft_result {
    float spectrum[DSP_FFT_MAX_SIZE / 2];   /* 幅值谱, 长度 = fft_size / 2 */
    float peak_freq;                        /* 峰值频率(Hz) */
    float peak_amp;                         /* 峰值幅度 */
    uint16_t fft_size;                      /* 实际 FFT 点数 */
    float sampling_rate;                    /* 采样率(Hz) */
 };

 /* RMS 结果 */
 struct rms_result {
    float value;                            /* RMS 值 */
    float mean;                             /* 均值 */
    float peak;                             /* 峰值 */    
 };
 
 /**
  * dsp_init - 初始化 DSP 模块(预分配 FFT 工作区)
  * 
  * Return: 0 on success, negative error code on failure
  */
 int dsp_init(void);

 /**
  * dsp_rms_compute - 计算一组数据的 RMS、均值和峰值
  * @input: 输入浮点数据数组
  * @len: 数据长度
  * @out: 输出结果结构体
  * 
  * Return: 0 on success, negative error code on failure
  */
 int dsp_rms_compute(const float *input, int len, struct rms_result *out);

 /**
  * dsp_fft_compute - 计算一组数据的 FFT 频谱(输入实数，输出复数谱的幅值)
  * @input: 输入时域信号数组(长度必须为 fft_size)
  * @fft_size: FFT 点数(必须为 2 的幂次方，且不超过 DSP_FFT_MAX_SIZE)
  * @sampling_rate: 采样率(Hz)，用于计算峰值频率
  * @out: 输出结果结构体指针(包含频谱、峰值频率和峰值幅度)
  * 
  * 注意: 输入数组会被破坏(内部使用, 但无需担心)
  * 
  * Return: 0 on success, negative error code on failure
  */
 int dsp_fft_compute(float *input, uint16_t fft_size, float sampling_rate, struct fft_result *out);

 /**
  * dsp_fft_compute_3axis - 从三轴样本批量计算 FFT 频谱(输入实数，输出复数谱的幅值)
  * @x_samples: X 轴输入时域信号数组(长度必须为 len)
  * @y_samples: Y 轴输入时域信号数组(长度必须为 len)
  * @z_samples: Z 轴输入时域信号数组(长度必须为 len)
  * @len: 每轴数据长度(必须为 fft_size，且为 2 的幂次方，且不超过 DSP_FFT_MAX_SIZE)
  * @sampling_rate: 采样率(Hz)，用于计算峰值频率
  * @out_x: X 轴输出结果结构体指针(包含频谱、峰值频率和峰值幅度)
  * @out_y: Y 轴输出结果结构体指针(包含频谱、峰值频率和峰值幅度)
  * @out_z: Z 轴输出结果结构体指针(包含频谱、峰值频率和峰值幅度)
  * 
  * 用于同时处理三轴传感器数据(如加速度计)，计算每轴的频谱特征
  * 
  * Return: 0 on success, negative error code on failure
  */
 int dsp_fft_compute_3axis(const float *x_samples, const float *y_samples, const float *z_samples,
    uint16_t len, float sampling_rate, struct fft_result *out_x, struct fft_result *out_y, struct fft_result *out_z);

 #endif /* DSP_H */