/**
 * @file app_main.c
 * @brief STM32F407 Enterprise Application V4.0 (Motor Control + PID + Fault)
 *
 * V4.0 特性:
 *   - 20Hz主循环 (50ms): PID控制 + 故障检测 + UART命令接收
 *   - 1Hz业务: DS18B20 + NTC + 协议发送 + 日志
 *   - PID闭环速度控制 (增量式, 积分分离, anti-windup)
 *   - 故障保护 (过流/过温/欠压/堵转, 自动停机)
 *   - ESP32 UART4远程电机控制 (启停/调速/PID/查询)
 *   - GUI触屏电机控制 (Phase 1)
 */

#include "main.h"
#include "cmsis_os.h"
#include "usart.h"
#include "gpio.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "modules.h"
#include "protocol/protocol.h"
#include "../bsp/ds18b20/bsp_ds18b20.h"
#include "../bsp/ntc/bsp_ntc.h"
#include "../bsp/motor/bsp_motor.h"
#include "../bsp/motor/bsp_motor_pid.h"
#include "../bsp/motor/bsp_motor_fault.h"
#ifdef USE_GUI
#include "gui/gui_app.h"
#endif

/* ==================== 应用配置 ==================== */

#define APP_UPDATE_INTERVAL_MS   50    /* 20Hz */
#define APP_SLOW_TICKS           20    /* 1Hz = 20 ticks */
#define PID_MAX_TARGET_RPM       3000
#define MOTOR_STATUS_REPORT_MS   2000  /* 2s上报一次 */
#define UART4_TX_TIMEOUT_MS      50

/* ==================== 模块内部状态 ==================== */

static uint32_t g_loop_count;
static uint8_t g_proto_seq;
static struct pid_state s_pid;
static uint32_t g_ticks_since_slow = APP_SLOW_TICKS;
static uint32_t s_last_status_ms;

/* UART4帧接收状态机 */
enum rx_state {
	RX_WAIT_H0, RX_WAIT_H1, RX_WAIT_LEN_H, RX_WAIT_LEN_L,
	RX_WAIT_DEV, RX_WAIT_CMD, RX_WAIT_SEQ, RX_WAIT_DATA,
	RX_WAIT_CRC_H, RX_WAIT_CRC_L, RX_WAIT_TAIL,
};
static enum rx_state s_rx_state = RX_WAIT_H0;
static uint8_t s_rx_buf[64];
static uint16_t s_rx_idx;
static uint16_t s_rx_data_len;
static uint8_t s_rx_cmd;
static uint8_t s_rx_seq;

/* ==================== UART4帧发送 ==================== */

static int send_frame_to_esp32(const uint8_t *frame, int len)
{
	HAL_StatusTypeDef status;

	if (!frame || len <= 0)
		return -ERR_INVALID_PARAM;

	status = HAL_UART_Transmit(&huart4, frame, (uint16_t)len,
				   UART4_TX_TIMEOUT_MS);
	if (status == HAL_OK)
		return len;

	pr_warn_with_tag("TX", "UART4 TX failed: status=%d\n", status);
	return -ERR_COMM_TX_FAIL;
}

/*
 * ==================== UART4 帧接收 ====================
 */

static bool handle_uart4_rx_byte(uint8_t byte)
{
	switch (s_rx_state) {
	case RX_WAIT_H0:
		if (byte == 0xAA) {
			s_rx_state = RX_WAIT_H1;
			s_rx_buf[0] = byte;
		}
		break;
	case RX_WAIT_H1:
		if (byte == 0x55) {
			s_rx_state = RX_WAIT_LEN_H;
			s_rx_buf[1] = byte;
		} else {
			s_rx_state = RX_WAIT_H0;
		}
		break;
	case RX_WAIT_LEN_H:
		s_rx_data_len = (uint16_t)byte << 8;
		s_rx_state = RX_WAIT_LEN_L;
		break;
	case RX_WAIT_LEN_L:
		s_rx_data_len |= byte;
		s_rx_state = RX_WAIT_DEV;
		break;
	case RX_WAIT_DEV:
		s_rx_state = RX_WAIT_CMD;
		break;
	case RX_WAIT_CMD:
		s_rx_cmd = byte;
		s_rx_state = RX_WAIT_SEQ;
		break;
	case RX_WAIT_SEQ:
		s_rx_seq = byte;
		s_rx_idx = 0;
		s_rx_state = (s_rx_data_len > 0)
			? RX_WAIT_DATA : RX_WAIT_CRC_H;
		break;
	case RX_WAIT_DATA:
		s_rx_buf[s_rx_idx++] = byte;
		if (s_rx_idx >= s_rx_data_len)
			s_rx_state = RX_WAIT_CRC_H;
		break;
	case RX_WAIT_CRC_H:
		s_rx_state = RX_WAIT_CRC_L;
		break;
	case RX_WAIT_CRC_L:
		s_rx_state = RX_WAIT_TAIL;
		break;
	case RX_WAIT_TAIL:
		s_rx_state = RX_WAIT_H0;
		if (byte == 0x0D)
			return true;
		break;
	}
	return false;
}

static void execute_motor_command(uint8_t cmd, const uint8_t *data,
				  uint16_t len)
{
	uint8_t subcmd;
	int32_t value;

	if (cmd == PROTO_CMD_MOTOR_QUERY) {
		s_last_status_ms = 0;
		return;
	}

	if (cmd != PROTO_CMD_MOTOR_CONTROL)
		return;

	if (proto_parse_motor_control(data, len, &subcmd, &value) != 0)
		return;

	switch (subcmd) {
	case MOTOR_SUBCMD_STOP:
		bsp_motor_stop();
		pid_set_enabled(&s_pid, false);
		pr_info_with_tag("MOTOR-CMD", "ESP32: STOP\n");
		break;
	case MOTOR_SUBCMD_START: {
		enum motor_state st;
		bsp_motor_get_state(&st);
		if (st == MOTOR_STATE_IDLE) {
			bsp_motor_start();
			if (value > 0 && value <= BSP_MOTOR_PWM_MAX_DUTY)
				bsp_motor_set_duty(value);
			pr_info_with_tag("MOTOR-CMD",
				"ESP32: START duty=%ld\n", (long)value);
		}
		break;
	}
	case MOTOR_SUBCMD_SET_DUTY:
		bsp_motor_set_duty(value);
		break;
	case MOTOR_SUBCMD_SET_SPEED:
		pid_set_enabled(&s_pid, true);
		pid_set_target(&s_pid, value,
			(int8_t)bsp_motor_get_direction());
		s_pid.cfg.output_max = BSP_MOTOR_PWM_MAX_DUTY;
		pr_info_with_tag("MOTOR-CMD",
			"ESP32: SET_SPEED target=%ld RPM\n", (long)value);
		break;
	case MOTOR_SUBCMD_SET_DIRECTION:
		bsp_motor_set_direction(value > 0
			? BSP_MOTOR_DIR_CW : BSP_MOTOR_DIR_CCW);
		break;
	case MOTOR_SUBCMD_PID_ENABLE:
		pid_set_enabled(&s_pid, (bool)value);
		break;
	case MOTOR_SUBCMD_EMERGENCY_STOP:
		bsp_motor_emergency_stop();
		pr_error_with_tag("MOTOR-CMD", "ESP32: EMERGENCY STOP\n");
		break;
	case MOTOR_SUBCMD_CLEAR_FAULT:
		bsp_motor_fault_clear();
		break;
	}
}

static void poll_uart4_and_dispatch(void)
{
	uint8_t byte;

	while (__HAL_UART_GET_FLAG(&huart4, UART_FLAG_RXNE)) {
		byte = (uint8_t)(huart4.Instance->DR & 0xFF);
		if (handle_uart4_rx_byte(byte))
			execute_motor_command(s_rx_cmd,
				s_rx_buf, s_rx_data_len);
	}
}

/*
 * ==================== 主循环 ====================
 */

static void main_loop_enterprise(void)
{
	enum motor_state state;
	int32_t rpm, duty, cur_ma, bus_mv, temp_dc;
	int dir;
	bool is_running;

	g_loop_count++;
	g_ticks_since_slow++;

	/*
	 * 1Hz慢速业务 (每20 tick = 1000ms)
	 */
	if (g_ticks_since_slow >= APP_SLOW_TICKS) {
		float temp_c, humidity_rh;
		int temp_result, tx_result;
		int temp_int, temp_frac;

		/*
		 * DS18B20温度
		 */
		temp_result = bsp_ds18b20_read_temp(&temp_c);
		if (temp_result >= 0) {
			temp_int = (int)temp_c;
			temp_frac = (int)((temp_c - (float)temp_int)
				* 10.0f + 0.5f);
			if (temp_frac < 0)
				temp_frac = -temp_frac;

			pr_info_with_tag("DS18B20", "Loop=%lu T=%d.%dC\n",
				(unsigned long)g_loop_count,
				temp_int, temp_frac);

			/*
			 * 发送温湿度协议帧至ESP32
			 */
			{
				struct proto_temp_humidity_data data;
				uint8_t frame[PROTO_FRAME_MAX_SIZE];
				int frame_len;

				data.temp_c = temp_c;
				data.humidity_rh = 55.0f;
				data.timestamp_ms = proto_timestamp_get();
				data.sensor_type = PROTO_SENSOR_TYPE_DS18B20;
				data.sensor_status = PROTO_SENSOR_STATUS_NORMAL;
				data.raw_adc_value = 0;

				frame_len = proto_build_temp_humidity_frame(
					frame, &data, g_proto_seq++);
				if (frame_len > 0)
					send_frame_to_esp32(frame, frame_len);
			}

#ifdef USE_GUI
			gui_app_update_sensor_data(temp_c, 55.0f);
#endif
		}

		/*
		 * NTC温度
		 */
		{
			float ntc_temp;
			int ntc_ret = bsp_ntc_read_temp(&ntc_temp);
			if (ntc_ret == 0) {
				int ntc_int = (int)ntc_temp;
				int ntc_frac = (int)((ntc_temp
					- (float)ntc_int) * 10.0f + 0.5f);
				if (ntc_frac < 0)
					ntc_frac = -ntc_frac;
				pr_info_with_tag("NTC", "T=%d.%dC\n",
					ntc_int, ntc_frac);
#ifdef USE_GUI
				gui_app_update_ntc_data(ntc_temp);
#endif
			}
		}

		g_ticks_since_slow = 0;
	}

	/*
	 * 20Hz快速业务 (每50ms)
	 */

	/* Step A: 消费GUI电机命令 */
#ifdef USE_GUI
	gui_app_consume_motor_cmds();
#endif

	/* Step B: 编码器速度更新 */
	bsp_motor_encoder_update_speed();
	bsp_motor_encoder_get_speed(&rpm);
	bsp_motor_get_state(&state);
	dir = bsp_motor_get_direction();
	is_running = (state == MOTOR_STATE_RUNNING);

	/* Step C: 同步PID使能状态 */
	if (motor_cmd_shared.pid_active != s_pid.enabled)
		pid_set_enabled(&s_pid, motor_cmd_shared.pid_active);

	/* Step D: PID控制 */
	if (s_pid.enabled && is_running) {
		int32_t target = (int32_t)motor_cmd_shared.slider_percent
			* PID_MAX_TARGET_RPM / 100;
		pid_set_target(&s_pid, target, (int8_t)dir);
		s_pid.cfg.output_max = BSP_MOTOR_PWM_MAX_DUTY;
		duty = pid_step(&s_pid, rpm);
		bsp_motor_set_duty(duty);
	} else {
		duty = bsp_motor_get_duty();
	}

	/* Step E: 故障检测 */
	bsp_motor_adc_read_current_calibrated(&cur_ma);
	bsp_motor_adc_read_bus_voltage(&bus_mv);
	bsp_motor_adc_read_temperature(&temp_dc);
	bsp_motor_fault_check(duty, rpm, cur_ma, bus_mv, temp_dc,
		is_running);

	/* Step F: UART4命令接收 (ESP32 → STM32) */
	poll_uart4_and_dispatch();

	/* Step G: 电机状态上报 (STM32 → ESP32, 每2s) */
	{
		uint32_t now = HAL_GetTick();

		if (s_last_status_ms == 0 ||
		    (now - s_last_status_ms) >= MOTOR_STATUS_REPORT_MS) {
			uint8_t frame[PROTO_FRAME_MAX_SIZE];
			int frame_len;
			int fault = bsp_motor_fault_get();

			s_last_status_ms = now;
			frame_len = proto_build_motor_status_frame(
				frame, g_proto_seq++,
				rpm, cur_ma, bus_mv, temp_dc,
				(uint8_t)state, (uint8_t)fault,
				duty, (int8_t)dir,
				s_pid.enabled);
			if (frame_len > 0)
				send_frame_to_esp32(frame, frame_len);
		}
	}

	/* Step H: GUI电机状态更新 */
#ifdef USE_GUI
	gui_app_update_motor_status(
		rpm < 0 ? (uint32_t)(-rpm) : (uint32_t)rpm,
		is_running, (int8_t)dir,
		s_pid.enabled, s_pid.target);
#endif

	/* Step I: 1Hz日志 */
	if (g_ticks_since_slow == 0) {
		pr_info_with_tag("MOTOR",
			"S=%d Duty=%ld/%d Dir=%s PID=%s "
			"F=%s I=%ldmA V=%ldmV "
			"T=%ld.%ldC RPM=%ld\n",
			(int)state, (long)duty,
			BSP_MOTOR_PWM_MAX_DUTY,
			dir > 0 ? "CW" : "CCW",
			s_pid.enabled ? "ON" : "OFF",
			bsp_motor_fault_get_name(bsp_motor_fault_get()),
			(long)cur_ma, (long)bus_mv,
			(long)temp_dc / 10,
			(long)(temp_dc < 0 ? -temp_dc : temp_dc) % 10,
			(long)rpm);
	}
}

/* ==================== FreeRTOS任务入口 ==================== */

void app_dht11_task_entry(void *argument)
{
	(void)argument;

	pr_info_with_tag("APP", "========================================\n");
	pr_info_with_tag("APP", " Enterprise Task V4.0 (Motor+PID+Fault)\n");
	pr_info_with_tag("APP", "========================================\n\n");

	/*
	 * 初始化DS18B20传感器
	 */
	{
		int ret = bsp_ds18b20_init();
		if (ret < 0) {
			pr_error_with_tag("APP",
				"FATAL: DS18B20 init failed: %d\n", ret);
		}
	}

	/*
	 * 初始化NTC传感器
	 */
	{
		int ret = bsp_ntc_init();
		if (ret < 0) {
			pr_error_with_tag("APP",
				"FATAL: NTC init failed: %d\n", ret);
		}
	}

	/*
	 * 初始化PID速度控制器 (默认参数)
	 */
	pid_init(&s_pid, NULL);
	{
		int kp_i = (int)s_pid.cfg.kp;
		int kp_f = (int)(s_pid.cfg.kp * 10.0f) % 10;
		int ki_i = (int)s_pid.cfg.ki;
		int ki_f = (int)(s_pid.cfg.ki * 10.0f) % 10;
		int kd_i = (int)s_pid.cfg.kd;
		int kd_f = (int)(s_pid.cfg.kd * 10.0f) % 10;

		pr_info_with_tag("APP", "PID: Kp=%d.%d Ki=%d.%d Kd=%d.%d "
			"Imax=%ld\n",
			kp_i, kp_f, ki_i, ki_f, kd_i, kd_f,
			(long)s_pid.cfg.integral_max);
	}

	while (1) {
		main_loop_enterprise();
		osDelay(APP_UPDATE_INTERVAL_MS);
	}
}
