/**
 * @file ringbuf.c
 * @author EnterWorldDoor
 * @brief 企业级环形缓冲区实现（线程安全、统计监控、覆盖模式）
 *
 * 设计要点:
 *   - 虚拟索引: head/tail 无限递增，通过 (head-tail) 判断已用空间
 *   - 优势: 无回绕歧义，空/满判断精确
 *   - 线程安全: FreeRTOS Mutex 保护所有共享状态
 *   - 覆盖模式: 满时自动丢弃最旧数据，一次性腾出足够空间
 */

#include "ringbuf.h"
#include <string.h>

/* ==================== 内部辅助函数 ==================== */

/**
 * get_used_internal - 获取当前已用字节数 (调用前需持有 mutex)
 */
static inline size_t get_used_internal(const struct ringbuf *rb)
{
    return rb->head - rb->tail;
}

/**
 * write_to_buffer - 向底层 buffer 写入数据 (处理环形边界)
 * @rb: ringbuf 实例
 * @virtual_pos: 虚拟写入位置
 * @data: 数据源
 * @len: 写入长度
 */
static void write_to_buffer(struct ringbuf *rb, size_t virtual_pos,
                           const uint8_t *data, size_t len)
{
    size_t pos = virtual_pos % rb->size;
    size_t first_part = rb->size - pos;

    if (len <= first_part) {
        memcpy(&rb->buffer[pos], data, len);
    } else {
        memcpy(&rb->buffer[pos], data, first_part);
        memcpy(&rb->buffer[0], &data[first_part], len - first_part);
    }
}

/**
 * read_from_buffer - 从底层 buffer 读取数据 (处理环形边界)
 */
static void read_from_buffer(const struct ringbuf *rb, size_t virtual_pos,
                             uint8_t *data, size_t len)
{
    size_t pos = virtual_pos % rb->size;
    size_t first_part = rb->size - pos;

    if (len <= first_part) {
        memcpy(data, &rb->buffer[pos], len);
    } else {
        memcpy(data, &rb->buffer[pos], first_part);
        memcpy(&data[first_part], &rb->buffer[0], len - first_part);
    }
}

/**
 * update_max_used - 更新历史最大占用记录
 */
static void update_max_used(struct ringbuf *rb)
{
    size_t used = get_used_internal(rb);
    if (used > rb->stats.max_used) {
        rb->stats.max_used = used;
    }
}

/* ==================== 生命周期 API ==================== */

int ringbuf_init(struct ringbuf *rb, uint8_t *buffer, size_t size,
                 bool overwrite)
{
    if (!rb || !buffer || size == 0) return APP_ERR_INVALID_PARAM;
    if (size > RINGBUF_MAX_SIZE) return APP_ERR_INVALID_PARAM;

    memset(rb, 0, sizeof(*rb));

    rb->buffer = buffer;
    rb->size = size;
    rb->overwrite = overwrite;
    rb->initialized = true;

    rb->mutex = xSemaphoreCreateMutex();
    if (!rb->mutex) {
        rb->initialized = false;
        return APP_ERR_NO_MEM;
    }

    return APP_ERR_OK;
}

void ringbuf_deinit(struct ringbuf *rb)
{
    if (!rb || !rb->initialized) return;

    if (rb->mutex) {
        vSemaphoreDelete(rb->mutex);
        rb->mutex = NULL;
    }

    rb->buffer = NULL;
    rb->size = 0;
    rb->head = 0;
    rb->tail = 0;
    rb->initialized = false;
}

bool ringbuf_is_initialized(const struct ringbuf *rb)
{
    return (rb != NULL && rb->initialized);
}

void ringbuf_reset(struct ringbuf *rb)
{
    if (!rb || !rb->initialized) return;

    xSemaphoreTake(rb->mutex, portMAX_DELAY);
    rb->head = 0;
    rb->tail = 0;
    xSemaphoreGive(rb->mutex);
}

/* ==================== 数据操作 API ==================== */

size_t ringbuf_push(struct ringbuf *rb, const uint8_t *data, size_t len)
{
    if (!rb || !rb->initialized || !rb->buffer || !data || len == 0) {
        return 0;
    }

    xSemaphoreTake(rb->mutex, portMAX_DELAY);

    size_t used = get_used_internal(rb);
    size_t available = rb->size - used;
    size_t write_len = len;
    size_t overflow_bytes = 0;

    if (available == 0) {
        if (!rb->overwrite) {
            rb->stats.underflow_count++;
            xSemaphoreGive(rb->mutex);
            return 0;
        }
        if (write_len > rb->size - 1) {
            write_len = rb->size - 1;
        }
        overflow_bytes = write_len;
        rb->tail += write_len;
        available = write_len;
    } else if (write_len > available) {
        if (rb->overwrite) {
            size_t extra = write_len - available;
            if (available + extra > rb->size - 1) {
                extra = (rb->size - 1) - available;
                write_len = rb->size - 1;
            }
            overflow_bytes = extra;
            rb->tail += extra;
            available = write_len;
        } else {
            write_len = available;
        }
    }

    write_to_buffer(rb, rb->head, data, write_len);
    rb->head += write_len;

    rb->stats.push_count++;
    rb->stats.push_bytes += write_len;
    rb->stats.overflow_count += overflow_bytes;
    update_max_used(rb);

    xSemaphoreGive(rb->mutex);
    return write_len;
}

size_t ringbuf_pop(struct ringbuf *rb, uint8_t *data, size_t len)
{
    if (!rb || !rb->initialized || !rb->buffer || !data || len == 0) {
        return 0;
    }

    xSemaphoreTake(rb->mutex, portMAX_DELAY);

    size_t used = get_used_internal(rb);
    if (used == 0) {
        rb->stats.underflow_count++;
        xSemaphoreGive(rb->mutex);
        return 0;
    }

    size_t read_len = (len > used) ? used : len;

    read_from_buffer(rb, rb->tail, data, read_len);
    rb->tail += read_len;

    rb->stats.pop_count++;
    rb->stats.pop_bytes += read_len;

    xSemaphoreGive(rb->mutex);
    return read_len;
}

size_t ringbuf_pop_timeout(struct ringbuf *rb, uint8_t *data, size_t len,
                             uint32_t timeout_ms)
{
    if (!rb || !rb->initialized || !rb->buffer || !data || len == 0) {
        return 0;
    }

    /*
     * timeout_ms == 0: 非阻塞, 尝试一次就返回
     */
    if (timeout_ms == 0) {
        if (xSemaphoreTake(rb->mutex, 0) != pdTRUE) return 0;
        size_t used = get_used_internal(rb);
        if (used == 0) {
            xSemaphoreGive(rb->mutex);
            return 0;
        }
        size_t read_len = (len > used) ? used : len;
        read_from_buffer(rb, rb->tail, data, read_len);
        rb->tail += read_len;
        rb->stats.pop_count++;
        rb->stats.pop_bytes += read_len;
        xSemaphoreGive(rb->mutex);
        return read_len;
    }

    /*
     * ⚠️ 【可靠轮询方案】用10ms时间片轮询 + vTaskDelay 实现阻塞等待
     *
     * 注意: FreeRTOS 100Hz tick下最小可靠延迟=10ms
     * fetch_batch已通过两阶段读取(先非阻塞排空)来规避此瓶颈
     * 此处仅作安全兜底, 极少被调用
     */
    TickType_t poll_interval = pdMS_TO_TICKS(10);
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (xTaskGetTickCount() < deadline) {
        if (xSemaphoreTake(rb->mutex, poll_interval) != pdTRUE) {
            continue;
        }

        size_t used = get_used_internal(rb);
        if (used > 0) {
            size_t read_len = (len > used) ? used : len;
            read_from_buffer(rb, rb->tail, data, read_len);
            rb->tail += read_len;
            rb->stats.pop_count++;
            rb->stats.pop_bytes += read_len;
            xSemaphoreGive(rb->mutex);
            return read_len;
        }

        xSemaphoreGive(rb->mutex);
        vTaskDelay(poll_interval);
    }

    rb->stats.underflow_count++;
    return 0;
}

size_t ringbuf_peek(struct ringbuf *rb, uint8_t *data, size_t len)
{
    if (!rb || !rb->initialized || !rb->buffer || !data || len == 0) {
        return 0;
    }

    xSemaphoreTake(rb->mutex, portMAX_DELAY);

    size_t used = get_used_internal(rb);
    if (used == 0) {
        xSemaphoreGive(rb->mutex);
        return 0;
    }

    size_t peek_len = (len > used) ? used : len;
    read_from_buffer(rb, rb->tail, data, peek_len);

    rb->stats.peek_count++;
    xSemaphoreGive(rb->mutex);
    return peek_len;
}

size_t ringbuf_peek_offset(struct ringbuf *rb, size_t offset,
                           uint8_t *data, size_t len)
{
    if (!rb || !rb->initialized || !rb->buffer || !data || len == 0) {
        return 0;
    }

    xSemaphoreTake(rb->mutex, portMAX_DELAY);

    size_t used = get_used_internal(rb);
    if (offset >= used) {
        xSemaphoreGive(rb->mutex);
        return 0;
    }

    size_t remaining = used - offset;
    size_t peek_len = (len > remaining) ? remaining : len;

    read_from_buffer(rb, rb->tail + offset, data, peek_len);

    rb->stats.peek_count++;
    xSemaphoreGive(rb->mutex);
    return peek_len;
}

size_t ringbuf_drop(struct ringbuf *rb, size_t len)
{
    if (!rb || !rb->initialized || len == 0) {
        return 0;
    }

    xSemaphoreTake(rb->mutex, portMAX_DELAY);

    size_t used = get_used_internal(rb);
    if (used == 0) {
        rb->stats.underflow_count++;
        xSemaphoreGive(rb->mutex);
        return 0;
    }

    size_t drop_len = (len > used) ? used : len;
    rb->tail += drop_len;

    rb->stats.pop_count++;

    xSemaphoreGive(rb->mutex);
    return drop_len;
}

/* ==================== 查询 API ==================== */

size_t ringbuf_available(const struct ringbuf *rb)
{
    if (!rb || !rb->initialized) return 0;

    xSemaphoreTake(rb->mutex, portMAX_DELAY);
    size_t avail = rb->size - get_used_internal(rb);
    xSemaphoreGive(rb->mutex);
    return avail;
}

size_t ringbuf_used(const struct ringbuf *rb)
{
    if (!rb || !rb->initialized) return 0;

    xSemaphoreTake(rb->mutex, portMAX_DELAY);
    size_t used = get_used_internal(rb);
    xSemaphoreGive(rb->mutex);
    return used;
}

bool ringbuf_is_empty(const struct ringbuf *rb)
{
    if (!rb || !rb->initialized) return true;

    xSemaphoreTake(rb->mutex, portMAX_DELAY);
    bool empty = (get_used_internal(rb) == 0);
    xSemaphoreGive(rb->mutex);
    return empty;
}

bool ringbuf_is_full(const struct ringbuf *rb)
{
    if (!rb || !rb->initialized) return true;

    xSemaphoreTake(rb->mutex, portMAX_DELAY);
    bool full = (get_used_internal(rb) >= rb->size);
    xSemaphoreGive(rb->mutex);
    return full;
}

size_t ringbuf_capacity(const struct ringbuf *rb)
{
    if (!rb || !rb->initialized) return 0;
    return rb->size;
}

/* ==================== 统计 API ==================== */

int ringbuf_get_stats(const struct ringbuf *rb, struct ringbuf_stats *stats)
{
    if (!rb || !rb->initialized || !stats) {
        return APP_ERR_INVALID_PARAM;
    }

    xSemaphoreTake(rb->mutex, portMAX_DELAY);
    memcpy(stats, &rb->stats, sizeof(*stats));
    xSemaphoreGive(rb->mutex);
    return APP_ERR_OK;
}

void ringbuf_reset_stats(struct ringbuf *rb)
{
    if (!rb || !rb->initialized) return;

    xSemaphoreTake(rb->mutex, portMAX_DELAY);
    memset(&rb->stats, 0, sizeof(rb->stats));
    xSemaphoreGive(rb->mutex);
}