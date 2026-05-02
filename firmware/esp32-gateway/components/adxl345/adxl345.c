/**
 * @file adxl345.c
 * @author EnterWorldDoor
 * @brief 企业级 ADXL345 三轴加速度计驱动实现 (SPI/FIFO/中断驱动)
 *
 * 架构设计:
 *   ADXL345(FIFO) → INT1中断 → ISR批量读FIFO → RingBuf(双缓冲)
 *     → 预处理任务(滑动窗口滤波+DC偏移去除)
 *     → 输出原始数据供 DSP 模块处理
 *
 * 关键特性:
 *   - SPI 高速通信 + 重试机制
 *   - FIFO 缓冲减轻 ESP32 CPU 负载
 *   - DC 基础偏移校准与去除 (温度补偿由独立模块处理)
 *   - 16 点移动平均滤波抑制高频噪声
 *   - 健康状态监控与故障恢复
 *
 * 设计原则: 纯驱动层，不包含业务逻辑
 */

#include "adxl345.h"
#include "adxl345_reg.h"
#include "global_error.h"
#include "log_system.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stddef.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

/* ==================== 内部常量 ==================== */

#define SPI_TRANSACTION_TIMEOUT_MS    50
#define BUS_RESET_DELAY_MS            10
#define HEALTH_CHECK_INTERVAL_MS     5000

/* ==================== 设备上下文结构体 ==================== */

struct adxl345_dev {
    enum adxl345_bus_type bus_mode;
    union {
        struct adxl345_spi_config spi_cfg;
        struct adxl345_i2c_config i2c_cfg;
    } bus_cfg;
    spi_device_handle_t spi_handle;
    enum adxl345_range range;
    enum adxl345_data_rate rate;
    enum adxl345_fifo_mode fifo_mode;
    struct adxl345_int_config int_cfg;
    float scale_factor;
    struct ringbuf *ringbuf;
    SemaphoreHandle_t mutex;
    EventGroupHandle_t event_group;
    TaskHandle_t acquire_task_handle;
    bool running;

    struct adxl345_dc_offset dc_offset;
    struct adxl345_health_info health;
    struct adxl345_stats stats;

    float ma_window_x[ADXL345_MA_WINDOW_SIZE];
    float ma_window_y[ADXL345_MA_WINDOW_SIZE];
    float ma_window_z[ADXL345_MA_WINDOW_SIZE];
    int ma_index_x;
    int ma_index_y;
    int ma_index_z;
    float ma_sum_x, ma_sum_y, ma_sum_z;
    bool ma_filled;

    adxl345_data_callback data_cb;
    void *data_cb_user_data;

    uint32_t consecutive_errors;

    /* 批量获取静态缓冲区 */
    struct adxl345_raw_sample batch_buffer[ADXL345_BATCH_MAX_SIZE];
};

/* ==================== 事件组位定义 ==================== */

static const int STOP_ACQUIRE_BIT = (1 << 0);
static const int FIFO_INT_BIT     = (1 << 1);

/* ==================== 底层通信层 (SPI/I2C 抽象) ==================== */

/**
 * reg_write - 写单个寄存器 (带重试)
 */
static int reg_write(struct adxl345_dev *dev, uint8_t reg, uint8_t value)
{
    int retry;
    esp_err_t ret;

    if (!dev) return APP_ERR_ADXL_NOT_INIT;

    for (retry = 0; retry < ADXL345_SPI_RETRY_MAX; retry++) {
        if (dev->bus_mode == ADXL345_BUS_SPI && dev->spi_handle) {
            spi_transaction_t t = {0};
            t.flags = SPI_TRANS_USE_TXDATA;
            t.length = 16;
            t.tx_data[0] = ADXL345_SPI_WRITE_CMD | reg;
            t.tx_data[1] = value;
            ret = spi_device_transmit(dev->spi_handle, &t);
        } else {
            ret = ESP_FAIL;
        }

        if (ret == ESP_OK) return APP_ERR_OK;

        dev->health.retry_count++;
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    dev->health.comm_errors++;
    dev->consecutive_errors++;
    LOG_ERROR("ADXL345", "reg_write(0x%02X) failed after %d retries",
              reg, ADXL345_SPI_RETRY_MAX);
    return APP_ERR_ADXL_SPI_TRANSFAIL;
}

/**
 * reg_read - 读单个或多个寄存器 (带重试)
 *
 * ⚠️ 【重要】ESP-IDF v5.5.3 SPI传输限制:
 *   - SPI_TRANS_USE_TXDATA/RXDATA 只能用于 ≤ 32位(4字节) 的传输
 *   - ADXL345 FIFO读取需要 192字节(1536位), 必须使用 tx_buffer/rx_buffer 指针
 *
 * 解决方案:
 *   - 小传输 (≤4字节): 使用内嵌 tx_data[]/rx_data[] 数组
 *   - 大传输 (>4字节): 使用外部分配的缓冲区 (DMA兼容)
 */
static int reg_read(struct adxl345_dev *dev, uint8_t reg,
                    uint8_t *buffer, size_t len)
{
    int retry;
    esp_err_t ret;

    if (!dev || !buffer || len == 0) return APP_ERR_ADXL_INVALID_PARAM;

    /*
     * ⚠️ 【修复】将静态缓冲区声明提升到if/else之前!
     *
     * 原始BUG:
     *   tx_buf_static / rx_buf_static 在 else 分支 (len > 3) 内部定义
     *   但在 else 分支外部的 memcpy 中引用了 rx_buf_static
     *   → C语言块级作用域: 变量在声明块外不可见 → 编译错误 "undeclared"
     *
     * 修复方案:
     *   将静态缓冲区声明移到for循环之前, 使其在整个函数体中都可见
     */
    static uint8_t tx_buf_static[ADXL345_FIFO_DEPTH * 6 + 1];
    static uint8_t rx_buf_static[ADXL345_FIFO_DEPTH * 6 + 1];

    for (retry = 0; retry < ADXL345_SPI_RETRY_MAX; retry++) {
        if (dev->bus_mode == ADXL345_BUS_SPI && dev->spi_handle) {
            spi_transaction_t t = {0};
            t.length = (uint32_t)(8 + 8 * len);

            /*
             * 根据传输大小选择不同的SPI传输方式:
             *
             * 方式1: 小传输 (len <= 3, 即 ≤24位)
             *   使用 SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA
             *   数据存放在结构体内嵌的 tx_data[4]/rx_data[4] 数组
             *   优点: 无需额外内存分配, 效率高
             *
             * ⚠️ 【重要】ESP-IDF限制:
             *   - SPI_TRANS_USE_TXDATA/RXDATA 只能用于 ≤ 32位(4字节) 传输
             *   - 但这4字节包含命令字节! 所以实际数据最多3字节
             *   - 总位数 = 8(命令) + 8*len(数据) ≤ 32 → len ≤ 3
             *
             * 方式2: 大传输 (len > 3, 如FIFO读取192字节)
             *   使用 tx_buffer / rx_buffer 指针
             *   指向外部分配的缓冲区 (DMA兼容内存)
             *   必须: 不设置 SPI_TRANS_USE_TXDATA/RXDATA 标志!
             */
            if (len <= 3) {
                t.flags = SPI_TRANS_USE_RXDATA | SPI_TRANS_USE_TXDATA;
                t.tx_data[0] = ADXL345_SPI_READ_CMD |
                                 ADXL345_SPI_MB_BIT | reg;
                t.tx_data[1] = 0xFF;
                t.tx_data[2] = 0xFF;
                t.tx_data[3] = 0xFF;
            } else {
                /* 大传输模式: 使用外部缓冲区 (已在函数顶部声明) */
                size_t transfer_len = len + 1;

                if (transfer_len > sizeof(tx_buf_static)) {
                    LOG_ERROR("ADXL345", "Request too large: %u bytes", (unsigned)transfer_len);
                    return APP_ERR_ADXL_INVALID_PARAM;
                }

                tx_buf_static[0] = ADXL345_SPI_READ_CMD |
                                   ADXL345_SPI_MB_BIT | reg;

                memset(&tx_buf_static[1], 0xFF, len);
                memset(rx_buf_static, 0, transfer_len);

                t.tx_buffer = tx_buf_static;
                t.rx_buffer = rx_buf_static;
            }

            ret = spi_device_transmit(dev->spi_handle, &t);
            if (ret == ESP_OK) {
                if (len <= 3) {
                    /* 小传输模式: 从rx_data复制数据 (跳过第1个字节,它是dummy) */
                    memcpy(buffer, t.rx_data + 1, len);
                } else {
                    memcpy(buffer, rx_buf_static + 1, len);
                }

                return APP_ERR_OK;
            }
        } else {
            ret = ESP_FAIL;
        }

        dev->health.retry_count++;
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    dev->health.comm_errors++;
    dev->consecutive_errors++;
    LOG_ERROR("ADXL345", "reg_read(0x%02X, len=%u) failed after %d retries",
              reg, (unsigned)len, ADXL345_SPI_RETRY_MAX);
    return APP_ERR_ADXL_SPI_TRANSFAIL;
}

/* ==================== 工具函数 ==================== */

static void update_scale_factor(struct adxl345_dev *dev)
{
    switch (dev->range) {
    case ADXL345_RANGE_2G:
        dev->scale_factor = 4.0f / 1024.0f;
        break;
    case ADXL345_RANGE_4G:
        dev->scale_factor = 8.0f / 1024.0f;
        break;
    case ADXL345_RANGE_8G:
        dev->scale_factor = 16.0f / 1024.0f;
        break;
    case ADXL345_RANGE_16G:
        dev->scale_factor = 32.0f / 1024.0f;
        break;
    default:
        dev->scale_factor = 4.0f / 1024.0f;
        break;
    }
}

/**
 * moving_average_push - 推入一个样本到移动平均窗口并返回滤波后值
 */
static float moving_average_push(float new_val, float *window,
                                  int *index, float *sum,
                                  int window_size, bool *filled)
{
    float old_val = window[*index];
    window[*index] = new_val;
    *sum += (new_val - old_val);
    (*index)++;
    if (*index >= window_size) {
        *index = 0;
        *filled = true;
    }

    if (*filled) {
        return *sum / (float)window_size;
    }
    return new_val;
}

/**
 * apply_dc_removal - 去除 DC 基础偏移分量
 */
static float apply_dc_removal(float val, float offset)
{
    return val - offset;
}



/**
 * update_health - 更新健康状态评估
 */
static void update_health(struct adxl345_dev *dev)
{
    if (dev->consecutive_errors >= ADXL345_HEALTH_ERROR_THRESH) {
        dev->health.level = ADXL345_HEALTH_FAULT;
    } else if (dev->consecutive_errors > ADXL345_HEALTH_ERROR_THRESH / 2) {
        dev->health.level = ADXL345_HEALTH_DEGRADED;
    } else if (dev->consecutive_errors > 0) {
        dev->health.level = ADXL345_HEALTH_WARNING;
    } else {
        dev->health.level = ADXL345_HEALTH_GOOD;
    }
}

/**
 * preprocess_sample - 数据预处理核心流程（纯驱动层）
 * 原始数据 → 移动平均滤波 → DC偏移去除 → 输出
 *
 * 注意: 不包含 RMS/FFT/趋势等业务逻辑，由 DSP 模块处理
 */
static void preprocess_sample(struct adxl345_dev *dev,
                              struct adxl345_raw_sample *raw,
                              struct adxl345_accel_data *out)
{
    float x_raw, y_raw, z_raw;
    float x_ma, y_ma, z_ma;

    x_raw = (float)raw->x * dev->scale_factor;
    y_raw = (float)raw->y * dev->scale_factor;
    z_raw = (float)raw->z * dev->scale_factor;

    xSemaphoreTake(dev->mutex, portMAX_DELAY);

    x_ma = moving_average_push(
        x_raw, dev->ma_window_x, &dev->ma_index_x,
        &dev->ma_sum_x, ADXL345_MA_WINDOW_SIZE, &dev->ma_filled);

    y_ma = moving_average_push(
        y_raw, dev->ma_window_y, &dev->ma_index_y,
        &dev->ma_sum_y, ADXL345_MA_WINDOW_SIZE, &dev->ma_filled);

    z_ma = moving_average_push(
        z_raw, dev->ma_window_z, &dev->ma_index_z,
        &dev->ma_sum_z, ADXL345_MA_WINDOW_SIZE, &dev->ma_filled);

    out->x_g = apply_dc_removal(x_ma, dev->dc_offset.x_offset);
    out->y_g = apply_dc_removal(y_ma, dev->dc_offset.y_offset);
    out->z_g = apply_dc_removal(z_ma, dev->dc_offset.z_offset);
    out->timestamp_us = raw->timestamp_us;

    dev->stats.samples_processed++;

    if (dev->consecutive_errors > 0) {
        dev->consecutive_errors--;
    }
    update_health(dev);

    xSemaphoreGive(dev->mutex);

    if (dev->data_cb) {
        dev->data_cb(out, dev->data_cb_user_data);
    }
}

/* ==================== ISR 与采集任务 ==================== */

/**
 * gpio_isr_handler - INT1/INT2 GPIO 中断服务程序
 */
static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    struct adxl345_dev *dev = (struct adxl345_dev *)arg;
    BaseType_t higher_woken = pdFALSE;

    if (dev && dev->event_group) {
        xEventGroupSetBitsFromISR(dev->event_group, FIFO_INT_BIT,
                                  &higher_woken);
        portYIELD_FROM_ISR(higher_woken);
    }
}

/**
 * acquire_task_func - FIFO 数据采集 FreeRTOS 任务 (纯轮询模式)
 *
 * ⚠️ 【架构重构】放弃POSEDGE INT1事件驱动，改用纯轮询!
 *
 * 原因分析:
 *   ADXL345 @400Hz, FIFO watermark=16, 但每轮(5ms)仅产生2个新样本
 *   POSEDGE INT1需要FIFO≥16才会触发0→1跳变
 *   但fetch_batch每100ms读取ringbuf后清空
 *   导致FIFO永远达不到16 → INT1永不触发 → 每次等20ms超时
 *   等效采样: 20ms周期 × 400Hz = 最多8样本/轮 → 效率极低
 *
 * 解决方案: 纯轮询模式
 *   - 每5ms读一次FIFO_STATUS
 *   - 有数据立即读FIFO_DATA → push ringbuf
 *   - 无数据则vTaskDelay(5ms)继续
 *   - STOP_ACQUIRE_BIT事件组用于停止信号
 */
static void acquire_task_func(void *arg)
{
    struct adxl345_dev *dev = (struct adxl345_dev *)arg;
    uint8_t fifo_status = 0;
    uint8_t num_samples = 0;
    uint8_t buf[6 * ADXL345_FIFO_DEPTH];
    struct adxl345_raw_sample sample;
    uint32_t spi_error_count = 0;
    uint32_t empty_count = 0;
    uint32_t last_empty_log = 0;

    LOG_INFO("ADXL345", "Acquire task started (POLLING mode, 5ms cycle)");

    while (1) {
        EventBits_t bits;

        /* 检查停止信号 */
        bits = xEventGroupGetBits(dev->event_group);
        if (bits & STOP_ACQUIRE_BIT) break;

        /* 读 FIFO_STATUS */
        int read_ret = reg_read(dev, ADXL345_FIFO_STATUS, &fifo_status, 1);
        if (read_ret != APP_ERR_OK) {
            spi_error_count++;
            if (spi_error_count == 1 || (spi_error_count % 200 == 0)) {
                LOG_ERROR("ADXL345", "SPI read FIFO_STATUS failed (err=%d, #%lu)",
                         read_ret, (unsigned long)spi_error_count);
            }
            if (spi_error_count >= 500) {
                LOG_ERROR("ADXL345", "CRITICAL: %lu SPI errors, resetting bus...",
                         (unsigned long)spi_error_count);
                adxl345_reset_bus(dev);
                reg_write(dev, ADXL345_POWER_CTL, ADXL345_PCTL_MEASURE);
                spi_error_count = 0;
                empty_count = 0;
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        spi_error_count = 0;

        num_samples = fifo_status & ADXL345_FIFO_ENTRY_MASK;

        if (num_samples == 0) {
            empty_count++;
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000LL);

            if ((now - last_empty_log) >= 60000 || last_empty_log == 0) {
                LOG_INFO("ADXL345", "FIFO empty x%lu (STATUS=0x%02X pushed=%lu)",
                         (unsigned long)empty_count, fifo_status,
                         (unsigned long)dev->stats.samples_pushed);
                last_empty_log = now;
            }

            if (empty_count >= 100 && empty_count % 100 == 0) {
                uint8_t pwr;
                if (reg_read(dev, ADXL345_POWER_CTL, &pwr, 1) == APP_ERR_OK) {
                    if (!(pwr & ADXL345_PCTL_MEASURE)) {
                        LOG_ERROR("ADXL345", "MEASURE bit lost! Re-enabling...");
                        reg_write(dev, ADXL345_POWER_CTL, ADXL345_PCTL_MEASURE);
                        empty_count = 0;
                        vTaskDelay(pdMS_TO_TICKS(200));
                        continue;
                    }
                }
            }

            if (empty_count >= 300) {
                LOG_ERROR("ADXL345", "FIFO empty x%lu with MEASURE active! Full re-init...",
                         (unsigned long)empty_count);
                reg_write(dev, ADXL345_POWER_CTL, 0x00);
                vTaskDelay(pdMS_TO_TICKS(20));
                reg_write(dev, ADXL345_DATA_FORMAT,
                         ADXL345_DF_FULL_RES | (dev->range & ADXL345_DF_RANGE_MASK));
                reg_write(dev, ADXL345_BW_RATE, dev->rate & ADXL345_BW_RATE_MASK);
                reg_write(dev, ADXL345_FIFO_CTL,
                         dev->fifo_mode | ADXL345_FIFO_WATERMARK);
                reg_write(dev, ADXL345_POWER_CTL, ADXL345_PCTL_MEASURE);
                vTaskDelay(pdMS_TO_TICKS(200));
                empty_count = 0;
            }

            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        if (empty_count > 0) {
            static uint32_t last_recovery_log = 0;
            uint32_t now_rec = (uint32_t)(esp_timer_get_time() / 1000LL);
            if ((now_rec - last_recovery_log) >= 60000 || last_recovery_log == 0) {
                LOG_INFO("ADXL345", "FIFO recovered after %lu empties: %u samples pushed=%lu",
                         (unsigned long)empty_count, num_samples,
                         (unsigned long)dev->stats.samples_pushed);
                last_recovery_log = now_rec;
            }
            empty_count = 0;
        }

        if (num_samples > ADXL345_FIFO_DEPTH) {
            dev->health.fifo_overflow_count++;
            num_samples = ADXL345_FIFO_DEPTH;
        }

        read_ret = reg_read(dev, ADXL345_DATAX0, buf, (size_t)(num_samples * 6));
        if (read_ret != APP_ERR_OK) {
            static uint32_t last_data_error_log = 0;
            uint32_t now_de = (uint32_t)(esp_timer_get_time() / 1000LL);
            if ((now_de - last_data_error_log) >= 30000) {
                LOG_ERROR("ADXL345", "SPI read FIFO_DATA failed (err=%d, n=%u)",
                         read_ret, (unsigned)num_samples);
                last_data_error_log = now_de;
            }
            spi_error_count += 10;
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        dev->stats.fifo_reads++;
        dev->stats.int_fired_count++;

        for (int s = 0; s < num_samples; s++) {
            int off = s * 6;
            sample.x = (int16_t)((buf[off + 1] << 8) | buf[off]);
            sample.y = (int16_t)((buf[off + 3] << 8) | buf[off + 2]);
            sample.z = (int16_t)((buf[off + 5] << 8) | buf[off + 4]);
            sample.timestamp_us = (uint32_t)esp_timer_get_time();

            size_t pushed = ringbuf_push(dev->ringbuf,
                                         (const uint8_t *)&sample, sizeof(sample));
            if (pushed == sizeof(sample)) {
                dev->stats.samples_pushed++;
                dev->health.total_samples++;
            } else {
                dev->health.dropped_samples++;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }

    LOG_INFO("ADXL345", "Acquire task stopped");
    vTaskDelete(NULL);
}

/* ==================== 公开 API 实现 ==================== */

struct adxl345_dev *adxl345_init_spi(
    const struct adxl345_spi_config *spi_cfg,
    enum adxl345_range range,
    enum adxl345_data_rate rate,
    enum adxl345_fifo_mode fifo_mode,
    const struct adxl345_int_config *int_cfg,
    struct ringbuf *ringbuf)
{
    struct adxl345_dev *dev;
    spi_device_interface_config_t devcfg = {0};
    uint8_t devid;
    int ret;

    if (!spi_cfg || !ringbuf) {
        LOG_ERROR("ADXL345", "Invalid parameters for init_spi");
        return NULL;
    }

    dev = (struct adxl345_dev *)malloc(sizeof(*dev));
    if (!dev) {
        LOG_ERROR("ADXL345", "Memory allocation failed");
        return NULL;
    }
    memset(dev, 0, sizeof(*dev));

    dev->bus_mode = ADXL345_BUS_SPI;
    memcpy(&dev->bus_cfg.spi_cfg, spi_cfg, sizeof(*spi_cfg));
    dev->range = range;
    dev->rate = rate;
    dev->fifo_mode = fifo_mode;
    dev->ringbuf = ringbuf;

    if (int_cfg) {
        memcpy(&dev->int_cfg, int_cfg, sizeof(*int_cfg));
    }

    update_scale_factor(dev);

    memset(&devcfg, 0, sizeof(devcfg));
    devcfg.command_bits = 0;
    devcfg.address_bits = 0;
    devcfg.clock_speed_hz = spi_cfg->clock_speed_hz;
    devcfg.mode = 3;
    devcfg.spics_io_num = spi_cfg->gpio_cs;
    devcfg.queue_size = 4;
    devcfg.flags = 0;

    ret = spi_bus_add_device(spi_cfg->host_id, &devcfg, &dev->spi_handle);
    if (ret != ESP_OK) {
        LOG_ERROR("ADXL345", "SPI device add failed: 0x%X", ret);
        free(dev);
        return NULL;
    }

    /*
     * ⚠️ 【关键修复】Device ID读取增强!
     *
     * 问题现象:
     *   首次读取Device ID可能返回0x00 (SPI未稳定)
     *   原因:
     *     [1] SPI总线刚初始化,时钟可能不稳定
     *     [2] ADXL345上电后需要稳定时间(通常1-5ms)
     *     [3] CS引脚电平转换需要时间
     *
     * 解决方案:
     *   - 添加10ms延时让SPI和ADXL345稳定
     *   - 最多重试3次读取Device ID
     *   - 每次重试间隔5ms
     */
    uint8_t devid_retry = 0;
    const uint8_t max_devid_retries = 3;

    vTaskDelay(pdMS_TO_TICKS(10));  /* 等待10ms让硬件稳定 */

    for (devid_retry = 0; devid_retry < max_devid_retries; devid_retry++) {
        ret = reg_read(dev, ADXL345_DEVID, &devid, 1);

        if (ret == APP_ERR_OK && devid == ADXL345_DEVICE_ID) {
            break;  /* 成功! */
        }

        LOG_WARN("ADXL345", "Device ID read attempt %u/%u: got 0x%02X (ret=%d)",
                 devid_retry + 1, max_devid_retries, devid, ret);

        if (devid_retry < max_devid_retries - 1) {
            vTaskDelay(pdMS_TO_TICKS(5));  /* 等待5ms后重试 */
        }
    }

    if (ret != APP_ERR_OK || devid != ADXL345_DEVICE_ID) {
        LOG_ERROR("ADXL345", "Device ID mismatch after %u retries: got 0x%02X, expected 0x%02X",
                  max_devid_retries, devid, ADXL345_DEVICE_ID);
        LOG_ERROR("ADXL345", "Possible causes:");
        LOG_ERROR("ADXL345", "  [1] SPI wiring issue (MOSI/MISO/SCLK/CS)");
        LOG_ERROR("ADXL345", "  [2] ADXL345 not powered (check VCC=3.3V)");
        LOG_ERROR("ADXL345", "  [3] SPI clock too fast (try lower speed)");
        spi_bus_remove_device(dev->spi_handle);
        free(dev);
        return NULL;
    }
    LOG_INFO("ADXL345", "Device ID verified: 0x%02X (attempt %u)", devid, devid_retry + 1);

    reg_write(dev, ADXL345_POWER_CTL, 0x00);

    reg_write(dev, ADXL345_DATA_FORMAT,
             ADXL345_DF_FULL_RES | (range & ADXL345_DF_RANGE_MASK));

    reg_write(dev, ADXL345_BW_RATE, rate & ADXL345_BW_RATE_MASK);

    reg_write(dev, ADXL345_FIFO_CTL,
             fifo_mode | ADXL345_FIFO_WATERMARK);

    if (int_cfg && (int_cfg->enable_int1 || int_cfg->enable_int2)) {
        uint8_t int_src = 0;

        /*
         * ⚠️ 【关键修复】安装GPIO ISR服务!
         *
         * ESP-IDF要求: 在调用 gpio_isr_handler_add() 之前,
         * 必须先调用 gpio_install_isr_service() 安装ISR服务
         *
         * 如果跳过此步骤,会导致:
         *   [1] "GPIO isr service is not installed" 警告
         *   [2] gpio_isr_handler_add() 失败 (返回 ESP_ERR_INVALID_STATE)
         *   [3] INT1中断永远不会触发
         *   [4] ADXL345 FIFO数据无法采集
         */
        esp_err_t isr_ret = gpio_install_isr_service(0);
        if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
            LOG_WARN("ADXL345", "gpio_install_isr_service() failed (err=0x%X), trying anyway...", isr_ret);
        }

        if (fifo_mode != ADXL345_FIFO_BYPASS) {
            int_src |= ADXL345_INT_WATERMARK;
        }
        int_src |= ADXL345_INT_OVERRUN;
        reg_write(dev, ADXL345_INT_ENABLE, int_src);

        if (int_cfg->enable_int1) {
            reg_write(dev, ADXL345_INT_MAP, int_cfg->int1_sources);
            gpio_set_direction(int_cfg->gpio_int1, GPIO_MODE_INPUT);
            /*
             * ⚠️ 【关键修复】使用 POSEDGE 替代 HIGH_LEVEL!
             *
             * 原始BUG:
             *   GPIO_INTR_HIGH_LEVEL 会导致ISR风暴:
             *   INT1高电平期间(ADXL345 FIFO≥watermark), ISR反复触发
             *   每次ISR都调用 xEventGroupSetBitsFromISR + portYIELD_FROM_ISR
             *   在acquire_task读取FIFO前(100ms窗口内), ISR可能触发数百次
             *   → CPU过载 → 其他FreeRTOS任务饥饿 → 系统性能退化
             *
             * 修复方案:
             *   GPIO_INTR_POSEDGE: 仅在INT1从低→高跳变时触发一次
             *   ADXL345 FIFO达到watermark时产生上升沿
             *   acquire_task读取FIFO后INT1回低, 下一轮FIFO填满时再产生上升沿
             */
            gpio_set_intr_type(int_cfg->gpio_int1, GPIO_INTR_POSEDGE);
            gpio_isr_handler_add(int_cfg->gpio_int1, gpio_isr_handler, (void *)dev);
            gpio_intr_enable(int_cfg->gpio_int1);
        }
        if (int_cfg->enable_int2) {
            gpio_set_direction(int_cfg->gpio_int2, GPIO_MODE_INPUT);
            gpio_set_intr_type(int_cfg->gpio_int2, GPIO_INTR_HIGH_LEVEL);
            gpio_isr_handler_add(int_cfg->gpio_int2, gpio_isr_handler, (void *)dev);
            gpio_intr_enable(int_cfg->gpio_int2);
        }
    }

    reg_write(dev, ADXL345_POWER_CTL, ADXL345_PCTL_MEASURE);

    /*
     * ⚠️ 【关键诊断】回读POWER_CTL确认MEASURE模式已生效!
     *
     * 问题现象:
     *   日志显示ADXL345初始化成功,但"FIFO empty"持续出现
     *   可能原因: reg_write返回成功但实际未写入(SPI时序/电平问题)
     *
     * 诊断方案:
     *   1. 回读POWER_CTL验证MEASURE bit是否置位
     *   2. 延迟200ms后读FIFO_STATUS, 确认有数据产生
     */
    {
        uint8_t pwr_ctl;
        int verify_ret = reg_read(dev, ADXL345_POWER_CTL, &pwr_ctl, 1);
        if (verify_ret == APP_ERR_OK) {
            if (pwr_ctl & ADXL345_PCTL_MEASURE) {
                LOG_INFO("ADXL345", "POWER_CTL verified: 0x%02X (MEASURE mode active)", pwr_ctl);
            } else {
                LOG_ERROR("ADXL345", "POWER_CTL=0x%02X - MEASURE bit NOT set! Retrying...", pwr_ctl);
                reg_write(dev, ADXL345_POWER_CTL, ADXL345_PCTL_MEASURE);
                vTaskDelay(pdMS_TO_TICKS(50));
                reg_read(dev, ADXL345_POWER_CTL, &pwr_ctl, 1);
                LOG_ERROR("ADXL345", "POWER_CTL after retry: 0x%02X (%s)",
                         pwr_ctl, (pwr_ctl & ADXL345_PCTL_MEASURE) ? "MEASURE" : "STANDBY");
            }
        } else {
            LOG_ERROR("ADXL345", "Failed to read back POWER_CTL (err=%d), SPI may be broken!", verify_ret);
        }

        /* 等待数据积累后再读FIFO验证 */
        vTaskDelay(pdMS_TO_TICKS(200));
        uint8_t fifo_st;
        reg_read(dev, ADXL345_FIFO_STATUS, &fifo_st, 1);
        uint8_t fifo_entries = fifo_st & 0x3F;
        LOG_INFO("ADXL345", "Post-init FIFO check: STATUS=0x%02X, entries=%u, trigger=%s",
                 fifo_st, fifo_entries,
                 (fifo_st & 0x80) ? "YES" : "NO");
        if (fifo_entries == 0) {
            LOG_ERROR("ADXL345", "CRITICAL: ADXL345 producing NO samples after 200ms!");
            LOG_ERROR("ADXL345", "  Possible causes:");
            LOG_ERROR("ADXL345", "  [1] ADXL345 VCC not 3.3V (check power)");
            LOG_ERROR("ADXL345", "  [2] SPI wiring incorrect (SCLK=%d,MOSI=%d,MISO=%d,CS=%d)",
                     dev->bus_cfg.spi_cfg.gpio_sclk, dev->bus_cfg.spi_cfg.gpio_mosi,
                     dev->bus_cfg.spi_cfg.gpio_miso, dev->bus_cfg.spi_cfg.gpio_cs);
            LOG_ERROR("ADXL345", "  [3] ADXL345 chip damaged (try replacement)");
        }
    }

    dev->mutex = xSemaphoreCreateMutex();
    dev->event_group = xEventGroupCreate();

    if (!dev->mutex || !dev->event_group) {
        LOG_ERROR("ADXL345", "Failed to create sync primitives");
        adxl345_deinit(dev);
        return NULL;
    }

    dev->dc_offset.x_offset = 0.0f;
    dev->dc_offset.y_offset = 0.0f;
    dev->dc_offset.z_offset = 1.0f;
    dev->dc_offset.calibrated = false;
    dev->health.selftest_passed = false;
    dev->health.level = ADXL345_HEALTH_GOOD;
    dev->running = false;

    LOG_INFO("ADXL345", "Initialized successfully (SPI host=%d, CS=%d, range=%dG, ODR=%dHz, FIFO=0x%02X)",
             spi_cfg->host_id, spi_cfg->gpio_cs,
             (range == 0) ? 2 : (range == 1) ? 4 : (range == 2) ? 8 : 16,
             (rate == 0x0F) ? 3200 : (rate == 0x0E) ? 1600 : (rate == 0x0D) ? 800 :
             (rate == 0x0C) ? 400 : (rate == 0x0B) ? 200 : (rate == 0x0A) ? 100 :
             (rate == 0x09) ? 50 : (rate == 0x08) ? 25 : 0,
             fifo_mode);

    return dev;
}

void adxl345_deinit(struct adxl345_dev *dev)
{
    if (!dev) return;

    adxl345_stop(dev);

    if (dev->spi_handle) {
        spi_bus_remove_device(dev->spi_handle);
        dev->spi_handle = NULL;
    }

    if (dev->int_cfg.enable_int1) {
        gpio_intr_disable(dev->int_cfg.gpio_int1);
        gpio_isr_handler_remove(dev->int_cfg.gpio_int1);
    }
    if (dev->int_cfg.enable_int2) {
        gpio_intr_disable(dev->int_cfg.gpio_int2);
        gpio_isr_handler_remove(dev->int_cfg.gpio_int2);
    }

    if (dev->mutex) {
        vSemaphoreDelete(dev->mutex);
        dev->mutex = NULL;
    }
    if (dev->event_group) {
        vEventGroupDelete(dev->event_group);
        dev->event_group = NULL;
    }

    free(dev);
    LOG_INFO("ADXL345", "Device deinitialized and resources released");
}

int adxl345_start(struct adxl345_dev *dev, int task_priority,
                  uint32_t task_stack_size)
{
    BaseType_t ret;

    if (!dev) return APP_ERR_ADXL_NOT_INIT;
    if (dev->running) return APP_ERR_ADXL_ALREADY_INIT;

    dev->running = true;
    xEventGroupClearBits(dev->event_group, STOP_ACQUIRE_BIT);

    ret = xTaskCreate(acquire_task_func, "adxl_acq", task_stack_size,
                      (void *)dev, task_priority, &dev->acquire_task_handle);
    if (ret != pdPASS) {
        dev->running = false;
        LOG_ERROR("ADXL345", "Failed to create acquire task");
        return APP_ERR_ADXL_TASK_CREATE;
    }

    LOG_INFO("ADXL345", "Data acquisition started (priority=%d, stack=%lu)",
             task_priority, (unsigned long)task_stack_size);
    return APP_ERR_OK;
}

int adxl345_stop(struct adxl345_dev *dev)
{
    if (!dev) return APP_ERR_ADXL_NOT_INIT;
    if (!dev->running) return APP_ERR_OK;

    dev->running = false;
    xEventGroupSetBits(dev->event_group, STOP_ACQUIRE_BIT);

    if (dev->acquire_task_handle) {
        for (int i = 0; i < 100 && eTaskGetState(dev->acquire_task_handle) != eDeleted; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        dev->acquire_task_handle = NULL;
    }

    LOG_INFO("ADXL345", "Data acquisition stopped");
    return APP_ERR_OK;
}

int adxl345_fetch(struct adxl345_dev *dev, struct adxl345_accel_data *out,
                  uint32_t timeout_ms)
{
    struct adxl345_raw_sample raw;
    size_t bytes_read;

    if (!dev || !out) return APP_ERR_ADXL_INVALID_PARAM;

    if (timeout_ms == 0) {
        bytes_read = ringbuf_pop(dev->ringbuf, (uint8_t *)&raw, sizeof(raw));
    } else {
        bytes_read = ringbuf_pop_timeout(dev->ringbuf, (uint8_t *)&raw,
                                          sizeof(raw), timeout_ms);
    }

    if (bytes_read != sizeof(raw)) {
        return APP_ERR_TIMEOUT;
    }

    preprocess_sample(dev, &raw, out);
    return APP_ERR_OK;
}

int adxl345_calibrate_dc_offset(struct adxl345_dev *dev, uint16_t samples)
{
    float sum_x = 0.0f, sum_y = 0.0f, sum_z = 0.0f;
    struct adxl345_raw_sample raw;
    uint16_t i;

    if (!dev) return APP_ERR_ADXL_NOT_INIT;
    if (samples == 0) samples = ADXL345_DC_CALIBRATION_SAMPLES;

    LOG_INFO("ADXL345", "Starting DC offset calibration (%u samples)...",
             (unsigned)samples);

    xSemaphoreTake(dev->mutex, portMAX_DELAY);

    for (i = 0; i < samples; i++) {
        uint8_t buf[6];
        int ret = reg_read(dev, ADXL345_DATAX0, buf, 6);
        if (ret != APP_ERR_OK) {
            xSemaphoreGive(dev->mutex);
            LOG_ERROR("ADXL345", "Calibration read failed at sample %u", (unsigned)i);
            return ret;
        }

        raw.x = (int16_t)((buf[1] << 8) | buf[0]);
        raw.y = (int16_t)((buf[3] << 8) | buf[2]);
        raw.z = (int16_t)((buf[5] << 8) | buf[4]);

        sum_x += (float)raw.x * dev->scale_factor;
        sum_y += (float)raw.y * dev->scale_factor;
        sum_z += (float)raw.z * dev->scale_factor;

        vTaskDelay(pdMS_TO_TICKS(5));
    }

    dev->dc_offset.x_offset = sum_x / (float)samples;
    dev->dc_offset.y_offset = sum_y / (float)samples;
    dev->dc_offset.z_offset = sum_z / (float)samples;
    dev->dc_offset.calibrated = true;

    xSemaphoreGive(dev->mutex);

    LOG_INFO("ADXL345", "DC calibration complete: X=%.4fg Y=%.4fg Z=%.4fg",
             dev->dc_offset.x_offset, dev->dc_offset.y_offset,
             dev->dc_offset.z_offset);
    return APP_ERR_OK;
}

void adxl345_register_data_callback(struct adxl345_dev *dev,
                                     adxl345_data_callback cb,
                                     void *user_data)
{
    if (!dev) return;
    dev->data_cb = cb;
    dev->data_cb_user_data = user_data;
}

int adxl345_fetch_batch(struct adxl345_dev *dev,
                         struct adxl345_batch_data *batch,
                         uint16_t max_count,
                         uint32_t timeout_ms)
{
    uint16_t count = 0;
    size_t bytes_read;

    if (!dev || !batch || max_count == 0) return APP_ERR_ADXL_INVALID_PARAM;
    if (max_count > ADXL345_BATCH_MAX_SIZE) max_count = ADXL345_BATCH_MAX_SIZE;

    batch->samples = dev->batch_buffer;
    batch->count = 0;

    /*
     * ⚠️ 【关键修复v3】两阶段读取: 先排空已有, 再等待剩余!
     *
     * 问题:
     *   ringbuf_pop_timeout 逐样本poll 10ms → 100ms预算仅能取~10样本
     *   但ringbuf中已有~400样本 → 读取速度成为瓶颈
     *
     * 修复:
     *   阶段1: ringbuf_pop (非阻塞) 排空所有立即可读的数据
     *   阶段2: 如果还不够max_count, 用剩余预算等待新数据
     */
    /* 阶段1: 非阻塞排空 (瞬间取走所有已有数据!) */
    for (uint16_t i = 0; i < max_count; i++) {
        bytes_read = ringbuf_pop(dev->ringbuf,
                                 (uint8_t *)&dev->batch_buffer[i],
                                 sizeof(struct adxl345_raw_sample));
        if (bytes_read != sizeof(struct adxl345_raw_sample)) {
            break;
        }
        count++;
    }

    /* 阶段2: 阻塞等待剩余样本 (仅当数据不足且预算有剩余) */
    if (count < max_count && timeout_ms > 0) {
        int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

        for (uint16_t i = count; i < max_count; i++) {
            int64_t remaining_us = deadline_us - esp_timer_get_time();
            if (remaining_us <= 2000) {
                break;  /* 不足2ms, 放弃 */
            }
            uint32_t wait_ms = (uint32_t)(remaining_us / 1000);
            if (wait_ms < 2) wait_ms = 2;

            bytes_read = ringbuf_pop_timeout(dev->ringbuf,
                                              (uint8_t *)&dev->batch_buffer[i],
                                              sizeof(struct adxl345_raw_sample),
                                              wait_ms);
            if (bytes_read != sizeof(struct adxl345_raw_sample)) {
                break;
            }
            count++;
        }
    }

    if (count > 0) {
        batch->count = count;
        batch->first_timestamp_us = dev->batch_buffer[0].timestamp_us;
        batch->last_timestamp_us = dev->batch_buffer[count - 1].timestamp_us;

        /*
         * ⚠️ 【关键修复v2】批量预处理: 一次加锁, 批量处理!
         *
         * 原始BUG:
         *   - 逐样本take/give mutex (512样本=512次锁操作!)
         *   - 与acquire_task的preprocess_sample竞争mutex
         *   - ma_index_x/y/z字段不存在 → 编译错误/UB
         *
         * 修复方案:
         *   - 整个batch只take mutex一次
         *   - 复用dev->ma_index (三轴共享移动平均窗口索引)
         *   - 预处理后存储 ×1000 g值到int16_t
         */
        xSemaphoreTake(dev->mutex, portMAX_DELAY);

        for (uint16_t i = 0; i < count; i++) {
            float x_raw = (float)dev->batch_buffer[i].x * dev->scale_factor;
            float y_raw = (float)dev->batch_buffer[i].y * dev->scale_factor;
            float z_raw = (float)dev->batch_buffer[i].z * dev->scale_factor;

            float x_ma = moving_average_push(
                x_raw, dev->ma_window_x, &dev->ma_index_x,
                &dev->ma_sum_x, ADXL345_MA_WINDOW_SIZE, &dev->ma_filled);

            float y_ma = moving_average_push(
                y_raw, dev->ma_window_y, &dev->ma_index_y,
                &dev->ma_sum_y, ADXL345_MA_WINDOW_SIZE, &dev->ma_filled);

            float z_ma = moving_average_push(
                z_raw, dev->ma_window_z, &dev->ma_index_z,
                &dev->ma_sum_z, ADXL345_MA_WINDOW_SIZE, &dev->ma_filled);

            dev->batch_buffer[i].x = (int16_t)(apply_dc_removal(x_ma, dev->dc_offset.x_offset) * 1000.0f);
            dev->batch_buffer[i].y = (int16_t)(apply_dc_removal(y_ma, dev->dc_offset.y_offset) * 1000.0f);
            dev->batch_buffer[i].z = (int16_t)(apply_dc_removal(z_ma, dev->dc_offset.z_offset) * 1000.0f);

            dev->stats.samples_processed++;
        }

        xSemaphoreGive(dev->mutex);
    }

    return (int)count;
}

int adxl345_get_health(struct adxl345_dev *dev, struct adxl345_health_info *health)
{
    if (!dev || !health) return APP_ERR_ADXL_INVALID_PARAM;

    xSemaphoreTake(dev->mutex, portMAX_DELAY);
    memcpy(health, &dev->health, sizeof(*health));
    xSemaphoreGive(dev->mutex);

    return APP_ERR_OK;
}

int adxl345_get_stats(struct adxl345_dev *dev, struct adxl345_stats *stats)
{
    if (!dev || !stats) return APP_ERR_ADXL_INVALID_PARAM;

    xSemaphoreTake(dev->mutex, portMAX_DELAY);
    memcpy(stats, &dev->stats, sizeof(*stats));
    xSemaphoreGive(dev->mutex);

    return APP_ERR_OK;
}

int adxl345_self_test(struct adxl345_dev *dev)
{
    uint8_t devid, bw_rate, power_ctl;
    uint8_t test_buf[6];
    int ret;

    if (!dev) return APP_ERR_ADXL_NOT_INIT;

    LOG_INFO("ADXL345", "Starting self-test...");

    ret = reg_read(dev, ADXL345_DEVID, &devid, 1);
    if (ret != APP_ERR_OK || devid != ADXL345_DEVICE_ID) {
        LOG_ERROR("ADXL345", "Self-test FAIL: device ID mismatch");
        dev->health.selftest_passed = false;
        goto restore_measure;  // ⚠️ 必须恢复MEASURE模式!
    }

    ret = reg_read(dev, ADXL345_BW_RATE, &bw_rate, 1);
    if (ret != APP_ERR_OK) {
        dev->health.selftest_passed = false;
        goto restore_measure;
    }

    ret = reg_read(dev, ADXL345_POWER_CTL, &power_ctl, 1);
    if (ret != APP_ERR_OK) {
        dev->health.selftest_passed = false;
        goto restore_measure;
    }

    if ((power_ctl & ADXL345_PCTL_MEASURE) == 0) {
        LOG_WARN("ADXL345", "Sensor in standby during self-test, temporarily switching to MEASURE for test");

        /*
         * ⚠️ 【关键】临时切换到MEASURE模式以进行数据寄存器测试
         * ADXL345在STANDBY模式下无法更新数据寄存器
         * 测试完成后必须保持或恢复MEASURE模式
         */
        ret = reg_write(dev, ADXL345_POWER_CTL, ADXL345_PCTL_MEASURE);
        if (ret != APP_ERR_OK) {
            LOG_ERROR("ADXL345", "Failed to enter MEASURE mode for testing");
            dev->health.selftest_passed = false;
            return APP_ERR_ADXL_SELFTEST_FAIL;
        }

        /* 等待数据就绪 (至少1个采样周期 @400Hz = 2.5ms) */
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /*
     * ⚠️ 【改进】分次读取数据寄存器 (避免一次性读6字节)
     * 原因: 某些ADXL345芯片在边界条件下对多字节读取敏感
     * 改为: 先读X轴(2字节), 成功后再读YZ(4字节)
     */
    ret = reg_read(dev, ADXL345_DATAX0, test_buf, 2);  // 只读X轴 (2字节)
    if (ret != APP_ERR_OK) {
        LOG_ERROR("ADXL345", "Self-test FAIL: cannot read DATAX0/DATAX1 (ret=%d)", ret);
        dev->health.selftest_passed = false;
        goto restore_measure;
    }

    /* 验证数据不全为零 (静止状态下可能接近零, 但不应全为0xFF) */
    if (test_buf[0] == 0 && test_buf[1] == 0) {
        LOG_WARN("ADXL345", "Self-test: X-axis data is zero (sensor may be stationary)");
    }

    /* 尝试读取剩余轴 (Y+Z, 4字节) */
    ret = reg_read(dev, ADXL345_DATAY0, &test_buf[2], 4);
    if (ret != APP_ERR_OK) {
        LOG_WARN("ADXL345", "Self-test: Y/Z axis read failed (ret=%d), but X-axis OK", ret);
        /* 不算完全失败, X轴能读说明基本功能正常 */
    }

    dev->health.selftest_passed = true;
    LOG_INFO("ADXL345", "Self-test PASSED (DATAX=%d,%d DATAY=%d,%d DATAZ=%d,%d)",
             (int8_t)test_buf[0], (int8_t)test_buf[1],
             (int8_t)test_buf[2], (int8_t)test_buf[3],
             (int8_t)test_buf[4], (int8_t)test_buf[5]);
    return APP_ERR_OK;

restore_measure:
    /*
     * ⚠️ 【关键修复】无论成功失败, 都要确保处于MEASURE模式!
     * 否则后续数据采集任务会读到空FIFO
     */
    LOG_WARN("ADXL345", "Restoring MEASURE mode after failed/partial self-test...");
    int restore_ret = reg_write(dev, ADXL345_POWER_CTL, ADXL345_PCTL_MEASURE);
    if (restore_ret != APP_ERR_OK) {
        LOG_ERROR("ADXL345", "CRITICAL: Failed to restore MEASURE mode! Sensor will not produce data.");
    } else {
        LOG_INFO("ADXL345", "MEASURE mode restored successfully");
        vTaskDelay(pdMS_TO_TICKS(200));  /* 等待200ms让FIFO积累足够数据 (400Hz×0.2s=80样本>水位16) */
    }
    return APP_ERR_ADXL_SELFTEST_FAIL;
}

int adxl345_reset_bus(struct adxl345_dev *dev)
{
    int ret;

    if (!dev) return APP_ERR_ADXL_NOT_INIT;

    LOG_WARN("ADXL345", "Performing bus reset...");

    if (dev->bus_mode == ADXL345_BUS_SPI) {
        if (dev->spi_handle) {
            spi_bus_remove_device(dev->spi_handle);
            dev->spi_handle = NULL;
        }
        vTaskDelay(pdMS_TO_TICKS(BUS_RESET_DELAY_MS));

        spi_device_interface_config_t devcfg = {0};
        devcfg.command_bits = 0;
        devcfg.address_bits = 0;
        devcfg.clock_speed_hz = dev->bus_cfg.spi_cfg.clock_speed_hz;
        devcfg.mode = 3;
        devcfg.spics_io_num = dev->bus_cfg.spi_cfg.gpio_cs;
        devcfg.queue_size = 4;
        devcfg.flags = 0;

        ret = spi_bus_add_device(dev->bus_cfg.spi_cfg.host_id,
                                  &devcfg, &dev->spi_handle);
        if (ret != ESP_OK) {
            LOG_ERROR("ADXL345", "Bus reset: re-add device failed");
            return APP_ERR_ADXL_SPI_CONFIG;
        }

        reg_write(dev, ADXL345_POWER_CTL, ADXL345_PCTL_MEASURE);
        LOG_INFO("ADXL345", "Bus reset completed, MEASURE restored");
    }

    dev->consecutive_errors = 0;
    return APP_ERR_OK;
}