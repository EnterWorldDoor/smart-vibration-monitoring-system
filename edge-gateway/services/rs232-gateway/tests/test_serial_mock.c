/**
 * @file test_serial_mock.c
 * @brief RS232 Gateway — 串口模块单元测试 (可恢复错误检测)
 */

#include "test_utils.h"
#include "serial.h"
#include <errno.h>
#include <string.h>

/* ==================== 可恢复错误检测测试 ==================== */

static void test_recoverable_error_enodev(void)
{
    errno = ENODEV;
    int recoverable = serial_is_recoverable_error(-1);
    TEST_ASSERT_TRUE(recoverable != 0);
}

static void test_recoverable_error_enxio(void)
{
    errno = ENXIO;
    int recoverable = serial_is_recoverable_error(-1);
    TEST_ASSERT_TRUE(recoverable != 0);
}

static void test_recoverable_error_eio(void)
{
    errno = EIO;
    int recoverable = serial_is_recoverable_error(-1);
    TEST_ASSERT_TRUE(recoverable != 0);
}

static void test_non_recoverable_error_eagain(void)
{
    errno = EAGAIN;
    int recoverable = serial_is_recoverable_error(-1);
    TEST_ASSERT_TRUE(recoverable == 0);
}

static void test_non_recoverable_error_eintr(void)
{
    errno = EINTR;
    int recoverable = serial_is_recoverable_error(-1);
    TEST_ASSERT_TRUE(recoverable == 0);
}

/* ==================== 配置验证 ==================== */

static void test_invalid_fd_read(void)
{
    uint8_t buf[16];
    int n = serial_read(-1, buf, sizeof(buf), 100);
    TEST_ASSERT_EQUAL(-1, n);
}

static void test_invalid_fd_write(void)
{
    uint8_t data[] = { 0xAA };
    int n = serial_write(-1, data, sizeof(data));
    TEST_ASSERT_EQUAL(-1, n);
}

static void test_null_port_open(void)
{
    struct serial_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = NULL;
    cfg.baudrate = 115200;

    int fd = serial_open(&cfg);
    TEST_ASSERT_EQUAL(-1, fd);
}

static void test_null_cfg_open(void)
{
    int fd = serial_open(NULL);
    TEST_ASSERT_EQUAL(-1, fd);
}

/* ==================== 主函数 ==================== */

int main(void)
{
    printf("\n=== Serial Mock Unit Tests ===\n\n");

    printf("[Recoverable Error Detection]\n");
    RUN_TEST(test_recoverable_error_enodev);
    RUN_TEST(test_recoverable_error_enxio);
    RUN_TEST(test_recoverable_error_eio);
    RUN_TEST(test_non_recoverable_error_eagain);
    RUN_TEST(test_non_recoverable_error_eintr);

    printf("\n[Config Validation]\n");
    RUN_TEST(test_invalid_fd_read);
    RUN_TEST(test_invalid_fd_write);
    RUN_TEST(test_null_port_open);
    RUN_TEST(test_null_cfg_open);

    return test_summary();
}
