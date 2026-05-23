/**
 * @file bsp_motor.c
 * @brief 顶层BSP编排 — 调用子模块并按正确顺序编排
 *
 * 模块分层:
 *   bsp_motor.c (编排层)
 *     ├── bsp_motor_pwm.c      (TIM1 PWM控制 + 方向)
 *     ├── bsp_motor_shutdown.c (CTRL_SD停机控制)
 *     ├── bsp_motor_encoder.c  (TIM3编码器)
 *     └── bsp_motor_adc.c      (ADC电流/电压/温度)
 *
 * 上电安全顺序:
 *   init: PWM config → CTRL_SD硬件置低 → ADC → 编码器
 *   start: 启动PWM(CH1+CH2) → 使能H桥
 *   stop:  关H桥 → 停PWM
 */

#include "bsp_motor.h"

extern int bsp_motor_pwm_init(void);
extern int bsp_motor_pwm_start(void);
extern int bsp_motor_pwm_stop(void);
extern int bsp_motor_pwm_set_duty(int32_t duty);
extern int bsp_motor_pwm_set_direction(int8_t dir);
extern int bsp_motor_pwm_get_direction(void);
extern int32_t bsp_motor_pwm_get_duty(void);
extern int bsp_motor_pwm_emergency_stop(void);

extern int bsp_motor_adc_init(void);
extern int bsp_motor_adc_calibrate_current(void);
extern int bsp_motor_adc_read_current(int32_t *current_ma);
extern int bsp_motor_adc_read_bus_voltage(int32_t *voltage_mv);
extern int bsp_motor_adc_read_temperature(int32_t *temp_decic);

extern int bsp_motor_encoder_init(void);
extern int bsp_motor_encoder_update_speed(void);

extern int bsp_motor_shutdown_init(void);
extern int bsp_motor_shutdown_enable(void);
extern int bsp_motor_shutdown_disable(void);
extern int bsp_motor_is_shutdown_enabled(void);

static enum motor_state s_motor_state = MOTOR_STATE_OFF;
static int8_t s_direction = BSP_MOTOR_DIR_CW;

int bsp_motor_init(void)
{
	int ret;

	s_motor_state = MOTOR_STATE_INIT;

	ret = bsp_motor_pwm_init();
	if (ret != 0)
		return ret;

	ret = bsp_motor_shutdown_init();
	if (ret != 0)
		return ret;

	ret = bsp_motor_adc_init();
	if (ret != 0)
		return ret;

	ret = bsp_motor_adc_calibrate_current();
	if (ret != 0)
		return ret;

	ret = bsp_motor_encoder_init();
	if (ret != 0)
		return ret;

	s_motor_state = MOTOR_STATE_IDLE;
	return 0;
}

int bsp_motor_start(void)
{
	if (s_motor_state != MOTOR_STATE_IDLE)
		return -1;

	bsp_motor_pwm_start();
	bsp_motor_shutdown_enable();

	s_motor_state = MOTOR_STATE_RUNNING;
	return 0;
}

int bsp_motor_stop(void)
{
	bsp_motor_shutdown_disable();
	bsp_motor_pwm_stop();

	s_motor_state = MOTOR_STATE_IDLE;
	return 0;
}

int bsp_motor_emergency_stop(void)
{
	bsp_motor_pwm_emergency_stop();
	bsp_motor_shutdown_disable();

	s_motor_state = MOTOR_STATE_FAULT;
	return 0;
}

int bsp_motor_set_duty(int32_t duty)
{
	if (s_motor_state != MOTOR_STATE_RUNNING &&
	    s_motor_state != MOTOR_STATE_STOPPING)
		return -1;

	if (duty < 0)
		duty = 0;
	if (duty > BSP_MOTOR_PWM_MAX_DUTY)
		duty = BSP_MOTOR_PWM_MAX_DUTY;

	return bsp_motor_pwm_set_duty(duty);
}

int bsp_motor_set_direction(int8_t dir)
{
	if (dir != BSP_MOTOR_DIR_CW && dir != BSP_MOTOR_DIR_CCW)
		return -1;
	if (dir == s_direction)
		return 0;

	int ret = bsp_motor_pwm_set_direction(dir);
	if (ret == 0)
		s_direction = dir;
	return ret;
}

int bsp_motor_get_direction(void)
{
	return (int)s_direction;
}

int32_t bsp_motor_get_duty(void)
{
	return bsp_motor_pwm_get_duty();
}

int bsp_motor_get_state(enum motor_state *state)
{
	if (!state)
		return -1;
	*state = s_motor_state;
	return 0;
}
