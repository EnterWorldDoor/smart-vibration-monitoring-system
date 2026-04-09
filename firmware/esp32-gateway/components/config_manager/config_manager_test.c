/**
 * @file config_manager_test.c
 * @author EnterWorldDoor
 * @brief config_manager 模块 Unity 企业级单元测试
 *
 * 测试覆盖范围：
 *   - 生命周期管理（init / deinit / is_initialized）
 *   - 配置校验（validate 各字段边界值与非法值）
 *   - 核心读写（get / set / save / reset）
 *   - 工厂重置（factory_reset）
 *   - 单字段 Getter/Setter（sample_rate / fft_size / device_id / rms_thresholds）
 *   - 导出导入（export / import 往返一致性、CRC 校验失败）
 *   - CRC32 计算（compute_crc32 一致性）
 *   - 版本查询（get_version）
 *   - 备份恢复（backup / restore 槽位边界、无效槽位拒绝）
 *   - 变更回调（register / unregister / 回调触发验证）
 *   - 边界条件（NULL 参数、重复初始化、未初始化调用）
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "unity.h"
#include "config_manager.h"
#include "global_error.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static int g_callback_fire_count;
static char g_last_callback_key[64];
static struct system_config g_last_old_cfg;
static struct system_config g_last_new_cfg;

static void test_callback(const char *key, const void *old_val, const void *new_val)
{
    g_callback_fire_count++;
    strncpy(g_last_callback_key, key ? key : "", sizeof(g_last_callback_key) - 1);
    if (old_val) memcpy(&g_last_old_cfg, old_val, sizeof(g_last_old_cfg));
    if (new_val) memcpy(&g_last_new_cfg, new_val, sizeof(g_last_new_cfg));
}

static void reset_callback_state(void)
{
    g_callback_fire_count = 0;
    memset(g_last_callback_key, 0, sizeof(g_last_callback_key));
    memset(&g_last_old_cfg, 0, sizeof(g_last_old_cfg));
    memset(&g_last_new_cfg, 0, sizeof(g_last_new_cfg));
}

void setUp(void)
{
    reset_callback_state();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
}

void tearDown(void)
{
    if (config_manager_is_initialized()) {
        config_manager_deinit();
    }
}

/* ==================== 辅助：构建合法配置 ==================== */

static struct system_config make_valid_config(void)
{
    struct system_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.config_version         = CONFIG_VERSION;
    cfg.sample_rate_hz         = 200;
    cfg.sensor_buffer_size     = 256;
    cfg.fft_size               = 256;
    cfg.rms_warning_threshold  = 1.5f;
    cfg.rms_critical_threshold = 3.0f;
    cfg.freq_peak_threshold    = 0.3f;
    cfg.ai_anomaly_threshold   = 0.6f;
    cfg.heartbeat_interval_ms  = 2000;
    cfg.device_id              = 42;
    strncpy(cfg.device_name, "TestDevice", sizeof(cfg.device_name) - 1);
    strncpy(cfg.gateway_url, "mqtt://test.local", sizeof(cfg.gateway_url) - 1);
    cfg.encryption_enabled     = true;
    cfg.auto_reboot_enabled    = false;
    cfg.reboot_interval_seconds = 43200;
    return cfg;
}

/* ==================== 1. 生命周期测试 ==================== */

TEST_CASE("init succeeds and sets initialized flag", "[cfg][lifecycle]")
{
    TEST_ASSERT_FALSE(config_manager_is_initialized());

    int ret = config_manager_init(NULL);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_TRUE(config_manager_is_initialized());
}

TEST_CASE("init with output pointer fills config structure", "[cfg][lifecycle]")
{
    struct system_config out;
    memset(&out, 0xFF, sizeof(out));

    int ret = config_manager_init(&out);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_EQUAL_UINT32(CONFIG_VERSION, out.config_version);
    TEST_ASSERT_EQUAL(100, out.sample_rate_hz);
    TEST_ASSERT_EQUAL(512, out.fft_size);
}

TEST_CASE("double init is idempotent and returns success", "[cfg][lifecycle]")
{
    TEST_ASSERT_EQUAL(APP_ERR_OK, config_manager_init(NULL));
    TEST_ASSERT_TRUE(config_manager_is_initialized());

    struct system_config out;
    int ret = config_manager_init(&out);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_EQUAL(100, out.sample_rate_hz);
}

TEST_CASE("deinit clears initialized flag", "[cfg][lifecycle]")
{
    config_manager_init(NULL);
    TEST_ASSERT_TRUE(config_manager_is_initialized());

    int ret = config_manager_deinit();
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_FALSE(config_manager_is_initialized());
}

TEST_CASE("deinit when not initialized returns error", "[cfg][lifecycle]")
{
    int ret = config_manager_deinit();
    TEST_ASSERT_EQUAL(APP_ERR_GENERAL, ret);
}

TEST_CASE("get before init returns NULL", "[cfg][lifecycle]")
{
    const struct system_config *p = config_manager_get();
    TEST_ASSERT_NULL(p);
}

/* ==================== 2. 校验测试 ==================== */

TEST_CASE("validate accepts fully valid configuration", "[cfg][validate]")
{
    struct system_config cfg = make_valid_config();
    TEST_ASSERT_EQUAL(APP_ERR_OK, config_manager_validate(&cfg));
}

TEST_CASE("validate rejects NULL pointer", "[cfg][validate]")
{
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, config_manager_validate(NULL));
}

TEST_CASE("validate rejects sample_rate_hz zero", "[cfg][validate]")
{
    struct system_config cfg = make_valid_config();
    cfg.sample_rate_hz = 0;
    TEST_ASSERT_EQUAL(APP_ERR_CONFIG_VALIDATION, config_manager_validate(&cfg));
}

TEST_CASE("validate rejects sample_rate_hz exceeding max", "[cfg][validate]")
{
    struct system_config cfg = make_valid_config();
    cfg.sample_rate_hz = 16001;
    TEST_ASSERT_EQUAL(APP_ERR_CONFIG_VALIDATION, config_manager_validate(&cfg));
}

TEST_CASE("validate accepts boundary sample_rate_hz min=1 max=16000", "[cfg][validate]")
{
    struct system_config cfg = make_valid_config();

    cfg.sample_rate_hz = 1;
    TEST_ASSERT_EQUAL(APP_ERR_OK, config_manager_validate(&cfg));

    cfg.sample_rate_hz = 16000;
    TEST_ASSERT_EQUAL(APP_ERR_OK, config_manager_validate(&cfg));
}

TEST_CASE("validate rejects sensor_buffer_size below minimum", "[cfg][validate]")
{
    struct system_config cfg = make_valid_config();
    cfg.sensor_buffer_size = 63;
    TEST_ASSERT_EQUAL(APP_ERR_CONFIG_VALIDATION, config_manager_validate(&cfg));
}

TEST_CASE("validate rejects sensor_buffer_size above maximum", "[cfg][validate]")
{
    struct system_config cfg = make_valid_config();
    cfg.sensor_buffer_size = 4097;
    TEST_ASSERT_EQUAL(APP_ERR_CONFIG_VALIDATION, config_manager_validate(&cfg));
}

TEST_CASE("validate rejects fft_size not power of two", "[cfg][validate]")
{
    struct system_config cfg = make_valid_config();
    cfg.fft_size = 100;
    TEST_ASSERT_EQUAL(APP_ERR_CONFIG_VALIDATION, config_manager_validate(&cfg));
}

TEST_CASE("validate rejects fft_size below minimum", "[cfg][validate]")
{
    struct system_config cfg = make_valid_config();
    cfg.fft_size = 32;
    TEST_ASSERT_EQUAL(APP_ERR_CONFIG_VALIDATION, config_manager_validate(&cfg));
}

TEST_CASE("validate accepts valid power-of-two fft_sizes", "[cfg][validate]")
{
    struct system_config cfg = make_valid_config();
    int sizes[] = {64, 128, 256, 512, 1024, 2048, 4096};
    for (int i = 0; i < (int)(sizeof(sizes)/sizeof(sizes[0])); i++) {
        cfg.fft_size = sizes[i];
        TEST_ASSERT_EQUAL_MESSAGE(APP_ERR_OK, config_manager_validate(&cfg),
                                  "Valid power-of-two FFT size rejected");
    }
}

TEST_CASE("validate rejects rms_warning negative", "[cfg][validate]")
{
    struct system_config cfg = make_valid_config();
    cfg.rms_warning_threshold = -0.1f;
    TEST_ASSERT_EQUAL(APP_ERR_CONFIG_VALIDATION, config_manager_validate(&cfg));
}

TEST_CASE("validate rejects rms_critical less than warning", "[cfg][validate]")
{
    struct system_config cfg = make_valid_config();
    cfg.rms_warning_threshold = 5.0f;
    cfg.rms_critical_threshold = 2.0f;
    TEST_ASSERT_EQUAL(APP_ERR_CONFIG_VALIDATION, config_manager_validate(&cfg));
}

TEST_CASE("validate rejects freq_peak_threshold outside [0,1]", "[cfg][validate]")
{
    struct system_config cfg = make_valid_config();

    cfg.freq_peak_threshold = -0.01f;
    TEST_ASSERT_EQUAL(APP_ERR_CONFIG_VALIDATION, config_manager_validate(&cfg));

    cfg.freq_peak_threshold = 1.01f;
    TEST_ASSERT_EQUAL(APP_ERR_CONFIG_VALIDATION, config_manager_validate(&cfg));
}

TEST_CASE("validate accepts freq_peak_threshold at boundaries 0 and 1", "[cfg][validate]")
{
    struct system_config cfg = make_valid_config();

    cfg.freq_peak_threshold = 0.0f;
    TEST_ASSERT_EQUAL(APP_ERR_OK, config_manager_validate(&cfg));

    cfg.freq_peak_threshold = 1.0f;
    TEST_ASSERT_EQUAL(APP_ERR_OK, config_manager_validate(&cfg));
}

TEST_CASE("validate rejects ai_anomaly_threshold outside [0,1]", "[cfg][validate]")
{
    struct system_config cfg = make_valid_config();
    cfg.ai_anomaly_threshold = 1.5f;
    TEST_ASSERT_EQUAL(APP_ERR_CONFIG_VALIDATION, config_manager_validate(&cfg));
}

TEST_CASE("validate rejects heartbeat_interval_ms below 100", "[cfg][validate]")
{
    struct system_config cfg = make_valid_config();
    cfg.heartbeat_interval_ms = 99;
    TEST_ASSERT_EQUAL(APP_ERR_CONFIG_VALIDATION, config_manager_validate(&cfg));
}

TEST_CASE("validate rejects heartbeat_interval_ms above 60000", "[cfg][validate]")
{
    struct system_config cfg = make_valid_config();
    cfg.heartbeat_interval_ms = 60001;
    TEST_ASSERT_EQUAL(APP_ERR_CONFIG_VALIDATION, config_manager_validate(&cfg));
}

/* ==================== 3. Get / Set / Save 测试 ==================== */

TEST_CASE("get returns non-null pointer after init", "[cfg][getset]")
{
    config_manager_init(NULL);
    const struct system_config *p = config_manager_get();
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL(100, p->sample_rate_hz);
    TEST_ASSERT_EQUAL(512, p->fft_size);
}

TEST_CASE("set with valid config updates all fields", "[cfg][getset]")
{
    config_manager_init(NULL);

    struct system_config new_cfg = make_valid_config();
    new_cfg.sample_rate_hz = 800;

    int ret = config_manager_set(&new_cfg);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);

    const struct system_config *current = config_manager_get();
    TEST_ASSERT_NOT_NULL(current);
    TEST_ASSERT_EQUAL(800, current->sample_rate_hz);
    TEST_ASSERT_EQUAL(256, current->fft_size);
    TEST_ASSERT_EQUAL(42, current->device_id);
}

TEST_CASE("set with NULL pointer returns error", "[cfg][getset]")
{
    config_manager_init(NULL);
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, config_manager_set(NULL));
}

TEST_CASE("set with invalid config rejects without modifying state", "[cfg][getset]")
{
    config_manager_init(NULL);

    struct system_config bad = make_valid_config();
    bad.sample_rate_hz = -1;

    int ret = config_manager_set(&bad);
    TEST_ASSERT_EQUAL(APP_ERR_CONFIG_VALIDATION, ret);

    const struct system_config *current = config_manager_get();
    TEST_ASSERT_EQUAL(100, current->sample_rate_hz);
}

TEST_CASE("save persists current in-memory config to NVS", "[cfg][save]")
{
    config_manager_init(NULL);

    struct system_config modified = make_valid_config();
    modified.sample_rate_hz = 1234;
    config_manager_set(&modified);

    int ret = config_manager_save();
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);

    config_manager_deinit();
    struct system_config reloaded;
    ret = config_manager_init(&reloaded);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_EQUAL(1234, reloaded.sample_rate_hz);
}

TEST_CASE("save before init returns error", "[cfg][save]")
{
    TEST_ASSERT_EQUAL(APP_ERR_GENERAL, config_manager_save());
}

TEST_CASE("set before init returns error", "[cfg][getset]")
{
    struct system_config cfg = make_valid_config();
    TEST_ASSERT_EQUAL(APP_ERR_GENERAL, config_manager_set(&cfg));
}

/* ==================== 4. Reset 测试 ==================== */

TEST_CASE("reset restores factory defaults", "[cfg][reset]")
{
    config_manager_init(NULL);

    struct system_config custom = make_valid_config();
    custom.sample_rate_hz = 9999;
    config_manager_set(&custom);

    int ret = config_manager_reset();
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);

    const struct system_config *current = config_manager_get();
    TEST_ASSERT_EQUAL(100, current->sample_rate_hz);
    TEST_ASSERT_EQUAL(512, current->sensor_buffer_size);
    TEST_ASSERT_EQUAL(512, current->fft_size);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, current->rms_warning_threshold);
    TEST_ASSERT_EQUAL_FLOAT(4.0f, current->rms_critical_threshold);
}

TEST_CASE("reset before init returns error", "[cfg][reset]")
{
    TEST_ASSERT_EQUAL(APP_ERR_GENERAL, config_manager_reset());
}

/* ==================== 5. 工厂重置测试 ==================== */

TEST_CASE("factory_reset erases NVS and restores defaults", "[cfg][factory]")
{
    config_manager_init(NULL);

    struct system_config custom = make_valid_config();
    custom.sample_rate_hz = 7777;
    config_manager_set(&custom);

    int ret = config_manager_factory_reset();
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);

    const struct system_config *cur = config_manager_get();
    TEST_ASSERT_EQUAL(100, cur->sample_rate_hz);
}

TEST_CASE("factory_reset before init returns error", "[cfg][factory]")
{
    TEST_ASSERT_EQUAL(APP_ERR_GENERAL, config_manager_factory_reset());
}

TEST_CASE("factory_reset then reload still shows defaults", "[cfg][factory]")
{
    config_manager_init(NULL);

    struct system_config custom = make_valid_config();
    custom.device_id = 99;
    config_manager_set(&custom);

    TEST_ASSERT_EQUAL(APP_ERR_OK, config_manager_factory_reset());

    config_manager_deinit();
    struct system_config reloaded;
    config_manager_init(&reloaded);
    TEST_ASSERT_EQUAL(1, reloaded.device_id);
    TEST_ASSERT_EQUAL(100, reloaded.sample_rate_hz);
}

/* ==================== 6. 单字段 Getter/Setter 测试 ==================== */

TEST_CASE("get_sample_rate returns default before init", "[cfg][field]")
{
    TEST_ASSERT_EQUAL(100, config_manager_get_sample_rate());
}

TEST_CASE("get_sample_rate returns set value after init", "[cfg][field]")
{
    config_manager_init(NULL);
    TEST_ASSERT_EQUAL(100, config_manager_get_sample_rate());
}

TEST_CASE("set_sample_rate updates and persists value", "[cfg][field]")
{
    config_manager_init(NULL);

    int ret = config_manager_set_sample_rate(500);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_EQUAL(500, config_manager_get_sample_rate());

    const struct system_config *c = config_manager_get();
    TEST_ASSERT_EQUAL(500, c->sample_rate_hz);
}

TEST_CASE("set_sample_rate rejects out of range values", "[cfg][field]")
{
    config_manager_init(NULL);

    TEST_ASSERT_EQUAL(APP_ERR_CONFIG_VALIDATION, config_manager_set_sample_rate(0));
    TEST_ASSERT_EQUAL(APP_ERR_CONFIG_VALIDATION, config_manager_set_sample_rate(-10));
    TEST_ASSERT_EQUAL(APP_ERR_CONFIG_VALIDATION, config_manager_set_sample_rate(16001));

    TEST_ASSERT_EQUAL(100, config_manager_get_sample_rate());
}

TEST_CASE("set_sample_rate accepts boundary values", "[cfg][field]")
{
    config_manager_init(NULL);

    TEST_ASSERT_EQUAL(APP_ERR_OK, config_manager_set_sample_rate(1));
    TEST_ASSERT_EQUAL(1, config_manager_get_sample_rate());

    TEST_ASSERT_EQUAL(APP_ERR_OK, config_manager_set_sample_rate(16000));
    TEST_ASSERT_EQUAL(16000, config_manager_get_sample_rate());
}

TEST_CASE("get_fft_size returns default before init", "[cfg][field]")
{
    TEST_ASSERT_EQUAL(512, config_manager_get_fft_size());
}

TEST_CASE("set_fft_size updates and persists value", "[cfg][field]")
{
    config_manager_init(NULL);

    int ret = config_manager_set_fft_size(1024);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_EQUAL(1024, config_manager_get_fft_size());
}

TEST_CASE("set_fft_size rejects non-power-of-two", "[cfg][field]")
{
    config_manager_init(NULL);

    TEST_ASSERT_EQUAL(APP_ERR_CONFIG_VALIDATION, config_manager_set_fft_size(100));
    TEST_ASSERT_EQUAL(APP_ERR_CONFIG_VALIDATION, config_manager_set_fft_size(300));
    TEST_ASSERT_EQUAL(APP_ERR_CONFIG_VALIDATION, config_manager_set_fft_size(63));

    TEST_ASSERT_EQUAL(512, config_manager_get_fft_size());
}

TEST_CASE("set_fft_size accepts valid power-of-two values", "[cfg][field]")
{
    config_manager_init(NULL);
    int sizes[] = {64, 128, 256, 1024, 2048, 4096};
    for (int i = 0; i < (int)(sizeof(sizes)/sizeof(sizes[0])); i++) {
        TEST_ASSERT_EQUAL(APP_ERR_OK, config_manager_set_fft_size(sizes[i]));
        TEST_ASSERT_EQUAL(sizes[i], config_manager_get_fft_size());
    }
}

TEST_CASE("get_device_id returns default before init", "[cfg][field]")
{
    TEST_ASSERT_EQUAL(1, config_manager_get_device_id());
}

TEST_CASE("get_device_id returns configured value", "[cfg][field]")
{
    config_manager_init(NULL);

    struct system_config cfg = make_valid_config();
    cfg.device_id = 200;
    config_manager_set(&cfg);

    TEST_ASSERT_EQUAL(200, config_manager_get_device_id());
}

TEST_CASE("get_rms_thresholds returns defaults before init", "[cfg][field]")
{
    float w = 0, c = 0;
    int ret = config_manager_get_rms_thresholds(&w, &c);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, w);
    TEST_ASSERT_EQUAL_FLOAT(4.0f, c);
}

TEST_CASE("get_rms_thresholds returns configured values", "[cfg][field]")
{
    config_manager_init(NULL);

    struct system_config cfg = make_valid_config();
    cfg.rms_warning_threshold = 1.2f;
    cfg.rms_critical_threshold = 3.6f;
    config_manager_set(&cfg);

    float w = 0, c = 0;
    int ret = config_manager_get_rms_thresholds(&w, &c);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_EQUAL_FLOAT(1.2f, w);
    TEST_ASSERT_EQUAL_FLOAT(3.6f, c);
}

TEST_CASE("get_rms_thresholds rejects NULL pointers", "[cfg][field]")
{
    float v = 0;
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM,
                      config_manager_get_rms_thresholds(NULL, &v));
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM,
                      config_manager_get_rms_thresholds(&v, NULL));
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM,
                      config_manager_get_rms_thresholds(NULL, NULL));
}

/* ==================== 7. Export / Import 测试 ==================== */

TEST_CASE("export produces buffer of expected size", "[cfg][export_import]")
{
    config_manager_init(NULL);

    uint8_t buf[CONFIG_EXPORT_BUF_SIZE + 16];
    size_t out_len = 0;

    int ret = config_manager_export(buf, sizeof(buf), &out_len);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)CONFIG_EXPORT_BUF_SIZE, (uint32_t)out_len);
}

TEST_CASE("export rejects NULL parameters", "[cfg][export_import]")
{
    config_manager_init(NULL);

    uint8_t buf[CONFIG_EXPORT_BUF_SIZE];
    size_t len = 0;

    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM,
                      config_manager_export(NULL, sizeof(buf), &len));
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM,
                      config_manager_export(buf, sizeof(buf), NULL));
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM,
                      config_manager_export(buf, CONFIG_EXPORT_BUF_SIZE - 1, &len));
}

TEST_CASE("export before init returns error", "[cfg][export_import]")
{
    uint8_t buf[CONFIG_EXPORT_BUF_SIZE];
    size_t len = 0;
    TEST_ASSERT_EQUAL(APP_ERR_GENERAL,
                      config_manager_export(buf, sizeof(buf), &len));
}

TEST_CASE("import round-trip preserves all fields", "[cfg][export_import]")
{
    config_manager_init(NULL);

    struct system_config original = make_valid_config();
    original.sample_rate_hz = 3333;
    original.device_id = 88;
    config_manager_set(&original);

    uint8_t exported[CONFIG_EXPORT_BUF_SIZE];
    size_t exp_len = 0;
    TEST_ASSERT_EQUAL(APP_ERR_OK,
                      config_manager_export(exported, sizeof(exported), &exp_len));

    struct system_config changed = make_valid_config();
    changed.sample_rate_hz = 1111;
    config_manager_set(&changed);

    int ret = config_manager_import(exported, exp_len);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);

    const struct system_config *restored = config_manager_get();
    TEST_ASSERT_EQUAL(3333, restored->sample_rate_hz);
    TEST_ASSERT_EQUAL(88, restored->device_id);
    TEST_ASSERT_EQUAL(256, restored->fft_size);
}

TEST_CASE("import rejects wrong length buffer", "[cfg][export_import]")
{
    config_manager_init(NULL);

    uint8_t buf[CONFIG_EXPORT_BUF_SIZE];
    size_t len = 0;
    config_manager_export(buf, sizeof(buf), &len);

    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM,
                      config_manager_import(buf, len - 1));
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM,
                      config_manager_import(buf, len + 1));
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM,
                      config_manager_import(NULL, len));
}

TEST_CASE("import rejects tampered data via CRC mismatch", "[cfg][export_import]")
{
    config_manager_init(NULL);

    uint8_t exported[CONFIG_EXPORT_BUF_SIZE];
    size_t len = 0;
    config_manager_export(exported, sizeof(exported), &len);

    exported[0] ^= 0xFF;
    TEST_ASSERT_EQUAL(APP_ERR_CONFIG_VALIDATION,
                      config_manager_import(exported, len));
}

TEST_CASE("import before init returns error", "[cfg][export_import]")
{
    uint8_t buf[CONFIG_EXPORT_BUF_SIZE] = {0};
    TEST_ASSERT_EQUAL(APP_ERR_GENERAL,
                      config_manager_import(buf, CONFIG_EXPORT_BUF_SIZE));
}

/* ==================== 8. CRC32 测试 ==================== */

TEST_CASE("compute_crc32 returns consistent result for same config", "[cfg][crc]")
{
    struct system_config cfg = make_valid_config();
    uint32_t crc1 = config_manager_compute_crc32(&cfg);
    uint32_t crc2 = config_manager_compute_crc32(&cfg);
    TEST_ASSERT_EQUAL_UINT32(crc1, crc2);
}

TEST_CASE("compute_crc32 detects single byte change", "[cfg][crc]")
{
    struct system_config cfg_a = make_valid_config();
    struct system_config cfg_b = make_valid_config();
    cfg_b.sample_rate_hz++;

    uint32_t crc_a = config_manager_compute_crc32(&cfg_a);
    uint32_t crc_b = config_manager_compute_crc32(&cfg_b);
    TEST_ASSERT_NOT_EQUAL_UINT32(crc_a, crc_b);
}

TEST_CASE("compute_crc32 with NULL returns zero", "[cfg][crc]")
{
    TEST_ASSERT_EQUAL_UINT32(0, config_manager_compute_crc32(NULL));
}

/* ==================== 9. 版本查询测试 ==================== */

TEST_CASE("get_version returns CONFIG_VERSION after init", "[cfg][version]")
{
    config_manager_init(NULL);
    TEST_ASSERT_EQUAL_UINT32(CONFIG_VERSION, config_manager_get_version());
}

TEST_CASE("get_version returns 0 before init", "[cfg][version]")
{
    TEST_ASSERT_EQUAL_UINT32(0, config_manager_get_version());
}

/* ==================== 10. 备份恢复测试 ==================== */

TEST_CASE("backup to slot 0 succeeds", "[cfg][backup_restore]")
{
    config_manager_init(NULL);

    struct system_config custom = make_valid_config();
    custom.sample_rate_hz = 4444;
    config_manager_set(&custom);

    int ret = config_manager_backup(0);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);
}

TEST_CASE("backup rejects slot >= CONFIG_MAX_BACKUP_SLOTS", "[cfg][backup_restore]")
{
    config_manager_init(NULL);
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, config_manager_backup(CONFIG_MAX_BACKUP_SLOTS));
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, config_manager_backup(255));
}

TEST_CASE("restore from backed up slot recovers config", "[cfg][backup_restore]")
{
    config_manager_init(NULL);

    struct system_config custom = make_valid_config();
    custom.sample_rate_hz = 5555;
    custom.device_id = 77;
    config_manager_set(&custom);

    TEST_ASSERT_EQUAL(APP_ERR_OK, config_manager_backup(1));

    struct system_config other = make_valid_config();
    other.sample_rate_hz = 1111;
    config_manager_set(&other);

    int ret = config_manager_restore(1);
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);

    const struct system_config *cur = config_manager_get();
    TEST_ASSERT_EQUAL(5555, cur->sample_rate_hz);
    TEST_ASSERT_EQUAL(77, cur->device_id);
}

TEST_CASE("restore from empty slot fails gracefully", "[cfg][backup_restore]")
{
    config_manager_init(NULL);

    int ret = config_manager_restore(9);
    TEST_ASSERT_EQUAL(APP_ERR_CONFIG_LOAD_FAIL, ret);
}

TEST_CASE("restore rejects invalid slot number", "[cfg][backup_restore]")
{
    config_manager_init(NULL);
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM, config_manager_restore(CONFIG_MAX_BACKUP_SLOTS));
}

TEST_CASE("backup and restore across multiple slots independently", "[cfg][backup_restore]")
{
    config_manager_init(NULL);

    struct system_config cfg_a = make_valid_config();
    cfg_a.sample_rate_hz = 111;
    config_manager_set(&cfg_a);
    config_manager_backup(0);

    struct system_config cfg_b = make_valid_config();
    cfg_b.sample_rate_hz = 222;
    config_manager_set(&cfg_b);
    config_manager_backup(1);

    config_manager_restore(0);
    TEST_ASSERT_EQUAL(111, config_manager_get()->sample_rate_hz);

    config_manager_restore(1);
    TEST_ASSERT_EQUAL(222, config_manager_get()->sample_rate_hz);
}

TEST_CASE("backup before init returns error", "[cfg][backup_restore]")
{
    TEST_ASSERT_EQUAL(APP_ERR_GENERAL, config_manager_backup(0));
}

TEST_CASE("restore before init returns error", "[cfg][backup_restore]")
{
    TEST_ASSERT_EQUAL(APP_ERR_GENERAL, config_manager_restore(0));
}

/* ==================== 11. 变更回调测试 ==================== */

TEST_CASE("register_callback accepts valid callback", "[cfg][callback]")
{
    config_manager_init(NULL);
    TEST_ASSERT_EQUAL(APP_ERR_OK,
                      config_manager_register_callback(test_callback));
}

TEST_CASE("register_callback rejects NULL", "[cfg][callback]")
{
    config_manager_init(NULL);
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM,
                      config_manager_register_callback(NULL));
}

TEST_CASE("callback fires on config_set", "[cfg][callback]")
{
    config_manager_init(NULL);
    config_manager_register_callback(test_callback);

    struct system_config new_cfg = make_valid_config();
    new_cfg.sample_rate_hz = 6666;
    config_manager_set(&new_cfg);

    TEST_ASSERT_EQUAL_INT(1, g_callback_fire_count);
    TEST_ASSERT_EQUAL_STRING("config", g_last_callback_key);
    TEST_ASSERT_EQUAL(6666, g_last_new_cfg.sample_rate_hz);
}

TEST_CASE("callback fires on reset", "[cfg][callback]")
{
    config_manager_init(NULL);
    config_manager_register_callback(test_callback);

    struct system_config custom = make_valid_config();
    custom.sample_rate_hz = 8888;
    config_manager_set(&custom);

    reset_callback_state();
    config_manager_reset();

    TEST_ASSERT_EQUAL_INT(1, g_callback_fire_count);
    TEST_ASSERT_EQUAL_STRING("reset", g_last_callback_key);
    TEST_ASSERT_EQUAL(100, g_last_new_cfg.sample_rate_hz);
}

TEST_CASE("callback fires on restore", "[cfg][callback]")
{
    config_manager_init(NULL);
    config_manager_register_callback(test_callback);

    struct system_config custom = make_valid_config();
    custom.device_id = 55;
    config_manager_set(&custom);
    config_manager_backup(2);

    struct system_config other = make_valid_config();
    other.device_id = 11;
    config_manager_set(&other);

    reset_callback_state();
    config_manager_restore(2);

    TEST_ASSERT_EQUAL_INT(1, g_callback_fire_count);
    TEST_ASSERT_EQUAL_STRING("restore", g_last_callback_key);
    TEST_ASSERT_EQUAL(55, g_last_new_cfg.device_id);
}

TEST_CASE("duplicate registration is ignored", "[cfg][callback]")
{
    config_manager_init(NULL);

    TEST_ASSERT_EQUAL(APP_ERR_OK,
                      config_manager_register_callback(test_callback));
    TEST_ASSERT_EQUAL(APP_ERR_OK,
                      config_manager_register_callback(test_callback));

    struct system_config cfg = make_valid_config();
    config_manager_set(&cfg);

    TEST_ASSERT_EQUAL_INT(1, g_callback_fire_count);
}

TEST_CASE("unregistered callback no longer fires", "[cfg][callback]")
{
    config_manager_init(NULL);
    config_manager_register_callback(test_callback);
    config_manager_unregister_callback(test_callback);

    struct system_config cfg = make_valid_config();
    config_manager_set(&cfg);

    TEST_ASSERT_EQUAL_INT(0, g_callback_fire_count);
}

TEST_CASE("unregister non-existent callback returns error", "[cfg][callback]")
{
    config_manager_init(NULL);
    TEST_ASSERT_EQUAL(APP_ERR_NOT_SUPPORTED,
                      config_manager_unregister_callback(test_callback));
}

TEST_CASE("unregister NULL returns error", "[cfg][callback]")
{
    config_manager_init(NULL);
    TEST_ASSERT_EQUAL(APP_ERR_INVALID_PARAM,
                      config_manager_unregister_callback(NULL));
}

/* ==================== 12. 持久化跨重启验证 ==================== */

TEST_CASE("config survives deinit-reinit cycle via NVS", "[cfg][persistence]")
{
    config_manager_init(NULL);

    struct system_config persistent = make_valid_config();
    persistent.sample_rate_hz = 9999;
    persistent.fft_size = 2048;
    persistent.device_id = 123;
    config_manager_set(&persistent);

    config_manager_deinit();

    struct system_config recovered;
    config_manager_init(&recovered);

    TEST_ASSERT_EQUAL(9999, recovered.sample_rate_hz);
    TEST_ASSERT_EQUAL(2048, recovered.fft_size);
    TEST_ASSERT_EQUAL(123, recovered.device_id);
    TEST_ASSERT_EQUAL_UINT32(CONFIG_VERSION, recovered.config_version);
}