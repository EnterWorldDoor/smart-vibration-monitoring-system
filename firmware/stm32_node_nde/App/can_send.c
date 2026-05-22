/*
 * App/can_send.c — NDE CAN协议层实现 (CRC8 + 17帧格式)
 *
 * 特征帧布局 (每帧8字节, data[7]=CRC8 over data[0..6]):
 *   帧0:  [seq=0] [window_idx] [feat[0..4] 5字节] [CRC8]
 *   帧1-15: [seq=N] [feat字节 6个]              [CRC8]
 *   帧16: [seq=16] [feat[95] + 0x00×5]          [CRC8]
 *
 * 心跳帧布局 (4字节, data[3]=CRC8 over data[0..2]):
 *   [online] [error_count] [temp_c] [CRC8]
 */

#include "can_send.h"
#include "bsp/can/bsp_can.h"
#include "global_error.h"
#include <string.h>

#define BSP_LOG_ENABLE  1
#include "bsp/bsp_log.h"

int can_send_init(void)
{
	return bsp_can_init();
}

int can_send_features(const float *features, uint8_t window_idx)
{
	const uint8_t *feat_bytes;
	uint8_t data[8];
	int ret;
	int seq;

	if (!features)
		return ERR_INVALID_PARAM;

	feat_bytes = (const uint8_t *)features;

	/* 帧0: seq=0, window_idx, feat[0..4], CRC8 */
	data[0] = 0;
	data[1] = window_idx;
	memcpy(&data[2], &feat_bytes[0], 5);
	data[7] = bsp_can_crc8(data, 7);
	ret = bsp_can_send_frame(CAN_NDE_FEATURE_ID, data, 8);
	if (ret < 0)
		goto err;

	/* 帧1-15: seq=N, feat[5+N*6..10+N*6], CRC8 */
	for (seq = 1; seq < 16; seq++) {
		uint16_t src_off;

		data[0] = (uint8_t)seq;
		src_off = (uint16_t)(5 + (seq - 1) * 6);
		memcpy(&data[1], &feat_bytes[src_off], 6);
		data[7] = bsp_can_crc8(data, 7);
		ret = bsp_can_send_frame(CAN_NDE_FEATURE_ID, data, 8);
		if (ret < 0)
			goto err;
	}

	/* 帧16: seq=16, feat[95] + 零填充, CRC8 */
	data[0] = 16;
	data[1] = feat_bytes[95];
	memset(&data[2], 0x00, 5);
	data[7] = bsp_can_crc8(data, 7);
	ret = bsp_can_send_frame(CAN_NDE_FEATURE_ID, data, 8);
	if (ret < 0)
		goto err;

	return 0;

err:
	pr_warn("CAN feature send failed at seq=%d\n", seq);
	return ret;
}

int can_send_heartbeat(uint8_t online, uint8_t error_count, int8_t temp_c)
{
	uint8_t data[4];
	int ret;

	data[0] = online ? 0x01 : 0x00;
	data[1] = error_count;
	data[2] = (uint8_t)temp_c;
	data[3] = bsp_can_crc8(data, 3);

	ret = bsp_can_send_frame(CAN_NDE_HEARTBEAT_ID, data, 4);
	if (ret < 0) {
		pr_warn("CAN heartbeat send failed\n");
		return ret;
	}

	return 0;
}
