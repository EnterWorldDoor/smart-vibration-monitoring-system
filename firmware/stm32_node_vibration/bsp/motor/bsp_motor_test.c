/**
 * @file bsp_motor_test.c
 * @brief Phase 1-3 验证 — KEY0 按键控制电机启停
 *
 * 消抖: 50ms按下确认 → 等待释放 → 50ms释放确认
 * 注意: test_delay_ms 使用 HAL_Delay, 在 FreeRTOS 任务中会阻塞
 *       当前任务(~1秒周期), 按键响应延迟约100-200ms 可接受
 */

#include "bsp_motor.h"
#include "gpio.h"
#include <stddef.h>

#define KEY0_PORT     GPIOE
#define KEY0_PIN      GPIO_PIN_2
#define TEST_DUTY     500       /* 5% 占空比 */
#define DEBOUNCE_MS   50

static int test_key_pressed(void)
{
	return HAL_GPIO_ReadPin(KEY0_PORT, KEY0_PIN) == GPIO_PIN_RESET;
}

static void test_delay_ms(uint32_t ms)
{
	HAL_Delay(ms);
}

/*
 * 检测一次完整的按键按下→释放 (带消抖)
 * 返回: 1=有效按键, 0=无操作
 */
static int test_key_detect(void)
{
	/* 快速返回: 按键未按下 */
	if (!test_key_pressed())
		return 0;

	/* 阶段1: 按下消抖 — 保持按下50ms才确认 */
	test_delay_ms(DEBOUNCE_MS);
	if (!test_key_pressed())
		return 0; /* 抖动/毛刺，忽略 */

	/* 阶段2: 等待释放 */
	while (test_key_pressed())
		test_delay_ms(5);

	/* 阶段3: 释放消抖 — 释放后稳定50ms */
	test_delay_ms(DEBOUNCE_MS);

	return 1;
}

/*
 * 电机测试入口 — 每1秒轮询一次
 * 返回: 0=操作执行, 1=无操作
 */
int bsp_motor_test_run(void)
{
	static int motor_running = 0;

	if (!test_key_detect())
		return 1;

	if (!motor_running) {
		bsp_motor_start();
		bsp_motor_set_duty(TEST_DUTY);
		motor_running = 1;
	} else {
		bsp_motor_stop();
		motor_running = 0;
	}

	return 0;
}

int bsp_motor_test_auto(void)
{
	bsp_motor_start();
	bsp_motor_set_duty(500);
	test_delay_ms(500);

	bsp_motor_set_duty(2000);
	test_delay_ms(2000);

	bsp_motor_set_duty(500);
	test_delay_ms(2000);

	bsp_motor_stop();
	test_delay_ms(500);

	return 0;
}
