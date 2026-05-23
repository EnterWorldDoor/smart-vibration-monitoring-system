/**
 * @file digital_io.h
 * @brief 12路隔离输入 — 语义统一层 (NC/NO反转 + 边沿/电平检测)
 *
 * 硬件: DMF407 隔离输入端子 CN25, 光耦 LTV-247 共阴极接法
 *   - NC (常闭): 正常=闭合(GPIO_SET), 断开(GPIO_RESET)=触发
 *   - NO (常开): 正常=断开(GPIO_RESET), 闭合(GPIO_SET)=触发
 *
 * 语义层: iso_input_read(ch) 返回已反转的语义值
 *   - 1 = 正常/激活/触发 (业务含义统一)
 *   - 0 = 空闲/断开
 *
 * 上层代码不感知某路输入是 NC 还是 NO。
 */

#ifndef __DIGITAL_IO_H
#define __DIGITAL_IO_H

#include <stdint.h>
#include <stdbool.h>

/* ==================== 输入通道常量 ==================== */

#define ISO_IN_CHANNEL_COUNT    12

#define ISO_IN1_ESTOP           0
#define ISO_IN2_MODE_SWITCH     1
#define ISO_IN3_ALARM_RESET     2
#define ISO_IN4                 3
#define ISO_IN5                 4
#define ISO_IN6                 5
#define ISO_IN7                 6
#define ISO_IN8                 7
#define ISO_IN9                 8
#define ISO_IN10                9
#define ISO_IN11                10
#define ISO_IN12                11

/* ==================== 系统状态枚举 ==================== */

typedef enum {
        SAFETY_NORMAL   = 0,  /* 安全回路闭合, 正常运行 */
        SAFETY_EMERGENCY = 1,  /* 急停拍下 (IN1 NC断开) */
        SAFETY_WAIT_RESET = 2, /* 急停已复位, 等待IN3确认 */
} safety_state_t;

typedef enum {
        HEALTH_NORMAL   = 0,
        HEALTH_WARNING  = 1,
        HEALTH_CRITICAL = 2,
} health_level_t;

typedef enum {
        MODE_MANUAL = 0,       /* 手动模式 */
        MODE_AUTO   = 1,       /* 自动模式 (AI控制) */
} operation_mode_t;

typedef enum {
        SYS_STATE_NORMAL    = 0,
        SYS_STATE_WARNING   = 1,
        SYS_STATE_CRITICAL  = 2,
        SYS_STATE_EMERGENCY = 3,
        SYS_STATE_WAIT_RESET = 4,
} system_state_t;

/* ==================== 事件类型 ==================== */

typedef enum {
        ISO_EVENT_ESTOP_PRESS   = 0,  /* IN1 急停按下 (NC断开) */
        ISO_EVENT_ESTOP_RELEASE = 1,  /* IN1 急停复位 (NC重新闭合) */
        ISO_EVENT_MODE_CHANGE   = 2,  /* IN2 模式切换 */
        ISO_EVENT_RESET_CONFIRM = 3,  /* IN3 报警复位确认 */
} iso_event_t;

/* ==================== 输入配置结构体 ==================== */

struct iso_input_cfg {
        uint8_t       channel;      /* IN1..IN12 */
        GPIO_TypeDef *port;         /* GPIO端口 */
        uint16_t      pin;          /* GPIO引脚 */
        bool          is_nc;        /* true=NC触点(需反转), false=NO触点 */
        bool          is_latching;  /* true=自锁(电平型), false=自复(边沿型) */
        const char   *name;         /* 名称 (日志用) */
};

/* ==================== 事件回调类型 ==================== */

typedef void (*iso_event_cb_t)(uint8_t channel, iso_event_t event, void *user_data);

/* ==================== 公共 API ==================== */

/**
 * digital_io_init - 初始化隔离输入模块
 *
 * 硬件层: 配置 IN1(PG3) EXTI下降沿, IN3(PG5) EXTI上升沿, 使能NVIC
 * 语义层: 上电读取所有12路物理状态 → 构建初始语义状态
 * 安全:   上电检测IN1已断开 → 直接进入EMERGENCY (Fail-Safe启动)
 *
 * Return: 0=成功, 负数=错误码
 */
int digital_io_init(void);

/**
 * iso_input_read - 读取指定通道的语义值 (已做NC/NO反转)
 * @ch: 通道号 (0..11)
 *
 * NC触点: GPIO_SET(闭合)→返回1(正常), GPIO_RESET(断开)→返回0(停机)
 * NO触点: GPIO_SET(闭合)→返回1(激活), GPIO_RESET(断开)→返回0(空闲)
 *
 * Return: 0 或 1
 */
bool iso_input_read(uint8_t ch);

/**
 * iso_is_estopped - 快捷: 急停是否激活
 *
 * Return: true=急停已拍下 (电机必须停)
 */
bool iso_is_estopped(void);

/**
 * iso_is_auto_mode - 快捷: 是否自动模式
 *
 * Return: true=自动, false=手动
 */
bool iso_is_auto_mode(void);

/**
 * iso_input_poll - 轮询 IN2 模式开关 (自锁按钮, app_enterprise 500ms调用)
 *
 * 检测电平变化 → 更新内部模式状态 → 触发事件回调
 * 极快返回 (<10μs), 无阻塞
 *
 * Return: 0=无变化, 1=模式已切换
 */
int iso_input_poll(void);

/**
 * iso_input_process_events - 处理 EXTI 事件队列, 推进安全状态机
 *
 * 由 app_enterprise 主循环周期调用 (1s).
 * 处理 EXTI ISR 投递的事件标志:
 *   - IN1下降沿 → SAFETY_EMERGENCY → 关PWM, 红灯, 蜂鸣连续
 *   - IN1上升沿 → SAFETY_WAIT_RESET → 黄灯闪烁, 等待IN3
 *   - IN3上升沿 → SAFETY_NORMAL (仅当安全状态=WAIT_RESET时有效)
 *
 * 安全规则:
 *   1. 急停复位后绝不自动重启电机, 必须等待IN3手动复位确认 (双动作恢复)
 *   2. EMERGENCY状态下所有电机控制指令被忽略
 *   3. 上电检测: 如果IN1已断开, 直接进入EMERGENCY状态 (Fail-Safe启动)
 */
void iso_input_process_events(void);

/**
 * iso_get_system_state - 返回合并后的系统状态
 *
 * 三维优先级合并: 安全状态 > 健康等级 > 运行模式
 * 合并规则参见 CONTEXT.md 系统状态模型表格
 */
system_state_t iso_get_system_state(void);

/**
 * iso_get_safety_state - 返回安全状态
 */
safety_state_t iso_get_safety_state(void);

/**
 * iso_get_health_level - 返回健康等级
 */
health_level_t iso_get_health_level(void);

/**
 * iso_get_operation_mode - 返回运行模式
 */
operation_mode_t iso_get_operation_mode(void);

/**
 * iso_set_health_level - 设置健康等级 (由AI/规则引擎调用)
 * @level: 新健康等级
 *
 * 通常由 ESP32 AI 分析结果或 ISO 10816 规则引擎驱动。
 * 变化时自动触发上行事件。
 */
void iso_set_health_level(health_level_t level);

/**
 * iso_register_callback - 注册输入事件回调
 * @ch:       通道号 (0..11)
 * @cb:       回调函数 (ISR安全, 应极短)
 * @user_data: 用户数据指针
 *
 * Return: 0=成功, 负数=错误码
 */
int iso_register_callback(uint8_t ch, iso_event_cb_t cb, void *user_data);

/**
 * iso_check_estop_release - 检测急停释放 (IN1 NC重新闭合)
 *
 * PG3 仅配置了 IT_FALLING, 不能通过 EXTI 检测释放.
 * 在 iso_input_process_events() 之前, 由主循环周期调用此函数
 * 检测 IN1 物理电平恢复.
 *
 * 如果 IN1 NC 重新闭合 (GPIO_SET) 且当前为 EMERGENCY 状态,
 * 则触发 iso_signal_estop_release() → WAIT_RESET.
 */
void iso_check_estop_release(void);

#endif /* __DIGITAL_IO_H */
