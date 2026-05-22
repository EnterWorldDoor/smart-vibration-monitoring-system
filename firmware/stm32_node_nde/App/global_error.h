/*
 * App/global_error.h — 精简全局错误码 (F103裸机版)
 *
 * 从F407 Modules/global_error/global_error.h 继承错误码分区,
 * 不含error_manager运行时 (太重, F103 64KB Flash放不下).
 *
 * 使用: return ERR_SENSOR_NOT_FOUND; 替代 return -1;
 */

#ifndef __GLOBAL_ERROR_H
#define __GLOBAL_ERROR_H

/* 成功 */
#define ERR_OK                          0

/* 通用错误 (-1 ~ -99) */
#define ERR_GENERAL                     -1
#define ERR_INVALID_PARAM               -2
#define ERR_NO_MEM                      -3
#define ERR_TIMEOUT                     -4
#define ERR_NOT_SUPPORTED               -5
#define ERR_BUSY                        -6
#define ERR_ALREADY_INIT                -7
#define ERR_NOT_INIT                    -8
#define ERR_NULL_POINTER                -9

/* CRC 校验错误 (-200 ~ -299) */
#define ERR_CRC_INIT_FAIL               -200
#define ERR_CRC_MISMATCH                -201
#define ERR_CRC_INVALID_PARAM           -202

/* 传感器错误 (-400 ~ -499) */
#define ERR_SENSOR_INIT_FAIL            -400
#define ERR_SENSOR_NOT_FOUND            -401
#define ERR_SENSOR_DATA_OVERRUN         -402
#define ERR_SENSOR_COMM_FAIL            -403

/* 通信错误 (-600 ~ -699) */
#define ERR_COMM_INIT_FAIL              -600
#define ERR_COMM_TX_FAIL                -601
#define ERR_COMM_RX_FAIL                -602
#define ERR_COMM_TIMEOUT                -603

/* DSP 错误 (-1000 ~ -1099) */
#define ERR_DSP_INIT_FAIL               -1000
#define ERR_DSP_PROCESS_FAIL            -1001
#define ERR_DSP_OVERFLOW                -1002

#endif /* __GLOBAL_ERROR_H */
