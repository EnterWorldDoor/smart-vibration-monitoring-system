/**
 * @file uart4_boot.c
 * @brief Bootloader 轮询 UART4 驱动实现
 *
 * 裸寄存器操作，不依赖 HAL UART 句柄。
 * 仅需 GPIO 时钟 + UART4 时钟已使能 (SystemInit 完成)。
 */

#include "uart4_boot.h"
#include "stm32f4xx_hal.h"

#define UART4_BAUD            115200
#define UART4_CLOCK            84000000  /* APB1 时钟, SystemCoreClock/2 */

/* ---- GPIO 寄存器基址 ---- */
#define GPIOC_BASE             0x40020800

/* ---- UART4 寄存器基址 (APB1) ---- */
#define UART4_BASE             0x40004C00

#define UART4_SR               (*(volatile uint32_t *)(UART4_BASE + 0x00))
#define UART4_DR               (*(volatile uint32_t *)(UART4_BASE + 0x04))
#define UART4_BRR              (*(volatile uint32_t *)(UART4_BASE + 0x08))
#define UART4_CR1              (*(volatile uint32_t *)(UART4_BASE + 0x0C))

/* UART 状态寄存器位 */
#define USART_SR_TXE           (1 << 7)
#define USART_SR_RXNE          (1 << 5)

/* UART 控制寄存器位 */
#define USART_CR1_UE           (1 << 13)
#define USART_CR1_TE           (1 << 3)
#define USART_CR1_RE           (1 << 2)

/* GPIO 寄存器偏移 */
#define GPIO_MODER_OFFSET      0x00
#define GPIO_AFRL_OFFSET       0x20

static void gpio_set_af(uint32_t port_base, uint8_t pin, uint8_t af)
{
	volatile uint32_t *moder = (volatile uint32_t *)(port_base + GPIO_MODER_OFFSET);
	volatile uint32_t *afr = (volatile uint32_t *)(port_base + GPIO_AFRL_OFFSET);
	uint32_t moder_val = *moder;
	uint32_t afr_val = *afr;

	/* 清除并设置 MODER 为 AF 模式 */
	moder_val &= ~(3 << (pin * 2));
	moder_val |= (2 << (pin * 2));
	*moder = moder_val;

	/* 设置 AFR */
	if (pin < 8) {
		afr_val &= ~(0xF << (pin * 4));
		afr_val |= (af << (pin * 4));
		*afr = afr_val;
	}
}

int uart4_boot_init(void)
{
	/* GPIO PC10 (TX), PC11 (RX) 配置为 AF8 (UART4) */
	gpio_set_af(GPIOC_BASE, 10, 8);
	gpio_set_af(GPIOC_BASE, 11, 8);

	/* 配置波特率: BRR = APB1_CLOCK / BAUD */
	UART4_BRR = UART4_CLOCK / UART4_BAUD;

	/* 使能 UART4: TX + RX + UART */
	UART4_CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;

	return 0;
}

int uart4_boot_send_byte(uint8_t byte)
{
	uint32_t timeout = 100000;

	while (!(UART4_SR & USART_SR_TXE) && --timeout)
		;
	if (timeout == 0)
		return -1;

	UART4_DR = byte;
	return 0;
}

int uart4_boot_send(const uint8_t *data, uint16_t len)
{
	for (uint16_t i = 0; i < len; i++) {
		if (uart4_boot_send_byte(data[i]) < 0)
			return -1;
	}
	return (int)len;
}

int uart4_boot_recv_byte(uint8_t *byte)
{
	if (UART4_SR & USART_SR_RXNE) {
		*byte = (uint8_t)(UART4_DR & 0xFF);
		return 1;
	}
	return 0;
}

void uart4_boot_flush_rx(void)
{
	uint8_t dummy;

	while (UART4_SR & USART_SR_RXNE)
		dummy = (uint8_t)(UART4_DR & 0xFF);
	(void)dummy;
}
