/**
 * @file ringbuf.h
 * @author EnterWorldDoor 
 * @brief 通用环形缓冲区实现 (基于 FreeRTOS 实现，线程安全)
 */

 #ifndef COMMON_RINGBUF_H
 #define COMMON_RINGBUF_H

 #include <stdint.h>
 #include <stdbool.h>
 #include "freertos/FreeRTOS.h"
 #include "freertos/semphr.h"

 struct ringbuf {
     uint8_t *buffer;        // 环形缓冲区内存
     size_t size;           // 缓冲区大小
     size_t head;           // 写指针
     size_t tail;           // 读指针
     SemaphoreHandle_t mutex; // 互斥锁，保护缓冲区访问  
     bool overwrite;         // 是否允许覆盖旧数据
 };

 /**
  * ringbuf_init - 初始化环形缓冲区
  * @rb: 指向 ringbuf 结构体的指针
  * @buffer: 外部提供的数组
  * @size: 缓冲区大小 (字节)
  * @overwrite: 是否允许覆盖旧数据
    *
    * Return: 0 on success, negative error code on failure
  */
 int ringbuf_init(struct ringbuf *rb, uint8_t *buffer, size_t size, bool overwrite);

 /**
  * ringbuf_push - 向环形缓冲区推送数据
  * @rb: 指向 ringbuf 结构体的指针
  * @data: 要推送的数据指针
  * @len: 要推送的数据长度 (字节)
  *
  * Return: 实际推送的数据长度 (字节),如果缓冲区已满且不允许覆盖，则返回 0
  */
 size_t ringbuf_push(struct ringbuf *rb, const uint8_t *data, size_t len);

 /**
  * ringbuf_pop - 从环形缓冲区弹出数据
  * @rb: 指向 ringbuf 结构体的指针
  * @data: 存储弹出数据的指针
  * @len: 要弹出的数据长度 (字节)
  *
  * Return: 实际读取的数据长度 (字节),如果缓冲区为空，则返回 0
  */
 size_t ringbuf_pop(struct ringbuf *rb, uint8_t *data, size_t len);

 /**
  * ringbuf_peek - 查看环形缓冲区中的数据但不弹出
  * @rb: 指向 ringbuf 结构体的指针
  * @data: 存储查看数据的指针
  * @len: 要查看的数据长度 (字节)
  *
  * Return: 实际查看的数据长度 (字节),如果缓冲区为空，则返回 0
  */
 size_t ringbuf_peek(struct ringbuf *rb, uint8_t *data, size_t len);

 /**  
  * ringbuf_reset - 清空环形缓冲区
  * @rb: 指向 ringbuf 结构体的指针
  */
 void ringbuf_reset(struct ringbuf *rb);

 /**
  * ringbuf_free - 释放环形缓冲区资源(互斥量)
  * @rb: 指向 ringbuf 结构体的指针
  */
 void ringbuf_free(struct ringbuf *rb);

 #endif /* COMMON_RINGBUF_H */