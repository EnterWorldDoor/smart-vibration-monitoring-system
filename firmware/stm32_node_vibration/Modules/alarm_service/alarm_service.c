/**
 * @file alarm_service.c
 * @brief 本地声光报警服务实现
 *
 * LED 输出矩阵 (优先级: 安全状态 > 健康等级 > 运行模式):
 *
 * | 安全状态   | 健康等级  | 运行模式 | 电机行为   | LED 输出      | 蜂鸣器      |
 * |-----------|----------|---------|-----------|---------------|------------|
 * | EMERGENCY | 任意     | 任意    | 强制停机   | 红灯常亮(OUT3)  | 连续       |
 * | WAIT_RESET| 任意     | 任意    | 强制停机   | 黄灯闪烁(OUT2)  | 停止       |
 * | NORMAL    | CRITICAL | AUTO    | AI决定停机 | 红灯常亮(OUT3)  | 连续       |
 * | NORMAL    | CRITICAL | MANUAL  | 保持当前   | 红灯常亮(OUT3)  | 静音       |
 * | NORMAL    | WARNING  | AUTO    | AI决定    | 黄灯常亮(OUT2)  | 1s间歇    |
 * | NORMAL    | WARNING  | MANUAL  | 保持当前   | 黄灯常亮(OUT2)  | 静音       |
 * | NORMAL    | NORMAL   | AUTO    | AI控制    | 绿灯常亮(OUT1)  | 停止       |
 * | NORMAL    | NORMAL   | MANUAL  | 保持当前   | 绿灯常亮(OUT1)  | 停止       |
 *
 * 闪烁实现: alarm_service 维护内部闪烁计数器,
 * 在 refresh() 中每 1s 翻转一次闪烁位.
 */

#include "alarm_service.h"
#include "main.h"
#include "tim.h"
#include "system_log/system_log.h"
#include "global_error/global_error.h"
#include <string.h>

/* ==================== GPIO 输出引脚映射 ==================== */

/*
 * 隔离输出 (光耦 LTV-247 共阴极, HIGH=导通=灯亮)
 */
#define OUT_CH_COUNT         4

static const struct {
        GPIO_TypeDef *port;
        uint16_t      pin;
} s_out_pins[OUT_CH_COUNT] = {
        [ALARM_OUT1_GREEN]  = { GPIOB, GPIO_PIN_4  },
        [ALARM_OUT2_YELLOW] = { GPIOB, GPIO_PIN_5  },
        [ALARM_OUT3_RED]    = { GPIOF, GPIO_PIN_1  },
        [ALARM_OUT4_RELAY]  = { GPIOC, GPIO_PIN_13 },
};

/* 有源蜂鸣器 */
#define BUZZER_PORT   GPIOF
#define BUZZER_PIN    GPIO_PIN_0

/* 板载LED (LOW=亮) */
#define DS0_PORT      GPIOE
#define DS0_PIN       GPIO_PIN_0
#define DS1_PORT      GPIOE
#define DS1_PIN       GPIO_PIN_1

/* ==================== 模块内部状态 ==================== */

static struct {
        bool               initialized;
        bool               out_state[OUT_CH_COUNT];  /* 当前输出状态 */
        buzzer_mode_t      buzzer_mode;
        uint8_t            blink_tick;               /* 闪烁计数器 (0-1) */
        bool               blink_phase;              /* 闪烁相位 */
} s_alarm;

/* ==================== 输出控制 ==================== */

void alarm_set_output(uint8_t ch, bool on)
{
        if (ch >= OUT_CH_COUNT)
                return;

        s_alarm.out_state[ch] = on;

        /*
         * 光耦 LTV-247 共阴极接法:
         * MCU推挽HIGH → 光耦导通 → LED灯亮
         */
        HAL_GPIO_WritePin(s_out_pins[ch].port,
                          s_out_pins[ch].pin,
                          on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void alarm_set_buzzer(buzzer_mode_t mode)
{
        s_alarm.buzzer_mode = mode;

        switch (mode) {
        case BUZZER_OFF:
                HAL_GPIO_WritePin(BUZZER_PORT, BUZZER_PIN, GPIO_PIN_RESET);
                break;
        case BUZZER_CONTINUOUS:
                HAL_GPIO_WritePin(BUZZER_PORT, BUZZER_PIN, GPIO_PIN_SET);
                break;
        case BUZZER_INTERMITTENT:
                /* 间歇模式: 由 refresh() 控制翻转, 先关闭 */
                HAL_GPIO_WritePin(BUZZER_PORT, BUZZER_PIN, GPIO_PIN_RESET);
                break;
        }
}

/* ==================== 紧急停机 (ISR 安全) ==================== */

void alarm_emergency_stop(void)
{
        /*
         * 直接操作寄存器, 不调用 HAL (HAL 非 ISR 安全).
         *
         * 1. 关闭 TIM1 所有 PWM 通道
         */
        TIM1->BDTR &= ~TIM_BDTR_MOE;   /* 硬件关断所有输出 */

        /*
         * 2. 红灯常亮 (OUT3, PF1)
         */
        GPIOF->BSRR = GPIO_PIN_1;      /* HIGH = 灯亮 */

        /*
         * 3. 关闭绿灯和黄灯
         */
        GPIOB->BSRR = (uint32_t)(GPIO_PIN_4 | GPIO_PIN_5) << 16;  /* RESET */

        /*
         * 4. 蜂鸣连续
         */
        GPIOF->BSRR = GPIO_PIN_0;      /* PF0 HIGH = 响 */

        /*
         * 5. 板载红灯亮
         */
        GPIOE->BSRR = (uint32_t)GPIO_PIN_0 << 16;   /* PE0 RESET = LOW = 亮 */
}

/* ==================== 健康等级更新 ==================== */

void alarm_service_update_health(health_level_t level)
{
        iso_set_health_level(level);
}

/* ==================== 状态矩阵刷新 (1s 周期) ==================== */

void alarm_service_refresh(void)
{
        safety_state_t   safety;
        health_level_t   health;
        operation_mode_t mode;
        system_state_t   sys_state;

        if (!s_alarm.initialized)
                return;

        safety   = iso_get_safety_state();
        health   = iso_get_health_level();
        mode     = iso_get_operation_mode();
        sys_state = iso_get_system_state();

        /*
         * 更新闪烁相位 (每1s翻转一次)
         */
        s_alarm.blink_tick++;
        if (s_alarm.blink_tick >= 2) {
                s_alarm.blink_tick = 0;
                s_alarm.blink_phase = !s_alarm.blink_phase;
        }

        /*
         * 状态矩阵驱动 LED 输出
         */
        switch (sys_state) {

        case SYS_STATE_EMERGENCY:
                alarm_set_output(ALARM_OUT1_GREEN,  false);
                alarm_set_output(ALARM_OUT2_YELLOW, false);
                alarm_set_output(ALARM_OUT3_RED,    true);
                alarm_set_buzzer(BUZZER_CONTINUOUS);
                break;

        case SYS_STATE_WAIT_RESET:
                alarm_set_output(ALARM_OUT1_GREEN,  false);
                alarm_set_output(ALARM_OUT3_RED,    false);
                alarm_set_output(ALARM_OUT2_YELLOW, s_alarm.blink_phase);  /* 闪烁 */
                alarm_set_buzzer(BUZZER_OFF);
                break;

        case SYS_STATE_CRITICAL:
                alarm_set_output(ALARM_OUT1_GREEN,  false);
                alarm_set_output(ALARM_OUT2_YELLOW, false);
                alarm_set_output(ALARM_OUT3_RED,    true);
                if (mode == MODE_AUTO)
                        alarm_set_buzzer(BUZZER_CONTINUOUS);
                else
                        alarm_set_buzzer(BUZZER_OFF);
                break;

        case SYS_STATE_WARNING:
                alarm_set_output(ALARM_OUT1_GREEN,  false);
                alarm_set_output(ALARM_OUT3_RED,    false);
                alarm_set_output(ALARM_OUT2_YELLOW, true);
                if (mode == MODE_AUTO)
                        alarm_set_buzzer(BUZZER_INTERMITTENT);
                else
                        alarm_set_buzzer(BUZZER_OFF);
                break;

        case SYS_STATE_NORMAL:
        default:
                alarm_set_output(ALARM_OUT2_YELLOW, false);
                alarm_set_output(ALARM_OUT3_RED,    false);
                alarm_set_output(ALARM_OUT1_GREEN,  true);
                alarm_set_buzzer(BUZZER_OFF);
                break;
        }

        /*
         * 间歇蜂鸣器控制: 1s 周期翻转
         */
        if (s_alarm.buzzer_mode == BUZZER_INTERMITTENT) {
                HAL_GPIO_WritePin(BUZZER_PORT, BUZZER_PIN,
                        s_alarm.blink_phase ? GPIO_PIN_SET : GPIO_PIN_RESET);
        }

        /*
         * 板载 LED 辅助指示
         *   DS0 (PE0, LOW=亮): 系统异常指示灯
         *   DS1 (PE1, LOW=亮): 系统正常指示灯
         */
        if (sys_state >= SYS_STATE_EMERGENCY) {
                /* 异常: DS0亮, DS1灭 */
                HAL_GPIO_WritePin(DS0_PORT, DS0_PIN, GPIO_PIN_RESET);
                HAL_GPIO_WritePin(DS1_PORT, DS1_PIN, GPIO_PIN_SET);
        } else if (sys_state >= SYS_STATE_WARNING) {
                /* 警告: 两灯同时闪烁 */
                HAL_GPIO_WritePin(DS0_PORT, DS0_PIN,
                        s_alarm.blink_phase ? GPIO_PIN_RESET : GPIO_PIN_SET);
                HAL_GPIO_WritePin(DS1_PORT, DS1_PIN,
                        s_alarm.blink_phase ? GPIO_PIN_RESET : GPIO_PIN_SET);
        } else {
                /* 正常: DS0灭, DS1亮 */
                HAL_GPIO_WritePin(DS0_PORT, DS0_PIN, GPIO_PIN_SET);
                HAL_GPIO_WritePin(DS1_PORT, DS1_PIN, GPIO_PIN_RESET);
        }
}

/* ==================== 初始化 ==================== */

int alarm_service_init(void)
{
        /*
         * GPIO 已在 MX_GPIO_Init() 中初始化:
         *   PB4/PB5: OUTPUT_PP, NOPULL, LOW
         *   PF0/PF1: OUTPUT_PP, NOPULL, LOW
         *   PC13:    OUTPUT_PP, NOPULL, LOW
         *   PE0/PE1: OUTPUT_PP, NOPULL, HIGH (LED off)
         *
         * 这里仅验证并设置初始状态.
         */
        memset(&s_alarm, 0, sizeof(s_alarm));

        /*
         * 初始状态: 全部输出关闭 (安全默认)
         * 下次 alarm_service_refresh() 会根据实际系统状态更新.
         */
        alarm_set_output(ALARM_OUT1_GREEN,  false);
        alarm_set_output(ALARM_OUT2_YELLOW, false);
        alarm_set_output(ALARM_OUT3_RED,    false);
        alarm_set_output(ALARM_OUT4_RELAY,  false);
        alarm_set_buzzer(BUZZER_OFF);

        s_alarm.initialized = true;

        pr_info_with_tag("ALARM",
                "Alarm service initialized (4 outputs + buzzer + 2 onboard LEDs)\n");

        return ERR_OK;
}
