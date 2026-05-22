/*
 * bsp/adxl345/bsp_adxl345.h — ADXL345 SPI驱动接口
 *
 * 硬件: ADXL345 via SPI1 (PA5=SCK, PA6=MISO, PA7=MOSI, PA4=CS)
 *       INT1 → PA3 (EXTI3, 下降沿, FIFO水印中断)
 *
 * 配置: ±16g, 400Hz ODR, FIFO流模式, 16样本水印
 */

#ifndef __BSP_ADXL345_H
#define __BSP_ADXL345_H

#include <stdint.h>
#include <stdbool.h>

/* ADXL345 寄存器地址 */
#define ADXL345_REG_DEVID       0x00
#define ADXL345_REG_BW_RATE     0x2C
#define ADXL345_REG_POWER_CTL   0x2D
#define ADXL345_REG_INT_ENABLE  0x2E
#define ADXL345_REG_INT_MAP     0x2F
#define ADXL345_REG_DATA_FORMAT 0x31
#define ADXL345_REG_DATAX0      0x32
#define ADXL345_REG_FIFO_CTL    0x38
#define ADXL345_REG_FIFO_STATUS 0x39

/* 输出数据率 */
#define ADXL345_RATE_400HZ      0x0C

/* FIFO水印采样数 (INT1触发阈值) */
#define ADXL345_FIFO_WATERMARK  16

/* 采样窗口大小 (DSP窗口 = 64样本 = 4次FIFO突发) */
#define ADXL345_WINDOW_SIZE     64

int bsp_adxl345_init(void);
bool bsp_adxl345_self_test(void);

/*
 * 检查INT1是否触发 (FIFO水印到达).
 * 由EXTI3中断回调置位, 主循环轮询.
 */
bool bsp_adxl345_data_ready(void);

/*
 * FIFO突发读取: 一次读最多max_samples组(x,y,z).
 * 返回实际读取的样本数.
 * buf布局: [x0,y0,z0, x1,y1,z1, ...], 长度 = max_samples * 3
 */
int bsp_adxl345_read_fifo_burst(int16_t *buf, uint8_t max_samples);

#endif /* __BSP_ADXL345_H */
