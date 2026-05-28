/**
 * @file crc32_hw.c
 * @brief STM32F407 硬件 CRC32 实现
 *
 * STM32F4 CRC 单元使用多项式 0x4C11DB7 (Ethernet/zip)。
 * 硬件输入/出数据自动位反转 — 输出值可直接与标准 CRC32 结果比对。
 */

#include "crc32_hw.h"
#include "spi_flash_w25q128.h"
#include "stm32f4xx_hal.h"

int crc32_hw_init(void)
{
	__HAL_RCC_CRC_CLK_ENABLE();
	crc32_hw_reset();
	return 0;
}

void crc32_hw_reset(void)
{
	CRC->CR |= CRC_CR_RESET;
}

uint32_t crc32_hw_calc(const uint8_t *data, uint32_t len)
{
	CRC->CR |= CRC_CR_RESET;

	/* 按 32-bit 对齐喂入 CRC 单元 */
	uint32_t words = len / 4;
	const uint32_t *p32 = (const uint32_t *)data;

	for (uint32_t i = 0; i < words; i++)
		CRC->DR = p32[i];

	/* 处理剩余 1-3 字节 */
	uint32_t remainder = len % 4;
	if (remainder > 0) {
		uint32_t tail = 0;
		const uint8_t *p8 = data + words * 4;

		for (uint32_t i = 0; i < remainder; i++)
			tail |= ((uint32_t)p8[i]) << (i * 8);
		CRC->DR = tail;
	}

	return CRC->DR;
}

uint32_t crc32_hw_calc_region(uint32_t spi_flash_offset, uint32_t byte_len)
{
	uint8_t buf[256];

	crc32_hw_reset();

	uint32_t remaining = byte_len;
	uint32_t offset = spi_flash_offset;

	while (remaining > 0) {
		uint32_t chunk = remaining;
		if (chunk > sizeof(buf))
			chunk = sizeof(buf);

		w25q128_read(offset, buf, chunk);

		/* 按字喂入 CRC */
		uint32_t words = chunk / 4;
		uint32_t *p32 = (uint32_t *)buf;

		for (uint32_t i = 0; i < words; i++)
			CRC->DR = p32[i];

		uint32_t rem = chunk % 4;
		if (rem > 0) {
			uint32_t tail = 0;
			for (uint32_t i = 0; i < rem; i++)
				tail |= ((uint32_t)buf[words * 4 + i]) << (i * 8);
			CRC->DR = tail;
		}

		offset += chunk;
		remaining -= chunk;
	}

	return CRC->DR;
}
