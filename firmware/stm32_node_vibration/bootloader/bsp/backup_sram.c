/**
 * @file backup_sram.c
 * @brief 备份 SRAM OTA 标志读写实现
 */

#include "backup_sram.h"
#include "stm32f4xx_hal.h"

int bkpsram_init(void)
{
	/* 使能电源接口时钟 */
	__HAL_RCC_PWR_CLK_ENABLE();

	/* 使能备份域访问 */
	HAL_PWR_EnableBkUpAccess();

	/* 使能备份 SRAM 时钟 */
	__HAL_RCC_BKPSRAM_CLK_ENABLE();

	/* 使能备份调压器 (VBAT 模式下保持 SRAM 内容) */
	HAL_PWREx_EnableBkUpReg();

	return 0;
}

void bkpsram_write_ota_flag(uint32_t fw_size, uint32_t crc32)
{
	uint32_t *p = (uint32_t *)BKPSRAM_BASE;
	p[0] = BKPSRAM_MAGIC_VALUE;
	p[1] = fw_size;
	p[2] = crc32;
}

bool bkpsram_check_ota_flag(struct ota_bkpsram_data *out)
{
	uint32_t *p = (uint32_t *)BKPSRAM_BASE;

	if (p[0] != BKPSRAM_MAGIC_VALUE)
		return false;

	out->magic = p[0];
	out->firmware_size = p[1];
	out->expected_crc32 = p[2];
	return true;
}

void bkpsram_clear(void)
{
	uint32_t *p = (uint32_t *)BKPSRAM_BASE;
	p[0] = 0;
	p[1] = 0;
	p[2] = 0;
}
