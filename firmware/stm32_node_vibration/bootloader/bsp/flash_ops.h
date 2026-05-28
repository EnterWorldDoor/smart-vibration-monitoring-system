/**
 * @file flash_ops.h
 * @brief STM32F407 内部 Flash 操作 (擦除/编程/校验)
 *
 * App 区域: Sector 2-11 (0x08008000 - 0x080FFFFF, 960KB)
 * Bootloader 所在 Sector 0-1 永远不会被擦除。
 */

#ifndef FLASH_OPS_H
#define FLASH_OPS_H

#include <stdint.h>
#include <stdbool.h>

#define APP_FLASH_START     0x08008000
#define APP_FLASH_SECTOR_FIRST  2
#define APP_FLASH_SECTOR_LAST   11

int  flash_ops_erase_app_region(void);
int  flash_ops_program_word(uint32_t addr, uint32_t word);
int  flash_ops_program_from_spi_flash(uint32_t spi_flash_offset,
				      uint32_t byte_count,
				      uint32_t flash_target_addr);
bool flash_ops_verify_against_spi_flash(uint32_t spi_flash_offset,
					uint32_t byte_count,
					uint32_t flash_target_addr);

#endif /* FLASH_OPS_H */
