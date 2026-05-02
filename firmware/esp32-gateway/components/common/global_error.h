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
#define APP_ERR_NO_DATA                 -7      /**< 无数据 */
#define APP_ERR_NO_SPACE                -8      /**< 空间不足 */

/* I2C错误 (-100 ~ -199 ) */
#define APP_ERR_I2C_INIT                -100
#define APP_ERR_I2C_WRITE               -101
#define APP_ERR_I2C_READ                -102
#define APP_ERR_I2C_DEV_NOT_FOUND       -103

/* 传感器错误 (-200 ~ -299 ) */
#define APP_ERR_SENSOR_INVALID_ID       -200
#define APP_ERR_SENSOR_DATA_OVERRUN     -201
#define APP_ERR_SENSOR_NOT_READY        -202
#define APP_ERR_SENSOR_NOT_INIT         -203    /**< 传感器未初始化 */
#define APP_ERR_TASK_CREATE_FAIL        -204    /**< 任务创建失败 */

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
#define APP_ERR_PROTO_INVALID_DATA     -1016    /**< 无效数据 */
#define APP_ERR_PROTO_NO_DATA          -1017    /**< 无数据 */
#define APP_ERR_PROTO_TIME_SYNC_PENDING -1018   /**< 时间同步待定 */

/* ADXL345 加速度计错误 (-1100 ~ -1199 ) */
#define APP_ERR_ADXL_INIT_FAIL          -1100
#define APP_ERR_ADXL_ALREADY_INIT       -1101
#define APP_ERR_ADXL_NOT_INIT           -1102
#define APP_ERR_ADXL_INVALID_PARAM      -1103
#define APP_ERR_ADXL_SPI_CONFIG         -1104
#define APP_ERR_ADXL_SPI_TRANSFAIL      -1105
#define APP_ERR_ADXL_DEV_ID_MISMATCH    -1106
#define APP_ERR_ADXL_FIFO_ERROR         -1107
#define APP_ERR_ADXL_INT_CONFIG         -1108
#define APP_ERR_ADXL_TASK_CREATE        -1109
#define APP_ERR_ADXL_QUEUE_CREATE       -1110
#define APP_ERR_ADXL_MUTEX_CREATE       -1111
#define APP_ERR_ADXL_SELFTEST_FAIL      -1112
#define APP_ERR_ADXL_BUS_TIMEOUT        -1113
#define APP_ERR_ADXL_HEALTH_DEGRADED    -1114

/* DSP 数字信号处理错误 (-1200 ~ -1299 ) */
#define APP_ERR_DSP_NOT_INIT            -1200
#define APP_ERR_DSP_INVALID_SIZE        -1201
#define APP_ERR_DSP_FFT_ERROR           -1202
#define APP_ERR_DSP_BUFFER_OVERFLOW     -1203
#define APP_ERR_DSP_WINDOW_ERROR        -1204

/* 温度补偿错误 (-1300 ~ -1399 ) */
#define APP_ERR_TEMP_NOT_INIT           -1300
#define APP_ERR_TEMP_NO_DATA            -1301
#define APP_ERR_TEMP_STALE_DATA         -1302
#define APP_ERR_TEMP_CALIBRATION_FAIL   -1303
#define APP_ERR_TEMP_SENSOR_OFFLINE     -1304

/* 数据融合错误 (-1400 ~ -1499 ) */
#define APP_ERR_FUSION_NOT_INIT         -1400
#define APP_ERR_FUSION_SYNC_ERROR       -1401
#define APP_ERR_FUSION_BUFFER_FULL      -1402

/* MQTT 通信错误 (-1500 ~ -1599 ) */
#define APP_ERR_MQTT_NOT_INIT           -1500
#define APP_ERR_MQTT_ALREADY_INIT       -1501
#define APP_ERR_MQTT_INVALID_PARAM      -1502
#define APP_ERR_MQTT_CONNECT_FAIL       -1503
#define APP_ERR_MQTT_DISCONNECT_FAIL    -1504
#define APP_ERR_MQTT_PUBLISH_FAIL       -1505
#define APP_ERR_MQTT_SUBSCRIBE_FAIL     -1506
#define APP_ERR_MQTT_UNSUBSCRIBE_FAIL   -1507
#define APP_ERR_MQTT_BUFFER_OVERFLOW    -1508
#define APP_ERR_MQTT_INVALID_MODE       -1509
#define APP_ERR_MQTT_VIRTUAL_DEV_FULL   -1510  /**< 虚拟设备槽已满 */
#define APP_ERR_MQTT_INVALID_DEV_ID     -1511  /**< 无效的设备 ID */
#define APP_ERR_MQTT_TLS_FAIL          -1512  /**< TLS 连接失败 */
#define APP_ERR_MQTT_TOPIC_TOO_LONG    -1513  /**< Topic 超长 */
#define APP_ERR_MQTT_JSON_ENCODE       -1514  /**< JSON 编码失败 */
#define APP_ERR_MQTT_CALLBACK_FULL     -1515  /**< 回调注册已满 */
#define APP_ERR_MQTT_TASK_CREATE       -1516  /**< 任务创建失败 */
#define APP_ERR_MQTT_QUEUE_CREATE      -1517  /**< 队列创建失败 */
#define APP_ERR_MQTT_MUTEX_CREATE      -1518  /**< 互斥量创建失败 */

#endif /* COMMON_GLOBAL_ERROR_H */