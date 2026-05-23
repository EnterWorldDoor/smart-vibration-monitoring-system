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
#include "cmsis_os2.h"                  /* CMSIS-RTOS2 API */

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

#include "system_log/system_log.h"     /* 日志系统 */
#include "wdg/wdg_heartbeat.h"          /* 看门狗守护任务 */

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
 * USE_GUI 已统一迁移至 Core/Inc/main.h (Private defines 区域)
 *
 * 启用/禁用GUI请修改 main.h，此文件通过 #include "main.h" 继承该定义。
 *
 * 原 FIXME 说明:
 *   之前因 FSMC 时序 (DATAST=9) 导致 LCD 写入异常触发 HardFault,
 *   现已修复为 DATAST=60 (在 fsmc.c 中), 可安全启用 USE_GUI。
 */

/*
 * GUI任务配置常量
 *
 * 优先级说明:
 *   - 使用 osPriorityNormal (与APP任务同级)
 *   - 避免高优先级GUI任务饥饿低优先级业务任务
 *   - FreeRTOS时间片轮转保证两个任务都能执行
 */
#define GUI_TASK_STACK_SIZE      (2048 * 4)   /* 8KB 栈空间 (LVGL需要较大栈) */
#define GUI_TASK_PRIORITY        osPriorityNormal  /* 与APP任务同级, 防止饥饿 */
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
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

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
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

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
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

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
   * ======== 任务2: 看门狗心跳守护任务 ========
   */
  {
    osThreadAttr_t wdg_task_attr = {
      .name = "wdg_daemon",
      .stack_size = 1024 * 4,              /* 4KB stack */
      .priority = (osPriority_t) osPriorityHigh,
    };
    osThreadId_t wdg_handle = osThreadNew(wdg_daemon_task, NULL, &wdg_task_attr);
    if (wdg_handle == NULL) {
      pr_error_with_tag("RTOS", "FATAL: Failed to create wdg_daemon task!\n");
      Error_Handler();
    }
  }

  /*
   * ======== 任务3: LVGL GUI刷新任务 (如果启用GUI) ========
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
    .stack_size = GUI_TASK_STACK_SIZE,
    .priority = (osPriority_t) GUI_TASK_PRIORITY,
  };
  osThreadNew(gui_task_entry, NULL, &gui_task_attr);

  pr_info_with_tag("RTOS", "[OK] LVGL GUI task created (%dB stack)\n",
                   GUI_TASK_STACK_SIZE);
  #endif

  pr_info_with_tag("RTOS", "[OK] FreeRTOS tasks initialized:\n");
  pr_info_with_tag("RTOS", "  • defaultTask (idle)\n");
  pr_info_with_tag("RTOS", "  • app_enterprise (simulation + ESP32)\n");
  pr_info_with_tag("RTOS", "  • wdg_daemon (IWDG heartbeat)\n");
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
  /* init code for LWIP */
  /*
   * FIX: LWIP init disabled - enterprise app does not use Ethernet.
   * MX_LWIP_Init() hangs indefinitely when PHY (LAN8720A) has no
   * link, permanently blocking defaultTask and starving other
   * same-priority tasks.
   *
   * To re-enable Ethernet, uncomment the line below AND ensure
   * the Ethernet cable is connected or add a timeout mechanism.
   */
  /* MX_LWIP_Init(); */
  /* USER CODE BEGIN StartDefaultTask */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END StartDefaultTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */

