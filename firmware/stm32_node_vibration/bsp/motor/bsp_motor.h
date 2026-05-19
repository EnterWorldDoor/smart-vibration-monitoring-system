/**
 * @file bsp_motor.h
 * @brief 直流有刷电机 BSP 层 — 公共 API
 *
 * 硬件映射:
 *   TIM1 CH1/CH1N (PA8/PB13): H桥U相上下PWM — 直流有刷电机驱动
 *   PF10: PM1_CTRL_SD — H桥使能 (高=使能, 低=关断)
 *   PB12: TIM1_BKIN — 硬件刹车输入 (低有效)
 *
 * 安全原则:
 *   - 初始化时 CTRL_SD=0, PWM=0%, 电机绝对静止
 *   - 上电/复位后必须显式调用 bsp_motor_start() 才能转动
 *   - 紧急停止直接硬件关断, 不依赖软件响应
 *
 * PWM Mode 2 (PWM2) 原因:
 *   PD6010D 使用EL0631开集型光耦, 信号经过光耦后极性翻转。
 *   PWM2模式下 CCR=0 → 输出HIGH → 光耦后LOW → MOSFET关 → 安全。
 *   若用PWM1, CCR=0 → 输出LOW → 光耦后HIGH → MOSFET开 → 危险!
 */

#ifndef __BSP_MOTOR_H
#define __BSP_MOTOR_H

#include <stdint.h>

/* PWM 参数 (来自CubeMX TIM1配置: PSC=167, ARR=9999) */
#define BSP_MOTOR_PWM_MAX_DUTY   9999  /* 对应 100% 占空比 */

/* 电流/电压/温度额定值 */
#define BSP_MOTOR_CURRENT_MAX_MA   8000  /* 8A (驱动板额定10A, 留20%裕量) */
#define BSP_MOTOR_VOLTAGE_MIN_MV  10000  /* 10V 欠压阈值 */
#define BSP_MOTOR_TEMP_MAX_C        800  /* 80°C 过温阈值 (0.1°C单位) */

/* 电机方向 */
#define BSP_MOTOR_DIR_CW    1
#define BSP_MOTOR_DIR_CCW  -1

/* 电机状态 (与状态机对应) */
enum motor_state {
	MOTOR_STATE_OFF      = 0,
	MOTOR_STATE_INIT     = 1,
	MOTOR_STATE_IDLE     = 2,
	MOTOR_STATE_RUNNING  = 3,
	MOTOR_STATE_STOPPING = 4,
	MOTOR_STATE_FAULT    = 5,
};

/* 故障码 */
enum motor_fault {
	MOTOR_FAULT_NONE        = 0,
	MOTOR_FAULT_OVERCURRENT = 1,
	MOTOR_FAULT_OVERTEMP    = 2,
	MOTOR_FAULT_UNDERVOLT   = 3,
	MOTOR_FAULT_BRAKE_TRIG  = 4,
	MOTOR_FAULT_ENCODER_ERR = 5,
	MOTOR_FAULT_STALL       = 6,
};

/* ---- 顶层编排 API (bsp_motor.c) ---- */

int bsp_motor_init(void);           /* 初始化所有子模块: PWM + shutdown */
int bsp_motor_start(void);          /* 使能CTRL_SD → 启动PWM */
int bsp_motor_stop(void);           /* 停止PWM → 关闭CTRL_SD */
int bsp_motor_emergency_stop(void); /* 立即关断: PWM停 + CTRL_SD拉低 */
int bsp_motor_set_duty(int32_t duty); /* 设置PWM占空比 (0~9999) */
int bsp_motor_set_direction(int8_t dir); /* 设置方向 (CW/CCW) */
int bsp_motor_get_direction(void);      /* 获取当前方向 */
int32_t bsp_motor_get_duty(void);       /* 获取当前占空比 */

/* ---- PWM 控制 API (bsp_motor_pwm.c) ---- */

int  bsp_motor_pwm_init(void);
int  bsp_motor_pwm_start(void);
int  bsp_motor_pwm_stop(void);
int  bsp_motor_pwm_set_duty(int32_t duty);
int  bsp_motor_pwm_set_direction(int8_t dir);
int  bsp_motor_pwm_get_direction(void);
int32_t bsp_motor_pwm_get_duty(void);
int  bsp_motor_pwm_emergency_stop(void);

/* ---- 编码器 API (bsp_motor_encoder.c) ---- */

int  bsp_motor_encoder_init(void);
int  bsp_motor_encoder_update_speed(void);
int  bsp_motor_encoder_get_speed(int32_t *rpm);
int  bsp_motor_encoder_get_position(int64_t *count);
void bsp_motor_encoder_diag(uint32_t *out, int n);
int  bsp_motor_encoder_pulse_detect(uint32_t *max_low_us, uint32_t *max_high_us);

/* ---- ADC 数据采集 API (bsp_motor_adc.c) ---- */

int  bsp_motor_adc_init(void);
int  bsp_motor_adc_read_current(int32_t *current_ma);
int  bsp_motor_adc_read_bus_voltage(int32_t *voltage_mv);
int  bsp_motor_adc_read_temperature(int32_t *temp_decic);
int  bsp_motor_adc_calibrate_current(void);
int  bsp_motor_adc_read_current_calibrated(int32_t *current_ma);

/* ---- 停机控制 API (bsp_motor_shutdown.c) ---- */

int  bsp_motor_shutdown_init(void);
int  bsp_motor_shutdown_enable(void);
int  bsp_motor_shutdown_disable(void);
int  bsp_motor_is_shutdown_enabled(void);

/* ---- 状态查询 ---- */

int bsp_motor_get_state(enum motor_state *state);

/* ---- Phase 1 测试 (按键控制启停) ---- */

int bsp_motor_test_run(void);   /* KEY0 按键切换: 启动/停止 */
int bsp_motor_test_auto(void);  /* 自动启停测试序列 (无按键) */

#endif /* __BSP_MOTOR_H */
