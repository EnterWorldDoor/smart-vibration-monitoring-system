/**
 * @file digital_io.c
 * @brief 隔离输入核心逻辑 — 语义层读取 + 轮询 + 状态管理
 *
 * 安全状态机 (双动作恢复):
 *
 *                         EXTI3下降沿 (IN1 NC断开)
 *   NORMAL ─────────────────────────────────────────→ EMERGENCY
 *     │                                                    │
 *     │  绿灯(OUT1)                                        │  TIM1 BDTR.MOE=0 (硬件关PWM)
 *     │  电机正常运行                                       │  红灯常亮(OUT3) + 蜂鸣连续
 *     │                                                    │  系统状态锁存, 电机CMD被忽略
 *     │                                                    │
 *     │                                           EXTI3上升沿 (IN1旋转复位, NC重新闭合)
 *     │                                                    │
 *     │                                          WAIT_RESET ←──┘
 *     │                                                    │
 *     │                                                    │  黄灯闪烁(OUT2) + 蜂鸣停止
 *     │                                                    │  等待手动复位确认
 *     │                                                    │  电机**不自动恢复**!
 *     │                                                    │
 *     └──── IN3上升沿 (报警复位确认) ────────────────────────────┘
 *                                                          │
 *                                                          │  绿灯恢复(OUT1)
 *                                                          │  TIM1 BDTR.MOE=1
 *                                                          │  电机CMD恢复接收
 *                                                          │
 *                                                         NORMAL
 *
 * 安全规则:
 *   1. 急停复位后绝不自动重启电机, 必须等待IN3手动复位确认 (双动作恢复)
 *   2. EMERGENCY状态下所有电机控制指令被忽略
 *   3. 上电检测: 如果IN1已断开, 直接进入EMERGENCY状态 (Fail-Safe启动)
 *   4. IN1断线 (NC开路) = 立即EMERGENCY (Fail-Safe)
 */

#include "digital_io.h"
#include "main.h"
#include "system_log/system_log.h"
#include "global_error/global_error.h"
#include <string.h>

/* ==================== 模块内部状态 ==================== */

static struct {
        safety_state_t     safety_state;    /* 安全状态机当前状态 */
        health_level_t     health_level;    /* 健康等级 (AI/规则引擎更新) */
        operation_mode_t   op_mode;         /* 运行模式 (IN2决定) */
        bool               initialized;
        iso_event_cb_t     callbacks[ISO_IN_CHANNEL_COUNT];
        void              *cb_user_data[ISO_IN_CHANNEL_COUNT];
} s_io;

/*
 * 外部引用: 配置表 (digital_io_config.c)
 */
extern const struct iso_input_cfg *iso_config_get(uint8_t ch);

/* ==================== 语义层读取 ==================== */

/**
 * iso_input_read - 读取通道ch的语义值
 *
 * NC触点: GPIO_SET(闭合)→语义1(正常), GPIO_RESET(断开)→语义0(停机)
 * NO触点: GPIO_SET(闭合)→语义1(激活), GPIO_RESET(断开)→语义0(空闲)
 */
bool iso_input_read(uint8_t ch)
{
        const struct iso_input_cfg *cfg;
        GPIO_PinState phys;

        cfg = iso_config_get(ch);
        if (!cfg)
                return false;

        phys = HAL_GPIO_ReadPin(cfg->port, cfg->pin);

        if (cfg->is_nc)
                return (phys == GPIO_PIN_SET) ? true : false;   /* NC闭合=正常 */
        else
                return (phys == GPIO_PIN_SET) ? true : false;   /* NO闭合=激活 */
}

bool iso_is_estopped(void)
{
        if (!s_io.initialized)
                return false;

        return (s_io.safety_state == SAFETY_EMERGENCY ||
                s_io.safety_state == SAFETY_WAIT_RESET);
}

bool iso_is_auto_mode(void)
{
        return (s_io.op_mode == MODE_AUTO);
}

/* ==================== 状态访问器 ==================== */

system_state_t iso_get_system_state(void)
{
        /*
         * 三维优先级合并: 安全状态 > 健康等级 > 运行模式
         * 参见 CONTEXT.md 系统状态模型表格
         */
        switch (s_io.safety_state) {
        case SAFETY_EMERGENCY:
                return SYS_STATE_EMERGENCY;
        case SAFETY_WAIT_RESET:
                return SYS_STATE_WAIT_RESET;
        case SAFETY_NORMAL:
                break;
        }

        /* 安全状态 NORMAL 时, 按健康等级决定 */
        switch (s_io.health_level) {
        case HEALTH_CRITICAL:
                return SYS_STATE_CRITICAL;
        case HEALTH_WARNING:
                return SYS_STATE_WARNING;
        case HEALTH_NORMAL:
                return SYS_STATE_NORMAL;
        }

        return SYS_STATE_NORMAL;
}

safety_state_t iso_get_safety_state(void)
{
        return s_io.safety_state;
}

health_level_t iso_get_health_level(void)
{
        return s_io.health_level;
}

operation_mode_t iso_get_operation_mode(void)
{
        return s_io.op_mode;
}

void iso_set_health_level(health_level_t level)
{
        if (level != s_io.health_level) {
                pr_info_with_tag("DIGITAL_IO",
                        "Health level changed: %d → %d\n",
                        (int)s_io.health_level, (int)level);
                s_io.health_level = level;
        }
}

/* ==================== 事件回调注册 ==================== */

int iso_register_callback(uint8_t ch, iso_event_cb_t cb, void *user_data)
{
        if (ch >= ISO_IN_CHANNEL_COUNT)
                return ERR_INVALID_PARAM;

        s_io.callbacks[ch] = cb;
        s_io.cb_user_data[ch] = user_data;

        return ERR_OK;
}

static void iso_notify_event(uint8_t ch, iso_event_t event)
{
        if (ch < ISO_IN_CHANNEL_COUNT && s_io.callbacks[ch]) {
                s_io.callbacks[ch](ch, event, s_io.cb_user_data[ch]);
        }
}

/* ==================== 安全状态机 (iso_input_process_events 内部) ==================== */

/*
 * 由 EXTI ISR (digital_io_exti.c) 设置的事件标志.
 * volatile: ISR 写入, 主循环读取.
 */
static volatile bool s_estop_activated;    /* IN1 下降沿 → EMERGENCY */
static volatile bool s_estop_released;     /* IN1 上升沿 → WAIT_RESET */
static volatile bool s_reset_pressed;      /* IN3 上升沿 → 确认复位 */

/*
 * 以下由 digital_io_exti.c 中的 ISR 调用.
 */
void iso_signal_estop_activate(void)
{
        s_estop_activated = true;
}

void iso_signal_estop_release(void)
{
        s_estop_released = true;
}

void iso_signal_reset_press(void)
{
        s_reset_pressed = true;
}

void iso_input_process_events(void)
{
        if (!s_io.initialized)
                return;

        /*
         * 处理急停激活 (最高优先级)
         */
        if (s_estop_activated) {
                s_estop_activated = false;

                pr_error_with_tag("DIGITAL_IO",
                        "EMERGENCY STOP activated!\n");

                s_io.safety_state = SAFETY_EMERGENCY;

                /* TIM1 硬件关断 PWM (BDTR.MOE=0) */
                HAL_TIM_PWM_Stop(&htim1, TIM_CHANNEL_1);
                HAL_TIM_PWM_Stop(&htim1, TIM_CHANNEL_2);
                HAL_TIM_PWM_Stop(&htim1, TIM_CHANNEL_3);
                HAL_TIMEx_PWMN_Stop(&htim1, TIM_CHANNEL_1);
                HAL_TIMEx_PWMN_Stop(&htim1, TIM_CHANNEL_2);
                HAL_TIMEx_PWMN_Stop(&htim1, TIM_CHANNEL_3);

                iso_notify_event(ISO_IN1_ESTOP, ISO_EVENT_ESTOP_PRESS, NULL);
        }

        /*
         * 处理急停释放 → WAIT_RESET (电机不自动恢复)
         */
        if (s_estop_released) {
                s_estop_released = false;

                if (s_io.safety_state == SAFETY_EMERGENCY) {
                        pr_info_with_tag("DIGITAL_IO",
                                "E-Stop released, waiting for reset confirm\n");
                        s_io.safety_state = SAFETY_WAIT_RESET;
                        iso_notify_event(ISO_IN1_ESTOP,
                                        ISO_EVENT_ESTOP_RELEASE, NULL);
                }
        }

        /*
         * 处理复位确认 (仅在 WAIT_RESET 状态有效)
         */
        if (s_reset_pressed) {
                s_reset_pressed = false;

                if (s_io.safety_state == SAFETY_WAIT_RESET) {
                        pr_info_with_tag("DIGITAL_IO",
                                "Reset confirmed, restoring normal operation\n");

                        s_io.safety_state = SAFETY_NORMAL;

                        /* 恢复 TIM1 PWM 输出 (BDTR.MOE=1) */
                        HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
                        HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_2);
                        HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_3);
                        HAL_TIMEx_PWMN_Start(&htim1, TIM_CHANNEL_1);
                        HAL_TIMEx_PWMN_Start(&htim1, TIM_CHANNEL_2);
                        HAL_TIMEx_PWMN_Start(&htim1, TIM_CHANNEL_3);

                        iso_notify_event(ISO_IN3_ALARM_RESET,
                                        ISO_EVENT_RESET_CONFIRM, NULL);
                }
        }
}

/* ==================== IN2 模式开关轮询 ==================== */

int iso_input_poll(void)
{
        bool current_mode;
        operation_mode_t new_mode;

        if (!s_io.initialized)
                return 0;

        /* 读取IN2物理电平, 经语义层反转 (NO触点) */
        current_mode = iso_input_read(ISO_IN2_MODE_SWITCH);
        new_mode = current_mode ? MODE_AUTO : MODE_MANUAL;

        if (new_mode != s_io.op_mode) {
                s_io.op_mode = new_mode;
                pr_info_with_tag("DIGITAL_IO",
                        "Mode switched to: %s\n",
                        (new_mode == MODE_AUTO) ? "AUTO" : "MANUAL");
                iso_notify_event(ISO_IN2_MODE_SWITCH,
                                ISO_EVENT_MODE_CHANGE, NULL);
                return 1;
        }

        return 0;
}

/* ==================== 初始化 ==================== */

int digital_io_init(void)
{
        /*
         * 注意: GPIO Init (MX_GPIO_Init) 已在 main() 中完成.
         * 这里仅配置 EXTI + 读取上电状态.
         *
         * EXTI 配置在 digital_io_exti.c 的 iso_exti_init() 中完成,
         * 由 digital_io_init() 内部调用.
         */

        /* 外部函数声明 (digital_io_exti.c) */
        extern int iso_exti_init(void);

        memset(&s_io, 0, sizeof(s_io));
        s_io.health_level = HEALTH_NORMAL;
        s_io.op_mode = MODE_MANUAL;  /* 默认手动, 安全 */

        /*
         * 步骤1: 先读 IN1 物理状态 → 决定上电安全状态
         *
         * NC 触点: 闭合(GPIO_SET)→正常, 断开(GPIO_RESET)→急停
         * 上电时 IN1 已断开 = Fail-Safe 启动 = 直接 EMERGENCY
         */
        {
                GPIO_PinState in1_phys;

                in1_phys = HAL_GPIO_ReadPin(GPIOG, GPIO_PIN_3);
                if (in1_phys == GPIO_PIN_RESET) {
                        pr_error_with_tag("DIGITAL_IO",
                                "Power-on E-Stop detected! Entering EMERGENCY.\n");
                        s_io.safety_state = SAFETY_EMERGENCY;
                } else {
                        s_io.safety_state = SAFETY_NORMAL;
                }
        }

        /*
         * 步骤2: 读 IN2 初始模式 (NO自锁: 闭合=自动, 断开=手动)
         */
        {
                GPIO_PinState in2_phys;

                in2_phys = HAL_GPIO_ReadPin(GPIOG, GPIO_PIN_4);
                if (in2_phys == GPIO_PIN_SET) {
                        s_io.op_mode = MODE_AUTO;
                } else {
                        s_io.op_mode = MODE_MANUAL;
                }

                pr_info_with_tag("DIGITAL_IO",
                        "Initial mode: %s\n",
                        (s_io.op_mode == MODE_AUTO) ? "AUTO" : "MANUAL");
        }

        /*
         * 步骤3: 初始化 EXTI (IN1下降沿 + IN3上升沿)
         */
        if (iso_exti_init() != 0) {
                pr_error_with_tag("DIGITAL_IO", "EXTI init failed\n");
                return ERR_COMM_INIT_FAIL;
        }

        s_io.initialized = true;

        pr_info_with_tag("DIGITAL_IO",
                "12-ch isolated input initialized (safety=%d, mode=%d)\n",
                (int)s_io.safety_state, (int)s_io.op_mode);

        return ERR_OK;
}
