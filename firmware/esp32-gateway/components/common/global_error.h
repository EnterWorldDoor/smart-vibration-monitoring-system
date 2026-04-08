/**
 * @file error.h
 * @author EnterWorldDoor 
 * @brief 统一错误码错误码定义
 */

 #ifndef COMMON_GLOBAL_ERROR_H
 #define COMMON_GLOBAL_ERROR_H

/* 成功 */
#define APP_ERR_OK                  0

/* 通用错误(-1 ~ -99 )*/
#define APP_ERR_GENERAL                 -1
#define APP_ERR_INVALID_PARAM           -2
#define APP_ERR_NO_MEM                  -3
#define APP_ERR_TIMEOUT                 -4
#define APP_ERR_NOT_SUPPORTED           -5
#define APP_ERR_BUSY                    -6

/* I2C错误 (-100 ~ -199 ) */
#define APP_ERR_I2C_INIT                -100
#define APP_ERR_I2C_WRITE               -101
#define APP_ERR_I2C_READ                -102
#define APP_ERR_I2C_DEV_NOT_FOUND       -103

/* 传感器错误 (-200 ~ -299 ) */
#define APP_ERR_SENSOR_INVALID_ID       -200
#define APP_ERR_SENSOR_DATA_OVERRUN     -201
#define APP_ERR_SENSOR_NOT_READY        -202

/* DSP 错误 (-300 ~ -399 ) */
#define APP_ERR_FFT_INVALID_SIZE        -300
#define APP_ERR_FFT_COMPUTE_FAIL        -301

/* AI 错误 (-400 ~ -499 ) */
#define APP_ERR_AI_MODEL_INVALID        -400
#define APP_ERR_AI_INFERENCE_FAIL       -401

/* 配置错误 (-500 ~ -599 ) */
#define APP_ERR_CONFIG_LOAD_FAIL        -500
#define APP_ERR_CONFIG_SAVE_FAIL        -501

/* 日志系统错误 (-600 ~ -699 ) */
#define APP_ERR_LOG_INIT_FAIL           -600
#define APP_ERR_LOG_MEMORY_ALLOC        -601
#define APP_ERR_LOG_MUTEX_CREATE        -602
#define APP_ERR_LOG_RINGBUF_INIT        -603
#define APP_ERR_LOG_INVALID_PARAM       -604
#define APP_ERR_LOG_FILE_OPEN           -605
#define APP_ERR_LOG_FILE_WRITE          -606
#define APP_ERR_LOG_FILE_ROTATE         -607
#define APP_ERR_LOG_QUEUE_CREATE        -608
#define APP_ERR_LOG_QUEUE_SEND          -609
#define APP_ERR_LOG_MQTT_CONNECT        -610
#define APP_ERR_LOG_MQTT_PUBLISH        -611
#define APP_ERR_LOG_CONFIG_INVALID      -612
#define APP_ERR_LOG_BUFFER_OVERFLOW     -613

 #endif /* COMMON_GLOBAL_ERROR_H */