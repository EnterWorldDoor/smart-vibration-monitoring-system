/**
 * @file bsp_motor_encoder.c
 * @brief 电机编码器 — TIM3 正交编码器模式
 */

#include "tim.h"
#include "bsp_motor.h"
#include <stddef.h>

#define ENCODER_COUNTS_PER_REV  660  /* 11 PPR × 30:1 × 2倍频 = 660 */
#define SPEED_SAMPLE_PERIOD_MS  100

static uint32_t s_last_counter;
static int64_t  s_total_count;
static int32_t  s_current_rpm;

int bsp_motor_encoder_init(void)
{
	uint32_t cr1;

	if (htim3.Instance != TIM3)
		return -1;

	/*
	 * 关键修复: HAL_TIM_Encoder_Init 配置了 SMCR/CCMR/CCER,
	 * 但输入通道的 CC1E/CC2E 和极性需要在 Init 中正确设置。
	 * 之后调用 Start 使能 CEN。
	 */
	if (HAL_TIM_Encoder_Start(&htim3, TIM_CHANNEL_ALL) != HAL_OK)
		return -1;

	cr1 = TIM3->CR1;
	if (!(cr1 & TIM_CR1_CEN))
		return -2;

	s_last_counter = TIM3->CNT;
	s_total_count = 0;
	s_current_rpm = 0;

	return 0;
}

/*
 * 编码器脉冲检测: 在 10ms 内高速轮询 PC6/PC7, 检测是否有电平变化
 * 返回: bit0=PC6有变化, bit1=PC7有变化 (0=无脉冲, 1=有脉冲)
 * max_low/high: 检测到的高/低电平持续时间最大值 (us)
 */
int bsp_motor_encoder_pulse_detect(uint32_t *max_low_us, uint32_t *max_high_us)
{
	uint32_t low_dur = 0, high_dur = 0;
	uint32_t max_low = 0, max_high = 0;
	uint32_t start, now;
	int saw_change_pc6 = 0, saw_change_pc7 = 0;
	int prev_pc6 = -1, prev_pc7 = -1;
	int cur_pc6, cur_pc7;

	start = HAL_GetTick();
	do {
		uint32_t idr = GPIOC->IDR;
		cur_pc6 = (idr & GPIO_PIN_6) ? 1 : 0;
		cur_pc7 = (idr & GPIO_PIN_7) ? 1 : 0;

		if (prev_pc6 >= 0 && cur_pc6 != prev_pc6)
			saw_change_pc6 = 1;
		if (prev_pc7 >= 0 && cur_pc7 != prev_pc7)
			saw_change_pc7 = 1;

		prev_pc6 = cur_pc6;
		prev_pc7 = cur_pc7;

		now = HAL_GetTick();
	} while ((now - start) < 10);  /* 10ms 窗口 */

	*max_low_us = max_low;
	*max_high_us = max_high;
	return (saw_change_pc6 ? 1 : 0) | (saw_change_pc7 ? 2 : 0);
}

/*
 * 全寄存器诊断 dump
 */
void bsp_motor_encoder_diag(uint32_t *out, int n)
{
	if (n < 10)
		return;

	out[0] = TIM3->CR1;
	out[1] = TIM3->SMCR;
	out[2] = TIM3->CCMR1;
	out[3] = TIM3->CCER;
	out[4] = TIM3->PSC;
	out[5] = TIM3->ARR;
	out[6] = TIM3->CNT;
	out[7] = GPIOC->MODER;
	out[8] = (GPIOC->MODER >> 12) & 0xF;
	out[9] = GPIOC->IDR & (GPIO_PIN_6 | GPIO_PIN_7);
}

static void encoder_update_count(void)
{
	uint32_t now = TIM3->CNT;
	int32_t delta;

	if (now >= s_last_counter)
		delta = (int32_t)(now - s_last_counter);
	else
		delta = (int32_t)(now + 65536 - s_last_counter);

	if (delta > 32768)
		delta -= 65536;

	s_total_count += delta;
	s_last_counter = now;
}

int bsp_motor_encoder_update_speed(void)
{
	int32_t delta;
	static int64_t s_last_total;

	encoder_update_count();
	delta = (int32_t)(s_total_count - s_last_total);
	s_last_total = s_total_count;

	s_current_rpm = (int32_t)((int64_t)delta * 60000 /
	                          (ENCODER_COUNTS_PER_REV * SPEED_SAMPLE_PERIOD_MS));
	return 0;
}

int bsp_motor_encoder_get_speed(int32_t *rpm)
{
	if (!rpm)
		return -1;
	*rpm = s_current_rpm;
	return 0;
}

int bsp_motor_encoder_get_position(int64_t *count)
{
	if (!count)
		return -1;
	encoder_update_count();
	*count = s_total_count;
	return 0;
}
