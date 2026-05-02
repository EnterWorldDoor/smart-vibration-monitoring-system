/**
 * @file mqtt_test.c
 * @author EnterWorldDoor
 * @brief MQTT 模块单元测试 (TLS + 离线缓存 + 批量发布 + 动态配置)
 *
 * 测试覆盖:
 *   - 生命周期: init/deinit/start/stop
 *   - TLS 配置: CA证书加载、双向认证
 *   - 离线缓冲区: 缓存/刷新/状态查询
 *   - 批量发布: 配置/累积/刷新
 *   - 动态配置: 订阅/取消订阅/解析
 *   - 虚拟设备管理
 *   - 统计信息准确性
 */

#include "mqtt.h"
#include "global_error.h"
#include "config_manager.h"
#include "unity.h"
#include <string.h>

/* ==================== 测试辅助宏和函数 ==================== */

#define TEST_MQTT_BROKER_URL     "mqtt://192.168.1.100:1883"
#define TEST_TLS_BROKER_URL      "mqtts://test.mosquitto.org:8883"
#define TEST_CA_CERT             "-----BEGIN CERTIFICATE-----\nTEST\n-----END CERTIFICATE-----"
#define TEST_CLIENT_ID           "EdgeVib-TEST-001"
#define TEST_TOPIC               "edgevib/test/vibration"
#define TEST_CONFIG_TOPIC        "edgevib/config/1/update"

/**
 * create_default_config - 创建默认测试配置
 */
static struct mqtt_config create_default_config(void)
{
    struct mqtt_config cfg = {0};
    
    cfg.mode = MQTT_MODE_TRAINING;
    strncpy(cfg.broker.url, TEST_MQTT_BROKER_URL, sizeof(cfg.broker.url) - 1);
    cfg.broker.port = 1883;
    cfg.broker.qos = 1;
    cfg.broker.keepalive_sec = 60;
    cfg.broker.clean_session = true;
    strncpy(cfg.broker.client_id, TEST_CLIENT_ID, sizeof(cfg.broker.client_id) - 1);
    cfg.broker.enable_tls = false;
    
    cfg.num_virtual_devices = 1;
    cfg.devices[0].virtual_dev_id = 1;
    snprintf(cfg.devices[0].name, sizeof(cfg.devices[0].name), "TestDevice_01");
    cfg.devices[0].enabled = true;
    
    cfg.publish_interval_ms = 1000;
    cfg.publish_vibration_data = true;
    cfg.publish_environment_data = false;
    cfg.publish_health_status = false;
    
    return cfg;
}

/**
 * create_tls_config - 创建 TLS 测试配置
 */
static struct mqtt_config create_tls_config(void)
{
    struct mqtt_config cfg = create_default_config();
    
    cfg.broker.enable_tls = true;
    strncpy(cfg.broker.url, TEST_TLS_BROKER_URL, sizeof(cfg.broker.url) - 1);
    cfg.broker.port = 8883;
    strncpy(cfg.broker.ca_cert, TEST_CA_CERT, sizeof(cfg.broker.ca_cert) - 1);
    cfg.broker.use_global_ca_store = false;
    cfg.broker.skip_cert_common_name_check = false;
    
    return cfg;
}

/* ==================== 生命周期测试 ==================== */

void test_mqtt_init_success(void)
{
    struct mqtt_config cfg = create_default_config();
    
    int ret = mqtt_init(&cfg);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_TRUE(mqtt_is_initialized());
    
    mqtt_deinit();
}

void test_mqtt_init_null_config(void)
{
    /* NULL 配置应使用默认值初始化 */
    int ret = mqtt_init(NULL);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_TRUE(mqtt_is_initialized());
    
    mqtt_deinit();
}

void test_mqtt_double_init_fail(void)
{
    struct mqtt_config cfg = create_default_config();
    
    int ret1 = mqtt_init(&cfg);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret1);
    
    int ret2 = mqtt_init(&cfg);
    TEST_ASSERT_EQUAL(APP_ERR_MQTT_ALREADY_INIT, ret2);
    
    mqtt_deinit();
}

void test_mqtt_deinit_without_init(void)
{
    int ret = mqtt_deinit();
    TEST_ASSERT_EQUAL(APP_ERR_MQTT_NOT_INIT, ret);
}

void test_mqtt_is_initialized_false(void)
{
    TEST_ASSERT_FALSE(mqtt_is_initialized());
}

/* ==================== TLS 配置测试 ==================== */

void test_mqtt_tls_config_loaded(void)
{
    struct mqtt_config cfg = create_tls_config();
    
    int ret = mqtt_init(&cfg);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    
    /* 验证 TLS 配置已保存 */
    char url[256];
    ret = mqtt_get_broker_url(url, sizeof(url));
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_EQUAL_STRING(TEST_TLS_BROKER_URL, url);
    
    mqtt_deinit();
}

void test_mqtt_tls_with_mutual_auth(void)
{
    struct mqtt_config cfg = create_tls_config();
    
    /* 添加客户端证书和私钥 (双向认证) */
    strncpy(cfg.broker.client_cert,
            "-----BEGIN CERTIFICATE-----\nCLIENT\n-----END CERTIFICATE-----",
            sizeof(cfg.broker.client_cert) - 1);
    strncpy(cfg.broker.client_key,
            "-----BEGIN PRIVATE KEY-----\nKEY\n-----END PRIVATE KEY-----",
            sizeof(cfg.broker.client_key) - 1);
    
    int ret = mqtt_init(&cfg);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    
    mqtt_deinit();
}

void test_mqtt_global_ca_store(void)
{
    struct mqtt_config cfg = create_tls_config();
    cfg.broker.use_global_ca_store = true;
    memset(cfg.broker.ca_cert, 0, sizeof(cfg.broker.ca_cert));  /* 清空CA证书 */
    
    int ret = mqtt_init(&cfg);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    
    mqtt_deinit();
}

/* ==================== 离线缓冲区测试 ==================== */

void test_mqtt_offline_cache_message(void)
{
    struct mqtt_config cfg = create_default_config();
    mqtt_init(&cfg);
    
    const char *test_payload = "{\"test\": \"data\"}";
    
    /* 未连接时缓存消息 */
    int ret = mqtt_cache_message_offline(
        TEST_TOPIC,
        test_payload,
        strlen(test_payload),
        1,
        false);
    
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    
    /* 检查缓冲区状态 */
    uint32_t cached_count = 0;
    uint8_t buffer_usage = 0;
    
    ret = mqtt_get_offline_buffer_status(&cached_count, &buffer_usage);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_GREATER_THAN(0, (int)cached_count);
    TEST_ASSERT_GREATER_THAN(0, (int)buffer_usage);
    
    mqtt_deinit();
}

void test_mqtt_offline_cache_multiple_messages(void)
{
    struct mqtt_config cfg = create_default_config();
    mqtt_init(&cfg);
    
    const char *payloads[] = {
        "{\"msg\": 1}",
        "{\"msg\": 2}",
        "{\"msg\": 3}",
        "{\"msg\": 4}",
        "{\"msg\": 5}"
    };
    
    for (int i = 0; i < 5; i++) {
        int ret = mqtt_cache_message_offline(
            TEST_TOPIC,
            payloads[i],
            strlen(payloads[i]),
            1,
            false);
        
        TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    }
    
    uint32_t cached_count = 0;
    mqtt_get_offline_buffer_status(&cached_count, NULL);
    TEST_ASSERT_EQUAL(5, (int)cached_count);
    
    mqtt_deinit();
}

void test_mqtt_clear_offline_buffer(void)
{
    struct mqtt_config cfg = create_default_config();
    mqtt_init(&cfg);
    
    /* 先缓存一些消息 */
    mqtt_cache_message_offline(TEST_TOPIC, "test", 4, 1, false);
    mqtt_cache_message_offline(TEST_TOPIC, "test2", 5, 1, false);
    
    uint32_t count_before = 0;
    mqtt_get_offline_buffer_status(&count_before, NULL);
    TEST_ASSERT_GREATER_THAN(0, (int)count_before);
    
    /* 清空缓冲区 */
    int ret = mqtt_clear_offline_buffer();
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    
    uint32_t count_after = 0;
    mqtt_get_offline_buffer_status(&count_after, NULL);
    TEST_ASSERT_EQUAL(0, (int)count_after);
    
    mqtt_deinit();
}

void test_mqtt_offline_invalid_params(void)
{
    struct mqtt_config cfg = create_default_config();
    mqtt_init(&cfg);
    
    /* NULL 参数 */
    int ret1 = mqtt_cache_message_offline(NULL, "data", 4, 1, false);
    TEST_ASSERT_NOT_EQUAL(APP_ERR_OK, ret1);
    
    int ret2 = mqtt_cache_message_offline(TEST_TOPIC, NULL, 4, 1, false);
    TEST_ASSERT_NOT_EQUAL(APP_ERR_OK, ret2);
    
    int ret3 = mqtt_cache_message_offline(TEST_TOPIC, "data", 0, 1, false);
    TEST_ASSERT_NOT_EQUAL(APP_ERR_OK, ret3);
    
    mqtt_deinit();
}

/* ==================== 批量发布测试 ==================== */

void test_mqtt_batch_config_set_get(void)
{
    struct mqtt_config cfg = create_default_config();
    mqtt_init(&cfg);
    
    struct batch_config batch_cfg = {
        .enabled = true,
        .max_messages = 8,
        .flush_interval_ms = 3000,
        .max_batch_size_bytes = 8192,
        .merge_same_topic = true
    };
    
    int ret = mqtt_set_batch_config(&batch_cfg);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    
    struct batch_config retrieved = {0};
    ret = mqtt_get_batch_config(&retrieved);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    
    TEST_ASSERT_EQUAL(true, retrieved.enabled);
    TEST_ASSERT_EQUAL(8, (int)retrieved.max_messages);
    TEST_ASSERT_EQUAL(3000, (int)retrieved.flush_interval_ms);
    TEST_ASSERT_EQUAL(8192, (int)retrieved.max_batch_size_bytes);
    TEST_ASSERT_EQUAL(true, retrieved.merge_same_topic);
    
    mqtt_deinit();
}

void test_mqtt_batch_config_validation(void)
{
    struct mqtt_config cfg = create_default_config();
    mqtt_init(&cfg);
    
    /* 边界值测试: max_messages=0 应被修正为最大值 */
    struct batch_config invalid_cfg = {
        .enabled = true,
        .max_messages = 0,  /* 无效值 */
        .flush_interval_ms = 0,
        .max_batch_size_bytes = 0,
        .merge_same_topic = false
    };
    
    int ret = mqtt_set_batch_config(&invalid_cfg);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    
    struct batch_config validated = {0};
    mqtt_get_batch_config(&validated);
    
    TEST_ASSERT_GREATER_OR_EQUAL(MQTT_BATCH_MAX_MESSAGES, (int)validated.max_messages);
    TEST_ASSERT_GREATER_OR_EQUAL(1000, (int)validated.flush_interval_ms);
    TEST_ASSERT_GREATER_OR_EQUAL(4096, (int)validated.max_batch_size_bytes);
    
    mqtt_deinit();
}

void test_mqtt_batch_disable(void)
{
    struct mqtt_config cfg = create_default_config();
    mqtt_init(&cfg);
    
    struct batch_config disabled = {
        .enabled = false,
        .max_messages = 4,
        .flush_interval_ms = 1000,
        .max_batch_size_bytes = 2048,
        .merge_same_topic = true
    };
    
    int ret = mqtt_set_batch_config(&disabled);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    
    struct batch_config retrieved = {0};
    mqtt_get_batch_config(&retrieved);
    
    TEST_ASSERT_FALSE(retrieved.enabled);
    
    mqtt_deinit();
}

/* ==================== 动态配置更新测试 ==================== */

void test_mqtt_config_subscribe_unsubscribe(void)
{
    struct mqtt_config cfg = create_default_config();
    mqtt_init(&cfg);
    
    /* 注意: 实际订阅需要连接到 Broker, 此处仅验证 API 可用性 */
    /* 在未连接状态下调用应返回错误 */
    
    int sub_ret = mqtt_subscribe_config_topic(TEST_CONFIG_TOPIC, 1);
    TEST_ASSERT_EQUAL(APP_ERR_MQTT_CONNECT_FAIL, sub_ret);  /* 未连接 */
    
    int unsub_ret = mqtt_unsubscribe_config_topic();
    TEST_ASSERT_EQUAL(APP_ERR_MQTT_CONNECT_FAIL, unsub_ret);  /* 未连接 */
    
    mqtt_deinit();
}

void test_mqtt_config_callback_registration(void)
{
    struct mqtt_config cfg = create_default_config();
    mqtt_init(&cfg);
    
    bool callback_called = false;
    
    void test_config_callback(const char *topic, const void *data,
                              size_t data_len, void *user_data) {
        (void)topic; (void)data; (void)data_len; (void)user_data;
        callback_called = true;
    }
    
    int ret = mqtt_register_config_callback(test_config_callback, NULL);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    
    /* 验证统计信息 API */
    struct mqtt_stats stats = {0};
    ret = mqtt_get_config_update_stats(&stats);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_EQUAL(0, (int)stats.config_update_count);
    
    mqtt_deinit();
}

/* ==================== 虚拟设备管理测试 ==================== */

void test_mqtt_add_remove_virtual_device(void)
{
    struct mqtt_config cfg = create_default_config();
    mqtt_init(&cfg);
    
    /* 初始数量应为 1 (物理设备本身) */
    uint8_t initial_count = mqtt_get_virtual_device_count();
    TEST_ASSERT_EQUAL(1, (int)initial_count);
    
    /* 添加虚拟设备 */
    struct virtual_device_config dev1 = {
        .virtual_dev_id = 2,
        .name = "Motor_02",
        .vibration_offset_x = 0.01f,
        .vibration_offset_y = -0.02f,
        .vibration_offset_z = 0.005f,
        .temp_offset_c = 1.5f,
        .humidity_offset_rh = -2.0f,
        .enabled = true
    };
    
    int add_ret = mqtt_add_virtual_device(&dev1);
    TEST_ASSERT_EQUAL(APP_ERR_OK, add_ret);
    TEST_ASSERT_EQUAL(2, (int)mqtt_get_virtual_device_count());
    
    /* 再添加一个 */
    struct virtual_device_config dev2 = {
        .virtual_dev_id = 3,
        .name = "Motor_03",
        .vibration_offset_x = 0.02f,
        .vibration_offset_y = 0.03f,
        .vibration_offset_z = -0.01f,
        .temp_offset_c = -0.5f,
        .humidity_offset_rh = 1.0f,
        .enabled = true
    };
    
    add_ret = mqtt_add_virtual_device(&dev2);
    TEST_ASSERT_EQUAL(APP_ERR_OK, add_ret);
    TEST_ASSERT_EQUAL(3, (int)mqtt_get_virtual_device_count());
    
    /* 移除虚拟设备 */
    int remove_ret = mqtt_remove_virtual_device(2);
    TEST_ASSERT_EQUAL(APP_ERR_OK, remove_ret);
    TEST_ASSERT_EQUAL(2, (int)mqtt_get_virtual_device_count());
    
    mqtt_deinit();
}

void test_mqtt_virtual_device_full(void)
{
    struct mqtt_config cfg = create_default_config();
    mqtt_init(&cfg);
    
    /* 尝试添加超过最大数量的虚拟设备 */
    for (uint8_t i = 0; i < MQTT_MAX_VIRTUAL_DEVICES; i++) {
        struct virtual_device_config dev = {
            .virtual_dev_id = 10 + i,
            .name = "TestDev",
            .enabled = true
        };
        if (i < MQTT_MAX_VIRTUAL_DEVICES - 1) {  /* 已有1个初始设备 */
            mqtt_add_virtual_device(&dev);
        } else {
            /* 最后一次应失败 */
            int ret = mqtt_add_virtual_device(&dev);
            TEST_ASSERT_EQUAL(APP_ERR_MQTT_VIRTUAL_DEV_FULL, ret);
        }
    }
    
    mqtt_deinit();
}

/* ==================== 统计信息测试 ==================== */

void test_mqtt_stats_initial_state(void)
{
    struct mqtt_config cfg = create_default_config();
    mqtt_init(&cfg);
    
    struct mqtt_stats stats = {0};
    int ret = mqtt_get_stats(&stats);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    
    /* 验证初始值全为0 */
    TEST_ASSERT_EQUAL(0, (int)stats.connect_count);
    TEST_ASSERT_EQUAL(0, (int)stats.disconnect_count);
    TEST_ASSERT_EQUAL(0, (int)stats.messages_published);
    TEST_ASSERT_EQUAL(0, (int)stats.bytes_sent);
    TEST_ASSERT_EQUAL(0, (int)stats.connection_errors);
    TEST_ASSERT_EQUAL(0, (int)stats.offline_cached_count);
    TEST_ASSERT_EQUAL(0, (int)stats.offline_dropped_count);
    TEST_ASSERT_EQUAL(0, (int)stats.batch_publish_count);
    TEST_ASSERT_EQUAL(0, (int)stats.config_update_count);
    
    mqtt_deinit();
}

void test_mqtt_reset_stats(void)
{
    struct mqtt_config cfg = create_default_config();
    mqtt_init(&cfg);
    
    /* 产生一些统计数据 */
    mqtt_cache_message_offline(TEST_TOPIC, "test", 4, 1, false);
    mqtt_cache_message_offline(TEST_TOPIC, "test2", 5, 1, false);
    
    /* 重置统计 */
    mqtt_reset_stats();
    
    /* 验证重置后归零 */
    struct mqtt_stats stats = {0};
    mqtt_get_stats(&stats);
    TEST_ASSERT_EQUAL(0, (int)stats.offline_cached_count);
    
    mqtt_deinit();
}

/* ==================== 连接状态测试 ==================== */

void test_mqtt_connection_states(void)
{
    struct mqtt_config cfg = create_default_config();
    mqtt_init(&cfg);
    
    /* 初始状态应为 DISCONNECTED */
    enum mqtt_connection_state state = mqtt_get_state();
    TEST_ASSERT_EQUAL(MQTT_STATE_DISCONNECTED, state);
    TEST_ASSERT_FALSE(mqtt_is_connected());
    
    /* 未初始化时的状态查询 */
    mqtt_deinit();
    state = mqtt_get_state();
    TEST_ASSERT_EQUAL(MQTT_STATE_DISCONNECTED, state);  /* 返回默认值 */
}

void test_mqtt_mode_switching(void)
{
    struct mqtt_config cfg = create_default_config();
    mqtt_init(&cfg);
    
    /* 初始模式 */
    enum mqtt_mode mode = mqtt_get_current_mode();
    TEST_ASSERT_EQUAL(MQTT_MODE_TRAINING, mode);
    
    /* 切换到 Upload 模式 */
    int ret = mqtt_switch_mode(MQTT_MODE_UPLOAD);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_EQUAL(MQTT_MODE_UPLOAD, mqtt_get_current_mode());
    
    /* 切换回 Training */
    ret = mqtt_switch_mode(MQTT_MODE_TRAINING);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_EQUAL(MQTT_MODE_TRAINING, mqtt_get_current_mode());
    
    /* 无效模式 */
    ret = mqtt_switch_mode((enum mqtt_mode)99);
    TEST_ASSERT_EQUAL(APP_ERR_MQTT_INVALID_MODE, ret);
    
    mqtt_deinit();
}

/* ==================== 错误处理测试 ==================== */

void test_mqtt_api_without_init(void)
{
    /* 所有 API 在未初始化时应返回错误 */
    TEST_ASSERT_EQUAL(APP_ERR_MQTT_NOT_INIT, mqtt_start());
    TEST_ASSERT_EQUAL(APP_ERR_MQTT_NOT_INIT, mqtt_stop());
    TEST_ASSERT_EQUAL(APP_ERR_MQTT_NOT_INIT, mqtt_reconnect());
    TEST_ASSERT_EQUAL(APP_ERR_MQTT_NOT_INIT, mqtt_get_state());
    TEST_ASSERT_FALSE(mqtt_is_connected());
    
    struct mqtt_stats stats;
    TEST_ASSERT_EQUAL(APP_ERR_MQTT_NOT_INIT, mqtt_get_stats(&stats));
    
    struct batch_config batch;
    TEST_ASSERT_EQUAL(APP_ERR_MQTT_NOT_INIT, mqtt_set_batch_config(&batch));
    TEST_ASSERT_EQUAL(APP_ERR_MQTT_NOT_INIT, mqtt_get_batch_config(&batch));
    
    TEST_ASSERT_EQUAL(APP_ERR_MQTT_NOT_INIT, mqtt_flush_batch_buffer());
    TEST_ASSERT_EQUAL(APP_ERR_MQTT_NOT_INIT, mqtt_flush_offline_buffer());
    TEST_ASSERT_EQUAL(APP_ERR_MQTT_NOT_INIT, mqtt_clear_offline_buffer());
}

/* ==================== Unity 测试运行器 ==================== */

void setUp(void)
{
    /* 每个测试前确保模块处于干净状态 */
    if (mqtt_is_initialized()) {
        mqtt_deinit();
    }
}

void tearDown(void)
{
    /* 每个测试后清理 */
    if (mqtt_is_initialized()) {
        mqtt_deinit();
    }
}

int main(void)
{
    UNITY_BEGIN();
    
    /* 生命周期测试 */
    RUN_TEST(test_mqtt_init_success);
    RUN_TEST(test_mqtt_init_null_config);
    RUN_TEST(test_mqtt_double_init_fail);
    RUN_TEST(test_mqtt_deinit_without_init);
    RUN_TEST(test_mqtt_is_initialized_false);
    
    /* TLS 配置测试 */
    RUN_TEST(test_mqtt_tls_config_loaded);
    RUN_TEST(test_mqtt_tls_with_mutual_auth);
    RUN_TEST(test_mqtt_global_ca_store);
    
    /* 离线缓冲区测试 */
    RUN_TEST(test_mqtt_offline_cache_message);
    RUN_TEST(test_mqtt_offline_cache_multiple_messages);
    RUN_TEST(test_mqtt_clear_offline_buffer);
    RUN_TEST(test_mqtt_offline_invalid_params);
    
    /* 批量发布测试 */
    RUN_TEST(test_mqtt_batch_config_set_get);
    RUN_TEST(test_mqtt_batch_config_validation);
    RUN_TEST(test_mqtt_batch_disable);
    
    /* 动态配置更新测试 */
    RUN_TEST(test_mqtt_config_subscribe_unsubscribe);
    RUN_TEST(test_mqtt_config_callback_registration);
    
    /* 虚拟设备管理测试 */
    RUN_TEST(test_mqtt_add_remove_virtual_device);
    RUN_TEST(test_mqtt_virtual_device_full);
    
    /* 统计信息测试 */
    RUN_TEST(test_mqtt_stats_initial_state);
    RUN_TEST(test_mqtt_reset_stats);
    
    /* 连接状态测试 */
    RUN_TEST(test_mqtt_connection_states);
    RUN_TEST(test_mqtt_mode_switching);
    
    /* 错误处理测试 */
    RUN_TEST(test_mqtt_api_without_init);
    
    return UNITY_END();
}
