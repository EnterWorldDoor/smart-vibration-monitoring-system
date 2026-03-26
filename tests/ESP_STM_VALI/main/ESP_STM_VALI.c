#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define UART_NUM UART_NUM_1     //使用 UART1, 对应引脚可配置
#define TXD_PIN 17              //发送引脚(连接 STM32 的 RX)
#define RXD_PIN 18              //接收引脚(连接 STM32 的 TX)
#define BUF_SIZE 128

static const char *TAG ="UART_TEST";

void uart_task(void *pvParameters)
{
    uint8_t *data =(uint8_t *)malloc(BUF_SIZE);
    while(1)
    {
        // 读取数据，直到遇到'\n'
        int len = uart_read_bytes(UART_NUM, data, BUF_SIZE,pdMS_TO_TICKS(1000));
        if (len > 0)
        {
            // 检查是否包含 "Hello ESP32!"
            if(strstr((char *)data,"Hello ESP32!") != NULL)
            {
               // 回复"OK\r\n"
               uart_write_bytes(UART_NUM,"OK\r\n",4);
               ESP_LOGI(TAG, "Received Hello, sent OK");     
            }
        }
    }
    free(data);
    vTaskDelete(NULL);
}


void app_main(void)
{
    // 配置UART 参数
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_STOP_BITS_1,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    // 安装 UART 驱动
    uart_driver_install(UART_NUM, BUF_SIZE * 2, BUF_SIZE * 2,0,NULL, 0);
    uart_param_config(UART_NUM, &uart_config);
    uart_set_pin(UART_NUM, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    // 创建任务处理接收
    xTaskCreate(uart_task, "uart_task", 4096, NULL, 10, NULL);
}
