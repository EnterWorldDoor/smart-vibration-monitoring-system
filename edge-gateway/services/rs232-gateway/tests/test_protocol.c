/**
 * @file test_protocol.c
 * @brief RS232 Gateway — 协议解析器单元测试
 *
 * 测试覆盖: CRC16-MODBUS、10 状态机帧解析、回调分发、统计计数器
 */

#include "test_utils.h"
#include "protocol.h"
#include <string.h>

/* ==================== CRC16 已知向量测试 ==================== */

static void test_crc16_empty(void)
{
    uint8_t data[] = { 0x00 };
    uint16_t crc = proto_crc16_modbus(data, 0);
    /* 空数据的 CRC16-MODBUS 初始值保持 0xFFFF */
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, crc);
}

static void test_crc16_known_vector_1(void)
{
    /* "123456789" → CRC16-MODBUS = 0x4B37 (行业标准测试向量) */
    uint8_t data[] = { '1','2','3','4','5','6','7','8','9' };
    uint16_t crc = proto_crc16_modbus(data, sizeof(data));
    TEST_ASSERT_EQUAL_UINT16(0x4B37, crc);
}

static void test_crc16_known_vector_2(void)
{
    uint8_t data[] = { 0x01, 0x02, 0x03 };
    uint16_t crc = proto_crc16_modbus(data, sizeof(data));
    /* CRC16-MODBUS of 01 02 03 = 0x6161 */
    TEST_ASSERT_EQUAL_UINT16(0x6161, crc);
}

static void test_crc16_all_0xAA(void)
{
    /* 128 字节的 0xAA — 边界测试，不崩溃即可 */
    uint8_t data[128];
    memset(data, 0xAA, sizeof(data));
    uint16_t crc = proto_crc16_modbus(data, sizeof(data));
    TEST_ASSERT_EQUAL_UINT16(0x63FF, crc);
}

/* ==================== 帧构建辅助 ==================== */

/*
 * 手动构建已知帧用于测试解析器。
 * 使用 proto_crc16_modbus 计算 CRC，确保与固件格式一致。
 */
static int build_test_frame(uint8_t *frame, uint16_t frame_size,
                             uint8_t cmd, const uint8_t *payload,
                             uint16_t payload_len, uint8_t seq)
{
    if (payload_len + 9 > frame_size)
        return -1;

    uint16_t idx = 0;
    frame[idx++] = (PROTO_HEADER >> 8) & 0xFF;
    frame[idx++] = PROTO_HEADER & 0xFF;
    frame[idx++] = (payload_len >> 8) & 0xFF;
    frame[idx++] = payload_len & 0xFF;
    frame[idx++] = PROTO_DEVICE_ID_STM32;
    frame[idx++] = cmd;
    frame[idx++] = seq;

    if (payload_len > 0) {
        memcpy(&frame[idx], payload, payload_len);
        idx += payload_len;
    }

    /* CRC 计算范围: frame[2] 到 frame[idx-1] */
    uint16_t crc = proto_crc16_modbus(&frame[2], idx - 2);
    frame[idx++] = (crc >> 8) & 0xFF;
    frame[idx++] = crc & 0xFF;
    frame[idx++] = PROTO_TAIL;

    return (int)idx;
}

/* ==================== 完整帧解析测试 ==================== */

static void test_parse_heartbeat_frame(void)
{
    proto_parse_init();

    /* 心跳帧: cmd=0xFE, seq=1, payload=0x01 */
    uint8_t payload[] = { 0x01 };
    uint8_t frame[32];
    int frm_len = build_test_frame(frame, sizeof(frame),
                                   PROTO_CMD_HEARTBEAT, payload, 1, 1);
    TEST_ASSERT_TRUE(frm_len > 0);

    proto_parse_feed_buf(frame, (uint16_t)frm_len);

    struct proto_parse_stats stats;
    proto_parse_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(1, stats.rx_frames);
    TEST_ASSERT_EQUAL_UINT32(0, stats.crc_errors);
    TEST_ASSERT_EQUAL_UINT32(0, stats.frame_errors);

    proto_parse_deinit();
}

static void test_parse_temp_humidity_frame(void)
{
    proto_parse_init();

    /* 构建 16 字节温湿度 payload */
    uint8_t payload[16];
    memset(payload, 0, sizeof(payload));
    float temp = 25.5f;
    float hum  = 60.0f;
    uint32_t ts = 12345;
    memcpy(&payload[0], &temp, 4);
    memcpy(&payload[4], &hum, 4);
    memcpy(&payload[8], &ts, 4);
    payload[12] = PROTO_SENSOR_TYPE_SHT30;
    payload[13] = PROTO_SENSOR_STATUS_NORMAL;

    uint8_t frame[64];
    int frm_len = build_test_frame(frame, sizeof(frame),
                                   PROTO_CMD_TEMP_HUMIDITY_DATA, payload, 16, 1);
    TEST_ASSERT_TRUE(frm_len > 0);

    proto_parse_feed_buf(frame, (uint16_t)frm_len);

    struct proto_parse_stats stats;
    proto_parse_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(1, stats.rx_frames);

    proto_parse_deinit();
}

static void test_parse_motor_status_frame(void)
{
    proto_parse_init();

    uint8_t payload[26];
    memset(payload, 0, sizeof(payload));
    int32_t rpm = 1500;
    int32_t current_ma = 3500;
    int32_t bus_mv = 24000;
    int32_t temp_dc = 355;  /* 35.5°C * 10 */
    memcpy(&payload[MOTOR_OFF_RPM], &rpm, 4);
    memcpy(&payload[MOTOR_OFF_CURRENT_MA], &current_ma, 4);
    memcpy(&payload[MOTOR_OFF_BUS_MV], &bus_mv, 4);
    memcpy(&payload[MOTOR_OFF_TEMP_DC], &temp_dc, 4);
    payload[MOTOR_OFF_STATE] = 1;
    payload[MOTOR_OFF_FAULT] = 0;
    int32_t duty = 75;
    memcpy(&payload[MOTOR_OFF_DUTY], &duty, 4);
    payload[MOTOR_OFF_DIRECTION] = 1;
    payload[MOTOR_OFF_PID_ACTIVE] = 1;

    uint8_t frame[64];
    int frm_len = build_test_frame(frame, sizeof(frame),
                                   PROTO_CMD_MOTOR_STATUS_RESP, payload, 26, 2);
    TEST_ASSERT_TRUE(frm_len > 0);

    proto_parse_feed_buf(frame, (uint16_t)frm_len);

    struct proto_parse_stats stats;
    proto_parse_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(1, stats.rx_frames);

    proto_parse_deinit();
}

static void test_parse_nde_feature_frame(void)
{
    proto_parse_init();

    uint8_t payload[100];
    memset(payload, 0, sizeof(payload));
    payload[0] = 42; /* window_idx */
    /* 填充 24 个 float 特征值 */
    for (int i = 0; i < 24; i++) {
        float f = (float)(i * 0.1);
        memcpy(&payload[4 + i * 4], &f, 4);
    }

    uint8_t frame[128];
    int frm_len = build_test_frame(frame, sizeof(frame),
                                   PROTO_CMD_NDE_FEATURE, payload, 100, 3);
    TEST_ASSERT_TRUE(frm_len > 0);

    proto_parse_feed_buf(frame, (uint16_t)frm_len);

    struct proto_parse_stats stats;
    proto_parse_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(1, stats.rx_frames);

    proto_parse_deinit();
}

static void test_parse_zero_payload_frame(void)
{
    /* 心跳帧 payload 长度可以为零 (ESP32 固件允许) */
    proto_parse_init();

    /* 构建 len=0 的帧 (matching protocol.c expected_len==0 → skip S_WAIT_DATA) */
    uint8_t frame[32];
    uint16_t idx = 0;
    frame[idx++] = (PROTO_HEADER >> 8) & 0xFF;
    frame[idx++] = PROTO_HEADER & 0xFF;
    frame[idx++] = 0x00;  /* len_h = 0 */
    frame[idx++] = 0x00;  /* len_l = 0 */
    frame[idx++] = PROTO_DEVICE_ID_STM32;
    frame[idx++] = 0xFE;  /* heartbeat */
    frame[idx++] = 0x01;  /* seq */
    uint16_t crc = proto_crc16_modbus(&frame[2], idx - 2);
    frame[idx++] = (crc >> 8) & 0xFF;
    frame[idx++] = crc & 0xFF;
    frame[idx++] = PROTO_TAIL;

    proto_parse_feed_buf(frame, (uint16_t)idx);

    struct proto_parse_stats stats;
    proto_parse_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(1, stats.rx_frames);
    TEST_ASSERT_EQUAL_UINT32(0, stats.crc_errors);

    proto_parse_deinit();
}

/* ==================== 错误恢复测试 ==================== */

static void test_crc_error_rejected(void)
{
    proto_parse_init();

    uint8_t payload[] = { 0x01, 0x02 };
    uint8_t frame[32];
    int frm_len = build_test_frame(frame, sizeof(frame),
                                   PROTO_CMD_HEARTBEAT, payload, 2, 1);
    TEST_ASSERT_TRUE(frm_len > 0);

    /* 破坏 CRC (修改最后一个 CRC 字节前的位置) */
    frame[frm_len - 3] ^= 0xFF;

    proto_parse_feed_buf(frame, (uint16_t)frm_len);

    struct proto_parse_stats stats;
    proto_parse_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.rx_frames);
    TEST_ASSERT_EQUAL_UINT32(1, stats.crc_errors);

    proto_parse_deinit();
}

static void test_truncated_frame_rejected(void)
{
    proto_parse_init();

    uint8_t frame_data[] = { 0xAA, 0x55, 0x00, 0x04 }; /* 只有 header + len, 截断 */
    proto_parse_feed_buf(frame_data, sizeof(frame_data));

    /* 解析器应该在 LEN_L 状态等待更多数据, 不应崩溃 */
    struct proto_parse_stats stats;
    proto_parse_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.rx_frames);
    TEST_ASSERT_EQUAL_UINT32(0, stats.crc_errors);

    proto_parse_deinit();
}

static void test_garbage_then_valid_self_sync(void)
{
    proto_parse_init();

    /* 先发垃圾字节 */
    uint8_t garbage[] = { 0x00, 0xFF, 0x12, 0x34, 0x00, 0x00, 0x00, 0x00,
                          0x7F, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

    proto_parse_feed_buf(garbage, sizeof(garbage));

    /* 再发有效帧 */
    uint8_t payload[] = { 0x01 };
    uint8_t frame[32];
    int frm_len = build_test_frame(frame, sizeof(frame),
                                   PROTO_CMD_HEARTBEAT, payload, 1, 2);
    proto_parse_feed_buf(frame, (uint16_t)frm_len);

    struct proto_parse_stats stats;
    proto_parse_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(1, stats.rx_frames); /* 只有有效帧被计数 */

    proto_parse_deinit();
}

static void test_oversized_length_rejected(void)
{
    proto_parse_init();

    /* 手工构建超长长度字段的帧 */
    uint8_t frame[16];
    frame[0] = (PROTO_HEADER >> 8) & 0xFF;
    frame[1] = PROTO_HEADER & 0xFF;
    frame[2] = 0xFF;  /* len_h = 0xFF */
    frame[3] = 0xFF;  /* len_l = 0xFF → len=65535 > PROTO_PARSE_MAX_DATA_LEN */

    proto_parse_feed_buf(frame, 4);

    struct proto_parse_stats stats;
    proto_parse_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(1, stats.frame_errors);
    TEST_ASSERT_EQUAL_UINT32(0, stats.rx_frames);

    proto_parse_deinit();
}

/* ==================== 回调分发测试 ==================== */

static int g_test_cb_invoked = 0;
static uint8_t g_test_cb_cmd = 0;
static uint8_t g_test_cb_dev = 0;
static const uint8_t *g_test_cb_data = NULL;
static uint16_t g_test_cb_len = 0;
static void *g_test_cb_user = NULL;

static void test_callback(uint8_t cmd, uint8_t dev_id,
                          const uint8_t *data, uint16_t len,
                          void *user_data)
{
    g_test_cb_invoked++;
    g_test_cb_cmd = cmd;
    g_test_cb_dev = dev_id;
    g_test_cb_data = data;
    g_test_cb_len = len;
    g_test_cb_user = user_data;
}

static void test_callback_dispatch(void)
{
    proto_parse_init();

    g_test_cb_invoked = 0;
    static int marker = 0xDEAD;
    proto_parse_register(PROTO_CMD_TEMP_HUMIDITY_DATA, test_callback, &marker);

    /* 构建并发送 CMD 0x04 帧 */
    uint8_t payload[16] = {0};
    uint8_t frame[64];
    int frm_len = build_test_frame(frame, sizeof(frame),
                                   PROTO_CMD_TEMP_HUMIDITY_DATA, payload, 16, 5);
    proto_parse_feed_buf(frame, (uint16_t)frm_len);

    TEST_ASSERT_EQUAL(1, g_test_cb_invoked);
    TEST_ASSERT_EQUAL_UINT8(PROTO_CMD_TEMP_HUMIDITY_DATA, g_test_cb_cmd);
    TEST_ASSERT_EQUAL_UINT8(PROTO_DEVICE_ID_STM32, g_test_cb_dev);
    TEST_ASSERT_EQUAL_UINT16(16, g_test_cb_len);
    TEST_ASSERT_TRUE(g_test_cb_user == &marker);

    proto_parse_deinit();
}

static void test_callback_unknown_cmd(void)
{
    proto_parse_init();

    /* 不注册任何回调, 发送未知 CMD */
    uint8_t payload[] = { 0xAA, 0xBB };
    uint8_t frame[32];
    int frm_len = build_test_frame(frame, sizeof(frame), 0x99, payload, 2, 1);
    proto_parse_feed_buf(frame, (uint16_t)frm_len);

    struct proto_parse_stats stats;
    proto_parse_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(1, stats.cmds_unknown);

    proto_parse_deinit();
}

/* ==================== 统计测试 ==================== */

static void test_stats_initial_zeros(void)
{
    proto_parse_init();

    struct proto_parse_stats stats;
    memset(&stats, 0xFF, sizeof(stats));
    proto_parse_get_stats(&stats);

    TEST_ASSERT_EQUAL_UINT32(0, stats.rx_frames);
    TEST_ASSERT_EQUAL_UINT32(0, stats.rx_bytes);
    TEST_ASSERT_EQUAL_UINT32(0, stats.crc_errors);
    TEST_ASSERT_EQUAL_UINT32(0, stats.frame_errors);
    TEST_ASSERT_EQUAL_UINT32(0, stats.cmds_unknown);

    proto_parse_deinit();
}

static void test_stats_after_frames(void)
{
    proto_parse_init();

    uint8_t payload[] = { 0x01 };
    uint8_t frame[32];

    for (int i = 0; i < 5; i++) {
        int frm_len = build_test_frame(frame, sizeof(frame),
                                       PROTO_CMD_HEARTBEAT, payload, 1, (uint8_t)i);
        proto_parse_feed_buf(frame, (uint16_t)frm_len);
    }

    struct proto_parse_stats stats;
    proto_parse_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(5, stats.rx_frames);

    proto_parse_deinit();
}

static void test_stats_reset(void)
{
    proto_parse_init();

    uint8_t payload[] = { 0x01 };
    uint8_t frame[32];
    int frm_len = build_test_frame(frame, sizeof(frame),
                                   PROTO_CMD_HEARTBEAT, payload, 1, 1);
    proto_parse_feed_buf(frame, (uint16_t)frm_len);

    proto_parse_reset_stats();

    struct proto_parse_stats stats;
    proto_parse_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.rx_frames);
    TEST_ASSERT_EQUAL_UINT32(0, stats.rx_bytes);

    proto_parse_deinit();
}

/* ==================== 边界条件测试 ==================== */

static void test_dump_frame_null(void)
{
    /* dump_frame 传入 NULL 不应崩溃 */
    proto_dump_frame("TEST", NULL, 0);
}

static void test_parse_feed_uninitialized(void)
{
    /* 未初始化的 feed 不应该崩溃 */
    proto_parse_deinit();  /* 确保未初始化 */
    proto_parse_feed(0xAA);
    /* 不崩溃即为通过 */
}

static void test_parse_register_null_callback(void)
{
    proto_parse_init();
    int ret = proto_parse_register(PROTO_CMD_TEMP_HUMIDITY_DATA, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, ret);
    proto_parse_deinit();
}

static void test_parse_register_heartbeat_rejected(void)
{
    proto_parse_init();
    int ret = proto_parse_register(PROTO_CMD_HEARTBEAT, test_callback, NULL);
    TEST_ASSERT_EQUAL(-1, ret);
    proto_parse_deinit();
}

/* ==================== 主函数 ==================== */

int main(void)
{
    printf("\n=== Protocol Unit Tests ===\n\n");

    printf("[CRC16 Tests]\n");
    RUN_TEST(test_crc16_empty);
    RUN_TEST(test_crc16_known_vector_1);
    RUN_TEST(test_crc16_known_vector_2);
    RUN_TEST(test_crc16_all_0xAA);

    printf("\n[Frame Parse Tests]\n");
    RUN_TEST(test_parse_heartbeat_frame);
    RUN_TEST(test_parse_temp_humidity_frame);
    RUN_TEST(test_parse_motor_status_frame);
    RUN_TEST(test_parse_nde_feature_frame);
    RUN_TEST(test_parse_zero_payload_frame);

    printf("\n[Error Recovery Tests]\n");
    RUN_TEST(test_crc_error_rejected);
    RUN_TEST(test_truncated_frame_rejected);
    RUN_TEST(test_garbage_then_valid_self_sync);
    RUN_TEST(test_oversized_length_rejected);

    printf("\n[Callback Dispatch Tests]\n");
    RUN_TEST(test_callback_dispatch);
    RUN_TEST(test_callback_unknown_cmd);

    printf("\n[Stats Tests]\n");
    RUN_TEST(test_stats_initial_zeros);
    RUN_TEST(test_stats_after_frames);
    RUN_TEST(test_stats_reset);

    printf("\n[Boundary Tests]\n");
    RUN_TEST(test_dump_frame_null);
    RUN_TEST(test_parse_feed_uninitialized);
    RUN_TEST(test_parse_register_null_callback);
    RUN_TEST(test_parse_register_heartbeat_rejected);

    return test_summary();
}
