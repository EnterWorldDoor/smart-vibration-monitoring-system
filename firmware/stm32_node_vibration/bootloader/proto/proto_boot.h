/**
 * @file proto_boot.h
 * @brief Bootloader 10 状态协议帧解析器
 *
 * 与 app_main.c 内联的 handle_uart4_rx_byte() 完全一致的状态机,
 * 精简为仅保留 OTA 命令所需的帧头/长度/CMD/数据/CRC/帧尾解析。
 *
 * 帧格式: [AA 55] [LEN_H LEN_L] [DEV] [CMD] [SEQ] [DATA...] [CRC_H CRC_L] [0D]
 */

#ifndef PROTO_BOOT_H
#define PROTO_BOOT_H

#include <stdint.h>
#include <stdbool.h>

#define PROTO_BOOT_HEADER       0xAA55
#define PROTO_BOOT_TAIL         0x0D
#define PROTO_BOOT_MAX_PAYLOAD  250

struct proto_boot_packet {
	uint8_t  cmd;
	uint8_t  data[PROTO_BOOT_MAX_PAYLOAD];
	uint16_t data_len;
};

int  proto_boot_init(void);
void proto_boot_feed_byte(uint8_t byte);
bool proto_boot_frame_ready(void);
void proto_boot_get_packet(struct proto_boot_packet *out);
uint16_t proto_boot_calc_crc16(const uint8_t *data, uint16_t len);

#endif /* PROTO_BOOT_H */
