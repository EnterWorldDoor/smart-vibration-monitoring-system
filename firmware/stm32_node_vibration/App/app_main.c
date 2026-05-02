/**
 * @file app_main.c
 * @brief STM32F407 Enterprise Application V2.0 (FreeRTOS Task)
 *
 * V2.0 企业级应用特性:
 *   - ✅ 模拟温湿度传感器 (Random Walk算法)
 *   - ✅ 完整ESP32协议栈 (CRC16-MODBUS, 16字节数据格式)
 *   - ✅ FreeRTOS任务调度 (1秒周期)
 *   - ✅ Linux内核编码风格
 *
 * 任务架构:
 *   app_enterprise_task:
 *     ├── simulator_update()      // 更新模拟传感器数据
 *     ├── send_temp_to_esp32()    // 发送到ESP32 (UART4)
 *     └── gui_app_update()        // 更新LVGL界面显示 (可选)
 */

/* ==================== 头文件 ==================== */

#include "main.h"
#include "cmsis_os.h"
#include "usart.h"
#include "gpio.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* 项目模块 */
#include "system_log/system_log.h"
#include "uart_log/uart_log.h"
#include "global_error/global_error.h"

/* GUI模块 (可选，如果使用LVGL) */
/* #include "gui/gui_app.h" */

/* ==================== 协议常量 (与ESP32 protocol.h一致) ==================== */

#define PROTO_HEADER              0xAA55
#define PROTO_TAIL                0x0D
#define DEVICE_ID                0x01
#define CMD_TEMP_HUMIDITY_DATA    0x04

/* ==================== 模拟传感器配置 ==================== */

#define SIM_TEMP_MIN             20.0f    /* 最低温度 °C */
#define SIM_TEMP_MAX             35.0f    /* 最高温度 °C */
#define SIM_HUMIDITY_MIN         40.0f    /* 最低湿度 %RH */
#define SIM_HUMIDITY_MAX         80.0f    /* 最高湿度 %RH */
#define SIM_UPDATE_INTERVAL_MS   1000     /* 数据更新间隔 ms */
#define SIM_VARIATION            0.5f     /* 单次最大变化量 */

/* ==================== 模块内部状态 ==================== */

static uint32_t g_loop_count = 0;

/*
 * ==================== CRC16-MODBUS 校验 (与ESP32完全一致) ====================
 */
static uint16_t crc16_modbus(const uint8_t *data, uint16_t len)
{
        uint16_t crc = 0xFFFF;
        uint16_t i, j;

        for (i = 0; i < len; i++) {
                crc ^= data[i];
                for (j = 0; j < 8; j++) {
                        if (crc & 0x0001)
                                crc = (crc >> 1) ^ 0xA001;
                        else
                                crc >>= 1;
                }
        }
        return crc;
}

/*
 * ==================== 模拟温湿度数据生成器 ====================
 *
 * 算法: Random Walk (随机游走)
 *   - 温度和湿度在合理范围内缓慢变化
 *   - 变化幅度受 SIM_VARIATION 限制
 *   - 湿度与温度负相关 (温度↑ → 湿度↓)
 *
 * 真实性增强:
 *   - 添加微小噪声 (±0.1°C / ±1%RH)
 *   - 边界反弹 (到达边界后反向变化)
 */
static struct {
        float current_temp;
        float current_humidity;
        float temp_direction;   /* +1.0 or -1.0 */
        float hum_direction;
        bool initialized;
} sim_sensor_state = {0};

static void simulator_init(void)
{
        sim_sensor_state.current_temp = 25.0f;   /* 初始25°C */
        sim_sensor_state.current_humidity = 60.0f; /* 初始60%RH */
        sim_sensor_state.temp_direction = 1.0f;
        sim_sensor_state.hum_direction = -1.0f;
        sim_sensor_state.initialized = true;

        pr_info_with_tag("SIM", "Temperature/humidity simulator initialized\n");
        pr_info_with_tag("SIM", "  Range: %.1f~%.1f°C / %.0f~%.0f%%RH\n",
                         SIM_TEMP_MIN, SIM_TEMP_MAX,
                         SIM_HUMIDITY_MIN, SIM_HUMIDITY_MAX);
}

static void simulator_update(void)
{
        if (!sim_sensor_state.initialized) {
                simulator_init();
        }

        /*
         * 温度随机游走
         */
        float temp_delta = ((float)(rand() % 100) / 100.0f) * SIM_VARIATION;
        temp_delta *= sim_sensor_state.temp_direction;

        sim_sensor_state.current_temp += temp_delta;

        /* 边界检测与反弹 */
        if (sim_sensor_state.current_temp >= SIM_TEMP_MAX) {
                sim_sensor_state.current_temp = SIM_TEMP_MAX;
                sim_sensor_state.temp_direction = -1.0f;
        } else if (sim_sensor_state.current_temp <= SIM_TEMP_MIN) {
                sim_sensor_state.current_temp = SIM_TEMP_MIN;
                sim_sensor_state.temp_direction = 1.0f;
        }

        /*
         * 湿度随机游走 (与温度负相关)
         */
        float hum_delta = ((float)(rand() % 100) / 100.0f) * SIM_VARIATION;
        hum_delta *= sim_sensor_state.hum_direction;

        /* 温湿度耦合: 温度上升时湿度下降更快 */
        if (sim_sensor_state.temp_direction > 0) {
                hum_delta *= 1.2f;
        }

        sim_sensor_state.current_humidity += hum_delta;

        /* 边界检测与反弹 */
        if (sim_sensor_state.current_humidity >= SIM_HUMIDITY_MAX) {
                sim_sensor_state.current_humidity = SIM_HUMIDITY_MAX;
                sim_sensor_state.hum_direction = -1.0f;
        } else if (sim_sensor_state.current_humidity <= SIM_HUMIDITY_MIN) {
                sim_sensor_state.current_humidity = SIM_HUMIDITY_MIN;
                sim_sensor_state.hum_direction = 1.0f;
        }

        /*
         * 添加测量噪声 (模拟真实传感器精度)
         */
        float noise_temp = ((float)(rand() % 20 - 10) / 100.0f);  /* ±0.1°C */
        float noise_hum = (float)(rand() % 3 - 1);               /* ±1%RH */

        sim_sensor_state.current_temp += noise_temp;
        sim_sensor_state.current_humidity += noise_hum;
}

static void simulator_get_data(float *temp_c, float *humidity_rh)
{
        if (temp_c)
                *temp_c = sim_sensor_state.current_temp;
        if (humidity_rh)
                *humidity_rh = sim_sensor_state.current_humidity;
}

/*
 * ==================== ESP32 协议帧构建与发送 ====================
 *
 * 帧格式 (与ESP32 protocol.c build_frame完全一致):
 *   [AA 55] [LEN_H LEN_L] [DEV_ID] [CMD] [SEQ] [DATA...] [CRC_L CRC_H] [0D]
 *
 * 温湿度数据载荷 (16字节):
 *   [0-3]   temperature_c     (float, 4 bytes)
 *   [4-7]   humidity_rh       (float, 4 bytes)
 *   [8-11]  timestamp_stm32_ms (uint32_t, 4 bytes)
 *   [12]    sensor_type       (uint8_t, 1 byte)  = 0xFF (CUSTOM/SIMULATED)
 *   [13]    sensor_status     (uint8_t, 1 byte)  = 0x00 (NORMAL)
 *   [14-15] raw_adc_value      (int16_t, 2 bytes) = 0x0000
 */
static int send_temp_to_esp32(float temp_c, float humidity_rh)
{
        uint8_t frame[64];
        uint16_t idx = 0;
        static uint8_t seq = 0;
        uint16_t payload_len;
        uint16_t crc_val;

        /*
         * 构建数据载荷 (16字节)
         */
        uint8_t payload[16];
        uint16_t offset = 0;

        memcpy(&payload[offset], &temp_c, sizeof(float));
        offset += 4;

        memcpy(&payload[offset], &humidity_rh, sizeof(float));
        offset += 4;

        {
                uint32_t timestamp = HAL_GetTick();
                memcpy(&payload[offset], &timestamp, sizeof(uint32_t));
                offset += 4;
        }

        payload[offset++] = 0xFF;  /* sensor_type = CUSTOM (simulated) */
        payload[offset++] = 0x00;  /* sensor_status = NORMAL */

        {
                int16_t raw_adc = 0;
                memcpy(&payload[offset], &raw_adc, sizeof(int16_t));
                offset += 2;
        }

        payload_len = offset;  /* ⚠️ BUG修复: LEN=纯DATA长度(16), 不含DevID/CMD/SEQ */

        /*
         * ======== 构建完整帧 ========
         *
         * ⚠️ 【关键修复】帧格式对齐ESP32 protocol.c build_frame()!
         *
         * ESP32期望格式:
         *   [AA 55] [LEN_H LEN_L] [DEV] [CMD] [SEQ] [DATA...] [CRC_H CRC_L] [0D]
         *   LEN = 纯DATA载荷长度 (如温湿度数据 = 16)
         *   CRC范围: LEN(2B) + DEV(1B) + CMD(1B) + SEQ(1B) + DATA(len B)
         *           =字节索引[2]到[DATA末尾]
         *
         * 原始BUG:
         *   LEN = 3 + offset (=19, 包含DEV/CMD/SEQ)
         *   CRC从frame[4]开始 (即DEV处, 不含LEN字段)
         *   → ESP32解析时expected_len=19(多读了3字节CRC+TAIL)
         *   → ESP32 CRC计算始于LEN字段(5+expected_len=24字节)
         *   → 与STM32的CRC(19字节)完全不匹配 → 所有帧被丢弃!
         */
        frame[idx++] = (PROTO_HEADER >> 8) & 0xFF;  /* AA */
        frame[idx++] = PROTO_HEADER & 0xFF;         /* 55 */
        frame[idx++] = (payload_len >> 8) & 0xFF;   /* len_H (0x00) */
        frame[idx++] = payload_len & 0xFF;          /* len_L (0x10 = 16) */
        frame[idx++] = DEVICE_ID;
        frame[idx++] = CMD_TEMP_HUMIDITY_DATA;
        frame[idx++] = ++seq;
        memcpy(&frame[idx], payload, offset);        /* 16字节数据 */
        idx += offset;

        /*
         * CRC16计算范围 (与ESP32 build_frame完全一致):
         *   从 frame[2] (LEN_H) 到 frame[idx-1] (DATA末尾)
         *   共 2(LEN) + 1(DEV) + 1(CMD) + 1(SEQ) + 16(DATA) = 21 字节
         */
        crc_val = crc16_modbus(&frame[2], idx - 2);
        /*
         * ⚠️ 【关键修复】CRC字节序: ESP32协议期望 Big-Endian (高字节先)!
         *
         * ESP32 build_frame:   out_buf[] = crc >> 8, crc & 0xFF  (高→低)
         * ESP32状态机:          STATE_WAIT_CRC_H → recv_crc = byte << 8
         *                       STATE_WAIT_CRC_L → recv_crc |= byte
         *
         * 原始BUG: STM32发送 crc & 0xFF, crc >> 8 (低→高)
         *   → ESP32收到的CRC被字节交换 → 校验失败!
         */
        frame[idx++] = (crc_val >> 8) & 0xFF;   /* CRC_H first (Big-Endian) */
        frame[idx++] = crc_val & 0xFF;          /* CRC_L second */
        frame[idx++] = PROTO_TAIL;

        /*
         * 通过UART4发送给ESP32
         *
         * ⚠️ 【关键修复】使用较短的超时时间!
         *
         * 原始配置问题:
         *   - 超时时间=100ms,如果UART4硬件问题会长时间阻塞
         *   - 阻塞期间其他任务无法运行
         *   - 如果连续失败,系统看起来像"卡死"
         *
         * 修复方案:
         *   - 减少超时时间到50ms (足够正常传输)
         *   - 发送失败只记录警告,不阻塞系统
         *   - 下次循环会重试
         */
        HAL_StatusTypeDef status = HAL_UART_Transmit(&huart4, frame, idx, 50);  // 100ms→50ms

        if (status == HAL_OK) {
                return (int)idx;
        } else {
                pr_warn_with_tag("TX", "UART4 TX failed: status=%d (timeout=%dms, len=%dB)\n",
                                status, 50, idx);
                return -ERR_COMM_TX_FAIL;
        }
}

/*
 * ==================== 主业务逻辑 (Enterprise V2.0) ====================
 */
static void main_loop_enterprise(void)
{
        float temp_c, humidity_rh;
        int tx_result;

        g_loop_count++;

        /*
         * ======== 更新模拟传感器数据 ========
         */
        simulator_update();
        simulator_get_data(&temp_c, &humidity_rh);

        /*
         * ======== 日志输出 ========
         * ⚠️ 【修复】使用ASCII字符替代Unicode度数符号
         *
         * 原始问题:
         *   %5.1f°C 中的 ° (U+00B0) 是Unicode字符
         *   在某些串口终端(XCOM/SSCOM)中显示为乱码 "掳"
         *
         * 修复方案:
         *   使用纯ASCII: "C" 代替 "°C"
         */
        pr_info_with_tag("DATA",
                         "| #%04lu | %5.1f C | %5.1f %%RH | SIM |\n",
                         (unsigned long)g_loop_count,
                         temp_c, humidity_rh);

        /*
         * ======== 发送到ESP32 ========
         */
        tx_result = send_temp_to_esp32(temp_c, humidity_rh);

        if (tx_result > 0) {
                pr_debug_with_tag("TX",
                                  "UART4 TX OK: %dB | T=%.1f H=%.1f\n",
                                  tx_result, temp_c, humidity_rh);
        } else {
                pr_warn_with_tag("TX",
                                 "UART4 TX FAIL: err=%d\n",
                                 tx_result);
        }

        /*
         * ======== 周期性统计输出 ========
         */
        if (g_loop_count % 10 == 0) {
                pr_info_with_tag("STAT",
                                 "Loop=%lu | Simulator ACTIVE |\n",
                                 (unsigned long)g_loop_count);
        }

        /*
         * ======== 可选: 更新LVGL界面 ========
         * 如果GUI已初始化，更新温湿度显示
         */
        #ifdef USE_GUI
        gui_app_update_sensor_data(temp_c, humidity_rh);
        #endif
}

/* ==================== FreeRTOS任务入口 (企业级V2.0) ==================== */

/**
 * app_enterprise_task_entry - 企业级主任务入口函数
 *
 * 任务功能:
 *   1. 初始化模拟传感器模块
 *   2. 周期性生成温湿度数据 (Random Walk算法)
 *   3. 通过UART4发送给ESP32 (标准协议格式)
 *   4. 可选: 更新LVGL GUI界面
 *
 * 调度参数:
 *   - 栈大小: 4KB (足够协议栈和日志系统)
 *   - 优先级: Normal (osPriorityNormal)
 *   - 周期: 1000ms (SIM_UPDATE_INTERVAL_MS)
 */
void app_dht11_task_entry(void *argument)
{
        (void)argument;

        pr_info_with_tag("APP", "✅ [DEBUG] app_enterprise_task ENTRY - Task started successfully!\n");
        pr_info_with_tag("APP", "✅ [DEBUG] Task stack: 4096 bytes, Priority: Normal\n");
        pr_info_with_tag("APP", "+======================================+\n");
        pr_info_with_tag("APP", "|  STM32F407 Enterprise System V2.0       |\n");
        pr_info_with_tag("APP", "|  =================================   |\n");
        pr_info_with_tag("APP", "|  ✅ Mode   : Simulation (DHT11 Bypass)|\n");
        pr_info_with_tag("APP", "|  ✅ Protocol: ESP32 UART4 (CRC16)    |\n");
        pr_info_with_tag("APP", "|  ✅ GUI    : LVGL Tablet Style      |\n");
        pr_info_with_tag("APP", "|  ✅ RTOS   : FreeRTOS Task          |\n");
        pr_info_with_tag("APP", "+======================================+\n\n");

        /*
         * 初始化模拟传感器
         */
        simulator_init();

        pr_info_with_tag("APP", "Enterprise task started successfully\n");
        pr_info_with_tag("APP", "Update interval: %dms\n\n", SIM_UPDATE_INTERVAL_MS);

        /*
         * 主循环 (FreeRTOS任务)
         */
        while (1) {
                main_loop_enterprise();

                /*
                 * 使用osDelay释放CPU给其他任务
                 * 比HAL_Delay更适合多任务环境
                 */
                osDelay(SIM_UPDATE_INTERVAL_MS);
        }
}
