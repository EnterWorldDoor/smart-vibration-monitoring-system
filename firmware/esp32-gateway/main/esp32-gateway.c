/**
 * @file esp32-gateway.c
 * @author EnterWorldDoor
 * @brief EdgeVib ESP32-S3 企业级网关主程序
 *
 * 硬件平台: ESP32-S3-DevKitC-1
 * 功能概述:
 *   - UART 接收 STM32 温湿度数据 (GPIO17/18, 115200bps)
 *   - SPI 读取 ADXL345 振动数据 (软件SPI)
 *   - 数据融合处理 (温度补偿 + RMS/FFT 分析)
 *   - MQTT 训练模式上传至 PC 端 Broker
 *
 * 架构设计:
 *   ┌─────────────────────────────────────────────────────┐
 *   │              FreeRTOS 任务架构                      │
 *   │                                                     │
 *   │  ┌──────────┐    ┌──────────┐    ┌──────────┐      │
 *   │  │UART RX   │───▶│Sensor    │───▶│MQTT      │      │
 *   │  │Task(P:7) │ Q1 │Task(P:6) │ Q2 │Task(P:5) │      │
 *   │  └──────────┘    └──────────┘    └──────────┘      │
 *   │       │               │               │             │
 *   │       ▼               ▼               ▼             │
 *   │  [Protocol]     [SensorService]   [MQTT Module]     │
 *   │       │               │               │             │
 *   │  STM32 ◄─────────── ADXL345        PC Broker         │
 *   │  (温湿度)          (振动数据)     (训练模式上传)     │
 *   └─────────────────────────────────────────────────────┘
 *
 * 数据流向:
 *   STM32 UART帧 → Protocol解析 → 温湿度数据 → SensorService温度补偿
 *                                              ↓
 *   ADXL345 SPI → 原始加速度 → RMS/FFT分析 → analysis_result
 *                                              ↓
 *                              MQTT Publish → edgevib/train/{id}/vibration
 *
 * 错误处理策略:
 *   - UART 接收超时: 记录日志 + 继续等待 (不阻塞)
 *   - 协议解析错误: 丢弃当前帧 + 统计计数
 *   - 传感器采集失败: 降级运行 + 自动重试
 *   - MQTT 断连: 离线缓存 + 自动重连补发
 *
 * 编码规范: Linux Kernel Style + AGENTS.md 项目规范
 */

/* ==================== 标准库头文件 ==================== */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>           /* PRIx32, PRIu32 等格式化宏 */

/* ==================== ESP-IDF 头文件 ==================== */
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"             /* WiFi 驱动 (STA模式连接热点) */
#include "nvs_flash.h"            /* NVS Flash (非易失性存储) */
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

/* ==================== 项目组件头文件 ==================== */
#include "log_system.h"           /* 日志系统 (统一日志接口) */
#include "global_error.h"         /* 统一错误码定义 */
#include "config_manager.h"       /* 配置管理器 (NVS持久化) */
#include "time_sync.h"            /* 时间同步 (SNTP) */
#include "protocol.h"             /* UART 协议栈 (STM32通信) */
#include "sensor_service.h"       /* 传感器服务 (ADXL345+DSP) */
#include "mqtt.h"                  /* MQTT 通信模块 (mqtt_app组件) */

/* 引入硬件引脚配置 (集中管理所有GPIO定义) */
#include "hardware_config.h"

/* ==================== 硬件驱动头文件 ==================== */
#include "driver/uart.h"          /* UART 驱动 (UART_NUM_1) */

/* ==================== ESP-IDF 扩展头文件 ==================== */
#include <esp_random.h>           /* 硬件随机数生成器 */
#include <esp_timer.h>            /* 高精度定时器 */

/* ==================== 硬件引脚宏定义 ==================== */

/**
 * UART1 配置 (STM32 通信)
 *
 * ⚠️ 【关键修复】UART TX/RX引脚修正!
 *
 * 物理连线 (基于图一+图二原理图):
 *   STM32侧:                    ESP32-S3 DevKitC-1侧:
 *   PC10 = UART4_TX (发数据)  ──→  GPIO18 = U1RXD (收数据)
 *   PC11 = UART4_RX (收数据)  ←──  GPIO17 = U1TXD (发数据)
 *   GND ↔ GND (必须共地!)
 *
 * 引脚功能说明:
 *   - GPIO17 = U1TXD: UART1 发送引脚 (TX, 数据输出到外部)
 *   - GPIO18 = U1RXD: UART1 接收引脚 (RX, 从外部输入数据)
 *
 * 交叉连接原则: A的TX必须接B的RX!
 */
#define UART_STM32_NUM            UART_NUM_1       /**< UART 端口号 */
#define UART_STM32_TX_PIN         17               /**< TX=GPIO17(U1TXD), 发送→STM32(PC11/RX) */
#define UART_STM32_RX_PIN         18               /**< RX=GPIO18(U1RXD), 接收←STM32(PC10/TX) */
#define UART_STM32_BAUD_RATE      115200           /**< 波特率 (bps) */
#define UART_STM32_BUF_SIZE       1024             /**< 缓冲区大小 (字节) */

/*
 * ADXL345 SPI引脚配置已移至 hardware_config.h
 * 此处保留注释作为参考, 实际使用时请包含 hardware_config.h
 *
 * 连接关系:
 *   SCL  (时钟)  → GPIO36 (ADXL345_SPI_SCLK_PIN)
 *   SDA  (MOSI)  → GPIO35 (ADXL345_SPI_MOSI_PIN)
 *   SDO  (MISO)  → GPIO37 (ADXL345_SPI_MISO_PIN)
 *   CS   (片选)  → GPIO40 (ADXL345_SPI_CS_PIN)
 *   INT1 (中断) → GPIO47 (ADXL345_INT1_PIN)
 */

/**
 * LED 指示灯 (可选)
 */
#define GPIO_LED_STATUS           2                /**< 状态指示灯 (内置LED) */
#define GPIO_LED_ERROR            4                /**< 错误指示灯 */

/**
 * WiFi STA 配置 (连接手机热点)
 *
 * ⚠️ 【重要】使用前必须修改以下配置！
 *
 * 修改位置1: WiFi_SSID - 改成你的手机热点名称
 *   例: #define WIFI_SSID "iPhone的热点"
 *
 * 修改位置2: WIFI_PASSWORD - 改成你的热点密码
 *   例: #define WIFI_PASSWORD "12345678"
 *
 * 修改位置3: MQTT_BROKER_URL_DEFAULT - 在 component_init_mqtt() 中
 *   改成你PC在热点下的IP地址
 *   例: "mqtt://192.168.43.156:1883"
 *
 * 获取PC的IP地址方法:
 *   Windows: 运行 cmd → 输入 ipconfig → 找到 "无线局域网适配器 WLAN" 的 IPv4 地址
 *   格式通常是: 192.168.43.xxx (xxx是动态分配的)
 */
#define WIFI_SSID                 "EdgeVib_Hotspot"    /**< ⚠️ 修改为你的手机热点名称! */
#define WIFI_PASSWORD             "1234567890"          /**< ⚠️ 修改为你的热点密码! */
#define WIFI_MAX_RETRY            10                  /**< WiFi 最大重连次数 */
#define WIFI_CONNECT_TIMEOUT_MS   20000              /**< WiFi 连接超时 (20秒) */

/* ==================== FreeRTOS 任务配置 ==================== */

/**
 * 任务优先级分配 (数字越大优先级越高)
 *
 * 设计原则:
 *   - UART接收 > 传感器处理 > MQTT上传
 *   - 保证实时数据不丢失
 *   - 避免低优先级任务饥饿
 */
#define TASK_PRIORITY_UART_RX     7                /**< UART 接收任务优先级 (最高) */
#define TASK_PRIORITY_SENSOR      6                /**< 传感器处理任务优先级 (中等) */
#define TASK_PRIORITY_MQTT        5                /**< MQTT 上传任务优先级 (较低) */
#define TASK_PRIORITY_MONITOR     2                /**< 系统监控任务优先级 (最低) */

/**
 * 任务栈大小 (字节)
 *
 * 设计原则:
 *   - UART任务: 中等栈 (协议解析需要一定栈空间)
 *   - Sensor任务: 大栈 (FFT/DSP计算需要较多栈)
 *   - MQTT任务: 中等栈 (JSON序列化)
 *   - Monitor任务: 小栈 (仅统计查询)
 */
#define TASK_STACK_UART_RX        4096             /**< UART 接收任务栈大小 */
#define TASK_STACK_SENSOR         4096             /**< 传感器处理任务栈 (仅Q1队列+状态查询) */
#define TASK_STACK_MQTT           8192             /**< MQTT 上传任务栈大小 (增大以防止json_buffer+processed_result栈溢出) */
#define TASK_STACK_MONITOR        4096             /**< 监控任务栈大小 (增大以容纳详细日志打印) */

/* ==================== FreeRTOS 队列配置 ==================== */

/**
 * 队列深度设计
 *
 * Q1 (UART→Sensor): 存储温湿度数据包
 *   - 深度: 10 (约10秒数据@1Hz采样率)
 *   - 过载策略: 丢弃最旧数据
 *
 * Q2 (Sensor→MQTT): 存储分析结果
 *   - 深度: 8 (约8个分析周期结果)
 *   - 过载策略: 丢弃最旧数据
 */
#define QUEUE_DEPTH_TEMP_DATA     10               /**< 温湿度队列深度 */
#define QUEUE_DEPTH_ANALYSIS      4                /**< 分析结果队列深度 */
#define QUEUE_TIMEOUT_MS          100              /**< 队列读取超时 (ms) */
#define QUEUE_WAIT_FOREVER        portMAX_DELAY    /**< 永久等待 */

/* ==================== 全局状态与句柄 ==================== */

/**
 * struct gateway_context - 网关全局上下文
 *
 * 集中管理所有全局资源:
 *   - FreeRTOS 队列句柄
 *   - 任务句柄
 *   - 运行状态标志
 *   - 统计计数器
 */
struct gateway_context {
    /* 队列句柄 */
    QueueHandle_t q_temp_data;                    /**< 温湿度数据队列 (UART→Sensor) */
    QueueHandle_t q_analysis_result;               /**< 分析结果队列 (Sensor→MQTT) */

    /* 任务句柄 */
    TaskHandle_t task_uart_rx;                     /**< UART 接收任务句柄 */
    TaskHandle_t task_sensor;                      /**< 传感器处理任务句柄 */
    TaskHandle_t task_mqtt;                        /**< MQTT 上传任务句柄 */
    TaskHandle_t task_monitor;                     /**< 系统监控任务句柄 */

    /* 运行状态 */
    volatile bool system_running;                  /**< 系统运行标志 */
    volatile bool uart_connected;                  /**< UART 连接状态 (心跳检测) */
    uint32_t error_count;                          /**< 累计错误次数 */
    uint64_t uptime_start_us;                      /**< 启动时间戳 */

    /* 统计信息 */
    struct {
        uint32_t uart_frames_received;              /**< UART 收到帧数 */
        uint32_t temp_data_processed;               /**< 处理的温湿度数据数 */
        uint32_t analysis_published;               /**< 发布的分析结果数 */
        uint32_t mqtt_publish_success;              /**< MQTT 发布成功次数 */
        uint32_t mqtt_publish_failed;               /**< MQTT 发布失败次数 */
        uint32_t sensor_errors;                     /**< 传感器错误次数 */
    } stats;
};

static struct gateway_context g_ctx = {0};          /**< 全局网关上下文实例 */

/* ==================== 函数前向声明 ==================== */

/* 硬件初始化 */
static int hardware_init_gpio(void);
static int hardware_init_uart(void);

/* WiFi 初始化 */
static int wifi_init_sta(void);                     /**< WiFi STA模式初始化 (连接热点) */
static void wifi_event_handler(void *arg,             /**< WiFi 事件回调 */
                                esp_event_base_t event_base,
                                int32_t event_id,
                                void *event_data);
static void ip_event_handler(void *arg,               /**< IP事件回调 (获取IP地址) */
                              esp_event_base_t event_base,
                              int32_t event_id,
                              void *event_data);

/* 组件初始化 */
static int component_init_log(void);
static int component_init_time_sync(void);
static int component_init_protocol(void);
static int component_init_sensor(void);
static int component_init_mqtt(void);

/* FreeRTOS 任务 */
static void task_uart_rx(void *arg);
static void task_sensor_process(void *arg);
static void task_mqtt_upload(void *arg);
static void task_system_monitor(void *arg);

/* 回调函数 */
static void on_temp_data_received(const struct temp_humidity_data *data,
                                  void *user_data);
static void on_analysis_ready(const struct analysis_result *result,
                               void *user_data);
static void on_sensor_error(int error_code,
                             const char *context,
                             void *user_data);
static void on_protocol_error(int error_code,
                              const char *context);
static void on_mqtt_event(int event_type,
                           const char *context,
                           void *user_data);

/* ==================== 初始化函数实现 ==================== */

/**
 * hardware_init_gpio - 初始化 GPIO 引脚
 *
 * 功能:
 *   - 配置 ADXL345 片选和中断引脚为输出/输入模式
 *   - 配置 LED 指示灯引脚
 *   - 设置默认电平
 *
 * Return: APP_ERR_OK 成功, 负数错误码
 */
static int hardware_init_gpio(void)
{
    gpio_config_t io_conf = {0};

    /*
     * ⚠️ 【关键修复】不在此处配置 CS 和 INT1 引脚!
     *
     * 原始BUG:
     *   hardware_init_gpio 将 GPIO40(CS) 配置为 GPIO_MODE_OUTPUT
     *   后续 sensor_service_init → adxl345_init_spi → spi_bus_add_device
     *   尝试将 GPIO40 作为硬件 CS 管理 → 双重控制导致 CS 时序异常
     *   → SPI 通信初几次成功后进入错误状态 → "FIFO empty"
     *
     * 修复方案:
     *   CS: 完全由 SPI 驱动管理 (spics_io_num)
     *   INT1: 完全由 adxl345_init_spi 内部配置 (gpio_set_direction + ISR注册)
     *   此处仅保留 LED 引脚配置
     */

    /* 配置状态 LED (输出, 默认关闭) */
#ifdef GPIO_LED_STATUS
    memset(&io_conf, 0, sizeof(io_conf));
    io_conf.pin_bit_mask = (1ULL << GPIO_LED_STATUS);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);
    gpio_set_level(GPIO_LED_STATUS, 0);
#endif

    LOG_INFO("HW-GPIO", "GPIO initialized (CS=%d, INT1=%d)",
             ADXL345_SPI_CS_PIN, ADXL345_INT1_PIN);

    return APP_ERR_OK;
}

/**
 * hardware_init_uart - 初始化 UART1 (STM32 通信接口)
 *
 * 功能:
 *   - 配置 UART1 为 115200, 8N1
 *   - 设置 TX/RX 引脚映射
 *   - 分配接收缓冲区
 *
 * 注意: 实际 UART 初始化在 protocol_init() 中完成,
 *       此函数仅做硬件层面的预检查
 *
 * Return: APP_ERR_OK 成功, 负数错误码
 */
static int hardware_init_uart(void)
{
    LOG_INFO("HW-UART", "UART%d configured: TX=%d, RX=%d, baud=%d",
             UART_STM32_NUM, UART_STM32_TX_PIN, UART_STM32_RX_PIN,
             UART_STM32_BAUD_RATE);

    return APP_ERR_OK;
}

/* ==================== WiFi 初始化函数实现 ==================== */

/**
 * wifi_event_handler - WiFi 事件处理回调
 *
 * 处理的WiFi事件:
 *   - WIFI_EVENT_STA_START: WiFi 开始连接
 *   - WIFI_EVENT_STA_CONNECTED: 已连接到AP (热点)
 *   - WIFI_EVENT_STA_DISCONNECTED: 断开连接 (触发重连)
 *   - WIFI_EVENT_STA_AUTHMODE_CHANGE: 认证模式变更
 *
 * @arg: 用户参数 (未使用)
 * @event_base: 事件基础类型 (WIFI_EVENT)
 * @event_id: 具体事件ID
 * @event_data: 事件数据 (wifi_event_t*)
 */
static void wifi_event_handler(void *arg,
                                esp_event_base_t event_base,
                                int32_t event_id,
                                void *event_data)
{
    (void)arg;

    switch (event_id) {
        case WIFI_EVENT_STA_START:
            LOG_INFO("WIFI", "WiFi started, connecting to AP...");
            esp_wifi_connect();  /* 触发连接 */
            break;

        case WIFI_EVENT_STA_CONNECTED:
            LOG_INFO("WIFI", "✓ Connected to AP: %s", WIFI_SSID);
#ifdef GPIO_LED_STATUS
            gpio_set_level(GPIO_LED_STATUS, 1);  /* 点亮状态灯 */
#endif
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            {
                wifi_event_sta_disconnected_t *disconn =
                    (wifi_event_sta_disconnected_t *)event_data;

                LOG_WARN("WIFI", "✗ Disconnected from AP (reason=%d)", disconn->reason);

#ifdef GPIO_LED_STATUS
                gpio_set_level(GPIO_LED_STATUS, 0);  /* 关闭状态灯 */
#endif

                /*
                 * 自动重连机制:
                 * ESP-IDF 默认会自动重连，但我们可以在这里添加额外逻辑
                 * 如: 重连次数限制、指数退避等
                 */
                LOG_INFO("WIFI", "Attempting to reconnect...");
                esp_wifi_connect();
            }
            break;

        case WIFI_EVENT_STA_AUTHMODE_CHANGE:
            LOG_DEBUG("WIFI", "Auth mode changed");
            break;

        default:
            LOG_DEBUG("WIFI", "Unhandled WiFi event: %ld", event_id);
            break;
    }
}

/**
 * ip_event_handler - IP地址获取事件回调
 *
 * 关键事件: IP_EVENT_STA_GOT_IP
 *   - 表示DHCP分配成功，获取到了IP地址
 *   - 此后才能进行MQTT连接
 *
 * @arg: 用户参数 (未使用)
 * @event_base: 事件基础类型 (IP_EVENT)
 * @event_id: 具体事件ID
 * @event_data: 事件数据 (ip_event_got_ip_t*)
 */
static void ip_event_handler(void *arg,
                              esp_event_base_t event_base,
                              int32_t event_id,
                              void *event_data)
{
    (void)arg;

    if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        char ip_str[IP4ADDR_STRLEN_MAX];  /* IPv4 地址字符串缓冲区 */

        LOG_INFO("WIFI", "========================================");
        LOG_INFO("WIFI", "  ✓ Got IP Address!");
        LOG_INFO("WIFI", "  IP: %s", esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, sizeof(ip_str)));
        LOG_INFO("WIFI", "  Netmask: %s", esp_ip4addr_ntoa(&event->ip_info.netmask, ip_str, sizeof(ip_str)));
        LOG_INFO("WIFI", "  Gateway: %s", esp_ip4addr_ntoa(&event->ip_info.gw, ip_str, sizeof(ip_str)));
        LOG_INFO("WIFI", "========================================");

        /*
         * ⚠️ 【重要】记录这个IP地址！这就是你PC需要连接的目标
         * 格式通常是: 192.168.43.xxx
         * 你需要把这个IP填入 MQTT Broker URL 中!
         */
    }
}

/**
 * wifi_init_sta - 初始化 WiFi STA模式并连接到手机热点
 *
 * 功能:
 *   1. 初始化 TCP/IP 协议栈 (LWIP)
 *   2. 创建默认事件循环 (如果尚未创建)
 *   3. 注册 WiFi 和 IP 事件处理器
 *   4. 创建网络接口 (STA 模式)
 *   5. 配置 WiFi STA 参数 (SSID/密码)
 *   6. 启动 WiFi 并等待连接
 *   7. 阻塞等待获取 IP 地址 (最多20秒)
 *
 * 连接流程:
 *   esp_wifi_start() → WIFI_EVENT_STA_START → esp_wifi_connect()
 *     → WIFI_EVENT_STA_CONNECTED → DHCP请求
 *       → IP_EVENT_STA_GOT_IP (成功!)
 *
 * Return: APP_ERR_OK 成功, 负数错误码
 *
 * ⚠️ 使用前必须修改文件开头的宏定义:
 *   - WIFI_SSID: 你的手机热点名称
 *   - WIFI_PASSWORD: 你的热点密码
 */
static int wifi_init_sta(void)
{
    esp_err_t ret;

    LOG_INFO("WIFI", "Initializing WiFi STA mode...");
    LOG_INFO("WIFI", "Target AP: %s", WIFI_SSID);

    /* ========== 第1步: 初始化 TCP/IP 协议栈 ==========
     * LWIP (Lightweight IP) 是ESP32的TCP/IP协议栈实现
     * 提供完整的 IPv4/IPv6 支持
     */
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        LOG_ERROR("WIFI", "Failed to initialize netif (err=0x%x)", ret);
        return APP_ERR_GENERAL;
    }

    /* ========== 第2步: 创建事件循环 ==========
     * ESP-IDF 的事件驱动架构核心
     * 所有系统事件 (WiFi/IP/MQTT) 都通过此循环分发
     */
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        LOG_ERROR("WIFI", "Failed to create event loop (err=0x%x)", ret);
        return APP_ERR_GENERAL;
    }

    /* ========== 第3步: 注册事件处理器 ==========
     * WiFi事件: 连接/断开/认证等
     * IP事件: 获取到IP地址 (最关键!)
     */
    ret = esp_event_handler_instance_register(WIFI_EVENT,
                                               ESP_EVENT_ANY_ID,
                                               &wifi_event_handler,
                                               NULL,
                                               NULL);
    if (ret != ESP_OK) {
        LOG_ERROR("WIFI", "Failed to register WiFi event handler (err=0x%x)", ret);
        return APP_ERR_GENERAL;
    }

    ret = esp_event_handler_instance_register(IP_EVENT,
                                               IP_EVENT_STA_GOT_IP,
                                               &ip_event_handler,
                                               NULL,
                                               NULL);
    if (ret != ESP_OK) {
        LOG_ERROR("WIFI", "Failed to register IP event handler (err=0x%x)", ret);
        return APP_ERR_GENERAL;
    }

    /* ========== 第4步: 创建网络接口 (STA Station模式) ==========
     * STA模式: ESP32作为客户端连接到路由器/AP
     * 对比 AP模式: ESP32作为路由器让其他设备连接
     */
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (!sta_netif) {
        LOG_ERROR("WIFI", "Failed to create STA netif");
        return APP_ERR_GENERAL;
    }

    /* 配置 WiFi 基础参数 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        LOG_ERROR("WIFI", "Failed to init WiFi driver (err=0x%x)", ret);
        return APP_ERR_GENERAL;
    }

    /* ========== 第5步: 配置 STA 连接参数 ==========
     * SSID: 热点名称 (必须修改为你的!)
     * Password: 热点密码 (必须修改为你的!)
     * 模式: WPA2_PSK (目前最安全的家庭网络加密方式)
     */
    wifi_config_t wifi_conf = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,  /* WPA2认证 */
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };

    /* 复制 SSID 和密码 (带长度检查) */
    size_t ssid_len = strlen(WIFI_SSID);
    size_t pass_len = strlen(WIFI_PASSWORD);

    if (ssid_len > sizeof(wifi_conf.sta.ssid) - 1) {
        LOG_ERROR("WIFI", "SSID too long (%u > %u)",
                 (unsigned)ssid_len, sizeof(wifi_conf.sta.ssid) - 1);
        return APP_ERR_INVALID_PARAM;
    }

    if (pass_len > sizeof(wifi_conf.sta.password) - 1) {
        LOG_ERROR("WIFI", "Password too long (%u > %u)",
                 (unsigned)pass_len, sizeof(wifi_conf.sta.password) - 1);
        return APP_ERR_INVALID_PARAM;
    }

    memcpy(wifi_conf.sta.ssid, WIFI_SSID, ssid_len + 1);
    memcpy(wifi_conf.sta.password, WIFI_PASSWORD, pass_len + 1);

    LOG_DEBUG("WIFI", "Configuring STA: SSID=%s, Auth=WPA2_PSK", WIFI_SSID);

    /* 设置 WiFi 配置 */
    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        LOG_ERROR("WIFI", "Failed to set STA mode (err=0x%x)", ret);
        return APP_ERR_GENERAL;
    }

    ret = esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_conf);
    if (ret != ESP_OK) {
        LOG_ERROR("WIFI", "Failed to set WiFi config (err=0x%x)", ret);
        return APP_ERR_GENERAL;
    }

    /* ========== 第6步: 启动 WiFi ==========
     * 这会触发 WIFI_EVENT_STA_START 事件
     * 在事件回调中会调用 esp_wifi_connect()
     */
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        LOG_ERROR("WIFI", "Failed to start WiFi (err=0x%x)", ret);
        return APP_ERR_GENERAL;
    }

    LOG_INFO("WIFI", "WiFi started, waiting for connection...");

    /* ========== 第7步: 等待获取 IP 地址 (阻塞) ==========
     * 最大等待时间: WIFI_CONNECT_TIMEOUT_MS (20秒)
     * 如果超时, 说明无法连接到热点, 可能原因:
     *   [1] SSID 或密码错误
     *   [2] 手机热点关闭或超出范围
     *   [3] 热点已达到最大连接数
     *   [4] 2.4GHz/5GHz频段不匹配 (ESP32只支持2.4G!)
     */
    int64_t start_time = (int64_t)esp_timer_get_time();
    bool got_ip = false;

    while (!got_ip) {
        int64_t elapsed_ms = ((int64_t)esp_timer_get_time() - start_time) / 1000LL;

        if (elapsed_ms >= WIFI_CONNECT_TIMEOUT_MS) {
            LOG_ERROR("WIFI", "⚠ WiFi connect timeout after %lld ms!", elapsed_ms);
            LOG_ERROR("WIFI", "Please check:");
            LOG_ERROR("WIFI", "  1. Is phone hotspot ON?");
            LOG_ERROR("WIFI", "  2. Is SSID '%s' correct?", WIFI_SSID);
            LOG_ERROR("WIFI", "  3. Is password correct?");
            LOG_ERROR("WIFI", "  4. Is it 2.4GHz? (ESP32 doesn't support 5GHz)");
            return APP_ERR_MQTT_CONNECT_FAIL;  /* 复用错误码 */
        }

        /* 检查是否已获取IP */
        esp_netif_ip_info_t ip_info;
        ret = esp_netif_get_ip_info(sta_netif, &ip_info);

        if (ret == ESP_OK && ip_info.ip.addr != 0) {
            got_ip = true;
            LOG_INFO("WIFI", "WiFi connected successfully! (took %lld ms)", elapsed_ms);
            break;
        }

        /* 每500ms打印一次进度 */
        vTaskDelay(pdMS_TO_TICKS(500));
        LOG_DEBUG("WIFI", "Waiting for IP... (%lld ms elapsed)", elapsed_ms);
    }

    return APP_ERR_OK;
}

/**
 * component_init_log - 初始化日志系统
 *
 * 配置项:
 *   - 日志等级: INFO (生产环境推荐)
 *   - 输出目标: UART (控制台)
 *   - 环形缓冲区: 4KB (用于后续远程日志上传)
 *
 * Return: APP_ERR_OK 成功, 负数错误码
 */
static int component_init_log(void)
{
    log_config_t log_cfg = {
        .level = LOG_LEVEL_INFO,
        .outputs = LOG_OUTPUT_UART,
        .ringbuf_size = 4096,
        .async_mode = true,
        .queue_size = 32,
        .batch_size = 8,
        .flush_interval_ms = 500
    };

    int ret = log_system_init_with_config(&log_cfg);
    if (ret != APP_ERR_OK) {
        LOG_ERROR("INIT", "Log system init failed (err=%d)", ret);
        return ret;
    }

    LOG_INFO("INIT", "Log system initialized (level=%d, ringbuf=%d bytes)",
             log_cfg.level, log_cfg.ringbuf_size);

    return APP_ERR_OK;
}

/**
 * component_init_time_sync - 初始化时间同步服务
 *
 * 功能:
 *   - 启动 SNTP 客户端
 *   - 配置时区 (CST-8, 北京时间)
 *   - 等待首次时间同步完成 (超时10秒)
 *
 * 时间同步重要性:
 *   - 所有传感器数据的时间戳依赖于此
 *   - MQTT 消息 timestamp_ms 字段
 *   - 温度补偿的时间对齐
 *
 * Return: APP_ERR_OK 成功, 负数错误码
 */
static int component_init_time_sync(void)
{
    int ret;

    /* 初始化 SNTP 时间同步 */
    ret = time_sync_init(true, "CST-8");
    if (ret != APP_ERR_OK) {
        LOG_ERROR("INIT", "Time sync init failed (err=%d)", ret);
        return ret;
    }

    /* 等待首次 NTP 同步 (阻塞最多10秒) */
    ret = time_sync_wait_sync(10000);
    if (ret != APP_ERR_OK) {
        /*
         * ⚠️ 【优化】NTP超时是常见情况,降级为INFO!
         *
         * 原始问题:
         *   每次启动都打印 WARN: "SNTP sync timeout"
         *   在无互联网环境 (如纯本地网络/训练模式) 下是正常的
         *   不应视为警告,而是信息提示
         *
         * 解决方案:
         *   降级为 LOG_INFO, 明确说明使用本地时间
         */
        LOG_INFO("INIT", "NTP sync not available (err=%d), using local time (acceptable in offline mode)", ret);
        /* 不视为致命错误, 可继续使用本地时间 */
    } else {
        LOG_INFO("INIT", "Time synchronized via SNTP");
    }

    return APP_ERR_OK;
}

/**
 * component_init_protocol - 初始化 UART 协议栈
 *
 * 功能:
 *   - 配置 UART1 参数 (波特率、引脚)
 *   - 启动协议接收任务
 *   - 注册温湿度数据回调
 *
 * Return: APP_ERR_OK 成功, 负数错误码
 */
static int component_init_protocol(void)
{
    int ret;

    /* 初始化协议栈 (内部会配置UART硬件) */
    ret = protocol_init(
        UART_STM32_NUM,           /* UART端口号 */
        UART_STM32_BAUD_RATE,     /* 波特率 */
        UART_STM32_TX_PIN,        /* TX引脚 */
        UART_STM32_RX_PIN         /* RX引脚 */
    );
    if (ret != APP_ERR_OK) {
        LOG_ERROR("INIT", "Protocol init failed (err=%d)", ret);
        return ret;
    }

    /* 注册温湿度数据接收回调 (当收到CMD_TEMP_HUMIDITY_DATA时调用) */
    ret = protocol_register_temp_callback(on_temp_data_received, &g_ctx);
    if (ret != APP_ERR_OK) {
        LOG_WARN("INIT", "Failed to register temp callback (err=%d)", ret);
        /* 不致命, 可通过轮询获取温度数据 */
    }

    /* 注册协议错误回调 (用于监控CRC错误等异常)
     * 注意: proto_error_callback_t 签名为 (error_code, context)，无 user_data
     * 使用包装函数 on_protocol_error 适配签名
     */
    ret = protocol_register_error_callback(on_protocol_error);
    if (ret != APP_ERR_OK) {
        LOG_WARN("INIT", "Failed to register proto error callback (err=%d)", ret);
    }

    /* 启动协议接收任务 (后台运行) */
    ret = protocol_start();
    if (ret != APP_ERR_OK) {
        LOG_ERROR("INIT", "Protocol start failed (err=%d)", ret);
        return ret;
    }

    LOG_INFO("INIT", "Protocol stack started (UART%d, %dbps)",
             UART_STM32_NUM, UART_STM32_BAUD_RATE);

    return APP_ERR_OK;
}

/**
 * component_init_sensor - 初始化传感器服务
 *
 * 功能:
 *   - 配置传感器参数 (采样率、缓冲区大小、FFT窗口)
 *   - 初始化 ADXL345 驱动 (SPI通信)
 *   - 初始化 DSP 工作区 (预分配内存)
 *   - 创建环形缓冲区
 *   - 注册分析完成回调 (→MQTT队列)
 *   - 注册错误回调 (→监控系统)
 *   - 启动采集和分析任务
 *
 * Return: APP_ERR_OK 成功, 负数错误码
 */
static int component_init_sensor(void)
{
    int ret;
    struct sensor_config sensor_cfg = {0};

    /* 从 config_manager 加载参数 (或使用硬编码默认值) */
    const struct system_config *sys_cfg = config_manager_get();

    if (sys_cfg) {
        sensor_cfg.sample_rate_hz = sys_cfg->sample_rate_hz;
        sensor_cfg.ring_buffer_size = sys_cfg->sensor_buffer_size;
        sensor_cfg.fft_size = sys_cfg->fft_size;
        sensor_cfg.analysis_interval_ms = sys_cfg->analysis_interval_ms;
        sensor_cfg.enable_temp_compensation = sys_cfg->temp_comp_enabled;
        sensor_cfg.enable_protocol_temp = sys_cfg->sensor_enable_temp_from_protocol;
        sensor_cfg.enable_detailed_logging = false;  /* 生产环境关闭详细日志 */
    } else {
        /* 默认配置 (无NVS配置时的后备方案) */
        sensor_cfg.sample_rate_hz = 400;           /* 400 Hz 采样率 */
        sensor_cfg.ring_buffer_size = 1024;        /* 1024 样本环形缓冲区 */
        sensor_cfg.fft_size = 512;                 /* 512 点 FFT */
        sensor_cfg.analysis_interval_ms = 1000;    /* 每1秒分析一次 */
        sensor_cfg.enable_temp_compensation = true;
        sensor_cfg.enable_protocol_temp = true;
        sensor_cfg.enable_detailed_logging = false;
    }

    /* 初始化传感器服务 */
    ret = sensor_service_init(&sensor_cfg);
    if (ret != APP_ERR_OK) {
        LOG_ERROR("INIT", "Sensor service init failed (err=%d)", ret);
        return ret;
    }

    /* 注册分析完成回调 (当RMS/FFT完成后调用, 将结果推送到MQTT队列) */
    ret = sensor_service_register_analysis_callback(on_analysis_ready, &g_ctx);
    if (ret != APP_ERR_OK) {
        LOG_ERROR("INIT", "Failed to register analysis callback (err=%d)", ret);
        return ret;
    }

    /* 注册传感器错误回调 */
    ret = sensor_service_register_error_callback(on_sensor_error, &g_ctx);
    if (ret != APP_ERR_OK) {
        LOG_WARN("INIT", "Failed to register sensor error callback (err=%d)", ret);
    }

    /* 启动传感器服务 (创建采集和分析FreeRTOS任务) */
    ret = sensor_service_start();
    if (ret != APP_ERR_OK) {
        LOG_ERROR("INIT", "Sensor service start failed (err=%d)", ret);
        return ret;
    }

    LOG_INFO("INIT", "Sensor service started (rate=%dHz, fft=%d, interval=%dms)",
             sensor_cfg.sample_rate_hz, sensor_cfg.fft_size,
             sensor_cfg.analysis_interval_ms);

    return APP_ERR_OK;
}

/**
 * component_init_mqtt - 初始化 MQTT 通信模块
 *
 * 功能:
 *   - 加载 MQTT 配置 (从 config_manager 或默认值)
 *   - 配置训练模式 (Training Mode → PC端Broker)
 *   - 启用批量发布优化 (减少网络开销)
 *   - 启用离线缓存 (断网不丢数据)
 *   - 注册事件回调 (连接状态监控)
 *   - 启动 MQTT 客户端 (连接Broker)
 *
 * Return: APP_ERR_OK 成功, 负数错误码
 */
static int component_init_mqtt(void)
{
    int ret;
    struct mqtt_config mqtt_cfg = {0};
    struct batch_config batch_cfg = {0};

    /* 从 config_manager 加载配置 */
    const struct system_config *sys_cfg = config_manager_get();

    if (sys_cfg) {
        /* 运行模式: 0=Training(PC), 1=Upload(树莓派) */
        mqtt_cfg.mode = (sys_cfg->mqtt_mode == 1) ? MQTT_MODE_UPLOAD : MQTT_MODE_TRAINING;

        /* Broker 地址 */
        if (strlen(sys_cfg->mqtt_broker_url) > 0) {
            strncpy(mqtt_cfg.broker.url, sys_cfg->mqtt_broker_url,
                    sizeof(mqtt_cfg.broker.url) - 1);
        } else {
            strncpy(mqtt_cfg.broker.url, "mqtt://192.168.43.23:1883",
                    sizeof(mqtt_cfg.broker.url) - 1);  /* 默认PC端IP */
        }
        mqtt_cfg.broker.port = (uint16_t)sys_cfg->mqtt_broker_port;

        /* 认证信息 */
        if (strlen(sys_cfg->mqtt_username) > 0) {
            strncpy(mqtt_cfg.broker.username, sys_cfg->mqtt_username,
                    sizeof(mqtt_cfg.broker.username) - 1);
        }
        if (strlen(sys_cfg->mqtt_password) > 0) {
            strncpy(mqtt_cfg.broker.password, sys_cfg->mqtt_password,
                    sizeof(mqtt_cfg.broker.password) - 1);
        }

        /* TLS 配置 */
        mqtt_cfg.broker.enable_tls = sys_cfg->mqtt_enable_tls;
        /* 注意: mqtt_ca_cert 未在 system_config 中定义，如需 CA 证书请扩展配置结构体 */
        (void)0;  /* 占位: TLS CA 证书配置 */

        /* QoS 和发布间隔 */
        mqtt_cfg.broker.qos = (uint8_t)sys_cfg->mqtt_qos;
        mqtt_cfg.broker.keepalive_sec = (uint16_t)sys_cfg->mqtt_keepalive_sec;
        mqtt_cfg.publish_interval_ms = sys_cfg->mqtt_publish_interval_ms;

        /* 发布内容开关 */
        mqtt_cfg.publish_vibration_data = sys_cfg->mqtt_publish_vibration;
        mqtt_cfg.publish_environment_data = sys_cfg->mqtt_publish_environment;
        mqtt_cfg.publish_health_status = sys_cfg->mqtt_publish_health;
    } else {
        /* 默认配置 (无NVS时的后备方案) */
        mqtt_cfg.mode = MQTT_MODE_TRAINING;

        /*
         * ⚠️ 【关键修复】修正MQTT Broker默认地址!
         *
         * 原始BUG:
         *   默认地址是 192.168.1.100:1883
         *   但ESP32连接的是手机热点(192.168.43.x网段)
         *   不同网段导致网络不可达 → MQTT连接失败(错误32774)
         *
         * 正确做法:
         *   使用手机热点网段的PC端IP
         *   从路由器管理页面查看已连接设备IP
         *   或在PC端运行 ipconfig 查看WLAN适配器IP
         *
         * 当前配置:
         *   ESP32 IP: 192.168.43.25 (从热点分配)
         *   PC (huwan) IP: 192.168.43.23 (需要确认)
         */
        snprintf(mqtt_cfg.broker.url, sizeof(mqtt_cfg.broker.url),
                 "mqtt://192.168.43.23:1883");  /* PC端IP (手机热点网段) */
        mqtt_cfg.broker.port = 1883;
        mqtt_cfg.broker.qos = 1;
        mqtt_cfg.broker.keepalive_sec = 120;
        mqtt_cfg.publish_interval_ms = 1000;
        mqtt_cfg.publish_vibration_data = true;
        mqtt_cfg.publish_environment_data = true;
        mqtt_cfg.publish_health_status = false;
    }

    /* 生成唯一 Client ID */
    snprintf(mqtt_cfg.broker.client_id, sizeof(mqtt_cfg.broker.client_id),
             "EdgeVib-%08" PRIx32, (uint32_t)esp_random());

    /* 虚拟设备配置 (默认仅物理设备本身) */
    mqtt_cfg.num_virtual_devices = 1;
    mqtt_cfg.devices[0].virtual_dev_id = 1;
    snprintf(mqtt_cfg.devices[0].name, sizeof(mqtt_cfg.devices[0].name),
             "ESP32-S3-DevKitC");

    /* LWT (Last Will and Testament) 配置 */
    mqtt_cfg.enable_lwt = true;
    snprintf(mqtt_cfg.lwt_topic, sizeof(mqtt_cfg.lwt_topic),
             "edgevib/status/%s", mqtt_cfg.broker.client_id);
    snprintf(mqtt_cfg.lwt_message, sizeof(mqtt_cfg.lwt_message),
             "{\"status\":\"offline\",\"client_id\":\"%s\"}", mqtt_cfg.broker.client_id);

    /* 初始化 MQTT 模块 */
    ret = mqtt_init(&mqtt_cfg);
    if (ret != APP_ERR_OK) {
        LOG_ERROR("INIT", "MQTT module init failed (err=%d)", ret);
        return ret;
    }

    /* 配置批量发布优化 */
    /*
     * ⚠️ 【关键修复】禁用Batch模式!
     *
     * 问题现象:
     *   ESP32日志: "Published batch: 3 messages, 1399 bytes -> edgevib/train/1/vibration"
     *   PC端日志:  "缺少字段: ['timestamp_ms', 'dev_id', 'rms_x', 'rms_y', ...]"
     *
     * 根因分析:
     *   Batch模式发送的JSON格式为:
     *     {"batch":true,"count":3,"messages":[{单条消息...}, ...]}
     *   而edge-ai/mqtt_subscriber.py 只能解析单条消息格式:
     *     {"dev_id":1,"data":{"vibration":{"rms_x":...},"environment":{...}}}
     *   PC端检查 'data' in data → batch格式外层无'data'键 → 字段提升逻辑被跳过
     *   → REQUIRED_FIELDS全部缺失 → 打印"缺少字段"警告
     *
     * 解决方案:
     *   禁用batch模式, 改为逐条发布单条消息JSON
     *   这样PC端可以正确解析每条消息并提取rms_x等字段
     */
    batch_cfg.enabled = false;
    batch_cfg.max_messages = 4;           /* 单次最多合并4条消息 */
    batch_cfg.flush_interval_ms = 2000;  /* 2秒超时自动刷新 */
    batch_cfg.max_batch_size_bytes = 4096;
    batch_cfg.merge_same_topic = true;
    ret = mqtt_set_batch_config(&batch_cfg);
    if (ret != APP_ERR_OK) {
        LOG_WARN("INIT", "Failed to set batch config (err=%d), using default", ret);
    }

    /* 注册 MQTT 事件回调 (连接/断开/错误监控) */
    ret = mqtt_register_event_callback(on_mqtt_event, &g_ctx);
    if (ret != APP_ERR_OK) {
        LOG_WARN("INIT", "Failed to register MQTT event callback (err=%d)", ret);
    }

    /* 启动 MQTT 客户端 (连接Broker) */
    ret = mqtt_start();
    if (ret != APP_ERR_OK) {
        LOG_ERROR("INIT", "MQTT client start failed (err=%d)", ret);
        return ret;
    }

    LOG_INFO("INIT", "MQTT module started (mode=%s, broker=%s, batch=%s)",
             mqtt_cfg.mode == MQTT_MODE_TRAINING ? "TRAINING" : "UPLOAD",
             mqtt_cfg.broker.url,
             batch_cfg.enabled ? "ON" : "OFF");

    return APP_ERR_OK;
}

/* ==================== 回调函数实现 ==================== */

/**
 * on_temp_data_received - 温湿度数据接收回调
 *
 * 触发时机:
 *   当 Protocol 模块收到 STM32 的 CMD_TEMP_HUMIDITY_DATA 帧时自动调用
 *
 * 处理逻辑:
 *   1. 将温湿度数据推送到 Q1 队列 (UART→Sensor)
 *   2. 更新统计计数器
 *   3. 如果队列满, 丢弃最旧的数据并记录警告
 *
 * @data: 解析后的温湿度数据结构体指针 (生命周期仅限本次回调)
 * @user_data: 用户上下文 (指向 g_ctx)
 */
static void on_temp_data_received(const struct temp_humidity_data *data,
                                   void *user_data)
{
    struct gateway_context *ctx = (struct gateway_context *)user_data;

    if (!ctx || !ctx->q_temp_data || !data) {
        return;
    }

    /* 尝试将数据推入队列 (非阻塞, 超时100ms) */
    BaseType_t ret = xQueueSend(ctx->q_temp_data, data, pdMS_TO_TICKS(QUEUE_TIMEOUT_MS));

    if (ret != pdTRUE) {
        static uint32_t last_q1_full = 0;
        uint32_t now_q1 = (uint32_t)(esp_timer_get_time() / 1000LL);
        if ((now_q1 - last_q1_full) >= 30000) {
            LOG_WARN("CALLBACK", "Temp data queue full, dropping oldest data");
            last_q1_full = now_q1;
        }
        struct temp_humidity_data dummy;
        xQueueReceive(ctx->q_temp_data, &dummy, 0);
        xQueueSend(ctx->q_temp_data, data, 0);
    }

    ctx->stats.temp_data_processed++;
    ctx->uart_connected = true;  /* 收到数据说明STM32在线 */

    LOG_DEBUG("CALLBACK", "Temp data received: T=%.1f°C, H=%.1f%%RH",
             data->temperature_c, data->humidity_rh);
}

/**
 * on_analysis_ready - 传感器分析完成回调
 *
 * 触发时机:
 *   当 Sensor Service 完成 RMS/FFT 分析后自动调用
 *
 * 处理逻辑:
 *   1. 将 analysis_result 推送到 Q2 队列 (Sensor→MQTT)
 *   2. 更新统计计数器
 *   3. 如果队列满, 丢弃最旧的分析结果
 *
 * @result: 完整分析结果指针 (只读, 生命周期仅限本次回调)
 * @user_data: 用户上下文 (指向 g_ctx)
 */
static void on_analysis_ready(const struct analysis_result *result,
                               void *user_data)
{
    struct gateway_context *ctx = (struct gateway_context *)user_data;

    if (!ctx || !ctx->q_analysis_result || !result) {
        return;
    }

    /* 尝试将分析结果推入队列 (非阻塞, 超时100ms) */
    BaseType_t ret = xQueueSend(ctx->q_analysis_result, result, pdMS_TO_TICKS(QUEUE_TIMEOUT_MS));

    if (ret != pdTRUE) {
        static struct analysis_result dummy;
        static uint32_t last_queue_full_warn = 0;
        uint32_t now_qfull = (uint32_t)(esp_timer_get_time() / 1000LL);
        if ((now_qfull - last_queue_full_warn) >= 30000) {
            LOG_WARN("CALLBACK", "Analysis result queue full, dropping oldest");
            last_queue_full_warn = now_qfull;
        }
        xQueueReceive(ctx->q_analysis_result, &dummy, 0);
        xQueueSend(ctx->q_analysis_result, result, 0);
    }

    static uint32_t last_dispatch_log = 0;
    uint32_t now_disp = (uint32_t)(esp_timer_get_time() / 1000LL);
    if ((now_disp - last_dispatch_log) >= 10000) {
        LOG_INFO("CALLBACK", "Analysis dispatched: RMS=%.4fg F=%.1fHz T=%.1f°C samples=%u state=%d",
                 result->overall_rms_g, result->peak_frequency_hz,
                 result->temperature_c, result->samples_analyzed,
                 (int)result->service_state);
        last_dispatch_log = now_disp;
    }
}

/**
 * on_sensor_error - 传感器错误回调
 *
 * 触发时机:
 *   - ADXL345 采集失败
 *   - DSP 计算溢出
 *   - 温度补偿异常
 *
 * 处理逻辑:
 *   1. 记录错误日志 (包含错误码和上下文)
 *   2. 更新错误计数器
 *   3. 如果错误过多 (>10次/分钟), 触发系统降级
 *
 * @error_code: 错误码 (APP_ERR_* 系列)
 * @context: 错误描述字符串
 * @user_data: 用户上下文
 */
static void on_sensor_error(int error_code,
                             const char *context,
                             void *user_data)
{
    struct gateway_context *ctx = user_data ? (struct gateway_context *)user_data : &g_ctx;

    if (!ctx) {
        return;
    }

    ctx->error_count++;
    ctx->stats.sensor_errors++;

    /*
     * ⚠️ 【优化】错误日志频率限制
     *
     * 问题:
     *   原代码每次错误都打印日志, 在降级模式下会导致:
     *     "Error -1103: Analysis failed (total_errors=2)"
     *     "Error -1103: Analysis failed (total_errors=3)"
     *     ... 每秒一次, 快速累积到100+
     *
     * 解决方案:
     *   - 相同错误码: 每10秒打印一次
     *   - 不同错误码: 立即打印
     *   - 总是更新计数器 (用于统计)
     */
    static uint32_t last_error_time = 0;
    static int last_error_code = 0;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000LL);

    if (error_code != last_error_code || (now - last_error_time) >= 10000) {
        LOG_ERROR("SENSOR-ERR", "Error %d: %s (total_errors=%u)",
                 error_code, context ? context : "unknown", ctx->error_count);
        last_error_time = now;
        last_error_code = error_code;
    }

    /* 闪烁错误LED (如果有配置) */
#ifdef GPIO_LED_ERROR
    static int blink_count = 0;
    if (++blink_count <= 3) {
        gpio_set_level(GPIO_LED_ERROR, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(GPIO_LED_ERROR, 0);
    } else {
        blink_count = 0;
    }
#endif
}

/**
 * on_protocol_error - Protocol 模块专用错误回调（2参数版本）
 *
 * 包装函数: 适配 proto_error_callback_t 签名 (error_code, context)
 * 内部调用完整的 on_sensor_error 并传入全局上下文 g_ctx
 */
static void on_protocol_error(int error_code,
                              const char *context)
{
    on_sensor_error(error_code, context, &g_ctx);
}

/**
 * on_mqtt_event - MQTT 事件回调
 *
 * 触发时机:
 *   - MQTT_EVENT_CONNECTED: 连接成功
 *   - MQTT_EVENT_DISCONNECTED: 断开连接
 *   - MQTT_EVENT_ERROR: 传输错误
 *
 * 处理逻辑:
 *   1. 记录事件日志
 *   2. 更新连接状态标志
 *   3. 断连时点亮错误LED
 *
 * @event_type: 事件类型 (对应 MQTT_EVENT_*)
 * @context: 事件描述
 * @user_data: 用户上下文
 */
static void on_mqtt_event(int event_type,
                           const char *context,
                           void *user_data)
{
    (void)user_data;

    switch (event_type) {
        case MQTT_STATE_CONNECTED:
            LOG_INFO("MQTT-EVT", "✓ Connected to broker: %s", context ? context : "");
#ifdef GPIO_LED_STATUS
            gpio_set_level(GPIO_LED_STATUS, 1);  /* 点亮状态灯 */
#endif
            break;

        case MQTT_STATE_DISCONNECTED:
            LOG_WARN("MQTT-EVT", "✗ Disconnected from broker: %s", context ? context : "");
#ifdef GPIO_LED_STATUS
            gpio_set_level(GPIO_LED_STATUS, 0);  /* 关闭状态灯 */
#endif
            break;

        case MQTT_STATE_ERROR:
            LOG_ERROR("MQTT-EVT", "⚠ Error: %s", context ? context : "");
            break;

        default:
            LOG_DEBUG("MQTT-EVT", "Event %d: %s", event_type, context ? context : "");
            break;
    }
}

/* ==================== FreeRTOS 任务实现 ==================== */

/**
 * task_uart_rx - UART 接收监控任务
 *
 * 功能:
 *   - 监控 UART 接收状态 (通过心跳检测判断STM32是否在线)
 *   - 定期请求温湿度数据 (如果长时间未收到)
 *   - 统计接收帧率和错误率
 *   - 异常时触发报警
 *
 * 优先级: 7 (最高, 保证实时性)
 * 栈大小: 4096 字节
 * 循环周期: 1 秒
 *
 * @arg: 用户参数 (未使用)
 */
static void task_uart_rx(void *arg)
{
    (void)arg;

    LOG_INFO("TASK-UART", "UART RX monitor task started (priority=%d)",
             TASK_PRIORITY_UART_RX);

    /*
     * 退避机制变量:
     * - consecutive_failures: 连续失败计数
     * - backoff_ms: 当前退避时间 (指数增长)
     * - last_temp_request: 上次请求时间戳
     */
    uint32_t consecutive_failures = 0;
    uint32_t backoff_ms = 1000;  /* 初始退避1秒 */
    static uint32_t last_temp_request = 0;

    while (g_ctx.system_running) {
        /* 检查对端在线状态 (基于心跳响应) */
        bool peer_alive = protocol_is_peer_alive();

        if (!peer_alive && g_ctx.uart_connected) {
            /*
             * ⚠️ 【优化】STM32离线提示添加频率控制!
             *
             * 原始问题:
             *   每次检测到STM32离线都打印WARN
             *   如果STM32长期离线,会频繁输出相同警告
             *
             * 解决方案:
             *   添加30秒频率控制,避免日志洪水
             */
            static uint32_t last_offline_warn = 0;
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000LL);
            if (now - last_offline_warn >= 30000 || last_offline_warn == 0) {
                LOG_WARN("TASK-UART", "STM32 peer appears offline (no heartbeat, check power/wiring)");
                last_offline_warn = now;
            }
            g_ctx.uart_connected = false;
            consecutive_failures++;
        } else if (peer_alive && !g_ctx.uart_connected) {
            /* STM32 恢复在线 */
            LOG_INFO("TASK-UART", "STM32 peer back online!");
            g_ctx.uart_connected = true;
            consecutive_failures = 0;  /* 重置失败计数 */
            backoff_ms = 1000;         /* 重置退避时间 */
        }

        /*
         * ⚠️ 【关键修复】STM32离线时禁止发送需要ACK的命令!
         *
         * 问题现象:
         *   日志中大量出现:
         *     "Retry send cmd=0x13 attempt 1/3"
         *     "ERROR PROTO: ACK timeout for cmd=0x13 after 3 retries"
         *     "ERROR SENSOR-ERR: Error -1010: ACK timeout (total_errors=N)"
         *
         * 根因分析:
         *   task_uart_rx 在 STM32离线时仍然调用 protocol_request_temp_data()
         *   该函数内部使用 protocol_send_with_ack() 需要等待ACK响应
         *   STM32离线 → 永远收不到ACK → 3次重试全部超时
         *   每次重试消耗 ~3000ms (3次 × 1000ms超时)
         *   同时心跳任务也在检测离线 → 双重日志输出
         *
         * 解决方案:
         *   - STM32离线时: 不发送任何需要ACK的请求命令
         *   - 只依赖心跳任务(task_heartbeat)检测在线状态
         *   - STM32恢复在线后: 自动恢复正常温度数据请求
         */
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000LL);  /* ms */

        if (g_ctx.uart_connected && (now - last_temp_request) > backoff_ms) {
            /* 仅在对端在线时才请求温度数据! */
            int ret = protocol_request_temp_data();
            if (ret == APP_ERR_OK) {
                LOG_DEBUG("TASK-UART", "Sent temperature data request to STM32");
            } else if (ret == APP_ERR_PROTO_ACK_TIMEOUT) {
                /*
                 * ACK超时说明对端可能刚掉线, 标记为离线并退避
                 * 下次循环会通过 protocol_is_peer_alive() 确认状态
                 */
                g_ctx.uart_connected = false;
                consecutive_failures++;
            }

            if (consecutive_failures > 0) {
                backoff_ms *= 2;
                if (backoff_ms > 30000) {
                    backoff_ms = 30000;  /* 最大30秒 */
                }
            }
            last_temp_request = now;
        } else if (!g_ctx.uart_connected) {
            /*
             * 离线状态: 不发送请求, 只做轻量级检查
             * 心跳任务负责检测恢复在线
             */
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        /* 获取协议统计信息 (每10秒打印一次) */
        static uint32_t last_stats_print = 0;
        if ((now - last_stats_print) >= 10000) {
            struct proto_stats pstats;
            if (protocol_get_stats(&pstats) == APP_ERR_OK) {
                /*
                 * 仅在有帧接收或错误时打印, 减少正常日志量
                 */
                if (pstats.rx_frames > 0 || pstats.crc_errors > 0) {
                    LOG_INFO("TASK-UART", "Protocol stats: rx_frames=%u, crc_err=%u",
                             pstats.rx_frames, pstats.crc_errors);
                }
                g_ctx.stats.uart_frames_received = pstats.rx_frames;
            }
            last_stats_print = now;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));  /* 1秒循环 */
    }

    LOG_INFO("TASK-UART", "UART RX monitor task stopped");
    vTaskDelete(NULL);
}

/**
 * task_sensor_process - 传感器数据处理协调任务
 *
 * 功能:
 *   - 从 Q1 队列获取温湿度数据
 *   - 调用 sensor_service 进行温度补偿
 *   - 监控传感器健康状态
 *   - 异常时触发降级处理
 *
 * 优先级: 6 (中等, 低于UART但高于MQTT)
 * 栈大小: 8192 字节 (需容纳可能的调试操作)
 * 阻塞方式: 队列读取 (带超时)
 *
 * @arg: 用户参数 (未使用)
 */
static void task_sensor_process(void *arg)
{
    (void)arg;
    struct temp_humidity_data temp_data;

    LOG_INFO("TASK-SENSOR", "Sensor process task started (priority=%d)",
             TASK_PRIORITY_SENSOR);

    while (g_ctx.system_running) {
        /* 从队列获取温湿度数据 (阻塞等待, 超时1000ms) */
        BaseType_t ret = xQueueReceive(g_ctx.q_temp_data, &temp_data,
                                       pdMS_TO_TICKS(QUEUE_TIMEOUT_MS));

        if (ret == pdTRUE) {
            /*
             * 温湿度数据处理流程:
             * 1. 数据已通过 protocol 层解析并校验
             * 2. sensor_service 内部已注册 temp_callback
             * 3. 温度补偿模块会自动应用此数据进行偏移校正
             * 4. 此处可添加额外业务逻辑 (如阈值告警)
             */

            LOG_DEBUG("TASK-SENSOR", "Processed temp data: T=%.1f°C, H=%.1f%%RH (status=%d)",
                     temp_data.temperature_c, temp_data.humidity_rh,
                     temp_data.sensor_status);

            /* TODO: 可在此添加温度阈值检测逻辑 */
            /*
             * if (temp_data.temperature_c > TEMP_WARNING_THRESHOLD) {
             *     LOG_WARN("TASK-SENSOR", "High temperature warning: %.1f°C",
             *              temp_data.temperature_c);
             * }
             */
        } else {
            /* 队列读取超时 (正常情况, 表示暂无新数据) */
            LOG_DEBUG("TASK-SENSOR", "No new temp data in queue (timeout)");
        }

        /* 查询传感器服务状态 (每5秒一次) */
        static uint32_t last_state_check = 0;
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000LL);

        if ((now - last_state_check) >= 5000) {
            enum sensor_state state = sensor_service_get_state();

            if (state == SENSOR_STATE_ERROR) {
                LOG_ERROR("TASK-SENSOR", "Sensor service in ERROR state!");
            } else if (state == SENSOR_STATE_DEGRADED) {
                LOG_WARN("TASK-SENSOR", "Sensor service running in DEGRADED mode");
            }

            last_state_check = now;
        }
    }

    LOG_INFO("TASK-SENSOR", "Sensor process task stopped");
    vTaskDelete(NULL);
}

/**
 * task_mqtt_upload - MQTT 数据上传任务
 *
 * 功能:
 *   - 从 Q2 队列获取分析结果
 *   - 调用 mqtt_publish_analysis_result() 发布到 Broker
 *   - 处理发布失败 (利用离线缓存机制)
 *   - 监控 MQTT 连接状态
 *   - 统计上传成功/失败率
 *
 * 优先级: 5 (较低, 允许积压)
 * 栈大小: 8192 字节 (增大以防止发布路径中json_buffer+processed_result栈溢出)
 * 阻塞方式: 队列读取 (带超时)
 *
 * @arg: 用户参数 (未使用)
 */
static void task_mqtt_upload(void *arg)
{
    (void)arg;
    static struct analysis_result analysis;

    LOG_INFO("TASK-MQTT", "MQTT upload task started (priority=%d)",
             TASK_PRIORITY_MQTT);

    while (g_ctx.system_running) {
        BaseType_t ret = xQueueReceive(g_ctx.q_analysis_result, &analysis,
                                       pdMS_TO_TICKS(2000));

        if (ret != pdTRUE) {
            static uint32_t last_q2_empty = 0;
            uint32_t now_q2 = (uint32_t)(esp_timer_get_time() / 1000LL);
            if ((now_q2 - last_q2_empty) >= 30000) {
                LOG_INFO("TASK-MQTT", "Q2 queue empty (no analysis results from sensor_service)");
                last_q2_empty = now_q2;
            }
            goto print_stats;
        }

        /*
         * ⚠️ 【关键修复】仅在"既无振动又无温度"时才跳过!
         *
         * 原始BUG (v1):
         *   无条件跳过 samples_analyzed==0 的结果
         *   → 降级模式温湿度数据被完全丢弃
         *
         * 原始BUG (v2):
         *   尝试修复: 在mqtt_publish_analysis_result()中允许
         *   samples_analyzed==0 && temperature_valid 发布
         *   但此处的入口检查直接 goto print_stats → 修复被绕过!
         *
         * 正确做法:
         *   只有 samples_analyzed==0 && temperature_valid==false 才跳过
         *   有温湿度数据时正常走发布流程
         */
        if (analysis.samples_analyzed == 0 && !analysis.temperature_valid) {
            static uint32_t last_skip_log = 0;
            uint32_t now_skip = (uint32_t)(esp_timer_get_time() / 1000LL);
            if ((now_skip - last_skip_log) >= 30000) {
                LOG_INFO("TASK-MQTT", "No data to publish (vib=0, temp=0)");
                last_skip_log = now_skip;
            }
            goto print_stats;
        }

        if (analysis.samples_analyzed == 0 && analysis.temperature_valid) {
            static uint32_t last_degraded_log = 0;
            uint32_t now_dg = (uint32_t)(esp_timer_get_time() / 1000LL);
            if ((now_dg - last_degraded_log) >= 10000) {
                LOG_INFO("TASK-MQTT", "Publishing degraded result (T=%.1f°C H=%.1f%% vib=0)",
                         analysis.temperature_c, analysis.humidity_rh);
                last_degraded_log = now_dg;
            }
        }

        if (!mqtt_is_connected()) {
            static uint32_t last_offline_log = 0;
            uint32_t now_off = (uint32_t)(esp_timer_get_time() / 1000LL);
            if ((now_off - last_offline_log) >= 30000) {
                LOG_INFO("TASK-MQTT", "MQTT offline, messages queued for later (published=%u)",
                         g_ctx.stats.analysis_published);
                last_offline_log = now_off;
            }
            mqtt_publish_analysis_result(&analysis, 0);
            goto print_stats;
        }

        int pub_ret = mqtt_publish_analysis_result(&analysis, 0);

        if (pub_ret == APP_ERR_OK) {
            g_ctx.stats.mqtt_publish_success++;
            g_ctx.stats.analysis_published++;

            static uint32_t last_pub_ok = 0;
            uint32_t now_pub = (uint32_t)(esp_timer_get_time() / 1000LL);
            if ((now_pub - last_pub_ok) >= 10000) {
                LOG_INFO("TASK-MQTT", "Published #%u (RMS=%.4fg F=%.1fHz T=%.1f°C)",
                         g_ctx.stats.analysis_published,
                         analysis.overall_rms_g, analysis.peak_frequency_hz,
                         analysis.temperature_c);
                last_pub_ok = now_pub;
            }
        } else if (pub_ret == APP_ERR_NO_DATA) {
            static uint32_t last_nodata_log = 0;
            uint32_t now_nd = (uint32_t)(esp_timer_get_time() / 1000LL);
            if ((now_nd - last_nodata_log) >= 30000) {
                LOG_INFO("TASK-MQTT", "Skipped publish (no sensor data)");
                last_nodata_log = now_nd;
            }
        } else {
            g_ctx.stats.mqtt_publish_failed++;
            g_ctx.stats.analysis_published++;

            static uint32_t last_pub_fail_warn = 0;
            uint32_t now_pf = (uint32_t)(esp_timer_get_time() / 1000LL);
            if ((now_pf - last_pub_fail_warn) >= 30000) {
                LOG_WARN("TASK-MQTT", "Publish failed (err=%d samples=%u rms=%.4f)",
                         pub_ret, analysis.samples_analyzed, analysis.overall_rms_g);
                last_pub_fail_warn = now_pf;
            }
        }

print_stats:
        static uint32_t last_stats = 0;
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000LL);

        if ((now - last_stats) >= 30000) {
            LOG_INFO("TASK-MQTT", "Upload stats: success=%u, failed=%u, published=%u",
                     g_ctx.stats.mqtt_publish_success,
                     g_ctx.stats.mqtt_publish_failed,
                     g_ctx.stats.analysis_published);
            last_stats = now;
        }
    }

    LOG_INFO("TASK-MQTT", "MQTT upload task stopped");
    vTaskDelete(NULL);
}

/**
 * task_system_monitor - 系统监控任务
 *
 * 功能:
 *   - 监控 FreeRTOS 堆栈剩余空间 (防止栈溢出)
 *   - 监控堆内存使用情况
 *   - 定期打印系统健康报告
 *   - 检测异常状态并尝试恢复
 *
 * 优先级: 2 (最低, 不影响实时业务)
 * 栈大小: 2048 字节
 * 循环周期: 10 秒
 *
 * @arg: 用户参数 (未使用)
 */
static void task_system_monitor(void *arg)
{
    (void)arg;

    LOG_INFO("TASK-MONITOR", "System monitor task started (priority=%d)",
             TASK_PRIORITY_MONITOR);

    while (g_ctx.system_running) {
        /* 获取各任务的堆栈高水位标记 (剩余最小栈空间) */
        UBaseType_t uart_stack_min = g_ctx.task_uart_rx ? uxTaskGetStackHighWaterMark(g_ctx.task_uart_rx) : 0;
        UBaseType_t sensor_stack_min = g_ctx.task_sensor ? uxTaskGetStackHighWaterMark(g_ctx.task_sensor) : 0;
        UBaseType_t mqtt_stack_min = g_ctx.task_mqtt ? uxTaskGetStackHighWaterMark(g_ctx.task_mqtt) : 0;
        UBaseType_t monitor_stack_min = uxTaskGetStackHighWaterMark(NULL);

        /* 获取堆内存使用情况 */
        size_t heap_free = esp_get_free_heap_size();
        size_t heap_min = esp_get_minimum_free_heap_size();

        /* 打印系统健康报告 */
        LOG_INFO("TASK-MONITOR", "=== System Health Report ===");
        LOG_INFO("TASK-MONITOR", "  Heap: free=%u bytes, min_free=%u bytes",
                 heap_free, heap_min);
        LOG_INFO("TASK-MONITOR", "  Stack usage:");
        LOG_INFO("TASK-MONITOR", "    UART_RX:   used=%u/%u bytes (%.1f%%)",
                 TASK_STACK_UART_RX - uart_stack_min, TASK_STACK_UART_RX,
                 (float)(TASK_STACK_UART_RX - uart_stack_min) / TASK_STACK_UART_RX * 100.0f);
        LOG_INFO("TASK-MONITOR", "    SENSOR:   used=%u/%u bytes (%.1f%%)",
                 TASK_STACK_SENSOR - sensor_stack_min, TASK_STACK_SENSOR,
                 (float)(TASK_STACK_SENSOR - sensor_stack_min) / TASK_STACK_SENSOR * 100.0f);
        LOG_INFO("TASK-MONITOR", "    MQTT:     used=%u/%u bytes (%.1f%%)",
                 TASK_STACK_MQTT - mqtt_stack_min, TASK_STACK_MQTT,
                 (float)(TASK_STACK_MQTT - mqtt_stack_min) / TASK_STACK_MQTT * 100.0f);
        LOG_INFO("TASK-MONITOR", "    MONITOR:  used=%u/%u bytes (%.1f%%)",
                 TASK_STACK_MONITOR - monitor_stack_min, TASK_STACK_MONITOR,
                 (float)(TASK_STACK_MONITOR - monitor_stack_min) / TASK_STACK_MONITOR * 100.0f);

        /* 检查堆内存是否过低 (<10KB) */
        if (heap_min < 10240) {
            LOG_ERROR("TASK-MONITOR", "⚠ Low memory warning: min_free=%u bytes!", heap_min);
        }

        /* 检查栈空间是否不足 (剩余<200字节) */
        if (uart_stack_min < 200 || sensor_stack_min < 200 ||
            mqtt_stack_min < 200 || monitor_stack_min < 200) {
            LOG_ERROR("TASK-MONITOR", "⚠ Stack overflow risk detected!");
            g_ctx.error_count++;
        }

        /* 打印网关总体统计 */
        uint64_t uptime_s = (esp_timer_get_time() - g_ctx.uptime_start_us) / 1000000ULL;
        LOG_INFO("TASK-MONITOR", "  Gateway uptime: %llu s", uptime_s);
        LOG_INFO("TASK-MONITOR", "  Total errors: %u", g_ctx.error_count);
        LOG_INFO("TASK-MONITOR", "  UART frames: %u", g_ctx.stats.uart_frames_received);
        LOG_INFO("TASK-MONITOR", "  Analysis published: %u", g_ctx.stats.analysis_published);
        LOG_INFO("TASK-MONITOR", "  MQTT success/fail: %u/%u",
                 g_ctx.stats.mqtt_publish_success,
                 g_ctx.stats.mqtt_publish_failed);

        vTaskDelay(pdMS_TO_TICKS(10000));  /* 10秒循环 */
    }

    LOG_INFO("TASK-MONITOR", "System monitor task stopped");
    vTaskDelete(NULL);
}

/* ==================== 主入口函数 ==================== */

/**
 * app_main - ESP32 应用主入口
 *
 * 初始化顺序 (严格遵循 AGENTS.md 规范):
 *   1. 日志系统 (LOG_INFO/WARN/ERROR)
 *   2. 配置管理器 (NVS 加载/默认值)
 *   3. 时间同步 (SNTP)
 *   4. 硬件 GPIO/UART 初始化
 *   5. 协议栈 (UART通信)
 *   6. 传感器服务 (ADXL345+DSP)
 *   7. MQTT 通信 (TLS+批量+离线缓存)
 *   8. 创建 FreeRTOS 任务 (4个核心任务)
 *   9. 进入 FreeRTOS 调度器 (永不返回)
 *
 * 错误处理策略:
 *   - 非致命错误: 记录日志, 继续启动 (降级运行)
 *   - 致命错误: 打印错误信息, 返回 (系统挂起)
 *
 * 注意:
 *   - app_main 执行完毕后, 控制权交给 FreeRTOS 调度器
 *   - 此函数不应返回 (除非发生致命错误)
 */
void app_main(void)
{
    /*
     * ================================================================
     * 生产模式 (完整初始化)
     * ================================================================
     *
     * 注意: Unity测试文件已通过CMakeLists.txt排除编译,
     *       此处不再需要 #ifdef CONFIG_UNITY_* 条件判断
     */

    esp_err_t esp_ret;
    int ret;

    /* ========== 第0步: 初始化日志系统 (必须最先!) ==========
     *
     * ⚠️ 【极其重要】日志系统必须在所有其他组件之前初始化!
     *   原因: 后续所有组件的初始化都需要使用 LOG_INFO/LOG_ERROR
     *         如果log_system未初始化,会导致 uart_write_bytes 错误
     */
    ret = component_init_log();
    if (ret != APP_ERR_OK) {
        /* 日志系统初始化失败是致命的, 但仍尝试继续 (printf作为后备) */
        printf("[FATAL] Log system init failed: %d\n", ret);
    }

    LOG_INFO("MAIN", "========================================");
    LOG_INFO("MAIN", "  EdgeVib ESP32-S3 Gateway Starting");
    LOG_INFO("MAIN", "  Firmware Version: 1.0.0");
    LOG_INFO("MAIN", "  Build Date: %s %s", __DATE__, __TIME__);
    LOG_INFO("MAIN","========================================");

    /* ========== 第1步: 初始化 NVS Flash (非易失性存储) ========== */
    esp_ret = nvs_flash_init();
    if (esp_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        esp_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /*
         * NVS 分区已满或版本不兼容:
         * 擦除全部内容后重新初始化 (相当于"恢复出厂设置")
         * 首次烧写新固件时通常会触发此分支
         */
        LOG_WARN("MAIN", "NVS partition needs reformatting, erasing...");
        esp_ret = nvs_flash_erase();
        if (esp_ret != ESP_OK) {
            LOG_ERROR("MAIN", "Failed to erase NVS (err=0x%x)", esp_ret);
            return;  /* 致命错误: 无法使用配置系统 */
        }
        
        esp_ret = nvs_flash_init();
        if (esp_ret != ESP_OK) {
            LOG_ERROR("MAIN", "Failed to re-init NVS after erase (err=0x%x)", esp_ret);
            return;
        }
        
        LOG_INFO("MAIN", "NVS erased and re-initialized successfully");
    } else if (esp_ret != ESP_OK) {
        LOG_ERROR("MAIN", "NVS flash init failed (err=0x%x)", esp_ret);
        /* 不致命: config_manager 会使用默认值 */
    } else {
        LOG_INFO("MAIN", "NVS Flash initialized OK");
    }

    /* ========== 第2步: 连接 WiFi (手机热点) ==========
     *
     * ⚠️ 【重要】WiFi 必须在其他网络组件之前连接!
     *   原因:
     *   [1] MQTT 需要 IP 地址才能连接 Broker
     *   [2] SNTP 时间同步需要网络
     *   [3] 所有 TCP/IP 通信依赖此步骤
     *
     * 默认配置 (需修改):
     *   SSID: "EdgeVib_Hotspot"  ← 改成你的热点名称!
     *   Password: "12345678"    ← 改成你的密码!
     *
     * 如果连接失败:
     *   - 检查手机热点是否开启 (2.4GHz)
     *   - 检查 SSID 和密码是否正确
     *   - 检查距离是否过远 (<10米)
     */
    ret = wifi_init_sta();
    if (ret != APP_ERR_OK) {
        LOG_ERROR("MAIN", "⚠ WiFi connection failed (err=%d)", ret);
        LOG_ERROR("MAIN", "System will continue in OFFLINE mode!");
        LOG_ERROR("MAIN", "MQTT/SNTP will not work without network!");
        /*
         * 不立即返回, 允许系统以离线模式启动:
         * - UART/传感器功能仍可工作
         * - 数据会缓存在 RingBuffer 中
         * - 等待后续手动重连或自动重连成功
         */
    } else {
        LOG_INFO("MAIN", "✓ WiFi connected successfully!");
    }

    /* ========== 第3步: 初始化配置管理器 ========== */
    ret = config_manager_init(NULL);
    if (ret != APP_ERR_OK) {
        LOG_WARN("MAIN", "Config manager init failed (err=%d), using defaults", ret);
        /* 不致命, 各模块会使用硬编码默认值 */
    } else {
        /* 打印当前 device_id (用于 Topic 构建) */
        uint8_t dev_id = config_manager_get_device_id();
        LOG_INFO("MAIN", "Device ID: %u (used in MQTT topics)", dev_id);
    }

    /* ========== 第4步: 初始化时间同步 (SNTP) ==========
     *
     * 依赖: WiFi 已连接 (否则会超时降级为本地时间)
     */
    ret = component_init_time_sync();
    if (ret != APP_ERR_OK) {
        /*
         * ⚠️ 【优化】时间同步失败降级为INFO!
         *
         * 原始问题:
         *   WiFi未连接或NTP不可达时打印WARN
         *   在离线训练模式下是正常的
         */
        LOG_INFO("MAIN", "Time sync unavailable (err=%d), using RTC/local time (OK for offline mode)", ret);
        /* 不致命, 可使用本地时间 */
    }

    /* ========== 第5步: 初始化硬件 (GPIO/UART) ========== */
    LOG_INFO("MAIN", "Initializing hardware (GPIO + UART)...");
    
    ret = hardware_init_gpio();
    if (ret != APP_ERR_OK) {
        LOG_ERROR("MAIN", "GPIO init failed (err=%d)", ret);
        return;  /* 致命错误: 无法控制ADXL345 */
    }
    LOG_INFO("MAIN", "GPIO initialized OK");

    ret = hardware_init_uart();
    if (ret != APP_ERR_OK) {
        LOG_ERROR("MAIN", "UART init failed (err=%d)", ret);
        return;  /* 致命错误: 无法通信STM32 */
    }
    LOG_INFO("MAIN", "UART initialized OK");

    /* ========== 第6步: 初始化协议栈 (UART通信) ========== */
    LOG_INFO("MAIN", "Initializing protocol stack...");
    
    ret = component_init_protocol();
    if (ret != APP_ERR_OK) {
        LOG_ERROR("MAIN", "Protocol init failed (err=%d)", ret);
        return;  /* 致命错误: 无法接收STM32数据 */
    }
    LOG_INFO("MAIN", "Protocol stack initialized OK");

    /* ========== 第7步: 初始化传感器服务 ========== */
    LOG_INFO("MAIN", "Initializing sensor service...");
    
    ret = component_init_sensor();
    if (ret != APP_ERR_OK) {
        LOG_ERROR("MAIN", "Sensor service init failed (err=%d)", ret);
        return;  /* 致命错误: 无法采集振动数据 */
    }
    LOG_INFO("MAIN", "Sensor service initialized OK");

    /* ========== 第8步: 初始化 MQTT 通信 ========== */
    LOG_INFO("MAIN", "Initializing MQTT module (connecting to broker)...");
    
    ret = component_init_mqtt();
    if (ret != APP_ERR_OK) {
        LOG_ERROR("MAIN", "MQTT init failed (err=%d), running in offline mode", ret);
        /* 不致命: 可以离线运行, 数据缓存在RingBuffer中 */
    } else {
        LOG_INFO("MAIN", "MQTT module initialized OK");
    }

    /* ========== 第9步: 创建 FreeRTOS 队列 ========== */

    /* Q1: 温湿度数据队列 (UART→Sensor) */
    g_ctx.q_temp_data = xQueueCreate(QUEUE_DEPTH_TEMP_DATA,
                                      sizeof(struct temp_humidity_data));
    if (!g_ctx.q_temp_data) {
        LOG_ERROR("MAIN", "Failed to create temp data queue!");
        return;
    }
    LOG_INFO("MAIN", "Created temp data queue (depth=%d, item_size=%u bytes)",
             QUEUE_DEPTH_TEMP_DATA, sizeof(struct temp_humidity_data));

    /* Q2: 分析结果队列 (Sensor→MQTT) */
    g_ctx.q_analysis_result = xQueueCreate(QUEUE_DEPTH_ANALYSIS,
                                            sizeof(struct analysis_result));
    if (!g_ctx.q_analysis_result) {
        LOG_ERROR("MAIN", "Failed to create analysis result queue!");
        return;
    }
    LOG_INFO("MAIN", "Created analysis result queue (depth=%d, item_size=%u bytes)",
             QUEUE_DEPTH_ANALYSIS, sizeof(struct analysis_result));

    /* ========== 第10步: 设置运行状态 ========== */
    g_ctx.system_running = true;
    g_ctx.uart_connected = false;
    g_ctx.error_count = 0;
    g_ctx.uptime_start_us = (int64_t)esp_timer_get_time();
    memset(&g_ctx.stats, 0, sizeof(g_ctx.stats));

    /* ========== 第11步: 创建 FreeRTOS 任务 ========== */

    /* 任务1: UART 接收监控 (优先级7, 最高) */
    BaseType_t task_ret = xTaskCreate(
        task_uart_rx,
        "uart_rx_task",
        TASK_STACK_UART_RX,
        NULL,
        TASK_PRIORITY_UART_RX,
        &g_ctx.task_uart_rx
    );
    if (task_ret != pdPASS) {
        LOG_ERROR("MAIN", "Failed to create UART RX task! (heap=%u)", esp_get_free_heap_size());
        return;
    }

    /* 任务2: 传感器数据处理 (优先级6) — 非致命! */
    task_ret = xTaskCreate(
        task_sensor_process,
        "sensor_task",
        TASK_STACK_SENSOR,
        NULL,
        TASK_PRIORITY_SENSOR,
        &g_ctx.task_sensor
    );
    if (task_ret != pdPASS) {
        LOG_ERROR("MAIN", "Failed to create sensor task (heap=%u) — continuing without it",
                 esp_get_free_heap_size());
        g_ctx.task_sensor = NULL;
    }

    /* 任务3: MQTT 数据上传 (优先级5) — 必须创建! */
    task_ret = xTaskCreate(
        task_mqtt_upload,
        "mqtt_upload",
        TASK_STACK_MQTT,
        NULL,
        TASK_PRIORITY_MQTT,
        &g_ctx.task_mqtt
    );
    if (task_ret != pdPASS) {
        LOG_ERROR("MAIN", "Failed to create MQTT task! (heap=%u)", esp_get_free_heap_size());
        return;
    }

    /* 任务4: 系统监控 (优先级2) — 非致命 */
    task_ret = xTaskCreate(
        task_system_monitor,
        "monitor_task",
        TASK_STACK_MONITOR,
        NULL,
        TASK_PRIORITY_MONITOR,
        &g_ctx.task_monitor
    );
    if (task_ret != pdPASS) {
        LOG_WARN("MAIN", "Failed to create monitor task — continuing without it");
        g_ctx.task_monitor = NULL;
    }

    /* ========== 启动完成, 打印摘要 ========== */
    LOG_INFO("MAIN", "========================================");
    LOG_INFO("MAIN", "  System Initialization Completed!");
    LOG_INFO("MAIN", "========================================");
    LOG_INFO("MAIN", "  Tasks created:");
    LOG_INFO("MAIN", "    - uart_rx_task    (priority=%d)", TASK_PRIORITY_UART_RX);
    LOG_INFO("MAIN", "    - sensor_task     (priority=%d)", TASK_PRIORITY_SENSOR);
    LOG_INFO("MAIN", "    - mqtt_upload     (priority=%d)", TASK_PRIORITY_MQTT);
    LOG_INFO("MAIN", "    - monitor_task    (priority=%d)", TASK_PRIORITY_MONITOR);
    LOG_INFO("MAIN", "  Queues created:");
    LOG_INFO("MAIN", "    - q_temp_data     (depth=%d)", QUEUE_DEPTH_TEMP_DATA);
    LOG_INFO("MAIN", "    - q_analysis      (depth=%d)", QUEUE_DEPTH_ANALYSIS);
    LOG_INFO("MAIN", "  Hardware config:");
    LOG_INFO("MAIN", "    - UART%d: TX=%d, RX=%d, %dbps",
             UART_STM32_NUM, UART_STM32_TX_PIN, UART_STM32_RX_PIN,
             UART_STM32_BAUD_RATE);
    LOG_INFO("MAIN", "    - ADXL345: CS=%d, INT1=%d",
             ADXL345_SPI_CS_PIN, ADXL345_INT1_PIN);
    LOG_INFO("MAIN", "========================================");

    /* 
     * 主循环结束, 控制权交给 FreeRTOS 调度器
     * 
     * 重要提示:
     *   - app_main 返回后, 空闲任务 (idle task) 会自动运行
     *   - 所有业务逻辑在上述4个任务中执行
     *   - 此函数不应返回 (除非发生致命错误导致提前return)
     */
}
