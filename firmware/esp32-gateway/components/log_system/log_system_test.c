#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "log_system.h"
#include "global_error.h"

static const char *TEST_LOG_FILE = "log_system_unity_test.log";

static void clear_log_file(void)
{
    remove(TEST_LOG_FILE);
}

void setUp(void)
{
    clear_log_file();
}

void tearDown(void)
{
    log_shutdown();
    clear_log_file();
}

TEST_CASE("log_system filters ERROR INFO DEBUG by level", "[log_system]")
{
    TEST_ASSERT_EQUAL(APP_ERR_OK,
                      log_system_init(LOG_LEVEL_INFO, LOG_OUTPUT_RINGBUF, 1024));

    LOG_ERROR("UT", "error-msg");
    LOG_INFO("UT", "info-msg");
    LOG_DEBUG("UT", "debug-msg");

    char buf[1024] = {0};
    size_t len = log_fetch_ringbuf(buf, sizeof(buf) - 1);
    buf[len] = '\0';

    TEST_ASSERT_GREATER_THAN(0, (int)len);
    TEST_ASSERT_NOT_NULL(strstr(buf, "error-msg"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "info-msg"));
    TEST_ASSERT_NULL(strstr(buf, "debug-msg"));
}

TEST_CASE("log_system accepts empty string message", "[log_system]")
{
    TEST_ASSERT_EQUAL(APP_ERR_OK,
                      log_system_init(LOG_LEVEL_DEBUG, LOG_OUTPUT_RINGBUF, 512));

    LOG_INFO("UT", "");

    char buf[512] = {0};
    size_t len = log_fetch_ringbuf(buf, sizeof(buf) - 1);
    buf[len] = '\0';

    TEST_ASSERT_GREATER_THAN(0, (int)len);
    TEST_ASSERT_NOT_NULL(strstr(buf, "[INFO]"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "UT:"));
}

TEST_CASE("log_system handles long string without crash", "[log_system]")
{
    TEST_ASSERT_EQUAL(APP_ERR_OK,
                      log_system_init(LOG_LEVEL_DEBUG, LOG_OUTPUT_RINGBUF, 1024));

    char long_msg[700];
    memset(long_msg, 'A', sizeof(long_msg) - 1);
    long_msg[sizeof(long_msg) - 1] = '\0';

    LOG_INFO("UT", "%s", long_msg);

    char buf[1024] = {0};
    size_t len = log_fetch_ringbuf(buf, sizeof(buf) - 1);
    buf[len] = '\0';

    TEST_ASSERT_GREATER_THAN(0, (int)len);
    TEST_ASSERT_NOT_NULL(strstr(buf, "[INFO]"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "UT:"));
}

TEST_CASE("log_system output target can switch between UART RingBuffer File", "[log_system]")
{
    TEST_ASSERT_EQUAL(APP_ERR_OK,
                      log_system_init(LOG_LEVEL_DEBUG, LOG_OUTPUT_RINGBUF, 512));

    LOG_INFO("UT", "rb-visible");
    char rb_buf[512] = {0};
    size_t rb_len = log_fetch_ringbuf(rb_buf, sizeof(rb_buf) - 1);
    rb_buf[rb_len] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(rb_buf, "rb-visible"));

    log_set_output(LOG_OUTPUT_UART);
    LOG_INFO("UT", "uart-only");
    memset(rb_buf, 0, sizeof(rb_buf));
    rb_len = log_fetch_ringbuf(rb_buf, sizeof(rb_buf) - 1);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)rb_len);

    log_shutdown();

    log_config_t cfg = {
        .level = LOG_LEVEL_DEBUG,
        .outputs = LOG_OUTPUT_FILE,
        .ringbuf_size = 0,
        .batch_size = 0,
    };
    strncpy(cfg.log_file_path, TEST_LOG_FILE, sizeof(cfg.log_file_path) - 1);

    int ret = log_system_init_with_config(&cfg);
    if (ret == APP_ERR_LOG_FILE_OPEN) {
        TEST_IGNORE_MESSAGE("File output test requires mounted POSIX-compatible VFS/host file system");
    }
    TEST_ASSERT_EQUAL(APP_ERR_OK, ret);

    LOG_INFO("UT", "file-visible");
    TEST_ASSERT_EQUAL(APP_ERR_OK, log_flush());

    FILE *fp = fopen(TEST_LOG_FILE, "r");
    TEST_ASSERT_NOT_NULL(fp);

    char file_buf[512] = {0};
    size_t read_len = fread(file_buf, 1, sizeof(file_buf) - 1, fp);
    fclose(fp);
    file_buf[read_len] = '\0';

    TEST_ASSERT_NOT_NULL(strstr(file_buf, "file-visible"));
}
