/**
 * @file can_nde.h
 * @brief NDE传感器节点CAN接收与多帧重组
 *
 * 接收F103通过CAN总线发来的NDE特征向量(0x201)和心跳帧(0x202)。
 * 16帧→96字节特征向量重组，完成后通过proto_build_nde_feature_frame
 * 封装为UART 0x15帧上行给ESP32。
 */

#ifndef __CAN_NDE_H
#define __CAN_NDE_H

#include <stdint.h>
#include <stdbool.h>

#define CAN_NDE_FEATURE_ID      0x201
#define CAN_NDE_HEARTBEAT_ID    0x202
#define CAN_NDE_FRAME_COUNT     17
#define CAN_NDE_FEAT_PER_FRAME  6
#define CAN_NDE_FEATURE_BYTES   96
#define CAN_NDE_BATCH_TIMEOUT_MS 100

typedef void (*can_nde_feature_callback_t)(const float *features, uint8_t window_idx);
typedef void (*can_nde_heartbeat_callback_t)(uint8_t online, uint8_t errors, int8_t temp_c);

int can_nde_init(void);
void can_nde_set_callbacks(can_nde_feature_callback_t feat_cb,
                           can_nde_heartbeat_callback_t hb_cb);
void can_nde_poll_timeout(void);

#endif /* __CAN_NDE_H */
