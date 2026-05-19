/**
 * @file bsp_motor_pwm.c
 * @brief TIM1 PWM 控制 — 直流有刷电机 H桥驱动 + 方向控制
 *
 * 硬件拓扑:
 *   PD6010D使用EL0631开集光耦 → 信号极性翻转:
 *     STM32 HIGH → 光耦LED亮 → 输出LOW → MOSFET关
 *     STM32 LOW  → 光耦LED灭 → 输出HIGH → MOSFET开
 *
 *   因此使用PWM Mode 2:
 *     CCR=0  → 输出始终HIGH → MOSFET始终关 (安全)
 *     CCR=max → 输出始终LOW  → MOSFET始终开 (全速)
 *
 * 方向控制 (H桥):
 *   正转: CH1(UH) PWM调速, CH2(VH)输出=0% → VL常开作回流路径
 *        电流: VBUS → UH → Motor(+) → Motor(-) → VL → GND
 *   反转: CH2(VH) PWM调速, CH1(UH)输出=0% → UL常开作回流路径
 *        电流: VBUS → VH → Motor(-) → Motor(+) → UL → GND
 *
 *   CHxN互补输出提供同步整流, IR2104S自动死区插入, 无需MCU侧软件死区
 */

#include "bsp_motor.h"
#include "tim.h"
#include <stddef.h>

static int32_t s_current_duty;   /* 当前占空比 0..9999 */
static int8_t  s_direction;      /* 1=正转, -1=反转 */

int bsp_motor_pwm_init(void)
{
	TIM_OC_InitTypeDef cfg = {0};

	cfg.OCMode        = TIM_OCMODE_PWM2;
	cfg.Pulse         = 0;
	cfg.OCPolarity    = TIM_OCPOLARITY_HIGH;
	cfg.OCNPolarity   = TIM_OCNPOLARITY_HIGH;
	cfg.OCFastMode    = TIM_OCFAST_DISABLE;
	cfg.OCIdleState   = TIM_OCIDLESTATE_RESET;
	cfg.OCNIdleState  = TIM_OCNIDLESTATE_RESET;

	if (HAL_TIM_PWM_ConfigChannel(&htim1, &cfg, TIM_CHANNEL_1) != HAL_OK)
		return -1;
	if (HAL_TIM_PWM_ConfigChannel(&htim1, &cfg, TIM_CHANNEL_2) != HAL_OK)
		return -1;
	if (HAL_TIM_PWM_ConfigChannel(&htim1, &cfg, TIM_CHANNEL_3) != HAL_OK)
		return -1;

	s_current_duty = 0;
	s_direction = BSP_MOTOR_DIR_CW;
	return 0;
}

int bsp_motor_pwm_start(void)
{
	if (HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1) != HAL_OK)
		return -1;
	if (HAL_TIMEx_PWMN_Start(&htim1, TIM_CHANNEL_1) != HAL_OK)
		return -1;
	if (HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_2) != HAL_OK)
		return -1;
	if (HAL_TIMEx_PWMN_Start(&htim1, TIM_CHANNEL_2) != HAL_OK)
		return -1;
	return 0;
}

int bsp_motor_pwm_stop(void)
{
	HAL_TIM_PWM_Stop(&htim1, TIM_CHANNEL_1);
	HAL_TIMEx_PWMN_Stop(&htim1, TIM_CHANNEL_1);
	HAL_TIM_PWM_Stop(&htim1, TIM_CHANNEL_2);
	HAL_TIMEx_PWMN_Stop(&htim1, TIM_CHANNEL_2);
	s_current_duty = 0;
	return 0;
}

int bsp_motor_pwm_emergency_stop(void)
{
	HAL_TIM_PWM_Stop(&htim1, TIM_CHANNEL_1);
	HAL_TIMEx_PWMN_Stop(&htim1, TIM_CHANNEL_1);
	HAL_TIM_PWM_Stop(&htim1, TIM_CHANNEL_2);
	HAL_TIMEx_PWMN_Stop(&htim1, TIM_CHANNEL_2);
	__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);
	__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, 0);
	s_current_duty = 0;
	return 0;
}

int bsp_motor_pwm_set_direction(int8_t dir)
{
	if (dir != BSP_MOTOR_DIR_CW && dir != BSP_MOTOR_DIR_CCW)
		return -1;

	if (dir == s_direction)
		return 0;

	/*
	 * 切换方向: 先将当前通道CCR清零, 再将目标通道设为当前占空比。
	 * 中间有极短窗口两通道CCR均为0 = H桥全关 = 无直通风险。
	 */
	int32_t saved = s_current_duty;
	__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);
	__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, 0);
	s_current_duty = 0;

	s_direction = dir;

	if (saved > 0) {
		uint32_t ch = (dir == BSP_MOTOR_DIR_CW)
			? TIM_CHANNEL_1 : TIM_CHANNEL_2;
		__HAL_TIM_SET_COMPARE(&htim1, ch, (uint32_t)saved);
		s_current_duty = saved;
	}

	return 0;
}

int bsp_motor_pwm_get_direction(void)
{
	return (int)s_direction;
}

int bsp_motor_pwm_set_duty(int32_t duty)
{
	uint32_t pulse;
	uint32_t ch;

	if (duty < 0)
		duty = 0;
	if (duty > BSP_MOTOR_PWM_MAX_DUTY)
		duty = BSP_MOTOR_PWM_MAX_DUTY;

	pulse = (uint32_t)duty;
	ch = (s_direction == BSP_MOTOR_DIR_CW)
		? TIM_CHANNEL_1 : TIM_CHANNEL_2;

	__HAL_TIM_SET_COMPARE(&htim1, ch, pulse);
	s_current_duty = (int32_t)pulse;

	return 0;
}

int32_t bsp_motor_pwm_get_duty(void)
{
	return s_current_duty;
}
