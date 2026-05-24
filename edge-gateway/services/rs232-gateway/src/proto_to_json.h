/**
 * @file proto_to_json.h
 * @brief RS232 Gateway — 协议帧 → JSON + MQTT Topic 路由
 */

#ifndef PROTO_TO_JSON_H
#define PROTO_TO_JSON_H

#include <stdint.h>
#include <stddef.h>

#define PROTO_JSON_MAX_SIZE  4096
#define PROTO_TOPIC_MAX_SIZE  256

/**
 * 将协议帧 payload 转换为 JSON 并确定 MQTT topic。
 *
 * @param cmd           协议命令字节
 * @param dev_id        帧中的设备 ID
 * @param payload       原始数据载荷
 * @param payload_len   载荷长度 (字节)
 * @param json_out      输出 JSON 缓冲区 (调用者分配)
 * @param json_size     json_out 缓冲区大小
 * @param json_len      输出: 实际 JSON 长度 (不含 null 终止符)
 * @param topic_out     输出 MQTT topic 缓冲区 (调用者分配)
 * @param topic_size    topic_out 缓冲区大小
 * @param topic_prefix  MQTT topic 前缀 (来自配置文件)
 *
 * @return 0=成功, -1=未知CMD(已生成通用格式), -2=payload太短, -3=缓冲区溢出
 */
int proto_to_json(uint8_t cmd, uint8_t dev_id,
                  const uint8_t *payload, uint16_t payload_len,
                  char *json_out, size_t json_size, size_t *json_len,
                  char *topic_out, size_t topic_size,
                  const char *topic_prefix);

#endif /* PROTO_TO_JSON_H */
