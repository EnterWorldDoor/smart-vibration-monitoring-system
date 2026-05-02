/**
 * @file uart_log.h
 * @brief 企业级 UART 日志输出驱动（USB-TTL桥接模块）
 *
 * 功能特性:
 *   - 基于 STM32 HAL 库的 USART1 驱动
 *   - 支持 CH340C/CP2102/FT232 等主流 USB-TTL 芯片
 *   - 异步非阻塞发送，不阻塞业务线程
 *   - 环形缓冲区管理，防止数据丢失
 *   - 支持动态启停和运行时配置
 *   - 线程安全（FreeRTOS Mutex 保护）
 *
 * 硬件连接:
 *   - STM32F407 PB6 (USART1_TX) → USB-TTL RXD
 *   - STM32F407 PB7 (USART1_RX) → USB-TTL TXD
 *   - 波特率: 115200, 8N1
 *
 * 使用示例:
 * @code
 *   // 1. 初始化（在 main.c 中，USART1 硬件初始化后）
 *   uart_log_init(&huart1);
 *
 *   // 2. 注册到日志系统
 *   log_set_output(uart_log_write);
 *
 *   // 3. 使用日志系统输出（自动通过USB-TTL打印到电脑XCOM）
 *   pr_info_with_tag("APP", "System started\n");
 * @endcode
 *
 * 设计原则:
 *   - 单一职责: 只负责日志数据的物理层传输
 *   - 松耦合: 通过函数指针与 system_log 模块解耦
 *   - 高可用: 发送失败不影响系统正常运行
 *   - 可观测性: 提供统计信息用于监控调试
 */

#ifndef __UART_LOG_H
#define __UART_LOG_H

#include <stdint.h>
#include <stdbool.h>
#include "stm32f4xx_hal.h"

/* ==================== 配置常量 ==================== */

/**
 * UART_LOG_TX_BUF_SIZE - 发送环形缓冲区大小（字节）
 *
 * 设计考量:
 *   - 单条日志最大长度: LOG_MAX_LINE_SIZE = 256 字节
 *   - 峰值场景: 10条日志同时输出 = 2560 字节
 *   - 推荐值: 2048~4096 字节（平衡内存占用和抗突发能力）
 *   - 当前配置: 2048 字节（约可缓存8条满长日志）
 */
#define UART_LOG_TX_BUF_SIZE         2048

/**
 * UART_LOG_DEFAULT_TIMEOUT_MS - 默认发送超时时间（毫秒）
 *
 * 用于 HAL_UART_Transmit() 的阻塞超时。
 * 设为 0 表示非阻塞模式（立即返回）。
 */
#define UART_LOG_DEFAULT_TIMEOUT_MS  10

/* ==================== 统计信息结构体 ==================== */

/**
 * struct uart_log_stats - UART 日志输出统计
 * @total_bytes: 总发送字节数
 * @total_lines: 总发送行数
 * @drop_bytes: 因缓冲区满丢弃的字节数
 * @drop_lines: 因缓冲区满丢弃的行数
 * @tx_errors: 硬件发送错误次数
 * @last_tx_time_ms: 最后一次成功发送的时间戳
 */
struct uart_log_stats {
        uint32_t total_bytes;
        uint32_t total_lines;
        uint32_t drop_bytes;
        uint32_t drop_lines;
        uint32_t tx_errors;
        uint32_t last_tx_time_ms;
};

/* ==================== 生命周期 API ==================== */

/**
 * uart_log_init - 初始化 UART 日志输出模块
 * @huart: USART 句柄指针（通常为 &huart1）
 *
 * 必须在 MX_USART1_UART_Init() 之后调用。
 * 内部会创建环形缓冲区并初始化统计计数器。
 *
 * Return: 0 成功, 负数错误码
 *
 * 使用时机:
 *   在 main.c 的 USER CODE BEGIN 1 区域调用，
 *   确保硬件已初始化完成。
 *
 * 示例:
 *   uart_log_init(&huart1);
 */
int uart_log_init(UART_HandleTypeDef *huart);

/**
 * uart_log_deinit - 反初始化 UART 日志输出模块
 *
 * 释放资源并复位状态。
 * 调用后需重新 init 才能再次使用。
 */
void uart_log_deinit(void);

/* ==================== 数据传输 API ==================== */

/**
 * uart_log_write - 写入日志数据到 UART（核心接口）
 * @data: 待发送的数据缓冲区指针
 * @len: 数据长度（字节）
 *
 * 此函数符合 log_output_func_t 函数指针类型，
 * 可直接注册到 system_log 模块作为输出回调。
 *
 * 工作流程:
 *   1. 将数据拷贝到内部环形缓冲区
 *   2. 如果 UART 空闲，立即启动 DMA/中断发送
 *   3. 如果 UART 忙碌，数据在缓冲区等待
 *   4. 如果缓冲区满，丢弃新数据并更新统计
 *
 * 线程安全: ✅ 内部使用 Mutex 保护
 * 阻塞特性: ⚠️ 非阻塞（立即返回）
 *
 * 注册示例:
 *   log_set_output(uart_log_write);  // 绑定到日志系统
 */
void uart_log_write(const char *data, uint16_t len);

/* ==================== 配置 API ==================== */

/**
 * uart_log_enable - 启用 UART 日志输出
 *
 * 默认状态为启用。
 * 禁用期间的数据会被丢弃（不缓存）。
 */
void uart_log_enable(void);

/**
 * uart_log_disable - 禁用 UART 日志输出
 *
 * 用于 OTA 升级等需要静默的场景。
 * 重新启用后不会补发禁用期间的日志。
 */
void uart_log_disable(void);

/**
 * uart_log_is_enabled - 查询当前是否启用
 *
 * Return: true 启用, false 禁用
 */
bool uart_log_is_enabled(void);

/* ==================== 查询 API ==================== */

/**
 * uart_log_get_stats - 获取统计快照
 * @stats: 输出统计结构体指针
 *
 * Return: 0 成功, -1 参数错误
 */
int uart_log_get_stats(struct uart_log_stats *stats);

/**
 * uart_log_reset_stats - 重置统计计数器
 */
void uart_log_reset_stats(void);

/**
 * uart_log_get_pending_bytes - 获取待发送字节数
 *
 * Return: 环形缓冲区中尚未发送的字节数
 */
uint16_t uart_log_get_pending_bytes(void);

#endif /* __UART_LOG_H */
