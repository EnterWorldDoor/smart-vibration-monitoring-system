/**
 * @file error.h
 * @author EnterWorldDoor 
 * @brief 统一错误码错误码定义
 */

 #ifndef COMMON_GLOBAL_ERROR_H
 #define COMMON_GLOBAL_ERROR_H

/* 成功 */
#define APP_ERR_OK                      0

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
#define APP_ERR_CONFIG_NVS_OPEN         -502
#define APP_ERR_CONFIG_NVS_READ         -503
#define APP_ERR_CONFIG_NVS_WRITE        -504
#define APP_ERR_CONFIG_NVS_COMMIT       -505
#define APP_ERR_CONFIG_VALIDATION       -506
#define APP_ERR_CONFIG_ENCRYPTION       -507
#define APP_ERR_CONFIG_VERSION_MISMATCH -508
#define APP_ERR_CONFIG_BACKUP_FAILED    -509

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

/* 系统监控错误 (-700 ~ -799 ) */
#define APP_ERR_MONITOR_INIT_FAIL       -700
#define APP_ERR_MONITOR_ALREADY_INIT    -701
#define APP_ERR_MONITOR_NOT_INIT        -702
#define APP_ERR_MONITOR_TASK_CREATE     -703
#define APP_ERR_MONITOR_INVALID_PARAM   -704
#define APP_ERR_MONITOR_CALLBACK_FULL   -705
#define APP_ERR_MONITOR_THRESHOLD_INVALID -706

/* OTA 升级错误 (-800 ~ -899 ) */
#define APP_ERR_OTA_INIT_FAIL           -800
#define APP_ERR_OTA_ALREADY_IN_PROGRESS -801
#define APP_ERR_OTA_NOT_INIT            -802
#define APP_ERR_OTA_INVALID_URL         -803
#define APP_ERR_OTA_HTTP_INIT           -804
#define APP_ERR_OTA_HTTP_CONNECT       -805
#define APP_ERR_OTA_HTTP_RESPONSE      -806
#define APP_ERR_OTA_DOWNLOAD_FAILED    -807
#define APP_ERR_OTA_WRITE_FAILED       -808
#define APP_ERR_OTA_VERIFY_FAILED      -809
#define APP_ERR_OTA_PARTITION_INVALID  -810
#define APP_ERR_OTA_BEGIN_FAILED       -811
#define APP_ERR_OTA_END_FAILED         -812
#define APP_ERR_OTA_SET_BOOT_FAILED    -813
#define APP_ERR_OTA_ABORTED            -814
#define APP_ERR_OTA_TIMEOUT            -815
#define APP_ERR_OTA_NO_UPDATE_PART     -816
#define APP_ERR_OTA_VERSION_CHECK_FAIL -817
#define APP_ERR_OTA_ROLLBACK_FAILED    -818
#define APP_ERR_OTA_CALLBACK_FULL      -819
#define APP_ERR_OTA_INVALID_STATE      -820

/* 时间同步错误 (-900 ~ -999 ) */
#define APP_ERR_TIME_INIT_FAIL         -900
#define APP_ERR_TIME_ALREADY_INIT       -901
#define APP_ERR_TIME_NOT_INIT           -902
#define APP_ERR_TIME_SNTPI_INIT_FAIL   -903
#define APP_ERR_TIME_EVENT_GROUP_FAIL  -904
#define APP_ERR_TIME_SNTPI_SYNC_TIMEOUT -905
#define APP_ERR_TIME_INVALID_PARAM     -906
#define APP_ERR_TIME_CALLBACK_FULL     -907
#define APP_ERR_TIME_TASK_CREATE_FAIL  -908

/* 协议通信错误 (-1000 ~ -1099 ) */
#define APP_ERR_PROTO_INIT_FAIL        -1000
#define APP_ERR_PROTO_ALREADY_INIT     -1001
#define APP_ERR_PROTO_NOT_INIT         -1002
#define APP_ERR_PROTO_INVALID_PARAM    -1003
#define APP_ERR_PROTO_UART_CONFIG      -1004
#define APP_ERR_PROTO_UART_INSTALL     -1005
#define APP_ERR_PROTO_TASK_CREATE      -1006
#define APP_ERR_PROTO_QUEUE_CREATE     -1007
#define APP_ERR_PROTO_MUTEX_CREATE     -1008
#define APP_ERR_PROTO_SEND_TIMEOUT     -1009
#define APP_ERR_PROTO_ACK_TIMEOUT       -1010
#define APP_ERR_PROTO_CRC_MISMATCH     -1011
#define APP_ERR_PROTO_FRAME_OVERFLOW   -1012
#define APP_ERR_PROTO_CALLBACK_FULL    -1013
#define APP_ERR_PROTO_INVALID_STATE    -1014
#define APP_ERR_PROTO_MAX_RETRY        -1015

 #endif /* COMMON_GLOBAL_ERROR_H */