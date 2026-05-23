/**
 * @file protocol.h
 * @brief STM32-ESP32 UART通信协议栈 (CRC16-MODBUS, 帧格式)
 *
 * 实现与ESP32节点之间的二进制通信协议，包括:
 *   - CRC16-MODBUS校验
 *   - 标准帧构建与解析
 *   - 温湿度传感器数据传输
 *
 * 帧格式 (与ESP32 protocol.c build_frame完全一致):
 *   [AA 55] [LEN_H LEN_L] [DEV_ID] [CMD] [SEQ] [DATA...] [CRC_H CRC_L] [0D]
 *
 * 数据流向:
 *   STM32 (UART4 TX) → ESP32 (UART RX)
 */

#ifndef __PROTOCOL_H
#define __PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include "global_error/global_error.h"

/* ==================== 协议常量 ==================== */

#define PROTO_HEADER                    0xAA55
#define PROTO_TAIL                      0x0D
#define PROTO_DEVICE_ID_STM32           0x01
#define PROTO_CMD_TEMP_HUMIDITY_DATA    0x04
#define PROTO_CMD_NDE_FEATURE           0x17
#define PROTO_CMD_NDE_HEARTBEAT         0x18
#define PROTO_CMD_DUAL_DIAG             0x19
#define PROTO_CMD_SYSTEM_STATUS         0x07   /**< 系统状态上报 (F407→ESP32) */
#define PROTO_CMD_EMERGENCY_EVENT       0x10   /**< 紧急停机事件 (F407→ESP32) */
#define PROTO_FRAME_MAX_SIZE            256
#define PROTO_PAYLOAD_MAX_SIZE          128

/* ==================== ESP32 → F407 下行命令字 ==================== */

#define PROTO_CMD_AI_RESULT             0x10   /**< AI 分析结果 (ESP32→F407) */
#define PROTO_CMD_CONTROL               0x11   /**< 控制命令下发 */
#define PROTO_CMD_CONFIG_SET            0x12   /**< 参数配置下发 */
#define PROTO_CMD_CONFIG_GET            0x13   /**< 参数配置查询 */
#define PROTO_CMD_TIME_SYNC_REQ         0x14   /**< 时间同步请求 */
#define PROTO_CMD_MOTOR_CONTROL         0x15   /**< 电机控制命令 */
#define PROTO_CMD_MOTOR_QUERY           0x16   /**< 电机状态查询 */

/* ==================== 系统命令字 ==================== */

#define PROTO_CMD_ACK                   0xF0   /**< ACK 确认 */
#define PROTO_CMD_NACK                  0xF1   /**< NACK 否定确认 */
#define PROTO_CMD_HEARTBEAT             0xFE   /**< 心跳帧 */
#define PROTO_CMD_RESET                 0xFF   /**< 远程复位 */

/* ==================== 解析器常量 ==================== */

#define PROTO_PARSE_MAX_CALLBACKS       16     /**< 最大回调注册数 */
#define PROTO_PARSE_MAX_DATA_LEN        250

/* ==================== 传感器类型定义 ==================== */

#define PROTO_SENSOR_TYPE_SIMULATED     0xFF
#define PROTO_SENSOR_TYPE_DHT11         0x01
#define PROTO_SENSOR_TYPE_SHT30         0x02
#define PROTO_SENSOR_TYPE_DS18B20       0x03
#define PROTO_SENSOR_TYPE_NTC           0x04

/* ==================== 电机控制命令字 ==================== */

/* STM32 → ESP32 */
#define PROTO_CMD_MOTOR_STATUS_RESP   0x06   /**< 电机状态上报 */

/* ESP32 → STM32 */
#define PROTO_CMD_MOTOR_CONTROL       0x15   /**< 电机控制命令 */
#define PROTO_CMD_MOTOR_QUERY         0x16   /**< 电机状态查询 */

/* 电机控制子命令 (CMD_MOTOR_CONTROL data[0]) */
#define MOTOR_SUBCMD_STOP             0x00
#define MOTOR_SUBCMD_START            0x01
#define MOTOR_SUBCMD_SET_DUTY         0x02
#define MOTOR_SUBCMD_SET_SPEED        0x03
#define MOTOR_SUBCMD_SET_DIRECTION    0x04
#define MOTOR_SUBCMD_PID_ENABLE       0x05
#define MOTOR_SUBCMD_EMERGENCY_STOP   0x06
#define MOTOR_SUBCMD_CLEAR_FAULT      0x07

/* 电机状态上报 payload 布局 (26字节) */
#define MOTOR_STATUS_PAYLOAD_LEN      26
/* Offsets within status payload:
 * [0-3]   rpm (int32_t, LE)
 * [4-7]   current_ma (int32_t, LE)
 * [8-11]  bus_mv (int32_t, LE)
 * [12-15] temp_dc (int32_t, LE, 0.1°C)
 * [16]    state (uint8_t)
 * [17]    fault (uint8_t)
 * [18-21] duty (int32_t, LE)
 * [22]    direction (int8_t, 1=CW/-1=CCW)
 * [23]    pid_active (uint8_t)
 * [24-25] reserved
 */

/* ==================== 传感器状态定义 ==================== */

#define PROTO_SENSOR_STATUS_NORMAL      0x00
#define PROTO_SENSOR_STATUS_ERROR       0x01
#define PROTO_SENSOR_STATUS_TIMEOUT     0x02
#define PROTO_SENSOR_STATUS_NO_DATA     0x03

/* ==================== 数据结构 ==================== */

/**
 * struct proto_temp_humidity_data - 温湿度传感器数据载荷
 * @temp_c: 温度值 (°C), float
 * @humidity_rh: 湿度值 (%RH), float
 * @timestamp_ms: 时间戳 (ms), uint32_t
 * @sensor_type: 传感器类型
 * @sensor_status: 传感器状态
 * @raw_adc_value: 原始ADC值 (预留)
 */
struct proto_temp_humidity_data {
        float temp_c;
        float humidity_rh;
        uint32_t timestamp_ms;
        uint8_t sensor_type;
        uint8_t sensor_status;
        int16_t raw_adc_value;
};

/* ==================== CRC16 API ==================== */

/**
 * proto_crc16_modbus - 计算CRC16-MODBUS校验值
 * @data: 数据缓冲区指针
 * @len: 数据长度 (字节)
 *
 * 算法: CRC-16/MODBUS, 多项式 0xA001, 初始值 0xFFFF
 * 与ESP32端的 crc16_modbus() 完全一致。
 *
 * Return: 16位CRC校验值
 */
uint16_t proto_crc16_modbus(const uint8_t *data, uint16_t len);

/* ==================== 帧构建 API ==================== */

/**
 * proto_build_temp_humidity_frame - 构建温湿度数据帧
 * @frame: 输出帧缓冲区 (至少 PROTO_FRAME_MAX_SIZE 字节)
 * @data: 温湿度传感器数据
 * @seq: 帧序列号 (由调用方管理, 每次发送递增)
 *
 * 构建完整的二进制协议帧，包括:
 *   1. 帧头 (2字节): 0xAA55
 *   2. 数据长度 (2字节): 大端序
 *   3. 设备ID (1字节)
 *   4. 命令码 (1字节)
 *   5. 序列号 (1字节)
 *   6. 数据载荷 (16字节)
 *   7. CRC16校验 (2字节, 大端序)
 *   8. 帧尾 (1字节): 0x0D
 *
 * Return: 帧总长度 (字节), 正数; 负数为错误码
 */
int proto_build_temp_humidity_frame(uint8_t *frame,
                                    const struct proto_temp_humidity_data *data,
                                    uint8_t seq);

/**
 * proto_build_generic_frame - 构建通用数据帧
 * @frame: 输出帧缓冲区
 * @cmd: 命令码
 * @payload: 数据载荷
 * @payload_len: 载荷长度 (字节)
 * @seq: 帧序列号
 *
 * Return: 帧总长度 (字节), 正数; 负数为错误码
 */
int proto_build_generic_frame(uint8_t *frame,
                              uint8_t cmd,
                              const uint8_t *payload,
                              uint16_t payload_len,
                              uint8_t seq);

/* ==================== 帧解析 API ==================== */

/**
 * proto_parse_header - 解析帧头
 * @byte: 接收到的字节
 *
 * 状态机解析，逐字节喂入。需要在调用方维护解析状态。
 *
 * Return: true 帧头匹配, false 不匹配或需要更多数据
 */
bool proto_parse_header(uint8_t byte);

/* ==================== 电机协议 API ==================== */

/**
 * proto_build_motor_status_frame - 构建电机状态上报帧
 * @frame: 输出帧缓冲区
 * @seq: 帧序列号
 * @rpm, current_ma, bus_mv, temp_dc, state, fault, duty, dir, pid_active
 */
int proto_build_motor_status_frame(uint8_t *frame, uint8_t seq,
	int32_t rpm, int32_t current_ma, int32_t bus_mv,
	int32_t temp_dc, uint8_t state, uint8_t fault,
	int32_t duty, int8_t dir, bool pid_active);

/**
 * proto_parse_motor_control - 解析电机控制命令payload
 * @payload: 数据载荷指针
 * @len: 载荷长度
 * @subcmd: 输出, 子命令
 * @value: 输出, 参数值 (duty/RPM/direction)
 * @return: 0=成功, 负数=错误
 */
int proto_parse_motor_control(const uint8_t *payload, uint16_t len,
	uint8_t *subcmd, int32_t *value);

/* ==================== NDE传感器节点协议 API ==================== */

#define NDE_FEATURE_VEC_DIM    24         /**< 特征向量维度 (与DE一致) */
#define NDE_FEATURE_PAYLOAD_LEN 100       /**< PAYLOAD: 4字节头 + 24×4字节特征 */
#define NDE_HEARTBEAT_PAYLOAD_LEN 4       /**< HEARTBEAT PAYLOAD: 4字节 */

/**
 * proto_build_nde_feature_frame - 构建NDE特征向量帧 (CMD 0x15)
 * @frame: 输出帧缓冲区
 * @features: 24维特征向量 (float32, 96字节)
 * @window_idx: 窗口序号 (0-255, 滚动)
 * @seq: 帧序列号
 * @return: 帧总长度 (字节), 正数; 负数为错误码
 */
int proto_build_nde_feature_frame(uint8_t *frame,
                                  const float *features,
                                  uint8_t window_idx,
                                  uint8_t seq);

/**
 * proto_build_nde_heartbeat_frame - 构建NDE心跳帧 (CMD 0x16)
 * @frame: 输出帧缓冲区
 * @online: NDE节点是否在线 (1=在线, 0=离线)
 * @error_count: 累计DSP/CAN错误计数
 * @temp_c: NDE侧温度 (int8_t, -40..125°C)
 * @reserved: 保留
 * @seq: 帧序列号
 * @return: 帧总长度 (字节), 正数; 负数为错误码
 */
int proto_build_nde_heartbeat_frame(uint8_t *frame,
                                    uint8_t online,
                                    uint8_t error_count,
                                    int8_t temp_c,
                                    uint8_t reserved,
                                    uint8_t seq);

/* ==================== 帧解析器 API (protocol_parser.c) ==================== */

typedef void (*proto_parse_callback_t)(uint8_t cmd, const uint8_t *data,
                                       uint16_t len);

int proto_parse_init(void);
void proto_parse_feed(uint8_t byte);
void proto_parse_feed_buf(const uint8_t *data, uint16_t len);
int proto_parse_register(uint8_t cmd, proto_parse_callback_t cb);
int proto_parse_unregister(uint8_t cmd);

struct proto_parse_stats {
    uint32_t rx_frames;
    uint32_t rx_bytes;
    uint32_t crc_errors;
    uint32_t frame_errors;
    uint32_t cmds_unknown;
};

void proto_parse_get_stats(struct proto_parse_stats *stats);
bool proto_parse_is_peer_alive(void);

/* ==================== 工具函数 ==================== */

uint32_t proto_timestamp_get(void);

#endif /* __PROTOCOL_H */
