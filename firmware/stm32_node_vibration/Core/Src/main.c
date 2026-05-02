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
#include "adc.h"
#include "can.h"
#include "dma.h"
#include "fsmc.h"
#include "i2c.h"
#include "iwdg.h"
#include "rtc.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "wwdg.h"
#include "gpio.h"

#include <string.h>
#include <stdio.h>

/* USER CODE BEGIN Includes */
#include "system_log/system_log.h"
#include "cmsis_os2.h"                 /* CMSIS-RTOS2 API (osKernelStart) */
#include "uart_log/uart_log.h"
/* USER CODE END Includes */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
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

  /* MCU Configuration--------------------------------------------------------*/
  HAL_Init();

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /*
   * ⚠️ 【关键修复】移除DWT计时器初始化!
   *
   * 原始代码问题:
   *   - DWT (Data Watchpoint and Trace) 是Cortex-M4调试组件
   *   - 在FreeRTOS环境下,DWT会导致以下致命问题:
   *     [1] 与SysTick/TIM6中断冲突,导致时间基异常
   *     [2] 影响FreeRTOS调度器的上下文切换
   *     [3] 导致任务栈指针异常,触发HardFault
   *     [4] HAL_GetTick()返回0,时间戳全部显示00:00:00.000
   *
   * 表现症状:
   *   - 系统启动后时间戳全为0
   *   - FreeRTOS任务创建后卡死,无任何输出
   *   - 温湿度数据无法打印
   *
   * 正确做法:
   *   - FreeRTOS任务中使用osDelay()进行延时(已实现)
   *   - main()中使用HAL_Delay()进行延时(启动阶段)
   *   - 不需要微秒级精度延时,毫秒级足够
   */

  /*
   * ⚠️ 【关键修复】最先初始化日志系统和UART1!
   *
   * 原始代码BUG:
   *   - 第102-177行调用pr_info_with_tag(),但日志系统在第188行才初始化
   *   - 导致system_log静默丢弃所有早期日志(见system_log.c:123)
   *   - 无法看到任何调试信息,难以定位问题!
   *
   * 修复方案:
   *   - 最先初始化USART1(物理层)
   *   - 然后立即初始化日志系统(逻辑层)
   *   - 之后的所有pr_info_with_tag()都能正常输出!
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

  pr_info_with_tag("SYS", "========================================\n");
  pr_info_with_tag("SYS", " STM32F407 Enterprise System V2.0\n");
  pr_info_with_tag("SYS", " Bootloader Mode (FreeRTOS Ready)\n");
  pr_info_with_tag("SYS", "========================================\n\n");

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();

  /* USER CODE BEGIN Init */

  /*
   * ======== 初始化所有外设 (企业级完整版) ========
   * 注意: 看门狗(IWDG/WWDG)保持禁用，便于调试
   *
   * ⚠️ 【调试模式】每个步骤都有详细日志输出
   * 如果卡在某个位置,最后一条日志就是卡死点!
   */

  /* 基础通信外设 */
  /* 注意: USART1和日志系统已在SysInit中初始化,这里不再重复 */

  pr_info_with_tag("SYS", "[DEBUG] Initializing USART3...\n");
  MX_USART3_UART_Init();         /* 预留串口 */
  pr_info_with_tag("SYS", "[DEBUG] USART3 OK\n");

  pr_info_with_tag("SYS", "[DEBUG] Initializing UART4 (ESP32 comm)...\n");
  MX_UART4_Init();               /* ESP32通信主通道 */
  pr_info_with_tag("SYS", "[DEBUG] UART4 OK\n");
  /* MX_USART6_UART_Init();        HMI触摸屏 - 暂未启用 */

  /* 总线外设 */
  pr_info_with_tag("SYS", "[DEBUG] Initializing FSMC (LCD bus)...\n");
  MX_FSMC_Init();                /* LCD显示屏 (16位并行) */
  pr_info_with_tag("SYS", "[DEBUG] FSMC OK - LCD bus ready\n");

  pr_info_with_tag("SYS", "[DEBUG] Initializing SPI2...\n");
  MX_SPI2_Init();                /* Flash/触摸控制器 */
  pr_info_with_tag("SYS", "[DEBUG] SPI2 OK\n");

  pr_info_with_tag("SYS", "[DEBUG] Initializing I2C2...\n");
  MX_I2C2_Init();                /* EEPROM/RTC备份 */
  pr_info_with_tag("SYS", "[DEBUG] I2C2 OK\n");

  /* 定时器外设 (仅启用的) */
  pr_info_with_tag("SYS", "[DEBUG] Initializing TIM1...\n");
  MX_TIM1_Init();                /* 高级定时器 (PWM/编码器) */
  pr_info_with_tag("SYS", "[DEBUG] TIM1 OK\n");
  /* MX_TIM2_Init();              通用定时器 - 暂未启用 */
  pr_info_with_tag("SYS", "[DEBUG] Initializing TIM3...\n");
  MX_TIM3_Init();                /* 通用定时器 */
  pr_info_with_tag("SYS", "[DEBUG] TIM3 OK\n");
  /* MX_TIM4~MX_TIM14: 暂未启用 */

  /* ADC/DMA */
  pr_info_with_tag("SYS", "[DEBUG] Initializing ADC1...\n");
  MX_ADC1_Init();                /* 振动/电压采集 */
  pr_info_with_tag("SYS", "[DEBUG] ADC1 OK\n");

  pr_info_with_tag("SYS", "[DEBUG] Initializing DMA...\n");
  MX_DMA_Init();                 /* DMA控制器 */
  pr_info_with_tag("SYS", "[DEBUG] DMA OK\n");

  /* CAN总线 */
  pr_info_with_tag("SYS", "[DEBUG] Initializing CAN1...\n");
  MX_CAN1_Init();                /* CAN总线通信 */
  pr_info_with_tag("SYS", "[DEBUG] CAN1 OK\n");

  /* RTC实时时钟 */
  pr_info_with_tag("SYS", "[DEBUG] Initializing RTC...\n");
  MX_RTC_Init();                 /* 系统时间戳 */
  pr_info_with_tag("SYS", "[DEBUG] RTC OK\n");

  /*
   * ⚠️ 看门狗禁用 (调试阶段)
   * 生产环境请启用:
   *   MX_IWDG_Init();           // 独立看门狗
   *   MX_WWDG_Init();           // 窗口看门狗
   */

  pr_info_with_tag("SYS", "[OK] All peripherals initialized (except watchdog)\n");

  /*
   * ⚠️ 注意: 日志系统已在SysInit阶段(最前面)初始化!
   * 原始代码在这里初始化导致早期日志全部丢失
   */

  pr_info_with_tag("SYS", "[OK] Logging system ready (initialized at SysInit stage)\n");
  pr_info_with_tag("SYS", "[OK] Starting FreeRTOS scheduler...\n\n");

  /* USER CODE END Init */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */

  /*
   * ======== 启动FreeRTOS调度器 ========
   *
   * ⚠️ 【关键修复】必须先调用 MX_FREERTOS_Init() 创建任务!
   *
   * 原始代码BUG:
   *   - 直接调用 osKernelStart() 但没有先创建任何任务!
   *   - 导致调度器启动后无任务可运行 → 系统空转
   *   - 表现为"启动后卡死",实际是没有任务存在
   *
   * 正确的FreeRTOS启动流程 (STM32CubeMX标准):
   *   1. MX_FREERTOS_Init()  → 创建所有任务/信号量/队列
   *   2. osKernelStart()     → 启动调度器,开始执行任务
   *
   * 创建的任务:
   *   - defaultTask (系统默认任务,低优先级)
   *   - app_enterprise (企业级应用任务,Normal优先级)
   *   - lvgl_gui (GUI刷新任务,如果启用USE_GUI)
   */

  /*
   * 步骤0: 初始化FreeRTOS内核 (CMSIS-RTOS2)
   *
   * ⚠️ 【关键修复】必须先调用 osKernelInitialize()!
   *
   * 致命BUG（已修复）:
   *   原始代码直接调用 MX_FREERTOS_Init() 和 osKernelStart()
   *   但没有先初始化CMSIS-RTOS2内核!
   *   导致 KernelState 仍然是 osKernelInactive
   *   osKernelStart() 检查状态失败,返回 osError!
   *
   * CMSIS-RTOS2 状态机:
   *   osKernelInactive → osKernelInitialize() → osKernelReady
   *   osKernelReady     → osKernelStart()      → osKernelRunning
   *
   * 正确流程:
   *   1. osKernelInitialize()  - 初始化内核,设置状态为 Ready
   *   2. MX_FREERTOS_Init()    - 创建任务/信号量/队列
   *   3. osKernelStart()       - 启动调度器(检查状态必须是Ready)
   */
  {
          osStatus_t init_stat;

          pr_info_with_tag("SYS", "[DEBUG] Initializing CMSIS-RTOS2 kernel...\n");
          init_stat = osKernelInitialize();

          if (init_stat != osOK) {
                  pr_error_with_tag("SYS", "FATAL: osKernelInitialize() failed! status=%d\n",
                                  (int)init_stat);
                  Error_Handler();
          }

          pr_info_with_tag("SYS", "[OK] CMSIS-RTOS2 kernel initialized\n");
  }

  /*
   * 步骤1: 初始化FreeRTOS并创建所有任务
   */
  pr_info_with_tag("SYS", "[DEBUG] Calling MX_FREERTOS_Init() to create tasks...\n");
  MX_FREERTOS_Init();
  pr_info_with_tag("SYS", "[DEBUG] MX_FREERTOS_Init() completed - tasks created\n");

  /*
   * ⚠️ 【关键修复】移除HAL_Delay - 避免FreeRTOS启动前死循环!
   *
   * 致命BUG（已修复）:
   *   原始代码使用 HAL_Delay(100/50/10) 进行UART缓冲区刷新
   *   但在FreeRTOS调度器启动前(osKernelStart之前):
   *     [1] TIM6时间基可能未正常工作 → HAL_GetTick()返回0
   *     [2] HAL_Delay(100) 内部: while((GetTick()-0)<100) → 死循环!
   *     [3] 系统永远卡在MX_FREERTOS_Init()之后,无法启动调度器!
   *
   * 表现症状:
   *   - 日志最后一行: "MX_FREERTOS_Init() completed"
   *   - 时间戳全为00:00:00.000
   *   - FreeRTOS任务从未执行
   *   - 温湿度数据无法打印
   *
   * 正确做法:
   *   - 使用简单的忙等待循环(NOP)代替HAL_Delay
   *   - 或者完全移除延时(UART发送足够快,不需要额外等待)
   *   - FreeRTOS任务中使用osDelay()(已正确实现)
   */
  {
          volatile uint32_t i;
          for (i = 0; i < 500000; i++)  /* ~100ms @168MHz */
          {
                  __NOP();
          }
  }

  /*
   * 步骤1.5: 启动前的系统健康检查
   */
  {
          pr_info_with_tag("SYS", "[HEALTH] Pre-scheduler check:\n");
          pr_info_with_tag("SYS", "  System tick: %lu ms\n", (unsigned long)HAL_GetTick());
          pr_info_with_tag("SYS", "  MSP (main stack): 0x%08X\n", (unsigned int)__get_MSP());
  }

  {
          volatile uint32_t i;
          for (i = 0; i < 250000; i++)  /* ~50ms @168MHz */
          {
                  __NOP();
          }
  }

  /*
   * 步骤2: 启动FreeRTOS调度器
   * 调度器将接管控制权,永远不会返回!
   *
   * ⚠️ 注意: 此函数内部会:
   *   1. 配置SysTick中断
   *   2. 启动第一个任务
   *   3. 切换到PSP (进程栈指针)
   *   4. 如果任何步骤失败,会导致HardFault
   */
  pr_info_with_tag("SYS", "[DEBUG] >>> About to call osKernelStart() <<<\n");

  {
          volatile uint32_t i;
          for (i = 0; i < 50000; i++)  /* ~10ms @168MHz */
          {
                  __NOP();
          }
  }

  /*
   * ⚠️ 【最后的机会】如果在osKernelStart()内部崩溃,
   *      我们将无法看到后续日志。此时应该检查:
   *   1. HardFault_Handler() 是否被触发
   *   2. 是否有栈溢出 (已启用检测)
   *   3. 内存是否足够 (heap_4.c 分配失败)
   */
  osKernelStart();

  /*
   * 理论上不可达 - 但如果osKernelStart()返回了,说明严重错误!
   */
  pr_error_with_tag("SYS", "FATAL: osKernelStart() returned unexpectedly!\n");
  Error_Handler();

  /* USER CODE END WHILE */

  /* USER CODE BEGIN 3 */

  /* 此处不可达 (osKernelStart不返回) */

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
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV1;
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
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the line number
  *         where the assert_param error occurred.
  * @param  file: pointer to the source file where the assert_param failed.
  * @param  line: line number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User code can be added here */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
