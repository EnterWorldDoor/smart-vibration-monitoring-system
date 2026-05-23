/**
 * @file alarm_service.h
 * @brief 本地声光报警服务 — 4路隔离输出 + 蜂鸣器 + 板载LED
 *
 * 隔离输出端子 (DMF407 CN25, 光耦 LTV-247 共阴极):
 *   OUT1 (PB4): 绿灯 AD16-22DS — 正常运行
 *   OUT2 (PB5): 黄灯 AD16-22DS — 预警 WARNING
 *   OUT3 (PF1): 红灯 AD16-22DS — 故障 CRITICAL
 *   OUT4 (PC13): 保留 — 工业继电器
 *
 * 板载:
 *   PF0: 有源蜂鸣器 (HIGH=响)
 *   PE0: DS0 板载红灯 (LOW=亮)
 *   PE1: DS1 板载绿灯 (LOW=亮)
 *
 * 驱动逻辑:
 *   光耦 LTV-247 共阴极接法:
 *     MCU推挽HIGH → 光耦导通 → 输出端LOW(灌电流)
 *     4.7K上拉默认输出HIGH(灯灭)
 *     → HIGH = 灯亮, LOW = 灯灭
 */

#ifndef __ALARM_SERVICE_H
#define __ALARM_SERVICE_H

#include <stdint.h>
#include <stdbool.h>
#include "digital_io/digital_io.h"

/* ==================== 输出通道定义 ==================== */

#define ALARM_OUT1_GREEN      0   /* PB4 — 正常运行 */
#define ALARM_OUT2_YELLOW     1   /* PB5 — 预警 */
#define ALARM_OUT3_RED        2   /* PF1 — 故障 */
#define ALARM_OUT4_RELAY      3   /* PC13 — 工业继电器 */

/* ==================== 蜂鸣器模式 ==================== */

typedef enum {
        BUZZER_OFF        = 0,  /* 停止 */
        BUZZER_CONTINUOUS = 1,  /* 连续鸣响 (EMERGENCY/CRITICAL) */
        BUZZER_INTERMITTENT = 2, /* 间歇鸣响 1s (WARNING) */
} buzzer_mode_t;

/* ==================== 公共 API ==================== */

/**
 * alarm_service_init - 初始化声光报警服务
 *
 * 初始化所有输出 GPIO (已在 MX_GPIO_Init 中配置, 这里仅设初始状态).
 * 蜂鸣器 PF0 初始 LOW=停止.
 * LED 初始状态取决于系统安全状态 (由 digital_io 提供).
 *
 * Return: 0=成功, 负数=错误码
 */
int alarm_service_init(void);

/**
 * alarm_service_update_health - 更新健康等级
 * @level: 新健康等级 (来自 AI/规则引擎)
 *
 * 由 AI 分析结果或 ISO 10816 规则引擎调用.
 * 内部调用 iso_set_health_level() 同步 digital_io 状态.
 */
void alarm_service_update_health(health_level_t level);

/**
 * alarm_service_refresh - 读取系统状态 → 查状态矩阵 → 刷新输出
 *
 * 由 app_enterprise 主循环周期调用 (1s).
 *
 * 状态矩阵 (优先级: 安全状态 > 健康等级 > 运行模式):
 *   EMERGENCY         → 红灯常亮(OUT3), 蜂鸣连续
 *   WAIT_RESET        → 黄灯闪烁(OUT2), 蜂鸣停止
 *   CRITICAL + AUTO   → 红灯常亮(OUT3), 蜂鸣连续
 *   CRITICAL + MANUAL → 红灯常亮(OUT3), 蜂鸣静音
 *   WARNING + AUTO    → 黄灯常亮(OUT2), 蜂鸣间歇1s
 *   WARNING + MANUAL  → 黄灯常亮(OUT2), 蜂鸣静音
 *   NORMAL            → 绿灯常亮(OUT1), 蜂鸣停止
 */
void alarm_service_refresh(void);

/**
 * alarm_emergency_stop - 紧急停机 (ISR安全)
 *
 * 直接从 ISR 调用:
 *   1. 关闭 TIM1 所有 PWM 通道
 *   2. 红灯常亮 + 蜂鸣连续
 *
 * 注意: 此函数极短, 仅操作 GPIO 和定时器寄存器.
 * 不调用日志、不阻塞、不分配内存.
 */
void alarm_emergency_stop(void);

/**
 * alarm_set_buzzer - 直接设置蜂鸣器模式 (内部使用)
 * @mode: 蜂鸣器模式
 */
void alarm_set_buzzer(buzzer_mode_t mode);

/**
 * alarm_set_output - 直接设置隔离输出通道 (内部使用)
 * @ch:   通道 0..3
 * @on:   true=灯亮, false=灯灭
 */
void alarm_set_output(uint8_t ch, bool on);

#endif /* __ALARM_SERVICE_H */
