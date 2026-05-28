/**
 * @file backup_sram.h
 * @brief STM32F407 备份 SRAM (4KB @ 0x40024000) OTA 标志管理
 *
 * 备份 SRAM 位于备份域，VBAT 供电。
 * 数据在 NVIC_SystemReset() 后存活，断电后丢失。
 */

#ifndef BACKUP_SRAM_H
#define BACKUP_SRAM_H

#include <stdint.h>
#include <stdbool.h>

#define BKPSRAM_BASE            0x40024000
#define BKPSRAM_MAGIC_OFFSET    0
#define BKPSRAM_SIZE_OFFSET     4
#define BKPSRAM_CRC32_OFFSET    8
#define BKPSRAM_MAGIC_VALUE     0x0TAF407   /**< "OTA F407" */

struct ota_bkpsram_data {
	uint32_t magic;
	uint32_t firmware_size;
	uint32_t expected_crc32;
};

int  bkpsram_init(void);
void bkpsram_write_ota_flag(uint32_t fw_size, uint32_t crc32);
bool bkpsram_check_ota_flag(struct ota_bkpsram_data *out);
void bkpsram_clear(void);

#endif /* BACKUP_SRAM_H */
