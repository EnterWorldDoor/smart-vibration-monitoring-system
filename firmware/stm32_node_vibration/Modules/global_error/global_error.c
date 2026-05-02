/**
 * @file global_error.c
 * @brief STM32 全局错误码管理系统实现
 *
 * 实现错误的记录、查询、清除等运行时管理功能。
 * 采用静态缓冲区设计，避免动态内存分配。
 */

#include "global_error.h"
#include <string.h>

/* ==================== 错误描述表 ==================== */

static const struct error_desc {
        int code;
        const char *str;
} error_table[] = {
        /* 通用错误 */
        { ERR_OK,          "success" },
        { ERR_GENERAL,     "general error" },
        { ERR_INVALID_PARAM, "invalid parameter" },
        { ERR_NO_MEM,      "no memory" },
        { ERR_TIMEOUT,     "operation timeout" },
        { ERR_NOT_SUPPORTED, "not supported" },
        { ERR_BUSY,        "device busy" },
        { ERR_ALREADY_INIT, "already initialized" },
        { ERR_NOT_INIT,    "not initialized" },
        { ERR_NULL_POINTER, "null pointer" },

        /* 队列错误 */
        { ERR_QUEUE_INIT_FAIL,   "queue init failed" },
        { ERR_QUEUE_FULL,        "queue full" },
        { ERR_QUEUE_EMPTY,       "queue empty" },
        { ERR_QUEUE_INVALID_SIZE, "invalid queue size" },

        /* CRC 错误 */
        { ERR_CRC_INIT_FAIL,    "crc init failed" },
        { ERR_CRC_MISMATCH,     "crc mismatch" },
        { ERR_CRC_INVALID_PARAM, "invalid crc param" },

        /* 日志系统错误 */
        { ERR_LOG_INIT_FAIL,      "log init failed" },
        { ERR_LOG_BUFFER_OVERFLOW, "log buffer overflow" },
        { ERR_LOG_INVALID_LEVEL,   "invalid log level" },

        /* 传感器错误 */
        { ERR_SENSOR_INIT_FAIL,   "sensor init failed" },
        { ERR_SENSOR_NOT_FOUND,   "sensor not found" },
        { ERR_SENSOR_DATA_OVERRUN, "sensor data overrun" },
        { ERR_SENSOR_COMM_FAIL,   "sensor comm fail" },

        /* DHT11 温湿度传感器错误 */
        { ERR_DHT11_INIT_FAIL,      "DHT11 init failed" },
        { ERR_DHT11_NOT_INIT,       "DHT11 not initialized" },
        { ERR_DHT11_INVALID_PARAM,   "DHT11 invalid parameter" },
        { ERR_DHT11_TIMEOUT,         "DHT11 communication timeout" },
        { ERR_DHT11_CHECKSUM_ERROR,  "DHT11 checksum error" },
        { ERR_DHT11_READ_FAIL,       "DHT11 read data failed" },
        { ERR_DHT11_NO_RESPONSE,     "DHT11 no response" },
        { ERR_DHT11_BUS_BUSY,        "DHT11 bus busy" },

        /* 电机控制错误 */
        { ERR_MOTOR_INIT_FAIL,    "motor init failed" },
        { ERR_MOTOR_SPEED_OVERRUN, "motor speed overrun" },
        { ERR_MOTOR_DRIVER_FAULT, "motor driver fault" },

        /* 通信错误 */
        { ERR_COMM_INIT_FAIL, "comm init failed" },
        { ERR_COMM_TX_FAIL,   "comm tx fail" },
        { ERR_COMM_RX_FAIL,   "comm rx fail" },
        { ERR_COMM_TIMEOUT,   "comm timeout" },

        /* 文件系统错误 */
        { ERR_FS_INIT_FAIL,      "filesystem init failed" },
        { ERR_FS_FILE_NOT_FOUND, "file not found" },
        { ERR_FS_WRITE_FAIL,     "write fail" },

        /* FreeRTOS 错误 */
        { ERR_RTOS_TASK_CREATE_FAIL,  "task create fail" },
        { ERR_RTOS_QUEUE_CREATE_FAIL, "queue create fail" },
        { ERR_RTOS_MUTEX_CREATE_FAIL, "mutex create fail" },
        { ERR_RTOS_SEM_CREATE_FAIL,   "semaphore create fail" },
};

#define ERROR_TABLE_SIZE  (sizeof(error_table) / sizeof(error_table[0]))

/* ==================== 时间戳获取函数指针 ==================== */

/*
 * 外部提供的时间戳获取函数。
 * 必须在系统初始化后通过 error_set_time_callback() 设置。
 * 默认返回 0，表示时间戳功能不可用。
 */
static uint32_t (*time_callback)(void) = NULL;

/**
 * error_set_time_callback - 设置时间戳回调函数
 * @cb: 返回当前时间戳 (ms) 的函数指针
 *
 * 允许上层设置自定义的时间戳源（如 HAL_GetTick()）。
 */
void error_set_time_callback(uint32_t (*cb)(void))
{
        time_callback = cb;
}

static uint32_t get_timestamp(void)
{
        if (time_callback)
                return time_callback();
        return 0;
}

/* ==================== 生命周期实现 ==================== */

int error_manager_init(struct error_manager *mgr,
                       struct error_record *record_buf,
                       uint16_t max_records)
{
        if (!mgr || !record_buf || max_records == 0)
                return ERR_INVALID_PARAM;

        memset(mgr, 0, sizeof(*mgr));
        memset(record_buf, 0,
               sizeof(struct error_record) * max_records);

        mgr->records = record_buf;
        mgr->max_records = max_records;
        mgr->initialized = true;

        return ERR_OK;
}

void error_manager_deinit(struct error_manager *mgr)
{
        if (!mgr || !mgr->initialized)
                return;

        memset(mgr, 0, sizeof(*mgr));
}

/* ==================== 错误操作实现 ==================== */

int error_manager_set(struct error_manager *mgr, int code,
                      uint8_t module_id)
{
        struct error_record *rec;

        if (!mgr || !mgr->initialized)
                return ERR_NOT_INIT;

        if (code == ERR_OK)
                return ERR_INVALID_PARAM;

        if (module_id >= MODULE_ID_MAX)
                return ERR_INVALID_PARAM;

        /* 更新最后错误码 */
        mgr->last_error = code;

        /* 查找是否已存在相同的未清除错误 */
        for (uint16_t i = 0; i < mgr->count; i++) {
                rec = &mgr->records[i];
                if (rec->code == code && !rec->cleared) {
                        rec->timestamp = get_timestamp();
                        return ERR_OK;
                }
        }

        /* 缓冲区满时，覆盖最旧的已清除记录 */
        if (mgr->count >= mgr->max_records) {
                for (uint16_t i = 0; i < mgr->count; i++) {
                        if (mgr->records[i].cleared) {
                                rec = &mgr->records[i];
                                rec->code = code;
                                rec->timestamp = get_timestamp();
                                rec->module_id = module_id;
                                rec->cleared = false;
                                mgr->active_count++;
                                return ERR_OK;
                        }
                }
                /* 无已清除记录，覆盖最旧记录 */
                rec = &mgr->records[0];
                if (!rec->cleared)
                        mgr->active_count--;
                rec->code = code;
                rec->timestamp = get_timestamp();
                rec->module_id = module_id;
                rec->cleared = false;
                mgr->active_count++;
                return ERR_OK;
        }

        /* 添加新记录 */
        rec = &mgr->records[mgr->count];
        rec->code = code;
        rec->timestamp = get_timestamp();
        rec->module_id = module_id;
        rec->cleared = false;
        mgr->count++;
        mgr->active_count++;

        return ERR_OK;
}

int error_manager_clear(struct error_manager *mgr, int code)
{
        int cleared_count = 0;

        if (!mgr || !mgr->initialized)
                return -1;

        for (uint16_t i = 0; i < mgr->count; i++) {
                if (mgr->records[i].code == code &&
                    !mgr->records[i].cleared) {
                        mgr->records[i].cleared = true;
                        cleared_count++;
                        mgr->active_count--;
                }
        }

        /* 如果清除了最后错误，重置为 OK */
        if (mgr->last_error == code && mgr->active_count == 0)
                mgr->last_error = ERR_OK;

        return cleared_count;
}

int error_manager_clear_all(struct error_manager *mgr)
{
        int cleared_count = 0;

        if (!mgr || !mgr->initialized)
                return -1;

        for (uint16_t i = 0; i < mgr->count; i++) {
                if (!mgr->records[i].cleared) {
                        mgr->records[i].cleared = true;
                        cleared_count++;
                }
        }

        mgr->active_count = 0;
        mgr->last_error = ERR_OK;

        return cleared_count;
}

/* ==================== 错误查询实现 ==================== */

bool error_manager_check(const struct error_manager *mgr, int code)
{
        if (!mgr || !mgr->initialized)
                return false;

        for (uint16_t i = 0; i < mgr->count; i++) {
                if (mgr->records[i].code == code &&
                    !mgr->records[i].cleared)
                        return true;
        }

        return false;
}

int error_manager_get_last(const struct error_manager *mgr)
{
        if (!mgr || !mgr->initialized)
                return ERR_NOT_INIT;

        return mgr->last_error;
}

uint16_t error_manager_get_active_count(const struct error_manager *mgr)
{
        if (!mgr || !mgr->initialized)
                return 0;

        return mgr->active_count;
}

bool error_manager_has_error(const struct error_manager *mgr)
{
        if (!mgr || !mgr->initialized)
                return false;

        return mgr->active_count > 0;
}

/* ==================== 错误信息实现 ==================== */

const char *error_get_string(int code)
{
        for (uint32_t i = 0; i < ERROR_TABLE_SIZE; i++) {
                if (error_table[i].code == code)
                        return error_table[i].str;
        }

        return "unknown error";
}

int error_manager_get_record(const struct error_manager *mgr,
                             uint16_t index,
                             struct error_record *record)
{
        if (!mgr || !mgr->initialized || !record)
                return ERR_INVALID_PARAM;

        if (index >= mgr->count)
                return ERR_INVALID_PARAM;

        memcpy(record, &mgr->records[index], sizeof(*record));

        return ERR_OK;
}
