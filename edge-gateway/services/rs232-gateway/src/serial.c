/**
 * @file serial.c
 * @brief RS232 Gateway — POSIX termios 串口实现
 *
 * 配置 115200 8N1, 非阻塞读取 (VMIN=0, VTIME 由 timeout_ms 决定)。
 * 使用 poll() 实现带超时的读取。
 */

#include "serial.h"
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

int serial_open(const struct serial_config *cfg)
{
    if (!cfg || !cfg->port)
        return -1;

    int fd = open(cfg->port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "serial_open: cannot open %s: %s\n",
                cfg->port, strerror(errno));
        return -1;
    }

    /* 设为阻塞模式 (poll() 负责超时控制) */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    struct termios tty;
    memset(&tty, 0, sizeof(tty));

    if (tcgetattr(fd, &tty) != 0) {
        fprintf(stderr, "serial_open: tcgetattr failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    /* 波特率 */
    speed_t speed = B115200;
    switch (cfg->baudrate) {
        case 9600:    speed = B9600;    break;
        case 19200:   speed = B19200;   break;
        case 38400:   speed = B38400;   break;
        case 57600:   speed = B57600;   break;
        case 115200:  speed = B115200;  break;
        case 230400:  speed = B230400;  break;
        case 460800:  speed = B460800;  break;
        case 921600:  speed = B921600;  break;
        default:
            fprintf(stderr, "serial_open: unsupported baudrate %d\n", cfg->baudrate);
            close(fd);
            return -1;
    }
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);

    /* 控制标志: 8 数据位, 无硬件流控, 启用接收器, 忽略调制解调器线 */
    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_cflag &= ~CSIZE;
    switch (cfg->data_bits) {
        case 5: tty.c_cflag |= CS5; break;
        case 6: tty.c_cflag |= CS6; break;
        case 7: tty.c_cflag |= CS7; break;
        default: tty.c_cflag |= CS8; break;
    }
    tty.c_cflag &= ~CRTSCTS;

    /* 停止位 */
    if (cfg->stop_bits == 2)
        tty.c_cflag |= CSTOPB;
    else
        tty.c_cflag &= ~CSTOPB;

    /* 校验位 */
    switch (cfg->parity) {
        case 'E': case 'e':
            tty.c_cflag |= PARENB;
            tty.c_cflag &= ~PARODD;
            break;
        case 'O': case 'o':
            tty.c_cflag |= PARENB | PARODD;
            break;
        default:
            tty.c_cflag &= ~PARENB;
            break;
    }

    /* 输入标志: 忽略奇偶校验错误 */
    tty.c_iflag = IGNPAR;

    /* 输出标志: 原始模式 */
    tty.c_oflag = 0;

    /* 本地标志: 无规范模式, 无回显, 无信号 */
    tty.c_lflag = 0;

    /* 非阻塞读取: VMIN=0, VTIME=timeout_ms/100 (十分之一秒) */
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = (cfg->timeout_ms > 0) ? (unsigned char)((cfg->timeout_ms + 50) / 100) : 1;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        fprintf(stderr, "serial_open: tcsetattr failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    /* 清空缓冲区 */
    tcflush(fd, TCIOFLUSH);

    return fd;
}

int serial_read(int fd, uint8_t *buf, size_t max_len, int timeout_ms)
{
    if (fd < 0 || !buf || max_len == 0)
        return -1;

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret < 0) {
        if (errno == EINTR)
            return 0;
        return -1;
    }
    if (ret == 0)
        return 0;

    if (!(pfd.revents & POLLIN))
        return 0;

    ssize_t n = read(fd, buf, max_len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR)
            return 0;
        return -1;
    }

    return (int)n;
}

int serial_write(int fd, const uint8_t *data, size_t len)
{
    if (fd < 0 || !data || len == 0)
        return -1;

    ssize_t n = write(fd, data, len);
    if (n < 0)
        return -1;
    return (int)n;
}

void serial_close(int fd)
{
    if (fd >= 0) {
        tcflush(fd, TCIOFLUSH);
        close(fd);
    }
}

int serial_is_recoverable_error(int fd)
{
    (void)fd;
    return (errno == ENODEV || errno == ENXIO || errno == EIO);
}
