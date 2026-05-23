/**
 * @file bsp_motor_pid.h
 * @brief 直流电机增量式PID速度控制器
 *
 * 算法: 增量式PID (位置式输出 = 上次输出 + 增量)
 *   u(k) = u(k-1) + Kp*(e(k)-e(k-1)) + Ki*e(k) + Kd*(e(k)-2*e(k-1)+e(k-2))
 *
 * 特性:
 *   - 积分分离 (大误差时关闭积分, 防止超调)
 *   - 输出限幅 (0 ~ output_max)
 *   - 积分限幅 (防止积分饱和/windup)
 *   - 死区 (小误差不调节, 减少PWM抖动)
 *   - 方向感知 (正转/反转分别控制)
 */

#ifndef __BSP_MOTOR_PID_H
#define __BSP_MOTOR_PID_H

#include <stdint.h>
#include <stdbool.h>

struct pid_config {
	float   kp;           /* 比例增益 */
	float   ki;           /* 积分增益 */
	float   kd;           /* 微分增益 */
	int32_t integral_max; /* 积分项上限 */
	int32_t output_max;   /* 输出上限 (BSP_MOTOR_PWM_MAX_DUTY) */
	int32_t deadzone;     /* 死区: |error|<此值不调节 */
	int32_t sep_threshold;/* 积分分离阈值: |error|>此值关闭积分 */
};

struct pid_state {
	struct pid_config cfg;
	int32_t target;       /* 目标RPM */
	int32_t output;       /* 当前输出 (PWM duty) */
	int32_t integral;     /* 累计积分 */
	int32_t prev_error;   /* e(k-1) */
	int32_t prev_error2;  /* e(k-2) */
	bool    enabled;
};

/**
 * pid_init - 初始化PID控制器
 * @st:   状态结构体 (静态分配)
 * @cfg:  配置参数 (NULL=使用默认值)
 */
void pid_init(struct pid_state *st, const struct pid_config *cfg);

/**
 * pid_set_target - 设置目标转速
 * @st:       状态结构体
 * @target:   目标RPM (绝对值, 正数)
 * @direction: 1=正转, -1=反转
 */
void pid_set_target(struct pid_state *st, int32_t target, int8_t direction);

/**
 * pid_step - 单步PID计算 (每50ms调用一次)
 * @st:         状态结构体
 * @current_rpm: 当前实际转速 (带符号, 正=正转, 负=反转)
 * @return:      新的PWM占空比 (0 ~ output_max)
 *
 * 调用者负责将返回值写入 bsp_motor_pwm_set_duty()。
 * PID禁用时返回上次输出值(不变)。
 */
int32_t pid_step(struct pid_state *st, int32_t current_rpm);

/**
 * pid_set_enabled - 启用/禁用PID
 */
void pid_set_enabled(struct pid_state *st, bool en);

/**
 * pid_reset - 复位PID状态 (清除积分和误差历史)
 */
void pid_reset(struct pid_state *st);

/**
 * pid_get_output - 获取当前输出
 */
int32_t pid_get_output(struct pid_state *st);

/**
 * pid_is_enabled - 查询PID是否启用
 */
bool pid_is_enabled(struct pid_state *st);

#endif /* __BSP_MOTOR_PID_H */
