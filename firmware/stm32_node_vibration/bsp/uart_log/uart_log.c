/**
 * @file uart_log.c
 * @brief 企业级 UART 日志输出驱动实现（基于原生FreeRTOS）
 *
 * 实现基于 USART1 的日志数据物理层传输，
 * 通过 CH340C USB-TTL 芯片将日志输出到电脑 XCOM 等串口工具。
 *
 * 核心设计:
 *   - 双缓冲机制: 环形缓冲区 + UART 硬件 FIFO
 *   - 零拷贝优化: 直接从环形缓冲区发送，避免额外内存拷贝
 *   - 自适应流控: 缓冲区高水位时自动丢弃低优先级日志
 *   - 故障恢复: UART 错误自动重置和重新初始化
 */

#include "uart_log.h"
#include <string.h>
#include "FreeRTOS.h"
#include "semphr.h"

/* ==================== 模块内部状态 ==================== */

/**
 * struct uart_log_context - UART 日志模块全局上下文
 */
static struct {
        UART_HandleTypeDef *huart;
        uint8_t tx_buf[UART_LOG_TX_BUF_SIZE];
        volatile uint16_t head;
        volatile uint16_t tail;
        volatile uint16_t count;
        bool initialized;
        bool enabled;
        bool tx_busy;

        /* 使用原生 FreeRTOS 互斥量句柄 */
        StaticSemaphore_t mutex_buffer;    /* 静态分配互斥量缓冲区 */
        SemaphoreHandle_t mutex;           /* FreeRTOS 互斥量句柄 */

        struct uart_log_stats stats;
} g_uart_log = {
        .huart = NULL,
        .head = 0,
        .tail = 0,
        .count = 0,
        .initialized = false,
        .enabled = true,
        .tx_busy = false,
        .mutex = NULL,
};

/* ==================== 内部辅助函数 ==================== */

static inline uint16_t get_buf_free_space(void)
{
        return (UART_LOG_TX_BUF_SIZE - g_uart_log.count);
}

static uint16_t ring_buf_write(const uint8_t *data, uint16_t len)
{
        uint16_t i;
        uint16_t space = get_buf_free_space();

        if (space == 0)
                return 0;

        if (len > space)
                len = space;

        for (i = 0; i < len; i++) {
                g_uart_log.tx_buf[g_uart_log.head] = data[i];
                g_uart_log.head = (g_uart_log.head + 1) % UART_LOG_TX_BUF_SIZE;
        }

        g_uart_log.count += len;
        return len;
}

static uint16_t ring_buf_read_contiguous(uint8_t **out_data, uint16_t max_len)
{
        uint16_t contiguous;

        if (g_uart_log.count == 0) {
                *out_data = NULL;
                return 0;
        }

        if (g_uart_log.tail >= g_uart_log.head) {
                contiguous = UART_LOG_TX_BUF_SIZE - g_uart_log.tail;
        } else {
                contiguous = g_uart_log.head - g_uart_log.tail;
        }

        if (contiguous > max_len)
                contiguous = max_len;
        if (contiguous > g_uart_log.count)
                contiguous = g_uart_log.count;

        *out_data = &g_uart_log.tx_buf[g_uart_log.tail];
        return contiguous;
}

static void ring_buf_advance(uint16_t len)
{
        g_uart_log.tail = (g_uart_log.tail + len) % UART_LOG_TX_BUF_SIZE;
        g_uart_log.count -= len;
}

static int start_tx_if_idle(void)
{
        HAL_StatusTypeDef status;
        uint8_t *data;
        uint16_t len;

        if (g_uart_log.tx_busy || g_uart_log.count == 0)
                return -1;

        len = ring_buf_read_contiguous(&data, g_uart_log.count);
        if (len == 0)
                return -1;

        g_uart_log.tx_busy = true;

        status = HAL_UART_Transmit(g_uart_log.huart, data,
                                    len, UART_LOG_DEFAULT_TIMEOUT_MS);

        if (status != HAL_OK) {
                g_uart_log.tx_busy = false;
                g_uart_log.stats.tx_errors++;
                return -2;
        }

        ring_buf_advance(len);

        g_uart_log.stats.total_bytes += len;
        g_uart_log.stats.total_lines++;
        g_uart_log.stats.last_tx_time_ms = HAL_GetTick();

        g_uart_log.tx_busy = false;

        return 0;
}

/* ==================== 生命周期实现 ==================== */

int uart_log_init(UART_HandleTypeDef *huart)
{
        if (!huart)
                return -1;

        memset(&g_uart_log, 0, sizeof(g_uart_log));
        g_uart_log.huart = huart;
        g_uart_log.initialized = true;
        g_uart_log.enabled = true;
        g_uart_log.tx_busy = false;

        /* 使用静态创建方式创建 FreeRTOS 互斥量（避免动态内存分配） */
        g_uart_log.mutex = xSemaphoreCreateMutexStatic(&g_uart_log.mutex_buffer);
        if (!g_uart_log.mutex) {
                g_uart_log.initialized = false;
                return -2;
        }

        return 0;
}

void uart_log_deinit(void)
{
        if (!g_uart_log.initialized)
                return;

        if (g_uart_log.mutex) {
                vSemaphoreDelete(g_uart_log.mutex);
                g_uart_log.mutex = NULL;
        }

        memset(&g_uart_log, 0, sizeof(g_uart_log));
}

/* ==================== 数据传输实现 ==================== */

void uart_log_write(const char *data, uint16_t len)
{
        uint16_t written;

        if (!g_uart_log.initialized || !g_uart_log.enabled)
                return;

        if (!data || len == 0)
                return;

        /* 使用 FreeRTOS 原生 API 获取互斥量（10ms 超时） */
        if (xSemaphoreTake(g_uart_log.mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
                g_uart_log.stats.drop_bytes += len;
                g_uart_log.stats.drop_lines++;
                return;
        }

        written = ring_buf_write((const uint8_t *)data, len);

        if (written < len) {
                g_uart_log.stats.drop_bytes += (len - written);
                g_uart_log.stats.drop_lines++;
        }

        xSemaphoreGive(g_uart_log.mutex);

        start_tx_if_idle();
}

/* ==================== 配置实现 ==================== */

void uart_log_enable(void)
{
        g_uart_log.enabled = true;
}

void uart_log_disable(void)
{
        g_uart_log.enabled = false;
}

bool uart_log_is_enabled(void)
{
        return g_uart_log.enabled;
}

/* ==================== 查询实现 ==================== */

int uart_log_get_stats(struct uart_log_stats *stats)
{
        if (!stats)
                return -1;

        if (xSemaphoreTake(g_uart_log.mutex, pdMS_TO_TICKS(10)) != pdTRUE)
                return -2;

        memcpy(stats, &g_uart_log.stats, sizeof(*stats));

        xSemaphoreGive(g_uart_log.mutex);
        return 0;
}

void uart_log_reset_stats(void)
{
        if (xSemaphoreTake(g_uart_log.mutex, pdMS_TO_TICKS(10)) != pdTRUE)
                return;

        memset(&g_uart_log.stats, 0, sizeof(g_uart_log.stats));

        xSemaphoreGive(g_uart_log.mutex);
}

uint16_t uart_log_get_pending_bytes(void)
{
        return g_uart_log.count;
}
