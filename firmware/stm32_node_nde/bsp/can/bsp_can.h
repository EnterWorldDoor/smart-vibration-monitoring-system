/*
 * bsp/can/bsp_can.h — CAN HAL封装 + CRC8
 *
 * CAN: PB8(RX)/PB9(TX), 重映射, 500kbps
 * CRC: CRC-8-Dallas/Maxim, 多项式 0x31
 */

#ifndef __BSP_CAN_H
#define __BSP_CAN_H

#include <stdint.h>

/* CAN ID */
#define CAN_NDE_FEATURE_ID      0x201
#define CAN_NDE_HEARTBEAT_ID    0x202

/* 特征帧: 17帧/批次 (含CRC8) */
#define CAN_NDE_FRAME_COUNT     17
#define CAN_NDE_FEAT_PER_FRAME  6
#define CAN_NDE_FEATURE_BYTES   96

int bsp_can_init(void);

/*
 * 发送一帧CAN数据.
 * can_id: 标准11-bit ID
 * data:   数据缓冲区
 * dlc:    数据长度 (0-8)
 * 返回: 0=成功, 负errno
 */
int bsp_can_send_frame(uint32_t can_id, const uint8_t *data, uint8_t dlc);

/*
 * CRC-8-Dallas/Maxim (x^8 + x^5 + x^4 + 1, 多项式 0x31).
 * 初始值0x00, 不反转输入/输出.
 * 用于每帧CRC8校验 (覆盖seq + payload共7字节).
 */
uint8_t bsp_can_crc8(const uint8_t *data, uint8_t len);

#endif /* __BSP_CAN_H */
