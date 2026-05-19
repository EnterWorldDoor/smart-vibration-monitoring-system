/**
 * @file bsp_motor_adc.c
 * @brief 电机驱动板模拟信号采集 — ADC1 三通道
 *
 * 通道映射:
 *   ADC1_IN8 (PB0) → PM1_AMPU  → CURT (电流, 经TP2412放大)
 *   ADC1_IN9 (PB1) → PM1_VBUS  → VBUS (母线电压分压)
 *   ADC1_IN0 (PA0) → PM1_VTEMP → VTEMP (NTC温度)
 *
 * 信号公式 (来自PD6010D手册):
 *   电流: Iout = 6 × 0.02Ω × I + 1.27V  →  I = (V_adc - 1.27) / 0.12  [A]
 *   电压: VBUS = POWER / 25             →  POWER = V_adc × 25  [V]
 *   温度: NTC 10K@25°C B=3950, VTEMP = 3.3V × 4.7K/(Rt+4.7K)
 *         → Rt = 4.7K × (3.3/VTEMP - 1)
 *         → T_K = 1 / (1/298.15 + ln(Rt/10000)/3950)
 */

#include "adc.h"
#include "bsp_motor.h"
#include <math.h>

/* ADC参考电压 (与DMF407板载3.3V一致) */
#define ADC_VREF_MV       3300
#define ADC_RESOLUTION    4096

/* 电流公式常量 */
#define CURRENT_SHUNT_OHM    0.02f   /* 采样电阻 20mR */
#define CURRENT_GAIN         6.0f    /* TP2412差分放大倍数 */
#define CURRENT_OFFSET_V     1.27f   /* 1.27V基准偏置 */

/* 母线电压分压比: POWER / (12K+12K+1K) * 1K = POWER / 25 */
#define BUS_RATIO_NUMERATOR   25.0f

/* NTC参数 */
#define NTC_R_REF_OHM        10000.0f  /* 25°C时电阻值 */
#define NTC_R_FIXED_OHM       4700.0f  /* 固定分压电阻 */
#define NTC_B_VALUE           3950.0f  /* B值 */
#define NTC_T0_KELVIN          298.15f /* 参考温度 25°C */

/*
 * 将ADC原始值转换为电压 (mV)
 */
static float adc_raw_to_mv(uint32_t raw)
{
	return (float)raw * (float)ADC_VREF_MV / (float)ADC_RESOLUTION;
}

/*
 * 读取指定通道的ADC原始值 (单次转换, 轮询)
 */
static int adc_read_channel(uint32_t channel, uint32_t *raw)
{
	ADC_ChannelConfTypeDef cfg = {0};
	HAL_StatusTypeDef status;

	cfg.Channel      = channel;
	cfg.Rank         = 1;
	cfg.SamplingTime = ADC_SAMPLETIME_480CYCLES; /* 480 cycles = ~12.5us */

	/* 先停止之前的转换 (如果有) */
	HAL_ADC_Stop(&hadc1);

	/* 切换通道 */
	if (HAL_ADC_ConfigChannel(&hadc1, &cfg) != HAL_OK)
		return -1;

	/* 启动转换 */
	if (HAL_ADC_Start(&hadc1) != HAL_OK)
		return -1;

	/* 等待转换完成 (超时10ms) */
	status = HAL_ADC_PollForConversion(&hadc1, 10);
	if (status != HAL_OK) {
		HAL_ADC_Stop(&hadc1);
		return -1;
	}

	*raw = HAL_ADC_GetValue(&hadc1);
	HAL_ADC_Stop(&hadc1);
	return 0;
}

/* ==================== 公开API ==================== */

int bsp_motor_adc_init(void)
{
	/* ADC1已在CubeMX中初始化, 此处仅做验证 */
	if (hadc1.Instance != ADC1)
		return -1;

	return 0;
}

/*
 * 读取电机电流 (mA)
 *
 * 公式: Iout = GAIN × R_shunt × I + V_offset
 *        I = (Iout - V_offset) / (GAIN × R_shunt)
 *
 * 校准: 电机停止时电流应为0mA。首次使用时建议校准偏移:
 *   offset = 电机静止时读到的 raw ADC 值
 *   后续 I_mA = (V_adc - V_offset_measured) / (GAIN × R_shunt) × 1000
 */
int bsp_motor_adc_read_current(int32_t *current_ma)
{
	uint32_t raw;
	float v_mv, current_a;

	if (!current_ma)
		return -1;

	if (adc_read_channel(ADC_CHANNEL_8, &raw) != 0)
		return -1;

	v_mv = adc_raw_to_mv(raw);

	/* I = (V_adc - 1.27V) / (6 × 0.02Ω) = (V_adc - 1.27) / 0.12 */
	current_a = (v_mv / 1000.0f - CURRENT_OFFSET_V) /
	            (CURRENT_GAIN * CURRENT_SHUNT_OHM);

	*current_ma = (int32_t)(current_a * 1000.0f);
	return 0;
}

/*
 * 读取母线电压 (mV)
 *
 * 公式: VBUS = POWER / 25 → POWER = VBUS × 25
 */
int bsp_motor_adc_read_bus_voltage(int32_t *voltage_mv)
{
	uint32_t raw;
	float vbus_v, power_v;

	if (!voltage_mv)
		return -1;

	if (adc_read_channel(ADC_CHANNEL_9, &raw) != 0)
		return -1;

	vbus_v = adc_raw_to_mv(raw) / 1000.0f;
	power_v = vbus_v * BUS_RATIO_NUMERATOR;

	*voltage_mv = (int32_t)(power_v * 1000.0f);
	return 0;
}

/*
 * 读取驱动板温度 (0.1°C)
 *
 * NTC 10K B=3950:
 *   Rt = R_fixed × (Vref / V_temp - 1)
 *   T_K = 1 / (1/T_0 + ln(Rt/R_ref) / B)
 *   T_C = T_K - 273.15
 *
 * 返回值单位为 0.1°C (例如 255 = 25.5°C)
 */
int bsp_motor_adc_read_temperature(int32_t *temp_decic)
{
	uint32_t raw;
	float vtemp, rt, t_kelvin, t_celsius;

	if (!temp_decic)
		return -1;

	if (adc_read_channel(ADC_CHANNEL_0, &raw) != 0)
		return -1;

	vtemp = adc_raw_to_mv(raw) / 1000.0f;

	/* 防除零: VTEMP不能为0 */
	if (vtemp <= 0.01f) {
		*temp_decic = 0;
		return -1;
	}

	rt = NTC_R_FIXED_OHM * ((float)ADC_VREF_MV / 1000.0f / vtemp - 1.0f);

	/* 防除零和对数输入异常 */
	if (rt <= 0.0f) {
		*temp_decic = 0;
		return -1;
	}

	t_kelvin = 1.0f / (1.0f / NTC_T0_KELVIN +
	                    logf(rt / NTC_R_REF_OHM) / NTC_B_VALUE);
	t_celsius = t_kelvin - 273.15f;

	*temp_decic = (int32_t)(t_celsius * 10.0f);
	return 0;
}

/*
 * 校准电流偏移
 *
 * 电机完全静止时(无PWM, H桥关断)调用此函数。
 * 记录此时的ADC原始值作为"零电流"参考。
 *
 * 后续 bsp_motor_adc_read_current_calibrated() 会自动减去此偏移。
 */
static int32_t s_current_offset_raw;

int bsp_motor_adc_calibrate_current(void)
{
	uint32_t raw;

	if (adc_read_channel(ADC_CHANNEL_8, &raw) != 0)
		return -1;

	s_current_offset_raw = (int32_t)raw;
	return 0;
}

int bsp_motor_adc_read_current_calibrated(int32_t *current_ma)
{
	uint32_t raw;
	float v_mv, v_offset_mv;
	float current_a;

	if (!current_ma)
		return -1;

	if (adc_read_channel(ADC_CHANNEL_8, &raw) != 0)
		return -1;

	v_mv = adc_raw_to_mv(raw);
	v_offset_mv = adc_raw_to_mv((uint32_t)s_current_offset_raw);

	current_a = ((v_mv - v_offset_mv) / 1000.0f) /
	            (CURRENT_GAIN * CURRENT_SHUNT_OHM);

	*current_ma = (int32_t)(current_a * 1000.0f);
	return 0;
}
