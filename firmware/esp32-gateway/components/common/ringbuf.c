/**
 * @file ringbuf.c
 * @author EnterWorldDoor
 * @brief 环形缓冲区实现
 */

#include "ringbuf.h"
#include <string.h>

/**
 * @brief 初始化环形缓冲区
 * @param rb 环形缓冲区指针
 * @param buffer 缓冲区内存
 * @param size 缓冲区大小
 * @param overwrite 是否覆盖模式
 * @return 0 on success, negative error code on failure
 */
int ringbuf_init(struct ringbuf *rb, uint8_t *buffer, size_t size, bool overwrite)
{
    if (!rb || !buffer || size == 0) return -1;
    
    rb->buffer = buffer;
    rb->size = size;
    rb->head = 0;
    rb->tail = 0;
    rb->overwrite = overwrite;
    
    // 创建互斥锁
    rb->mutex = xSemaphoreCreateMutex();
    if (!rb->mutex) {
        return -2;
    }
    
    return 0;
}

/**
 * @brief 向环形缓冲区推送数据
 * @param rb 环形缓冲区指针
 * @param data 数据指针
 * @param len 数据长度
 * @return 实际写入的字节数
 */
size_t ringbuf_push(struct ringbuf *rb, const uint8_t *data, size_t len)
{
    if (!rb || !rb->buffer || !data || len == 0) return 0;
    
    // 获取互斥锁
    if (rb->mutex) {
        xSemaphoreTake(rb->mutex, portMAX_DELAY);
    }
    
    size_t available = rb->size - (rb->head - rb->tail);
    if (available == 0) {
        if (!rb->overwrite) {
            if (rb->mutex) {
                xSemaphoreGive(rb->mutex);
            }
            return 0;
        }
        // 覆盖模式下，移动 tail 以腾出空间
        rb->tail = rb->head - (rb->size - 1);
        available = 1;
    }
    
    size_t write_len = len;
    if (write_len > available) {
        write_len = available;
    }
    
    // 分两次写入（如果需要）
    size_t first_part = rb->size - (rb->head % rb->size);
    if (write_len <= first_part) {
        memcpy(&rb->buffer[rb->head % rb->size], data, write_len);
    } else {
        memcpy(&rb->buffer[rb->head % rb->size], data, first_part);
        memcpy(&rb->buffer[0], &data[first_part], write_len - first_part);
    }
    
    rb->head += write_len;
    
    // 释放互斥锁
    if (rb->mutex) {
        xSemaphoreGive(rb->mutex);
    }
    
    return write_len;
}

/**
 * @brief 从环形缓冲区弹出数据
 * @param rb 环形缓冲区指针
 * @param data 数据指针
 * @param len 数据长度
 * @return 实际读取的字节数
 */
size_t ringbuf_pop(struct ringbuf *rb, uint8_t *data, size_t len)
{
    if (!rb || !rb->buffer || !data || len == 0) return 0;
    
    // 获取互斥锁
    if (rb->mutex) {
        xSemaphoreTake(rb->mutex, portMAX_DELAY);
    }
    
    size_t available = rb->head - rb->tail;
    if (available == 0) {
        if (rb->mutex) {
            xSemaphoreGive(rb->mutex);
        }
        return 0;
    }
    
    size_t read_len = len;
    if (read_len > available) {
        read_len = available;
    }
    
    // 分两次读取（如果需要）
    size_t first_part = rb->size - (rb->tail % rb->size);
    if (read_len <= first_part) {
        memcpy(data, &rb->buffer[rb->tail % rb->size], read_len);
    } else {
        memcpy(data, &rb->buffer[rb->tail % rb->size], first_part);
        memcpy(&data[first_part], &rb->buffer[0], read_len - first_part);
    }
    
    rb->tail += read_len;
    
    // 释放互斥锁
    if (rb->mutex) {
        xSemaphoreGive(rb->mutex);
    }
    
    return read_len;
}

/**
 * @brief 查看环形缓冲区中的数据但不弹出
 * @param rb 环形缓冲区指针
 * @param data 存储查看数据的指针
 * @param len 要查看的数据长度
 * @return 实际查看的数据长度
 */
size_t ringbuf_peek(struct ringbuf *rb, uint8_t *data, size_t len)
{
    if (!rb || !rb->buffer || !data || len == 0) return 0;
    
    // 获取互斥锁
    if (rb->mutex) {
        xSemaphoreTake(rb->mutex, portMAX_DELAY);
    }
    
    size_t available = rb->head - rb->tail;
    if (available == 0) {
        if (rb->mutex) {
            xSemaphoreGive(rb->mutex);
        }
        return 0;
    }
    
    size_t read_len = len;
    if (read_len > available) {
        read_len = available;
    }
    
    // 分两次读取（如果需要）
    size_t first_part = rb->size - (rb->tail % rb->size);
    if (read_len <= first_part) {
        memcpy(data, &rb->buffer[rb->tail % rb->size], read_len);
    } else {
        memcpy(data, &rb->buffer[rb->tail % rb->size], first_part);
        memcpy(&data[first_part], &rb->buffer[0], read_len - first_part);
    }
    
    // 释放互斥锁
    if (rb->mutex) {
        xSemaphoreGive(rb->mutex);
    }
    
    return read_len;
}

/**
 * @brief 重置环形缓冲区
 * @param rb 环形缓冲区指针
 */
void ringbuf_reset(struct ringbuf *rb)
{
    if (!rb) return;
    
    // 获取互斥锁
    if (rb->mutex) {
        xSemaphoreTake(rb->mutex, portMAX_DELAY);
    }
    
    rb->head = 0;
    rb->tail = 0;
    
    // 释放互斥锁
    if (rb->mutex) {
        xSemaphoreGive(rb->mutex);
    }
}

/**
 * @brief 释放环形缓冲区资源
 * @param rb 环形缓冲区指针
 */
void ringbuf_free(struct ringbuf *rb)
{
    if (!rb) return;
    
    // 释放互斥锁
    if (rb->mutex) {
        vSemaphoreDelete(rb->mutex);
        rb->mutex = NULL;
    }
    
    rb->buffer = NULL;
    rb->size = 0;
    rb->head = 0;
    rb->tail = 0;
    rb->overwrite = false;
}