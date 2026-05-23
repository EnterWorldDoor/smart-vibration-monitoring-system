/**
 * @file digital_io_exti.c
 * @brief 隔离输入 EXTI 中断服务 (IN1 PG3 下降沿 + IN3 PG5 上升沿)
 *
 * EXTI 分配:
 *   PG3 (IN1 急停 NC)  → EXTI3 下降沿 → 急停激活
 *   PG5 (IN3 复位 NO)  → EXTI5 上升沿 → 复位确认
 *
 * EXTI 共享冲突:
 *   EXTI3: PG3(IN1,已用) ↔ PD3(IN10) — IN10 不能用 EXTI
 *   EXTI9_5: PG5用EXTI5, PG6-9 保留
 *
 * ISR安全: 回调极短 (<10μs), 仅设标志位 + 关PWM.
 */

#include "digital_io.h"
#include "main.h"
#include "tim.h"
#include "system_log/system_log.h"
#include <string.h>

/*
 * 前向声明 (digital_io.c 中的标志位)
 */
extern void iso_signal_estop_activate(void);
extern void iso_signal_estop_release(void);
extern void iso_signal_reset_press(void);

/* ==================== EXTI 初始化 ==================== */

int iso_exti_init(void)
{
        GPIO_InitTypeDef gpio_cfg = {0};

        /*
         * PG3 (IN1 急停): EXTI3 下降沿
         * NC 触点断开 = 下降沿 → Emergency Stop
         */
        gpio_cfg.Pin   = GPIO_PIN_3;
        gpio_cfg.Mode  = GPIO_MODE_IT_FALLING;
        gpio_cfg.Pull  = GPIO_PULLUP;
        gpio_cfg.Speed = GPIO_SPEED_FREQ_LOW;
        HAL_GPIO_Init(GPIOG, &gpio_cfg);

        /*
         * PG5 (IN3 复位): EXTI5 上升沿
         * NO 自复按钮按下 = 上升沿 → Reset Confirm
         */
        gpio_cfg.Pin   = GPIO_PIN_5;
        gpio_cfg.Mode  = GPIO_MODE_IT_RISING;
        gpio_cfg.Pull  = GPIO_PULLUP;
        gpio_cfg.Speed = GPIO_SPEED_FREQ_LOW;
        HAL_GPIO_Init(GPIOG, &gpio_cfg);

        /*
         * 使能 NVIC 中断线
         *
         * 优先级: 6 (低于 wdg_daemon=4, 高于 FreeRTOS=15)
         * 急停 ISR 必须快速响应, 但不应打断关键守护任务.
         */
        HAL_NVIC_SetPriority(EXTI3_IRQn, 5, 0);
        HAL_NVIC_EnableIRQ(EXTI3_IRQn);

        HAL_NVIC_SetPriority(EXTI9_5_IRQn, 5, 0);
        HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);

        pr_info_with_tag("DIGITAL_IO",
                "EXTI configured: PG3(EXTI3↓=E-Stop) PG5(EXTI5↑=Reset)\n");

        return 0;
}

/* ==================== HAL GPIO EXTI 回调 ==================== */

/**
 * HAL_GPIO_EXTI_Callback - 覆盖 HAL 弱定义
 *
 * 由 HAL_GPIO_EXTI_IRQHandler() 在 EXTI ISR 中调用.
 * ISR 上下文 — 仅设标志, 不阻塞, 不做日志 IO.
 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
        switch (GPIO_Pin) {

        case GPIO_PIN_3:
                /*
                 * IN1 急停 (PG3).
                 *
                 * EXTI3 配置为 IT_FALLING, 所以此回调仅在下降沿触发
                 * (NC断开 = 拍下急停). 但硬件抖动可能导致误触发.
                 * 我们在用户代码层做去抖 (iso_input_process_events).
                 *
                 * 但这里也需要检测上升沿用于释放检测.
                 * 实际上 HAL GPIO EXTI 在双边沿模式下才能检测两种边沿.
                 *
                 * 当前配置: PG3 = IT_FALLING only.
                 * 释放检测: 在 iso_input_process_events 中通过轮询
                 * iso_input_read(IN1) 检测物理电平恢复来判断释放.
                 *
                 * 简化方案: 仅 EXTI 检测下降沿 (急停拍下).
                 * 释放 (上升沿/重新闭合) 由 iso_input_poll 轮询检测.
                 */
                iso_signal_estop_activate();
                break;

        case GPIO_PIN_5:
                /*
                 * IN3 报警复位 (PG5).
                 *
                 * EXTI5 配置为 IT_RISING:
                 * NO 自复按钮按下 = 上升沿 = Reset Confirm.
                 */
                iso_signal_reset_press();
                break;

        default:
                break;
        }
}

/*
 * ==================== IN1 释放检测 (轮询, 由 iso_input_poll 调用) ====================
 *
 * 因为 PG3 只配置了 IT_FALLING, 不能通过 EXTI 检测释放.
 * 在 iso_input_process_events 之前, 先检测 IN1 物理电平恢复.
 *
 * 由 digital_io.c 的 iso_input_process_events() 内部调用.
 */
void iso_check_estop_release(void)
{
        /*
         * 读取 IN1 物理电平 (NC: GPIO_SET = 闭合 = 正常)
         * 注意: 使用 iso_input_read 已经做了语义反转,
         * 这里直接读物理电平更可靠.
         */
        GPIO_PinState phys;

        phys = HAL_GPIO_ReadPin(GPIOG, GPIO_PIN_3);
        if (phys == GPIO_PIN_SET) {
                /*
                 * IN1 NC 重新闭合 → 急停已旋转复位.
                 * 但电机不自动恢复, 进入 WAIT_RESET.
                 */
                iso_signal_estop_release();
        }
}
