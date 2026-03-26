#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "esp_log.h"

#define I2C_MASTER_SCL_IO           GPIO_NUM_5      // SCL 引脚
#define I2C_MASTER_SDA_IO           GPIO_NUM_4      // SDA 引脚
#define I2C_MASTER_NUM              I2C_NUM_0       // I2C 端口号
#define I2C_MASTER_FREQ_HZ          100000          // 100kHz
#define I2C_MASTER_TX_BUF_DISABLE   0               // 不使用发送缓冲区
#define I2C_MASTER_RX_BUF_DISABLE   0               // 不使用接收缓冲区

#define ADXL345_I2C_ADDR            0x53            // SDO 接地时的地址
#define ADXL345_DEVID_REG           0x00            // 设备 ID 寄存器
#define ADXL345_DEVID_EXPECTED      0xE5            // 预期 ID

static const char *TAG = "ADXL345";

// 初始化 I2C 总线
static void i2c_master_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_MASTER_NUM, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_MASTER_NUM, conf.mode,
                                       I2C_MASTER_RX_BUF_DISABLE,
                                       I2C_MASTER_TX_BUF_DISABLE,0));
}

// 读取一个字节寄存器
static uint8_t adxl345_read_byte(uint8_t reg)
{
    uint8_t data;
    i2c_cmd_handle_t cmd = i2c_cmd_link_creare();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd,(ADXL345_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2_master_start(cmd);
    i2c_master_write_byte(cmd, (ADXL345_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, &data, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);
    if(ret != ESP_OK){
        ESP_LOGE(TAG, "I2C read failed: %s", esp_err_to_name(ret));
        return 0;
    }
    return data;
}


void app_main(void)
{
    ESP_LOGI(TAG, "Initializing I2C...");
    i2c_master_init();

    vTaskDelay(pdMS_TO_TICKS(100)); // 等待传感器上电稳定

    ESP_LOGI(TAG, "Reading device ID...");
    uint8_t dev_id = adxl345_read_byte(ADXL345_DEVID_REG);
    ESP_LOGI(TAG, "Device ID: 0x%02X", dev_id);

    if(dev_id == ADXL345_DEVID_EXPECTED) {
        ESP_LOGI(TAG, "ADXL345 detected successfully!");
    } else {
        ESP_LOGI(TAG, "ADXL345 not detected! Expected 0x%02X, got 0x%02X",
                 ADXL345_DEVID_EXPECTED, dev_id);
    }
    // 可选：读取加速度数据并打印
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/*
查看监视器输出
烧录完成后，自动运行，也可以手动打开监视器：

bash
idf.py monitor
你会看到类似输出：

text
I (xxx) ADXL345: Initializing I2C...
I (xxx) ADXL345: Reading device ID...
I (xxx) ADXL345: Device ID: 0xE5
I (xxx) ADXL345: ADXL345 detected successfully!
如果看到 Device ID: 0xE5 且成功信息，说明通信正常。

*/
