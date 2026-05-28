/**
 * @file crc32_hw.h
 * @brief STM32F407 硬件 CRC32 计算单元封装
 *
 * 使用 AHB1 总线 CRC 外设，Ethernet 多项式 (0x4C11DB7)。
 * CRC->DR 输入/出数据自动位反转以符合 zip/ethernet CRC32 标准。
 */

#ifndef CRC32_HW_H
#define CRC32_HW_H

#include <stdint.h>

int  crc32_hw_init(void);
void crc32_hw_reset(void);
uint32_t crc32_hw_calc(const uint8_t *data, uint32_t len);

/**
 * crc32_hw_calc_region - 对 SPI Flash 某段区域计算 CRC32
 * @spi_flash_offset: SPI Flash 中的起始偏移
 * @byte_len: 要计算的数据长度
 *
 * 分块读取 SPI Flash 数据并通过硬件 CRC 单元计算。
 * Return: 32-bit CRC 值 (可直接与标准 CRC32 比对)
 */
uint32_t crc32_hw_calc_region(uint32_t spi_flash_offset, uint32_t byte_len);

#endif /* CRC32_HW_H */
