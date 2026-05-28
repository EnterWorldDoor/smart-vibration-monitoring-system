/**
 * @file flash_ops.c
 * @brief STM32F407 内部 Flash 操作实现
 *
 * 注意: 擦除/编程内部 Flash 期间 CPU 会暂停 (Flash 等待状态)。
 * Bootloader 单线程无中断场景下这是安全的。
 */

#include "flash_ops.h"
#include "spi_flash_w25q128.h"
#include "stm32f4xx_hal.h"

int flash_ops_erase_app_region(void)
{
	uint32_t sector_error = 0;
	FLASH_EraseInitTypeDef erase;
	int ret = -1;

	HAL_FLASH_Unlock();
	__HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR |
			       FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
			       FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

	for (uint32_t sector = APP_FLASH_SECTOR_FIRST;
	     sector <= APP_FLASH_SECTOR_LAST; sector++) {
		erase.TypeErase = FLASH_TYPEERASE_SECTORS;
		erase.Banks = FLASH_BANK_1;
		erase.Sector = sector;
		erase.NbSectors = 1;
		erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

		if (HAL_FLASHEx_Erase(&erase, &sector_error) != HAL_OK) {
			ret = -1;
			goto out;
		}
	}

	ret = 0;

out:
	HAL_FLASH_Lock();
	return ret;
}

int flash_ops_program_word(uint32_t addr, uint32_t word)
{
	if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, word) != HAL_OK)
		return -1;
	return 0;
}

int flash_ops_program_from_spi_flash(uint32_t spi_flash_offset,
				     uint32_t byte_count,
				     uint32_t flash_target_addr)
{
	uint8_t buf[256];
	uint32_t remaining = byte_count;
	uint32_t spi_off = spi_flash_offset;
	uint32_t flash_addr = flash_target_addr;

	HAL_FLASH_Unlock();
	__HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR |
			       FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
			       FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

	while (remaining > 0) {
		uint32_t chunk = remaining;
		if (chunk > sizeof(buf))
			chunk = sizeof(buf);

		w25q128_read(spi_off, buf, chunk);

		/* 逐字编程到内部 Flash */
		uint32_t words = chunk / 4;
		for (uint32_t i = 0; i < words; i++) {
			uint32_t w = ((uint32_t)buf[i * 4]) |
				     ((uint32_t)buf[i * 4 + 1] << 8) |
				     ((uint32_t)buf[i * 4 + 2] << 16) |
				     ((uint32_t)buf[i * 4 + 3] << 24);
			if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
					      flash_addr, w) != HAL_OK) {
				HAL_FLASH_Lock();
				return -1;
			}
			flash_addr += 4;
		}

		/* 处理不足 4 字节的尾部: 用 0xFF 补齐 */
		uint32_t rem = chunk % 4;
		if (rem > 0) {
			uint32_t w = 0xFFFFFFFF;
			uint8_t *wb = (uint8_t *)&w;
			for (uint32_t i = 0; i < rem; i++)
				wb[i] = buf[words * 4 + i];
			if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
					      flash_addr, w) != HAL_OK) {
				HAL_FLASH_Lock();
				return -1;
			}
			flash_addr += 4;
		}

		spi_off += chunk;
		remaining -= chunk;
	}

	HAL_FLASH_Lock();
	return 0;
}

bool flash_ops_verify_against_spi_flash(uint32_t spi_flash_offset,
					uint32_t byte_count,
					uint32_t flash_target_addr)
{
	uint8_t buf_flash[256];
	uint8_t buf_spi[256];
	uint32_t remaining = byte_count;
	uint32_t spi_off = spi_flash_offset;
	uint32_t flash_addr = flash_target_addr;

	while (remaining > 0) {
		uint32_t chunk = remaining;
		if (chunk > sizeof(buf_flash))
			chunk = sizeof(buf_flash);

		w25q128_read(spi_off, buf_spi, chunk);

		/* 从内部 Flash 逐字节读取 */
		const uint8_t *src = (const uint8_t *)flash_addr;
		for (uint32_t i = 0; i < chunk; i++)
			buf_flash[i] = src[i];

		/* 逐字节比对 */
		for (uint32_t i = 0; i < chunk; i++) {
			if (buf_flash[i] != buf_spi[i])
				return false;
		}

		spi_off += chunk;
		flash_addr += chunk;
		remaining -= chunk;
	}

	return true;
}
