/**
 * @file adxl345.c
 * @author EnterWorldDoor 
 * @brief ADXL345 加速度传感器驱动(I2C)
 */

 #include "adxl345.h"
 #include "adxl345_reg.h"
 #include "global_error.h"
 #include "log_system.h"
 #include <string.h>
 #include <math.h>
 #include "driver/i2c.h"

 #define ADXL345_DEFULT_ADDR    0x53 /* ADXL345 默认I2C地址 */
 #define I2C_TIMEOUT_MS     100

 /* ADXL345 设备句柄内部结构 */
 struct adxl345_dev {
    int i2c_port;
    uint8_t i2c_addr;
    enum adxl345_range range;
    float scale_factor;     /* 转换因子: 原始值 -> g */
 };

 /* 私有函数: I2C 写单字节 */
 static int i2c_write_byte(struct adxl345_dev *dev, uint8_t reg, uint8_t data)
 {
    uint8_t write_buf[2] = {reg, data};
    esp_err_t ret = i2c_master_write_to_device(dev->i2c_port, dev->i2c_addr,
                                                write_buf, sizeof(write_buf), pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    return (ret == ESP_OK) ? APP_ERR_OK : APP_ERR_I2C_WRITE;                                            
 }

 /* 私有函数: I2C 读单字节 */
 static int i2c_read_byte(struct adxl345_dev *dev, uint8_t reg, uint8_t *data)
{
   esp_err_t ret = i2c_master_write_read_device(dev->i2c_port, dev->i2c_addr, &reg, 1, data, 1, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
   return (ret == ESP_OK) ? APP_ERR_OK : APP_ERR_I2C_READ;
}

/* 私用函数: I2C 多字节读 */
static int i2c_read_bytes(struct adxl345_dev *dev, uint8_t reg, uint8_t *buf, size_t len)
{
   esp_err_t ret = i2c_master_write_read_device(dev->i2c_port, dev->i2c_addr, &reg, 1, buf, len, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
   return (ret == ESP_OK) ? APP_ERR_OK : APP_ERR_I2C_READ; 
}

/* 私有函数: 更新 scale factor */
static void update_scale_factor(struct adxl345_dev *dev)
{
   switch (dev->range) {
         case ADXL345_RANGE_2G:
               dev->scale_factor = 4.0f / 1024.0f; /* 2g 对应 1024 */
               break;
         case ADXL345_RANGE_4G:
               dev->scale_factor = 8.0f / 1024.0f; /* 4g 对应 1024 */
               break;
         case ADXL345_RANGE_8G:
               dev->scale_factor = 16.0f / 1024.0f; /* 8g 对应 1024 */
               break;   
         case ADXL345_RANGE_16G:
               dev->scale_factor = 32.0f / 1024.0f; /* 16g 对应 1024 */
               break;
         default:
               dev->scale_factor = 4.0f / 1024.0f;
               break;
   }                  
}

struct adxl345_dev *adxl345_init(int i2c_port, uint8_t i2c_addr,
                                 enum adxl345_range range,
                                 enum adxl345_data_rate rate)
{
   if (i2c_addr == 0) i2c_addr = ADXL345_DEFULT_ADDR;

   struct adxl345_dev *dev = malloc(sizeof(struct adxl345_dev));
   if (!dev) {
      LOG_ERROR("ADXL345", "Failed to allocate memory for ADXL345 device");
      return NULL;
   }
   memset(dev, 0, sizeof(*dev));
   dev->i2c_port = i2c_port;
   dev->i2c_addr = i2c_addr;
   dev->range = range;
   update_scale_factor(dev);

   /* 检查设备ID */
   uint8_t dev_id = 0;
   int ret = i2c_read_byte(dev, ADXL345_DEVID, &dev_id);   
   if (ret != APP_ERR_OK || dev_id != 0xE5) {
      LOG_ERROR("ADXL345", "Failed to read device ID or device ID mismatch (0x%02X)", dev_id);
      free(dev);
      return NULL;
   }
   LOG_INFO("ADXL345", "ADXL345 device ID: 0x%02X", dev_id);

   /* 进入待机模式以配置寄存器 */
   uint8_t power_ctl;
   i2c_read_byte(dev, ADXL345_POWER_CTL, &power_ctl);
   power_ctl &= ~0x08; /* 进入待机模式,清除 measure 位 */
   i2c_write_byte(dev, ADXL345_POWER_CTL, power_ctl);

   /* 配置数据格式(全分辨率, 右对齐, 量程) */
   uint8_t data_format = 0x08; /* 全分辨率 */
   data_format |= (range & 0x03) << 0; /* 设置量程 */
   i2c_write_byte(dev, ADXL345_DATA_FORMAT, data_format);

   /* 配置数据速率 */
   i2c_write_byte(dev, ADXL345_BW_RATE, rate & 0x0F);

   /* 进入测量模式 */
   power_ctl |= 0x08; /* 设置 measure 位 */
   i2c_write_byte(dev, ADXL345_POWER_CTL, power_ctl);

   LOG_INFO("ADXL345", "ADXL345 initialized successfully (I2C addr: 0x%02X, range: %dG, rate: %dHz)", dev->i2c_addr, range, rate);
   return dev;
}

void adxl345_deinit(struct adxl345_dev *dev)
{
   if (dev) {
      /* 进入待机模式 */
      uint8_t power_ctl;
      i2c_read_byte(dev, ADXL345_POWER_CTL, &power_ctl);
      power_ctl &= ~0x08; /* 清除 measure 位 */
      i2c_write_byte(dev, ADXL345_POWER_CTL, power_ctl);
      free(dev);
      LOG_INFO("ADXL345", "ADXL345 device deinitialized");
   }
}

int adxl345_read_raw(struct adxl345_dev *dev, int16_t *x, int16_t *y, int16_t *z)
{
   if (!dev || !x || !y || !z) return APP_ERR_INVALID_PARAM;

   uint8_t buf[6];
   int ret = i2c_read_bytes(dev, ADXL345_DATAX0, buf, sizeof(buf));
   if (ret != APP_ERR_OK) {
      LOG_ERROR("ADXL345", "Failed to read any raw data");
      return ret;
   }

   *x = (int16_t)((buf[1] << 8) | buf[0]);
   *y = (int16_t)((buf[3] << 8) | buf[2]);
   *z = (int16_t)((buf[5] << 8) | buf[4]);

   return APP_ERR_OK;
}

int adxl345_read_g(struct adxl345_dev *dev, float *x_g, float *y_g, float *z_g)
{
   if (!dev || !x_g || !y_g || !z_g) return APP_ERR_INVALID_PARAM;

   int16_t x_raw, y_raw, z_raw;
   int ret = adxl345_read_raw(dev, &x_raw, &y_raw, &z_raw);
   if (ret != APP_ERR_OK) {
      LOG_ERROR("ADXL345", "Failed to read raw data needed for g conversion");
      return ret;
   }

   *x_g = (float)x_raw * dev->scale_factor;
   *y_g = (float)y_raw * dev->scale_factor;
   *z_g = (float)z_raw * dev->scale_factor;

   return APP_ERR_OK;
}

int adxl345_self_test(struct adxl345_dev *dev)
{
   /* 简易自检: 读取静止时 Z 轴应接近 1g */
   float x, y, z;
   int ret = adxl345_read_g(dev, &x, &y, &z);
   if (ret != APP_ERR_OK) {
      LOG_ERROR("ADXL345", "Self-test failed: unable to read data");
      return ret;
   }

   if (fabsf(z - 1.0f) > 0.5f) {
      LOG_ERROR("ADXL345", "Self-test failed: Z axis reading out of expected range (%.2fg)", z);
      return APP_ERR_SENSOR_NOT_READY;
   }
   LOG_INFO("ADXL345", "Self-test passed: Z axis reading = %.2fg", z);
   return APP_ERR_OK;
}

int adxl345_set_range(struct adxl345_dev *dev, enum adxl345_range range)
{
   if (!dev) return APP_ERR_INVALID_PARAM;
   dev->range = range;
   update_scale_factor(dev);

   /* 更新配置 DATA_FORMAT 寄存器 */
   uint8_t data_format;
   int ret = i2c_read_byte(dev, ADXL345_DATA_FORMAT, &data_format);
   if (ret != APP_ERR_OK) {
      LOG_ERROR("ADXL345", "Failed to read data format register for range update");
      return ret;
   }
   data_format &= ~0x03; /* 清除量程位 */
   data_format |= (range & 0x03); /* 设置新的量程 */
   return i2c_write_byte(dev, ADXL345_DATA_FORMAT, data_format);
}