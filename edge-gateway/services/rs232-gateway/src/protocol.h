/**
 * @file protocol.h
 * @brief RS232 Gateway — 协议常量、结构体、API 声明
 *
 * 从 firmware/stm32_node_vibration/Modules/protocol/protocol.h 提取,
 * 移除所有 STM32 HAL / FreeRTOS 依赖。
 *
 * 帧格式 (与 ESP32 protocol.c 完全一致):
 *   [AA 55] [LEN_H LEN_L] [DEV] [CMD] [SEQ] [DATA...] [CRC_H CRC_L] [0D]
 *
 * CRC 计算范围: LEN(2) + DEV(1) + CMD(1) + SEQ(1) + DATA(N) = 5+N 字节
 */

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ==================== 协议常量 ==================== */

#define PROTO_HEADER                    0xAA55
#define PROTO_TAIL                      0x0D
#define PROTO_DEVICE_ID_STM32           0x01
#define PROTO_FRAME_MAX_SIZE            256
#define PROTO_PAYLOAD_MAX_SIZE          128
#define PROTO_PARSE_MAX_CALLBACKS       16
#define PROTO_PARSE_MAX_DATA_LEN        250

/* ---- 上行 CMD (STM32 → Orange Pi) ---- */
#define PROTO_CMD_TEMP_HUMIDITY_DATA    0x04
#define PROTO_CMD_MOTOR_STATUS_RESP     0x06
#define PROTO_CMD_SYSTEM_STATUS         0x07
#define PROTO_CMD_EMERGENCY_EVENT       0x10
#define PROTO_CMD_NDE_FEATURE           0x17
#define PROTO_CMD_NDE_HEARTBEAT         0x18
#define PROTO_CMD_DUAL_DIAG             0x19
#define PROTO_CMD_HEARTBEAT             0xFE

/* ---- 下行 CMD (ESP32 → STM32, 保留供参考) ---- */
#define PROTO_CMD_AI_RESULT             0x10
#define PROTO_CMD_CONTROL               0x11
#define PROTO_CMD_CONFIG_SET            0x12
#define PROTO_CMD_CONFIG_GET            0x13
#define PROTO_CMD_TIME_SYNC_REQ         0x14
#define PROTO_CMD_MOTOR_CONTROL         0x15
#define PROTO_CMD_MOTOR_QUERY           0x16

/* ---- 系统 CMD ---- */
#define PROTO_CMD_ACK                   0xF0
#define PROTO_CMD_NACK                  0xF1
#define PROTO_CMD_RESET                 0xFF

/* ==================== 各 CMD Payload 长度 ==================== */

#define TEMP_HUMIDITY_PAYLOAD_LEN       16
#define MOTOR_STATUS_PAYLOAD_LEN        26
#define SYSTEM_STATUS_PAYLOAD_LEN       8
#define EMERGENCY_EVENT_PAYLOAD_LEN     8
#define DUAL_DIAG_PAYLOAD_LEN           32
#define NDE_FEATURE_PAYLOAD_LEN         100
#define NDE_HEARTBEAT_PAYLOAD_LEN       4
#define HEARTBEAT_PAYLOAD_LEN           1

/* ==================== 电机状态 Payload 偏移 ==================== */

#define MOTOR_OFF_RPM            0
#define MOTOR_OFF_CURRENT_MA     4
#define MOTOR_OFF_BUS_MV         8
#define MOTOR_OFF_TEMP_DC        12
#define MOTOR_OFF_STATE          16
#define MOTOR_OFF_FAULT          17
#define MOTOR_OFF_DUTY           18
#define MOTOR_OFF_DIRECTION      22
#define MOTOR_OFF_PID_ACTIVE     23

/* ==================== 传感器类型 ==================== */

#define PROTO_SENSOR_TYPE_SIMULATED     0xFF
#define PROTO_SENSOR_TYPE_DHT11         0x01
#define PROTO_SENSOR_TYPE_SHT30         0x02
#define PROTO_SENSOR_TYPE_DS18B20       0x03
#define PROTO_SENSOR_TYPE_NTC           0x04

#define PROTO_SENSOR_STATUS_NORMAL      0x00
#define PROTO_SENSOR_STATUS_ERROR       0x01
#define PROTO_SENSOR_STATUS_TIMEOUT     0x02
#define PROTO_SENSOR_STATUS_NO_DATA     0x03

/* ==================== 电机控制子命令 ==================== */

#define MOTOR_SUBCMD_STOP             0x00
#define MOTOR_SUBCMD_START            0x01
#define MOTOR_SUBCMD_SET_DUTY         0x02
#define MOTOR_SUBCMD_SET_SPEED        0x03
#define MOTOR_SUBCMD_SET_DIRECTION    0x04
#define MOTOR_SUBCMD_PID_ENABLE       0x05
#define MOTOR_SUBCMD_EMERGENCY_STOP   0x06
#define MOTOR_SUBCMD_CLEAR_FAULT      0x07

/* ==================== NDE 常量 ==================== */

#define NDE_FEATURE_VEC_DIM        24
#define NDE_FEATURE_PAYLOAD_LEN    100
#define NDE_HEARTBEAT_PAYLOAD_LEN  4

/* ==================== 10 状态机枚举 ==================== */

enum proto_state {
    S_WAIT_HEADER_H,
    S_WAIT_HEADER_L,
    S_WAIT_LEN_H,
    S_WAIT_LEN_L,
    S_WAIT_DEV_ID,
    S_WAIT_CMD,
    S_WAIT_SEQ,
    S_WAIT_DATA,
    S_WAIT_CRC_H,
    S_WAIT_CRC_L,
    S_WAIT_TAIL,
};

/* ==================== 解析器统计 ==================== */

struct proto_parse_stats {
    uint32_t rx_frames;
    uint32_t rx_bytes;
    uint32_t crc_errors;
    uint32_t frame_errors;
    uint32_t cmds_unknown;
    uint32_t bytes_raw;
    uint64_t last_frame_ts_ms;
};

/* ==================== 回调签名 ==================== */

typedef void (*proto_parse_callback_t)(uint8_t cmd, uint8_t dev_id,
                                       const uint8_t *data, uint16_t len,
                                       void *user_data);

/* ==================== CRC16 API ==================== */

uint16_t proto_crc16_modbus(const uint8_t *data, uint16_t len);

/* ==================== 解析器 API ==================== */

int  proto_parse_init(void);
void proto_parse_deinit(void);
void proto_parse_feed(uint8_t byte);
void proto_parse_feed_buf(const uint8_t *data, uint16_t len);

int  proto_parse_register(uint8_t cmd, proto_parse_callback_t cb, void *user_data);
int  proto_parse_unregister(uint8_t cmd);

void proto_parse_get_stats(struct proto_parse_stats *stats);
void proto_parse_reset_stats(void);

void proto_dump_frame(const char *tag, const uint8_t *frame, uint16_t len);

#endif /* PROTOCOL_H */
