/**
 * @file spi_flash_w25q128.c
 * @brief W25Q128 SPI Flash 驱动实现 (SPI2 阻塞模式)
 */

#include "spi_flash_w25q128.h"
#include "stm32f4xx_hal.h"

/* 外部 SPI2 句柄 (由 MX_SPI2_Init 填充) */
extern SPI_HandleTypeDef hspi2;

/* ---- 内部辅助 ---- */

static void cs_low(void)
{
	HAL_GPIO_WritePin(GPIOI, GPIO_PIN_0, GPIO_PIN_RESET);
}

static void cs_high(void)
{
	HAL_GPIO_WritePin(GPIOI, GPIO_PIN_0, GPIO_PIN_SET);
}

static uint8_t spi2_transfer_byte(uint8_t tx)
{
	uint8_t rx = 0;

	HAL_SPI_TransmitReceive(&hspi2, &tx, &rx, 1, 100);
	return rx;
}

static void spi2_write(const uint8_t *data, uint16_t len)
{
	HAL_SPI_Transmit(&hspi2, (uint8_t *)data, len, 100);
}

static void spi2_read(uint8_t *buf, uint16_t len)
{
	uint8_t dummy = 0xFF;

	HAL_SPI_TransmitReceive(&hspi2, &dummy, buf, len, 100);
}

static void w25q_write_enable(void)
{
	uint8_t cmd = W25Q_CMD_WRITE_ENABLE;

	cs_low();
	spi2_write(&cmd, 1);
	cs_high();
}

/* ---- 公共 API ---- */

int w25q128_init(void)
{
	/* SPI2 GPIO 已在 MX_GPIO_Init 中配置 */
	/* SPI2 外设已在 MX_SPI2_Init 中配置 */
	/* 仅确认 CS 引脚为高 (取消选中) */
	cs_high();

	/* 唤醒芯片 (如果处于掉电模式) */
	uint8_t cmd = W25Q_CMD_RELEASE_POWERDOWN;

	cs_low();
	spi2_write(&cmd, 1);
	cs_high();

	/* 等待就绪 */
	w25q128_wait_busy();

	return 0;
}

int w25q128_read_jedec_id(uint8_t *manufacturer, uint8_t *mem_type,
			  uint8_t *capacity)
{
	uint8_t cmd = W25Q_CMD_JEDEC_ID;
	uint8_t rx[3] = {0};

	cs_low();
	spi2_write(&cmd, 1);
	spi2_read(rx, 3);
	cs_high();

	*manufacturer = rx[0];
	*mem_type = rx[1];
	*capacity = rx[2];

	return 0;
}

int w25q128_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
	uint8_t cmd[4];

	cmd[0] = W25Q_CMD_READ_DATA;
	cmd[1] = (uint8_t)(addr >> 16);
	cmd[2] = (uint8_t)(addr >> 8);
	cmd[3] = (uint8_t)(addr);

	cs_low();
	spi2_write(cmd, 4);
	spi2_read(buf, (uint16_t)len);
	cs_high();

	return 0;
}

int w25q128_write_page(uint32_t addr, const uint8_t *buf, uint32_t len)
{
	uint8_t cmd[4];

	if (len > W25Q128_PAGE_SIZE)
		return -1;

	w25q_write_enable();

	cmd[0] = W25Q_CMD_PAGE_PROGRAM;
	cmd[1] = (uint8_t)(addr >> 16);
	cmd[2] = (uint8_t)(addr >> 8);
	cmd[3] = (uint8_t)(addr);

	cs_low();
	spi2_write(cmd, 4);
	spi2_write(buf, (uint16_t)len);
	cs_high();

	w25q128_wait_busy();

	return (int)len;
}

int w25q128_erase_sector(uint32_t addr)
{
	uint8_t cmd[4];

	w25q_write_enable();

	cmd[0] = W25Q_CMD_SECTOR_ERASE;
	cmd[1] = (uint8_t)(addr >> 16);
	cmd[2] = (uint8_t)(addr >> 8);
	cmd[3] = (uint8_t)(addr);

	cs_low();
	spi2_write(cmd, 4);
	cs_high();

	w25q128_wait_busy();

	return 0;
}

int w25q128_erase_chip(void)
{
	uint8_t cmd = W25Q_CMD_CHIP_ERASE;

	w25q_write_enable();
	cs_low();
	spi2_write(&cmd, 1);
	cs_high();
	w25q128_wait_busy();

	return 0;
}

void w25q128_wait_busy(void)
{
	uint8_t cmd = W25Q_CMD_READ_STATUS1;
	uint8_t sr1;

	do {
		cs_low();
		spi2_write(&cmd, 1);
		spi2_read(&sr1, 1);
		cs_high();
	} while (sr1 & W25Q_SR1_BUSY);
}
