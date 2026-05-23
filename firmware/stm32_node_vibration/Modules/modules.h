/**
 * @file modules.h
 * @brief STM32 基础模块汇总头文件
 *
 * 统一引入所有基础模块的头文件。
 * 上层业务代码只需 include 本文件即可使用所有基础功能。
 *
 * 使用方法:
 *   #include "modules.h"
 *
 * 模块列表:
 *   - global_error: 全局错误码定义与管理
 *   - queue: 通用环形队列
 *   - crc: CRC 校验工具 (CRC8/16/32)
 *   - timestamp: 时间戳与延时工具
 *   - system_log: 日志系统
 *   - digital_io: 12路隔离输入 (NC/NO语义反转 + 安全状态机)
 *   - alarm_service: 声光报警 (4路隔离输出 + 蜂鸣器 + 板载LED)
 *   - protocol: UART 通信协议（与 ESP32 对接）
 *   - wdg: 看门狗心跳守护
 */

#ifndef __MODULES_H
#define __MODULES_H

/* ==================== 核心依赖（必须最先包含）==================== */

#include "global_error/global_error.h"

/* ==================== 通用工具模块 ==================== */

#include "utils/queue.h"


/* ==================== 日志系统 ==================== */

#include "system_log/system_log.h"

/* ==================== 隔离输入输出模块 ==================== */

#include "digital_io/digital_io.h"
#include "alarm_service/alarm_service.h"

/* ==================== 通信协议模块 ==================== */

#include "protocol/protocol.h"

/* ==================== 看门狗模块 ==================== */

#include "wdg/wdg_heartbeat.h"

#endif /* __MODULES_H */
