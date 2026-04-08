/**
 * @file sensor_service.c
 * @author EnterWorldDoor 
 * @brief 传感器服务实现: 周期性采集 ADXL345, 存入环形缓冲区, 提供接口供上层获取数据
 */

 #include "sensor_service.h"
 #include "adxl345.h"
 #include "ringbuf.h"
 #include "global_error.h"
 #include "log_system.h"
 #include "esp_log.h"
 #include "esp_timer.h"
 #include "time_sync.h"     /* 获取全局时间戳 */
 #include "freertos/FreeRTOS.h"
 #include "freertos/task.h"
 #include "freertos/semphr.h"
 #include <string.h>

 #define SENSOR_TASK_STACK_SIZE         4096 
 #define SENSOR_TASK_PRIORITY           5

 /* 静态变量 */
 static struct adxl345_dev *g_adxl = NULL;          /* ADXL345 设备实例 */
 static struct ringbuf g_ringbuf;                   /* 环形缓冲区实例 */
 static uint8_t *g_ringbuf_storage = NULL;          /* 环形缓冲区内存 */
 static TaskHandle_t g_task_handle = NULL;         /* 采集任务句柄 */
 static bool g_running = false;                     /* 采集任务运行状态 */
 static int g_sample_interval_ms = 10;              /* 采样间隔 (ms) */
 static sensor_data_ready_cb_t g_callback = NULL;   /* 数据就绪回调函数 */

 /* 私有函数: 采集任务主体 */
 static void sensor_task(void *arg)
 {
    int64_t next_wake_us = 0;
    struct vib_sample sample;

   while (g_running) {
      /* 计算下一次唤醒时间 */
      next_wake_us = esp_timer_get_time() + g_sample_interval_ms * 1000; /* 计算下一次唤醒时间 */

      /* 读取 ADXL345 数据 */
      int ret = adxl345_read_g(g_adxl, &sample.x, &sample.y, &sample.z);
      if (ret == APP_ERR_OK) {
         sample.timestamp_us = time_sync_get_timestamp_us(); /* 获取全局时间戳 */
         /* 将样本写入环形缓冲区 */
         ringbuf_push(&g_ringbuf, (uint8_t *)&sample, sizeof(sample));

         /* 调用回调函数通知数据就绪,通知其他模块 */
         if (g_callback) {
            g_callback(&sample);
         } 
         } else {
            LOG_ERROR("SENSOR", "Failed to read ADXL345 data: %d", ret);
         }

         /* 精确延时 */
         int64_t now_us = esp_timer_get_time();
         if (next_wake_us > now_us) {
            int64_t delay_us = next_wake_us - now_us;
            vTaskDelay(pdMS_TO_TICKS(delay_us / 1000)); /* 转换为 FreeRTOS tick */
      }
   }
   vTaskDelete(NULL); /* 任务结束 */
 }

 int sensor_service_init(int sample_rate_hz, int buffer_size)
 {
     if (sample_rate_hz <= 0 || buffer_size <= 0) {
         return APP_ERR_INVALID_PARAM;
     }
     g_sample_interval_ms = 1000 / sample_rate_hz;

     /* 初始化 ADXL345 驱动 (I2C 端口需提前初始化) */
     /* 注意: I2C 初始化应在主函数中完成, 这里假设 I2C0 已经初始化配置 */
     g_adxl = adxl345_init(0, 0x53, ADXL345_RANGE_16G, ADXL345_RATE_400);
     if (!g_adxl) {
         LOG_ERROR("SENSOR", "Failed to initialize ADXL345");
         return APP_ERR_I2C_DEV_NOT_FOUND;
     }

     /* 自检 */
    adxl345_self_test(g_adxl);

     /* 分配环形缓冲区存储 */
     size_t ringbuf_size = sizeof(struct vib_sample) * buffer_size;
     g_ringbuf_storage = malloc(ringbuf_size);
     if (!g_ringbuf_storage) {
         LOG_ERROR("SENSOR", "Failed to allocate memory for ring buffer");
         adxl345_deinit(g_adxl);
         return APP_ERR_NO_MEM;
     }
       /* 初始化环形缓冲区 */
       ringbuf_init(&g_ringbuf, g_ringbuf_storage, ringbuf_size, true); /* 覆盖旧数据 */
       LOG_INFO("SENSOR", "Sensor service initialized with sample rate: %d Hz, buffer size: %d", sample_rate_hz, buffer_size);
       return APP_ERR_OK;
 }

 int sensor_service_start(void)
 {
   if (g_running) return APP_ERR_BUSY; /* 已经在运行 */
   g_running = true;
   BaseType_t ret = xTaskCreate(sensor_task, "sensor_task", 
                                    SENSOR_TASK_STACK_SIZE, NULL, SENSOR_TASK_PRIORITY, &g_task_handle);
   if (ret != pdPASS) {
      LOG_ERROR("SENSOR", "Failed to create sensor task");
      g_running = false;
      return APP_ERR_GENERAL;
   }
   LOG_INFO("SENSOR", "Sensor service started");
   return APP_ERR_OK;                                
 }

 int sensor_service_stop(void)
 {
   if (!g_running) return APP_ERR_OK; /* 已经停止 */
   g_running = false;
   if (g_task_handle) {
      vTaskDelete(g_task_handle);
      g_task_handle = NULL;
   }
   /* 清空缓冲区 */
   g_ringbuf.head = 0;
   g_ringbuf.tail = 0;
   LOG_INFO("SENSOR", "Sensor service stopped");
   return APP_ERR_OK;
 }

 int sensor_service_fetch(struct vib_sample *out)
 {
   if (!out) return APP_ERR_INVALID_PARAM;
   size_t len = ringbuf_pop(&g_ringbuf, (uint8_t *)out, sizeof(*out));
   return (len == sizeof(*out) ? APP_ERR_OK : APP_ERR_NO_MEM); /* 没有数据可读 */
 }

 int sensor_service_fetch_block(struct vib_sample *out, int max_count)
 {
   if (!out || max_count <= 0) return APP_ERR_INVALID_PARAM;
   size_t total =0;
   while (total < max_count) {
      size_t len = ringbuf_pop(&g_ringbuf, (uint8_t *)&out[total], sizeof(struct vib_sample));
      if (len != sizeof(struct vib_sample)) {
         break; /* 没有更多数据可读 */
      }
      total++;
   }
   return (int)total; /* 返回实际读取的样本数量 */
 }

 int sensor_service_register_callback(sensor_data_ready_cb_t cb)
 {
   g_callback = cb;
   return APP_ERR_OK;
 }