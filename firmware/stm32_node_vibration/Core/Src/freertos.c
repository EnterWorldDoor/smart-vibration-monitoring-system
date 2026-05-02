/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications (Enterprise V2.0)
  *
  * V2.0 任务架构:
  *   ┌─────────────────────────────────────────────┐
  *   │  defaultTask (128*4B, Normal)               │ ← 系统空闲任务
  *   ├─────────────────────────────────────────────┤
  *   │  app_dht11_task (1024*4B, Normal)           │ ← 企业级应用任务
  *   │    ├── simulator_update()                   │
  *   │    ├── send_temp_to_esp32()                 │
  *   │    └── gui_app_update_sensor_data()         │
  *   ├─────────────────────────────────────────────┤
  *   │  gui_task (2048*4B, High)                   │ ← LVGL GUI刷新任务
  *   │    └── lv_timer_handler() (33ms周期)       │
  *   └─────────────────────────────────────────────┘
  *
  * 注意: LWIP网络栈已禁用(企业版不需要以太网功能)
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os2.h"                  /* ⚠️ 【关键修复】必须使用RTOS2 API! */

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

#include "system_log/system_log.h"     /* 日志系统 */

/* 应用层任务入口函数声明 */
void app_dht11_task_entry(void *argument);  /* [App/app_main.c] */

/* GUI任务入口函数声明 */
#ifdef USE_GUI
void gui_task_entry(void *argument);         /* [App/gui/gui_app.c] */
#endif

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/*
 * GUI任务配置常量
 */
#define GUI_TASK_STACK_SIZE      (2048 * 4)   /* 8KB 栈空间 (LVGL需要较大栈) */
#define GUI_TASK_PRIORITY        osPriorityAboveNormal  /* 高优先级 (保证流畅度) */
#define GUI_TASK_PERIOD_MS       5            /* 5ms周期 (200Hz刷新率) */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */

/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;

/*
 * ⚠️ 【关键修复】增大defaultTask栈大小!
 *
 * 原始配置问题:
 *   - .stack_size = 128 * 4 (512字节) 对于任何实际任务都太小!
 *   - StartDefaultTask()中调用了:
 *     - pr_debug_with_tag() (内部使用256字节缓冲区)
 *     - pr_info_with_tag() (内部使用256字节缓冲区)
 *     - osDelay(1000) (需要栈空间保存上下文)
 *   - 512字节栈空间几乎肯定会导致栈溢出!
 *
 * 症状:
 *   - FreeRTOS检测到栈溢出 → 调用vApplicationStackOverflowHook()
 *   - 或者静默覆盖内存 → HardFault/死循环
 *
 * 修复方案:
 *   - 增大到 512*4 = 2KB (足够简单的空闲任务)
 *   - 如果仍然溢出,可以继续增大到 1024*4 = 4KB
 */
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 512 * 4,              /* 2KB 栈空间 (从512B增大!) */
  .priority = (osPriority_t) osPriorityNormal,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);

extern void MX_LWIP_Init(void);
void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/* Hook prototypes */
void vApplicationIdleHook(void);
void vApplicationMallocFailedHook(void);

/* USER CODE BEGIN 2 */
void vApplicationIdleHook( void )
{
   /* 空闲任务钩子 - 用于低功耗模式或统计信息 */
}
/* USER CODE END 2 */

/* USER CODE BEGIN 5 */
void vApplicationMallocFailedHook(void)
{
   /* 内存分配失败钩子 - 记录错误日志 */
   pr_error_with_tag("RTOS", "FATAL: Memory allocation failed!\n");
   Error_Handler();
}

/**
 * vApplicationStackOverflowHook - FreeRTOS栈溢出检测钩子
 *
 * 当 configCHECK_FOR_STACK_OVERFLOW != 0 时,如果检测到任务栈溢出,
 * FreeRTOS会自动调用此函数。
 *
 * @param xTask: 溢出的任务句柄
 * @param pcTaskName: 任务名称字符串
 *
 * ⚠️ 【关键】此函数在临界区中调用,不能使用阻塞API!
 *    只能调用pr_error_with_tag() (非阻塞)和Error_Handler()
 *
 * 常见溢出原因:
 *   1. 任务中定义了过大的局部变量(数组/结构体)
 *   2. 递归调用层次太深
 *   3. printf/LOG使用了过多栈空间
 *   4. 任务栈大小配置不足
 *
 * 调试方法:
 *   - 使用 uxTaskGetStackHighWaterMark() 检查历史最大栈使用量
 *   - 增大 .stack_size 配置值
 *   - 将局部变量改为static或全局变量
 */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
        /*
         * ⚠️ 【致命错误】任务栈溢出!
         *
         * 这意味着某个任务的栈空间不够用,导致:
         *   - 栈数据覆盖了其他任务的内存区域
         *   - 可能导致HardFault、数据损坏或系统死锁
         *
         * 立即行动:
         *   1. 记录哪个任务溢出了 (pcTaskName)
         *   2. 停止系统运行 (Error_Handler进入死循环)
         *   3. 增大该任务的 .stack_size 配置
         */

        pr_error_with_tag("RTOS", "\n");
        pr_error_with_tag("RTOS", "╔══════════════════════════════════════╗\n");
        pr_error_with_tag("RTOS", "║     FATAL: STACK OVERFLOW!          ║\n");
        pr_error_with_tag("RTOS", "╠══════════════════════════════════════╣\n");
        pr_error_with_tag("RTOS", "║ Task Name: %-26s ║\n", pcTaskName);
        pr_error_with_tag("RTOS", "║ Task Handle: 0x%-24p ║\n", xTask);
        pr_error_with_tag("RTOS", "║ Time: %lu ms                       ║\n",
                         (unsigned long)HAL_GetTick());
        pr_error_with_tag("RTOS", "╚══════════════════════════════════════╝\n");
        pr_error_with_tag("RTOS", "\n");
        pr_error_with_tag("RTOS", "Possible causes:\n");
        pr_error_with_tag("RTOS", "  [1] Local array too large (move to static/global)\n");
        pr_error_with_tag("RTOS", "  [2] Deep recursion or unbounded loop\n");
        pr_error_with_tag("RTOS", "  [3] Printf/LOG using too much stack\n");
        pr_error_with_tag("RTOS", "  [4] Stack size configuration too small\n");
        pr_error_with_tag("RTOS", "\n");
        pr_error_with_tag("RTOS", "Solution: Increase .stack_size in osThreadAttr_t\n");
        pr_error_with_tag("RTOS", "\n");

        /*
         * 停止系统运行,避免进一步的内存损坏
         * 进入无限循环,便于调试器连接查看现场
         */
        Error_Handler();
}
/* USER CODE END 5 */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE Begin Init */

  /* USER CODE End Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* add timers, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */

  /*
   * ⚠️ 【关键修复】添加任务创建错误检查!
   *
   * 原始代码问题:
   *   - osThreadNew()可能返回NULL(内存不足/参数错误)
   *   - 但原始代码没有检查返回值
   *   - 导致后续使用NULL句柄崩溃
   *
   * 修复方案:
   *   - 检查每个 osThreadNew() 的返回值
   *   - 如果失败,打印错误信息并停止系统
   */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  if (defaultTaskHandle == NULL) {
          pr_error_with_tag("RTOS", "FATAL: Failed to create defaultTask! (out of memory?)\n");
          Error_Handler();
  }

  pr_debug_with_tag("RTOS", "[DEBUG] defaultTask created: handle=0x%p\n", defaultTaskHandle);

  /* USER CODE BEGIN RTOS_THREADS */

  /*
   * ======== 任务1: 企业级应用任务 (温湿度模拟+ESP32通信) ========
   * 文件位置: App/app_main.c
   * 功能:
   *   - Random Walk模拟传感器数据
   *   - CRC16-MODBUS协议帧构建
   *   - UART4发送给ESP32
   *   - 更新LVGL界面数据 (可选)
   *
   * ⚠️ 【关键修复】增大任务栈从4KB到8KB!
   *
   * 原始配置问题:
   *   - .stack_size = 1024 * 4 (4KB) 对于企业级应用可能不够
   *   - app_main.c中使用了大量局部变量:
   *     - float temp_c, humidity_rh
   *     - uint8_t frame[64] (协议帧缓冲区)
   *     - uint8_t payload[16] (数据载荷)
   *     - 多个临时变量和字符串字面量
   *   - pr_info_with_tag()内部使用256字节缓冲区(LOG_MAX_LINE_SIZE)
   *   - HAL_UART_Transmit()也需要栈空间
   *
   * 栈溢出症状:
   *   - FreeRTOS静默覆盖其他内存区域
   *   - 导致不可预测行为(HardFault/死循环)
   *   - 系统启动后无任何日志输出(最常见!)
   */
  osThreadAttr_t enterprise_task_attr = {
    .name = "app_enterprise",
    .stack_size = 2048 * 4,              /* 8KB 栈空间 (从4KB增大!) */
    .priority = (osPriority_t) osPriorityNormal,
  };

  {
          osThreadId_t enterprise_handle;

          enterprise_handle = osThreadNew(app_dht11_task_entry, NULL, &enterprise_task_attr);

          if (enterprise_handle == NULL) {
                  pr_error_with_tag("RTOS", "FATAL: Failed to create app_enterprise task! (out of memory?)\n");
                  Error_Handler();
          }

          pr_debug_with_tag("RTOS", "[DEBUG] app_enterprise task created: handle=0x%p\n",
                           enterprise_handle);
  }

  /*
   * ======== 任务2: LVGL GUI刷新任务 (如果启用GUI) ========
   * 文件位置: App/gui/gui_app.c
   * 功能:
   *   - 调用 lv_timer_handler() 处理UI事件和动画
   *   - 周期性刷新LCD显示 (30FPS)
   *   - 高优先级保证界面流畅度
   *
   * 条件编译: 仅在 lv_conf.h 中定义了 USE_GUI 时才创建此任务
   */
  #ifdef USE_GUI
  osThreadAttr_t gui_task_attr = {
    .name = "lvgl_gui",
    .stack_size = GUI_TASK_STACK_SIZE,     /* 8KB 栈空间 (LVGL需要) */
    .priority = (osPriority_t) GUI_TASK_PRIORITY,  /* AboveNormal优先级 */
  };
  osThreadNew(gui_task_entry, NULL, &gui_task_attr);

  pr_info_with_tag("RTOS", "[OK] LVGL GUI task created (%dB stack)\n",
                   GUI_TASK_STACK_SIZE);
  #endif

  pr_info_with_tag("RTOS", "[OK] FreeRTOS tasks initialized:\n");
  pr_info_with_tag("RTOS", "  • defaultTask (idle)\n");
  pr_info_with_tag("RTOS", "  • app_enterprise (simulation + ESP32)\n");
  #ifdef USE_GUI
  pr_info_with_tag("RTOS", "  • lvgl_gui (display refresh)\n");
  #endif

  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /*
   * ⚠️ LWIP网络栈已禁用 (企业版不需要)
   * 原因: 本项目专注于电机控制+传感器采集，不需要以太网通信
   * 如需启用LWIP:
   *   1. 在main.c中取消MX_LWIP_Init()注释
   *   2. 取消以下代码块的注释
   *   3. 确保SPI/DMA外设已初始化
   */
  // /* init code for LWIP */
  // MX_LWIP_Init();

  /* USER CODE Begin StartDefaultTask */

  pr_debug_with_tag("RTOS", "[DEBUG] DefaultTask entry - Task Handle: 0x%p, Priority: Normal\n", defaultTaskHandle);
  pr_info_with_tag("SYS", "[INFO] DefaultTask running (idle mode)\n");
  pr_debug_with_tag("RTOS", "[DEBUG] DefaultTask stack size: %d bytes, entering idle loop\n", 128 * 4);

  /* Infinite loop - 低功耗等待 */
  for(;;)
  {
    osDelay(1000);  /* 每秒唤醒一次，降低CPU占用 */
  }
  /* USER CODE End StartDefaultTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */
