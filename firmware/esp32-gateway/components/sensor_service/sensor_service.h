/**
 * @file sensor_service.h
 * @author EnterWorldDoor 
 * @brief 传感器统一服务接口 (驱动与上层解耦)
 */

 #ifndef SENSOR_SERVICE_H
 #define SENSOR_SERVICE_H
 
 #include "global_error.h"
 #include <stdint.h>
 
 /* 三轴振动样本 (浮点 g 值) */
 struct vib_sample {
    float x;
    float y;
    float z;
    int64_t timestamp_us; /* 时间戳，单位微秒 */
 };

 /**
  * sensor_service_init - 初始化传感器服务
  * @sample_rate_hz: 采样率 (Hz) , 如 100 表示 100Hz
  * @buffer_size: 环形缓冲区可存储的样本数量
  * 
  * Return: 0 on success, negative error code on failure
  */
 int sensor_service_init(int sample_rate_hz, int buffer_size);

 /**
  * sensor_service_start - 启动采集任务 (内部创建 FreeRTOS 任务)
  */
 int sensor_service_start(void);

 /**
  * sensor_service_stop - 停止采集任务并释放资源
  */
 int sensor_service_stop(void);

 /**
  * sensor_service_fetch - 从环形缓冲区获取最新的振动样本 (非阻塞)
  * @out: 输出参数，存储获取到的样本数据
  * 
  * Return: 0 on success, negative error code if no new sample is available
  */
 int sensor_service_fetch(struct vib_sample *out);
 
 /**
  * sensor_service_fetch_block - 从环形缓冲区获取多个振动样本 (批量)
  * @out: 输出参数，存储获取到的样本数据
  * @max_count: 最大获取样本数量
  * 
  * Return: 实际获取的样本数量，0 表示无新样本，负数表示错误
  */
 int sensor_service_fetch_block(struct vib_sample *out, int max_count);

 /**
  * sensor_service_register_callback - 注册数据就绪回调函数 (可选)
  * @cb: 回调函数 (在任务采集中被调用, 不可阻塞)
  */
 typedef void (*sensor_data_ready_cb_t)(const struct vib_sample *sample);
 int sensor_service_register_callback(sensor_data_ready_cb_t cb);

 #endif /* SENSOR_SERVICE_H */