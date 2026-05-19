/**
 * @file bsp_motor_fault.c
 * @brief 电机故障检测实现 — 确认窗口防误触, 自动恢复状态机
 */

#include "bsp_motor_fault.h"
#include "bsp_motor.h"
#include "system_log/system_log.h"
#include "main.h"  /* HAL_GetTick */

/* 故障确认: 持续超过阈值时间才触发, 避免瞬时尖峰误报 */
struct fault_confirm {
	int      code;         /* 待确认的故障码 */
	uint32_t start_ms;     /* 首次检测到的时间戳 */
	uint32_t duration_ms;  /* 需要持续的时间 */
};

static struct {
	int      active_fault;   /* 当前故障码 (0=无故障) */
	uint32_t fault_time_ms;  /* 故障触发时刻 */
	int      retry_count;    /* 已尝试恢复次数 */
	uint32_t fault_total;    /* 累计故障次数 */

	struct fault_confirm confirm;
} s_fault = {0};

/* 开始确认一个潜在故障 */
static void confirm_start(int code, uint32_t duration_ms)
{
	if (s_fault.confirm.code != code) {
		s_fault.confirm.code       = code;
		s_fault.confirm.start_ms   = HAL_GetTick();
		s_fault.confirm.duration_ms = duration_ms;
	}
}

/* 检查确认窗口: 如果持续够久则确认故障, 如果条件消失则取消 */
static bool confirm_check(void)
{
	struct fault_confirm *c = &s_fault.confirm;
	if (c->code == MOTOR_FAULT_NONE)
		return false;

	if ((HAL_GetTick() - c->start_ms) >= c->duration_ms)
		return true;

	return false;
}

/* 取消确认 (故障条件消失) */
static void confirm_cancel(void)
{
	s_fault.confirm.code = MOTOR_FAULT_NONE;
}

/*
 * 故障触发: 记录日志, 紧急停止电机, 进入FAULT状态
 */
static void fault_trigger(int code)
{
	s_fault.active_fault   = code;
	s_fault.fault_time_ms  = HAL_GetTick();
	s_fault.confirm.code   = MOTOR_FAULT_NONE;
	s_fault.fault_total++;

	pr_error_with_tag("MOTOR-FAULT",
		"FAULT TRIGGERED: %s (total=%lu)\n",
		bsp_motor_fault_get_name(code),
		(unsigned long)s_fault.fault_total);

	bsp_motor_emergency_stop();
}

int bsp_motor_fault_check(int32_t duty, int32_t rpm,
			  int32_t current_ma, int32_t bus_mv,
			  int32_t temp_dc, bool is_running)
{
	int candidate = MOTOR_FAULT_NONE;
	int abs_rpm;

	/* 已有活跃故障, 不重复检测 */
	if (s_fault.active_fault != MOTOR_FAULT_NONE)
		return s_fault.active_fault;

	if (rpm < 0)
		abs_rpm = -rpm;
	else
		abs_rpm = rpm;

	/*
	 * 检测各类故障条件
	 */

	/* 过流: >8000mA (8A, 额定10A留20%余量) */
	if (current_ma > BSP_MOTOR_CURRENT_MAX_MA)
		candidate = MOTOR_FAULT_OVERCURRENT;

	/* 过温: >80°C (temp_dc单位0.1°C) */
	if (temp_dc > BSP_MOTOR_TEMP_MAX_C)
		candidate = MOTOR_FAULT_OVERTEMP;

	/* 欠压: <10V (仅运行时检测, 避免静止时低电压误报) */
	if (is_running && bus_mv < BSP_MOTOR_VOLTAGE_MIN_MV)
		candidate = MOTOR_FAULT_UNDERVOLT;

	/* 堵转: duty>0 且 |rpm|<30 持续2秒 */
	if (is_running && duty > 0 && abs_rpm < 30)
		candidate = MOTOR_FAULT_STALL;

	/*
	 * 故障确认窗口逻辑
	 */
	if (candidate != MOTOR_FAULT_NONE) {
		uint32_t dur;

		switch (candidate) {
		case MOTOR_FAULT_OVERCURRENT:
			dur = FAULT_CONFIRM_OVERCURRENT_MS;
			break;
		case MOTOR_FAULT_OVERTEMP:
			dur = FAULT_CONFIRM_OVERTEMP_MS;
			break;
		case MOTOR_FAULT_STALL:
			dur = FAULT_CONFIRM_STALL_MS;
			break;
		default:
			dur = 0;  /* 欠压: 瞬时触发 */
			break;
		}

		if (dur > 0) {
			confirm_start(candidate, dur);
			if (confirm_check())
				fault_trigger(candidate);
		} else {
			fault_trigger(candidate);
		}
	} else {
		confirm_cancel();
	}

	return s_fault.active_fault;
}

int bsp_motor_fault_get(void)
{
	return s_fault.active_fault;
}

const char *bsp_motor_fault_get_name(int fault)
{
	switch (fault) {
	case MOTOR_FAULT_NONE:
		return "NONE";
	case MOTOR_FAULT_OVERCURRENT:
		return "OVERCURRENT";
	case MOTOR_FAULT_OVERTEMP:
		return "OVERTEMP";
	case MOTOR_FAULT_UNDERVOLT:
		return "UNDERVOLTAGE";
	case MOTOR_FAULT_STALL:
		return "STALL";
	default:
		return "UNKNOWN";
	}
}

bool bsp_motor_fault_can_recover(void)
{
	if (s_fault.active_fault == MOTOR_FAULT_NONE)
		return false;

	/* 等待冷却时间 */
	if ((HAL_GetTick() - s_fault.fault_time_ms) < FAULT_RECOVER_WAIT_MS)
		return false;

	/* 超过最大重试次数 */
	if (s_fault.retry_count >= FAULT_MAX_RETRIES)
		return false;

	return true;
}

void bsp_motor_fault_clear(void)
{
	s_fault.active_fault  = MOTOR_FAULT_NONE;
	s_fault.fault_time_ms = 0;
	s_fault.retry_count++;

	pr_info_with_tag("MOTOR-FAULT",
		"Fault cleared (retry %d/%d)\n",
		s_fault.retry_count, FAULT_MAX_RETRIES);
}

uint32_t bsp_motor_fault_count(void)
{
	return s_fault.fault_total;
}
