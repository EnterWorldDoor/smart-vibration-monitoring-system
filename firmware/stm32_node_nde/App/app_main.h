/*
 * App/app_main.h — NDE裸机应用入口
 *
 * app_nde_init(): 硬件初始化 (ADXL345 + DSP + CAN), 健康状态机就绪.
 * app_nde_loop(): 主循环 — FIFO采样 → DSP → CAN发送 → 心跳 → IWDG喂狗.
 */

#ifndef __APP_MAIN_H
#define __APP_MAIN_H

#include <stdint.h>

/* 三级健康状态 */
enum app_health {
	HEALTH_OK,          /* 全功能正常 */
	HEALTH_DEGRADED,    /* 组件异常, 尝试自恢复 */
	HEALTH_CRITICAL,    /* 传感器/CAN连续失败>3, 停止喂狗等待IWDG复位 */
};

int app_nde_init(void);
void app_nde_loop(void);

#endif /* __APP_MAIN_H */
