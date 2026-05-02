/**
 * @file uart_log_example.c
 * @brief UART 日志输出模块使用示例（USB-TTL桥接）
 *
 * 本示例展示如何将 system_log 模块的日志通过 USART1
 * 输出到电脑的 XCOM 串口调试助手。
 *
 * 硬件要求:
 *   - 开发板: ATK-DMF407 (STM32F407)
 *   - USB-TTL: CH340C 芯片 (开发板自带)
 *   - 连接: USB Type-C 口连接电脑
 *   - 跳线: JP7 跳线帽连接 PB6/PB7 到 CH340
 *
 * 软件配置:
 *   - 波特率: 115200
 *   - 数据位: 8
 *   - 停止位: 1
 *   - 校验位: None
 *
 * XCOM 配置:
 *   - 选择正确的 COM 口（设备管理器中查看）
 *   - 波特率: 115200
 *   - 勾选 "十六进制显示" 可查看原始数据
 *   - 建议勾选 "自动换行" 提高可读性
 */

#include "uart_log.h"
#include "system_log/system_log.h"
#include "stm32f4xx_hal.h"
#include <stdio.h>
#include <string.h>

/* ==================== 外部变量声明 ==================== */

extern UART_HandleTypeDef huart1;

/* ==================== 初始化函数 ==================== */

/**
 * uart_log_system_init - 完整的 UART 日志系统初始化
 *
 * 在 main.c 的 USER CODE BEGIN 1 区域调用此函数。
 * 完成以下步骤:
 *   1. 初始化底层 UART 驱动
 *   2. 配置日志系统参数
 *   3. 注册 UART 输出回调
 *   4. 启用时间戳
 *
 * 使用示例:
 *   int main(void) {
 *     HAL_Init();
 *     SystemClock_Config();
 *     MX_GPIO_Init();
 *     MX_USART1_UART_Init();  // CubeMX 生成的硬件初始化
 *
 *     // 用户代码开始
 *     uart_log_system_init(); // 初始化日志输出
 *
 *     pr_info_with_tag("SYS", "System started successfully\n");
 *     // ...
 *   }
 */
void uart_log_system_init(void)
{
        struct log_config log_cfg;
        int ret;

        ret = uart_log_init(&huart1);
        if (ret != 0) {
                while (1) {
                        HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_0);
                        HAL_Delay(500);
                }
        }

        memset(&log_cfg, 0, sizeof(log_cfg));
        log_cfg.level = LOG_LEVEL_DEBUG;
        log_cfg.tag = "STM32";
        log_cfg.output = uart_log_write;
        log_cfg.enable_timestamp = true;
        log_cfg.async_mode = false;

        ret = log_init(&log_cfg);
        if (ret != ERR_OK) {
                while (1) {
                        HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_0);
                        HAL_Delay(200);
                }
        }

        pr_info_with_tag("SYS", "====================================\n");
        pr_info_with_tag("SYS", " UART Log System Initialized\n");
        pr_info_with_tag("SYS", " Hardware: CH340C (USART1)\n");
        pr_info_with_tag("SYS", " Baudrate: 115200\n");
        pr_info_with_tag("SYS", " Output: USB-TTL -> PC XCOM\n");
        pr_info_with_tag("SYS", "====================================\n\n");
}

/* ==================== 使用示例函数 ==================== */

/**
 * example_uart_log_usage - 日志使用示例
 *
 * 展示不同级别和标签的日志输出效果。
 */
void example_uart_log_usage(void)
{
        uint8_t test_data[] = {0xAA, 0x55, 0x01, 0x02, 0x03};

        pr_debug_with_tag("TEST", "Debug message (only in debug mode)\n");

        pr_info_with_tag("APP",
                         "Application running normally\n"
                         "  Firmware version: 2.0.0\n"
                         "  Build date: %s %s\n", __DATE__, __TIME__);

        pr_warn_with_tag("SENSOR",
                         "Temperature approaching limit: %.1f C\n",
                         85.5);

        pr_error_with_tag("MOTOR",
                          "Driver fault detected!\n"
                          "  Error code: 0x%02X\n"
                          "  Action: Emergency stop\n",
                          0x0F);

        pr_info_with_tag("DATA",
                         "Raw ADC data: [%02X, %02X, %02X, %02X, %02X]\n",
                         test_data[0], test_data[1], test_data[2],
                         test_data[3], test_data[4]);
}

/**
 * example_uart_log_stats - 统计信息查询示例
 */
void example_uart_log_stats(void)
{
        struct uart_log_stats stats;
        struct log_stats log_stats;

        if (uart_log_get_stats(&stats) == 0) {
                pr_info_with_tag("STATS",
                                 "\n--- UART Log Statistics ---\n"
                                 " Total bytes: %lu\n"
                                 " Total lines: %lu\n"
                                 " Drop bytes:  %lu\n"
                                 " Drop lines:  %lu\n"
                                 " TX errors:   %lu\n"
                                 " Pending:     %u bytes\n",
                                 (unsigned long)stats.total_bytes,
                                 (unsigned long)stats.total_lines,
                                 (unsigned long)stats.drop_bytes,
                                 (unsigned long)stats.drop_lines,
                                 (unsigned long)stats.tx_errors,
                                 uart_log_get_pending_bytes());
        }

        if (log_get_stats(&log_stats) == ERR_OK) {
                pr_info_with_tag("STATS",
                                 "--- System Log Statistics ---\n"
                                 " Total lines:    %lu\n"
                                 " Dropped lines:  %lu\n"
                                 " Bytes written:  %lu\n",
                                 (unsigned long)log_stats.total_lines,
                                 (unsigned long)log_stats.dropped_lines,
                                 (unsigned long)log_stats.bytes_written);
        }
}
