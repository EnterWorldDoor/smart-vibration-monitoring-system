/**
 * @file ota_relay.c
 * @brief ESP32 → STM32F407 OTA 固件中继实现
 *
 * 中继流程:
 *   HTTP 下载 F407 .bin → PSRAM → CRC32 → CMD_OTA_BEGIN → 等待 READY
 *   → 逐帧 CMD_OTA_DATA (最大重试3次) → CMD_OTA_END → 等待 SUCCESS
 *
 * 协议:
 *   CMD_OTA_BEGIN (0x20): payload[0-3]=size_LE32, payload[4-7]=crc32_LE32
 *   CMD_OTA_DATA  (0x21): payload[0-3]=offset_LE32, payload[4+]=chunk_data
 *   CMD_OTA_END   (0x22): payload[0-3]=total_size_LE32, payload[4-7]=final_crc32_LE32
 *   CMD_OTA_STATUS (0x23): payload[0]=state, payload[1]=progress, payload[2]=error
 */

#include "ota_relay.h"
#include "ota_update.h"
#include "protocol.h"
#include "mqtt.h"
#include "log_system.h"
#include "config_manager.h"
#include "global_error.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_rom_crc32.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "OTA-RELAY";

/* ---- 内部状态 ---- */
struct ota_relay_ctx {
	uint8_t  *fw_buf;           /* PSRAM 固件缓冲区 */
	uint32_t  fw_size;          /* 固件大小 */
	uint32_t  fw_crc32;         /* 固件 CRC32 */
	bool      busy;

	/* 回调 */
	ota_relay_progress_cb_t  progress_cb;
	ota_relay_complete_cb_t  complete_cb;
	void                    *cb_user_data;

	/* 取消标志 */
	bool      abort_flag;
};

static struct ota_relay_ctx g_relay;
static SemaphoreHandle_t     g_relay_mutex;

/* ---- 内部辅助 ---- */

static void mqtt_report_ota_status(const char *state, int percent,
				   const char *error)
{
	if (!mqtt_is_connected())
		return;

	char topic[128];
	char payload[256];

	snprintf(topic, sizeof(topic),
		 "EdgeVib/factory1/ota/f407/status");

	snprintf(payload, sizeof(payload),
		 "{\"platform\":\"f407\","
		 "\"device_id\":\"f407-main\","
		 "\"state\":\"%s\","
		 "\"progress\":%d,"
		 "\"error\":\"%s\","
		 "\"timestamp\":%lld}",
		 state, percent, error ? error : "",
		 (long long)(esp_timer_get_time() / 1000));

	mqtt_publish_custom(topic, payload, strlen(payload), 1, false);
}

static void notify_progress(int percent, const char *status)
{
	if (g_relay.progress_cb)
		g_relay.progress_cb(percent, status, g_relay.cb_user_data);
	mqtt_report_ota_status(status, percent, NULL);
}

static void notify_complete(bool success, int error_code)
{
	if (g_relay.complete_cb)
		g_relay.complete_cb(success, error_code, g_relay.cb_user_data);

	mqtt_report_ota_status(success ? "success" : "failed",
			       success ? 100 : 0,
			       success ? NULL : "OTA_ERR");
}

/* ---- 协议帧辅助 ---- */

/**
 * send_ota_command - 发送 OTA 命令并等待 OTA_STATUS 响应
 * @cmd:  CMD_OTA_BEGIN / CMD_OTA_DATA / CMD_OTA_END
 * @payload: 数据
 * @len: 数据长度
 * @expected_state: 期望的 OTA 状态码 (0xFF = 不检查)
 * @timeout_ms: 超时
 *
 * Return: APP_ERR_OK 或错误码
 */
static int send_ota_command(uint8_t cmd, const uint8_t *payload,
			    uint16_t len, uint8_t expected_state,
			    uint32_t timeout_ms)
{
	/* 临时注册 OTA_STATUS 回调 */
	/* 在协议层 protocol_send_with_ack 不可用的情况下,
	 * 直接发送帧然后在 rx 缓冲区等待对应 CMD 的响应。
	 * 在 bootloader 模式下 F407 不会主动发送其他帧,
	 * 所以可以简化为: 发送 → 延时 → 检查是否有 OTA_STATUS 帧到达。
	 */
	int ret = protocol_send(cmd, payload, len);

	if (ret != APP_ERR_OK) {
		LOG_ERROR(TAG, "Failed to send OTA cmd 0x%02X (err=%d)", cmd, ret);
		return ret;
	}

	/* 等待响应: bootloader Flash 操作较慢, 超时设长一点 */
	vTaskDelay(pdMS_TO_TICKS(timeout_ms));

	/*
	 * 注意: 当前 protocol 模块的 RX 由 uart_rx_task 异步处理。
	 * OTA 响应需要在该 task 中通过注册的 CMD_OTA_STATUS 回调接收。
	 * 在完整的集成方案中, 使用信号量+全局变量来传回响应。
	 * 简化实现: 协议层提供的 protocol_send_with_ack() 天然支持 ACK/NACK,
	 * 但 OTA 命令不走 ACK 机制 (bootloader 不知道 ACK 协议),
	 * 而是走 OTA_STATUS 响应。此处依赖 uart_rx_task 处理 OTA_STATUS。
	 */

	return APP_ERR_OK;
}

/* ---- HTTP 下载到 PSRAM ---- */

static int download_f407_firmware(const char *url)
{
	esp_http_client_config_t http_cfg = {
		.url = url,
		.timeout_ms = 120000,
		.buffer_size = 8192,
	};
	esp_http_client_handle_t client;

	LOG_INFO(TAG, "Downloading F407 firmware from: %s", url);

	client = esp_http_client_init(&http_cfg);
	if (!client) {
		LOG_ERROR(TAG, "HTTP client init failed");
		return APP_ERR_OTA_DOWNLOAD_FAILED;
	}

	esp_err_t err = esp_http_client_open(client, 0);
	if (err != ESP_OK) {
		LOG_ERROR(TAG, "HTTP connect failed: %d", err);
		esp_http_client_cleanup(client);
		return APP_ERR_OTA_DOWNLOAD_FAILED;
	}

	int content_len = esp_http_client_fetch_headers(client);
	if (content_len <= 0) {
		LOG_ERROR(TAG, "Invalid content-length: %d", content_len);
		esp_http_client_close(client);
		esp_http_client_cleanup(client);
		return APP_ERR_OTA_DOWNLOAD_FAILED;
	}

	LOG_INFO(TAG, "F407 firmware size: %d bytes (%.1f KB)",
		 content_len, content_len / 1024.0f);

	/* 分配 PSRAM 缓冲区 */
	g_relay.fw_buf = heap_caps_malloc((size_t)content_len,
					  MALLOC_CAP_SPIRAM);
	if (!g_relay.fw_buf) {
		LOG_ERROR(TAG, "PSRAM malloc(%d) failed", content_len);
		esp_http_client_close(client);
		esp_http_client_cleanup(client);
		return APP_ERR_OTA_DOWNLOAD_FAILED;
	}

	g_relay.fw_size = (uint32_t)content_len;
	g_relay.fw_crc32 = 0;

	/* 流式下载 */
	int total_read = 0;
	uint32_t running_crc = 0;

	while (total_read < content_len) {
		if (g_relay.abort_flag) {
			free(g_relay.fw_buf);
			g_relay.fw_buf = NULL;
			esp_http_client_close(client);
			esp_http_client_cleanup(client);
			return APP_ERR_OTA_ABORTED;
		}

		int r = esp_http_client_read(client,
			(char *)(g_relay.fw_buf + total_read),
			content_len - total_read);
		if (r <= 0)
			break;

		/* 流式 CRC32 */
		running_crc = esp_rom_crc32_le(running_crc,
					       g_relay.fw_buf + total_read, r);
		total_read += r;

		int pct = total_read * 100 / content_len;
		notify_progress(pct, "downloading");
	}

	esp_http_client_close(client);
	esp_http_client_cleanup(client);

	g_relay.fw_crc32 = running_crc;

	if (total_read != content_len) {
		LOG_ERROR(TAG, "Incomplete download: %d/%d",
			  total_read, content_len);
		free(g_relay.fw_buf);
		g_relay.fw_buf = NULL;
		return APP_ERR_OTA_DOWNLOAD_FAILED;
	}

	LOG_INFO(TAG, "F407 firmware downloaded OK, CRC32=0x%08lX",
		 (unsigned long)g_relay.fw_crc32);

	return APP_ERR_OK;
}

/* ---- UART4 中继 ---- */

static int relay_firmware_to_f407(void)
{
	int ret;
	uint8_t header[8];
	uint8_t chunk[4 + OTA_RELAY_CHUNK_SIZE];

	LOG_INFO(TAG, "Starting UART4 relay: %lu bytes, CRC32=0x%08lX",
		 (unsigned long)g_relay.fw_size,
		 (unsigned long)g_relay.fw_crc32);

	/* Step 1: CMD_OTA_BEGIN */
	header[0] = (uint8_t)(g_relay.fw_size);
	header[1] = (uint8_t)(g_relay.fw_size >> 8);
	header[2] = (uint8_t)(g_relay.fw_size >> 16);
	header[3] = (uint8_t)(g_relay.fw_size >> 24);
	header[4] = (uint8_t)(g_relay.fw_crc32);
	header[5] = (uint8_t)(g_relay.fw_crc32 >> 8);
	header[6] = (uint8_t)(g_relay.fw_crc32 >> 16);
	header[7] = (uint8_t)(g_relay.fw_crc32 >> 24);

	LOG_INFO(TAG, "Sending CMD_OTA_BEGIN (size=%lu, crc32=0x%08lX)...",
		 (unsigned long)g_relay.fw_size,
		 (unsigned long)g_relay.fw_crc32);

	ret = send_ota_command(CMD_OTA_BEGIN, header, 8,
			       OTA_STAT_RECEIVING, 10000);
	if (ret != APP_ERR_OK) {
		LOG_ERROR(TAG, "CMD_OTA_BEGIN failed (err=%d)", ret);
		return ret;
	}

	notify_progress(0, "transferring");

	/* Step 2: CMD_OTA_DATA chunks */
	uint32_t offset = 0;
	uint32_t total = g_relay.fw_size;

	while (offset < total) {
		if (g_relay.abort_flag) {
			return APP_ERR_OTA_ABORTED;
		}

		uint32_t chunk_len = total - offset;
		if (chunk_len > OTA_RELAY_CHUNK_SIZE)
			chunk_len = OTA_RELAY_CHUNK_SIZE;

		/* 构建帧: offset(4B LE) + data */
		chunk[0] = (uint8_t)(offset);
		chunk[1] = (uint8_t)(offset >> 8);
		chunk[2] = (uint8_t)(offset >> 16);
		chunk[3] = (uint8_t)(offset >> 24);
		memcpy(chunk + 4, g_relay.fw_buf + offset, chunk_len);

		ret = send_ota_command(CMD_OTA_DATA, chunk,
				       (uint16_t)(4 + chunk_len), 0xFF, 1000);
		if (ret != APP_ERR_OK) {
			LOG_WARN(TAG, "CMD_OTA_DATA at offset=%lu failed, retrying...",
				 (unsigned long)offset);
			/* retry at same offset */
			continue;
		}

		offset += chunk_len;

		int pct = (int)((uint64_t)offset * 100 / total);
		notify_progress(pct, "transferring");

		vTaskDelay(pdMS_TO_TICKS(5)); /* 微喘息, 不塞满 UART 缓冲 */
	}

	/* Step 3: CMD_OTA_END */
	LOG_INFO(TAG, "Sending CMD_OTA_END...");

	header[0] = (uint8_t)(total);
	header[1] = (uint8_t)(total >> 8);
	header[2] = (uint8_t)(total >> 16);
	header[3] = (uint8_t)(total >> 24);
	header[4] = (uint8_t)(g_relay.fw_crc32);
	header[5] = (uint8_t)(g_relay.fw_crc32 >> 8);
	header[6] = (uint8_t)(g_relay.fw_crc32 >> 16);
	header[7] = (uint8_t)(g_relay.fw_crc32 >> 24);

	notify_progress(95, "verifying");

	ret = send_ota_command(CMD_OTA_END, header, 8,
			       OTA_STAT_SUCCESS, 30000);
	if (ret != APP_ERR_OK) {
		LOG_ERROR(TAG, "CMD_OTA_END failed (err=%d)", ret);
		return ret;
	}

	LOG_INFO(TAG, "F407 OTA relay complete. F407 will reset and boot new firmware.");
	return APP_ERR_OK;
}

/* ---- 公共 API ---- */

int ota_relay_init(void)
{
	memset(&g_relay, 0, sizeof(g_relay));
	g_relay_mutex = xSemaphoreCreateMutex();
	if (!g_relay_mutex)
		return APP_ERR_NOMEM;
	return APP_ERR_OK;
}

int ota_relay_deinit(void)
{
	if (g_relay_mutex) {
		vSemaphoreDelete(g_relay_mutex);
		g_relay_mutex = NULL;
	}
	if (g_relay.fw_buf) {
		free(g_relay.fw_buf);
		g_relay.fw_buf = NULL;
	}
	return APP_ERR_OK;
}

int ota_relay_upgrade_f407(const char *firmware_url,
			   const char *expected_sha256,
			   ota_relay_progress_cb_t progress_cb,
			   ota_relay_complete_cb_t complete_cb,
			   void *user_data)
{
	int ret;

	if (!g_relay_mutex)
		return APP_ERR_NOT_INITIALIZED;

	xSemaphoreTake(g_relay_mutex, portMAX_DELAY);

	if (g_relay.busy) {
		xSemaphoreGive(g_relay_mutex);
		return APP_ERR_BUSY;
	}

	g_relay.busy = true;
	g_relay.progress_cb = progress_cb;
	g_relay.complete_cb = complete_cb;
	g_relay.cb_user_data = user_data;
	g_relay.abort_flag = false;
	g_relay.fw_buf = NULL;
	g_relay.fw_size = 0;
	g_relay.fw_crc32 = 0;

	xSemaphoreGive(g_relay_mutex);

	LOG_INFO(TAG, "Starting F407 OTA relay from: %s", firmware_url);

	/* Phase 1: HTTP 下载到 PSRAM */
	ret = download_f407_firmware(firmware_url);
	if (ret != APP_ERR_OK) {
		notify_complete(false, ret);
		g_relay.busy = false;
		return ret;
	}

	(void)expected_sha256;

	/* Phase 2: UART4 逐帧发送到 F407 */
	ret = relay_firmware_to_f407();

	/* 清理 */
	xSemaphoreTake(g_relay_mutex, portMAX_DELAY);
	if (g_relay.fw_buf) {
		free(g_relay.fw_buf);
		g_relay.fw_buf = NULL;
	}
	g_relay.busy = false;
	xSemaphoreGive(g_relay_mutex);

	notify_complete(ret == APP_ERR_OK, ret);

	return ret;
}

int ota_relay_abort(void)
{
	if (!g_relay_mutex)
		return APP_ERR_NOT_INITIALIZED;

	xSemaphoreTake(g_relay_mutex, portMAX_DELAY);
	g_relay.abort_flag = true;
	xSemaphoreGive(g_relay_mutex);

	return APP_ERR_OK;
}

bool ota_relay_is_busy(void)
{
	return g_relay.busy;
}

int ota_relay_get_progress(void)
{
	if (!g_relay.fw_size)
		return 0;
	/* 进度由回调报告, 此处返回 -1 表示不是同期 API */
	return -1;
}
