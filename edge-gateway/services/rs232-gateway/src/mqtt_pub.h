/**
 * @file mqtt_pub.h
 * @brief RS232 Gateway — libmosquitto MQTT 发布者 API
 */

#ifndef MQTT_PUB_H
#define MQTT_PUB_H

#include <stddef.h>

/* MQTT 消息回调类型 (订阅用) */
typedef void (*mqtt_msg_cb_t)(const char *topic, const void *payload,
			      size_t len, void *user_data);

struct mqtt_pub_config {
    const char *broker_url;    /* e.g. "tcp://localhost:1883" */
    const char *client_id;     /* e.g. "edgevib-rs232-gateway" */
    int         keepalive_sec; /* e.g. 60 */
    int         qos;           /* e.g. 1 */
};

/* 初始化 MQTT 客户端 (libmosquitto init + 异步连接)
 * 返回 0 成功, -1 失败 */
int mqtt_pub_init(const struct mqtt_pub_config *cfg);

/* 发布消息。返回 0 成功, -1 失败。
 * 内置自动重连, libmosquitto 内部排队处理。 */
int mqtt_pub_send(const char *topic, const void *payload, size_t len);

/* 检查连接状态。返回 1 已连接, 0 未连接。 */
int mqtt_pub_is_connected(void);

/* 订阅主题。连接后自动重新订阅。
 * 返回 0 成功, -1 失败。 */
int mqtt_pub_subscribe(const char *topic, int qos, mqtt_msg_cb_t callback,
		       void *user_data);

/* 优雅关闭: 断开连接并清理。 */
void mqtt_pub_deinit(void);

#endif /* MQTT_PUB_H */
