/**
 * @file data_manager.h
 * @author EnterWorldDoor
 * @brief 数据管理器: 系统数据总线, 融合原始样本、DSP、AI 结果
 */

 #ifndef DATA_MANAGER_H_
 #define DATA_MANAGER_H_

 #include <stdint.h>
 #include "sensor_service.h"
 #include "dsp.h"
 #include "global_error.h"

 /* 前向声明(AI 结果结构在 ai_service.h 中定义) */
 struct ai_result;

 /* 故障诊断结果结构(在 fault_diagnosis.h 中定义) */
 struct fault_result;
 
 /* 系统完整数据包 */
 struct system_data_packet {
    uint64_t timestamp_us;                  /* 统一时间戳 */
    struct vib_sample raw_vib;              /* 原始三轴振动数据 */
    struct fft_result fft_x;                /* x轴 FFT 结果 */
    struct fft_result fft_y;
    struct fft_result fft_z;                
    struct rms_result rms_x;                /* x 轴 RMS */
    struct rms_result rms_y;
    struct rms_result rms_z;
    //struct ai_result  ai;                   /* AI 推理结果 */
    //struct fault_result fault;              /* 故障诊断结果 */
    uint8_t packet_seq;                       /* 数据包序号(用于调试) */
 };

 /**
  * data_manager_init - 初始化数据管理器
  * @max_packet_history: 最多缓存多少个完整数据包 (用于历史回溯)
  */
 int data_manager_init(int max_packet_history);

 /**
  * data_manager_push_raw - 推送原始样本 (由 sensor_service 回调调用)
  * @sample: 原始振动样本
  * 
  * 内部会触发: 保存最新原始数据 -> 通知 DSP 任务进行 FFT/RMS 计算
  * 此函数应尽量轻量, 不阻塞采集任务
  */
 int data_manager_push_raw(const struct vib_sample *sample);

 /**
  * data_manager_get_latest - 获取最新完整数据包 (非阻塞)
  * @out: 输出数据包指针
  * 
  * Return: 0 on success, -APP_ERR_NO_MEM if no packet available
  */
 int data_manager_get_latest(struct system_data_packet *out);

 /**
  * data_manager_subscribe - 订阅数据包更新 (回调方式)
  * @cb: 当新的完整数据包准备好时调用 (在 DSP 处理任务上下文中)
  */
 typedef void (*data_manager_cb_t)(const struct system_data_packet *pkt);
 int data_manager_subscribe(data_manager_cb_t cb);

 /**
  * data_manager_trigger_processing - 强制触发一次 DSP+AI 处理 (通常内部自动)
  */
 int data_manager_trigger_procesing(void);

#endif /* DATA_MANAGER_H_ */