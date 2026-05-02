/**
 * @file sensor_service.c
 * @author EnterWorldDoor
 * @brief 企业级传感器服务实现 (数据融合管道核心)
 *
 * 数据流管道:
 *   ADXL345(原始数据) → 批量获取 → 温度补偿 → DSP(FFT/RMS) → 输出
 *
 * 任务模型:
 *   - 主服务任务: 负责定时分析和数据分发
 *   - 回调机制: 异步通知 AI/MQTT 模块
 */

#include "sensor_service.h"
#include "adxl345.h"
#include "adxl345_reg.h"      /* ADXL345 寄存器定义 (INT_WATERMARK等宏) */
#include "dsp.h"
#include "protocol.h"
#include "temperature_compensation.h"
#include "ringbuf.h"
#include "global_error.h"
#include "log_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <math.h>

/* 引入硬件引脚定义 (用于ADXL345 SPI配置) */
#include "hardware_config.h"

/* ==================== 模块内部状态 ==================== */

static struct {
    bool initialized;
    enum sensor_state state;

    /* 子模块句柄 */
    struct adxl345_dev *adxl;
    struct ringbuf ringbuf;
    uint8_t *ringbuf_storage;

    /* 配置 */
    struct sensor_config config;

    /* 任务 */
    TaskHandle_t task_handle;
    bool running;

    /* 回调注册 */
    struct {
        analysis_ready_callback_t cb;
        void *user_data;
    } analysis_callbacks[SENSOR_MAX_CALLBACKS];
    int analysis_callback_count;

    sensor_error_callback_t error_cb;
    void *error_user_data;

    /* 最新结果缓存 */
    struct analysis_result latest_result;
    bool has_latest_result;

    /* 当前温度缓存 */
    float current_temp_c;
    float current_humidity_rh;
    bool has_temperature;

    /* Protocol 温度回调缓存 (独立于队列路径) */
    struct temp_humidity_data protocol_temp_data;
    bool has_protocol_temperature;

    /* 统计 */
    struct sensor_stats stats;

    /* 线程安全 */
    SemaphoreHandle_t mutex;
} g_ss = {0};

/* ==================== 内部辅助函数 ==================== */

/**
 * apply_default_config - 应用默认配置
 */
static void apply_default_config(void)
{
    g_ss.config.sample_rate_hz = SENSOR_DEFAULT_SAMPLE_RATE_HZ;
    g_ss.config.ring_buffer_size = SENSOR_DEFAULT_BUFFER_SIZE;
    g_ss.config.fft_size = SENSOR_FFT_WINDOW_SIZE;
    g_ss.config.window_type = DSP_WINDOW_HANN;
    g_ss.config.analysis_interval_ms = SENSOR_ANALYSIS_INTERVAL_MS;
    g_ss.config.enable_temp_compensation = true;
    g_ss.config.enable_protocol_temp = true;
    g_ss.config.enable_detailed_logging = false;
}

/**
 * notify_error - 通知错误回调
 */
static void notify_error(int error_code, const char *context)
{
    if (g_ss.error_cb) {
        g_ss.error_cb(error_code, context, g_ss.error_user_data);
    }
}

/**
 * dispatch_analysis_callbacks - 分发分析结果到所有回调
 */
static void dispatch_analysis_callbacks(const struct analysis_result *result)
{
    for (int i = 0; i < g_ss.analysis_callback_count; i++) {
        if (g_ss.analysis_callbacks[i].cb) {
            g_ss.analysis_callbacks[i].cb(result,
                                            g_ss.analysis_callbacks[i].user_data);
        }
    }
}

/**
 * on_protocol_temperature - Protocol 温度数据到达回调
 */
static void on_protocol_temperature(const struct temp_humidity_data *temp_data,
                                    void *user_data)
{
    (void)user_data;
    static uint32_t temp_receive_count = 0;  // 接收计数器

    xSemaphoreTake(g_ss.mutex, portMAX_DELAY);
    g_ss.current_temp_c = temp_data->temperature_c;
    g_ss.current_humidity_rh = temp_data->humidity_rh;
    g_ss.protocol_temp_data.temperature_c = temp_data->temperature_c;
    g_ss.protocol_temp_data.humidity_rh = temp_data->humidity_rh;
    g_ss.has_temperature = true;
    g_ss.has_protocol_temperature = true;
    g_ss.stats.temp_data_received++;
    xSemaphoreGive(g_ss.mutex);

    if (g_ss.config.enable_temp_compensation) {
        temp_comp_push_temperature(temp_data->temperature_c,
                                   temp_data->timestamp_esp32_us);
    }

    temp_receive_count++;

    /*
     * ✅ 【关键】始终打印温度接收日志！
     *
     * 这是用户验证STM32→ESP32通信是否成功的最重要指标。
     * 使用LOG_INFO级别确保在正常模式下也能看到。
     *
     * 日志频率控制:
     *   - 前10条：每次都打印（确认通信建立）
     *   - 之后：每10条打印一次（避免刷屏）
     */
    if (temp_receive_count <= 10 || temp_receive_count % 10 == 0) {
        LOG_INFO("SENSOR", "✅ Temp callback: T=%.2f°C H=%.1f%%RH (#%lu)",
                 temp_data->temperature_c,
                 temp_data->humidity_rh,
                 (unsigned long)temp_receive_count);
    }

    if (g_ss.config.enable_detailed_logging) {
        LOG_DEBUG("SENSOR", "Temperature update: %.2f°C H=%.1f%%RH (detailed)",
                  temp_data->temperature_c, temp_data->humidity_rh);
    }
}

/**
 * perform_single_analysis - 执行一次完整的振动分析
 */
static int perform_single_analysis(struct analysis_result *result)
{
    int64_t start_time = esp_timer_get_time();
    struct adxl345_batch_data batch;
    int ret;

    memset(result, 0, sizeof(*result));

    if (!g_ss.adxl) {
        static uint32_t last_noadxl_warn = 0;
        uint32_t now_adxl = (uint32_t)(esp_timer_get_time() / 1000000LL);
        if ((now_adxl - last_noadxl_warn) >= 30) {
            LOG_WARN("SENSOR", "ADXL345 not available (DEGRADED mode), skipping analysis");
            last_noadxl_warn = now_adxl;
        } else {
            LOG_DEBUG("SENSOR", "ADXL345 not available (DEGRADED mode)");
        }
        result->service_state = g_ss.state;
        result->analysis_timestamp_us = (uint32_t)esp_timer_get_time();
        return APP_ERR_SENSOR_NOT_INIT;
    }

    /*
     * ⚠️ 【关键优化】超时时间适配纯轮询采集速率!
     *
     * acquire_task纯轮询模式: 5ms周期, 每轮2个样本(400Hz)
     * 有效速率: 400 samples/sec
     *
     * FFT窗口512样本 → 需要约1.3秒缓冲
     * 超时设置: 2000ms (确保有足够数据)
     */
    static bool first_analysis = true;
    uint32_t fetch_timeout = first_analysis ? 2000 : 1500;

    ret = adxl345_fetch_batch(g_ss.adxl, &batch, g_ss.config.fft_size, fetch_timeout);
    if (first_analysis) first_analysis = false;

    if (ret <= 0) {
        static uint32_t last_no_data_warn = 0;
        uint32_t now_warn = (uint32_t)(esp_timer_get_time() / 1000LL);
        if ((now_warn - last_no_data_warn) >= 30000) {
            LOG_WARN("SENSOR", "No data from ADXL345 for analysis (ret=%d, timeout=%lu)",
                     ret, (unsigned long)fetch_timeout);
            last_no_data_warn = now_warn;
        } else {
            LOG_DEBUG("SENSOR", "No data from ADXL345 for analysis (ret=%d, timeout=%lu)",
                     ret, (unsigned long)fetch_timeout);
        }
        result->service_state = g_ss.state;
        xSemaphoreTake(g_ss.mutex, portMAX_DELAY);
        if (g_ss.has_temperature) {
            result->temperature_c = g_ss.current_temp_c;
            result->humidity_rh = g_ss.current_humidity_rh;
            result->temperature_valid = true;
        } else if (g_ss.has_protocol_temperature) {
            result->temperature_c = g_ss.protocol_temp_data.temperature_c;
            result->humidity_rh = g_ss.protocol_temp_data.humidity_rh;
            result->temperature_valid = true;
        }
        xSemaphoreGive(g_ss.mutex);
        result->analysis_timestamp_us = (uint32_t)esp_timer_get_time();
        result->samples_analyzed = 0;
        return ret;
    }

    /*
     * ⚠️ 【关键修复】截断到最大2的幂次方!
     *
     * DSP FFT 要求输入长度必须是2的幂次方
     * 如果 fetch_batch 返回的不是2的幂次方, 截断到最近的2的幂次方
     * 例如: batch.count=200 → 使用128个样本
     *       batch.count=500 → 使用256个样本
     */
    uint16_t actual_count = batch.count;
    {
        uint16_t pow2 = 1;
        while (pow2 * 2 <= actual_count) pow2 *= 2;
        if (pow2 != actual_count) {
            LOG_DEBUG("SENSOR", "Truncating batch %u → %u (power-of-2 for FFT)",
                     actual_count, pow2);
            actual_count = pow2;
        }
    }
    if (actual_count < 2) {
        static uint32_t last_insuf_warn = 0;
        uint32_t now_insuf = (uint32_t)(esp_timer_get_time() / 1000000LL);
        if ((now_insuf - last_insuf_warn) >= 30) {
            LOG_WARN("SENSOR", "Insufficient samples for FFT (count=%u)", batch.count);
            last_insuf_warn = now_insuf;
        } else {
            LOG_DEBUG("SENSOR", "Insufficient samples for FFT (count=%u)", batch.count);
        }
        result->analysis_timestamp_us = (uint32_t)esp_timer_get_time();
        result->samples_analyzed = 0;
        xSemaphoreTake(g_ss.mutex, portMAX_DELAY);
        if (g_ss.has_temperature) {
            result->temperature_c = g_ss.current_temp_c;
            result->humidity_rh = g_ss.current_humidity_rh;
            result->temperature_valid = true;
        }
        xSemaphoreGive(g_ss.mutex);
        return APP_ERR_TIMEOUT;
    }

    static float x_buf[SENSOR_FFT_WINDOW_SIZE];
    static float y_buf[SENSOR_FFT_WINDOW_SIZE];
    static float z_buf[SENSOR_FFT_WINDOW_SIZE];

    for (uint16_t i = 0; i < actual_count; i++) {
        x_buf[i] = (float)batch.samples[i].x / 1000.0f;
        y_buf[i] = (float)batch.samples[i].y / 1000.0f;
        z_buf[i] = (float)batch.samples[i].z / 1000.0f;

        if (g_ss.config.enable_temp_compensation && g_ss.has_temperature) {
            int64_t ts_us = (int64_t)batch.samples[i].timestamp_us;
            temp_comp_compensate(&x_buf[i], &y_buf[i], &z_buf[i], ts_us);
            g_ss.stats.temp_comp_applied++;
        }
    }

    float sampling_rate = (float)g_ss.config.sample_rate_hz;
    ret = dsp_fft_compute_3axis(x_buf, y_buf, z_buf, actual_count,
                                sampling_rate, g_ss.config.window_type,
                                &result->vibration);
    if (ret != APP_ERR_OK) {
        static uint32_t last_dsp_err = 0;
        uint32_t now_dsp = (uint32_t)(esp_timer_get_time() / 1000000LL);
        if ((now_dsp - last_dsp_err) >= 30) {
            LOG_ERROR("SENSOR", "DSP FFT analysis failed: %d", ret);
            last_dsp_err = now_dsp;
        }
        result->analysis_timestamp_us = (uint32_t)esp_timer_get_time();
        result->samples_analyzed = actual_count;
        return ret;
    }

    result->overall_rms_g = result->vibration.vector_rms.value;
    result->peak_frequency_hz = result->vibration.x_fft.peak_freq;
    result->peak_amplitude_g = result->vibration.x_fft.peak_amp;

    xSemaphoreTake(g_ss.mutex, portMAX_DELAY);
    if (g_ss.has_temperature) {
        result->temperature_c = g_ss.current_temp_c;
        result->humidity_rh = g_ss.current_humidity_rh;
        result->temperature_valid = true;
    } else if (g_ss.has_protocol_temperature) {
        result->temperature_c = g_ss.protocol_temp_data.temperature_c;
        result->humidity_rh = g_ss.protocol_temp_data.humidity_rh;
        result->temperature_valid = true;
    }
    result->temp_compensation_active = g_ss.config.enable_temp_compensation;
    xSemaphoreGive(g_ss.mutex);

    result->service_state = g_ss.state;
    result->analysis_timestamp_us = (uint32_t)esp_timer_get_time();
    result->samples_analyzed = actual_count;
    result->total_analyses = g_ss.stats.total_analyses_performed + 1;

    /*
     * ⚠️ 【修复】安全获取ADXL345健康状态
     * 仅在 ADXL345 可用时调用, 避免空指针访问
     */
    if (g_ss.adxl) {
        adxl345_get_health(g_ss.adxl, &result->adxl_health);
    } else {
        memset(&result->adxl_health, 0, sizeof(result->adxl_health));
        result->adxl_health.level = ADXL345_HEALTH_FAULT;  /* 标记设备不可用 */
        result->adxl_health.selftest_passed = false;
    }

    protocol_get_stats(&result->protocol_stats);
    dsp_get_stats(&result->dsp_stats);

    uint64_t compute_time = (uint64_t)(esp_timer_get_time() - start_time);
    g_ss.stats.total_compute_time_us += compute_time;
    if (compute_time > g_ss.stats.max_compute_time_us) {
        g_ss.stats.max_compute_time_us = (uint32_t)compute_time;
    }

    return APP_ERR_OK;
}

 int sensor_service_fetch(struct vib_sample *out)
{
    if (!out) return APP_ERR_INVALID_PARAM;
    size_t len = ringbuf_pop(&g_ss.ringbuf, (uint8_t *)out, sizeof(*out));
    return (len == sizeof(*out) ? APP_ERR_OK : APP_ERR_TIMEOUT);
}

 int sensor_service_fetch_block(struct vib_sample *out, int max_count)
{
    if (!out || max_count <= 0) return APP_ERR_INVALID_PARAM;
    int total = 0;
    while (total < max_count) {
        size_t len = ringbuf_pop(&g_ss.ringbuf, (uint8_t *)&out[total], sizeof(struct vib_sample));
        if (len != sizeof(struct vib_sample)) break;
        total++;
    }
    return total;
}

/* ==================== 主服务任务 ==================== */

/**
 * sensor_service_task - 主服务任务 (定时分析循环)
 */
static void sensor_service_task(void *arg)
{
    (void)arg;
    TickType_t last_wake_time = xTaskGetTickCount();

    LOG_INFO("SENSOR", "Service task started (interval=%u ms)",
             g_ss.config.analysis_interval_ms);

    while (g_ss.running) {
        vTaskDelayUntil(&last_wake_time,
                        pdMS_TO_TICKS(g_ss.config.analysis_interval_ms));

        /*
         * ⚠️ 【关键优化】降级模式: 即使ADXL345失败也上传温湿度!
         *
         * 原始问题:
         *   ADXL345初始化失败后, g_ss.adxl == NULL
         *   原代码直接 continue, 完全跳过分析
         *   → 没有数据 → MQTT无发布 → PC端收不到任何东西
         *
         * 优化方案:
         *   在降级模式下:
         *     [1] 从Protocol模块获取最新温湿度数据(从STM32接收的)
         *     [2] 构造包含温湿度的analysis_result (振动数据标记为无效)
         *     [3] 触发回调 → MQTT上传温湿度数据
         *     [4] 确保PC端Edge-AI能收到至少部分数据
         */
        if (!g_ss.adxl) {
            static struct analysis_result degraded_result;

            memset(&degraded_result, 0, sizeof(degraded_result));

            /*
             * 尝试从Protocol获取温湿度数据
             */
            extern int proto_get_last_temp_humidity(float *temp_c, float *humidity_rh);
            float temp_c = 0.0f, humidity_rh = 0.0f;

            if (proto_get_last_temp_humidity(&temp_c, &humidity_rh) == APP_ERR_OK) {
                degraded_result.temperature_c = temp_c;
                degraded_result.humidity_rh = humidity_rh;
                degraded_result.temperature_valid = true;
            }

            /*
             * ⚠️ 【关键修复】完整初始化所有字段!
             *
             * 原始BUG:
             *   只设置了 temperature/humidity/overall_rms 等顶层字段
             *   未初始化 vibration 子结构体中的 x_rms/y_rms/z_rms/x_fft 等
             *   导致 serialize_access_result_to_json() 访问未定义内存 → LoadProhibited!
             *
             * 修复方案:
             *   显式设置 vibration 子结构体的所有必需字段
             *   确保 JSON 序列化时所有字段都有有效值 (即使是0)
             */
            degraded_result.vibration.x_rms.value = 0.0f;
            degraded_result.vibration.y_rms.value = 0.0f;
            degraded_result.vibration.z_rms.value = 0.0f;
            degraded_result.vibration.vector_rms.value = 0.0f;
            degraded_result.vibration.x_fft.peak_freq = 0.0f;
            degraded_result.vibration.x_fft.peak_amp = 0.0f;
            degraded_result.vibration.x_fft.peak_count = 0;
            degraded_result.vibration.y_fft.peak_freq = 0.0f;
            degraded_result.vibration.y_fft.peak_amp = 0.0f;
            degraded_result.vibration.y_fft.peak_count = 0;
            degraded_result.vibration.z_fft.peak_freq = 0.0f;
            degraded_result.vibration.z_fft.peak_amp = 0.0f;
            degraded_result.vibration.z_fft.peak_count = 0;

            degraded_result.overall_rms_g = 0.0f;
            degraded_result.peak_frequency_hz = 0.0f;
            degraded_result.peak_amplitude_g = 0.0f;
            degraded_result.service_state = SENSOR_STATE_DEGRADED;
            degraded_result.analysis_timestamp_us = (uint32_t)esp_timer_get_time();
            degraded_result.samples_analyzed = 0;
            degraded_result.total_analyses = g_ss.stats.total_analyses_performed + 1;

            /*
             * 标记ADXL345健康状态为不可用
             *
             * 注意: struct adxl345_health_info (adxl345.h:138-146) 没有 description 字段
             * 仅使用现有字段: level, selftest_passed, comm_errors 等
             */
            degraded_result.adxl_health.level = ADXL345_HEALTH_FAULT;
            degraded_result.adxl_health.selftest_passed = false;
            degraded_result.adxl_health.comm_errors = 1;  /* 标记通信异常 */

            static uint32_t last_degraded_upload = 0;
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000LL);

            /*
             * ⚠️ 【关键修复】降级模式下必须dispatch到MQTT!
             *
             * 原始BUG:
             *   降级模式仅LOG记录温湿度数据, 不通过dispatch推入MQTT队列
             *   → PC端Python脚本完全收不到任何数据
             *   → 用户误以为系统卡死
             *
             * 修复方案:
             *   降级结果(含有效温湿度+零值振动)通过dispatch_analysis_callbacks
             *   推入MQTT队列 → PC端能持续收到环境监测数据
             */
            if ((now - last_degraded_upload) >= 2000 ||
                last_degraded_upload == 0) {
                LOG_INFO("SENSOR", "DEGRADED mode: T=%.1f°C H=%.1f%%RH (no ADXL345 vibration) -> dispatching to MQTT",
                         degraded_result.temperature_c,
                         degraded_result.humidity_rh);

                xSemaphoreTake(g_ss.mutex, portMAX_DELAY);
                g_ss.stats.total_analyses_performed++;
                memcpy(&g_ss.latest_result, &degraded_result, sizeof(degraded_result));
                g_ss.has_latest_result = true;
                xSemaphoreGive(g_ss.mutex);

                dispatch_analysis_callbacks(&degraded_result);
                last_degraded_upload = now;
            }

            continue;  /* 继续下一次循环 */
        }

        static struct analysis_result result;
        int ret = perform_single_analysis(&result);

        xSemaphoreTake(g_ss.mutex, portMAX_DELAY);
        g_ss.stats.total_analyses_performed++;
        if (ret == APP_ERR_OK) {
            g_ss.stats.successful_analyses++;
            memcpy(&g_ss.latest_result, &result, sizeof(result));
            g_ss.has_latest_result = true;
            g_ss.state = SENSOR_STATE_RUNNING;
        } else {
            g_ss.stats.failed_analyses++;
            g_ss.state = SENSOR_STATE_DEGRADED;
            notify_error(ret, "Analysis failed");
        }
        g_ss.stats.total_samples_acquired += result.samples_analyzed;
        xSemaphoreGive(g_ss.mutex);

        if (ret == APP_ERR_OK) {
            dispatch_analysis_callbacks(&result);
        } else if (result.temperature_valid) {
            /*
             * ⚠️ 【关键修复】分析失败但有温湿度数据时也必须dispatch!
             *
             * 原始BUG:
             *   ret != APP_ERR_OK 时完全跳过 dispatch_analysis_callbacks
             *   → 即使result中已填充了有效的温湿度数据
             *   → PC端Python脚本收不到任何数据
             *   → 表现为"收到3条数据后卡住"
             *
             * 修复方案:
             *   当 temperature_valid == true 时, 无论振动分析是否成功,
             *   都将结果(含有效温湿度+零值振动)dispatch给MQTT模块
             *   确保PC端能持续收到环境监测数据
             */
            dispatch_analysis_callbacks(&result);
            LOG_INFO("SENSOR", "Dispatched degraded result (temp valid, vib failed, ret=%d)", ret);
        }

        if (g_ss.config.enable_detailed_logging &&
            (g_ss.stats.total_analyses_performed % 10 == 0)) {
            LOG_INFO("SENSOR", "Stats: analyses=%u success=%u fail=%u temp_rx=%u",
                     g_ss.stats.total_analyses_performed,
                     g_ss.stats.successful_analyses,
                     g_ss.stats.failed_analyses,
                     g_ss.stats.temp_data_received);
        }
    }

    LOG_INFO("SENSOR", "Service task stopped");
    vTaskDelete(NULL);
}

/* ==================== 公开 API 实现 ==================== */

int sensor_service_init(const struct sensor_config *config)
{
    if (g_ss.initialized) return APP_ERR_BUSY;

    memset(&g_ss, 0, sizeof(g_ss));

    g_ss.mutex = xSemaphoreCreateMutex();
    if (!g_ss.mutex) {
        LOG_ERROR("SENSOR", "Failed to create mutex");
        return APP_ERR_NO_MEM;
    }

    if (config) {
        memcpy(&g_ss.config, config, sizeof(*config));
    } else {
        apply_default_config();
    }

    /*
     * ========== ADXL345 SPI 配置 ==========
     *
     * ⚠️ 【重要】必须与 hardware_config.h 中的硬件引脚定义一致!
     *
     * 引脚映射 (来自 hardware_config.h, 图三新接线):
     *   SCLK (时钟)  → GPIO36 (FSPICLK / SPI07)
     *   MOSI (数据出) → GPIO35 (FSPID / SPI06)
     *   MISO (数据入) → GPIO37 (FSPIQ / SPI07S)
     *   CS   (片选)  → GPIO40 (MTDO / CLK_OUT2)
     *   INT1 (中断)  → GPIO47 (SPICLK_P)
     *
     * SPI主机选择:
     *   - ESP32-S3 有 SPI2 和 SPI3 两个控制器
     *   - SPI2_HOST = 1 (HSPI, 固定引脚: 11/13/12/10)
     *   - SPI3_HOST = 2 (VSPI, 支持任意GPIO映射) ★ 使用此主机
     *
     * ⚠️ 【关键】新引脚(GPIO35/36/37)为Octal SPI引脚,
     *   必须使用 SPI3_HOST 才能正确映射这些GPIO!
     */
    struct adxl345_spi_config spi_cfg = {
        .host_id = SPI3_HOST,
        .gpio_cs = ADXL345_SPI_CS_PIN,       // GPIO40 - CS (片选)
        .gpio_miso = ADXL345_SPI_MISO_PIN,   // GPIO37 - MISO (数据输入)
        .gpio_mosi = ADXL345_SPI_MOSI_PIN,   // GPIO35 - MOSI (数据输出)
        .gpio_sclk = ADXL345_SPI_SCLK_PIN,   // GPIO36 - SCLK (时钟)
        .clock_speed_hz = 2000000            // 2MHz (降低增强跳线可靠性, ADXL345@400Hz足够)
    };

    /*
     * ========== 初始化 SPI3 主机总线 (必须在使用前完成!) ==========
     *
     * ⚠️ 【关键】ESP-IDF 要求先初始化SPI主机总线,然后才能添加设备!
     *
     * 配置说明:
     *   - SPI3_HOST: 使用SPI3控制器 (支持任意GPIO映射!)
     *   - mosi/miso/sclk: 对应图三新引脚 GPIO35/37/36
     *   - max_transfer_sz: 最大单次传输字节数 (256字节支持FIFO批量读取)
     */
    spi_bus_config_t buscfg = {
        .mosi_io_num = spi_cfg.gpio_mosi,       // GPIO35 - MOSI (数据输出到ADXL345)
        .miso_io_num = spi_cfg.gpio_miso,       // GPIO37 - MISO (从ADXL345输入)
        .sclk_io_num = spi_cfg.gpio_sclk,       // GPIO36 - SCLK (时钟)
        .quadwp_io_num = -1,                    // 不使用WP
        .quadhd_io_num = -1,                    // 不使用HD
        .max_transfer_sz = 256                  // 最大传输256字节 (增大以支持FIFO批量读取)
    };

    bool spi_bus_ok = false;
    esp_err_t spi_ret = spi_bus_initialize(spi_cfg.host_id, &buscfg, SPI_DMA_CH_AUTO);
    if (spi_ret != ESP_OK) {
        LOG_ERROR("SENSOR", "Failed to initialize SPI bus (err=0x%X), attempting to continue without ADXL345", spi_ret);
        LOG_ERROR("SENSOR", "  Possible causes:");
        LOG_ERROR("SENSOR", "    [1] SPI host ID invalid (try SPI3_HOST=2)");
        LOG_ERROR("SENSOR", "    [2] GPIO pins conflict with other peripherals");
        LOG_ERROR("SENSOR", "    [3] Flash/PSRAM using SPI2 (try SPI3_HOST instead)");
        /*
         * SPI总线初始化失败不是致命的:
         * - 可以继续运行(使用STM32温湿度数据)
         * - 振动数据将无法采集
         * - MQTT仍会发送环境数据(温度/湿度)
         */
        spi_bus_ok = false;  // 标记SPI不可用
    } else {
        LOG_INFO("SENSOR", "SPI%d host initialized successfully (MOSI=%d, MISO=%d, SCLK=%d)",
                 spi_cfg.host_id, spi_cfg.gpio_mosi, spi_cfg.gpio_miso, spi_cfg.gpio_sclk);
        spi_bus_ok = true;
    }

    size_t ringbuf_size = sizeof(struct vib_sample) * g_ss.config.ring_buffer_size;
    g_ss.ringbuf_storage = (uint8_t *)malloc(ringbuf_size);
    if (!g_ss.ringbuf_storage) {
        LOG_ERROR("SENSOR", "Failed to allocate ring buffer (%u bytes)",
                  (unsigned)ringbuf_size);
        vSemaphoreDelete(g_ss.mutex);
        return APP_ERR_NO_MEM;
    }

    int ret = ringbuf_init(&g_ss.ringbuf, g_ss.ringbuf_storage,
                            ringbuf_size, true);
    if (ret != APP_ERR_OK) {
        free(g_ss.ringbuf_storage);
        vSemaphoreDelete(g_ss.mutex);
        return ret;
    }

    /*
     * ========== 初始化 ADXL345 传感器 ==========
     *
     * ⚠️ 【关键】仅在SPI总线初始化成功时才尝试初始化ADXL345!
     *
     * 如果 spi_bus_ok == false:
     *   - 不再调用 adxl345_init_spi() (避免 "invalid host" 错误)
     *   - 直接设置 g_ss.adxl = NULL (降级模式)
     *   - 系统仍可正常运行(使用STM32温湿度数据)
     */
    if (spi_bus_ok) {
        /*
         * ⚠️ 【关键修复】必须传入中断配置!
         *
         * 原始BUG:
         *   adxl345_init_spi() 的第5个参数 int_cfg 传入了 NULL
         *   导致:
         *     [1] INT1引脚中断未使能 (GPIO ISR未注册)
         *     [2] ADXL345 FIFO水印中断源未启用
         *     [3] acquire_task进入轮询模式但可能读不到数据
         *     [4] 结果: "No data from ADXL345 for analysis"
         *
         * 正确做法:
         *   配置INT1中断,使能FIFO水印中断
         *   当FIFO样本数达到水位(默认16)时触发INT1
         *   ISR通知acquire_task读取FIFO数据
         */
        struct adxl345_int_config int_cfg = {
            .enable_int1 = true,
            .enable_int2 = false,
            .gpio_int1 = ADXL345_INT1_PIN,
            .gpio_int2 = -1,
            .int1_sources = ADXL345_INT_WATERMARK | ADXL345_INT_OVERRUN,
            .int2_sources = 0
        };

        LOG_INFO("SENSOR", "[DEBUG] Initializing ADXL345 with INT1 interrupt (GPIO%d)...",
                 ADXL345_INT1_PIN);

        g_ss.adxl = adxl345_init_spi(&spi_cfg, ADXL345_RANGE_16G,
                                     ADXL345_RATE_400,
                                     ADXL345_FIFO_STREAM, &int_cfg,
                                     &g_ss.ringbuf);
        if (!g_ss.adxl) {
            /*
             * ⚠️ ADXL345初始化失败 - 降级运行模式!
             *
             * 可能的原因:
             *   [1] ADXL345模块未连接或接线错误
             *   [2] SPI通信故障 (引脚配置/硬件问题)
             *   [3] ADXL345芯片损坏
             *
             * 降级影响:
             *   ❌ 无法采集振动加速度数据
             *   ✅ 仍可接收STM32的温湿度数据
             *   ✅ MQTT仍可发送环境数据(温度/湿度/时间戳)
             *   ✅ 系统不会崩溃,可以继续运行
             */
            LOG_WARN("SENSOR", "⚠ ADXL345 init failed, running in DEGRADED mode (vibration data unavailable)");
            LOG_WARN("SENSOR", "   - Temperature/Humidity from STM32: AVAILABLE");
            LOG_WARN("SENSOR", "   - Vibration data from ADXL345: UNAVAILABLE");

            /*
             * 不返回错误! 继续运行!
             * 后续代码会检测到g_ss.adxl==NULL并跳过振动相关操作
             */
            g_ss.adxl = NULL;  // 明确标记为NULL
        } else {
            LOG_INFO("SENSOR", "ADXL345 initialized successfully (SPI%d, 5MHz)", spi_cfg.host_id);

            struct dsp_config dsp_cfg = {
                .default_fft_size = g_ss.config.fft_size,
                .window_type = g_ss.config.window_type,
                .enable_dc_removal = true
            };
            ret = dsp_init(&dsp_cfg);
            if (ret != APP_ERR_OK) {
                LOG_ERROR("SENSOR", "Failed to initialize DSP module: %d", ret);
                adxl345_deinit(g_ss.adxl);
                g_ss.adxl = NULL;
                ringbuf_deinit(&g_ss.ringbuf);
                free(g_ss.ringbuf_storage);
                g_ss.ringbuf_storage = NULL;
                vSemaphoreDelete(g_ss.mutex);
                g_ss.mutex = NULL;
                return ret;
            }

            if (g_ss.config.enable_temp_compensation) {
                ret = temp_comp_init(NULL);
                if (ret != APP_ERR_OK) {
                    LOG_WARN("SENSOR", "Temp compensation init failed, disabling: %d", ret);
                    g_ss.config.enable_temp_compensation = false;
                }
            }

            if (g_ss.config.enable_protocol_temp && protocol_is_initialized()) {
                protocol_register_temp_callback(on_protocol_temperature, NULL);
                LOG_INFO("SENSOR", "Protocol temperature callback registered");
            }

            /*
             * ⚠️ 【关键修复】延迟self-test到acquire_task稳定运行后!
             *
             * 原始BUG:
             *   self-test在adxl345_init_spi()之后立即执行
             *   导致:
             *     [1] 读取数据寄存器触发FIFO重置
             *     [2] acquire_task刚启动时ringbuf为空
             *     [3] sensor_service_task首次分析必然失败
             *     [4] 日志持续显示"No data from ADXL345"
             *
             * 根本原因:
             *   ADXL345的self-test需要切换到MEASURE模式并读取数据寄存器
             *   这个操作会干扰FIFO的正常数据采集流程
             *   特别是在刚初始化、FIFO还未稳定工作时
             *
             * 解决方案:
             *   [方案A - 推荐] 完全跳过运行时self-test
             *     理由: adxl345_init_spi()已验证Device ID=0xE5
             *           说明SPI通信正常,硬件连接正确
             *           self-test主要用于生产测试,不适合每次启动都执行
             *
             *   [方案B - 备选] 延迟self-test到5秒后
             *     让acquire_task先稳定运行,填满ringbuf
             *     然后再执行self-test(此时FIFO已有数据缓冲)
             *
             * 当前实现: 采用方案A (注释掉self-test)
             * 如需启用self-test用于调试,取消下方注释并注释掉方案A
             */

            /* ===== 方案A: 跳过self-test (推荐用于生产环境) ===== */
            LOG_INFO("SENSOR", "Skipping runtime self-test (Device ID verified during init)");
            /*
             * 注意: 无法直接设置 g_ss.adxl->health.selftest_passed
             * 因为 struct adxl345_dev 是不透明类型(opaque type)
             * 其定义在 adxl345.c 中, sensor_service.c 只能访问头文件中的函数API
             *
             * 替代方案:
             *   - adxl345_init_spi() 已验证 Device ID = 0xE5 (SPI通信正常)
             *   - 后续分析任务成功执行时,会自动更新健康状态
             *   - 如需强制标记self-test通过,需在 adxl345.h/c 中添加 setter 函数
             */

            /* ===== 方案B: 延迟self-test (用于调试,取消注释以启用) ===== */
            /*
            LOG_INFO("SENSOR", "Scheduling delayed self-test in 5 seconds...");
            // 注意: 此处不直接调用,而是在sensor_service_task中延迟执行
            // 或使用FreeRTOS定时器触发
            */

        }
    } else {
        /*
         * SPI总线初始化失败 - 直接进入降级模式
         */
        LOG_WARN("SENSOR", "⚠ SPI bus initialization failed, running in DEGRADED mode");
        LOG_WARN("SENSOR", "   - Temperature/Humidity from STM32: AVAILABLE");
        LOG_WARN("SENSOR", "   - Vibration data from ADXL345: UNAVAILABLE (SPI error)");
        g_ss.adxl = NULL;  // 明确标记为NULL
    }

    /*
     * 启动ADXL345数据采集任务 (仅在成功初始化时)
     * 如果g_ss.adxl==NULL (降级模式),跳过此步骤
     */
    if (g_ss.adxl) {
        ret = adxl345_start(g_ss.adxl, SENSOR_TASK_PRIORITY - 1,
                            4096);
        if (ret != APP_ERR_OK) {
            LOG_ERROR("SENSOR", "Failed to start ADXL345: %d", ret);
            dsp_deinit();
            adxl345_deinit(g_ss.adxl);
            g_ss.adxl = NULL;
            ringbuf_deinit(&g_ss.ringbuf);
            free(g_ss.ringbuf_storage);
            g_ss.ringbuf_storage = NULL;
            vSemaphoreDelete(g_ss.mutex);
            g_ss.mutex = NULL;
            return ret;
        }
        LOG_INFO("SENSOR", "ADXL345 data acquisition task started");
    } else {
        LOG_WARN("SENSOR", "Skipping ADXL345 start (DEGRADED mode - no vibration data)");
    }

    g_ss.initialized = true;
    g_ss.state = SENSOR_STATE_INITIALIZED;
    g_ss.has_temperature = false;
    g_ss.has_latest_result = false;

    LOG_INFO("SENSOR", "Sensor service initialized "
             "(rate=%dHz, fft=%d, temp_comp=%s, proto_temp=%s)",
             g_ss.config.sample_rate_hz,
             g_ss.config.fft_size,
             g_ss.config.enable_temp_compensation ? "ON" : "OFF",
             g_ss.config.enable_protocol_temp ? "ON" : "OFF");
    return APP_ERR_OK;
}

int sensor_service_deinit(void)
{
    if (!g_ss.initialized) return APP_ERR_SENSOR_NOT_INIT;

    sensor_service_stop();

    if (g_ss.adxl) {
        adxl345_stop(g_ss.adxl);
        adxl345_deinit(g_ss.adxl);
        g_ss.adxl = NULL;
    }

    dsp_deinit();

    if (g_ss.config.enable_temp_compensation) {
        temp_comp_deinit();
    }

    if (g_ss.ringbuf_storage) {
        ringbuf_deinit(&g_ss.ringbuf);
        free(g_ss.ringbuf_storage);
        g_ss.ringbuf_storage = NULL;
    }

    if (g_ss.mutex) {
        vSemaphoreDelete(g_ss.mutex);
        g_ss.mutex = NULL;
    }

    g_ss.initialized = false;
    g_ss.state = SENSOR_STATE_UNINITIALIZED;
    memset(&g_ss, 0, sizeof(g_ss));

    LOG_INFO("SENSOR", "Sensor service deinitialized");
    return APP_ERR_OK;
}

bool sensor_service_is_initialized(void)
{
    return g_ss.initialized;
}

int sensor_service_start(void)
{
    if (!g_ss.initialized) return APP_ERR_SENSOR_NOT_INIT;
    if (g_ss.running) return APP_ERR_BUSY;

    g_ss.running = true;
    g_ss.state = SENSOR_STATE_RUNNING;

    BaseType_t ret = xTaskCreate(sensor_service_task, "sensor_svc",
                                  SENSOR_TASK_STACK_SIZE, NULL,
                                  SENSOR_TASK_PRIORITY, &g_ss.task_handle);
    if (ret != pdPASS) {
        LOG_ERROR("SENSOR", "Failed to create service task");
        g_ss.running = false;
        g_ss.state = SENSOR_STATE_ERROR;
        return APP_ERR_TASK_CREATE_FAIL;
    }

    LOG_INFO("SENSOR", "Sensor service started (analysis interval=%u ms)",
             g_ss.config.analysis_interval_ms);
    return APP_ERR_OK;
}

int sensor_service_stop(void)
{
    if (!g_ss.initialized) return APP_ERR_SENSOR_NOT_INIT;
    if (!g_ss.running) return APP_ERR_OK;

    g_ss.running = false;
    g_ss.state = SENSOR_STATE_INITIALIZED;

    if (g_ss.task_handle) {
        vTaskDelay(pdMS_TO_TICKS(200));
        g_ss.task_handle = NULL;
    }

    LOG_INFO("SENSOR", "Sensor service stopped");
    return APP_ERR_OK;
}

/* ==================== 查询 API 实现 ==================== */

int sensor_service_get_latest_analysis(struct analysis_result *result)
{
    if (!result) return APP_ERR_INVALID_PARAM;
    if (!g_ss.initialized) return APP_ERR_SENSOR_NOT_INIT;

    xSemaphoreTake(g_ss.mutex, portMAX_DELAY);
    if (!g_ss.has_latest_result) {
        xSemaphoreGive(g_ss.mutex);
        return APP_ERR_NO_DATA;
    }
    memcpy(result, &g_ss.latest_result, sizeof(*result));
    xSemaphoreGive(g_ss.mutex);

    return APP_ERR_OK;
}

int sensor_service_fetch_sample(struct vib_sample *sample)
{
    if (!sample) return APP_ERR_INVALID_PARAM;
    if (!g_ss.initialized) return APP_ERR_SENSOR_NOT_INIT;

    size_t len = ringbuf_pop(&g_ss.ringbuf, (uint8_t *)sample, sizeof(*sample));
    return (len == sizeof(*sample)) ? APP_ERR_OK : APP_ERR_TIMEOUT;
}

int sensor_service_get_current_temperature(float *temp_c)
{
    if (!temp_c) return APP_ERR_INVALID_PARAM;

    xSemaphoreTake(g_ss.mutex, portMAX_DELAY);
    if (!g_ss.has_temperature) {
        xSemaphoreGive(g_ss.mutex);
        return APP_ERR_TEMP_NO_DATA;
    }
    *temp_c = g_ss.current_temp_c;
    xSemaphoreGive(g_ss.mutex);

    return APP_ERR_OK;
}

/* ==================== 注册 API 实现 ==================== */

int sensor_service_register_analysis_callback(analysis_ready_callback_t cb,
                                               void *user_data)
{
    if (!cb) return APP_ERR_INVALID_PARAM;
    if (!g_ss.initialized) return APP_ERR_SENSOR_NOT_INIT;
    if (g_ss.analysis_callback_count >= SENSOR_MAX_CALLBACKS) {
        return APP_ERR_NO_SPACE;
    }

    xSemaphoreTake(g_ss.mutex, portMAX_DELAY);
    g_ss.analysis_callbacks[g_ss.analysis_callback_count].cb = cb;
    g_ss.analysis_callbacks[g_ss.analysis_callback_count].user_data = user_data;
    g_ss.analysis_callback_count++;
    xSemaphoreGive(g_ss.mutex);

    LOG_INFO("SENSOR", "Analysis callback registered (total=%d)",
             g_ss.analysis_callback_count);
    return APP_ERR_OK;
}

int sensor_service_unregister_analysis_callback(analysis_ready_callback_t cb)
{
    if (!cb || !g_ss.initialized) return APP_ERR_INVALID_PARAM;

    xSemaphoreTake(g_ss.mutex, portMAX_DELAY);
    for (int i = 0; i < g_ss.analysis_callback_count; i++) {
        if (g_ss.analysis_callbacks[i].cb == cb) {
            for (int j = i; j < g_ss.analysis_callback_count - 1; j++) {
                g_ss.analysis_callbacks[j] = g_ss.analysis_callbacks[j + 1];
            }
            g_ss.analysis_callback_count--;
            xSemaphoreGive(g_ss.mutex);
            return APP_ERR_OK;
        }
    }
    xSemaphoreGive(g_ss.mutex);
    return APP_ERR_NOT_SUPPORTED;
}

int sensor_service_register_error_callback(sensor_error_callback_t cb,
                                            void *user_data)
{
    if (!g_ss.initialized) return APP_ERR_SENSOR_NOT_INIT;
    g_ss.error_cb = cb;
    g_ss.error_user_data = user_data;
    return APP_ERR_OK;
}

enum sensor_state sensor_service_get_state(void)
{
    return g_ss.state;
}

int sensor_service_get_stats(struct sensor_stats *stats)
{
    if (!stats) return APP_ERR_INVALID_PARAM;
    if (!g_ss.initialized) return APP_ERR_SENSOR_NOT_INIT;

    xSemaphoreTake(g_ss.mutex, portMAX_DELAY);
    memcpy(stats, &g_ss.stats, sizeof(*stats));
    xSemaphoreGive(g_ss.mutex);
    return APP_ERR_OK;
}

void sensor_service_reset_stats(void)
{
    if (!g_ss.initialized) return;
    xSemaphoreTake(g_ss.mutex, portMAX_DELAY);
    memset(&g_ss.stats, 0, sizeof(g_ss.stats));
    xSemaphoreGive(g_ss.mutex);
}

int sensor_service_force_analysis(void)
{
    if (!g_ss.initialized || !g_ss.running) return APP_ERR_SENSOR_NOT_INIT;

    struct analysis_result result;
    int ret = perform_single_analysis(&result);

    if (ret == APP_ERR_OK) {
        dispatch_analysis_callbacks(&result);
    }

    return ret;
}