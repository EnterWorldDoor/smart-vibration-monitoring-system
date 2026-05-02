/**
 * @file queue.c
 * @brief 通用环形队列实现
 *
 * 采用索引循环设计，支持覆盖模式和批量操作。
 * 所有操作均为 O(1) 时间复杂度（除批量操作外）。
 */

#include "queue.h"
#include <string.h>

/* ==================== 生命周期实现 ==================== */

int queue_init(struct queue *q, void *buffer,
               uint16_t capacity, uint16_t item_size,
               bool overwrite)
{
        if (!q || !buffer)
                return ERR_NULL_POINTER;

        if (capacity == 0 || capacity > QUEUE_MAX_CAPACITY)
                return ERR_INVALID_PARAM;

        if (item_size == 0)
                return ERR_INVALID_PARAM;

        memset(q, 0, sizeof(*q));
        q->buffer = buffer;
        q->capacity = capacity;
        q->item_size = item_size;
        q->overwrite = overwrite;
        q->initialized = true;

        return ERR_OK;
}

void queue_deinit(struct queue *q)
{
        if (!q || !q->initialized)
                return;

        memset(q, 0, sizeof(*q));
}

bool queue_is_initialized(const struct queue *q)
{
        return q && q->initialized;
}

void queue_reset(struct queue *q)
{
        if (!q || !q->initialized)
                return;

        q->head = 0;
        q->tail = 0;
        q->count = 0;
        memset(&q->stats, 0, sizeof(q->stats));
}

/* ==================== 数据操作实现 ==================== */

int queue_enqueue(struct queue *q, const void *item)
{
        uint8_t *dst;

        if (!q || !q->initialized)
                return ERR_NOT_INIT;

        if (!item)
                return ERR_NULL_POINTER;

        /* 非覆盖模式且队列已满 */
        if (!q->overwrite && q->count >= q->capacity) {
                q->stats.enqueue_fail_count++;
                return ERR_QUEUE_FULL;
        }

        /* 写入数据到 head 位置 */
        dst = (uint8_t *)q->buffer +
              ((size_t)q->head * q->item_size);
        memcpy(dst, item, q->item_size);

        /* 更新 head 指针 */
        q->head = (q->head + 1) % q->capacity;

        /* 覆盖模式：如果满了，移动 tail */
        if (q->count >= q->capacity) {
                q->tail = (q->tail + 1) % q->capacity;
                q->stats.overflow_count++;
        } else {
                q->count++;
        }

        q->stats.enqueue_count++;

        /* 更新峰值使用量 */
        if (q->count > q->stats.peak_usage)
                q->stats.peak_usage = q->count;

        return ERR_OK;
}

uint16_t queue_enqueue_batch(struct queue *q, const void *items,
                             uint16_t count)
{
        const uint8_t *src = items;
        uint16_t enqueued = 0;

        if (!q || !q->initialized || !items)
                return 0;

        for (uint16_t i = 0; i < count; i++) {
                int ret = queue_enqueue(q, src + (size_t)i * q->item_size);
                if (ret != ERR_OK && !q->overwrite)
                        break;
                enqueued++;
        }

        return enqueued;
}

int queue_dequeue(struct queue *q, void *item)
{
        const uint8_t *src;

        if (!q || !q->initialized)
                return ERR_NOT_INIT;

        if (!item)
                return ERR_NULL_POINTER;

        if (q->count == 0) {
                q->stats.dequeue_fail_count++;
                return ERR_QUEUE_EMPTY;
        }

        /* 从 tail 位置读取数据 */
        src = (const uint8_t *)q->buffer +
              ((size_t)q->tail * q->item_size);
        memcpy(item, src, q->item_size);

        /* 更新 tail 指针 */
        q->tail = (q->tail + 1) % q->capacity;
        q->count--;

        q->stats.dequeue_count++;

        return ERR_OK;
}

uint16_t queue_dequeue_batch(struct queue *q, void *items,
                             uint16_t max_count)
{
        uint8_t *dst = items;
        uint16_t dequeued = 0;

        if (!q || !q->initialized || !items)
                return 0;

        for (uint16_t i = 0; i < max_count; i++) {
                int ret = queue_dequeue(q,
                                        dst + (size_t)i * q->item_size);
                if (ret != ERR_OK)
                        break;
                dequeued++;
        }

        return dequeued;
}

int queue_peek(const struct queue *q, void *item)
{
        const uint8_t *src;

        if (!q || !q->initialized)
                return ERR_NOT_INIT;

        if (!item)
                return ERR_NULL_POINTER;

        if (q->count == 0)
                return ERR_QUEUE_EMPTY;

        src = (const uint8_t *)q->buffer +
              ((size_t)q->tail * q->item_size);
        memcpy(item, src, q->item_size);

        return ERR_OK;
}

/* ==================== 查询 API 实现 ==================== */

uint16_t queue_get_count(const struct queue *q)
{
        if (!q || !q->initialized)
                return 0;

        return q->count;
}

uint16_t queue_get_available(const struct queue *q)
{
        if (!q || !q->initialized)
                return 0;

        return q->capacity - q->count;
}

uint16_t queue_get_capacity(const struct queue *q)
{
        if (!q || !q->initialized)
                return 0;

        return q->capacity;
}

bool queue_is_empty(const struct queue *q)
{
        if (!q || !q->initialized)
                return true;

        return q->count == 0;
}

bool queue_is_full(const struct queue *q)
{
        if (!q || !q->initialized)
                return false;

        return q->count >= q->capacity;
}

/* ==================== 统计 API 实现 ==================== */

int queue_get_stats(const struct queue *q, struct queue_stats *stats)
{
        if (!q || !q->initialized || !stats)
                return ERR_INVALID_PARAM;

        memcpy(stats, &q->stats, sizeof(*stats));

        return ERR_OK;
}

void queue_reset_stats(struct queue *q)
{
        if (!q || !q->initialized)
                return;

        memset(&q->stats, 0, sizeof(q->stats));
}
