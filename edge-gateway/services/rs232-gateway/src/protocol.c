/**
 * @file protocol.c
 * @brief RS232 Gateway — CRC16-MODBUS + 10 状态帧解析器
 *
 * 从以下固件文件移植:
 *   firmware/stm32_node_vibration/Modules/protocol/protocol.c    (CRC16)
 *   firmware/stm32_node_vibration/Modules/protocol/protocol_parser.c (状态机)
 *
 * 修改点:
 *   - HAL_GetTick() → clock_gettime(CLOCK_MONOTONIC) 封装的 millis()
 *   - FreeRTOS mutex   → pthread_mutex_t
 *   - 移除 heartbeat peer 检测 (RS232 gateway 只是消费者)
 *   - 移除 ACK/NACK 发送逻辑 (只收不发)
 *   - 统计增加 bytes_raw 和 last_frame_ts_ms 字段
 */

#include "protocol.h"
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <stdio.h>

/* ==================== 时间工具 ==================== */

static uint64_t millis(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* ==================== CRC16-MODBUS ==================== */

uint16_t proto_crc16_modbus(const uint8_t *data, uint16_t len)
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

/* ==================== 内部状态 ==================== */

static struct {
    enum proto_state state;
    uint8_t  frame[PROTO_PARSE_MAX_DATA_LEN];
    uint16_t data_idx;
    uint16_t expected_len;

    uint8_t  dev_id;
    uint8_t  cmd;
    uint8_t  seq;
    uint16_t recv_crc;

    struct {
        proto_parse_callback_t cb;
        void *user_data;
    } callbacks[256];

    struct proto_parse_stats stats;

    pthread_mutex_t mutex;
    bool initialized;
} g_ps;

/* ==================== 内部帧派发 ==================== */

static void dispatch_frame(uint8_t cmd, const uint8_t *data, uint16_t len)
{
    if (cmd == PROTO_CMD_HEARTBEAT)
        return;

    if (g_ps.callbacks[cmd].cb) {
        g_ps.callbacks[cmd].cb(cmd, g_ps.dev_id, data, len,
                               g_ps.callbacks[cmd].user_data);
    } else {
        g_ps.stats.cmds_unknown++;
    }
}

/* ==================== 初始化/反初始化 ==================== */

int proto_parse_init(void)
{
    memset(&g_ps, 0, sizeof(g_ps));
    g_ps.state = S_WAIT_HEADER_H;

    if (pthread_mutex_init(&g_ps.mutex, NULL) != 0)
        return -1;

    g_ps.initialized = true;
    return 0;
}

void proto_parse_deinit(void)
{
    if (!g_ps.initialized)
        return;
    pthread_mutex_destroy(&g_ps.mutex);
    memset(&g_ps, 0, sizeof(g_ps));
}

/* ==================== 回调注册 ==================== */

int proto_parse_register(uint8_t cmd, proto_parse_callback_t cb, void *user_data)
{
    if (!cb)
        return -1;
    if (cmd == PROTO_CMD_HEARTBEAT)
        return -1;

    pthread_mutex_lock(&g_ps.mutex);
    g_ps.callbacks[cmd].cb = cb;
    g_ps.callbacks[cmd].user_data = user_data;
    pthread_mutex_unlock(&g_ps.mutex);
    return 0;
}

int proto_parse_unregister(uint8_t cmd)
{
    pthread_mutex_lock(&g_ps.mutex);
    g_ps.callbacks[cmd].cb = NULL;
    g_ps.callbacks[cmd].user_data = NULL;
    pthread_mutex_unlock(&g_ps.mutex);
    return 0;
}

/* ==================== 统计 ==================== */

void proto_parse_get_stats(struct proto_parse_stats *stats)
{
    if (!stats)
        return;
    pthread_mutex_lock(&g_ps.mutex);
    memcpy(stats, &g_ps.stats, sizeof(*stats));
    pthread_mutex_unlock(&g_ps.mutex);
}

void proto_parse_reset_stats(void)
{
    pthread_mutex_lock(&g_ps.mutex);
    memset(&g_ps.stats, 0, sizeof(g_ps.stats));
    pthread_mutex_unlock(&g_ps.mutex);
}

/* ==================== 帧调试输出 ==================== */

void proto_dump_frame(const char *tag, const uint8_t *frame, uint16_t len)
{
    if (!frame || len == 0)
        return;

    fprintf(stderr, "[%s] %u bytes: ", tag ? tag : "FRAME", len);
    for (uint16_t i = 0; i < len; i++)
        fprintf(stderr, "%02X ", frame[i]);
    fprintf(stderr, "\n");
}

/* ==================== 核心: 10 状态机 (逐字节喂入) ==================== */

void proto_parse_feed(uint8_t byte)
{
    if (!g_ps.initialized)
        return;

    g_ps.stats.bytes_raw++;

    switch (g_ps.state) {

    case S_WAIT_HEADER_H:
        if (byte == ((PROTO_HEADER >> 8) & 0xFF))
            g_ps.state = S_WAIT_HEADER_L;
        break;

    case S_WAIT_HEADER_L:
        if (byte == (PROTO_HEADER & 0xFF))
            g_ps.state = S_WAIT_LEN_H;
        else
            g_ps.state = S_WAIT_HEADER_H;
        break;

    case S_WAIT_LEN_H:
        g_ps.expected_len = (uint16_t)byte << 8;
        g_ps.state = S_WAIT_LEN_L;
        break;

    case S_WAIT_LEN_L:
        g_ps.expected_len |= byte;
        if (g_ps.expected_len > PROTO_PARSE_MAX_DATA_LEN) {
            g_ps.stats.frame_errors++;
            g_ps.state = S_WAIT_HEADER_H;
        } else {
            g_ps.state = S_WAIT_DEV_ID;
            g_ps.data_idx = 0;
        }
        break;

    case S_WAIT_DEV_ID:
        g_ps.dev_id = byte;
        g_ps.state = S_WAIT_CMD;
        break;

    case S_WAIT_CMD:
        g_ps.cmd = byte;
        g_ps.state = S_WAIT_SEQ;
        break;

    case S_WAIT_SEQ:
        g_ps.seq = byte;
        g_ps.data_idx = 0;
        g_ps.state = (g_ps.expected_len > 0) ? S_WAIT_DATA : S_WAIT_CRC_H;
        break;

    case S_WAIT_DATA:
        if (g_ps.data_idx < g_ps.expected_len) {
            g_ps.frame[g_ps.data_idx++] = byte;
            if (g_ps.data_idx >= g_ps.expected_len)
                g_ps.state = S_WAIT_CRC_H;
        } else {
            g_ps.state = S_WAIT_HEADER_H;
        }
        break;

    case S_WAIT_CRC_H:
        g_ps.recv_crc = (uint16_t)byte << 8;
        g_ps.state = S_WAIT_CRC_L;
        break;

    case S_WAIT_CRC_L:
        g_ps.recv_crc |= byte;
        g_ps.state = S_WAIT_TAIL;
        break;

    case S_WAIT_TAIL:
        if (byte == PROTO_TAIL) {
            /* 构建 CRC 校验缓冲区: LEN(2) + DEV(1) + CMD(1) + SEQ(1) + DATA(N) */
            uint8_t crc_buf[PROTO_PARSE_MAX_DATA_LEN + 5];
            uint16_t crb = 0;

            crc_buf[crb++] = (g_ps.expected_len >> 8) & 0xFF;
            crc_buf[crb++] = g_ps.expected_len & 0xFF;
            crc_buf[crb++] = g_ps.dev_id;
            crc_buf[crb++] = g_ps.cmd;
            crc_buf[crb++] = g_ps.seq;

            if (g_ps.expected_len > 0) {
                memcpy(&crc_buf[crb], g_ps.frame, g_ps.expected_len);
                crb += g_ps.expected_len;
            }

            uint16_t calc_crc = proto_crc16_modbus(crc_buf, crb);

            if (calc_crc == g_ps.recv_crc) {
                uint16_t frame_total = 7 + g_ps.expected_len + 2;
                g_ps.stats.rx_frames++;
                g_ps.stats.rx_bytes += frame_total;
                g_ps.stats.last_frame_ts_ms = millis();

                dispatch_frame(g_ps.cmd, g_ps.frame, g_ps.expected_len);
            } else {
                g_ps.stats.crc_errors++;
            }
        } else {
            g_ps.stats.frame_errors++;
        }
        g_ps.state = S_WAIT_HEADER_H;
        break;

    default:
        g_ps.state = S_WAIT_HEADER_H;
        break;
    }
}

/* ==================== 批量喂入 ==================== */

void proto_parse_feed_buf(const uint8_t *data, uint16_t len)
{
    if (!data || len == 0)
        return;
    for (uint16_t i = 0; i < len; i++)
        proto_parse_feed(data[i]);
}
