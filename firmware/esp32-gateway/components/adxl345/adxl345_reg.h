/**
 * @file adxl345_reg.h
 * @author EnterWorldDoor 
 * @brief ADXL345 寄存器地址与位域定义（企业级完整版）
 *
 * 包含所有寄存器地址及关键寄存器的位域定义，
 * 用于 FIFO、中断、SPI CRC 等高级功能配置。
 */

#ifndef ADXL345_REG_H
#define ADXL345_REG_H

/* ==================== 寄存器地址映射 ==================== */

#define ADXL345_DEVID           0x00  /**< 设备ID (固定值 0xE5) */
#define ADXL345_THRESH_TAP      0x1D  /**< 轻拍阈值 */
#define ADXL345_OFSX            0x1E  /**< X轴偏移校准 */
#define ADXL345_OFSY            0x1F  /**< Y轴偏移校准 */
#define ADXL345_OFSZ            0x20  /**< Z轴偏移校准 */
#define ADXL345_DUR             0x21  /**< 轻拍持续时间 */
#define ADXL345_LATENT          0x22  /**< 轻拍潜伏时间 */   
#define ADXL345_WINDOW          0x23  /**< 轻拍窗口 */
#define ADXL345_THRESH_ACT      0x24  /**< 活动阈值 */
#define ADXL345_THRESH_INACT    0x25  /**< 不活动阈值 */
#define ADXL345_TIME_INACT      0x26  /**< 不活动时间 */
#define ADXL345_ACT_INACT_CTL   0x27  /**< 活动/不活动AC耦合控制 */
#define ADXL345_THRESH_FF       0x28  /**< 自由落体阈值 */
#define ADXL345_TIME_FF         0x29  /**< 自由落体时间 */
#define ADXL345_TAP_AXES        0x2A  /**< 轻拍轴选择 */
#define ADXL345_ACT_TAP_STATUS  0x2B  /**< 活动/轻拍中断源状态 */
#define ADXL345_BW_RATE         0x2C  /**< 数据速率与低功耗模式 */
#define ADXL345_POWER_CTL       0x2D  /**< 电源控制 (节能/测量/睡眠) */
#define ADXL345_INT_ENABLE      0x2E  /**< 中断使能 */
#define ADXL345_INT_MAP         0x2F  /**< 中断引脚映射 (INT1/INT2) */
#define ADXL345_INT_SOURCE      0x30  /**< 中断源识别 (读取清零) */
#define ADXL345_DATA_FORMAT     0x31  /**< 数据格式 (分辨率/量程/对齐/满量程) */
#define ADXL345_DATAX0          0x32  /**< X轴数据 LSB */
#define ADXL345_DATAX1          0x33  /**< X轴数据 MSB */
#define ADXL345_DATAY0          0x34  /**< Y轴数据 LSB */
#define ADXL345_DATAY1          0x35  /**< Y轴数据 MSB */
#define ADXL345_DATAZ0          0x36  /**< Z轴数据 LSB */
#define ADXL345_DATAZ1          0x37  /**< Z轴数据 MSB */
#define ADXL345_FIFO_CTL        0x38  /**< FIFO 控制 */
#define ADXL345_FIFO_STATUS     0x39  /**< FIFO 状态 */

/* ==================== 关键常量 ==================== */

#define ADXL345_DEVICE_ID       0xE5  /**< 正确的设备ID */
#define ADXL345_FIFO_DEPTH      32    /**< FIFO 最大深度 (32级) */
#define ADXL345_SPI_READ_CMD    0x80  /**< SPI 读命令 (多字节读时 OR 0x40) */
#define ADXL345_SPI_WRITE_CMD   0x00  /**< SPI 写命令 */
#define ADXL345_SPI_MB_BIT      0x40  /**< SPI 多字节传输标志位 */

/* ==================== DATA_FORMAT (0x31) 位域 ==================== */

#define ADXL345_DF_SELF_TEST    0x80  /**< 自检模式使能 */
#define ADXL345_DF_SPI_3WIRE    0x40  /**< 3线SPI模式 */
#define ADXL345_DF_INT_INVERT   0x20  /**< 中断极性 (0=高电平有效, 1=低电平有效) */
#define ADXL345_DF_FULL_RES     0x10  /**< 全分辨率模式 (10位固定/根据量程动态) */
#define ADXL345_DF_JUSTIFY      0x08  /**< 左对齐 (0=右对齐, 符号扩展) */
#define ADXL345_DF_RANGE_MASK   0x03  /**< 量程选择: 00=±2g, 01=±4g, 10=±8g, 11=±16g */

/* ==================== BW_RATE (0x2C) 位域 ==================== */

#define ADXL345_BW_LOW_POWER    0x10  /**< 低功耗模式使能 (降低噪声但增加误差) */
#define ADXL345_BW_RATE_MASK    0x0F  /**< 数据输出速率 (ODR) 选择 */

/* ==================== POWER_CTL (0x2D) 位域 ==================== */

#define ADXL345_PCTL_LINK       0x20  /**< 活动与不活动功能链接 */
#define ADXL345_PCTL_AUTO_SLEEP 0x10  /**< 自动睡眠使能 */
#define ADXL345_PCTL_MEASURE    0x08  /**< 测量模式使能 */
#define ADXL345_PCTL_SLEEP      0x04  /**< 睡眠模式使能 */
#define ADXL345_PCTL_WAKEUP_MASK 0x03  /**< 唤醒频率: 00=8Hz, 01=4Hz, 10=2Hz, 11=1Hz */

/* ==================== FIFO_CTL (0x38) 位域 ==================== */

#define ADXL345_FIFO_MODE_MASK  0xC0  /**< FIFO 模式: Bypass(00)/FIFO(01)/Stream(10)/Trigger(11) */
#define ADXL345_FIFO_TRIGGER    0x20  /**< 触发模式 (配合INT1) */
#define ADXL345_FIFO_SAMPLES_MASK 0x1F /**< FIFO 触发样本数 (FIFO模式下为水位点) */

/* ==================== FIFO_STATUS (0x39) 位域 ==================== */

#define ADXL345_FIFO_ENTRY_MASK 0x3F  /**< FIFO 中当前条目数 (0~32) */
#define ADXL345_FIFO_OVERRUN    0x80  /**< FIFO 溢出标志 (读取后清除) */

/* ==================== INT_ENABLE (0x2E) / INT_MAP (0x2F) / INT_SOURCE (0x30) 位域 ==================== */

#define ADXL345_INT_DATA_READY  0x80  /**< 新数据就绪 */
#define ADXL345_INT_SINGLE_TAP  0x40  /**< 单击检测 */
#define ADXL345_INT_DOUBLE_TAP  0x20  /**< 双击检测 */
#define ADXL345_INT_ACTIVITY    0x10  /**< 活动 */
#define ADXL345_INT_INACTIVITY  0x08  /**< 不活动 */
#define ADXL345_INT_FREE_FALL   0x04  /**< 自由落体 */
#define ADXL345_INT_WATERMARK   0x02  /**< FIFO 水位达到 */
#define ADXL345_INT_OVERRUN     0x01  /**< FIFO 溢出 */

#endif /* ADXL345_REG_H */