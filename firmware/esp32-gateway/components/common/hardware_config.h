/**
 * @file hardware_config.h
 * @brief ESP32-S3 硬件引脚配置 (集中管理)
 *
 * 所有硬件外设的GPIO引脚定义集中在此文件,
 * 避免分散在各个源文件中导致配置不一致.
 *
 * 引脚分配原则:
 *   - 优先使用ESP32-S3推荐的默认SPI/I2C/UART引脚
 *   - 避免使用Flash/PSRAM占用的引脚 (6-11)
 *   - 考虑模块间的信号干扰和布线便利性
 */

#ifndef HARDWARE_CONFIG_H
#define HARDWARE_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== ADXL345 加速度传感器 (SPI接口) ==================== */
/**
 * ADXL345 SPI接线说明 (基于图三 DevKitC-1 引脚表):
 *   模块引脚    →  ESP32-S3 GPIO   功能           ESP32信号名
 *   ─────────────────────────────────────────────────────────────
 *   SCL/SCLK   →  GPIO36          SPI时钟         FSPICLK / SPI07
 *   SDA/MOSI   →  GPIO35          SPI数据输出      FSPID / SPI06
 *   SDO/MISO   →  GPIO37          SPI数据输入      FSPIQ / SPI07S
 *   CS         →  GPIO40          SPI片选(低有效)  MTDO / CLK_OUT2
 *   INT1       →  GPIO47          中断1(可选)      SPICLK_P
 *   VCC        →  3V3             电源             3.3V
 *   GND        →  GND             地               GND
 *
 * 注意:
 *   - GPIO35/36/37 为 Octal SPI 引脚, 需使用 SPI3_HOST (支持任意GPIO映射)
 *   - CS引脚必须由软件控制 (不能使用硬件CS)
 *   - INT1用于FIFO满/活动检测中断 (可选)
 */
#define ADXL345_SPI_SCLK_PIN      36    /**< SCL/SCLK - SPI时钟 (FSPICLK) */
#define ADXL345_SPI_MOSI_PIN      35    /**< SDA/MOSI - SPI数据输出 (FSPID) */
#define ADXL345_SPI_MISO_PIN      37    /**< SDO/MISO - SPI数据输入 (FSPIQ) */
#define ADXL345_SPI_CS_PIN        40    /**< CS - 片选 (低有效, MTDO) */
#define ADXL345_INT1_PIN          47    /**< INT1 - 中断1 (可选, SPICLK_P) */

/* ==================== UART1 (STM32通信) ==================== */
/**
 * UART1接线说明:
 *   STM32      →  ESP32-S3 GPIO   功能
 *   ────────────────────────────────────
 *   TX         →  GPIO17          UART1_RX (接收STM32数据)
 *   RX         →  GPIO18          UART1_TX (发送到STM32)
 *   GND        →  GND            共地
 *
 * 波特率: 115200 bps
 * 数据位: 8
 * 停止位: 1
 * 校验: None
 */
#define STM32_UART_TX_PIN         17    /**< UART1 TX → STM32 RX */
#define STM32_UART_RX_PIN         18    /**< UART1 RX ← STM32 TX */

/* ==================== LED指示灯 (可选) ==================== */
/**
 * LED指示灯 (用于状态指示):
 *   - GPIO2: 内置LED (Boot灯)
 *   - 可自定义其他GPIO连接外部LED
 */
#ifdef CONFIG_USE_STATUS_LED
#define LED_STATUS_PIN            2     /**< 状态指示LED (低电平点亮) */
#endif

/* ==================== I2C (预留) ==================== */
/**
 * I2C0总线 (预留用于扩展传感器):
 *   SDA: GPIO21
 *   SCL: GPIO22
 */
#ifdef CONFIG_ENABLE_I2C
#define I2C_SDA_PIN               21
#define I2C_SCL_PIN               22
#endif

/* ==================== WiFi天线选择 ==================== */
/**
 * ESP32-S3 WiFi天线选择:
 *   - 如果使用外部天线, 可能需要设置此选项
 */
// #define CONFIG_WIFI_ANT_GPIO       -1  /* 外部天线控制引脚 (如需要) */

#ifdef __cplusplus
}
#endif

#endif /* HARDWARE_CONFIG_H */
