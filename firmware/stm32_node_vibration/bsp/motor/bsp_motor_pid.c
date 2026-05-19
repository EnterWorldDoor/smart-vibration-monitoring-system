/**
 * @file bsp_motor_pid.c
 * @brief 增量式PID速度控制器实现
 *
 * 默认参数:
 *   Kp=2.5, Ki=0.3, Kd=0.5 — 适用于JGB37-520E (30:1减速)
 *   积分限幅=3000, 输出限幅=9999, 死区=20RPM, 积分分离阈值=500RPM
 */

#include "bsp_motor_pid.h"
#include <stdlib.h>  /* abs() */

/* 默认PID参数 */
static const struct pid_config PID_DEFAULTS = {
	.kp            = 2.5f,
	.ki            = 0.3f,
	.kd            = 0.5f,
	.integral_max  = 3000,
	.output_max    = 9999,
	.deadzone      = 20,
	.sep_threshold = 500,
};

void pid_init(struct pid_state *st, const struct pid_config *cfg)
{
	if (!st)
		return;

	if (cfg)
		st->cfg = *cfg;
	else
		st->cfg = PID_DEFAULTS;

	st->target      = 0;
	st->output      = 0;
	st->integral    = 0;
	st->prev_error  = 0;
	st->prev_error2 = 0;
	st->enabled     = false;
}

void pid_set_target(struct pid_state *st, int32_t target, int8_t direction)
{
	if (!st)
		return;

	/* target始终为正, 方向由外部控制 */
	if (target < 0)
		target = 0;

	st->target = target;
}

void pid_set_enabled(struct pid_state *st, bool en)
{
	if (!st)
		return;

	if (en && !st->enabled) {
		/* 启用时复位历史状态, 避免旧误差冲击 */
		st->integral    = 0;
		st->prev_error  = 0;
		st->prev_error2 = 0;
	}
	st->enabled = en;
}

void pid_reset(struct pid_state *st)
{
	if (!st)
		return;

	st->integral    = 0;
	st->prev_error  = 0;
	st->prev_error2 = 0;
	st->output      = 0;
	st->enabled     = false;
}

int32_t pid_step(struct pid_state *st, int32_t current_rpm)
{
	int32_t error, delta, inc;
	float p_term, i_term, d_term;

	if (!st || !st->enabled)
		return st ? st->output : 0;

	/*
	 * 误差计算: target总是正数, current_rpm带符号(方向)。
	 * 取绝对值比较, 保证正反转对称控制。
	 */
	{
		int32_t abs_rpm = current_rpm;
		if (abs_rpm < 0)
			abs_rpm = -abs_rpm;

		error = st->target - abs_rpm;
	}

	/* 死区: 小误差不调节 */
	{
		int32_t dz = st->cfg.deadzone;
		if (error >= -dz && error <= dz) {
			st->prev_error2 = st->prev_error;
			st->prev_error  = error;
			return st->output;
		}
	}

	/* 比例项: Kp * (e(k) - e(k-1)) */
	delta = error - st->prev_error;
	p_term = st->cfg.kp * (float)delta;

	/* 积分项: Ki * e(k) (带积分分离和限幅) */
	if (abs(error) <= st->cfg.sep_threshold) {
		st->integral += error;
		if (st->integral > st->cfg.integral_max)
			st->integral = st->cfg.integral_max;
		else if (st->integral < -st->cfg.integral_max)
			st->integral = -st->cfg.integral_max;
		i_term = st->cfg.ki * (float)error;
	} else {
		i_term = 0.0f;
	}

	/* 微分项: Kd * (e(k) - 2*e(k-1) + e(k-2)) */
	d_term = st->cfg.kd * (float)(error - 2 * st->prev_error + st->prev_error2);

	/* 增量合成 */
	inc = (int32_t)(p_term + i_term + d_term);

	/* 输出 = 上次输出 + 增量, 然后限幅 */
	st->output += inc;
	if (st->output < 0)
		st->output = 0;
	if (st->output > st->cfg.output_max)
		st->output = st->cfg.output_max;

	/* 输出饱和时抑制积分 (anti-windup) */
	if (st->output >= st->cfg.output_max && inc > 0)
		st->integral -= error;
	else if (st->output <= 0 && inc < 0)
		st->integral -= error;

	/* 保存误差历史 */
	st->prev_error2 = st->prev_error;
	st->prev_error  = error;

	return st->output;
}

int32_t pid_get_output(struct pid_state *st)
{
	if (!st)
		return 0;
	return st->output;
}

bool pid_is_enabled(struct pid_state *st)
{
	if (!st)
		return false;
	return st->enabled;
}
