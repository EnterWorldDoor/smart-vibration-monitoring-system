/**
 * @file global_error.h
 * @brief STM32 全局错误码定义与管理系统
 *
 * 提供统一的错误码定义和错误状态管理机制。
 * 所有模块必须使用本文件定义的错误码，禁止自定义错误码。
 *
 * 设计原则:
 *   - 负数表示错误，0 表示成功
 *   - 按模块分区，每个模块占用 100 个错误码范围
 *   - 支持错误记录、查询、清除等运行时管理
 */

#ifndef __GLOBAL_ERROR_H
#define __GLOBAL_ERROR_H

#include <stdint.h>
#include <stdbool.h>

/* ==================== 错误码定义 ==================== */

/* 成功 */
#define ERR_OK                          0

/*
 * 通用错误 (-1 ~ -99)
 * 适用于所有模块的通用错误场景
 */
#define ERR_GENERAL                     -1
#define ERR_INVALID_PARAM               -2
#define ERR_NO_MEM                      -3
#define ERR_TIMEOUT                     -4
#define ERR_NOT_SUPPORTED               -5
#define ERR_BUSY                        -6
#define ERR_ALREADY_INIT                -7
#define ERR_NOT_INIT                    -8
#define ERR_NULL_POINTER                -9

/*
 * 队列错误 (-100 ~ -199)
 * 通用队列操作相关错误
 */
#define ERR_QUEUE_INIT_FAIL             -100
#define ERR_QUEUE_FULL                  -101
#define ERR_QUEUE_EMPTY                 -102
#define ERR_QUEUE_INVALID_SIZE          -103

/*
 * CRC 校验错误 (-200 ~ -299)
 * 数据完整性校验相关错误
 */
#define ERR_CRC_INIT_FAIL               -200
#define ERR_CRC_MISMATCH                -201
#define ERR_CRC_INVALID_PARAM           -202

/*
 * 日志系统错误 (-300 ~ -399)
 * 日志输出相关错误
 */
#define ERR_LOG_INIT_FAIL               -300
#define ERR_LOG_BUFFER_OVERFLOW         -301
#define ERR_LOG_INVALID_LEVEL           -302

/*
 * 传感器错误 (-400 ~ -499)
 * ADXL345 等传感器相关错误
 */
#define ERR_SENSOR_INIT_FAIL            -400
#define ERR_SENSOR_NOT_FOUND            -401
#define ERR_SENSOR_DATA_OVERRUN         -402
#define ERR_SENSOR_COMM_FAIL            -403

/*
 * DHT11 温湿度传感器错误 (-404 ~ -419)
 * DHT11 专用错误码，扩展传感器错误范围
 */
#define ERR_DHT11_INIT_FAIL             -404
#define ERR_DHT11_NOT_INIT              -405
#define ERR_DHT11_INVALID_PARAM         -406
#define ERR_DHT11_TIMEOUT               -407
#define ERR_DHT11_CHECKSUM_ERROR        -408
#define ERR_DHT11_READ_FAIL             -409
#define ERR_DHT11_NO_RESPONSE           -410
#define ERR_DHT11_BUS_BUSY              -411

/*
 * 电机控制错误 (-500 ~ -599)
 * PWM/电机驱动相关错误
 */
#define ERR_MOTOR_INIT_FAIL             -500
#define ERR_MOTOR_SPEED_OVERRUN         -501
#define ERR_MOTOR_DRIVER_FAULT          -502

/*
 * 通信错误 (-600 ~ -699)
 * UART/CAN/SPI/I2C 通信相关错误
 */
#define ERR_COMM_INIT_FAIL              -600
#define ERR_COMM_TX_FAIL                -601
#define ERR_COMM_RX_FAIL                -602
#define ERR_COMM_TIMEOUT                -603

/*
 * 文件系统错误 (-700 ~ -799)
 * FATFS/存储相关错误（预留）
 */
#define ERR_FS_INIT_FAIL                -700
#define ERR_FS_FILE_NOT_FOUND           -701
#define ERR_FS_WRITE_FAIL               -702

/*
 * FreeRTOS 错误 (-800 ~ -899)
 * 操作系统资源相关错误
 */
#define ERR_RTOS_TASK_CREATE_FAIL       -800
#define ERR_RTOS_QUEUE_CREATE_FAIL      -801
#define ERR_RTOS_MUTEX_CREATE_FAIL      -802
#define ERR_RTOS_SEM_CREATE_FAIL        -803

/*
 * 协议栈错误 (-900 ~ -999)
 * UART通信协议相关错误
 */
#define ERR_PROTO_FRAME_OVERFLOW        -900
#define ERR_PROTO_SEND_FAIL             -901
#define ERR_PROTO_INVALID_DATA          -902
#define ERR_PROTO_CRC_MISMATCH          -903
#define ERR_PROTO_ALREADY_INIT          -904
#define ERR_PROTO_NOT_INIT              -905
#define ERR_PROTO_ACK_TIMEOUT           -906
#define ERR_PROTO_NO_DATA               -907
#define ERR_PROTO_BATCH_OVERFLOW        -908
#define ERR_PROTO_CALLBACK_FULL         -909

/* ==================== 错误状态管理 ==================== */

/**
 * struct error_record - 单条错误记录
 * @code: 错误码
 * @timestamp: 发生时间戳 (ms)
 * @module_id: 触发错误的模块 ID
 * @cleared: 是否已被清除
 */
struct error_record {
        int code;
        uint32_t timestamp;
        uint8_t module_id;
        bool cleared;
};

/**
 * struct error_manager - 错误管理器实例
 * @records: 错误记录数组
 * @max_records: 最大记录数
 * @count: 当前记录总数
 * @active_count: 未清除的活跃错误数
 * @last_error: 最近一次错误码
 * @initialized: 初始化标志
 */
struct error_manager {
        struct error_record *records;
        uint16_t max_records;
        uint16_t count;
        uint16_t active_count;
        int last_error;
        bool initialized;
};

/* ==================== 模块 ID 定义 ==================== */

#define MODULE_ID_GLOBAL                0
#define MODULE_ID_QUEUE                 1
#define MODULE_ID_CRC                   2
#define MODULE_ID_LOG                   3
#define MODULE_ID_SENSOR                4
#define MODULE_ID_MOTOR                 5
#define MODULE_ID_COMM                  6
#define MODULE_ID_FS                    7
#define MODULE_ID_RTOS                  8
#define MODULE_ID_APP                   9
#define MODULE_ID_MAX                   10

/* ==================== 生命周期 API ==================== */

/**
 * error_manager_init - 初始化错误管理器
 * @mgr: 错误管理器指针
 * @record_buf: 外部提供的记录缓冲区
 * @max_records: 最大记录数量
 *
 * Return: ERR_OK 成功, 负数错误码
 */
int error_manager_init(struct error_manager *mgr,
                       struct error_record *record_buf,
                       uint16_t max_records);

/**
 * error_manager_deinit - 反初始化错误管理器
 * @mgr: 错误管理器指针
 */
void error_manager_deinit(struct error_manager *mgr);

/* ==================== 错误操作 API ==================== */

/**
 * error_manager_set - 记录一个错误
 * @mgr: 错误管理器指针
 * @code: 错误码
 * @module_id: 模块 ID
 *
 * 将错误码记录到管理器中，并更新最后错误状态。
 *
 * Return: ERR_OK 成功, 负数错误码
 */
int error_manager_set(struct error_manager *mgr, int code,
                      uint8_t module_id);

/**
 * error_manager_clear - 清除指定错误码的所有记录
 * @mgr: 错误管理器指针
 * @code: 要清除的错误码
 *
 * 标记该错误码的所有记录为已清除状态。
 *
 * Return: 被清除的记录数, -1 表示参数错误
 */
int error_manager_clear(struct error_manager *mgr, int code);

/**
 * error_manager_clear_all - 清除所有错误记录
 * @mgr: 错误管理器指针
 *
 * Return: 被清除的记录总数
 */
int error_manager_clear_all(struct error_manager *mgr);

/* ==================== 错误查询 API ==================== */

/**
 * error_manager_check - 检查指定错误是否活跃
 * @mgr: 错误管理器指针
 * @code: 要检查的错误码
 *
 * Return: true 存在未清除的该错误, false 不存在
 */
bool error_manager_check(const struct error_manager *mgr, int code);

/**
 * error_manager_get_last - 获取最近一次错误码
 * @mgr: 错误管理器指针
 *
 * Return: 最后一次错误码, ERR_OK 表示无错误
 */
int error_manager_get_last(const struct error_manager *mgr);

/**
 * error_manager_get_active_count - 获取活跃错误数量
 * @mgr: 错误管理器指针
 *
 * Return: 未清除的错误记录数
 */
uint16_t error_manager_get_active_count(const struct error_manager *mgr);

/**
 * error_manager_has_error - 检查是否存在任何活跃错误
 * @mgr: 错误管理器指针
 *
 * Return: true 存在活跃错误, false 无错误
 */
bool error_manager_has_error(const struct error_manager *mgr);

/* ==================== 错误信息 API ==================== */

/**
 * error_get_string - 获取错误码对应的描述字符串
 * @code: 错误码
 *
 * Return: 错误描述字符串指针
 */
const char *error_get_string(int code);

/**
 * error_manager_get_record - 获取指定索引的错误记录
 * @mgr: 错误管理器指针
 * @index: 记录索引 (0 ~ count-1)
 * @record: 输出记录结构体
 *
 * Return: ERR_OK 成功, 负数错误码
 */
int error_manager_get_record(const struct error_manager *mgr,
                             uint16_t index,
                             struct error_record *record);

/* ==================== 时间戳配置 API ==================== */

/**
 * error_set_time_callback - 设置时间戳回调函数
 * @cb: 返回当前时间戳 (ms) 的函数指针
 *
 * 允许上层设置自定义的时间戳源（如 HAL_GetTick()）。
 */
void error_set_time_callback(uint32_t (*cb)(void));

#endif /* __GLOBAL_ERROR_H */
