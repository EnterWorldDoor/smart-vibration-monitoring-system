/**
 * @file spi_flash_w25q128.h
 * @brief W25Q128 16MB SPI NOR Flash 驱动 (SPI2, 阻塞模式)
 *
 * 引脚:
 *   PC2 = MISO (AF5), PC3 = MOSI (AF5)
 *   PI1 = SCK (AF5),  PI0 = CS (GPIO 软件控制)
 *
 * DMA 不可用: DMA1_Stream4 被 UART4 TX 抢占, 全程阻塞 HAL_SPI 操作。
 */

#ifndef W25Q128_H
#define W25Q128_H

#include <stdint.h>
#include <stddef.h>

/* 命令码 */
#define W25Q_CMD_WRITE_ENABLE       0x06
#define W25Q_CMD_WRITE_DISABLE      0x04
#define W25Q_CMD_READ_STATUS1       0x05
#define W25Q_CMD_READ_STATUS2       0x35
#define W25Q_CMD_READ_DATA          0x03
#define W25Q_CMD_FAST_READ          0x0B
#define W25Q_CMD_PAGE_PROGRAM       0x02
#define W25Q_CMD_SECTOR_ERASE       0x20	/* 4KB */
#define W25Q_CMD_BLOCK_ERASE_32K    0x52
#define W25Q_CMD_BLOCK_ERASE_64K    0xD8
#define W25Q_CMD_CHIP_ERASE         0xC7
#define W25Q_CMD_JEDEC_ID           0x9F
#define W25Q_CMD_POWER_DOWN         0xB9
#define W25Q_CMD_RELEASE_POWERDOWN  0xAB

/* 状态寄存器位 */
#define W25Q_SR1_BUSY               (1 << 0)
#define W25Q_SR1_WEL                (1 << 1)

/* 几何参数 */
#define W25Q128_SECTOR_SIZE         4096
#define W25Q128_PAGE_SIZE           256
#define W25Q128_TOTAL_SIZE          (16 * 1024 * 1024)

int  w25q128_init(void);
int  w25q128_read_jedec_id(uint8_t *manufacturer, uint8_t *mem_type,
			   uint8_t *capacity);
int  w25q128_read(uint32_t addr, uint8_t *buf, uint32_t len);
int  w25q128_write_page(uint32_t addr, const uint8_t *buf, uint32_t len);
int  w25q128_erase_sector(uint32_t addr);
int  w25q128_erase_chip(void);
void w25q128_wait_busy(void);

#endif /* W25Q128_H */
