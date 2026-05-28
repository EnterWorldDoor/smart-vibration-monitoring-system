/**
 * @file ota_relay.h
 * @brief ESP32 → STM32F407 OTA 固件中继模块
 *
 * 功能:
 *   1. 从 OTA Server HTTP 下载 F407 .bin 固件到 PSRAM
 *   2. CRC32 校验固件完整性
 *   3. 通过 UART4 协议逐帧发送 CMD_OTA_BEGIN/DATA/END
 *   4. 处理 F407 bootloader 的 ACK/NACK 响应和重传
 *   5. 通过回调上报进度和结果
 *   6. MQTT 发布 OTA 状态
 */

#ifndef OTA_RELAY_H
#define OTA_RELAY_H

#include <stdint.h>
#include <stdbool.h>
#include "global_error.h"

/* 每帧 payload 大小 (250 - 4B offset header) */
#define OTA_RELAY_CHUNK_SIZE    192
#define OTA_RELAY_RETRY_MAX     3
#define OTA_RELAY_ACK_TIMEOUT_MS  5000

/* 进度回调 */
typedef void (*ota_relay_progress_cb_t)(int percent, const char *status,
					void *user_data);

/* 完成回调 */
typedef void (*ota_relay_complete_cb_t)(bool success, int error_code,
					void *user_data);

/* 生命周期 */
int ota_relay_init(void);
int ota_relay_deinit(void);

/* 核心 API */
int ota_relay_upgrade_f407(const char *firmware_url,
			   const char *expected_sha256,
			   ota_relay_progress_cb_t progress_cb,
			   ota_relay_complete_cb_t complete_cb,
			   void *user_data);

int ota_relay_abort(void);

/* 状态 */
bool ota_relay_is_busy(void);
int  ota_relay_get_progress(void);

#endif /* OTA_RELAY_H */
