/**
 * @file queue.h
 * @brief 通用环形队列（静态内存、泛型支持、线程安全可选）
 *
 * 适用场景:
 *   - 传感器数据缓冲（ADXL345 加速度采样）
 *   - 任务间数据传递
 *   - UART/SPI DMA 接收缓存
 *
 * 设计特点:
 *   - 纯软件实现，无硬件依赖
 *   - 支持任意数据类型（通过元素大小参数化）
 *   - 可选覆盖模式（旧数据覆盖 vs 拒绝新数据）
 *   - 提供运行时统计信息
 */

#ifndef __QUEUE_H
#define __QUEUE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "global_error/global_error.h"

#define QUEUE_MAX_CAPACITY       1024    /**< 最大队列容量 */
#define QUEUE_DEFAULT_TIMEOUT    100     /**< 默认超时时间 (ms) */

/**
 * struct queue_stats - 队列运行统计
 */
struct queue_stats {
        uint64_t enqueue_count;           /**< 入队总次数 */
        uint64_t dequeue_count;           /**< 出队总次数 */
        uint64_t enqueue_fail_count;      /**< 入队失败次数（满） */
        uint64_t dequeue_fail_count;      /**< 出队失败次数（空） */
        uint64_t overflow_count;          /**< 覆盖模式下被覆盖的元素数 */
        uint32_t peak_usage;              /**< 历史最大使用量 */
};

/**
 * struct queue - 队列实例
 * @buffer: 底层存储缓冲区 (外部分配)
 * @capacity: 队列容量 (元素个数)
 * @item_size: 单个元素大小 (字节)
 * @head: 写指针索引
 * @tail: 读指针索引
 * @count: 当前元素个数
 * @overwrite: true=覆盖模式, false=拒绝模式
 * @initialized: 初始化标志
 * @stats: 运行统计
 */
struct queue {
        void *buffer;
        uint16_t capacity;
        uint16_t item_size;
        volatile uint16_t head;
        volatile uint16_t tail;
        volatile uint16_t count;
        bool overwrite;
        bool initialized;
        struct queue_stats stats;
};

/* ==================== 生命周期 API ==================== */

/**
 * queue_init - 初始化队列
 * @q: 队列结构体指针
 * @buffer: 外部提供的存储缓冲区
 * @capacity: 队列容量 (最大元素个数)
 * @item_size: 单个元素大小 (字节)
 * @overwrite: true 允许覆盖旧数据, false 满时拒绝入队
 *
 * Return: ERR_OK 成功, 负数错误码
 */
int queue_init(struct queue *q, void *buffer,
               uint16_t capacity, uint16_t item_size,
               bool overwrite);

/**
 * queue_deinit - 反初始化队列
 * @q: 队列结构体指针
 *
 * 注意: 不释放 buffer 内存
 */
void queue_deinit(struct queue *q);

/**
 * queue_is_initialized - 查询是否已初始化
 * @q: 队列结构体指针
 * Return: true 已初始化, false 未初始化
 */
bool queue_is_initialized(const struct queue *q);

/**
 * queue_reset - 清空队列（重置状态，保留配置）
 * @q: 队列结构体指针
 */
void queue_reset(struct queue *q);

/* ==================== 数据操作 API ==================== */

/**
 * queue_enqueue - 入队操作
 * @q: 队列结构体指针
 * @item: 待入队的数据指针
 *
 * 非覆盖模式: 队列满时返回 ERR_QUEUE_FULL
 * 覆盖模式: 自动覆盖最旧的元素
 *
 * Return: ERR_OK 成功, 负数错误码
 */
int queue_enqueue(struct queue *q, const void *item);

/**
 * queue_enqueue_batch - 批量入队
 * @q: 队列结构体指针
 * @items: 数据数组指针
 * @count: 元素个数
 *
 * Return: 实际成功入队的元素个数
 */
uint16_t queue_enqueue_batch(struct queue *q, const void *items,
                             uint16_t count);

/**
 * queue_dequeue - 出队操作
 * @q: 队列结构体指针
 * @item: 输出缓冲区指针
 *
 * Return: ERR_OK 成功, 负数错误码
 */
int queue_dequeue(struct queue *q, void *item);

/**
 * queue_dequeue_batch - 批量出队
 * @q: 队列结构体指针
 * @items: 输出缓冲区指针
 * @max_count: 最大出队个数
 *
 * Return: 实际出队的元素个数
 */
uint16_t queue_dequeue_batch(struct queue *q, void *items,
                             uint16_t max_count);

/**
 * queue_peek - 查看队首元素（不弹出）
 * @q: 队列结构体指针
 * @item: 输出缓冲区指针
 *
 * Return: ERR_OK 成功, 负数错误码
 */
int queue_peek(const struct queue *q, void *item);

/* ==================== 查询 API ==================== */

/**
 * queue_get_count - 获取当前元素个数
 * @q: 队列结构体指针
 * Return: 当前元素数
 */
uint16_t queue_get_count(const struct queue *q);

/**
 * queue_get_available - 获取剩余空间
 * @q: 队列结构体指针
 * Return: 可入队的元素数
 */
uint16_t queue_get_available(const struct queue *q);

/**
 * queue_get_capacity - 获取队列容量
 * @q: 队列结构体指针
 * Return: 总容量
 */
uint16_t queue_get_capacity(const struct queue *q);

/**
 * queue_is_empty - 队列是否为空
 * @q: 队列结构体指针
 * Return: true 空, false 非空
 */
bool queue_is_empty(const struct queue *q);

/**
 * queue_is_full - 队列是否已满
 * @q: 队列结构体指针
 * Return: true 满, false 未满
 */
bool queue_is_full(const struct queue *q);

/* ==================== 统计 API ==================== */

/**
 * queue_get_stats - 获取运行统计快照
 * @q: 队列结构体指针
 * @stats: 输出统计结构体
 * Return: ERR_OK or error code
 */
int queue_get_stats(const struct queue *q, struct queue_stats *stats);

/**
 * queue_reset_stats - 重置所有统计计数器
 * @q: 队列结构体指针
 */
void queue_reset_stats(struct queue *q);

#endif /* __QUEUE_H */
