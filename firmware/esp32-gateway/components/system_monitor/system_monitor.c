/**
 * @file system_monitor.c
 * @author EnterWorldDoor
 * @brief 企业级系统监控实现：CPU/内存/任务/WDT/阈值告警/历史趋势/线程安全
 *
 * 架构设计:
 *   - 后台监控任务按配置间隔采样系统状态
 *   - 所有状态查询通过 Mutex 保护，确保多任务并发安全
 *   - 阈值告警在采样后自动检查并触发回调通知
 *   - 历史数据使用环形缓冲区存储，支持趋势分析
 *   - CPU 使用率基于空闲任务运行时间比例计算
 */

#include "system_monitor.h"
#include "log_system.h"
#include "global_error.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include <string.h>
#include <stddef.h>

/* ==================== 模块内部状态 ==================== */

static struct {
    bool initialized;
    TaskHandle_t task_handle;
    SemaphoreHandle_t mutex;
    uint32_t interval_ms;
    int history_depth;
    int history_count;
    int history_head;
    bool enable_wdt_feed;
    struct monitor_thresholds thresholds;
    struct monitor_history_entry *history_buf;

    /* CPU 计算用: 空闲任务累计运行时间 */
    uint32_t idle_ticks_prev;
    uint32_t total_ticks_prev;
    float cpu_usage_smoothed;          /**< 平滑后的 CPU 使用率 */

    /* 告警回调 */
    monitor_alarm_callback_t alarm_cbs[MONITOR_MAX_CALLBACKS];
    void *alarm_cb_user_data[MONITOR_MAX_CALLBACKS];
    int alarm_cb_count;
} g_mon = {0};

/* ==================== 默认阈值配置 ==================== */

static const struct monitor_thresholds DEFAULT_THRESHOLDS = {
    .cpu_warn_percent      = 80.0f,
    .cpu_critical_percent  = 95.0f,
    .heap_warn_bytes       = 10240,     /* 10 KB 警告 */
    .heap_critical_bytes   = 5120,      /* 5 KB 严重 */
    .stack_warn_percent    = 70.0f,
    .stack_critical_percent = 90.0f,
};

/* ==================== 内部辅助函数 ==================== */

/**
 * compute_fragmentation - 计算堆内存碎片率
 * @free_size: 当前空闲大小
 * @min_free: 历史最小空闲
 * Return: 碎片率百分比 (0~100)
 */
static float compute_fragmentation(size_t free_size, size_t min_free)
{
    if (free_size == 0) return 100.0f;
    float frag = (1.0f - (float)min_free / (float)free_size) * 100.0f;
    if (frag < 0.0f) frag = 0.0f;
    if (frag > 100.0f) frag = 100.0f;
    return frag;
}

/**
 * notify_alarm - 触发所有注册的告警回调
 * @type: 告警类型
 * @current: 当前值
 * @threshold: 阈值值
 */
static void notify_alarm(int type, float current, float threshold)
{
    for (int i = 0; i < g_mon.alarm_cb_count; i++) {
        if (g_mon.alarm_cbs[i]) {
            g_mon.alarm_cbs[i](type, current, threshold,
                             g_mon.alarm_cb_user_data[i]);
        }
    }
}

/**
 * check_thresholds - 在采样后检查是否触发告警
 * @status: 最新采样的系统状态
 */
static void check_thresholds(const struct system_status *status)
{
    const struct monitor_thresholds *th = &g_mon.thresholds;

    /* CPU 检查 */
    if (th->cpu_critical_percent > 0 && status->cpu.usage_percent >= th->cpu_critical_percent) {
        LOG_ERROR("MON", "CPU CRITICAL: %.1f%% >= %.1f%%",
                  status->cpu.usage_percent, th->cpu_critical_percent);
        notify_alarm(MONITOR_ALARM_CPU_CRITICAL, status->cpu.usage_percent,
                     th->cpu_critical_percent);
    } else if (th->cpu_warn_percent > 0 && status->cpu.usage_percent >= th->cpu_warn_percent) {
        LOG_WARN("MON", "CPU WARNING: %.1f%% >= %.1f%%",
                 status->cpu.usage_percent, th->cpu_warn_percent);
        notify_alarm(MONITOR_ALARM_CPU_HIGH, status->cpu.usage_percent,
                     th->cpu_warn_percent);
    }

    /* 堆内存检查 */
    if (th->heap_critical_bytes > 0 && status->mem.free_heap <= th->heap_critical_bytes) {
        LOG_ERROR("MON", "HEAP CRITICAL: %u bytes <= %u bytes",
                  (unsigned)status->mem.free_heap, (unsigned)th->heap_critical_bytes);
        notify_alarm(MONITOR_ALARM_HEAP_LOW, (float)status->mem.free_heap,
                     (float)th->heap_critical_bytes);
    } else if (th->heap_warn_bytes > 0 && status->mem.free_heap <= th->heap_warn_bytes) {
        LOG_WARN("MON", "HEAP WARNING: %u bytes <= %u bytes",
                 (unsigned)status->mem.free_heap, (unsigned)th->heap_warn_bytes);
        notify_alarm(MONITOR_ALARM_HEAP_LOW, (float)status->mem.free_heap,
                     (float)th->heap_warn_bytes);
    }

    /* 最小堆内存检查 */
    if (th->heap_critical_bytes > 0 && status->mem.min_free_heap <= th->heap_critical_bytes) {
        notify_alarm(MONITOR_ALARM_HEAP_MIN_LOW, (float)status->mem.min_free_heap,
                     (float)th->heap_critical_bytes);
    }

    /* 单任务栈使用率检查 */
    if (th->stack_critical_percent > 0 || th->stack_warn_percent > 0) {
        for (int i = 0; i < status->task_info_count; i++) {
            float usage = status->tasks[i].stack_usage_percent;
            if (th->stack_critical_percent > 0 && usage >= th->stack_critical_percent) {
                LOG_ERROR("MON", "STACK CRITICAL [%s]: %.1f%%",
                          status->tasks[i].name, usage);
                notify_alarm(MONITOR_ALARM_TASK_STACK_HIGH, usage,
                             th->stack_critical_percent);
            } else if (th->stack_warn_percent > 0 && usage >= th->stack_warn_percent) {
                LOG_WARN("MON", "STACK WARNING [%s]: %.1f%%",
                         status->tasks[i].name, usage);
                notify_alarm(MONITOR_ALARM_TASK_STACK_HIGH, usage,
                             th->stack_warn_percent);
            }
        }
    }
}

/**
 * push_history - 将当前采样推入历史环形缓冲区
 * @status: 当前系统状态
 */
static void push_history(const struct system_status *status)
{
    if (!g_mon.history_buf || g_mon.history_depth <= 0) return;

    int idx = g_mon.history_head;
    g_mon.history_buf[idx].cpu_usage = status->cpu.usage_percent;
    g_mon.history_buf[idx].free_heap = status->mem.free_heap;
    g_mon.history_buf[idx].timestamp_s = (uint32_t)(status->uptime_ms / 1000ULL);

    g_mon.head = (idx + 1) % g_mon.history_depth;
    if (g_mon.history_count < g_mon.history_depth) {
        g_mon.history_count++;
    }
}

/**
 * collect_status - 内部函数：采集一次完整的系统状态快照（调用者需持有锁）
 * @out: 输出结构体指针
 */
static void collect_status(struct system_status *out)
{
    memset(out, 0, sizeof(*out));
    out->uptime_ms = esp_timer_get_time() / 1000ULL;

    /* --- 内存信息 --- */
    out->mem.free_heap = esp_get_free_heap_size();
    out->mem.min_free_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    out->mem.total_heap = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    out->mem.fragmentation_percent = compute_fragmentation(
        out->mem.free_heap, out->mem.min_free_heap);

    /* PSRAM（如果存在）*/
    #if CONFIG_SPIRAM
    out->mem.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    out->mem.min_free_psram = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    #else
    out->mem.free_psram = 0;
    out->mem.min_free_psram = 0;
    #endif

    /* --- CPU 信息 --- */
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    out->task_count = (int)task_count;

    static TaskStatus_t *task_status_buf = NULL;
    static uint32_t task_status_buf_size = 0;
    uint32_t needed = task_count * sizeof(TaskStatus_t);
    if (needed > task_status_buf_size) {
        if (task_status_buf) free(task_status_buf);
        task_status_buf = malloc(needed);
        if (task_status_buf) {
            task_status_buf_size = needed;
        } else {
            task_status_buf_size = 0;
        }
    }

    uint32_t total_run_time = 0;
    if (task_status_buf && task_count > 0) {
        task_count = uxTaskGetSystemState(task_status_buf, task_count,
                                          &total_run_time);
        out->task_count = (int)task_count;
    }

    out->cpu.idle_ticks_total = 0;
    out->cpu.total_ticks = total_run_time;

    /* 寻找空闲任务统计其运行时间 */
    for (UBaseType_t i = 0; i < task_count; i++) {
        const char *name = task_status_buf[i].pcTaskName;
        if (name && strstr(name, "idle") != NULL) {
            out->cpu.idle_ticks_total = task_status_buf[i].ulRunTimeCounter;
            break;
        }
    }

    /* 计算 CPU 使用率 */
    if (g_mon.total_ticks_prev > 0 && total_run_time > g_mon.total_ticks_prev) {
        uint32_t delta_total = total_run_time - g_mon.total_ticks_prev;
        uint32_t delta_idle = out->cpu.idle_ticks_total - g_mon.idle_ticks_prev;
        if (delta_total > 0) {
            float idle_ratio = (float)delta_idle / (float)delta_total;
            float cpu_raw = (1.0f - idle_ratio) * 100.0f;
            if (cpu_raw < 0.0f) cpu_raw = 0.0f;
            if (cpu_raw > 100.0f) cpu_raw = 100.0f;
            /* EMA 平滑: α=0.3 */
            g_mon.cpu_usage_smoothed = 0.3f * cpu_raw + 0.7f * g_mon.cpu_usage_smoothed;
        }
    } else {
        g_mon.cpu_usage_smoothed = 0.0f;
    }
    out->cpu.usage_percent = g_mon.cpu_usage_smoothed;

    g_mon.idle_ticks_prev = out->cpu.idle_ticks_total;
    g_mon.total_ticks_prev = total_run_time;

    /* --- 任务详情 --- */
    out->task_info_count = 0;
    if (task_status_buf) {
        int max_tasks = (task_count < MONITOR_MAX_TASKS)
                          ? (int)task_count : MONITOR_MAX_TASKS;
        for (int i = 0; i < max_tasks; i++) {
            TaskHandle_t handle = task_status_buf[i].xHandle;
            strncpy(out->tasks[i].name, task_status_buf[i].pcTaskName,
                    configMAX_TASK_NAME_LEN - 1);
            out->tasks[i].task_number = task_status_buf[i].xTaskNumber;
            out->tasks[i].stack_high_water_mark = uxTaskGetStackHighWaterMark(handle);
            out->tasks[i].stack_size = (handle)
                                       ? uxTaskGetStackSize(handle) : 0;
            if (out->tasks[i].stack_size > 0) {
                out->tasks[i].stack_usage_percent =
                    100.0f * (1.0f - (float)out->tasks[i].stack_high_water_mark /
                                      (float)out->tasks[i].stack_size);
            } else {
                out->tasks[i].stack_usage_percent = 0.0f;
            }
            out->task_info_count++;
        }
    }
}

/* ==================== 后台监控任务 ==================== */

static void monitor_task(void *arg)
{
    (void)arg;
    LOG_INFO("MON", "Background monitoring started (interval=%u ms)", g_mon.interval_ms);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(g_mon.interval_ms));

        xSemaphoreTake(g_mon.mutex, portMAX_DELAY);

        struct system_status status;
        collect_status(&status);

        push_history(&status);
        check_thresholds(&status);

        LOG_DEBUG("MON", "CPU:%.1f%% Heap:%u/%u Tasks:%d Frag:%.1f%%",
                  status.cpu.usage_percent,
                  (unsigned)status.mem.free_heap,
                  (unsigned)status.mem.total_heap,
                  status.task_count,
                  status.mem.fragmentation_percent);

        xSemaphoreGive(g_mon.mutex);

        if (g_mon.enable_wdt_feed) {
            esp_task_wdt_reset();
        }
    }
}

/* ==================== 生命周期 API ==================== */

int system_monitor_init(uint32_t interval_ms)
{
    struct monitor_config cfg = {
        .interval_ms = (interval_ms > 0) ? interval_ms : MONITOR_DEFAULT_INTERVAL,
        .history_depth = MONITOR_HISTORY_DEPTH,
        .enable_wdt_feed = false,
        .thresholds = DEFAULT_THRESHOLDS,
    };
    return system_monitor_init_with_config(&cfg);
}

int system_monitor_init_with_config(const struct monitor_config *cfg)
{
    if (!cfg) return APP_ERR_INVALID_PARAM;
    if (g_mon.initialized) return APP_ERR_MONITOR_ALREADY_INIT;

    memset(&g_mon, 0, sizeof(g_mon));

    g_mon.mutex = xSemaphoreCreateMutex();
    if (!g_mon.mutex) {
        LOG_ERROR("MON", "Failed to create mutex");
        return APP_ERR_NO_MEM;
    }

    g_mon.interval_ms = cfg->interval_ms;
    if (g_mon.interval_ms == 0) g_mon.interval_ms = MONITOR_DEFAULT_INTERVAL;
    g_mon.history_depth = cfg->history_depth;
    g_mon.enable_wdt_feed = cfg->enable_wdt_feed;
    g_mon.thresholds = cfg->thresholds ? *cfg->thresholds : DEFAULT_THRESHOLDS;

    if (g_mon.history_depth > 0) {
        g_mon.history_buf = malloc(sizeof(struct monitor_history_entry) *
                                 g_mon.history_depth);
        if (!g_mon.history_buf) {
            LOG_WARN("MON", "History buffer alloc failed, disabling history");
            g_mon.history_depth = 0;
        }
    }

    g_mon.initialized = true;

    if (g_mon.interval_ms > 0) {
        BaseType_t ret = xTaskCreate(monitor_task, "sys_monitor",
                                     MONITOR_TASK_STACK_SIZE, NULL,
                                     MONITOR_TASK_PRIORITY, &g_mon.task_handle);
        if (ret != pdPASS) {
            LOG_ERROR("MON", "Failed to create monitor task");
            vSemaphoreDelete(g_mon.mutex);
            if (g_mon.history_buf) { free(g_mon.history_buf); g_mon.history_buf = NULL; }
            g_mon.initialized = false;
            return APP_ERR_MONITOR_TASK_CREATE;
        }
    }

    LOG_INFO("MON", "Initialized (interval=%u ms, history=%d, wdt=%d)",
             g_mon.interval_ms, g_mon.history_depth, g_mon.enable_wdt_feed);
    return APP_ERR_OK;
}

int system_monitor_deinit(void)
{
    if (!g_mon.initialized) return APP_ERR_MONITOR_NOT_INIT;

    if (g_mon.task_handle) {
        vTaskDelete(g_mon.task_handle);
        g_mon.task_handle = NULL;
    }

    if (g_mon.history_buf) {
        free(g_mon.history_buf);
        g_mon.history_buf = NULL;
    }

    vSemaphoreDelete(g_mon.mutex);
    g_mon.mutex = NULL;

    g_mon.initialized = false;
    g_mon.alarm_cb_count = 0;
    memset(g_mon.alarm_cbs, 0, sizeof(g_mon.alarm_cbs));
    memset(g_mon.alarm_cb_user_data, 0, sizeof(g_mon.alarm_cb_user_data));

    LOG_INFO("MON", "Deinitialized");
    return APP_ERR_OK;
}

bool system_monitor_is_initialized(void)
{
    return g_mon.initialized;
}

/* ==================== 状态查询 API ==================== */

int system_monitor_get_status(struct system_status *status)
{
    if (!status) return APP_ERR_INVALID_PARAM;
    if (!g_mon.initialized || !g_mon.mutex) return APP_ERR_MONITOR_NOT_INIT;

    xSemaphoreTake(g_mon.mutex, portMAX_DELAY);
    collect_status(status);
    xSemaphoreGive(g_mon.mutex);
    return APP_ERR_OK;
}

int system_monitor_get_cpu(struct cpu_info *cpu)
{
    if (!cpu) return APP_ERR_INVALID_PARAM;
    struct system_status s;
    int ret = system_monitor_get_status(&s);
    if (ret == APP_ERR_OK) *cpu = s.cpu;
    return ret;
}

int system_monitor_get_memory(struct memory_info *mem)
{
    if (!mem) return APP_ERR_INVALID_PARAM;
    struct system_status s;
    int ret = system_monitor_get_status(&s);
    if (ret == APP_ERR_OK) *mem = s.mem;
    return ret;
}

int system_monitor_get_task_count(void)
{
    if (!g_mon.initialized) return -1;
    return uxTaskGetNumberOfTasks();
}

int system_monitor_get_task_info(int index, struct task_info *info)
{
    if (!info || index < 0) return APP_ERR_INVALID_PARAM;
    if (!g_mon.initialized || !g_mon.mutex) return APP_ERR_MONITOR_NOT_INIT;

    struct system_status s;
    int ret = system_monitor_get_status(&s);
    if (ret != APP_ERR_OK) return ret;
    if (index >= s.task_info_count) return APP_ERR_INVALID_PARAM;
    *info = s.tasks[index];
    return APP_ERR_OK;
}

uint64_t system_monitor_get_uptime(void)
{
    return esp_timer_get_time() / 1000ULL;
}

/* ==================== 日志输出 API ==================== */

void system_monitor_dump(void)
{
    if (!g_mon.initialized || !g_mon.mutex) {
        LOG_WARN("MONITOR", "dump: not initialized");
        return;
    }

    struct system_status s;
    xSemaphoreTake(g_mon.mutex, portMAX_DELAY);
    collect_status(&s);
    xSemaphoreGive(g_mon.mutex);

    uint32_t up_s = (uint32_t)(s.uptime_ms / 1000ULL);
    uint32_t up_m = up_s / 60;
    uint32_t up_h = up_m / 60;
    LOG_INFO("MONITOR", "========== SYSTEM STATUS ==========");
    LOG_INFO("MONITOR", "Uptime: %02uh %02um %02us", up_h, up_m % 60, up_s % 60);
    LOG_INFO("MONITOR", "CPU Usage: %.1f%%", s.cpu.usage_percent);
    LOG_INFO("MONITOR", "Heap: free=%u min=%u total=%u frag=%.1f%%",
             (unsigned)s.mem.free_heap, (unsigned)s.mem.min_free_heap,
             (unsigned)s.mem.total_heap, s.mem.fragmentation_percent);
#if CONFIG_SPIRAM
    LOG_INFO("MONITOR", "PSRAM: free=%u min=%u",
             (unsigned)s.mem.free_psram, (unsigned)s.mem.min_free_psram);
#endif
    LOG_INFO("MONITOR", "Tasks: %d", s.task_count);
    LOG_INFO("MONITOR", "====================================");
}

void system_monitor_dump_tasks(void)
{
    if (!g_mon.initialized || !g_mon.mutex) return;

    struct system_status s;
    xSemaphoreTake(g_mon.mutex, portMAX_DELAY);
    collect_status(&s);
    xSemaphoreGive(g_mon.mutex);

    LOG_INFO("MONITOR", "===== TASK DETAILS =====");
    for (int i = 0; i < s.task_info_count; i++) {
        LOG_INFO("MONITOR",
                 "[%2d] %-16s stack=%u/%u (%.1f%%)",
                 i, s.tasks[i].name,
                 (unsigned)(s.tasks[i].stack_size - s.tasks[i].stack_high_water_mark),
                 (unsigned)s.tasks[i].stack_size,
                 s.tasks[i].stack_usage_percent);
    }
    LOG_INFO("MONITOR", "========================");
}

void system_monitor_dump_memory(void)
{
    if (!g_mon.initialized || !g_mon.mutex) return;

    struct memory_info m;
    int ret = system_monitor_get_memory(&m);
    if (ret != APP_ERR_OK) return;

    LOG_INFO("MONITOR", "===== MEMORY DETAILS =====");
    LOG_INFO("MONITOR", "Free Heap : %u bytes (%.1f KB)",
             (unsigned)m.free_heap, m.free_heap / 1024.0f);
    LOG_INFO("MONITOR", "Min Free  : %u bytes (%.1f KB)",
             (unsigned)m.min_free_heap, m.min_free_heap / 1024.0f);
    LOG_INFO("MONITOR", "Total Heap: %u bytes (%.1f KB)",
             (unsigned)m.total_heap, m.total_heap / 1024.0f);
    LOG_INFO("MONITOR", "Fragment  : %.1f%%", m.fragmentation_percent);
#if CONFIG_SPIRAM
    LOG_INFO("MONITOR", "PSRAM Free: %u bytes (%.1f KB)",
             (unsigned)m.free_psram, m.free_psram / 1024.0f);
#endif
    LOG_INFO("MONITOR", "============================");
}

/* ==================== 阈值与告警 API ==================== */

int system_monitor_set_thresholds(const struct monitor_thresholds *th)
{
    if (!th) return APP_ERR_INVALID_PARAM;
    if (!g_mon.initialized || !g_mon.mutex) return APP_ERR_MONITOR_NOT_INIT;

    if (th->cpu_warn_percent < 0 || th->cpu_warn_percent > 100) return APP_ERR_MONITOR_THRESHOLD_INVALID;
    if (th->cpu_critical_percent < 0 || th->cpu_critical_percent > 100) return APP_ERR_MONITOR_THRESHOLD_INVALID;
    if (th->stack_warn_percent < 0 || th->stack_warn_percent > 100) return APP_ERR_MONITOR_THRESHOLD_INVALID;
    if (th->stack_critical_percent < 0 || th->stack_critical_percent > 100) return APP_ERR_MONITOR_THRESHOLD_INVALID;
    if (th->cpu_warn_percent > 0 && th->cpu_critical_percent > 0 &&
        th->cpu_warn_percent > th->cpu_critical_percent) return APP_ERR_MONITOR_THRESHOLD_INVALID;

    xSemaphoreTake(g_mon.mutex, portMAX_DELAY);
    g_mon.thresholds = *th;
    xSemaphoreGive(g_mon.mutex);

    LOG_INFO("MON", "Thresholds updated: CPU(w=%.0f/c=%.0f) Heap(w=%u/c=%u) Stack(w=%.0f/c=%.0f)",
             th->cpu_warn_percent, th->cpu_critical_percent,
             (unsigned)th->heap_warn_bytes, (unsigned)th->heap_critical_bytes,
             th->stack_warn_percent, th->stack_critical_percent);
    return APP_ERR_OK;
}

int system_monitor_get_thresholds(struct monitor_thresholds *th)
{
    if (!th) return APP_ERR_INVALID_PARAM;
    if (!g_mon.initialized || !g_mon.mutex) return APP_ERR_MONITOR_NOT_INIT;

    xSemaphoreTake(g_mon.mutex, portMAX_DELAY);
    *th = g_mon.thresholds;
    xSemaphoreGive(g_mon.mutex);
    return APP_ERR_OK;
}

int system_monitor_register_alarm_callback(monitor_alarm_callback_t cb, void *user_data)
{
    if (!cb) return APP_ERR_INVALID_PARAM;
    if (!g_mon.initialized) return APP_ERR_MONITOR_NOT_INIT;
    if (g_mon.alarm_cb_count >= MONITOR_MAX_CALLBACKS) return APP_ERR_MONITOR_CALLBACK_FULL;

    for (int i = 0; i < g_mon.alarm_cb_count; i++) {
        if (g_mon.alarm_cbs[i] == cb) {
            LOG_WARN("MON", "Callback already registered");
            return APP_ERR_OK;
        }
    }

    g_mon.alarm_cbs[g_mon.alarm_cb_count] = cb;
    g_mon.alarm_cb_user_data[g_mon.alarm_cb_count] = user_data;
    g_mon.alarm_cb_count++;
    LOG_INFO("MON", "Alarm callback registered (total=%d)", g_mon.alarm_cb_count);
    return APP_ERR_OK;
}

int system_monitor_unregister_alarm_callback(monitor_alarm_callback_t cb)
{
    if (!cb) return APP_ERR_INVALID_PARAM;
    if (!g_mon.initialized) return APP_ERR_MONITOR_NOT_INIT;

    for (int i = 0; i < g_mon.alarm_cb_count; i++) {
        if (g_mon.alarm_cbs[i] == cb) {
            for (int j = i; j < g_mon.alarm_cb_count - 1; j++) {
                g_mon.alarm_cbs[j] = g_mon.alarm_cbs[j + 1];
                g_mon.alarm_cb_user_data[j] = g_mon.alarm_cb_user_data[j + 1];
            }
            g_mon.alarm_cb_count--;
            g_mon.alarm_cbs[g_mon.alarm_cb_count] = NULL;
            g_mon.alarm_cb_user_data[g_mon.alarm_cb_count] = NULL;
            LOG_INFO("MON", "Alarm callback unregistered (remaining=%d)",
                     g_mon.alarm_cb_count);
            return APP_ERR_OK;
        }
    }
    LOG_WARN("MON", "Callback not found");
    return APP_ERR_NOT_SUPPORTED;
}

/* ==================== 历史数据 API ==================== */

int system_monitor_get_history_depth(void)
{
    return g_mon.history_count;
}

int system_monitor_get_history_entry(int index, struct monitor_history_entry *entry)
{
    if (!entry || index < 0) return APP_ERR_INVALID_PARAM;
    if (!g_mon.initialized || !g_mon.history_buf) return APP_ERR_MONITOR_NOT_INIT;
    if (index >= g_mon.history_count) return APP_ERR_INVALID_PARAM;

    xSemaphoreTake(g_mon.mutex, portMAX_DELAY);
    int real_idx = (g_mon.head - 1 - index + g_mon.history_depth) % g_mon.history_depth;
    *entry = g_mon.history_buf[real_idx];
    xSemaphoreGive(g_mon.mutex);
    return APP_ERR_OK;
}

void system_monitor_clear_history(void)
{
    if (!g_mon.initialized || !g_mon.mutex) return;
    xSemaphoreTake(g_mon.mutex, portMAX_DELAY);
    g_mon.history_head = 0;
    g_mon.history_count = 0;
    if (g_mon.history_buf) {
        memset(g_mon.history_buf, 0,
               sizeof(struct monitor_history_entry) * g_mon.history_depth);
    }
    xSemaphoreGive(g_mon.mutex);
    LOG_INFO("MON", "History cleared");
}