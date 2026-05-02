/**
 * @file protocol.c
 * @author EnterWorldDoor
 * @brief 企业级 UART 通信协议实现：状态机解析、CRC16、ACK/心跳、线程安全、统计
 *
 * 架构设计:
 *   - 帧格式: Header(2) + Length(2) + DevID(1) + Cmd(1) + Seq(1) + Data(N) + CRC(2) + Tail(1)
 *   - 状态机: 10 状态解析，防粘包/防错位
 *   - CRC16: MODBUS-RTU 多项式 0x8005
 *   - ACK: 发送等待确认，超时重发 (可配置)
 *   - 心跳: 周期性心跳保活检测对端在线状态
 *   - 序号: 自增序号防丢包/防重放
 *   - Mutex: 所有共享状态通过互斥量保护
 *   - 统计: TX/RX/Error 计数器用于系统监控
 */

#include "protocol.h"
#include "log_system.h"
#include "global_error.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>

/* ==================== 解析状态机定义 ==================== */

typedef enum {
    STATE_WAIT_HEADER_H,
    STATE_WAIT_HEADER_L,
    STATE_WAIT_LEN_H,
    STATE_WAIT_LEN_L,
    STATE_WAIT_DEV_ID,
    STATE_WAIT_CMD,
    STATE_WAIT_SEQ,
    STATE_WAIT_DATA,
    STATE_WAIT_CRC_H,
    STATE_WAIT_CRC_L,
    STATE_WAIT_TAIL,
} parser_state_t;

/* ==================== 模块内部状态 ==================== */

static struct {
    bool initialized;
    int uart_num;
    uint8_t dev_id;
    uint8_t seq;

    /* 线程安全 */
    SemaphoreHandle_t mutex;

    /* 任务句柄 */
    TaskHandle_t rx_task;
    TaskHandle_t heartbeat_task;

    /* ACK 同步 */
    EventGroupHandle_t ack_event;
    uint8_t ack_cmd_waiting;
    uint8_t ack_seq_waiting;

    /* 回调注册 */
    struct {
        proto_callback_t cb;
        void *user_data;
    } callbacks[PROTO_MAX_CALLBACKS];
    int callback_count;

    /* 错误回调 */
    proto_error_callback_t error_cb;

    /* 温湿度数据回调 (新增) */
    struct {
        temp_data_callback_t cb;
        void *user_data;
    } temp_callbacks[8];
    int temp_callback_count;

    /* 最新温湿度数据缓存 (新增) */
    struct temp_humidity_data latest_temp;
    bool has_latest_temp;

    /* 时间同步信息 (新增) */
    struct time_sync_info time_sync;
    struct {
        proto_time_sync_callback_t cb;
        void *user_data;
    } time_sync_callbacks[4];
    int time_sync_callback_count;

    /* 统计计数器 */
    struct proto_stats stats;

    /* 心跳 */
    bool peer_alive;
    TickType_t last_heartbeat_tick;

    /* 配置 */
    struct proto_config cfg;
} g_proto = {0};

/* EventGroup 位定义 */
static const int ACK_RECEIVED_BIT = BIT0;
static const int STOP_RX_BIT      = BIT1;
static const int STOP_HB_BIT      = BIT2;

/* ==================== 内部辅助函数 ==================== */

/**
 * crc16_modbus - 计算 CRC16-MODBUS 校验值
 * @data: 数据指针
 * @len: 数据长度
 * Return: 16位 CRC 值
 */
static uint16_t crc16_modbus(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001)
                crc = (crc >> 1) ^ 0xA001;
            else
                crc >>= 1;
        }
    }
    return crc;
}

/**
 * build_frame - 构建完整帧数据
 * @cmd: 命令字
 * @seq: 序号
 * @data: 数据载荷
 * @len: 数据长度
 * @out_buf: 输出缓冲区
 * @out_len: 输出实际帧长度
 *
 * Return: APP_ERR_OK or error code
 */
static int build_frame(uint8_t cmd, uint8_t seq,
                      const uint8_t *data, uint16_t len,
                      uint8_t *out_buf, uint16_t *out_len)
{
    if (len > PROTO_MAX_DATA_LEN) return APP_ERR_PROTO_FRAME_OVERFLOW;
    if (!out_buf || !out_len) return APP_ERR_INVALID_PARAM;

    uint16_t idx = 0;

    out_buf[idx++] = (PROTO_HEADER >> 8) & 0xFF;
    out_buf[idx++] = PROTO_HEADER & 0xFF;
    out_buf[idx++] = (len >> 8) & 0xFF;
    out_buf[idx++] = len & 0xFF;
    out_buf[idx++] = g_proto.dev_id;
    out_buf[idx++] = cmd;
    out_buf[idx++] = seq;

    if (data && len > 0) {
        memcpy(&out_buf[idx], data, len);
        idx += len;
    }

    uint16_t crc = crc16_modbus(&out_buf[2], idx - 2);
    out_buf[idx++] = (crc >> 8) & 0xFF;
    out_buf[idx++] = crc & 0xFF;
    out_buf[idx++] = PROTO_TAIL;

    *out_len = idx;
    return APP_ERR_OK;
}

/**
 * uart_send_raw - 通过 UART 发送原始字节
 */
static void uart_send_raw(const uint8_t *buf, int len)
{
    if (g_proto.uart_num >= 0 && len > 0) {
        uart_write_bytes(g_proto.uart_num, (const char *)buf, len);
    }
}

/**
 * update_stats_tx - 更新发送统计
 */
static void update_stats_tx(uint16_t frame_len)
{
    xSemaphoreTake(g_proto.mutex, portMAX_DELAY);
    g_proto.stats.tx_frames++;
    g_proto.stats.tx_bytes += frame_len;
    xSemaphoreGive(g_proto.mutex);
}

/**
 * update_stats_rx - 更新接收统计
 */
static void update_stats_rx(uint16_t frame_len)
{
    xSemaphoreTake(g_proto.mutex, portMAX_DELAY);
    g_proto.stats.rx_frames++;
    g_proto.stats.rx_bytes += frame_len;
    xSemaphoreGive(g_proto.mutex);
}

/**
 * notify_error - 通知错误回调
 */
static void notify_error(int err_code, const char *context)
{
    if (g_proto.error_cb) {
        g_proto.error_cb(err_code, context);
    }
}

/**
 * dispatch_callback - 分发命令到注册的回调
 */
static void dispatch_callback(uint8_t cmd, const uint8_t *data,
                               uint16_t len, uint8_t dev_id, uint8_t seq)
{
    for (int i = 0; i < g_proto.callback_count; i++) {
        if (g_proto.callbacks[i].cb) {
            g_proto.callbacks[i].cb(cmd, data, len,
                                   g_proto.callbacks[i].user_data);
        }
    }

    if (g_proto.cfg.enable_ack &&
        (cmd != CMD_ACK) && (cmd != CMD_NACK) &&
        (cmd != CMD_HEARTBEAT)) {
        uint8_t ack_data[] = { cmd, seq };
        protocol_send(CMD_ACK, ack_data, sizeof(ack_data));
    }
}

/**
 * parse_temp_humidity_data - 解析温湿度传感器数据帧
 * @data: 原始数据载荷
 * @len: 数据长度
 * @out: 输出解析后的结构体
 *
 * 数据格式 (STM32 端协议):
 *   [0-3]   temperature_c     (float, 4 bytes)
 *   [4-7]   humidity_rh       (float, 4 bytes)
 *   [8-11]  timestamp_stm32_ms (uint32_t, 4 bytes)
 *   [12]    sensor_type       (uint8_t, 1 byte)
 *   [13]    sensor_status     (uint8_t, 1 byte)
 *   [14-15] raw_adc_value      (int16_t, 2 bytes)
 *   总计: 16 bytes
 *
 * Return: APP_ERR_OK or error code
 */
static int parse_temp_humidity_data(const uint8_t *data, uint16_t len,
                                     struct temp_humidity_data *out)
{
    if (!data || !out || len < 16) return APP_ERR_PROTO_INVALID_DATA;

    memcpy(&out->temperature_c, &data[0], sizeof(float));
    memcpy(&out->humidity_rh, &data[4], sizeof(float));
    memcpy(&out->timestamp_stm32_ms, &data[8], sizeof(uint32_t));
    out->sensor_type = (enum temp_sensor_type)data[12];
    out->sensor_status = data[13];
    memcpy(&out->raw_adc_value, &data[14], sizeof(int16_t));

    out->timestamp_esp32_us = (uint32_t)esp_timer_get_time();

    return APP_ERR_OK;
}

/**
 * dispatch_temp_callback - 分发温湿度数据到所有注册的温度回调
 */
static void dispatch_temp_callback(const struct temp_humidity_data *temp_data)
{
    xSemaphoreTake(g_proto.mutex, portMAX_DELAY);
    memcpy(&g_proto.latest_temp, temp_data, sizeof(*temp_data));
    g_proto.has_latest_temp = true;
    xSemaphoreGive(g_proto.mutex);

    for (int i = 0; i < g_proto.temp_callback_count; i++) {
        if (g_proto.temp_callbacks[i].cb) {
            g_proto.temp_callbacks[i].cb(temp_data,
                                        g_proto.temp_callbacks[i].user_data);
        }
    }
}

/**
 * handle_time_sync_response - 处理时间同步响应
 */
static void handle_time_sync_response(const uint8_t *data, uint16_t len)
{
    if (len < 4) return;

    uint32_t stm32_timestamp_ms;
    memcpy(&stm32_timestamp_ms, data, sizeof(uint32_t));

    int64_t esp32_now_us = esp_timer_get_time();
    int64_t stm32_now_us = (int64_t)stm32_timestamp_ms * 1000LL;

    xSemaphoreTake(g_proto.mutex, portMAX_DELAY);
    g_proto.time_sync.stm32_epoch_offset_us = esp32_now_us - stm32_now_us;
    g_proto.time_sync.last_sync_time_us = (uint32_t)esp32_now_us;
    g_proto.time_sync.is_synchronized = true;
    g_proto.time_sync.consecutive_failures = 0;
    xSemaphoreGive(g_proto.mutex);

    LOG_INFO("PROTO", "Time sync complete: offset=%lld us",
             (long long)g_proto.time_sync.stm32_epoch_offset_us);

    for (int i = 0; i < g_proto.time_sync_callback_count; i++) {
        if (g_proto.time_sync_callbacks[i].cb) {
            g_proto.time_sync_callbacks[i].cb(
                &g_proto.time_sync,
                g_proto.time_sync_callbacks[i].user_data);
        }
    }
}

/* ==================== 接收任务（状态机解析）==================== */

static void rx_task_func(void *arg)
{
    (void)arg;
    parser_state_t state = STATE_WAIT_HEADER_H;
    uint8_t frame[PROTO_MAX_FRAME_LEN];
    int frame_idx = 0;
    uint16_t expected_len = 0;
    uint8_t dev_id = 0, cmd = 0, seq = 0;
    uint16_t recv_crc = 0;
    uint8_t buffer[128];

    LOG_INFO("PROTO", "RX task started (state machine parser)");

    /*
     * ⚠️ 【调试模式】UART接收统计
     * 用于诊断为什么ESP32收不到STM32的数据
     * 如果 total_bytes > 0 但 frames = 0: 物理层正常, 协议解析有问题
     * 如果 total_bytes = 0: 物理层完全没有数据 (接线/驱动问题)
     */
    static uint32_t total_bytes = 0;
    static uint32_t debug_counter = 0;

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(
            g_proto.ack_event, STOP_RX_BIT,
            pdFALSE, pdTRUE, pdMS_TO_TICKS(10));
        if (bits & STOP_RX_BIT) break;

        int read_len = uart_read_bytes(g_proto.uart_num, buffer,
                                       sizeof(buffer), pdMS_TO_TICKS(10));

        if (read_len > 0) {
            /* 收到数据! 累计统计 */
            total_bytes += read_len;

            /* 每100字节打印一次 (避免日志洪水) */
            if (total_bytes - debug_counter >= 100) {
                LOG_INFO("PROTO-RX", "📥 Received %u bytes total (last=%d)", total_bytes, read_len);
                debug_counter = total_bytes;
            }
        } else if (read_len == 0) {
            /*
             * ⚠️ 【优化】无UART数据是常见情况,降级为DEBUG!
             *
             * 原始问题:
             *   每10秒打印 WARN: "No UART data received"
             *   STM32未上电/未编程/UART断开时是正常的
             *   不应频繁输出WARN级别日志
             *
             * 解决方案:
             *   降级为 LOG_DEBUG (仅详细日志模式显示)
             *   保持10秒频率控制
             */
            static uint32_t last_no_data_log = 0;
            uint32_t now = (uint32_t)(esp_timer_get_time() / 10000LL);  // 10ms单位
            if (now - last_no_data_log >= 1000) {  // 10秒一次
                LOG_DEBUG("PROTO-RX", "No UART data received (total=%u, waiting for STM32...)", total_bytes);
                last_no_data_log = now;
            }
            continue;
        } else {
            /* read_len < 0: UART错误 */
            LOG_ERROR("PROTO-RX", "UART read error (ret=%d)", read_len);
            continue;
        }

        for (int i = 0; i < read_len; i++) {
            uint8_t byte_val = buffer[i];

            switch (state) {
            case STATE_WAIT_HEADER_H:
                if (byte_val == ((PROTO_HEADER >> 8) & 0xFF))
                    state = STATE_WAIT_HEADER_L;
                break;

            case STATE_WAIT_HEADER_L:
                if (byte_val == (PROTO_HEADER & 0xFF))
                    state = STATE_WAIT_LEN_H;
                else
                    state = STATE_WAIT_HEADER_H;
                break;

            case STATE_WAIT_LEN_H:
                expected_len = byte_val << 8;
                state = STATE_WAIT_LEN_L;
                break;

            case STATE_WAIT_LEN_L:
                expected_len |= byte_val;
                if (expected_len > PROTO_MAX_DATA_LEN + 2) {
                    xSemaphoreTake(g_proto.mutex, portMAX_DELAY);
                    g_proto.stats.frame_errors++;
                    xSemaphoreGive(g_proto.mutex);
                    LOG_WARN("PROTO", "Invalid length %d", expected_len);
                    state = STATE_WAIT_HEADER_H;
                } else {
                    state = STATE_WAIT_DEV_ID;
                    frame_idx = 0;
                }
                break;

            case STATE_WAIT_DEV_ID:
                dev_id = byte_val;
                state = STATE_WAIT_CMD;
                break;

            case STATE_WAIT_CMD:
                cmd = byte_val;
                state = STATE_WAIT_SEQ;
                break;

            case STATE_WAIT_SEQ:
                seq = byte_val;
                state = STATE_WAIT_DATA;
                frame_idx = 0;
                break;

            case STATE_WAIT_DATA:
                if (frame_idx < expected_len) {
                    frame[frame_idx++] = byte_val;
                    if (frame_idx >= expected_len)
                        state = STATE_WAIT_CRC_H;
                } else {
                    state = STATE_WAIT_HEADER_H;
                }
                break;

            case STATE_WAIT_CRC_H:
                recv_crc = byte_val << 8;
                state = STATE_WAIT_CRC_L;
                break;

            case STATE_WAIT_CRC_L:
                recv_crc |= byte_val;
                state = STATE_WAIT_TAIL;
                break;

            case STATE_WAIT_TAIL:
                if (byte_val == PROTO_TAIL) {
                    /*
                     * ⚠️ 【关键修复】统一CRC校验逻辑 (与STM32发送端完全一致!)
                     *
                     * STM32发送端 (protocol.c:149):
                     *   crc16_calculate(&out_buf[2], idx - 2)
                     *   计算范围: 从LEN字段开始到DATA结束
                     *   包含: LEN(2B) + DEV_ID(1B) + CMD(1B) + SEQ(1B) + DATA(nB)
                     *   不包含: SOF(2B) + CRC(2B) + TAIL(1B)
                     *
                     * ESP32接收端应做同样的计算:
                     *   构建 crc_buf = {len_h, len_l, dev_id, cmd, seq, data...}
                     *   calc_crc = crc16_modbus(crc_buf, 4 + expected_len)
                     *   比较 calc_crc 与 recv_crc
                     */

                    /* 构建待校验的数据缓冲区 (与STM32发送时相同) */
                    uint8_t crc_check_buf[PROTO_MAX_DATA_LEN + 5];
                    int crc_buf_len = 0;

                    /* 添加 LEN 字段 */
                    crc_check_buf[crc_buf_len++] = (expected_len >> 8) & 0xFF;
                    crc_check_buf[crc_buf_len++] = expected_len & 0xFF;

                    /* 添加 DEV_ID */
                    crc_check_buf[crc_buf_len++] = dev_id;

                    /* 添加 CMD */
                    crc_check_buf[crc_buf_len++] = cmd;

                    /* 添加 SEQ */
                    crc_check_buf[crc_buf_len++] = seq;

                    /* 添加 DATA (纯数据部分) */
                    /*
                     * ✅ 【关键修复】expected_len 的正确含义:
                     *
                     * STM32发送端 build_frame():
                     *   - 参数 len = 纯DATA载荷长度 (如温湿度数据16字节)
                     *   - 帧中 LEN字段 = len (如 0x0010)
                     *   - CRC计算范围: LEN(2B) + DEV_ID(1B) + CMD(1B) + SEQ(1B) + DATA(len B)
                     *
                     * ESP32接收端状态机:
                     *   - 接收到的 expected_len = 帧中LEN字段值 = 纯DATA长度
                     *   - frame[] 数组中存储了 expected_len 字节的纯DATA
                     *   - 所以这里直接使用 expected_len 作为DATA长度即可!
                     */
                    if (expected_len > 0 && expected_len <= PROTO_MAX_DATA_LEN) {
                        memcpy(&crc_check_buf[crc_buf_len], frame, expected_len);
                        crc_buf_len += expected_len;
                    }

                    /* 计算CRC (使用与STM32相同的Modbus算法) */
                    uint16_t calc_crc = crc16_modbus(crc_check_buf, crc_buf_len);

                    /* 比较计算的CRC与接收到的CRC */
                    if (calc_crc == recv_crc) {
                        /* ✅ CRC校验通过! */
                        uint16_t total_len = 7 + expected_len + 2;
                        update_stats_rx(total_len);

                        if (g_proto.cfg.debug_dump) {
                            uint8_t full_frame[PROTO_MAX_FRAME_LEN];
                            int fidx = 0;
                            full_frame[fidx++] = (PROTO_HEADER >> 8) & 0xFF;
                            full_frame[fidx++] = PROTO_HEADER & 0xFF;
                            full_frame[fidx++] = (expected_len >> 8) & 0xFF;
                            full_frame[fidx++] = expected_len & 0xFF;
                            full_frame[fidx++] = dev_id;
                            full_frame[fidx++] = cmd;
                            full_frame[fidx++] = seq;
                            memcpy(&full_frame[fidx], frame, expected_len);
                            fidx += expected_len;
                            full_frame[fidx++] = (recv_crc >> 8) & 0xFF;
                            full_frame[fidx++] = recv_crc & 0xFF;
                            full_frame[fidx++] = PROTO_TAIL;
                            protocol_dump_frame(full_frame, fidx, "RX");
                        }

                        if (cmd == CMD_HEARTBEAT) {
                            xSemaphoreTake(g_proto.mutex, portMAX_DELAY);
                            g_proto.stats.heartbeat_received++;
                            g_proto.peer_alive = true;
                            g_proto.last_heartbeat_tick = xTaskGetTickCount();
                            xSemaphoreGive(g_proto.mutex);
                        } else if (cmd == CMD_ACK) {
                            if (g_proto.ack_cmd_waiting != 0) {
                                xEventGroupSetBits(g_proto.ack_event,
                                                   ACK_RECEIVED_BIT);
                                xSemaphoreTake(g_proto.mutex, portMAX_DELAY);
                                g_proto.stats.ack_received++;
                                xSemaphoreGive(g_proto.mutex);
                            }
                        } else if (cmd == CMD_TEMP_HUMIDITY_DATA) {
                            struct temp_humidity_data temp_data;
                            int ret = parse_temp_humidity_data(
                                frame, expected_len, &temp_data);
                            if (ret == APP_ERR_OK) {
                                dispatch_temp_callback(&temp_data);
                                LOG_DEBUG("PROTO", "Temp data: T=%.2f°C H=%.1f%%RH",
                                          temp_data.temperature_c,
                                          temp_data.humidity_rh);
                            } else {
                                LOG_WARN("PROTO", "Failed to parse temp data");
                            }
                            dispatch_callback(cmd, frame, expected_len,
                                              dev_id, seq);
                        } else if (cmd == CMD_TIME_SYNC_RESP) {
                            handle_time_sync_response(frame, expected_len);
                            dispatch_callback(cmd, frame, expected_len,
                                              dev_id, seq);
                        } else {
                            dispatch_callback(cmd, frame, expected_len,
                                              dev_id, seq);
                        }
                    } else {
                        xSemaphoreTake(g_proto.mutex, portMAX_DELAY);
                        g_proto.stats.crc_errors++;
                        xSemaphoreGive(g_proto.mutex);
                        LOG_WARN("PROTO", "CRC mismatch recv=0x%04x",
                                 recv_crc);
                        notify_error(APP_ERR_PROTO_CRC_MISMATCH, "CRC check failed");
                    }
                } else {
                    xSemaphoreTake(g_proto.mutex, portMAX_DELAY);
                    g_proto.stats.frame_errors++;
                    xSemaphoreGive(g_proto.mutex);
                }
                state = STATE_WAIT_HEADER_H;
                break;

            default:
                state = STATE_WAIT_HEADER_H;
                break;
            }
        }
    }

    LOG_INFO("PROTO", "RX task stopped");
    vTaskDelete(NULL);
}

/* ==================== 心跳任务 ==================== */

static void heartbeat_task_func(void *arg)
{
    (void)arg;

    LOG_INFO("PROTO", "Heartbeat task started (interval=%u ms)",
             g_proto.cfg.heartbeat_interval_ms);

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(
            g_proto.ack_event, STOP_HB_BIT,
            pdFALSE, pdTRUE,
            pdMS_TO_TICKS(g_proto.cfg.heartbeat_interval_ms));

        if (bits & STOP_HB_BIT) break;

        protocol_send_heartbeat();

        xSemaphoreTake(g_proto.mutex, portMAX_DELAY);
        g_proto.stats.heartbeat_sent++;
        TickType_t elapsed = xTaskGetTickCount() - g_proto.last_heartbeat_tick;
        if (elapsed > pdMS_TO_TICKS(g_proto.cfg.heartbeat_interval_ms * 3)) {
            g_proto.peer_alive = false;
            LOG_WARN("PROTO", "Peer appears offline (no heartbeat for %lu ms)",
                     (unsigned long)(elapsed * portTICK_PERIOD_MS));
        }
        xSemaphoreGive(g_proto.mutex);
    }

    LOG_INFO("PROTO", "Heartbeat task stopped");
    vTaskDelete(NULL);
}

/* ==================== 公开 API 实现 ==================== */

int protocol_init(int uart_num, int baud_rate, int tx_pin, int rx_pin)
{
    if (g_proto.initialized) return APP_ERR_PROTO_ALREADY_INIT;
    if (uart_num < 0) return APP_ERR_PROTO_INVALID_PARAM;

    memset(&g_proto, 0, sizeof(g_proto));

    g_proto.mutex = xSemaphoreCreateMutex();
    if (!g_proto.mutex) {
        LOG_ERROR("PROTO", "Failed to create mutex");
        return APP_ERR_PROTO_MUTEX_CREATE;
    }

    g_proto.ack_event = xEventGroupCreate();
    if (!g_proto.ack_event) {
        vSemaphoreDelete(g_proto.mutex);
        g_proto.mutex = NULL;
        return APP_ERR_PROTO_QUEUE_CREATE;
    }

    g_proto.uart_num = uart_num;
    g_proto.dev_id = PROTO_DEV_ID_DEFAULT;
    g_proto.seq = 0;
    g_proto.peer_alive = true;
    g_proto.last_heartbeat_tick = xTaskGetTickCount();

    g_proto.cfg.dev_id = PROTO_DEV_ID_DEFAULT;
    g_proto.cfg.baud_rate = (baud_rate > 0) ? baud_rate : 115200;
    g_proto.cfg.enable_ack = true;
    g_proto.cfg.enable_heartbeat = true;
    g_proto.cfg.ack_timeout_ms = PROTO_ACK_TIMEOUT_MS;
    g_proto.cfg.max_retries = PROTO_MAX_RETRIES;
    g_proto.cfg.heartbeat_interval_ms = PROTO_HEARTBEAT_INTERVAL_MS;
    g_proto.cfg.debug_dump = false;

    uart_config_t uart_cfg = {
        .baud_rate = g_proto.cfg.baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    esp_err_t err = uart_param_config(uart_num, &uart_cfg);
    if (err != ESP_OK) {
        LOG_ERROR("PROTO", "UART param config failed: 0x%x", err);
        vSemaphoreDelete(g_proto.mutex);
        vEventGroupDelete(g_proto.ack_event);
        g_proto.mutex = NULL;
        g_proto.ack_event = NULL;
        return APP_ERR_PROTO_UART_CONFIG;
    }

    err = uart_set_pin(uart_num, tx_pin, rx_pin,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        LOG_ERROR("PROTO", "UART set pin failed: 0x%x", err);
        vSemaphoreDelete(g_proto.mutex);
        vEventGroupDelete(g_proto.ack_event);
        g_proto.mutex = NULL;
        g_proto.ack_event = NULL;
        return APP_ERR_PROTO_UART_CONFIG;
    }

    err = uart_driver_install(uart_num, PROTO_RX_BUF_SIZE,
                              PROTO_TX_BUF_SIZE, 0, NULL, 0);
    if (err != ESP_OK) {
        LOG_ERROR("PROTO", "UART driver install failed: 0x%x", err);
        vSemaphoreDelete(g_proto.mutex);
        vEventGroupDelete(g_proto.ack_event);
        g_proto.mutex = NULL;
        g_proto.ack_event = NULL;
        return APP_ERR_PROTO_UART_INSTALL;
    }

    g_proto.initialized = true;
    LOG_INFO("PROTO", "Protocol initialized on UART%d, baud=%d, dev_id=%d",
             uart_num, g_proto.cfg.baud_rate, g_proto.dev_id);
    return APP_ERR_OK;
}

int protocol_deinit(void)
{
    if (!g_proto.initialized) return APP_ERR_PROTO_NOT_INIT;

    protocol_stop();

    if (g_proto.uart_num >= 0) {
        uart_driver_delete(g_proto.uart_num);
        g_proto.uart_num = -1;
    }

    if (g_proto.mutex) {
        vSemaphoreDelete(g_proto.mutex);
        g_proto.mutex = NULL;
    }

    if (g_proto.ack_event) {
        vEventGroupDelete(g_proto.ack_event);
        g_proto.ack_event = NULL;
    }

    g_proto.callback_count = 0;
    memset(g_proto.callbacks, 0, sizeof(g_proto.callbacks));
    g_proto.error_cb = NULL;
    g_proto.initialized = false;

    LOG_INFO("PROTO", "Protocol deinitialized");
    return APP_ERR_OK;
}

bool protocol_is_initialized(void)
{
    return g_proto.initialized;
}

int protocol_start(void)
{
    if (!g_proto.initialized) return APP_ERR_PROTO_NOT_INIT;

    BaseType_t ret = xTaskCreate(rx_task_func, "proto_rx",
                                  PROTO_TASK_STACK_SIZE, NULL,
                                  PROTO_TASK_PRIORITY, &g_proto.rx_task);
    if (ret != pdPASS) {
        LOG_ERROR("PROTO", "Failed to create RX task");
        return APP_ERR_PROTO_TASK_CREATE;
    }

    if (g_proto.cfg.enable_heartbeat) {
        ret = xTaskCreate(heartbeat_task_func, "proto_hb",
                          PROTO_TASK_STACK_SIZE, NULL,  /* 使用4KB栈大小,防止心跳函数调用链过深 */
                          PROTO_TASK_PRIORITY + 1, &g_proto.heartbeat_task);
        if (ret != pdPASS) {
            LOG_WARN("PROTO", "Failed to create heartbeat task");
            g_proto.heartbeat_task = NULL;
        }
    }

    LOG_INFO("PROTO", "Protocol tasks started");
    return APP_ERR_OK;
}

int protocol_stop(void)
{
    if (!g_proto.initialized) return APP_ERR_PROTO_NOT_INIT;

    if (g_proto.ack_event) {
        xEventGroupSetBits(g_proto.ack_event, STOP_RX_BIT | STOP_HB_BIT);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    g_proto.rx_task = NULL;
    g_proto.heartbeat_task = NULL;

    LOG_INFO("PROTO", "Protocol tasks stopped");
    return APP_ERR_OK;
}

int protocol_send(uint8_t cmd, const uint8_t *data, uint16_t len)
{
    if (!g_proto.initialized) return APP_ERR_PROTO_NOT_INIT;
    if (len > PROTO_MAX_DATA_LEN) return APP_ERR_PROTO_FRAME_OVERFLOW;

    uint8_t frame[PROTO_MAX_FRAME_LEN];
    uint16_t frame_len = 0;

    xSemaphoreTake(g_proto.mutex, portMAX_DELAY);
    uint8_t seq = ++g_proto.seq;
    xSemaphoreGive(g_proto.mutex);

    int ret = build_frame(cmd, seq, data, len, frame, &frame_len);
    if (ret != APP_ERR_OK) return ret;

    if (g_proto.cfg.debug_dump) {
        protocol_dump_frame(frame, frame_len, "TX");
    }

    uart_send_raw(frame, frame_len);
    update_stats_tx(frame_len);

    return APP_ERR_OK;
}

int protocol_send_with_ack(uint8_t cmd, const uint8_t *data, uint16_t len,
                            int timeout_ms)
{
    if (!g_proto.initialized) return APP_ERR_PROTO_NOT_INIT;
    if (!g_proto.cfg.enable_ack) {
        return protocol_send(cmd, data, len);
    }

    if (timeout_ms <= 0) timeout_ms = g_proto.cfg.ack_timeout_ms;

    xSemaphoreTake(g_proto.mutex, portMAX_DELAY);
    uint8_t seq = ++g_proto.seq;
    g_proto.ack_cmd_waiting = cmd;
    g_proto.ack_seq_waiting = seq;
    xEventGroupClearBits(g_proto.ack_event, ACK_RECEIVED_BIT);
    xSemaphoreGive(g_proto.mutex);

    uint8_t max_retry = g_proto.cfg.max_retries;
    int retry = 0;

    for (retry = 0; retry <= max_retry; retry++) {
        if (retry > 0) {
            xSemaphoreTake(g_proto.mutex, portMAX_DELAY);
            g_proto.stats.retries++;
            xSemaphoreGive(g_proto.mutex);
            LOG_WARN("PROTO", "Retry send cmd=0x%02x attempt %d/%d",
                     cmd, retry, max_retry);
        }

        uint8_t frame[PROTO_MAX_FRAME_LEN];
        uint16_t frame_len = 0;
        int ret = build_frame(cmd, seq, data, len, frame, &frame_len);
        if (ret != APP_ERR_OK) return ret;

        if (g_proto.cfg.debug_dump) {
            protocol_dump_frame(frame, frame_len, "TX");
        }

        uart_send_raw(frame, frame_len);
        update_stats_tx(frame_len);

        EventBits_t bits = xEventGroupWaitBits(
            g_proto.ack_event, ACK_RECEIVED_BIT,
            pdTRUE, pdTRUE,
            pdMS_TO_TICKS(timeout_ms));

        if (bits & ACK_RECEIVED_BIT) {
            xSemaphoreTake(g_proto.mutex, portMAX_DELAY);
            g_proto.ack_cmd_waiting = 0;
            g_proto.ack_seq_waiting = 0;
            xSemaphoreGive(g_proto.mutex);
            return APP_ERR_OK;
        }
    }

    xSemaphoreTake(g_proto.mutex, portMAX_DELAY);
    g_proto.ack_cmd_waiting = 0;
    g_proto.ack_seq_waiting = 0;
    g_proto.stats.ack_timeouts++;
    xSemaphoreGive(g_proto.mutex);

    LOG_ERROR("PROTO", "ACK timeout for cmd=0x%02x after %d retries",
              cmd, max_retry);
    notify_error(APP_ERR_PROTO_ACK_TIMEOUT, "ACK timeout");
    return APP_ERR_PROTO_ACK_TIMEOUT;
}

int protocol_send_heartbeat(void)
{
    if (!g_proto.initialized) return APP_ERR_PROTO_NOT_INIT;

    uint8_t hb_data[] = { g_proto.dev_id };
    return protocol_send(CMD_HEARTBEAT, hb_data, sizeof(hb_data));
}

int protocol_register_callback(uint8_t cmd, proto_callback_t cb,
                                void *user_data)
{
    if (!cb) return APP_ERR_INVALID_PARAM;
    if (!g_proto.initialized) return APP_ERR_PROTO_NOT_INIT;
    if (g_proto.callback_count >= PROTO_MAX_CALLBACKS)
        return APP_ERR_PROTO_CALLBACK_FULL;

    xSemaphoreTake(g_proto.mutex, portMAX_DELAY);
    g_proto.callbacks[g_proto.callback_count].cb = cb;
    g_proto.callbacks[g_proto.callback_count].user_data = user_data;
    g_proto.callback_count++;
    xSemaphoreGive(g_proto.mutex);

    LOG_DEBUG("PROTO", "Callback registered for cmd=0x%02x (total=%d)",
             cmd, g_proto.callback_count);
    return APP_ERR_OK;
}

int protocol_unregister_callback(uint8_t cmd, proto_callback_t cb)
{
    if (!cb) return APP_ERR_INVALID_PARAM;
    if (!g_proto.initialized) return APP_ERR_PROTO_NOT_INIT;

    xSemaphoreTake(g_proto.mutex, portMAX_DELAY);
    for (int i = 0; i < g_proto.callback_count; i++) {
        if (g_proto.callbacks[i].cb == cb) {
            for (int j = i; j < g_proto.callback_count - 1; j++) {
                g_proto.callbacks[j] = g_proto.callbacks[j + 1];
            }
            g_proto.callback_count--;
            g_proto.callbacks[g_proto.callback_count].cb = NULL;
            g_proto.callbacks[g_proto.callback_count].user_data = NULL;
            xSemaphoreGive(g_proto.mutex);
            return APP_ERR_OK;
        }
    }
    xSemaphoreGive(g_proto.mutex);
    return APP_ERR_NOT_SUPPORTED;
}

int protocol_register_error_callback(proto_error_callback_t cb)
{
    if (!g_proto.initialized) return APP_ERR_PROTO_NOT_INIT;
    g_proto.error_cb = cb;
    return APP_ERR_OK;
}

int protocol_get_stats(struct proto_stats *stats)
{
    if (!stats) return APP_ERR_INVALID_PARAM;
    if (!g_proto.initialized) return APP_ERR_PROTO_NOT_INIT;

    xSemaphoreTake(g_proto.mutex, portMAX_DELAY);
    memcpy(stats, &g_proto.stats, sizeof(*stats));
    xSemaphoreGive(g_proto.mutex);
    return APP_ERR_OK;
}

void protocol_reset_stats(void)
{
    xSemaphoreTake(g_proto.mutex, portMAX_DELAY);
    memset(&g_proto.stats, 0, sizeof(g_proto.stats));
    xSemaphoreGive(g_proto.mutex);
}

uint8_t protocol_get_dev_id(void)
{
    return g_proto.dev_id;
}

void protocol_set_dev_id(uint8_t dev_id)
{
    xSemaphoreTake(g_proto.mutex, portMAX_DELAY);
    g_proto.dev_id = dev_id;
    g_proto.cfg.dev_id = dev_id;
    xSemaphoreGive(g_proto.mutex);
}

bool protocol_is_peer_alive(void)
{
    bool alive;
    xSemaphoreTake(g_proto.mutex, portMAX_DELAY);
    alive = g_proto.peer_alive;
    xSemaphoreGive(g_proto.mutex);
    return alive;
}

void protocol_dump_frame(const uint8_t *frame, uint16_t len,
                         const char *direction)
{
    static char hexbuf[512];
    int pos = 0;
    for (uint16_t i = 0; i < len && pos < (int)sizeof(hexbuf) - 4; i++) {
        pos += snprintf(hexbuf + pos, sizeof(hexbuf) - pos, "%02x ", frame[i]);
    }
    LOG_DEBUG("PROTO", "[%s] [%d B] %s", direction, len, hexbuf);
}

void protocol_dump_stats(void)
{
    struct proto_stats s;
    if (protocol_get_stats(&s) == APP_ERR_OK) {
        LOG_INFO("PROTO", "--- Protocol Statistics ---");
        LOG_INFO("PROTO", "TX : %u frames / %u bytes", s.tx_frames, s.tx_bytes);
        LOG_INFO("PROTO", "RX : %u frames / %u bytes", s.rx_frames, s.rx_bytes);
        LOG_INFO("PROTO", "ERR: CRC=%u Frame=%u AckTimeout=%u Retry=%u",
                 s.crc_errors, s.frame_errors, s.ack_timeouts, s.retries);
        LOG_INFO("PROTO", "HB : sent=%u received=%u",
                 s.heartbeat_sent, s.heartbeat_received);
        LOG_INFO("PROTO", "ACK received: %u", s.ack_received);
        LOG_INFO("PROTO", "Peer alive: %s",
                 protocol_is_peer_alive() ? "YES" : "NO");
        LOG_INFO("PROTO", "Temp callbacks: %d", g_proto.temp_callback_count);
        LOG_INFO("PROTO", "Time sync: %s",
                 g_proto.time_sync.is_synchronized ? "YES" : "NO");
        LOG_INFO("PROTO", "---------------------------");
    }
}

/* ==================== 温湿度传感器 API 实现 ==================== */

int protocol_register_temp_callback(temp_data_callback_t cb, void *user_data)
{
    if (!g_proto.initialized) return APP_ERR_PROTO_NOT_INIT;
    if (!cb || g_proto.temp_callback_count >= 8) return APP_ERR_INVALID_PARAM;

    xSemaphoreTake(g_proto.mutex, portMAX_DELAY);
    g_proto.temp_callbacks[g_proto.temp_callback_count].cb = cb;
    g_proto.temp_callbacks[g_proto.temp_callback_count].user_data = user_data;
    g_proto.temp_callback_count++;
    xSemaphoreGive(g_proto.mutex);

    LOG_INFO("PROTO", "Temp callback registered (total=%d)",
             g_proto.temp_callback_count);
    return APP_ERR_OK;
}

int protocol_unregister_temp_callback(temp_data_callback_t cb)
{
    if (!cb) return APP_ERR_INVALID_PARAM;
    if (!g_proto.initialized) return APP_ERR_PROTO_NOT_INIT;

    xSemaphoreTake(g_proto.mutex, portMAX_DELAY);
    for (int i = 0; i < g_proto.temp_callback_count; i++) {
        if (g_proto.temp_callbacks[i].cb == cb) {
            for (int j = i; j < g_proto.temp_callback_count - 1; j++) {
                g_proto.temp_callbacks[j] = g_proto.temp_callbacks[j + 1];
            }
            g_proto.temp_callback_count--;
            xSemaphoreGive(g_proto.mutex);
            return APP_ERR_OK;
        }
    }
    xSemaphoreGive(g_proto.mutex);
    return APP_ERR_NOT_SUPPORTED;
}

int protocol_request_temp_data(void)
{
    if (!g_proto.initialized) return APP_ERR_PROTO_NOT_INIT;

    uint8_t cmd = CMD_CONFIG_GET;
    return protocol_send_with_ack(cmd, &cmd, 1, 1000);
}

int protocol_get_latest_temp(struct temp_humidity_data *out)
{
    if (!out) return APP_ERR_INVALID_PARAM;
    if (!g_proto.initialized) return APP_ERR_PROTO_NOT_INIT;

    xSemaphoreTake(g_proto.mutex, portMAX_DELAY);
    if (!g_proto.has_latest_temp) {
        xSemaphoreGive(g_proto.mutex);
        return APP_ERR_PROTO_NO_DATA;
    }
    memcpy(out, &g_proto.latest_temp, sizeof(*out));
    xSemaphoreGive(g_proto.mutex);

    return APP_ERR_OK;
}

/* ==================== 时间同步 API 实现 ==================== */

int protocol_register_time_sync_callback(proto_time_sync_callback_t cb,
                                          void *user_data)
{
    if (!g_proto.initialized) return APP_ERR_PROTO_NOT_INIT;
    if (!cb || g_proto.time_sync_callback_count >= 4) return APP_ERR_INVALID_PARAM;

    xSemaphoreTake(g_proto.mutex, portMAX_DELAY);
    g_proto.time_sync_callbacks[g_proto.time_sync_callback_count].cb = cb;
    g_proto.time_sync_callbacks[g_proto.time_sync_callback_count].user_data = user_data;
    g_proto.time_sync_callback_count++;
    xSemaphoreGive(g_proto.mutex);

    return APP_ERR_OK;
}

int protocol_initiate_time_sync(void)
{
    if (!g_proto.initialized) return APP_ERR_PROTO_NOT_INIT;

    int64_t esp32_now_us = esp_timer_get_time();
    uint32_t esp32_now_ms = (uint32_t)(esp32_now_us / 1000LL);

    uint8_t data[4];
    memcpy(data, &esp32_now_ms, sizeof(uint32_t));

    int ret = protocol_send(CMD_TIME_SYNC_REQ, data, sizeof(data));
    if (ret != APP_ERR_OK) {
        xSemaphoreTake(g_proto.mutex, portMAX_DELAY);
        g_proto.time_sync.consecutive_failures++;
        xSemaphoreGive(g_proto.mutex);
        LOG_WARN("PROTO", "Time sync request failed");
    }

    return ret;
}

int protocol_get_time_sync_info(struct time_sync_info *info)
{
    if (!info) return APP_ERR_INVALID_PARAM;
    if (!g_proto.initialized) return APP_ERR_PROTO_NOT_INIT;

    xSemaphoreTake(g_proto.mutex, portMAX_DELAY);
    memcpy(info, &g_proto.time_sync, sizeof(*info));
    xSemaphoreGive(g_proto.mutex);

    return APP_ERR_OK;
}

int protocol_convert_stm32_to_esp32_time(uint32_t stm32_timestamp_ms,
                                          int64_t *esp32_timestamp_us)
{
    if (!esp32_timestamp_us) return APP_ERR_INVALID_PARAM;
    if (!g_proto.initialized) return APP_ERR_PROTO_NOT_INIT;

    xSemaphoreTake(g_proto.mutex, portMAX_DELAY);
    if (!g_proto.time_sync.is_synchronized) {
        xSemaphoreGive(g_proto.mutex);
        return APP_ERR_PROTO_TIME_SYNC_PENDING;
    }

    int64_t stm32_us = (int64_t)stm32_timestamp_ms * 1000LL;
    *esp32_timestamp_us = stm32_us + g_proto.time_sync.stm32_epoch_offset_us;
    xSemaphoreGive(g_proto.mutex);

    return APP_ERR_OK;
}

/* ==================== 获取最新温湿度数据 API ==================== */

int proto_get_last_temp_humidity(float *temp_c, float *humidity_rh)
{
    if (!g_proto.initialized) return APP_ERR_PROTO_NOT_INIT;
    if (!temp_c || !humidity_rh) return APP_ERR_INVALID_PARAM;

    xSemaphoreTake(g_proto.mutex, portMAX_DELAY);

    if (!g_proto.has_latest_temp) {
        xSemaphoreGive(g_proto.mutex);
        *temp_c = 0.0f;
        *humidity_rh = 0.0f;
        return APP_ERR_PROTO_NO_DATA;
    }

    *temp_c = g_proto.latest_temp.temperature_c;
    *humidity_rh = g_proto.latest_temp.humidity_rh;

    xSemaphoreGive(g_proto.mutex);

    return APP_ERR_OK;
}