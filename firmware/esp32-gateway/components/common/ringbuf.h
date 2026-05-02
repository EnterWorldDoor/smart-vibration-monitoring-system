/**
 * @file ringbuf.h
 * @author EnterWorldDoor
 * @brief 企业级通用环形缓冲区（线程安全、统计监控、覆盖模式）
 *
 * 适用场景:
 *   - ADXL345 加速度计高速数据采集缓存
 *   - UART/SPI DMA 接收缓冲
 *   - 生产者-消费者模型数据管道
 *
 * 功能特性:
 *   - FreeRTOS Mutex 线程安全
 *   - 可选覆盖模式（旧数据覆盖 vs 丢弃新数据）
 *   - 虚拟索引设计（head/tail 无限递增，无回绕歧义）
 *   - 完整统计计数器（push/pop/overflow/underflow）
 *   - 丰富的查询 API（available/used/full/empty）
 */

#ifndef COMMON_RINGBUF_H
#define COMMON_RINGBUF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "global_error.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define RINGBUF_MAX_SIZE         65536    /**< 最大缓冲区大小 (64KB) */
#define RINGBUF_DEFAULT_TIMEOUT  100      /**< 默认 Mutex 超时 (ms) */

/**
 * struct ringbuf_stats - 环形缓冲区运行统计
 */
struct ringbuf_stats {
    uint64_t push_count;            /**< push 调用总次数 */
    uint64_t pop_count;             /**< pop 调用总次数 */
    uint64_t peek_count;            /**< peek 调用总次数 */
    uint64_t push_bytes;            /**< push 写入总字节数 */
    uint64_t pop_bytes;             /**< pop 读取总字节数 */
    uint64_t overflow_count;        /**< 覆盖模式下被覆盖的字节数 */
    uint64_t underflow_count;       /**< pop 时缓冲区为空的次数 */
    uint64_t max_used;              /**< 历史最大占用字节数 */
};

/**
 * struct ringbuf - 环形缓冲区实例
 */
struct ringbuf {
    uint8_t *buffer;                /**< 底层存储数组 (外部分配) */
    size_t size;                   /**< 缓冲区容量 (字节) */
    volatile size_t head;           /**< 写指针 (虚拟索引) */
    volatile size_t tail;           /**< 读指针 (虚拟索引) */
    SemaphoreHandle_t mutex;        /**< 互斥锁 (线程安全) */
    bool overwrite;                /**< true=覆盖旧数据, false=拒绝写入 */
    bool initialized;              /**< 初始化标志 */
    struct ringbuf_stats stats;    /**< 运行统计 */
};

/* ==================== 生命周期 API ==================== */

/**
 * ringbuf_init - 初始化环形缓冲区
 * @rb: ringbuf 结构体指针
 * @buffer: 外部提供的内存缓冲区 (调用者负责生命周期)
 * @size: 缓冲区大小 (字节, 推荐 2^n)
 * @overwrite: true 允许覆盖旧数据, false 缓冲区满时拒绝写入
 *
 * Return: APP_ERR_OK 成功, 负数错误码
 */
int ringbuf_init(struct ringbuf *rb, uint8_t *buffer, size_t size,
                 bool overwrite);

/**
 * ringbuf_deinit - 反初始化环形缓冲区 (释放互斥量)
 * @rb: ringbuf 结构体指针
 *
 * 注意: 不释放 buffer 内存 (由调用者管理)
 */
void ringbuf_deinit(struct ringbuf *rb);

/**
 * ringbuf_is_initialized - 查询是否已初始化
 * @rb: ringbuf 结构体指针
 * Return: true 已初始化, false 未初始化
 */
bool ringbuf_is_initialized(const struct ringbuf *rb);

/**
 * ringbuf_reset - 清空缓冲区 (重置头尾指针，保留配置)
 * @rb: ringbuf 结构体指针
 */
void ringbuf_reset(struct ringbuf *rb);

/* ==================== 数据操作 API ==================== */

/**
 * ringbuf_push - 推送数据到缓冲区尾部
 * @rb: ringbuf 结构体指针
 * @data: 待写入数据
 * @len: 待写入长度 (字节)
 *
 * 非覆盖模式: 缓冲区满时返回 0
 * 覆盖模式: 自动覆盖最旧的数据
 *
 * Return: 实际写入字节数
 */
size_t ringbuf_push(struct ringbuf *rb, const uint8_t *data, size_t len);

/**
 * ringbuf_pop - 从缓冲区头部弹出数据
 * @rb: ringbuf 结构体指针
 * @data: 输出缓冲区
 * @len: 期望读取长度 (字节)
 *
 * Return: 实际读取字节数 (不足表示缓冲区数据不够)
 */
size_t ringbuf_pop(struct ringbuf *rb, uint8_t *data, size_t len);

/**
 * ringbuf_pop_timeout - 从缓冲区头部弹出数据（带超时）
 * @rb: ringbuf 结构体指针
 * @data: 输出缓冲区
 * @len: 期望读取长度 (字节)
 * @timeout_ms: 超时时间 (毫秒)
 *
 * 阻塞等待直到有数据可读或超时。
 * 用于 ADXL345 等需要同步获取数据的场景。
 *
 * Return: 实际读取字节数 (0 表示超时)
 */
size_t ringbuf_pop_timeout(struct ringbuf *rb, uint8_t *data, size_t len,
                           uint32_t timeout_ms);

/**
 * ringbuf_peek - 查看数据但不弹出 (不影响头尾指针)
 * @rb: ringbuf 结构体指针
 * @data: 输出缓冲区
 * @len: 期望查看长度 (字节)
 *
 * Return: 实际可查看字节数
 */
size_t ringbuf_peek(struct ringbuf *rb, uint8_t *data, size_t len);

/**
 * ringbuf_peek_offset - 从指定偏移处查看数据 (不弹出)
 * @rb: ringbuf 结构体指针
 * @offset: 从当前位置偏移多少字节
 * @data: 输出缓冲区
 * @len: 期望查看长度
 *
 * 用于 ADXL345 场景: 不消费数据的情况下查看历史样本
 *
 * Return: 实际可查看字节数
 */
size_t ringbuf_peek_offset(struct ringbuf *rb, size_t offset,
                           uint8_t *data, size_t len);

/**
 * ringbuf_drop - 丢弃指定长度数据 (跳过不读取)
 * @rb: ringbuf 结构体指针
 * @len: 要丢弃的字节数
 *
 * Return: 实际丢弃字节数
 */
size_t ringbuf_drop(struct ringbuf *rb, size_t len);

/* ==================== 查询 API ==================== */

/**
 * ringbuf_available - 获取当前可用空间 (可写入字节数)
 * @rb: ringbuf 结构体指针
 * Return: 剩余可写字节数
 */
size_t ringbuf_available(const struct ringbuf *rb);

/**
 * ringbuf_used - 获取当前已存储数据量 (可读取字节数)
 * @rb: ringbuf 结构体指针
 * Return: 已存储字节数
 */
size_t ringbuf_used(const struct ringbuf *rb);

/**
 * ringbuf_is_empty - 缓冲区是否为空
 * @rb: ringbuf 结构体指针
 * Return: true 空, false 非空
 */
bool ringbuf_is_empty(const struct ringbuf *rb);

/**
 * ringbuf_is_full - 缓冲区是否已满
 * @rb: ringbuf 结构体指针
 * Return: true 满, false 未满
 */
bool ringbuf_is_full(const struct ringbuf *rb);

/**
 * ringbuf_capacity - 获取缓冲区总容量
 * @rb: ringbuf 结构体指针
 * Return: 容量 (字节)
 */
size_t ringbuf_capacity(const struct ringbuf *rb);

/* ==================== 统计 API ==================== */

/**
 * ringbuf_get_stats - 获取运行统计快照
 * @rb: ringbuf 结构体指针
 * @stats: 输出统计结构体
 * Return: APP_ERR_OK or error code
 */
int ringbuf_get_stats(const struct ringbuf *rb, struct ringbuf_stats *stats);

/**
 * ringbuf_reset_stats - 重置所有统计计数器
 * @rb: ringbuf 结构体指针
 */
void ringbuf_reset_stats(struct ringbuf *rb);

#endif /* COMMON_RINGBUF_H */