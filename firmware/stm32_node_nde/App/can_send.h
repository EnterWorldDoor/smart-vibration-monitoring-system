/*
 * App/can_send.h — NDE CAN协议层接口
 *
 * 将24维特征向量(96字节)封装为17帧CRC8格式通过bsp_can发送.
 * 心跳帧每1s发送, 含online状态+错误计数+CRC8.
 */

#ifndef __CAN_SEND_H
#define __CAN_SEND_H

#include <stdint.h>

#include "bsp/can/bsp_can.h"

int can_send_init(void);

/*
 * 发送24维特征向量 (17帧CRC8格式, CAN ID 0x201).
 * features: 24×float32 = 96字节
 * window_idx: 窗口序号 (0-255, 心跳和日志用)
 * 返回: 0=成功, -EIO=CAN发送失败
 */
int can_send_features(const float *features, uint8_t window_idx);

/*
 * 发送心跳帧 (CAN ID 0x202).
 * online:      1=在线, 0=离线
 * error_count: 累积DSP错误计数
 * temp_c:      温度(无传感器填22)
 * 返回: 0=成功, -EIO=CAN发送失败
 */
int can_send_heartbeat(uint8_t online, uint8_t error_count, int8_t temp_c);

#endif /* __CAN_SEND_H */
