/**
 * @file adxl345_reg.h
 * @author EnterWorldDoor 
 * @brief ADXL345 寄存器地址定义
 */

 #ifndef ADXL345_REG_H
 #define ADXL345_REG_H

 #define ADXL345_DEVID           0x00 /* 设备ID寄存器 */
 #define ADXL345_THRESH_TAP      0x1D /* 轻拍阈值寄存器 */
 #define ADXL345_OFSX            0x1E /* X轴偏移寄存器 */
 #define ADXL345_OFSY            0x1F /* Y轴偏移寄存器 */
 #define ADXL345_OFSZ            0x20 /* Z轴偏移寄存器 */
 #define ADXL345_DUR             0x21 /* 轻拍持续时间寄存器 */
 #define ADXL345_LATENT          0x22 /* 轻拍潜伏时间寄存器 */   
 #define ADXL345_WINDOW          0x23 /* 轻拍窗口寄存器 */
 #define ADXL345_THRESH_ACT      0x24 /* 活动阈值寄存器 */
 #define ADXL345_THRESH_INACT    0x25 /* 不活动阈值寄存器 */
 #define ADXL345_TIME_INACT      0x26 /* 不活动时间寄存器 */
 #define ADXL345_ACT_INACT_CTL   0x27 /* 活动/不活动控制寄存器 */
 #define ADXL345_THRESH_FF       0x28 /* 自由落体阈值寄存器 */
 #define ADXL345_TIME_FF         0x29 /* 自由落体时间寄存器 */
 #define ADXL345_TAP_AXES        0x2A /* 轻拍轴控制寄存器 */
 #define ADXL345_ACT_TAP_STATUS  0x2B /* 活动/轻拍状态寄存器 */
 #define ADXL345_BW_RATE         0x2C /* 数据速率和功耗模式寄存器 */
 #define ADXL345_POWER_CTL       0x2D /* 电源控制寄存器 */
 #define ADXL345_INT_ENABLE      0x2E /* 中断使能寄存器 */
 #define ADXL345_INT_MAP         0x2F /* 中断映射寄存器 */
 #define ADXL345_INT_SOURCE      0x30 /* 中断源寄存器 */
 #define ADXL345_DATA_FORMAT     0x31 /* 数据格式寄存器 */
 #define ADXL345_DATAX0          0x32 /* X轴数据寄存器0 */
 #define ADXL345_DATAX1          0x33 /* X轴数据寄存器1 */
 #define ADXL345_DATAY0          0x34 /* Y轴数据寄存器0 */
 #define ADXL345_DATAY1          0x35 /* Y轴数据寄存器1 */
 #define ADXL345_DATAZ0          0x36 /* Z轴数据寄存器0 */
 #define ADXL345_DATAZ1          0x37 /* Z轴数据寄存器1 */
 #define ADXL345_FIFO_CTL        0x38 /* FIFO控制寄存器 */
 #define ADXL345_FIFO_STATUS     0x39 /* FIFO状态寄存器 */

 #endif /* ADXL345_REG_H */