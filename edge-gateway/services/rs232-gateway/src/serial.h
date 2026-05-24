/**
 * @file serial.h
 * @brief RS232 Gateway — POSIX termios 串口 API
 */

#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>
#include <stddef.h>

struct serial_config {
    const char *port;
    int         baudrate;
    int         data_bits;
    int         stop_bits;
    char        parity;      /* 'N', 'E', 'O' */
    int         timeout_ms;
};

/* 打开串口。成功返回 fd (>=0)，失败返回 -1 并设置 errno。 */
int serial_open(const struct serial_config *cfg);

/* 读取数据。返回读取字节数，0=超时，-1=错误。 */
int serial_read(int fd, uint8_t *buf, size_t max_len, int timeout_ms);

/* 写入数据。返回写入字节数，-1=错误。 */
int serial_write(int fd, const uint8_t *data, size_t len);

/* 关闭串口。 */
void serial_close(int fd);

/* 判断是否可恢复错误 (USB 拔出: ENODEV, ENXIO)。 */
int serial_is_recoverable_error(int fd);

#endif /* SERIAL_H */
