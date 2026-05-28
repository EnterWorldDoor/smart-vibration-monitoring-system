/**
 * @file mqtt_pub.c
 * @brief RS232 Gateway — libmosquitto 薄封装 (仅发布)
 *
 * 使用 libmosquitto 异步 API:
 *   - mosquitto_connect_async() + mosquitto_loop_start()
 *   - 自动重连由 libmosquitto 内部处理
 *   - QoS 1 发布, 不保留消息
 */

#include "mqtt_pub.h"
#include <mosquitto.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ==================== 订阅管理 ==================== */

#define MAX_SUBSCRIPTIONS 16

struct subscription {
	char            topic[256];
	int             qos;
	mqtt_msg_cb_t   callback;
	void           *user_data;
	int             mid;         /* mosquitto subscription message ID */
};

static struct subscription g_subs[MAX_SUBSCRIPTIONS];
static int                 g_sub_count;

/* ==================== 内部状态 ==================== */

static struct mosquitto *g_mosq = NULL;
static int g_connected = 0;
static int g_qos = 1;

/* ==================== libmosquitto 回调 ==================== */

static void on_connect(struct mosquitto *mosq, void *obj, int rc)
{
    (void)mosq;
    (void)obj;

    if (rc == 0) {
        g_connected = 1;
        fprintf(stdout, "[%ld] INFO  rs232-gw: MQTT connected to broker\n",
                (long)time(NULL));

        /* 重新订阅所有已注册主题 */
        for (int i = 0; i < g_sub_count; i++) {
            int ret = mosquitto_subscribe(g_mosq, &g_subs[i].mid,
                                          g_subs[i].topic, g_subs[i].qos);
            if (ret == MOSQ_ERR_SUCCESS) {
                fprintf(stdout, "[%ld] INFO  rs232-gw: MQTT re-subscribed to '%s'\n",
                        (long)time(NULL), g_subs[i].topic);
            }
        }
    } else {
        g_connected = 0;
        fprintf(stderr, "[%ld] WARN  rs232-gw: MQTT connect failed (rc=%d): %s\n",
                (long)time(NULL), rc, mosquitto_connack_string(rc));
    }
}

static void on_disconnect(struct mosquitto *mosq, void *obj, int rc)
{
    (void)mosq;
    (void)obj;

    g_connected = 0;
    if (rc != 0) {
        fprintf(stderr, "[%ld] WARN  rs232-gw: MQTT disconnected (rc=%d), auto-reconnecting...\n",
                (long)time(NULL), rc);
    }
}

static void on_publish(struct mosquitto *mosq, void *obj, int mid)
{
    (void)mosq;
    (void)obj;
    (void)mid;
}

static void on_message(struct mosquitto *mosq, void *obj,
		       const struct mosquitto_message *msg)
{
    (void)mosq;
    (void)obj;

    if (!msg || !msg->topic)
        return;

    for (int i = 0; i < g_sub_count; i++) {
        if (strcmp(g_subs[i].topic, msg->topic) == 0 &&
            g_subs[i].callback) {
            g_subs[i].callback(msg->topic, msg->payload,
                               (size_t)msg->payloadlen,
                               g_subs[i].user_data);
        }
    }
}

/* ==================== URL 解析 ==================== */

static int parse_broker_url(const char *url, char *host, size_t host_sz,
                            int *port)
{
    /* 格式: tcp://host:port */
    const char *p = url ? strstr(url, "://") : NULL;
    if (!p) {
        snprintf(host, host_sz, "%s", url ? url : "localhost");
        *port = 1883;
        return 0;
    }
    p += 3;
    const char *colon = strchr(p, ':');
    if (colon) {
        size_t len = (size_t)(colon - p);
        if (len >= host_sz) len = host_sz - 1;
        memcpy(host, p, len);
        host[len] = '\0';
        *port = atoi(colon + 1);
        if (*port <= 0) *port = 1883;
    } else {
        snprintf(host, host_sz, "%s", p);
        *port = 1883;
    }
    return 0;
}

/* ==================== 公开 API ==================== */

int mqtt_pub_init(const struct mqtt_pub_config *cfg)
{
    if (!cfg || !cfg->broker_url || !cfg->client_id)
        return -1;

    mosquitto_lib_init();

    g_qos = cfg->qos;
    g_mosq = mosquitto_new(cfg->client_id, true, NULL);
    if (!g_mosq) {
        fprintf(stderr, "[%ld] ERROR rs232-gw: mosquitto_new failed\n",
                (long)time(NULL));
        return -1;
    }

    mosquitto_connect_callback_set(g_mosq, on_connect);
    mosquitto_disconnect_callback_set(g_mosq, on_disconnect);
    mosquitto_publish_callback_set(g_mosq, on_publish);
    mosquitto_message_callback_set(g_mosq, on_message);

    char host[256];
    int port;
    parse_broker_url(cfg->broker_url, host, sizeof(host), &port);

    int keepalive = cfg->keepalive_sec > 0 ? cfg->keepalive_sec : 60;
    int rc = mosquitto_connect_async(g_mosq, host, port, keepalive);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[%ld] ERROR rs232-gw: mosquitto_connect_async failed: %s\n",
                (long)time(NULL), mosquitto_strerror(rc));
        mosquitto_destroy(g_mosq);
        g_mosq = NULL;
        mosquitto_lib_cleanup();
        return -1;
    }

    rc = mosquitto_loop_start(g_mosq);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[%ld] ERROR rs232-gw: mosquitto_loop_start failed: %s\n",
                (long)time(NULL), mosquitto_strerror(rc));
        mosquitto_destroy(g_mosq);
        g_mosq = NULL;
        mosquitto_lib_cleanup();
        return -1;
    }

    return 0;
}

int mqtt_pub_send(const char *topic, const void *payload, size_t len)
{
    if (!g_mosq || !topic || !payload)
        return -1;

    int rc = mosquitto_publish(g_mosq, NULL, topic, (int)len, payload,
                               g_qos, false);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[%ld] WARN  rs232-gw: mqtt_pub_send failed (topic=%s): %s\n",
                (long)time(NULL), topic, mosquitto_strerror(rc));
        return -1;
    }
    return 0;
}

int mqtt_pub_subscribe(const char *topic, int qos, mqtt_msg_cb_t callback,
                       void *user_data)
{
    if (!g_mosq || !topic || !callback)
        return -1;

    if (g_sub_count >= MAX_SUBSCRIPTIONS) {
        fprintf(stderr, "[%ld] ERROR rs232-gw: max subscriptions (%d) reached\n",
                (long)time(NULL), MAX_SUBSCRIPTIONS);
        return -1;
    }

    struct subscription *s = &g_subs[g_sub_count];
    snprintf(s->topic, sizeof(s->topic), "%s", topic);
    s->qos = qos;
    s->callback = callback;
    s->user_data = user_data;

    if (g_connected) {
        int ret = mosquitto_subscribe(g_mosq, &s->mid, topic, qos);
        if (ret != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "[%ld] ERROR rs232-gw: subscribe '%s' failed: %s\n",
                    (long)time(NULL), topic, mosquitto_strerror(ret));
            return -1;
        }
    }

    g_sub_count++;

    fprintf(stdout, "[%ld] INFO  rs232-gw: MQTT subscribed to '%s' (qos=%d)\n",
            (long)time(NULL), topic, qos);
    return 0;
}

int mqtt_pub_is_connected(void)
{
    return g_connected;
}

void mqtt_pub_deinit(void)
{
    if (g_mosq) {
        mosquitto_disconnect(g_mosq);
        mosquitto_loop_stop(g_mosq, false);
        mosquitto_destroy(g_mosq);
        g_mosq = NULL;
    }
    mosquitto_lib_cleanup();
    g_connected = 0;
}
