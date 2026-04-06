/**
 * @file error.h
 * @author EnterWorldDoor 
 * @brief 统一错误码错误码定义
 */

 #ifndef COMMON_GLOBAL_ERROR_H
 #define COMMON_GLOBAL_ERROR_H

/* 成功 */
#define ERR_OK                      0

/* 通用错误(-1 ~ -99 )*/
#define ERR_GENERAL                 -1
#define ERR_INVALID_PARAM           -2
#define ERR_NO_MEM                  -3
#define ERR_TIMEOUT                 -4
#define ERR_NOT_SUPPORTED           -5
#define ERR_BUSY                    -6

/* I2C错误 (-100 ~ -199 ) */
#define ERR_I2C_INIT                -100
#define ERR_I2C_WRITE               -101
#define ERR_I2C_READ                -102
#define ERR_I2C_DEV_NOT_FOUND       -103

/* 传感器错误 (-200 ~ -299 ) */
#define ERR_SENSOR_INVALID_ID       -200
#define ERR_SENSOR_DATA_OVERRUN     -201
#define ERR_SENSOR_NOT_READY        -202

/* DSP 错误 (-300 ~ -399 ) */
#define ERR_FFT_INVALID_SIZE        -300
#define ERR_FFT_COMPUTE_FAIL        -301

/* AI 错误 (-400 ~ -499 ) */
#define ERR_AI_MODEL_INVALID        -400
#define ERR_AI_INFERENCE_FAIL       -401

/* 配置错误 (-500 ~ -599 ) */
#define ERR_CONFIG_LOAD_FAIL        -500
#define ERR_CONFIG_SAVE_FAIL        -501

 #endif /* COMMON_ERROR_H */