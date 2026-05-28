/**
 * @file main.c
 * @brief F407 Bootloader 入口 (32KB, Sector 0-1, 无 RTOS)
 *
 * 启动流程:
 *   1. SystemInit() — HSI 16MHz, 设置 VTOR, 使能 FPU
 *   2. 检查备份 SRAM 中的 OTA 升级标志 (魔数 0x0TAF407)
 *   3. 无标志: 校验 App 栈顶地址 → 设置 MSP → 跳转 App (0x08008000)
 *   4. 有标志: 进入 OTA 模式 — 接收固件 → SPI Flash → CRC → 内部 Flash → 复位
 */

#include "bsp/backup_sram.h"
#include "bsp/spi_flash_w25q128.h"
#include "bsp/crc32_hw.h"
#include "bsp/flash_ops.h"
#include "bsp/uart4_boot.h"
#include "proto/proto_boot.h"
#include "stm32f4xx_hal.h"
#include "Modules/protocol/protocol.h"

/* ---- OTA 状态 ---- */
enum {
	OTA_MODE_IDLE,
	OTA_MODE_RECEIVING,
	OTA_MODE_VERIFYING,
	OTA_MODE_FLASHING,
	OTA_MODE_DONE,
};

static uint32_t g_fw_total_size;
static uint32_t g_fw_expected_crc;
static uint32_t g_fw_bytes_received;
static uint8_t  g_ota_state;

/* ---- 内部辅助 ---- */

static void send_ota_status(uint8_t stat, uint8_t progress, uint16_t error)
{
	uint8_t payload[3];
	uint8_t frame[32];
	int len;

	payload[0] = stat;
	payload[1] = progress;
	payload[2] = (uint8_t)(error & 0xFF);

	len = proto_build_generic_frame(frame, PROTO_CMD_OTA_STATUS,
					payload, 3, 0);
	if (len > 0)
		uart4_boot_send(frame, (uint16_t)len);
}

static void delay_ms(uint32_t ms)
{
	for (uint32_t i = 0; i < ms * 10000; i++)
		__asm__ volatile ("nop");
}

/**
 * jump_to_app - 跳转到应用固件 (0x08008000)
 *
 * 1. 校验栈顶指针在 SRAM 范围内 (0x20000000 - 0x20030000)
 * 2. 校验复位向量在 Flash 范围内
 * 3. 关中断 → 设置 MSP → 加载 PC → 跳转
 */
static void jump_to_app(void)
{
	uint32_t app_sp = *(volatile uint32_t *)APP_FLASH_START;
	uint32_t app_pc = *(volatile uint32_t *)(APP_FLASH_START + 4);

	/* 校验栈顶指针 */
	if (app_sp < 0x20000000 || app_sp > 0x20030000)
		return;

	/* 校验复位向量 */
	if (app_pc < APP_FLASH_START || app_pc > 0x08100000)
		return;

	/* 关全局中断 */
	__disable_irq();

	/* 复位所有外设到默认状态 (App 期望干净的环境) */
	/* 关闭 SysTick */
	SysTick->CTRL = 0;

	/* 设置 MSP */
	__set_MSP(app_sp);

	/* 跳转到应用 */
	((void (*)(void))app_pc)();
}

/* ---- 主函数 ---- */

int main(void)
{
	struct ota_bkpsram_data ota_flag;
	bool in_ota_mode = false;
	int ret;

	/* ===== 第1步: 最小系统初始化 ===== */
	SystemInit();        /* HSI 16MHz, FPU, VTOR=0x08000000 */
	SystemCoreClockUpdate();

	/* ===== 第2步: 检查备份 SRAM OTA 标志 ===== */
	ret = bkpsram_init();
	if (ret == 0) {
		if (bkpsram_check_ota_flag(&ota_flag))
			in_ota_mode = true;
	}

#ifdef BOOTLOADER_DEBUG
	in_ota_mode = true;
	ota_flag.firmware_size = 0;
	ota_flag.expected_crc32 = 0;
#endif

	if (!in_ota_mode) {
		/* 正常启动 — 直接跳转 App */
		jump_to_app();
		/* 跳转失败则继续执行 OTA 恢复 */
	}

	/* ===== 第3步: OTA 模式 — 初始化外设 ===== */

	/* MX_GPIO_Init 等效: 使能所有 GPIO 时钟 */
	__HAL_RCC_GPIOA_CLK_ENABLE();
	__HAL_RCC_GPIOB_CLK_ENABLE();
	__HAL_RCC_GPIOC_CLK_ENABLE();
	__HAL_RCC_GPIOD_CLK_ENABLE();
	__HAL_RCC_GPIOE_CLK_ENABLE();
	__HAL_RCC_GPIOF_CLK_ENABLE();
	__HAL_RCC_GPIOG_CLK_ENABLE();
	__HAL_RCC_GPIOH_CLK_ENABLE();
	__HAL_RCC_GPIOI_CLK_ENABLE();

	/* 使能 SPI2 和 UART4 时钟 */
	__HAL_RCC_SPI2_CLK_ENABLE();
	__HAL_RCC_UART4_CLK_ENABLE();

	uart4_boot_init();
	MX_SPI2_Init();
	w25q128_init();
	crc32_hw_init();
	proto_boot_init();

	g_ota_state = OTA_MODE_IDLE;
	g_fw_total_size = 0;
	g_fw_expected_crc = 0;
	g_fw_bytes_received = 0;

	send_ota_status(OTA_STAT_READY, 0, 0);

	/* ===== 第4步: OTA 接收主循环 ===== */
	while (1) {
		uint8_t byte;

		if (uart4_boot_recv_byte(&byte)) {
			proto_boot_feed_byte(byte);

			if (proto_boot_frame_ready()) {
				struct proto_boot_packet pkt;

				proto_boot_get_packet(&pkt);

				switch (pkt.cmd) {
				case PROTO_CMD_OTA_BEGIN:
					if (pkt.data_len >= 8) {
						g_fw_total_size = pkt.data[0] |
							((uint32_t)pkt.data[1] << 8) |
							((uint32_t)pkt.data[2] << 16) |
							((uint32_t)pkt.data[3] << 24);
						g_fw_expected_crc = pkt.data[4] |
							((uint32_t)pkt.data[5] << 8) |
							((uint32_t)pkt.data[6] << 16) |
							((uint32_t)pkt.data[7] << 24);

						/* 擦除 SPI Flash 对应区域 */
						uint32_t sectors =
							(g_fw_total_size + W25Q128_SECTOR_SIZE - 1)
							/ W25Q128_SECTOR_SIZE;
						for (uint32_t i = 0; i < sectors; i++)
							w25q128_erase_sector(i * W25Q128_SECTOR_SIZE);

						g_fw_bytes_received = 0;
						g_ota_state = OTA_MODE_RECEIVING;
						send_ota_status(OTA_STAT_RECEIVING, 0, 0);
					} else {
						send_ota_status(OTA_STAT_ERROR, 0,
								OTA_ERR_FLASH_ERASE);
					}
					break;

				case PROTO_CMD_OTA_DATA:
					if (pkt.data_len >= 4) {
						uint32_t offset = pkt.data[0] |
							((uint32_t)pkt.data[1] << 8) |
							((uint32_t)pkt.data[2] << 16) |
							((uint32_t)pkt.data[3] << 24);
						uint16_t dlen = pkt.data_len - 4;
						const uint8_t *fw = pkt.data + 4;

						/* 按页写入 SPI Flash */
						for (uint16_t i = 0; i < dlen;
						     i += W25Q128_PAGE_SIZE) {
							uint16_t chunk = dlen - i;
							if (chunk > W25Q128_PAGE_SIZE)
								chunk = W25Q128_PAGE_SIZE;
							w25q128_write_page(
								offset + i, fw + i, chunk);
						}

						g_fw_bytes_received += dlen;
						uint8_t pct = 0;
						if (g_fw_total_size > 0)
							pct = (uint8_t)(
								(uint64_t)g_fw_bytes_received
								* 100 / g_fw_total_size);
						send_ota_status(OTA_STAT_RECEIVING,
								pct, 0);
					}
					break;

				case PROTO_CMD_OTA_END:
					g_ota_state = OTA_MODE_VERIFYING;
					{
						uint32_t computed =
							crc32_hw_calc_region(
								0, g_fw_total_size);
						if (computed != g_fw_expected_crc) {
							send_ota_status(OTA_STAT_ERROR,
								0, OTA_ERR_CRC_MISMATCH);
							g_ota_state = OTA_MODE_IDLE;
							break;
						}
					}

					/* CRC 通过 — 擦除内部 Flash */
					g_ota_state = OTA_MODE_FLASHING;
					send_ota_status(OTA_STAT_WRITING, 0, 0);

					if (flash_ops_erase_app_region() != 0) {
						send_ota_status(OTA_STAT_ERROR,
							0, OTA_ERR_INTERNAL_ERASE);
						break;
					}

					/* 从 SPI Flash 拷贝到内部 Flash */
					if (flash_ops_program_from_spi_flash(
						    0, g_fw_total_size,
						    APP_FLASH_START) != 0) {
						send_ota_status(OTA_STAT_ERROR,
							0, OTA_ERR_INTERNAL_WRITE);
						break;
					}

					/* 校验 */
					if (!flash_ops_verify_against_spi_flash(
						    0, g_fw_total_size,
						    APP_FLASH_START)) {
						send_ota_status(OTA_STAT_ERROR,
							0, OTA_ERR_FLASH_READ);
						break;
					}

					/* 成功 */
					g_ota_state = OTA_MODE_DONE;
					send_ota_status(OTA_STAT_SUCCESS, 100, 0);

					/* 清除备份 SRAM 标志 */
					bkpsram_clear();

					/* 等待 UART TX 完成 */
					delay_ms(100);

					/* 复位到新固件 */
					NVIC_SystemReset();

					/* 不会执行到这里 */
					while (1)
						;
					break;

				default:
					break;
				}
			}
		}
	}

	return 0;
}

/**
 * HardFault_Handler — 捕获硬件错误
 * 在 bootloader 中没有恢复机制，直接复位。
 */
void HardFault_Handler(void)
{
	NVIC_SystemReset();
}

/**
 * 空 ISR — 所有未使用的异常向量指向这里。
 * 避免未定义的中断导致 HardFault。
 */
void Default_Handler(void)
{
	NVIC_SystemReset();
}

/* 弱别名 — 链接器会将所有未定义的 ISR 指向 Default_Handler */
void NMI_Handler(void)            __attribute__((weak, alias("Default_Handler")));
void MemManage_Handler(void)      __attribute__((weak, alias("Default_Handler")));
void BusFault_Handler(void)       __attribute__((weak, alias("Default_Handler")));
void UsageFault_Handler(void)     __attribute__((weak, alias("Default_Handler")));
void SVC_Handler(void)            __attribute__((weak, alias("Default_Handler")));
void DebugMon_Handler(void)       __attribute__((weak, alias("Default_Handler")));
void PendSV_Handler(void)         __attribute__((weak, alias("Default_Handler")));
void SysTick_Handler(void)        __attribute__((weak, alias("Default_Handler")));
