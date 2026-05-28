/**
 * @file uart4_boot.h
 * @brief Bootloader 极简 UART4 驱动 (轮询模式, 无中断/DMA/RTOS)
 *
 * 引脚: PC10 = TX (AF8), PC11 = RX (AF8)
 * 波特率: 115200 8N1
 */

#ifndef UART4_BOOT_H
#define UART4_BOOT_H

#include <stdint.h>
#include <stddef.h>

int  uart4_boot_init(void);
int  uart4_boot_send_byte(uint8_t byte);
int  uart4_boot_send(const uint8_t *data, uint16_t len);
int  uart4_boot_recv_byte(uint8_t *byte);   /* 返回 1=收到, 0=无数据 */
void uart4_boot_flush_rx(void);

#endif /* UART4_BOOT_H */
