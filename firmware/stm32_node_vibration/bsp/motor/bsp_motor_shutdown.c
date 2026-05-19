/**
 * @file bsp_motor_shutdown.c
 * @brief 电机停机控制 — CTRL_SD (PF10)
 *
 * PD6010D 停机链:
 *   STM32 PF10(=CTRL_SD) → PD6010D CN4 → 光耦 → SD_IN → IR2104S
 *
 *   CTRL_SD=HIGH → 光耦LED亮 → 光耦导通 → SD_IN=1 → 半桥IC使能 → H桥可输出
 *   CTRL_SD=LOW  → 光耦LED灭 → 光耦关断 → SD_IN=0 → 半桥IC关断 → H桥关死
 *
 *   上电默认: PF10 输出 LOW (H桥物理关断, 最安全状态)
 *   必须显式调用 bsp_motor_shutdown_enable() 才能使能H桥
 *
 * PD6010D 过流自锁:
 *   驱动板板载过流保护(10A迟滞比较器) → OC_OUT拉低 → 自锁电路锁定
 *   → SD_IN强制为0 → H桥关断 (无论CTRL_SD状态)
 *   这是硬件级保护, STM32无需软件参与
 */

#include "bsp_motor.h"
#include "gpio.h"
#include <stddef.h>

/* CTRL_SD 引脚: PM1_CTRL_SD */
#define CTRL_SD_GPIO_PORT  GPIOF
#define CTRL_SD_GPIO_PIN   GPIO_PIN_10

static int s_enabled; /* 0=关断, 1=使能 */

int bsp_motor_shutdown_init(void)
{
	GPIO_InitTypeDef cfg = {0};

	__HAL_RCC_GPIOF_CLK_ENABLE();

	cfg.Pin   = CTRL_SD_GPIO_PIN;
	cfg.Mode  = GPIO_MODE_OUTPUT_PP;
	cfg.Pull  = GPIO_PULLDOWN;  /* 默认下拉 → MCU复位时物理安全 */
	cfg.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(CTRL_SD_GPIO_PORT, &cfg);

	/* 确保初始状态为关断 */
	HAL_GPIO_WritePin(CTRL_SD_GPIO_PORT, CTRL_SD_GPIO_PIN, GPIO_PIN_RESET);
	s_enabled = 0;

	return 0;
}

int bsp_motor_shutdown_enable(void)
{
	HAL_GPIO_WritePin(CTRL_SD_GPIO_PORT, CTRL_SD_GPIO_PIN, GPIO_PIN_SET);
	s_enabled = 1;
	return 0;
}

int bsp_motor_shutdown_disable(void)
{
	HAL_GPIO_WritePin(CTRL_SD_GPIO_PORT, CTRL_SD_GPIO_PIN, GPIO_PIN_RESET);
	s_enabled = 0;
	return 0;
}

int bsp_motor_is_shutdown_enabled(void)
{
	return s_enabled;
}
