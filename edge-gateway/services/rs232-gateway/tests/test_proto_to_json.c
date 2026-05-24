/**
 * @file test_proto_to_json.c
 * @brief RS232 Gateway — JSON 转换单元测试
 *
 * 测试各 CMD 类型 → JSON 转换、Topic 路由、dev_id 映射、边界条件
 */

#include "test_utils.h"
#include "proto_to_json.h"
#include "protocol.h"
#include <string.h>
#include <stdio.h>

static const char *TEST_PREFIX = "EdgeVib/factory1/motor";

/* ==================== CMD 0x04 温湿度 ==================== */

static void test_json_temp_humidity(void)
{
    uint8_t payload[16] = {0};
    float temp = 25.3f;
    float hum  = 58.7f;
    uint32_t ts = 12345;
    memcpy(&payload[0], &temp, 4);
    memcpy(&payload[4], &hum, 4);
    memcpy(&payload[8], &ts, 4);
    payload[12] = PROTO_SENSOR_TYPE_SHT30;
    payload[13] = PROTO_SENSOR_STATUS_NORMAL;

    char json[4096];
    char topic[256];
    size_t json_len = 0;

    int ret = proto_to_json(PROTO_CMD_TEMP_HUMIDITY_DATA, 0x01,
                            payload, 16,
                            json, sizeof(json), &json_len,
                            topic, sizeof(topic), TEST_PREFIX);

    TEST_ASSERT_EQUAL(0, ret);
    /* 验证关键字段 */
    TEST_ASSERT_TRUE(strstr(json, "\"source\":\"rs232\"") != NULL);
    TEST_ASSERT_TRUE(strstr(json, "\"cmd\":\"temp_humidity\"") != NULL);
    TEST_ASSERT_TRUE(strstr(json, "\"dev_id\":\"de01\"") != NULL);
    TEST_ASSERT_TRUE(strstr(json, "\"temp_c\":25.30") != NULL);
    TEST_ASSERT_TRUE(strstr(json, "\"humidity_rh\":58.70") != NULL);
    TEST_ASSERT_TRUE(strstr(json, "\"sensor_type\":2") != NULL);
    /* 验证 topic */
    TEST_ASSERT_EQUAL_STRING("EdgeVib/factory1/motor/de01/data/sensor", topic);
}

/* ==================== CMD 0x06 电机状态 ==================== */

static void test_json_motor_status(void)
{
    uint8_t payload[26] = {0};
    int32_t rpm = 1500;
    int32_t current_ma = 3500;
    int32_t bus_mv = 24000;
    int32_t temp_dc = 355;  /* 35.5°C */
    int32_t duty = 75;
    memcpy(&payload[MOTOR_OFF_RPM], &rpm, 4);
    memcpy(&payload[MOTOR_OFF_CURRENT_MA], &current_ma, 4);
    memcpy(&payload[MOTOR_OFF_BUS_MV], &bus_mv, 4);
    memcpy(&payload[MOTOR_OFF_TEMP_DC], &temp_dc, 4);
    payload[MOTOR_OFF_STATE] = 1;
    payload[MOTOR_OFF_FAULT] = 0;
    memcpy(&payload[MOTOR_OFF_DUTY], &duty, 4);
    payload[MOTOR_OFF_DIRECTION] = 1;
    payload[MOTOR_OFF_PID_ACTIVE] = 1;

    char json[4096];
    char topic[256];
    size_t json_len = 0;

    int ret = proto_to_json(PROTO_CMD_MOTOR_STATUS_RESP, 0x01,
                            payload, 26,
                            json, sizeof(json), &json_len,
                            topic, sizeof(topic), TEST_PREFIX);

    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_TRUE(strstr(json, "\"cmd\":\"motor_status\"") != NULL);
    TEST_ASSERT_TRUE(strstr(json, "\"rpm\":1500") != NULL);
    TEST_ASSERT_TRUE(strstr(json, "\"current_ma\":3500") != NULL);
    TEST_ASSERT_TRUE(strstr(json, "\"bus_mv\":24000") != NULL);
    TEST_ASSERT_TRUE(strstr(json, "\"pid_active\":true") != NULL);
    TEST_ASSERT_EQUAL_STRING("EdgeVib/factory1/motor/de01/data/motor", topic);
}

/* ==================== CMD 0x07 系统状态 ==================== */

static void test_json_system_status(void)
{
    uint8_t payload[8] = {0};
    payload[0] = 0;  /* NORMAL */
    payload[1] = 1;  /* AUTO */
    payload[2] = 0;  /* e_stop NORMAL */
    payload[3] = 0;  /* health NORMAL */
    payload[4] = 4;  /* event_source = AI change */

    char json[4096];
    char topic[256];
    size_t json_len = 0;

    int ret = proto_to_json(PROTO_CMD_SYSTEM_STATUS, 0x01,
                            payload, 8,
                            json, sizeof(json), &json_len,
                            topic, sizeof(topic), TEST_PREFIX);

    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_TRUE(strstr(json, "\"cmd\":\"system_status\"") != NULL);
    TEST_ASSERT_TRUE(strstr(json, "\"system_state\":0") != NULL);
    TEST_ASSERT_TRUE(strstr(json, "\"operation_mode\":1") != NULL);
    TEST_ASSERT_TRUE(strstr(json, "\"health_level\":0") != NULL);
    TEST_ASSERT_EQUAL_STRING("EdgeVib/factory1/motor/de01/status/health", topic);
}

/* ==================== CMD 0x10 紧急事件 ==================== */

static void test_json_emergency(void)
{
    uint8_t payload[8] = {0};
    uint32_t event_code = 1;
    uint32_t severity   = 3;
    memcpy(&payload[0], &event_code, 4);
    memcpy(&payload[4], &severity, 4);

    char json[4096];
    char topic[256];
    size_t json_len = 0;

    int ret = proto_to_json(PROTO_CMD_EMERGENCY_EVENT, 0x01,
                            payload, 8,
                            json, sizeof(json), &json_len,
                            topic, sizeof(topic), TEST_PREFIX);

    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_TRUE(strstr(json, "\"cmd\":\"emergency\"") != NULL);
    TEST_ASSERT_TRUE(strstr(json, "\"event_code\":1") != NULL);
    TEST_ASSERT_TRUE(strstr(json, "\"severity\":3") != NULL);
    TEST_ASSERT_EQUAL_STRING("EdgeVib/factory1/motor/de01/status/emergency", topic);
}

/* ==================== CMD 0x17 NDE 特征向量 ==================== */

static void test_json_nde_feature(void)
{
    uint8_t payload[100] = {0};
    payload[0] = 42;  /* window_idx */
    /* 填充 24 个 float */
    for (int i = 0; i < 24; i++) {
        float f = (float)(i * 0.1);
        memcpy(&payload[4 + i * 4], &f, 4);
    }

    char json[4096];
    char topic[256];
    size_t json_len = 0;

    int ret = proto_to_json(PROTO_CMD_NDE_FEATURE, 0x02,
                            payload, 100,
                            json, sizeof(json), &json_len,
                            topic, sizeof(topic), TEST_PREFIX);

    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_TRUE(strstr(json, "\"cmd\":\"nde_feature\"") != NULL);
    TEST_ASSERT_TRUE(strstr(json, "\"window_idx\":42") != NULL);
    TEST_ASSERT_TRUE(strstr(json, "\"features\":[") != NULL);
    /* 验证包含 24 个浮点数 (检查逗号数量) */
    int commas = 0;
    for (size_t i = 0; i < json_len && json[i] != ']'; i++) {
        if (json[i] == ',') commas++;
    }
    TEST_ASSERT_TRUE(commas >= 23);  /* 至少 23 个逗号 → 24 个值 */
    TEST_ASSERT_EQUAL_STRING("EdgeVib/factory1/motor/nde01/data/sensor", topic);
}

/* ==================== CMD 0x18 NDE 心跳 ==================== */

static void test_json_nde_heartbeat(void)
{
    uint8_t payload[4];
    payload[0] = 0x01;  /* online */
    payload[1] = 0;     /* error_count */
    payload[2] = 42;    /* temp_c = 42°C */

    char json[4096];
    char topic[256];
    size_t json_len = 0;

    int ret = proto_to_json(PROTO_CMD_NDE_HEARTBEAT, 0x02,
                            payload, 4,
                            json, sizeof(json), &json_len,
                            topic, sizeof(topic), TEST_PREFIX);

    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_TRUE(strstr(json, "\"cmd\":\"nde_heartbeat\"") != NULL);
    TEST_ASSERT_TRUE(strstr(json, "\"online\":true") != NULL);
    TEST_ASSERT_TRUE(strstr(json, "\"temp_c\":42") != NULL);
    TEST_ASSERT_EQUAL_STRING("EdgeVib/factory1/motor/nde01/status/health", topic);
}

/* ==================== CMD 0xFE 心跳 ==================== */

static void test_json_heartbeat(void)
{
    uint8_t payload[] = { 0x01 };

    char json[4096];
    char topic[256];
    size_t json_len = 0;

    int ret = proto_to_json(PROTO_CMD_HEARTBEAT, 0x01,
                            payload, 1,
                            json, sizeof(json), &json_len,
                            topic, sizeof(topic), TEST_PREFIX);

    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_TRUE(strstr(json, "\"cmd\":\"heartbeat\"") != NULL);
    TEST_ASSERT_TRUE(strstr(json, "\"alive\":true") != NULL);
    TEST_ASSERT_EQUAL_STRING("EdgeVib/factory1/motor/de01/status/health", topic);
}

/* ==================== 未知 CMD ==================== */

static void test_json_unknown_cmd(void)
{
    uint8_t payload[] = { 0x01, 0x02, 0x03 };

    char json[4096];
    char topic[256];
    size_t json_len = 0;

    int ret = proto_to_json(0xAB, 0x01,
                            payload, 3,
                            json, sizeof(json), &json_len,
                            topic, sizeof(topic), TEST_PREFIX);

    /* 未知 CMD 返回 -1 表示已生成 base64 通用格式 */
    TEST_ASSERT_EQUAL(-1, ret);
    TEST_ASSERT_TRUE(strstr(json, "\"cmd\":\"unknown_0xAB\"") != NULL);
    TEST_ASSERT_TRUE(strstr(json, "\"data_b64\":\"") != NULL);
    TEST_ASSERT_EQUAL_STRING("EdgeVib/factory1/motor/de01/data/generic", topic);
}

/* ==================== dev_id 映射 ==================== */

static void test_dev_id_mapping(void)
{
    uint8_t payload[1] = { 0x01 };
    char json[4096];
    char topic[256];
    size_t json_len = 0;

    /* 0x01 → de01 */
    proto_to_json(PROTO_CMD_HEARTBEAT, 0x01, payload, 1,
                  json, sizeof(json), &json_len, topic, sizeof(topic), TEST_PREFIX);
    TEST_ASSERT_TRUE(strstr(json, "\"dev_id\":\"de01\"") != NULL);

    /* 0x02 → nde01 */
    proto_to_json(PROTO_CMD_HEARTBEAT, 0x02, payload, 1,
                  json, sizeof(json), &json_len, topic, sizeof(topic), TEST_PREFIX);
    TEST_ASSERT_TRUE(strstr(json, "\"dev_id\":\"nde01\"") != NULL);

    /* 0xFF → dev_0xFF */
    proto_to_json(PROTO_CMD_HEARTBEAT, 0xFF, payload, 1,
                  json, sizeof(json), &json_len, topic, sizeof(topic), TEST_PREFIX);
    TEST_ASSERT_TRUE(strstr(json, "\"dev_id\":\"dev_0xFF\"") != NULL);
}

/* ==================== 字段存在性验证 ==================== */

static void test_json_source_field(void)
{
    uint8_t payload[16] = {0};
    char json[4096];
    char topic[256];
    size_t json_len = 0;

    /* 验证每个 CMD 类型都有 "source":"rs232" */
    proto_to_json(PROTO_CMD_TEMP_HUMIDITY_DATA, 0x01, payload, 16,
                  json, sizeof(json), &json_len, topic, sizeof(topic), TEST_PREFIX);
    TEST_ASSERT_TRUE(strstr(json, "\"source\":\"rs232\"") != NULL);

    proto_to_json(PROTO_CMD_MOTOR_STATUS_RESP, 0x01, payload, 26,
                  json, sizeof(json), &json_len, topic, sizeof(topic), TEST_PREFIX);
    TEST_ASSERT_TRUE(strstr(json, "\"source\":\"rs232\"") != NULL);
}

static void test_json_ts_field(void)
{
    uint8_t payload[1] = { 0x01 };
    char json[4096];
    char topic[256];
    size_t json_len = 0;

    proto_to_json(PROTO_CMD_HEARTBEAT, 0x01,
                  payload, 1,
                  json, sizeof(json), &json_len,
                  topic, sizeof(topic), TEST_PREFIX);

    /* ts 字段存在且值大于 0 (正常的 Unix 时间戳) */
    TEST_ASSERT_TRUE(strstr(json, "\"ts\":") != NULL);
}

/* ==================== 错误输入测试 ==================== */

static void test_payload_too_short(void)
{
    char json[4096];
    char topic[256];
    size_t json_len = 0;

    /* 温湿度需要 16 字节, 只给 10 字节 */
    uint8_t short_payload[10] = {0};
    int ret = proto_to_json(PROTO_CMD_TEMP_HUMIDITY_DATA, 0x01,
                            short_payload, 10,
                            json, sizeof(json), &json_len,
                            topic, sizeof(topic), TEST_PREFIX);
    TEST_ASSERT_EQUAL(-2, ret);
}

static void test_null_json_buffer(void)
{
    char topic[256];
    size_t json_len = 0;
    uint8_t payload[1] = { 0x01 };

    int ret = proto_to_json(PROTO_CMD_HEARTBEAT, 0x01,
                            payload, 1,
                            NULL, 100, &json_len,
                            topic, sizeof(topic), TEST_PREFIX);
    TEST_ASSERT_EQUAL(-2, ret);
}

/* ==================== 主函数 ==================== */

int main(void)
{
    printf("\n=== Proto-to-JSON Unit Tests ===\n\n");

    printf("[CMD Type Tests]\n");
    RUN_TEST(test_json_temp_humidity);
    RUN_TEST(test_json_motor_status);
    RUN_TEST(test_json_system_status);
    RUN_TEST(test_json_emergency);
    RUN_TEST(test_json_nde_feature);
    RUN_TEST(test_json_nde_heartbeat);
    RUN_TEST(test_json_heartbeat);

    printf("\n[Special Cases]\n");
    RUN_TEST(test_json_unknown_cmd);
    RUN_TEST(test_dev_id_mapping);

    printf("\n[Field Verification]\n");
    RUN_TEST(test_json_source_field);
    RUN_TEST(test_json_ts_field);

    printf("\n[Error Input Tests]\n");
    RUN_TEST(test_payload_too_short);
    RUN_TEST(test_null_json_buffer);

    return test_summary();
}
