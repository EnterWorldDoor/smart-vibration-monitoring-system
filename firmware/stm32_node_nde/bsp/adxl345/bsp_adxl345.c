/*
 * bsp/adxl345/bsp_adxl345.c — ADXL345 SPI驱动实现
 *
 * INT1 (PA3) 下降沿触发EXTI3中断 → 置位 s_data_ready 标志.
 * 主循环轮询 bsp_adxl345_data_ready() → FIFO突发读取.
 */

#include "bsp_adxl345.h"
#include "main.h"
#include "spi.h"
#include "gpio.h"
#include "../../App/global_error.h"

#define BSP_LOG_ENABLE  1
#include "bsp_log.h"

/* SPI命令: R/W=0x80 | MB=0x40 | addr */
#define ADXL345_READ_CMD(reg)   ((reg) | 0xC0)
#define ADXL345_WRITE_CMD(reg)  ((reg) & 0x3F)

extern SPI_HandleTypeDef hspi1;

static volatile bool s_data_ready;

/* ---- 内部SPI操作 ---- */

static void adxl345_write_reg(uint8_t reg, uint8_t val)
{
	uint8_t tx[2] = { ADXL345_WRITE_CMD(reg), val };
	HAL_GPIO_WritePin(ADXL345_CS_GPIO_Port, ADXL345_CS_Pin,
			  GPIO_PIN_RESET);
	HAL_SPI_Transmit(&hspi1, tx, 2, 10);
	HAL_GPIO_WritePin(ADXL345_CS_GPIO_Port, ADXL345_CS_Pin,
			  GPIO_PIN_SET);
}

static uint8_t adxl345_read_reg(uint8_t reg)
{
	uint8_t tx[2] = { ADXL345_READ_CMD(reg), 0xFF };
	uint8_t rx[2] = {0};
	HAL_GPIO_WritePin(ADXL345_CS_GPIO_Port, ADXL345_CS_Pin,
			  GPIO_PIN_RESET);
	HAL_SPI_TransmitReceive(&hspi1, tx, rx, 2, 10);
	HAL_GPIO_WritePin(ADXL345_CS_GPIO_Port, ADXL345_CS_Pin,
			  GPIO_PIN_SET);
	return rx[1];
}

static void adxl345_read_multi(uint8_t reg, uint8_t *buf, uint8_t len)
{
	uint8_t tx = ADXL345_READ_CMD(reg);
	HAL_GPIO_WritePin(ADXL345_CS_GPIO_Port, ADXL345_CS_Pin,
			  GPIO_PIN_RESET);
	HAL_SPI_Transmit(&hspi1, &tx, 1, 10);
	HAL_SPI_Receive(&hspi1, buf, len, 10);
	HAL_GPIO_WritePin(ADXL345_CS_GPIO_Port, ADXL345_CS_Pin,
			  GPIO_PIN_SET);
}

/* ---- 公共接口 ---- */

int bsp_adxl345_init(void)
{
	uint8_t devid;

	HAL_GPIO_WritePin(ADXL345_CS_GPIO_Port, ADXL345_CS_Pin,
			  GPIO_PIN_SET);
	HAL_Delay(5);

	devid = adxl345_read_reg(ADXL345_REG_DEVID);
	if (devid != 0xE5) {
		pr_error("ADXL345 not found, devid=0x%02X (expected 0xE5)\n",
			 devid);
		return ERR_SENSOR_NOT_FOUND;
	}
	pr_info("ADXL345 found, devid=0xE5\n");

	/* ±16g, 全分辨率, 右对齐 */
	adxl345_write_reg(ADXL345_REG_DATA_FORMAT, 0x0B);

	/* 400Hz 输出数据率 */
	adxl345_write_reg(ADXL345_REG_BW_RATE, ADXL345_RATE_400HZ);

	/*
	 * FIFO流模式, 水印 = ADXL345_FIFO_WATERMARK (16).
	 * 0x90 = 0b10010000: Stream(1), INT1(0), samples=16(10000)
	 */
	adxl345_write_reg(ADXL345_REG_FIFO_CTL, 0x90);

	/* 使能FIFO水印中断 → INT1 */
	adxl345_write_reg(ADXL345_REG_INT_ENABLE, 0x02);
	adxl345_write_reg(ADXL345_REG_INT_MAP, 0x00);

	/* 测量模式 */
	adxl345_write_reg(ADXL345_REG_POWER_CTL, 0x08);

	s_data_ready = false;

	pr_info("ADXL345 initialized: 400Hz, ±16g, FIFO stream, wm=%d\n",
		ADXL345_FIFO_WATERMARK);
	return 0;
}

bool bsp_adxl345_self_test(void)
{
	return adxl345_read_reg(ADXL345_REG_DEVID) == 0xE5;
}

bool bsp_adxl345_data_ready(void)
{
	if (s_data_ready) {
		s_data_ready = false;
		return true;
	}
	return false;
}

int bsp_adxl345_read_fifo_burst(int16_t *buf, uint8_t max_samples)
{
	uint8_t fifo_status;
	uint8_t samples;
	uint8_t raw[ADXL345_FIFO_WATERMARK * 6];
	int i;

	if (!buf || max_samples == 0)
		return ERR_INVALID_PARAM;

	fifo_status = adxl345_read_reg(ADXL345_REG_FIFO_STATUS);
	samples = fifo_status & 0x3F;  /* bits[5:0] = entries */

	if (samples == 0)
		return 0;

	if (samples > max_samples)
		samples = max_samples;

	adxl345_read_multi(ADXL345_REG_DATAX0, raw, (uint8_t)(samples * 6));

	for (i = 0; i < samples; i++) {
		buf[i * 3]     = (int16_t)((raw[i * 6 + 1] << 8) | raw[i * 6]);
		buf[i * 3 + 1] = (int16_t)((raw[i * 6 + 3] << 8) | raw[i * 6 + 2]);
		buf[i * 3 + 2] = (int16_t)((raw[i * 6 + 5] << 8) | raw[i * 6 + 4]);
	}

	return samples;
}

/*
 * EXTI3中断回调 — PA3 (ADXL345 INT1) 下降沿.
 * 在stm32f1xx_it.c的EXTI3_IRQHandler中调用HAL_GPIO_EXTI_IRQHandler,
 * HAL内部清中断标志后调用此回调.
 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
	if (GPIO_Pin == ADXL345_INT1_Pin) {
		s_data_ready = true;
	}
}
