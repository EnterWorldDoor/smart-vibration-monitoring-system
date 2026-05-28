/**
 * @file main.c
 * @brief RS232 Gateway — 主入口
 *
 * YAML 配置加载 → 模块初始化 → 主循环 (串口读取→解析→MQTT发布)
 * 信号处理 (SIGTERM/SIGINT 优雅关闭)
 * 错误恢复: 进程绝不对可恢复错误退出, systemd 处理致命崩溃
 */

#include "protocol.h"
#include "serial.h"
#include "mqtt_pub.h"
#include "proto_to_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <yaml.h>

/* ==================== 日志宏 ==================== */

#define log_info(fmt, ...) \
    fprintf(stdout, "[%ld] INFO  rs232-gw: " fmt "\n", (long)time(NULL), ##__VA_ARGS__)

#define log_warn(fmt, ...) \
    fprintf(stderr, "[%ld] WARN  rs232-gw: " fmt "\n", (long)time(NULL), ##__VA_ARGS__)

#define log_error(fmt, ...) \
    fprintf(stderr, "[%ld] ERROR rs232-gw: " fmt "\n", (long)time(NULL), ##__VA_ARGS__)

/* ==================== 应用配置 ==================== */

struct app_config {
    struct serial_config   serial;
    struct mqtt_pub_config mqtt;
    char                   topic_prefix[256];
    int                    heartbeat_timeout_ms;
    int                    reconnect_delay_sec;
};

/* ==================== 全局状态 ==================== */

static volatile sig_atomic_t g_running = 1;

/* ==================== 主/备切换状态机 ==================== */

/*
 * F407 UART4 (主通道, 经 ESP32 WiFi 中转) vs UART5/RS232 (备通道, 直连)
 * 切换策略: UART4 任一 CRC 合法帧 = 存活; 3s 无帧切 UART5; 5s 恢复切回
 * 滞回 5s > 3s 防乒乓切换
 */

#define BACKUP_SWITCH_TIMEOUT_MS   3000
#define BACKUP_RECOVER_TIMEOUT_MS  5000

static int  g_backup_active;
static long g_last_uart4_alive_ms;

static long current_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void update_uart4_heartbeat(void)
{
    g_last_uart4_alive_ms = current_time_ms();
}

static int is_uart4_alive(void)
{
    return (current_time_ms() - g_last_uart4_alive_ms) < BACKUP_SWITCH_TIMEOUT_MS;
}

/* ==================== OTA 下行处理 ==================== */

#define OTA_DOWNLINK_TOPIC  "EdgeVib/+/ota/f407/downlink"
#define OTA_UPLINK_TOPIC    "EdgeVib/factory1/ota/f407/uplink"

static void on_ota_downlink(const char *topic, const void *payload,
                            size_t len, void *user_data)
{
    int serial_fd = *(int *)user_data;

    log_info("OTA downlink received (topic=%s, len=%zu)", topic, len);

    /* 将下行数据转发到 F407 UART5 串口 */
    if (serial_fd >= 0 && payload && len > 0) {
        int written = serial_write(serial_fd, (const uint8_t *)payload, len);
        if (written < 0) {
            log_warn("OTA downlink serial_write failed");
        } else {
            log_info("OTA downlink forwarded %d bytes to F407 UART5", written);
        }
    }
}

/* ==================== 信号处理 ==================== */

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ==================== 解析器回调 ==================== */

static void on_frame_parsed(uint8_t cmd, uint8_t dev_id,
                            const uint8_t *data, uint16_t len,
                            void *user_data)
{
    /*
     * 任一 CRC 合法帧到达即表示 UART4 (主通道 ESP32) 存活。
     * 更新心跳时间戳供主/备切换状态机使用。
     */
    update_uart4_heartbeat();

    const char *prefix = (const char *)user_data;
    char json[PROTO_JSON_MAX_SIZE];
    char topic[PROTO_TOPIC_MAX_SIZE];
    size_t json_len = 0;

    int ret = proto_to_json(cmd, dev_id, data, len,
                            json, sizeof(json), &json_len,
                            topic, sizeof(topic), prefix);
    if (ret < -1) {
        log_warn("proto_to_json failed for CMD 0x%02X (ret=%d)", cmd, ret);
        return;
    }

    if (json_len > 0 && json_len < sizeof(json))
        json[json_len] = '\0';
    else
        json[sizeof(json) - 1] = '\0';

    int mq_ret = mqtt_pub_send(topic, json, json_len);
    if (mq_ret < 0)
        log_warn("mqtt_pub_send failed (topic=%s)", topic);
}

/* ==================== YAML 解析 ==================== */

/*
 * 简单的 libyaml 事件解析器。
 * 配置文件结构 (扁平 key-value):
 *   serial:
 *     port: "/dev/ttyUSB0"
 *     baudrate: 115200
 *     ...
 *   mqtt:
 *     broker_url: "tcp://localhost:1883"
 *     ...
 */

static int load_config(const char *path, struct app_config *cfg)
{
    FILE *fh = fopen(path, "r");
    if (!fh) {
        fprintf(stderr, "ERROR: cannot open config file '%s': %s\n",
                path, strerror(errno));
        return -1;
    }

    yaml_parser_t parser;
    yaml_token_t  token;
    int done = 0;
    int error = 0;

    char current_section[32] = "";
    char current_key[64] = "";

    yaml_parser_initialize(&parser);
    yaml_parser_set_input_file(&parser, fh);

    enum { S_TOP, S_SECTION, S_KEY, S_VALUE } state = S_TOP;

    while (!done) {
        yaml_parser_scan(&parser, &token);

        switch (token.type) {
        case YAML_STREAM_START_TOKEN:
            break;
        case YAML_STREAM_END_TOKEN:
            done = 1;
            break;

        case YAML_BLOCK_MAPPING_START_TOKEN:
            break;

        case YAML_BLOCK_END_TOKEN:
            state = S_TOP;
            current_section[0] = '\0';
            break;

        case YAML_KEY_TOKEN:
            if (state == S_TOP || state == S_SECTION) {
                state = S_SECTION;
            } else if (state == S_KEY || state == S_VALUE) {
                state = S_KEY;
            }
            break;

        case YAML_VALUE_TOKEN:
            state = S_VALUE;
            break;

        case YAML_SCALAR_TOKEN:
            {
                const char *s = (const char *)token.data.scalar.value;
                size_t s_len = token.data.scalar.length;

                if (state == S_SECTION) {
                    /* Section name */
                    snprintf(current_section, sizeof(current_section), "%.*s",
                             (int)s_len, s);
                    state = S_TOP;
                } else if (state == S_KEY) {
                    /* Key name */
                    snprintf(current_key, sizeof(current_key), "%.*s",
                             (int)s_len, s);
                } else if (state == S_VALUE) {
                    /* Value */
                    char val[256];
                    snprintf(val, sizeof(val), "%.*s", (int)s_len, s);

                    if (strcmp(current_section, "serial") == 0) {
                        if (strcmp(current_key, "port") == 0)
                            cfg->serial.port = strdup(val);
                        else if (strcmp(current_key, "baudrate") == 0)
                            cfg->serial.baudrate = atoi(val);
                        else if (strcmp(current_key, "data_bits") == 0)
                            cfg->serial.data_bits = atoi(val);
                        else if (strcmp(current_key, "stop_bits") == 0)
                            cfg->serial.stop_bits = atoi(val);
                        else if (strcmp(current_key, "parity") == 0)
                            cfg->serial.parity = val[0];
                        else if (strcmp(current_key, "timeout_ms") == 0)
                            cfg->serial.timeout_ms = atoi(val);
                    } else if (strcmp(current_section, "mqtt") == 0) {
                        if (strcmp(current_key, "broker_url") == 0)
                            cfg->mqtt.broker_url = strdup(val);
                        else if (strcmp(current_key, "client_id") == 0)
                            cfg->mqtt.client_id = strdup(val);
                        else if (strcmp(current_key, "publish_topic_prefix") == 0) {
                            snprintf(cfg->topic_prefix, sizeof(cfg->topic_prefix),
                                     "%s", val);
                        }
                    } else if (strcmp(current_section, "heartbeat") == 0) {
                        if (strcmp(current_key, "peer_timeout_ms") == 0)
                            cfg->heartbeat_timeout_ms = atoi(val);
                    }
                    state = S_KEY;
                }
            }
            break;

        default:
            break;
        }

        if (token.type != YAML_STREAM_END_TOKEN)
            yaml_token_delete(&token);

        if (error)
            break;
    }

    yaml_token_delete(&token);
    yaml_parser_delete(&parser);
    fclose(fh);

    return 0;
}

/* ==================== 主函数 ==================== */

int main(int argc, char **argv)
{
    const char *config_path = (argc > 1) ? argv[1] : "config/rs232-gateway.yaml";

    /* ---- 默认配置 ---- */
    struct app_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.serial.port       = "/dev/ttyUSB0";
    cfg.serial.baudrate   = 115200;
    cfg.serial.data_bits  = 8;
    cfg.serial.stop_bits  = 1;
    cfg.serial.parity     = 'N';
    cfg.serial.timeout_ms = 100;
    cfg.mqtt.broker_url   = "tcp://localhost:1883";
    cfg.mqtt.client_id    = "edgevib-rs232-gateway";
    cfg.mqtt.keepalive_sec = 60;
    cfg.mqtt.qos          = 1;
    snprintf(cfg.topic_prefix, sizeof(cfg.topic_prefix), "EdgeVib/factory1/motor");
    cfg.heartbeat_timeout_ms = 3000;
    cfg.reconnect_delay_sec  = 2;

    /* ---- 加载配置 ---- */
    if (load_config(config_path, &cfg) != 0) {
        log_error("config load failed, exiting");
        return EXIT_FAILURE;
    }

    log_info("rs232-gateway starting (config: %s)", config_path);
    log_info("  serial: port=%s baud=%d data=%d stop=%d parity=%c timeout=%d",
             cfg.serial.port, cfg.serial.baudrate, cfg.serial.data_bits,
             cfg.serial.stop_bits, cfg.serial.parity, cfg.serial.timeout_ms);
    log_info("  mqtt: broker=%s client=%s prefix=%s",
             cfg.mqtt.broker_url, cfg.mqtt.client_id, cfg.topic_prefix);

    /* ---- 信号处理 ---- */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* ---- 初始化协议解析器 ---- */
    if (proto_parse_init() != 0) {
        log_error("proto_parse_init failed");
        return EXIT_FAILURE;
    }

    /* ---- 注册 CMD 回调 ---- */
    proto_parse_callback_t cb = on_frame_parsed;
    void *user_data = (void *)cfg.topic_prefix;
    for (int i = 0; i < 256; i++) {
        if (i != PROTO_CMD_HEARTBEAT)
            proto_parse_register((uint8_t)i, cb, user_data);
    }

    /* ---- 初始化 MQTT ---- */
    if (mqtt_pub_init(&cfg.mqtt) != 0) {
        log_warn("mqtt_pub_init failed, will retry on publish");
    }

    /* ---- 初始化主/备切换状态机 ---- */
    g_backup_active = 0;
    g_last_uart4_alive_ms = current_time_ms();

    /* ---- 主循环 ---- */
    int serial_fd = -1;
    int serial_fd_backup = -1;  /* UART5 (RS232 备份通道) */
    uint8_t rx_buf[256];

    /* ---- 订阅 OTA 下行命令 ---- */
    {
        mqtt_pub_subscribe(OTA_DOWNLINK_TOPIC, 1, on_ota_downlink, &serial_fd_backup);
        log_info("subscribed to OTA downlink: %s", OTA_DOWNLINK_TOPIC);
    }

    while (g_running) {
        /* 打开串口 (失败则重试) */
        if (serial_fd < 0) {
            serial_fd = serial_open(&cfg.serial);
            if (serial_fd < 0) {
                log_warn("serial_open(%s) failed, retrying in %ds...",
                         cfg.serial.port, cfg.reconnect_delay_sec);
                sleep((unsigned)cfg.reconnect_delay_sec);
                continue;
            }
            log_info("serial port opened: fd=%d", serial_fd);
        }

        /* 读取数据 */
        int n = serial_read(serial_fd, rx_buf, sizeof(rx_buf),
                            cfg.serial.timeout_ms);

        if (n < 0) {
            if (serial_is_recoverable_error(serial_fd)) {
                log_warn("serial device disconnected, closing...");
                serial_close(serial_fd);
                serial_fd = -1;
            } else {
                log_error("serial_read error: %s", strerror(errno));
            }
            continue;
        }

        if (n == 0)
            continue;

        /* 喂入解析器 (回调中自动更新 UART4 心跳) */
        proto_parse_feed_buf(rx_buf, (uint16_t)n);

        /* ---- 主/备切换状态机 ---- */
        {
            int was_backup = g_backup_active;

            if (is_uart4_alive()) {
                /* UART4 主通道正常 */
                if (g_backup_active) {
                    long elapsed = current_time_ms() - g_last_uart4_alive_ms;
                    if (elapsed < 0 || elapsed >= BACKUP_RECOVER_TIMEOUT_MS) {
                        /* 实际上刚恢复, elapsed < timeout 表示 alive 刚成立 */
                    }
                    /* 连续 5s 以上 → 切回主通道 */
                    if (elapsed >= BACKUP_RECOVER_TIMEOUT_MS) {
                        g_backup_active = 0;
                        log_info("master/backup: UART4 recovered, switching BACK to UART4 (primary)");
                    }
                }
            } else {
                /* UART4 主通道失联 */
                if (!g_backup_active) {
                    long elapsed = current_time_ms() - g_last_uart4_alive_ms;
                    if (elapsed >= BACKUP_SWITCH_TIMEOUT_MS) {
                        g_backup_active = 1;
                        log_warn("master/backup: UART4 lost for %ldms, switching TO UART5 (backup)",
                                 elapsed);
                    }
                }
            }

            /* 状态变化时上报 MQTT */
            if (was_backup != g_backup_active) {
                char msg[128];
                snprintf(msg, sizeof(msg),
                         "{\"backup_active\":%d,\"uart4_alive\":%d,\"timestamp_ms\":%ld}",
                         g_backup_active, is_uart4_alive(), current_time_ms());
                mqtt_pub_send("EdgeVib/factory1/gateway/rs232-gw/status", msg, strlen(msg));
            }
        }
    }

    /* ---- 优雅关闭 ---- */
    log_info("shutting down...");
    proto_parse_deinit();
    mqtt_pub_deinit();
    if (serial_fd >= 0)
        serial_close(serial_fd);
    log_info("rs232-gateway stopped");

    return EXIT_SUCCESS;
}
