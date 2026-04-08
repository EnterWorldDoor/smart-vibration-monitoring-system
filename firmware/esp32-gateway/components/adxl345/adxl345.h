/**
 * @file adxl345.h
 * @author EnterWorldDoor 
 * @brief ADXL345 三轴加速度计驱动头文件
 */

 #ifndef ADXL345_H
 #define ADXL345_H

 #include <stdint.h>
 #include "global_error.h"

 /* 量程范围 */
 enum adxl345_range {
     ADXL345_RANGE_2G = 0,
     ADXL345_RANGE_4G = 1,
     ADXL345_RANGE_8G = 2,
     ADXL345_RANGE_16G = 3,
 }; 
 /* 数据输出速率(Hz) */
 enum adxl345_data_rate {
    ADXL345_RATE_3200 = 0b1111,     /* 3200 Hz */
    ADXL345_RATE_1600 = 0b1110,     /* 1600 Hz */
    ADXL345_RATE_800  = 0b1101,     /* 800 Hz */
    ADXL345_RATE_400  = 0b1100,     /* 400 Hz */
    ADXL345_RATE_200  = 0b1011,     /* 200 Hz */
    ADXL345_RATE_100  = 0b1010,     /* 100 Hz */
    ADXL345_RATE_50   = 0b1001,     /* 50 Hz */    
    ADXL345_RATE_25   = 0b1000,     /* 25 Hz */
 };

 /* 设备句柄(不透明结构) */
 struct adxl345_dev;

 /**
  * adxl345_init - 初始化 ADXL345 设备
  * @i2c_port: I2C 端口号(0 或 1)
  * @i2c_addr: 设备 I2C 地址 (通常 0x53 或 0x1D)
  * @range: 加速度量程范围
  * @rate: 数据输出速率
  * 
  * Return: 设备句柄 on success, NULL on error
  */
 struct adxl345_dev *adxl345_init(int i2c_port, uint8_t i2c_addr,
                                    enum adxl345_range range, enum adxl345_data_rate rate);

 /**
  * adxl345_deinit - 反初始化 ADXL345 设备，释放资源
  * @dev: 设备句柄
  */                                   
 void adxl345_deinit(struct adxl345_dev *dev);
 
 /**
  * adxl345_read_raw - 读取原始加速度数据
  * @dev: 设备句柄
  * @x: 存储 X 轴原始数据的指针 (16 位有符号)
  * @y: 存储 Y 轴原始数据的指针
  * @z: 存储 Z 轴原始数据的指针
  * 
  * Return: 0 on success, negative error code on failure
  */
 int adxl345_read_raw(struct adxl345_dev *dev, int16_t *x, int16_t *y, int16_t *z);

 /**
  * adxl345_read_g - 读取加速度数据并转换为 g 单位
  * @dev: 设备句柄
  * @x_g: 存储 X 轴加速度 (g) 的指针
  * @y_g: 存储 Y 轴加速度 (g) 的指针
  * @z_g: 存储 Z 轴加速度 (g) 的指
  */
 int adxl345_read_g(struct adxl345_dev *dev, float *x_g, float *y_g, float *z_g);

 /**
  * adxl345_self_test - 执行自检，验证设备功能
  * @dev: 设备句柄
  * 
  * Return: 0 on success, negative error code on failure
  */
 int adxl345_self_test(struct adxl345_dev *dev);

 /**
  * adxl345_set_range - 动态改变量程
  * @dev: 设备句柄
  * @range: 新的量程范围
  * 
  * Return: 0 on success, negative error code on failure
  */
 int adxl345_set_range(struct adxl345_dev *dev, enum adxl345_range range);

 #endif /* ADXL345_H */