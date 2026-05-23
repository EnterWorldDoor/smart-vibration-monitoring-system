/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : STM32F407 Enterprise System V2.0 (Bootloader)
  *
  * V2.0 Enterprise Features:
  *   - ✅ All peripherals enabled (except watchdog)
  *   - ✅ Full ESP32 protocol stack (CRC16-MODBUS, frame format)
  *   - ✅ FreeRTOS multi-task architecture
  *   - ✅ LVGL GUI framework ready
  *   - ✅ Linux kernel coding style compliance
  *
  * Hardware Platform:
  *   MCU: STM32F407IGT6 (Cortex-M4, 168MHz)
  *   Board: ATK-DMF407 Development Kit
  *   LCD: 2.8" TFT (320x240, FSMC 16-bit, ILI9341)
  *
  * Architecture:
  *   main():
  *     ├── HAL_Init() + SystemClock_Config()
  *     ├── MX_GPIO_Init()
  *
  *     ├── Initialize ALL peripherals (USART/FSMC/SPI/I2C/TIM/ADC/DMA/CAN/RTC)
  *     ├── Initialize logging system
  *     └── osKernelStart() → FreeRTOS Scheduler
  *           │
  *           └── app_dht11_task_entry() [App/app_main.c]
  *                 ├── simulator_update()      // Random Walk algorithm
  *                 ├── send_temp_to_esp32()    // UART4 protocol
  *                 └── gui_app_update()        // LVGL display (optional)
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"
#include "adc.h"
#include "can.h"
#include "dma.h"
#include "i2c.h"
#include "iwdg.h"
#include "lwip.h"
#include "rtc.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "wwdg.h"
#include "gpio.h"
#include "fsmc.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
#include "system_log/system_log.h"
#include "cmsis_os2.h"                 /* CMSIS-RTOS2 API (osKernelStart) */
#include "../../bsp/uart_log/uart_log.h"

#include "../../bsp/lcd/lcd.h"
#include "../../lvgl.h"
#include "../../App/gui/gui_app.h"
#include "../../bsp/motor/bsp_motor.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void MX_FREERTOS_Init(void);
/* USER CODE BEGIN PFP */

/*
 * ⚠️ 【关键】FreeRTOS初始化函数外部声明
 *
 * 函数定义位置: Core/Src/freertos.c:179
 * 功能: 创建所有FreeRTOS任务 (defaultTask, app_enterprise, lvgl_gui等)
 *
 * 注意: STM32CubeMX生成代码时不会自动在main.c中添加此声明
 *       必须手动添加,否则编译器报 "implicit declaration" 错误!
 */
void MX_FREERTOS_Init(void);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /*
   * ======== 第一步: 初始化 USART1 + 日志系统 (最优先!) ========
   * 必须在任何其他初始化之前完成
   */
  MX_USART1_UART_Init();

  {
          struct log_config log_cfg;
          memset(&log_cfg, 0, sizeof(log_cfg));

          uart_log_init(&huart1);

          log_cfg.level = LOG_LEVEL_INFO;
          log_cfg.tag = "STM32-V2";
          log_cfg.output = uart_log_write;
          log_cfg.enable_timestamp = true;
          log_cfg.async_mode = false;

          log_init(&log_cfg);
  }

  /*
   * 第一步半: 第一时间输出 banner (16MHz)
   */
  pr_info_with_tag("SYS", "========================================\n");
  pr_info_with_tag("SYS", " STM32F407 Enterprise System V2.0\n");
  pr_info_with_tag("SYS", " Bootloader Mode (FreeRTOS Ready)\n");
  pr_info_with_tag("SYS", "========================================\n\n");

  /*
   * 第二步: 等待 SystemClock_Config 完成
   * 不在此时初始化任何其他外设!
   * 原因: 此时是 HSI 16MHz, 所有外设应在 168MHz PLL 之后统一初始化
   */
  pr_info_with_tag("SYS", "[DEBUG] About to call SystemClock_Config...\n");

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /*
   * 时钟切换后必须重新初始化 TIM6 时间基准
   * HAL_RCC_ClockConfig 内部会调用 HAL_InitTick, 但显式调用确保正确
   */
  /*
   * 使用SysTick作为HAL时间基准 (代替TIM6)。
   * FreeRTOS的SysTick_Handler (cmsis_os2.c)同时调用HAL_IncTick和xPortSysTickHandler。
   */
  SysTick_Config(SystemCoreClock / 1000U);

  /*
   * 清空NVIC所有挂起中断, 并全局关闭所有外设中断。
   *
   * 背景: SysTick优先级为15(最低), 如果有任何外设中断在
   * 持续挂起(如DMA circular模式), SysTick永远得不到CPU响应,
   * 导致HAL_GetTick()不递增 → 日志时间戳恒为1ms → 系统假死。
   *
   * 解决方案: 启动阶段关闭所有外设中断, 调度器启动后由
   * 各任务按需重新开启 (UART4等通讯外设在app_main中使能)。
   */
  for (int irq = 0; irq < 96; irq++) {
          NVIC_DisableIRQ((IRQn_Type)irq);
          NVIC_ClearPendingIRQ((IRQn_Type)irq);
  }

  MX_USART1_UART_Init();

  pr_info_with_tag("SYS", "[OK] SystemClock_Config + TIM6 timebase ready at 168MHz\n");

  /* USER CODE END SysInit */

  /*
   * ======== 第三步: 初始化所有外设 (168MHz) ========
   */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_ADC1_Init();
  MX_CAN1_Init();
  MX_FSMC_Init();
  MX_I2C2_Init();
  /* MX_IWDG_Init();    ---- 禁用IWDG调试 */
  MX_RTC_Init();
  MX_SPI2_Init();
  MX_TIM1_Init();
  MX_TIM3_Init();
  MX_UART4_Init();
  MX_USART1_UART_Init();
  MX_USART3_UART_Init();
  /* MX_WWDG_Init();    ---- 禁用WWDG调试 */
  MX_ADC3_Init();
  MX_UART5_Init();
  pr_info_with_tag("SYS", "[OK] All peripherals initialized\n");
  /* USER CODE BEGIN 2 */

  /*
   * ======== LVGL GUI 初始化 ========
   * 必须在所有外设初始化后、FreeRTOS调度器启动前完成
   */

  /*
   * 使能CCMRAM时钟 (0x10000000, 64KB)
   * SystemInit()不会自动使能CCMRAM, 必须显式使能。
   * LVGL显示缓冲区放置于CCMRAM以节省主SRAM,
   * 若未使能则写入无效 → LCD花屏。
   */
  __HAL_RCC_CCMDATARAMEN_CLK_ENABLE();

  pr_info_with_tag("SYS", "[DEBUG] Initializing LCD display...\n");
  {
          int ret = bsp_lcd_init();
          if (ret != 0) {
                  pr_error_with_tag("SYS", "FATAL: LCD init failed: %d\n", ret);
                  Error_Handler();
          }
  }
  pr_info_with_tag("SYS", "[OK] LCD initialized\n");

  pr_info_with_tag("SYS", "[DEBUG] Initializing LVGL graphics library...\n");
  lv_init();
  pr_info_with_tag("SYS", "[OK] LVGL core initialized\n");

  pr_info_with_tag("SYS", "[DEBUG] Initializing LVGL display driver...\n");
  {
          static lv_disp_draw_buf_t draw_buf;
          /*
           * LVGL单缓冲 (20行 = 12800字节, SRAM)。
           * 20行缓冲将全屏刷新拆分从24次降到12次,
           * 减少窗口设置开销, 提升渲染稳定性。
           */
          static lv_color_t buf_1[LCD_WIDTH * 20];

          lv_disp_draw_buf_init(&draw_buf, buf_1, NULL,
                                LCD_WIDTH * 20);

          static lv_disp_drv_t disp_drv;
          lv_disp_drv_init(&disp_drv);
          disp_drv.hor_res = LCD_WIDTH;
          disp_drv.ver_res = LCD_HEIGHT;
          disp_drv.flush_cb = lcd_flush_cb;
          disp_drv.draw_buf = &draw_buf;
          lv_disp_drv_register(&disp_drv);
  }
  pr_info_with_tag("SYS", "[OK] LVGL display driver registered (buf=20lines SRAM)\n");

  pr_info_with_tag("SYS", "[DEBUG] Initializing GUI application...\n");
  {
          int ret = gui_app_init();
          if (ret != 0) {
                  pr_error_with_tag("SYS", "FATAL: GUI app init failed: %d\n",
                                   ret);
                  Error_Handler();
          }
  }
  pr_info_with_tag("SYS", "[OK] GUI application initialized\n");

        /*
         * 预填充LCD为粉白色 (0xFF9E = #FEF0F5 RGB565)
         * 即使LVGL首帧渲染异常, 背景也不会显示黑灰色。
         * LVGL后续帧会正常覆盖此预填充区域。
         */
        bsp_lcd_fill_screen(0xFF9E);
        lv_obj_invalidate(lv_scr_act());

  /*
   * 初始化触摸输入设备 (在GUI对象创建之后)
   */
  pr_info_with_tag("SYS", "[DEBUG] Initializing touch input device...\n");
  {
          int ret = gui_app_touch_input_init();
          if (ret != 0) {
                  pr_warn_with_tag("SYS",
                                  "Touch input init failed: %d, "
                                  "GUI will operate without touch\n",
                                  ret);
          }
  }
  pr_info_with_tag("SYS", "[OK] Touch input device initialized\n");

  pr_info_with_tag("SYS", "[OK] LVGL GUI subsystem ready\n");

  /*
   * ======== 电机 BSP 初始化 ========
   * 必须在 TIM1/TIM3/ADC/DMA 外设初始化后调用
   * 初始化后: PWM=0%, CTRL_SD=0 (H桥物理关断, 安全态)
   * 必须显式调用 bsp_motor_start() 才能驱动电机
   */
  pr_info_with_tag("SYS", "[DEBUG] Initializing motor BSP...\n");
  {
          int ret = bsp_motor_init();
          if (ret != 0) {
                  pr_error_with_tag("SYS", "FATAL: Motor BSP init failed: %d\n", ret);
                  Error_Handler();
          }
  }
  pr_info_with_tag("SYS", "[OK] Motor BSP initialized (PWM safe, H-bridge off)\n");

  /*
   * ======== 启动前准备 ========
   * UART忙等待确保所有日志已发送完毕, 避免FreeRTOS调度器
   * 启动后printf/log竞争UART导致输出截断。
   */
  {
          volatile uint32_t i;
          for (i = 0; i < 3600000; i++)  /* ~100ms @168MHz */
          {
                  __NOP();
          }
  }

  /*
   * 诊断: 显式使能全局中断 (以防被之前某段代码意外关闭)
   * 并打印 PRIMASK/BASEPRI 寄存器值。
   */
  __enable_irq();

  pr_info_with_tag("SYS", "[HEALTH] Pre-scheduler check:\n");
  pr_info_with_tag("SYS", "  System tick: %lu ms\n", (unsigned long)HAL_GetTick());
  pr_info_with_tag("SYS", "  MSP (main stack): 0x%08X\n", (unsigned int)__get_MSP());
  pr_info_with_tag("SYS", "  PRIMASK: %lu (0=IRQ enabled)\n",
                   (unsigned long)__get_PRIMASK());
  pr_info_with_tag("SYS", "  SysTick CTRL: 0x%08lX\n",
                   (unsigned long)SysTick->CTRL);
  pr_info_with_tag("SYS", "  SysTick LOAD: %lu\n",
                   (unsigned long)SysTick->LOAD);
  pr_info_with_tag("SYS", "  SysTick VAL: %lu\n",
                   (unsigned long)SysTick->VAL);
  pr_info_with_tag("SYS", "  VTOR: 0x%08lX\n",
                   (unsigned long)SCB->VTOR);
  pr_info_with_tag("SYS", "  SCB_ICSR: 0x%08lX\n",
                   (unsigned long)SCB->ICSR);

  /*
   * 关键诊断: 直接调用HAL_IncTick, 然后再次读取tick值。
   * 如果tick增加了, 说明HAL_IncTick工作正常,
   * 只是SysTick的硬件中断没有被CPU响应(VECTPENDING=15但VECTACTIVE=0)。
   */
  HAL_IncTick();
  pr_info_with_tag("SYS", "  System tick after manual HAL_IncTick: %lu ms\n",
                   (unsigned long)HAL_GetTick());

  /*
   * 直接调用SysTick_Handler, 验证Handler是否被正确链接。
   */
  {
          extern void SysTick_Handler(void);
          SysTick_Handler();
  }
  pr_info_with_tag("SYS", "  System tick after manual SysTick_Handler call: %lu ms\n",
                   (unsigned long)HAL_GetTick());

  {
          volatile uint32_t i;
          for (i = 0; i < 250000; i++)  /* ~50ms @168MHz */
          {
                  __NOP();
          }
  }

  pr_info_with_tag("SYS", "[DEBUG] >>> About to call osKernelStart() <<<\n");

  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();  /* Call init function for freertos objects (in cmsis_os2.c) */
  MX_FREERTOS_Init();

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  /*
   * FALLBACK: osKernelStart() 返回了! 这意味着调度器启动失败。
   * 可能原因: 堆内存不足(无法创建空闲任务)、CMSIS-RTOS2状态错误。
   */
  pr_error_with_tag("SYS", "FATAL: osKernelStart() returned! Scheduler failed to start!\n");
  Error_Handler();
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_LSI|RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM6 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM6)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /*
   * 尝试通过 USART1 输出 "ERR\n" 再进入死循环
   * 使用直接寄存器操作, 不依赖 HAL (HAL 可能已损坏)
   */
  {
          volatile uint32_t i;

          RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
          RCC->APB2ENR |= RCC_APB2ENR_USART1EN;

          GPIOA->MODER &= ~GPIO_MODER_MODER9;
          GPIOA->MODER |= GPIO_MODER_MODER9_1;
          GPIOA->AFR[1] &= ~(0xF << 4);
          GPIOA->AFR[1] |= (7 << 4);

          USART1->BRR = 139;
          USART1->CR1 = USART_CR1_UE | USART_CR1_TE;

          for (i = 0; i < 100000; i++)
                  __NOP();

          USART1->DR = 'E';
          while (!(USART1->SR & USART_SR_TXE))
                  ;
          USART1->DR = 'R';
          while (!(USART1->SR & USART_SR_TXE))
                  ;
          USART1->DR = 'R';
          while (!(USART1->SR & USART_SR_TXE))
                  ;
          USART1->DR = '\r';
          while (!(USART1->SR & USART_SR_TXE))
                  ;
          USART1->DR = '\n';
          while (!(USART1->SR & USART_SR_TXE))
                  ;
  }

  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User code can be added here */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
