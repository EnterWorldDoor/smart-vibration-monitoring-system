/*
 * App/app_main.c — NDE裸机应用主循环
 *
 * 数据流: ADXL345 FIFO突发 → 累积64样本
 *        → DSP (q15 FFT + 24维特征)
 *        → CAN发送 (17帧CRC8 @ 2s)
 *        → CAN心跳 (1帧CRC8 @ 1s)
 *
 * 健康状态机: HEALTH_OK → DEGRADED (≥1错误)
 *                      → CRITICAL (连续失败>3, 停止喂狗)
 *
 * IWDG: LSI 40kHz, 3s超时, 主循环喂狗
 */

#include "app_main.h"
#include "bsp/adxl345/bsp_adxl345.h"
#include "bsp/can/bsp_can.h"
#include "dsp_nde.h"
#include "can_send.h"
#include "main.h"
#include <string.h>

/* IWDG feed — direct register (STM32F1 HAL 1.8.6 has no IWDG driver) */
#define IWDG_FEED()  (IWDG->KR = 0xAAAA)

#define BSP_LOG_ENABLE  1
#include "bsp/bsp_log.h"

/* 连续故障阈值: 超过则进入CRITICAL */
#define CONSECUTIVE_FAIL_MAX    3

/* 定时参数 (ms) */
#define FEATURE_INTERVAL_MS     2000
#define HEARTBEAT_INTERVAL_MS   1000

/* NDE无温度传感器, 填固定值 */
#define NDE_TEMP_DEFAULT        22

struct app_state {
	enum app_health health;
	uint8_t error_count;
	uint8_t consecutive_dsp_fail;
	uint8_t consecutive_can_fail;
	uint32_t last_feature_ms;
	uint32_t last_heartbeat_ms;
	uint8_t window_idx;

	/* FIFO采样累积缓冲 */
	int16_t buf_x[ADXL345_WINDOW_SIZE];
	int16_t buf_y[ADXL345_WINDOW_SIZE];
	int16_t buf_z[ADXL345_WINDOW_SIZE];
	uint8_t buf_count;
};

static struct app_state s_app;

/*
 * 转换健康状态为心跳online字节.
 */
static uint8_t health_to_online(enum app_health h)
{
	return (h == HEALTH_CRITICAL) ? 0 : 1;
}

int app_nde_init(void)
{
	int ret;

	memset(&s_app, 0, sizeof(s_app));
	s_app.health = HEALTH_OK;

	ret = bsp_adxl345_init();
	if (ret < 0) {
		pr_error("ADXL345 init failed, entering CRITICAL\n");
		s_app.health = HEALTH_CRITICAL;
		return ret;
	}

	ret = dsp_nde_init();
	if (ret < 0) {
		pr_error("DSP init failed\n");
		return ret;
	}

	ret = can_send_init();
	if (ret < 0) {
		pr_error("CAN init failed, entering CRITICAL\n");
		s_app.health = HEALTH_CRITICAL;
		return ret;
	}

	s_app.last_feature_ms = HAL_GetTick();
	s_app.last_heartbeat_ms = HAL_GetTick();

	pr_info("NDE node ready, health=OK\n");
	return 0;
}

/*
 * 从ADXL345 FIFO读取一批样本并追加到累积缓冲.
 * 返回新读取的样本数, 负值=错误.
 */
static int sample_accumulate(void)
{
	int16_t burst_buf[ADXL345_FIFO_WATERMARK * 3];
	int burst_count;
	int i;

	burst_count = bsp_adxl345_read_fifo_burst(burst_buf,
			 ADXL345_FIFO_WATERMARK);
	if (burst_count <= 0)
		return burst_count;

	for (i = 0; i < burst_count; i++) {
		if (s_app.buf_count >= ADXL345_WINDOW_SIZE)
			break;

		s_app.buf_x[s_app.buf_count] = burst_buf[i * 3];
		s_app.buf_y[s_app.buf_count] = burst_buf[i * 3 + 1];
		s_app.buf_z[s_app.buf_count] = burst_buf[i * 3 + 2];
		s_app.buf_count++;
	}

	return burst_count;
}

/*
 * 窗口满 → DSP处理 + 定时CAN发送.
 */
static void process_window(void)
{
	struct dsp_nde_result result;
	int ret;

	s_app.buf_count = 0;

	ret = dsp_nde_process(s_app.buf_x, s_app.buf_y, s_app.buf_z,
			      ADXL345_WINDOW_SIZE, &result);
	if (ret < 0) {
		s_app.error_count++;
		s_app.consecutive_dsp_fail++;
		pr_warn("DSP process failed\n");
		return;
	}

	result.window_idx = s_app.window_idx++;
	s_app.consecutive_dsp_fail = 0;

	/* 每2s发送特征向量 (CAN 0x201) */
	{
		uint32_t now;

		now = HAL_GetTick();
		if (now - s_app.last_feature_ms >= FEATURE_INTERVAL_MS) {
			s_app.last_feature_ms = now;
			ret = can_send_features(result.features,
					       result.window_idx);
			if (ret < 0) {
				s_app.error_count++;
				s_app.consecutive_can_fail++;
				pr_warn("CAN features send failed\n");
			} else {
				s_app.consecutive_can_fail = 0;
			}
		}
	}
}

/*
 * 每1s发送心跳帧 (CAN 0x202).
 */
static void send_heartbeat(void)
{
	uint32_t now;
	uint8_t online;
	int ret;

	now = HAL_GetTick();
	if (now - s_app.last_heartbeat_ms < HEARTBEAT_INTERVAL_MS)
		return;

	s_app.last_heartbeat_ms = now;
	online = health_to_online(s_app.health);

	ret = can_send_heartbeat(online, s_app.error_count,
				NDE_TEMP_DEFAULT);
	if (ret < 0) {
		s_app.consecutive_can_fail++;
		pr_warn("CAN heartbeat send failed\n");
	}
}

/*
 * 评估健康状态转移.
 */
static void update_health(void)
{
	enum app_health prev;

	prev = s_app.health;

	if (s_app.consecutive_dsp_fail > CONSECUTIVE_FAIL_MAX ||
	    s_app.consecutive_can_fail > CONSECUTIVE_FAIL_MAX) {
		s_app.health = HEALTH_CRITICAL;
	} else if (s_app.error_count > 0) {
		s_app.health = HEALTH_DEGRADED;
	} else {
		s_app.health = HEALTH_OK;
	}

	if (s_app.health != prev) {
		pr_warn("Health transition: %d → %d (err=%u, dsp_fail=%u,"
			" can_fail=%u)\n",
			prev, s_app.health, s_app.error_count,
			s_app.consecutive_dsp_fail,
			s_app.consecutive_can_fail);
	}
}

void app_nde_loop(void)
{
	/* 1. FIFO突发采样 */
	if (bsp_adxl345_data_ready())
		sample_accumulate();

	/* 2. 窗口满 → DSP + CAN */
	if (s_app.buf_count >= ADXL345_WINDOW_SIZE)
		process_window();

	/* 3. 定时心跳 */
	send_heartbeat();

	/* 4. 评估健康状态 */
	update_health();

	/*
	 * 5. IWDG喂狗 — CRITICAL状态停止喂狗,
	 *    让IWDG在3s后触发硬件复位.
	 */
	if (s_app.health != HEALTH_CRITICAL)
		IWDG_FEED();
}
