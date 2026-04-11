/**
 * @file protocol.h
 * @author EnterWorldDoor
 * @brief 企业级 UART 通信协议（帧格式、状态机、ACK/心跳、命令分发）
 *
 * 功能特性:
 *   - 自定义二进制帧协议: Header + Length + DevID + Cmd + Seq + Data + CRC + Tail
 *   - 状态机解析: 10 状态防粘包/防错位
 *   - CRC16-MODBUS 校验 (多项式 0x8005)
 *   - ACK 确认机制: 发送等待确认，超时重发
 *   - 心跳保活: 周期性心跳检测设备在线
 *   - 序号机制: 防丢包/防重放
 *   - FreeRTOS Mutex 线程安全
 *   - 统计计数器: TX/RX/Error 用于系统监控
 *   - 可调试: 帧级 dump 输出
 *   - 与 config_manager 集成获取配置参数
 */

#ifndef PROTOCOL_H_
#define PROTOCOL_H_

#include <stdint.h>
#include <stdbool.h>
#include "global_error.h"
#include "freertos/FreeRTOS.h"

/* ==================== 协议常量 ==================== */

#define PROTO_HEADER              0xAA55    /**< 帧头固定值 */
#define PROTO_TAIL                0x0D     /**< 帧尾固定值 (CR) */
#define PROTO_MAX_DATA_LEN        250      /**< 最大数据负载长度 */
#define PROTO_FRAME_OVERHEAD      9        /**< 帧头(2)+长度(2)+DevID(1)+Cmd(1)+Seq(1)+CRC(2)+Tail(1) */
#define PROTO_MAX_FRAME_LEN       (PROTO_FRAME_OVERHEAD + PROTO_MAX_DATA_LEN)  /**< 最大帧长度 */
#define PROTO_RX_BUF_SIZE         1024     /**< UART 接收缓冲区 */
#define PROTO_TX_BUF_SIZE         1024     /**< UART 发送缓冲区 */
#define PROTO_TASK_STACK_SIZE     4096     /**< 接收任务栈大小 */
#define PROTO_TASK_PRIORITY       5        /**< 接收任务优先级 */
#define PROTO_ACK_TIMEOUT_MS      500      /**< ACK 超时时间 (ms) */
#define PROTO_MAX_RETRIES         3        /**< 最大重试次数 */
#define PROTO_HEARTBEAT_INTERVAL_MS 1000   /**< 心跳间隔 (ms) */
#define PROTO_MAX_CALLBACKS       16       /**< 最大回调注册数 */
#define PROTO_DEV_ID_DEFAULT      1        /**< 默认设备 ID */

/* ==================== 命令字定义 ==================== */

/* STM32 → ESP32 (采集端发送) */
#define CMD_ADC_DATA              0x01     /**< ADC 传感器数据 */
#define CMD_ENCODER_DATA          0x02     /**< 编码器转速数据 */
#define CMD_STATUS_REPORT         0x03     /**< 设备状态上报 */

/* ESP32 → STM32 (控制端发送) */
#define CMD_AI_RESULT             0x10     /**< AI 分析结果 */
#define CMD_CONTROL               0x11     /**< 控制命令下发 */
#define CMD_CONFIG_SET            0x12     /**< 参数配置下发 */
#define CMD_CONFIG_GET            0x13     /**< 参数配置查询 */

/* 系统命令 */
#define CMD_ACK                   0xF0     /**< ACK 确认帧 */
#define CMD_NACK                  0xF1     /**< NACK 否定确认 */
#define CMD_HEARTBEAT             0xFE     /**< 心跳帧 */
#define CMD_RESET                 0xFF     /**< 远程复位 */

/* ==================== 数据结构 ==================== */

/**
 * struct proto_packet - 解析后的协议数据包
 */
struct proto_packet {
    uint8_t  dev_id;              /**< 源设备 ID */
    uint8_t  cmd;                 /**< 命令字 */
    uint8_t  seq;                 /**< 序号 */
    uint8_t  *data;               /**< 数据载荷指针 */
    uint16_t data_len;            /**< 数据载荷长度 */
};

/**
 * struct adc_data - ADC 传感器数据包 (STM32→ESP32)
 */
struct adc_data {
    float vib_x;                   /**< X轴振动加速度 */
    float vib_y;                   /**< Y轴振动加速度 */
    float vib_z;                   /**< Z轴振动加速度 */
    float temperature;             /**< 温度 (°C) */
};

/**
 * struct encoder_data - 编码器数据包 (STM32→ESP32)
 */
struct encoder_data {
    float rpm;                     /**< 转速 (RPM) */
    int8_t direction;             /**< 方向: 1=正转, -1=反转, 0=停止 */
};

/**
 * struct status_report - 设备状态上报 (STM32→ESP32)
 */
struct status_report {
    uint8_t device_status;         /**< 设备状态: 0=正常, 1=警告, 2=故障 */
    uint8_t error_code;           /**< 错误码 */
    uint16_t uptime_s;            /**< 运行时间 (秒) */
    float supply_voltage;         /**< 供电电压 (V) */
};

/**
 * struct ai_result - AI分析结果 (ESP32→STM32)
 */
struct ai_result {
    float rms;                     /**< RMS 有效值 */
    float freq;                    /**< 主频率 (Hz) */
    float amplitude;              /**< 幅值 */
    uint8_t abnormal;             /**< 异常标志: 0=正常, 1=异常 */
    uint8_t severity;             /**< 严重程度: 0~3 */
};

/**
 * struct ctrl_command - 控制命令 (ESP32→STM32)
 */
struct ctrl_command {
    uint8_t target_device;        /**< 目标设备: 0=全局, 1=电机, 2=加热器... */
    uint8_t command;              /**< 命令: 0=停止, 1=启动, 2=复位 */
    int16_t value;                /**< 参数值 (如 PWM 占空比) */
};

/**
 * struct proto_stats - 协议统计信息
 */
struct proto_stats {
    uint32_t tx_frames;            /**< 发送帧总数 */
    uint32_t rx_frames;            /**< 接收帧总数 */
    uint32_t tx_bytes;             /**< 发送字节总数 */
    uint32_t rx_bytes;             /**< 接收字节总数 */
    uint32_t crc_errors;           /**< CRC 错误次数 */
    uint32_t frame_errors;         /**< 帧格式错误次数 */
    uint32_t ack_timeouts;         /**< ACK 超时次数 */
    uint32_t retries;              /**< 重发次数 */
    uint32_t ack_received;         /**< 收到 ACK 次数 */
    uint32_t heartbeat_sent;       /**< 发送心跳次数 */
    uint32_t heartbeat_received;   /**< 收到心跳次数 */
};

/**
 * struct proto_config - 协议模块配置
 */
struct proto_config {
    uint8_t dev_id;                /**< 本机设备 ID */
    uint32_t baud_rate;            /**< 波特率 */
    bool enable_ack;               /**< 是否启用 ACK 机制 */
    bool enable_heartbeat;        /**< 是否启用心跳 */
    uint16_t ack_timeout_ms;       /**< ACK 超时 (ms) */
    uint8_t max_retries;           /**< 最大重试次数 */
    uint32_t heartbeat_interval_ms;/**< 心跳间隔 (ms) */
    bool debug_dump;               /**< 是否输出帧调试信息 */
};

/* ==================== 回调函数类型 ==================== */

/**
 * proto_callback_t - 命令处理回调函数类型
 * @cmd: 命令字
 * @data: 数据载荷指针
 * @len: 数据载荷长度
 * @user_data: 用户上下文指针 (注册时传入)
 */
typedef void (*proto_callback_t)(uint8_t cmd, const uint8_t *data,
                                 uint16_t len, void *user_data);

/**
 * proto_error_callback_t - 协议错误回调函数类型
 * @error_code: 错误码 (APP_ERR_PROTO_*)
 * @context: 附加上下文信息
 */
typedef void (*proto_error_callback_t)(int error_code, const char *context);

/* ==================== 生命周期 API ==================== */

/**
 * protocol_init - 初始化协议栈
 * @uart_num: UART 端口号 (如 UART_NUM_4)
 * @baud_rate: 波特率 (如 115200)
 * @tx_pin: TX 引脚号
 * @rx_pin: RX 引脚号
 *
 * Return: APP_ERR_OK on success, negative error code on failure
 */
int protocol_init(int uart_num, int baud_rate, int tx_pin, int rx_pin);

/**
 * protocol_deinit - 反初始化协议栈，释放所有资源
 *
 * Return: APP_ERR_OK or error code
 */
int protocol_deinit(void);

/**
 * protocol_is_initialized - 查询是否已初始化
 *
 * Return: true 已初始化, false 未初始化
 */
bool protocol_is_initialized(void);

/**
 * protocol_start - 启动接收任务和心跳任务
 *
 * Return: APP_ERR_OK or error code
 */
int protocol_start(void);

/**
 * protocol_stop - 停止所有后台任务
 *
 * Return: APP_ERR_OK or error code
 */
int protocol_stop(void);

/* ==================== 发送 API ==================== */

/**
 * protocol_send - 发送数据帧 (无 ACK 确认)
 * @cmd: 命令字
 * @data: 负载数据指针 (NULL 表示无载荷)
 * @len: 负载长度
 *
 * 内部自动添加帧头、长度、DevID、序号、CRC、帧尾。
 *
 * Return: APP_ERR_OK on success, negative error code on failure
 */
int protocol_send(uint8_t cmd, const uint8_t *data, uint16_t len);

/**
 * protocol_send_with_ack - 发送数据帧并等待 ACK 确认
 * @cmd: 命令字
 * @data: 负载数据指针
 * @len: 负载长度
 * @timeout_ms: ACK 等待超时 (ms), 0 使用默认值
 *
 * 内部自动重发直到收到 ACK 或超过最大重试次数。
 *
 * Return: APP_ERR_OK on success, APP_ERR_PROTO_ACK_TIMEOUT on timeout
 */
int protocol_send_with_ack(uint8_t cmd, const uint8_t *data, uint16_t len,
                            int timeout_ms);

/**
 * protocol_send_heartbeat - 手动发送一次心跳帧
 *
 * Return: APP_ERR_OK or error code
 */
int protocol_send_heartbeat(void);

/* ==================== 注册 API ==================== */

/**
 * protocol_register_callback - 注册命令处理回调
 * @cmd: 命令字
 * @cb: 回调函数
 * @user_data: 用户上下文 (传递给回调)
 *
 * Return: APP_ERR_OK or error code
 */
int protocol_register_callback(uint8_t cmd, proto_callback_t cb,
                                void *user_data);

/**
 * protocol_unregister_callback - 注销命令处理回调
 * @cmd: 命令字
 * @cb: 待注销的回调函数
 *
 * Return: APP_ERR_OK or error code
 */
int protocol_unregister_callback(uint8_t cmd, proto_callback_t cb);

/**
 * protocol_register_error_callback - 注册错误回调
 * @cb: 错误回调函数
 *
 * 当发生 CRC 错误、帧错误等异常时调用。
 *
 * Return: APP_ERR_OK or error code
 */
int protocol_register_error_callback(proto_error_callback_t cb);

/* ==================== 查询 API ==================== */

/**
 * protocol_get_stats - 获取协议统计信息
 * @stats: 输出统计结构体指针
 *
 * Return: APP_ERR_OK or error code
 */
int protocol_get_stats(struct proto_stats *stats);

/**
 * protocol_reset_stats - 重置统计计数器
 */
void protocol_reset_stats(void);

/**
 * protocol_get_dev_id - 获取本机设备 ID
 *
 * Return: 设备 ID
 */
uint8_t protocol_get_dev_id(void);

/**
 * protocol_set_dev_id - 设置本机设备 ID
 * @dev_id: 新的设备 ID
 */
void protocol_set_dev_id(uint8_t dev_id);

/**
 * protocol_is_peer_alive - 查询对端是否在线
 *
 * 基于最近心跳响应判断。
 *
 * Return: true 在线, false 离线/未知
 */
bool protocol_is_peer_alive(void);

/* ==================== 调试 API ==================== */

/**
 * protocol_dump_frame - 打印一帧的十六进制内容
 * @frame: 帧数据指针
 * @len: 帧长度
 * @direction: 方向描述 ("TX" 或 "RX")
 */
void protocol_dump_frame(const uint8_t *frame, uint16_t len,
                         const char *direction);

/**
 * protocol_dump_stats - 打印统计信息到日志
 */
void protocol_dump_stats(void);

#endif /* PROTOCOL_H_ */