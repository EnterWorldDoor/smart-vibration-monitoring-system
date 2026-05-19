/**
 * @file bsp_motor_fault.h
 * @brief 电机故障检测与保护模块
 *
 * 检测项目:
 *   - 过流 (current > 8000mA, 持续200ms确认)
 *   - 过温 (temp > 80°C, 持续500ms确认)
 *   - 欠压 (voltage < 10V, 瞬时触发)
 *   - 堵转 (duty>0 且 rpm==0, 持续2秒确认)
 *
 * 故障状态机: FAULT→WAIT(3s)→RETRY(1次)→PERMANENT
 */

#ifndef __BSP_MOTOR_FAULT_H
#define __BSP_MOTOR_FAULT_H

#include <stdint.h>
#include <stdbool.h>
#include "bsp_motor.h"

/* 故障确认/恢复参数 */

/* 故障确认时间 (ms, 避免瞬时尖峰误触发) */
#define FAULT_CONFIRM_OVERCURRENT_MS   200
#define FAULT_CONFIRM_OVERTEMP_MS      500
#define FAULT_CONFIRM_STALL_MS         2000

/* 故障恢复参数 */
#define FAULT_RECOVER_WAIT_MS          3000  /* 故障后等待冷却时间 */
#define FAULT_MAX_RETRIES              1     /* 最多尝试恢复次数 */

/**
 * bsp_motor_fault_check - 检查所有故障条件
 * @duty:         当前PWM占空比 (0=停止)
 * @rpm:          当前转速
 * @current_ma:   当前电流 (mA)
 * @bus_mv:       母线电压 (mV)
 * @temp_dc:      驱动器温度 (0.1°C)
 * @is_running:   电机是否运行中
 * @return:       故障码 (MOTOR_FAULT_NONE=无故障)
 */
int bsp_motor_fault_check(int32_t duty, int32_t rpm,
			  int32_t current_ma, int32_t bus_mv,
			  int32_t temp_dc, bool is_running);

/**
 * bsp_motor_fault_get - 获取当前故障码
 */
int bsp_motor_fault_get(void);

/**
 * bsp_motor_fault_get_name - 获取故障名称字符串
 */
const char *bsp_motor_fault_get_name(int fault);

/**
 * bsp_motor_fault_can_recover - 是否允许尝试恢复
 * @return: true=可以尝试重新启动
 */
bool bsp_motor_fault_can_recover(void);

/**
 * bsp_motor_fault_clear - 清除故障状态 (用于恢复尝试)
 */
void bsp_motor_fault_clear(void);

/**
 * bsp_motor_fault_count - 获取累计故障次数
 */
uint32_t bsp_motor_fault_count(void);

#endif /* __BSP_MOTOR_FAULT_H */
