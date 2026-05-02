/**
 * @file system_log.c
 * @brief 企业级日志系统实现
 *
 * 提供格式化的日志输出，支持多等级过滤和时间戳。
 * 通过回调函数实现输出目标的可定制化。
 */

#include "system_log.h"
#include <string.h>
#include <stdio.h>
#include "main.h"   /* HAL_GetTick */

/* ==================== 全局状态 ==================== */

static struct {
        enum log_level level;
        const char *tag;
        log_output_func_t output;
        bool enable_timestamp;
        bool initialized;
        struct log_stats stats;
} g_log = {
        .level = LOG_LEVEL_INFO,
        .tag = LOG_DEFAULT_TAG,
        .output = NULL,
        .enable_timestamp = true,
        .initialized = false,
};

/* ==================== 等级名称映射 ==================== */

static const char *level_strings[] = {
        [LOG_LEVEL_DEBUG] = "DEBUG",
        [LOG_LEVEL_INFO]  = "INFO ",
        [LOG_LEVEL_WARN]  = "WARN ",
        [LOG_LEVEL_ERROR] = "ERROR",
};

#define LEVEL_STRING_COUNT (sizeof(level_strings) / sizeof(level_strings[0]))

/* ==================== 初始化实现 ==================== */

int log_init(const struct log_config *config)
{
        if (config) {
                if (config->level >= LOG_LEVEL_NONE)
                        return ERR_INVALID_PARAM;

                g_log.level = config->level;
                g_log.tag = config->tag ? config->tag : LOG_DEFAULT_TAG;
                g_log.output = config->output;
                g_log.enable_timestamp = config->enable_timestamp;
        } else {
                /* 使用默认配置 */
                g_log.level = LOG_LEVEL_INFO;
                g_log.tag = LOG_DEFAULT_TAG;
                g_log.output = NULL;
                g_log.enable_timestamp = true;
        }

        memset(&g_log.stats, 0, sizeof(g_log.stats));
        g_log.initialized = true;

        return ERR_OK;
}

void log_deinit(void)
{
        if (!g_log.initialized)
                return;

        memset(&g_log, 0, sizeof(g_log));
}

bool log_is_initialized(void)
{
        return g_log.initialized;
}

/* ==================== 配置实现 ==================== */

void log_set_level(enum log_level level)
{
        if (level >= LOG_LEVEL_NONE)
                return;

        g_log.level = level;
}

enum log_level log_get_level(void)
{
        return g_log.level;
}

void log_set_tag(const char *tag)
{
        if (tag)
                g_log.tag = tag;
}

void log_set_output(log_output_func_t output)
{
        g_log.output = output;
}

void log_enable_timestamp(bool enable)
{
        g_log.enable_timestamp = enable;
}

/* ==================== 核心日志实现 ==================== */

void log_write(enum log_level level, const char *tag,
               const char *fmt, ...)
{
        char buffer[LOG_MAX_LINE_SIZE];
        int offset = 0;
        int written;
        va_list args;

        /* 检查初始化状态和等级过滤 */
        if (!g_log.initialized || level < g_log.level)
                return;

        /* 检查等级有效性 */
        if ((uint32_t)level >= LEVEL_STRING_COUNT)
                return;

        /* 构建时间戳 */
        if (g_log.enable_timestamp && g_log.initialized) {
                uint32_t ts = HAL_GetTick();
                uint32_t ms = ts % 1000;
                uint32_t sec = ts / 1000;
                uint32_t min = sec / 60;
                uint32_t hour = min / 60;

                written = snprintf(buffer + offset,
                                   sizeof(buffer) - offset,
                                   "[%02lu:%02lu:%02lu.%03lu] ",
                                   (unsigned long)(hour % 24),
                                   (unsigned long)(min % 60),
                                   (unsigned long)(sec % 60),
                                   (unsigned long)ms);
                if (written > 0 && offset < (int)sizeof(buffer))
                        offset += written;
        }

        /* 构建等级和标签 */
        {
                const char *used_tag = tag ? tag : g_log.tag;

                written = snprintf(buffer + offset,
                                   sizeof(buffer) - offset,
                                   "[%s] [%s] ",
                                   level_strings[level],
                                   used_tag);
                if (written > 0 && offset < (int)sizeof(buffer))
                        offset += written;
        }

        /* 格式化用户消息 */
        va_start(args, fmt);
        written = vsnprintf(buffer + offset,
                            sizeof(buffer) - offset,
                            fmt, args);
        va_end(args);

        if (written > 0 && offset < (int)sizeof(buffer))
                offset += written;

        /* 确保字符串终止且不越界 */
        if (offset >= (int)sizeof(buffer))
                offset = sizeof(buffer) - 1;
        buffer[offset] = '\0';

        /* 更新统计信息 */
        g_log.stats.total_lines++;
        g_log.stats.bytes_written += (uint16_t)offset;
        if ((uint32_t)level < 4)
                g_log.stats.level_counts[level]++;

        /* 输出日志 */
        if (g_log.output) {
                g_log.output(buffer, (uint16_t)offset);
        }
}

/* ==================== 统计实现 ==================== */

int log_get_stats(struct log_stats *stats)
{
        if (!stats)
                return ERR_INVALID_PARAM;

        memcpy(stats, &g_log.stats, sizeof(*stats));

        return ERR_OK;
}

void log_reset_stats(void)
{
        memset(&g_log.stats, 0, sizeof(g_log.stats));
}
