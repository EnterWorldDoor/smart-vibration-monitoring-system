/*
 * bsp/bsp_log.h — 精简日志宏 (F103裸机版)
 *
 * 使用:
 *   #define BSP_LOG_ENABLE  1  // 启用日志 (0=全禁)
 *   #include "bsp_log.h"
 *   pr_info("adxl345 devid: 0x%02X\n", devid);
 *   pr_warn("CAN frame lost, seq=%d\n", seq);
 *   pr_error("ADXL345 not found\n");
 *
 * 底层通过USART1(TX=PA9)阻塞输出, 波特率115200.
 * 发布版本设 BSP_LOG_ENABLE=0 禁用全部日志.
 */

#ifndef __BSP_LOG_H
#define __BSP_LOG_H

#include <stdint.h>

#ifndef BSP_LOG_ENABLE
#define BSP_LOG_ENABLE  1
#endif

#if BSP_LOG_ENABLE

#include "usart.h"
#include <stdio.h>
#include <string.h>

static inline void _bsp_log_puts(const char *str)
{
	HAL_UART_Transmit(&huart1, (const uint8_t *)str,
			  (uint16_t)strlen(str), 100);
}

#define pr_info(fmt, ...) \
	do { \
		char _buf[128]; \
		int _n = snprintf(_buf, sizeof(_buf), "[INFO] " fmt, ##__VA_ARGS__); \
		if (_n > 0 && _n < (int)sizeof(_buf)) \
			_bsp_log_puts(_buf); \
	} while (0)

#define pr_warn(fmt, ...) \
	do { \
		char _buf[128]; \
		int _n = snprintf(_buf, sizeof(_buf), "[WARN] " fmt, ##__VA_ARGS__); \
		if (_n > 0 && _n < (int)sizeof(_buf)) \
			_bsp_log_puts(_buf); \
	} while (0)

#define pr_error(fmt, ...) \
	do { \
		char _buf[128]; \
		int _n = snprintf(_buf, sizeof(_buf), "[ERROR] " fmt, ##__VA_ARGS__); \
		if (_n > 0 && _n < (int)sizeof(_buf)) \
			_bsp_log_puts(_buf); \
	} while (0)

#else /* BSP_LOG_ENABLE == 0 */

#define pr_info(fmt, ...)  ((void)0)
#define pr_warn(fmt, ...)  ((void)0)
#define pr_error(fmt, ...) ((void)0)

#endif /* BSP_LOG_ENABLE */

#endif /* __BSP_LOG_H */
