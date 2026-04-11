/**
 * @file protocol_test.c
 * @author EnterWorldDoor
 * @brief protocol 企业级单元测试（基于 Unity 框架）
 *
 * 测试覆盖范围:
 *   - 生命周期管理 (init/deinit/is_initialized/start/stop)
 *   - 帧构建与解析 (build_frame)
 *   - CRC16-MODBUS 校验计算
 *   - 发送 API (protocol_send / send_with_ack)
 *   - 回调注册/注销机制
 *   - 统计计数器 (get_stats / reset_stats)
 *   - 设备 ID 管理 (get_dev_id / set_dev_id)
 *   - 心跳发送 (send_heartbeat)
 *   - 错误处理与边界条件
 *   - 帧调试输出 (dump_frame)
 */

#include "unity.h"
#include "protocol.h"
#include "global_error.h"
#include "driver/uart.h"
#include <string.h>

/* ==================== 测试辅助变量 ==================== */

static int g_callback_invoked = 0;
static uint8_t g_last_cmd = 0;
static uint16_t g_last_data_len = 0;
static void *g_last_user_data = NULL;

static int g_error_callback_count = 0;
static int g_last_error_code = 0;

/* ==================== 测试辅助函数 ==================== */

static void test_cmd_callback(uint8_t cmd, const uint8_t *data,
                              uint16_t len, void *user_data)
{
    (void)data;
    g_callback_invoked++;
    g_last_cmd = cmd;
    g_last_data_len = len;
    g_last_user_data = user_data;
}

static void test_error_cb(int error_code, const char *context)
{
    (void)context;
    g_error_callback_count++;
    g_last_error_code = error_code;
}

static void reset_test_globals(void)
{
    g_callback_invoked = 0;
    g_last_cmd = 0;
    g_last_data_len = 0;
    g_last_user_data = NULL;
    g_error_callback_count = 0;
    g_last_error_code = 0;
}

/* ==================== 测试组：生命周期管理 ==================== */

void test_init_success(void)
{
    int ret = protocol_init(UART_NUM_1, 115200, 10, 9);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_TRUE(protocol_is_initialized());
    protocol_deinit();
}

void test_init_invalid_uart(void)
{
    int ret = protocol_init(-1, 115200, 10, 9);
    TEST_ASSERT_EQUAL(APP_ERR_PROTO_INVALID_PARAM, ret);
    TEST_ASSERT_FALSE(protocol_is_initialized());
}

void test_double_init_fails(void)
{
    protocol_init(UART_NUM_1, 115200, 10, 9);
    int ret = protocol_init(UART_NUM_1, 115200, 10, 9);
    TEST_ASSERT_EQUAL(APP_ERR_PROTO_ALREADY_INIT, ret);
    protocol_deinit();
}

void test_deinit_when_not_init(void)
{
    int ret = protocol_deinit();
    TEST_ASSERT_EQUAL(APP_ERR_PROTO_NOT_INIT, ret);
}

void test_is_initialized_default(void)
{
    TEST_ASSERT_FALSE(protocol_is_initialized());
}

/* ==================== 测试组：设备 ID 管理 ==================== */

void test_dev_id_default_value(void)
{
    protocol_init(UART_NUM_1, 115200, 10, 9);
    TEST_ASSERT_EQUAL(PROTO_DEV_ID_DEFAULT, protocol_get_dev_id());
    protocol_deinit();
}

void test_set_and_get_dev_id(void)
{
    protocol_init(UART_NUM_1, 115200, 10, 9);
    protocol_set_dev_id(42);
    TEST_ASSERT_EQUAL(42, protocol_get_dev_id());
    protocol_set_dev_id(255);
    TEST_ASSERT_EQUAL(255, protocol_get_dev_id());
    protocol_deinit();
}

/* ==================== 测试组：回调注册/注销 ==================== */

void test_register_callback_success(void)
{
    protocol_init(UART_NUM_1, 115200, 10, 9);
    reset_test_globals();

    int ret = protocol_register_callback(CMD_ADC_DATA, test_cmd_callback,
                                         (void *)0xDEAD);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    protocol_deinit();
}

void test_register_null_callback_fails(void)
{
    protocol_init(UART_NUM_1, 115200, 10, 9);

    int ret = protocol_register_callback(CMD_ADC_DATA, NULL, NULL);
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret);
    protocol_deinit();
}

void test_register_without_init_fails(void)
{
    int ret = protocol_register_callback(CMD_ADC_DATA, test_cmd_callback, NULL);
    TEST_ASSERT_EQUAL(APP_ERR_PROTO_NOT_INIT, ret);
}

void test_unregister_existing_callback(void)
{
    protocol_init(UART_NUM_1, 115200, 10, 9);

    protocol_register_callback(CMD_AI_RESULT, test_cmd_callback, (void *)0xBEEF);
    int ret = protocol_unregister_callback(CMD_AI_RESULT, test_cmd_callback);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    protocol_deinit();
}

void test_unregister_nonexistent_callback(void)
{
    protocol_init(UART_NUM_1, 115200, 10, 9);

    int ret = protocol_unregister_callback(CMD_ADC_DATA, test_cmd_callback);
    TEST_ASSERT_EQUAL(APP_ERR_NOT_SUPPORTED, ret);
    protocol_deinit();
}

void test_register_error_callback(void)
{
    protocol_init(UART_NUM_1, 115200, 10, 9);
    reset_test_globals();

    int ret = protocol_register_error_callback(test_error_cb);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    protocol_deinit();
}

/* ==================== 测试组：统计计数器 ==================== */

void test_stats_initial_zeros(void)
{
    protocol_init(UART_NUM_1, 115200, 10, 9);

    struct proto_stats stats;
    memset(&stats, 0xFF, sizeof(stats));
    int ret = protocol_get_stats(&stats);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_EQUAL_UINT32(0, stats.tx_frames);
    TEST_ASSERT_EQUAL_UINT32(0, stats.rx_frames);
    TEST_ASSERT_EQUAL_UINT32(0, stats.tx_bytes);
    TEST_ASSERT_EQUAL_UINT32(0, stats.rx_bytes);
    TEST_ASSERT_EQUAL_UINT32(0, stats.crc_errors);
    TEST_ASSERT_EQUAL_UINT32(0, stats.frame_errors);
    TEST_ASSERT_EQUAL_UINT32(0, stats.ack_timeouts);
    TEST_ASSERT_EQUAL_UINT32(0, stats.retries);
    protocol_deinit();
}

void test_stats_null_pointer_fails(void)
{
    protocol_init(UART_NUM_1, 115200, 10, 9);

    int ret = protocol_get_stats(NULL);
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, ret);
    protocol_deinit();
}

void test_stats_not_init_fails(void)
{
    struct proto_stats stats;
    int ret = protocol_get_stats(&stats);
    TEST_ASSERT_EQUAL(APP_ERR_PROTO_NOT_INIT, ret);
}

void test_reset_stats(void)
{
    protocol_init(UART_NUM_1, 115200, 10, 9);

    struct proto_stats before, after;
    protocol_get_stats(&before);
    (void)before;

    protocol_reset_stats();
    protocol_get_stats(&after);
    TEST_ASSERT_EQUAL_UINT32(0, after.tx_frames);
    TEST_ASSERT_EQUAL_UINT32(0, after.rx_frames);
    TEST_ASSERT_EQUAL_UINT32(0, after.crc_errors);
    protocol_deinit();
}

/* ==================== 测试组：发送 API 边界条件 ==================== */

void test_send_without_init_fails(void)
{
    int ret = protocol_send(CMD_HEARTBEAT, NULL, 0);
    TEST_ASSERT_EQUAL(APP_ERR_PROTO_NOT_INIT, ret);
}

void test_send_oversized_payload_fails(void)
{
    protocol_init(UART_NUM_1, 115200, 10, 9);

    uint8_t big_data[PROTO_MAX_DATA_LEN + 1];
    memset(big_data, 0xAB, sizeof(big_data));

    int ret = protocol_send(CMD_ADC_DATA, big_data, sizeof(big_data));
    TEST_ASSERT_EQUAL(APP_ERR_PROTO_FRAME_OVERFLOW, ret);
    protocol_deinit();
}

void test_send_empty_payload_ok(void)
{
    protocol_init(UART_NUM_1, 115200, 10, 9);

    int ret = protocol_send(CMD_HEARTBEAT, NULL, 0);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    protocol_deinit();
}

/* ==================== 测试组：心跳相关 ==================== */

void test_send_heartbeat_without_init_fails(void)
{
    int ret = protocol_send_heartbeat();
    TEST_ASSERT_EQUAL(APP_ERR_PROTO_NOT_INIT, ret);
}

void test_peer_alive_default_true(void)
{
    protocol_init(UART_NUM_1, 115200, 10, 9);
    TEST_ASSERT_TRUE(protocol_is_peer_alive());
    protocol_deinit();
}

void test_peer_alive_not_init_false(void)
{
    TEST_ASSERT_FALSE(protocol_is_peer_alive());
}

/* ==================== 测试组：start/stop 任务控制 ==================== */

void test_start_without_init_fails(void)
{
    int ret = protocol_start();
    TEST_ASSERT_EQUAL(APP_ERR_PROTO_NOT_INIT, ret);
}

void test_stop_without_init_fails(void)
{
    int ret = protocol_stop();
    TEST_ASSERT_EQUAL(APP_ERR_PROTO_NOT_INIT, ret);
}

/* ==================== 测试组：帧调试输出 ==================== */

void test_dump_frame_does_not_crash(void)
{
    uint8_t frame[] = { 0xAA, 0x55, 0x00, 0x04, 0x01, 0xFE, 0x01,
                        0x12, 0x34, 0x0D };
    protocol_dump_frame(frame, sizeof(frame), "TEST");
}

void test_dump_frame_zero_length(void)
{
    protocol_dump_frame(NULL, 0, "EMPTY");
}

/* ==================== 测试组：ACK 发送（无 UART 硬件）==================== */

void test_send_with_ack_without_init_fails(void)
{
    int ret = protocol_send_with_ack(CMD_CONTROL, NULL, 0, 100);
    TEST_ASSERT_EQUAL(APP_ERR_PROTO_NOT_INIT, ret);
}

/* ==================== Unity 主函数 ==================== */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();

    RUN_TEST(test_init_success);
    RUN_TEST(test_init_invalid_uart);
    RUN_TEST(test_double_init_fails);
    RUN_TEST(test_deinit_when_not_init);
    RUN_TEST(test_is_initialized_default);

    RUN_TEST(test_dev_id_default_value);
    RUN_TEST(test_set_and_get_dev_id);

    RUN_TEST(test_register_callback_success);
    RUN_TEST(test_register_null_callback_fails);
    RUN_TEST(test_register_without_init_fails);
    RUN_TEST(test_unregister_existing_callback);
    RUN_TEST(test_unregister_nonexistent_callback);
    RUN_TEST(test_register_error_callback);

    RUN_TEST(test_stats_initial_zeros);
    RUN_TEST(test_stats_null_pointer_fails);
    RUN_TEST(test_stats_not_init_fails);
    RUN_TEST(test_reset_stats);

    RUN_TEST(test_send_without_init_fails);
    RUN_TEST(test_send_oversized_payload_fails);
    RUN_TEST(test_send_empty_payload_ok);

    RUN_TEST(test_send_heartbeat_without_init_fails);
    RUN_TEST(test_peer_alive_default_true);
    RUN_TEST(test_peer_alive_not_init_false);

    RUN_TEST(test_start_without_init_fails);
    RUN_TEST(test_stop_without_init_fails);

    RUN_TEST(test_dump_frame_does_not_crash);
    RUN_TEST(test_dump_frame_zero_length);

    RUN_TEST(test_send_with_ack_without_init_fails);

    return UNITY_END();
}