/**
 * @file proto_to_json.c
 * @brief RS232 Gateway — 协议帧 → JSON + MQTT Topic 路由
 *
 * 所有 JSON 通过 snprintf() 构建, 不依赖外部 JSON 库。
 * STM32F407 和 Orange Pi (aarch64) 均为 little-endian,
 * 因此 memcpy() 直接解释多字节字段。
 *
 * JSON 输出格式 (所有 payload 共有字段):
 *   {"ts":<unix_sec>,"source":"rs232","dev_id":"<str>","cmd":"<name>",
 *    "data":{<cmd-specific>}}
 */

#include "proto_to_json.h"
#include "protocol.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

/* ==================== dev_id → 字符串映射 ==================== */

static const char *dev_id_to_str(uint8_t dev_id)
{
    switch (dev_id) {
        case 0x01: return "de01";
        case 0x02: return "nde01";
        default: {
            static char buf[16];
            snprintf(buf, sizeof(buf), "dev_0x%02X", dev_id);
            return buf;
        }
    }
}

/* ==================== CMD → 名称 ==================== */

static const char *cmd_to_name(uint8_t cmd)
{
    switch (cmd) {
        case PROTO_CMD_TEMP_HUMIDITY_DATA:  return "temp_humidity";
        case PROTO_CMD_MOTOR_STATUS_RESP:   return "motor_status";
        case PROTO_CMD_SYSTEM_STATUS:       return "system_status";
        case PROTO_CMD_EMERGENCY_EVENT:     return "emergency";
        case PROTO_CMD_NDE_FEATURE:         return "nde_feature";
        case PROTO_CMD_NDE_HEARTBEAT:       return "nde_heartbeat";
        case PROTO_CMD_DUAL_DIAG:           return "dual_diag";
        case PROTO_CMD_HEARTBEAT:           return "heartbeat";
        default:                            return NULL;
    }
}

/* ==================== CMD → Topic 后缀 ==================== */

static const char *cmd_to_topic_suffix(uint8_t cmd)
{
    switch (cmd) {
        case PROTO_CMD_TEMP_HUMIDITY_DATA:
        case PROTO_CMD_NDE_FEATURE:
        case PROTO_CMD_DUAL_DIAG:
            return "data/sensor";

        case PROTO_CMD_MOTOR_STATUS_RESP:
            return "data/motor";

        case PROTO_CMD_SYSTEM_STATUS:
        case PROTO_CMD_NDE_HEARTBEAT:
        case PROTO_CMD_HEARTBEAT:
            return "status/health";

        case PROTO_CMD_EMERGENCY_EVENT:
            return "status/emergency";

        default:
            return "data/generic";
    }
}

/* ==================== 安全 memcpy 读取整数 ==================== */

static int32_t read_le_i32(const uint8_t *p)
{
    int32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint32_t read_le_u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static float read_le_float(const uint8_t *p)
{
    float v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static int16_t read_le_i16(const uint8_t *p)
{
    int16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

/* ==================== Base64 编码 ==================== */

static const char g_b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_encode(const uint8_t *src, size_t src_len,
                         char *dst, size_t dst_size)
{
    size_t out_len = ((src_len + 2) / 3) * 4;
    if (out_len + 1 > dst_size)
        return -1;

    size_t i = 0, j = 0;
    while (i < src_len) {
        uint32_t triple = (uint32_t)(src[i]) << 16;
        if (i + 1 < src_len) triple |= (uint32_t)(src[i + 1]) << 8;
        if (i + 2 < src_len) triple |= (uint32_t)(src[i + 2]);

        dst[j++] = g_b64_table[(triple >> 18) & 0x3F];
        dst[j++] = g_b64_table[(triple >> 12) & 0x3F];
        dst[j++] = (i + 1 < src_len) ? g_b64_table[(triple >> 6) & 0x3F] : '=';
        dst[j++] = (i + 2 < src_len) ? g_b64_table[triple & 0x3F] : '=';

        i += 3;
    }
    dst[j] = '\0';
    return (int)j;
}

/* ==================== 各 CMD 的 JSON data 构建 ==================== */

static int build_json_temp_humidity(const uint8_t *payload,
                                     char *buf, size_t buf_size)
{
    float temp_c      = read_le_float(&payload[0]);
    float humidity_rh = read_le_float(&payload[4]);
    uint32_t ts_ms    = read_le_u32(&payload[8]);
    uint8_t s_type    = payload[12];
    uint8_t s_status  = payload[13];
    int16_t raw_adc   = read_le_i16(&payload[14]);

    return snprintf(buf, buf_size,
        "\"temp_c\":%.2f,\"humidity_rh\":%.2f,\"timestamp_ms\":%u,"
        "\"sensor_type\":%u,\"sensor_status\":%u,\"raw_adc_value\":%d",
        temp_c, humidity_rh, ts_ms, s_type, s_status, raw_adc);
}

static int build_json_motor_status(const uint8_t *payload,
                                    char *buf, size_t buf_size)
{
    int32_t rpm        = read_le_i32(&payload[MOTOR_OFF_RPM]);
    int32_t current_ma = read_le_i32(&payload[MOTOR_OFF_CURRENT_MA]);
    int32_t bus_mv     = read_le_i32(&payload[MOTOR_OFF_BUS_MV]);
    int32_t temp_dc    = read_le_i32(&payload[MOTOR_OFF_TEMP_DC]);
    uint8_t state      = payload[MOTOR_OFF_STATE];
    uint8_t fault      = payload[MOTOR_OFF_FAULT];
    int32_t duty       = read_le_i32(&payload[MOTOR_OFF_DUTY]);
    int8_t  direction  = (int8_t)payload[MOTOR_OFF_DIRECTION];
    uint8_t pid_active = payload[MOTOR_OFF_PID_ACTIVE];

    return snprintf(buf, buf_size,
        "\"rpm\":%d,\"current_ma\":%d,\"bus_mv\":%d,"
        "\"temp_dc\":%.1f,\"state\":%u,\"fault\":%u,\"duty\":%d,"
        "\"direction\":%d,\"pid_active\":%s",
        rpm, current_ma, bus_mv,
        temp_dc * 0.1, state, fault, duty,
        direction, pid_active ? "true" : "false");
}

static int build_json_system_status(const uint8_t *payload,
                                     char *buf, size_t buf_size)
{
    uint8_t system_state   = payload[0];
    uint8_t operation_mode = payload[1];
    uint8_t e_stop_state   = payload[2];
    uint8_t health_level   = payload[3];
    uint8_t event_source   = payload[4];

    return snprintf(buf, buf_size,
        "\"system_state\":%u,\"operation_mode\":%u,\"e_stop_state\":%u,"
        "\"health_level\":%u,\"event_source\":%u",
        system_state, operation_mode, e_stop_state, health_level, event_source);
}

static int build_json_emergency(const uint8_t *payload,
                                 char *buf, size_t buf_size)
{
    uint32_t event_code = read_le_u32(&payload[0]);
    uint32_t severity   = read_le_u32(&payload[4]);

    return snprintf(buf, buf_size,
        "\"event_code\":%u,\"severity\":%u", event_code, severity);
}

static int build_json_nde_feature(const uint8_t *payload,
                                   char *buf, size_t buf_size)
{
    uint8_t window_idx = payload[0];
    size_t off = 4;  /* skip 4-byte header */

    int written = snprintf(buf, buf_size,
        "\"window_idx\":%u,\"features\":[", window_idx);
    if (written < 0 || (size_t)written >= buf_size)
        return -1;

    size_t remain = buf_size - written;
    char *p = buf + written;
    int total = written;

    for (int i = 0; i < NDE_FEATURE_VEC_DIM; i++) {
        float f = read_le_float(&payload[off + (unsigned)i * 4]);
        int n = snprintf(p, remain, "%s%.6f",
                         (i > 0) ? "," : "", (double)f);
        if (n < 0 || (size_t)n >= remain)
            return -1;
        p += n;
        remain -= n;
        total += n;
    }

    int n = snprintf(p, remain, "]");
    if (n < 0 || (size_t)n >= remain)
        return -1;
    return total + n;
}

static int build_json_nde_heartbeat(const uint8_t *payload,
                                     char *buf, size_t buf_size)
{
    uint8_t online      = payload[0];
    uint8_t error_count = payload[1];
    int8_t  temp_c      = (int8_t)payload[2];

    return snprintf(buf, buf_size,
        "\"online\":%s,\"error_count\":%u,\"temp_c\":%d",
        (online == 0x01) ? "true" : "false", error_count, temp_c);
}

static int build_json_heartbeat(const uint8_t *payload,
                                 char *buf, size_t buf_size)
{
    uint8_t alive = payload[0];
    return snprintf(buf, buf_size, "\"alive\":%s",
                    (alive == 0x01) ? "true" : "false");
}

static int build_json_dual_diag(const uint8_t *payload, uint16_t payload_len,
                                 char *buf, size_t buf_size)
{
    char b64[128];
    int b64_len = base64_encode(payload, payload_len, b64, sizeof(b64));
    if (b64_len < 0)
        return -1;
    return snprintf(buf, buf_size, "\"data_b64\":\"%s\"", b64);
}

static int build_json_unknown(uint8_t cmd, const uint8_t *payload,
                               uint16_t payload_len,
                               char *buf, size_t buf_size)
{
    char b64[256];
    int b64_len = base64_encode(payload, payload_len, b64, sizeof(b64));
    if (b64_len < 0)
        return -1;
    return snprintf(buf, buf_size,
        "\"cmd_hex\":\"0x%02X\",\"data_b64\":\"%s\"", cmd, b64);
}

/* ==================== 主转换函数 ==================== */

int proto_to_json(uint8_t cmd, uint8_t dev_id,
                  const uint8_t *payload, uint16_t payload_len,
                  char *json_out, size_t json_size, size_t *json_len,
                  char *topic_out, size_t topic_size,
                  const char *topic_prefix)
{
    if (!json_out || !json_len || !topic_out || !topic_prefix)
        return -2;
    if (!payload && payload_len > 0)
        return -2;

    char data_buf[3072];
    int data_len = 0;
    const char *cmd_name = cmd_to_name(cmd);

    /* 构建 data 部分 */
    switch (cmd) {
    case PROTO_CMD_TEMP_HUMIDITY_DATA:
        if (payload_len < TEMP_HUMIDITY_PAYLOAD_LEN) return -2;
        data_len = build_json_temp_humidity(payload, data_buf, sizeof(data_buf));
        break;

    case PROTO_CMD_MOTOR_STATUS_RESP:
        if (payload_len < MOTOR_STATUS_PAYLOAD_LEN) return -2;
        data_len = build_json_motor_status(payload, data_buf, sizeof(data_buf));
        break;

    case PROTO_CMD_SYSTEM_STATUS:
        if (payload_len < SYSTEM_STATUS_PAYLOAD_LEN) return -2;
        data_len = build_json_system_status(payload, data_buf, sizeof(data_buf));
        break;

    case PROTO_CMD_EMERGENCY_EVENT:
        if (payload_len < EMERGENCY_EVENT_PAYLOAD_LEN) return -2;
        data_len = build_json_emergency(payload, data_buf, sizeof(data_buf));
        break;

    case PROTO_CMD_NDE_FEATURE:
        if (payload_len < NDE_FEATURE_PAYLOAD_LEN) return -2;
        data_len = build_json_nde_feature(payload, data_buf, sizeof(data_buf));
        break;

    case PROTO_CMD_NDE_HEARTBEAT:
        if (payload_len < NDE_HEARTBEAT_PAYLOAD_LEN) return -2;
        data_len = build_json_nde_heartbeat(payload, data_buf, sizeof(data_buf));
        break;

    case PROTO_CMD_DUAL_DIAG:
        cmd_name = "dual_diag";
        data_len = build_json_dual_diag(payload, payload_len,
                                        data_buf, sizeof(data_buf));
        break;

    case PROTO_CMD_HEARTBEAT:
        if (payload_len < HEARTBEAT_PAYLOAD_LEN) return -2;
        data_len = build_json_heartbeat(payload, data_buf, sizeof(data_buf));
        break;

    default:
        cmd_name = NULL;
        data_len = build_json_unknown(cmd, payload, payload_len,
                                      data_buf, sizeof(data_buf));
        break;
    }

    if (data_len < 0)
        return -3;

    /* 构建完整 JSON */
    time_t ts = time(NULL);
    const char *dev_str = dev_id_to_str(dev_id);

    char cmd_field[32];
    if (cmd_name) {
        snprintf(cmd_field, sizeof(cmd_field), "\"cmd\":\"%s\"", cmd_name);
    } else {
        snprintf(cmd_field, sizeof(cmd_field), "\"cmd\":\"unknown_0x%02X\"", cmd);
    }

    int json_total = snprintf(json_out, json_size,
        "{\"ts\":%lld,\"source\":\"rs232\",\"dev_id\":\"%s\",%s,\"data\":{%s}}",
        (long long)ts, dev_str, cmd_field, data_buf);

    if (json_total < 0 || (size_t)json_total >= json_size)
        return -3;

    *json_len = (size_t)json_total;

    /* 构建 MQTT Topic */
    const char *suffix = cmd_to_topic_suffix(cmd);
    int topic_len = snprintf(topic_out, topic_size, "%s/%s/%s",
                             topic_prefix, dev_str, suffix);
    if (topic_len < 0 || (size_t)topic_len >= topic_size)
        return -3;

    return (cmd_name) ? 0 : -1;
}
