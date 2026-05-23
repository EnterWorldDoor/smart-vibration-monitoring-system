/**
 * @file digital_io_config.c
 * @brief 12路隔离输入配置表 (静态数组)
 *
 * EXTI共享冲突 (STM32F4 每EXTI线仅一个Port):
 *   EXTI3: PG3(IN1,已用) ↔ PD3(IN10) — IN10不能再配EXTI
 *   EXTI7: PG7(IN5) ↔ PD7(IN11) — 选PG7
 *   EXTI8: PG8(IN6) ↔ PC8(IN9) — 选PG8
 *
 * 受影响通道 (IN9/IN10/IN11) 仅支持轮询, 不配EXTI。
 */

#include "digital_io.h"
#include "main.h"

/*
 * 12路通道配置表 (编译时初始化, 零RAM分配)
 *
 * is_nc:       true=NC触点(硬件反转), false=NO触点(不作反转)
 * is_latching: true=自锁按钮(电平型, 轮询), false=自复按钮(边沿型, EXTI)
 *
 * IN1 急停 (NC, 自锁): 蘑菇头旋转复位, 拍下=断开, 旋转复位后重新闭合
 * IN2 模式切换 (NO, 自锁): 按下锁住=自动(闭合), 抬起=手动(断开)
 * IN3 报警复位 (NO, 自复): 按下=上升沿触发, 松开后自弹回
 * IN4-12: 保留, 默认NO/自复, 可外接工业传感器
 */
static const struct iso_input_cfg s_iso_config[ISO_IN_CHANNEL_COUNT] = {
        /* ch,  port,      pin,          is_nc, is_latching, name */
        [ISO_IN1_ESTOP]       = { 0,  GPIOG, GPIO_PIN_3,  true,  true,  "IN1-EStop"       },
        [ISO_IN2_MODE_SWITCH] = { 1,  GPIOG, GPIO_PIN_4,  false, true,  "IN2-ModeSwitch"  },
        [ISO_IN3_ALARM_RESET] = { 2,  GPIOG, GPIO_PIN_5,  false, false, "IN3-AlarmReset"  },
        [ISO_IN4]             = { 3,  GPIOG, GPIO_PIN_6,  false, false, "IN4-Reserved"    },
        [ISO_IN5]             = { 4,  GPIOG, GPIO_PIN_7,  false, false, "IN5-Reserved"    },
        [ISO_IN6]             = { 5,  GPIOG, GPIO_PIN_8,  false, false, "IN6-Reserved"    },
        [ISO_IN7]             = { 6,  GPIOG, GPIO_PIN_9,  false, false, "IN7-Reserved"    },
        [ISO_IN8]             = { 7,  GPIOG, GPIO_PIN_10, false, false, "IN8-Reserved"    },
        [ISO_IN9]             = { 8,  GPIOC, GPIO_PIN_8,  false, false, "IN9-Reserved"    },
        [ISO_IN10]            = { 9,  GPIOD, GPIO_PIN_3,  false, false, "IN10-Reserved"   },
        [ISO_IN11]            = { 10, GPIOD, GPIO_PIN_7,  false, false, "IN11-Reserved"   },
        [ISO_IN12]            = { 11, GPIOG, GPIO_PIN_2,  false, false, "IN12-Reserved"   },
};

/**
 * iso_config_get - 获取指定通道的配置 (内部使用)
 */
const struct iso_input_cfg *iso_config_get(uint8_t ch)
{
        if (ch >= ISO_IN_CHANNEL_COUNT)
                return NULL;
        return &s_iso_config[ch];
}
