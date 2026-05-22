/**
 * @file can_nde.c
 * @brief NDE CAN接收与多帧重组实现
 *
 * CAN帧格式 (0x201, 16帧/批次, 每2s一批):
 *   byte[0]: seq (frame index 0-15)
 *   byte[1..7]: 6 bytes of feature data
 *   seq=0: byte[1] also carries window_idx (embedded in first data byte)
 *
 * CAN帧格式 (0x202, 1帧/次, 每1s):
 *   byte[0]: online (1=OK, 0=offline)
 *   byte[1]: error_count
 *   byte[2]: temp_c (int8)
 *   byte[3]: reserved
 */

#include "can_nde.h"
#include "can.h"
#include "protocol/protocol.h"
#include "main.h"
#include "system_log/system_log.h"
#include <string.h>
#include "freertos.h"  /* for FreeRTOS API */
#include "wdg/wdg_heartbeat.h"

/*
 * CRC-8-Dallas/Maxim (多项式 0x31 = x^8 + x^5 + x^4 + 1).
 * 与F103 NDE节点 bsp_can.c 完全一致.
 */
static const uint8_t crc8_table[256] = {
	0x00, 0x31, 0x62, 0x53, 0xC4, 0xF5, 0xA6, 0x97,
	0xB9, 0x88, 0xDB, 0xEA, 0x7D, 0x4C, 0x1F, 0x2E,
	0x43, 0x72, 0x21, 0x10, 0x87, 0xB6, 0xE5, 0xD4,
	0xFA, 0xCB, 0x98, 0xA9, 0x3E, 0x0F, 0x5C, 0x6D,
	0x86, 0xB7, 0xE4, 0xD5, 0x42, 0x73, 0x20, 0x11,
	0x3F, 0x0E, 0x5D, 0x6C, 0xFB, 0xCA, 0x99, 0xA8,
	0xC5, 0xF4, 0xA7, 0x96, 0x01, 0x30, 0x63, 0x52,
	0x7C, 0x4D, 0x1E, 0x2F, 0xB8, 0x89, 0xDA, 0xEB,
	0x3D, 0x0C, 0x5F, 0x6E, 0xF9, 0xC8, 0x9B, 0xAA,
	0x84, 0xB5, 0xE6, 0xD7, 0x40, 0x71, 0x22, 0x13,
	0x7E, 0x4F, 0x1C, 0x2D, 0xBA, 0x8B, 0xD8, 0xE9,
	0xC7, 0xF6, 0xA5, 0x94, 0x03, 0x32, 0x61, 0x50,
	0xBB, 0x8A, 0xD9, 0xE8, 0x7F, 0x4E, 0x1D, 0x2C,
	0x02, 0x33, 0x60, 0x51, 0xC6, 0xF7, 0xA4, 0x95,
	0xF8, 0xC9, 0x9A, 0xAB, 0x3C, 0x0D, 0x5E, 0x6F,
	0x41, 0x70, 0x23, 0x12, 0x85, 0xB4, 0xE7, 0xD6,
	0x7A, 0x4B, 0x18, 0x29, 0xBE, 0x8F, 0xDC, 0xED,
	0xC3, 0xF2, 0xA1, 0x90, 0x07, 0x36, 0x65, 0x54,
	0x39, 0x08, 0x5B, 0x6A, 0xFD, 0xCC, 0x9F, 0xAE,
	0x80, 0xB1, 0xE2, 0xD3, 0x44, 0x75, 0x26, 0x17,
	0xFC, 0xCD, 0x9E, 0xAF, 0x38, 0x09, 0x5A, 0x6B,
	0x45, 0x74, 0x27, 0x16, 0x81, 0xB0, 0xE3, 0xD2,
	0xBF, 0x8E, 0xDD, 0xEC, 0x7B, 0x4A, 0x19, 0x28,
	0x06, 0x37, 0x64, 0x55, 0xC2, 0xF3, 0xA0, 0x91,
	0x47, 0x76, 0x25, 0x14, 0x83, 0xB2, 0xE1, 0xD0,
	0xFE, 0xCF, 0x9C, 0xAD, 0x3A, 0x0B, 0x58, 0x69,
	0x04, 0x35, 0x66, 0x57, 0xC0, 0xF1, 0xA2, 0x93,
	0xBD, 0x8C, 0xDF, 0xEE, 0x79, 0x48, 0x1B, 0x2A,
	0xC1, 0xF0, 0xA3, 0x92, 0x05, 0x34, 0x67, 0x56,
	0x78, 0x49, 0x1A, 0x2B, 0xBC, 0x8D, 0xDE, 0xEF,
	0x82, 0xB3, 0xE0, 0xD1, 0x46, 0x77, 0x24, 0x15,
	0x3B, 0x0A, 0x59, 0x68, 0xFF, 0xCE, 0x9D, 0xAC,
};

static uint8_t crc8_compute(const uint8_t *data, uint8_t len)
{
	uint8_t crc = 0x00;
	uint8_t i;

	for (i = 0; i < len; i++)
		crc = crc8_table[crc ^ data[i]];

	return crc;
}

static int s_hb_slot_can = -1;

static struct {
    uint8_t buf[CAN_NDE_FEATURE_BYTES];
    uint32_t seq_map;  /* 17 frames need >16 bits */
    uint8_t window_idx;
    uint32_t last_frame_tick;
    uint8_t active;

    can_nde_feature_callback_t feat_cb;
    can_nde_heartbeat_callback_t hb_cb;
} s_nde;

int can_nde_init(void)
{
    CAN_FilterTypeDef filter;

    memset(&s_nde, 0, sizeof(s_nde));

    /* Filter: accept 0x201 and 0x202 on FIFO0, mask match on ID bits [10:0] */
    filter.FilterBank = 0;
    filter.FilterMode = CAN_FILTERMODE_IDLIST;
    filter.FilterScale = CAN_FILTERSCALE_16BIT;
    filter.FilterIdHigh = (CAN_NDE_FEATURE_ID << 5);
    filter.FilterIdLow = (CAN_NDE_HEARTBEAT_ID << 5);
    filter.FilterMaskIdHigh = 0;
    filter.FilterMaskIdLow = 0;
    filter.FilterFIFOAssignment = CAN_RX_FIFO0;
    filter.FilterActivation = ENABLE;
    filter.SlaveStartFilterBank = 14;

    if (HAL_CAN_ConfigFilter(&hcan1, &filter) != HAL_OK) {
        pr_error_with_tag("CAN_NDE", "Filter config failed\n");
        return -1;
    }

    if (HAL_CAN_Start(&hcan1) != HAL_OK) {
        pr_error_with_tag("CAN_NDE", "CAN Start failed\n");
        return -1;
    }

    /* Enable CAN1 RX0 interrupt in NVIC */
    HAL_NVIC_SetPriority(CAN1_RX0_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(CAN1_RX0_IRQn);

    if (HAL_CAN_ActivateNotification(&hcan1, CAN_IT_RX_FIFO0_MSG_PENDING) != HAL_OK) {
        pr_error_with_tag("CAN_NDE", "Notification activate failed\n");
        return -1;
    }

    pr_info_with_tag("CAN_NDE", "CAN1 started, filter: 0x201+0x202 → FIFO0\n");

    /* Register heartbeat (no reset — CAN loss is not a system failure) */
    if (s_hb_slot_can < 0)
        s_hb_slot_can = wdg_heartbeat_register("can_nde", 10000, NULL, false);

    return 0;
}

void can_nde_set_callbacks(can_nde_feature_callback_t feat_cb,
                           can_nde_heartbeat_callback_t hb_cb)
{
    s_nde.feat_cb = feat_cb;
    s_nde.hb_cb = hb_cb;
}

/*
 * Feed one CAN data frame (8 bytes) into the assembly buffer.
 * Called from HAL_CAN_RxFifo0MsgPendingCallback.
 */
static void can_nde_feed_frame(uint32_t can_id, const uint8_t *data)
{
    /* Any valid CAN frame refreshes NDE heartbeat */
    if (s_hb_slot_can >= 0)
        wdg_heartbeat_update(s_hb_slot_can);

    if (can_id == CAN_NDE_HEARTBEAT_ID) {
        /* Verify CRC8 over data[0..2] */
        if (data[3] != crc8_compute(data, 3)) {
            pr_warn_with_tag("CAN_NDE",
                "Heartbeat CRC mismatch\n");
            return;
        }
        if (s_nde.hb_cb) {
            s_nde.hb_cb(data[0], data[1], (int8_t)data[2]);
        }
        return;
    }

    if (can_id != CAN_NDE_FEATURE_ID)
        return;

    {
        uint8_t seq = data[0] & 0x1F;
        uint8_t expected_crc;
        uint16_t dst_off;
        uint8_t feat_start;
        uint8_t feat_bytes;
        int i;

        if (seq >= CAN_NDE_FRAME_COUNT)
            return;

        /* CRC8 verification over data[0..6] against data[7] */
        expected_crc = crc8_compute(data, 7);
        if (data[7] != expected_crc) {
            pr_warn_with_tag("CAN_NDE",
                "Frame CRC mismatch, seq=%d\n", seq);
            return;
        }

        /* Timeout check: if >100ms since last frame, start new batch */
        if (s_nde.active) {
            uint32_t elapsed;

            elapsed = HAL_GetTick() - s_nde.last_frame_tick;
            if (elapsed > CAN_NDE_BATCH_TIMEOUT_MS) {
                pr_warn_with_tag("CAN_NDE",
                    "Batch timeout, reset. Got seq=%d/%d\n",
                    __builtin_popcount(s_nde.seq_map),
                    CAN_NDE_FRAME_COUNT);
                s_nde.seq_map = 0;
                s_nde.active = 0;
            }
        }

        s_nde.last_frame_tick = HAL_GetTick();

        /* seq=0 starts new batch */
        if (seq == 0 && !s_nde.active) {
            s_nde.seq_map = 0;
            s_nde.active = 1;
            s_nde.window_idx = data[1];
        }

        if (!s_nde.active)
            return;

        /* Don't re-process already received seq */
        if (s_nde.seq_map & (1UL << seq))
            return;

        /*
         * Frame 0: data[2..6] = 5 bytes feat[0..4]
         * Frame 1-15: data[1..6] = 6 bytes feat each
         * Frame 16: data[1] = 1 byte feat[95], rest padding
         */
        if (seq == 0) {
            feat_start = 2;
            feat_bytes = 5;
            dst_off = 0;
        } else if (seq < 16) {
            feat_start = 1;
            feat_bytes = 6;
            dst_off = (uint16_t)(5 + (seq - 1) * 6);
        } else {
            /* seq == 16: only 1 byte of real data */
            feat_start = 1;
            feat_bytes = 1;
            dst_off = 95;
        }

        for (i = 0; i < feat_bytes; i++)
            s_nde.buf[dst_off + i] = data[feat_start + i];

        s_nde.seq_map |= (1UL << seq);

        /* All 17 frames received → assembly complete */
        if (s_nde.seq_map == 0x1FFFF) {
            if (s_nde.feat_cb) {
                s_nde.feat_cb((const float *)s_nde.buf,
                          s_nde.window_idx);
            }
            s_nde.seq_map = 0;
            s_nde.active = 0;
        }
    }
}

/*
 * Periodically called from main loop to detect stale batches.
 */
void can_nde_poll_timeout(void)
{
    if (!s_nde.active)
        return;

    uint32_t elapsed = HAL_GetTick() - s_nde.last_frame_tick;
    if (elapsed > CAN_NDE_BATCH_TIMEOUT_MS) {
        pr_warn_with_tag("CAN_NDE", "Batch timeout poll, got seq=%d/17\n",
                         __builtin_popcount(s_nde.seq_map));
        s_nde.seq_map = 0;
        s_nde.active = 0;
    }
}

/*
 * HAL CAN receive callback — called from CAN IRQ context.
 */
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    CAN_RxHeaderTypeDef rx_header;
    uint8_t rx_data[8];

    if (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &rx_header, rx_data) != HAL_OK)
        return;

    if (rx_header.IDE == CAN_ID_STD) {
        can_nde_feed_frame(rx_header.StdId, rx_data);
    }
}
