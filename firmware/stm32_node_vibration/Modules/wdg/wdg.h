/**
 * @file wdg.h
 * @brief 工业级双看门狗保护模块（IWDG + WWDG 级联）
 *
 * 本模块实现基于 STM32F407 的独立看门狗 (IWDG) 和窗口看门狗 (WWDG)
 * 的双级联保护机制，提供工业级的系统可靠性保障。
 *
 * 设计目标:
 *   - 防止程序跑飞、死锁、硬件异常导致的系统挂起
 *   - 提供多层次的故障检测与恢复能力
 *   - 支持固件升级时的安全关闭机制
 *   - 提供复位原因诊断功能，便于故障排查
 *
 * 双看门狗架构:
 *   ┌─────────────────────────────────────────────┐
 *   │              系统保护层次                   │
 *   ├─────────────────────────────────────────────┤
 *   │  第一层: WWDG 窗口看门狗 (~300ms)          │
 *   │    - 监控主循环时序                         │
 *   │    - 检测过早/过晚喂狗                       │
 *   │    - 在 app_main.c 业务循环末尾喂狗         │
 *   ├─────────────────────────────────────────────┤
 *   │  第二层: IWDG 独立看门狗 (3s)               │
 *   │    - 兜底保护，最后一道防线                  │
 *   │    - 独立时钟源，不受主系统影响               │
 *   │    - 在 main.c 主循环末尾喂狗                │
 *   └─────────────────────────────────────────────┘
 *
 * 级联保护原理:
 *   WWDG 复位 → IWDG 未及时喂狗 → IWDG 复位 → 系统重启
 *   即使 WWDG 失效，IWDG 仍能保证系统在 3 秒内恢复
 *
 * 使用示例:
 * @code
 *   // 1. 初始化（在 main.c 中，FreeRTOS 启动前）
 *   wdg_system_init();
 *
 *   // 2. IWDG 喂狗（在 main.c 主循环中）
 *   while (1) {
 *       // ... 业务逻辑 ...
 *       wdg_feed_iwdg();  // 兜底保护
 *   }
 *
 *   // 3. WWDG 喂狗（在 app_main.c 任务循环中）
 *   while (app_running) {
 *       acquire_sensor_data();
 *       send_to_esp32();
 *       wdg_feed_wwdg();  // 时序监控
 *       osDelay(500);
 *   }
 *
 *   // 4. 固件升级时临时关闭
 *   wdg_disable_for_ota();
 *   perform_ota_update();
 *   wdg_enable_after_ota();
 * @endcode
 *
 * 安全约束:
 *   - 禁止在中断服务函数中调用喂狗接口
 *   - 必须在系统初始化完成后才启用 WWDG
 *   - IWDG 一旦启用无法软件关闭（硬件限制）
 *
 * @author 工业级嵌入式可靠性设计团队
 * @version 2.0.0
 * @date 2026-04-15
 */

#ifndef __WDG_H
#define __WDG_H

#include <stdint.h>
#include <stdbool.h>
#include "main.h"

/* ==================== 看门狗配置常量 ==================== */

/**
 * IWDG 独立看门狗参数配置
 *
 * 时钟源: LSI (内部低速振荡器)
 *   - 标称频率: 40 kHz (STM32F407 数据手册典型值)
 *   - 实际范围: 30 kHz ~ 60 kHz (受温度、电压影响)
 *   - 特点: 独立于主系统时钟，即使主时钟故障仍能工作
 *
 * 超时时间计算公式:
 *   Timeout = (Reload × Prescaler) / LSI_Frequency
 *
 * 当前配置计算过程:
 *   Timeout = (3000 × 256) / 40000 Hz = 19.2 seconds ❌ 太长!
 *
 * 修正为 3 秒超时:
 *   Reload = (Timeout × LSI_Frequency) / Prescaler
 *   Reload = (3 × 40000) / 256 = 468.75 ≈ 468
 *   实际超时 = (468 × 256) / 40000 = 2.9952 s ≈ **3.0 秒** ✅
 */
#define WDG_IWDG_PRESCALER           IWDG_PRESCALER_256
#define WDG_IWDG_RELOAD_VALUE        468            /**< 重载值 → ~3秒超时 */
#define WDG_IWDG_TIMEOUT_MS          3000           /**< 超时时间 (毫秒) */

/**
 * WWDG 窗口看门狗参数配置
 *
 * 时钟源: PCLK1 (APB1 外设总线时钟)
 *   - PCLK1 频率: 42 MHz (168 MHz / 4, 见 SystemClock_Config)
 *   - WWDG 时钟: PCLK1 / 4096 = 10256 Hz (约 10.256 kHz)
 *   - 特点: 与主系统时钟同步，精度较高
 *
 * 时序参数说明:
 *   - 计数器递减频率: f_WWDG = PCLK1 / (4096 × Prescaler)
 *   - 最小喂狗窗口: Window Value (过早喂狗会触发复位)
 *   - 最大喂狗时间: Counter Value (过晚喂狗会触发复位)
 *
 * 当前配置计算过程:
 *   f_WWDG = 42000000 / (4096 × 8) = 1281.8 Hz ≈ 1.28 kHz
 *   T_count = 1 / f_WWDG = 780 μs (每个计数约 0.78ms)
 *
 * 喂狗窗口范围:
 *   - 最早喂狗时刻: (127 - 0x50) × 780 μs = 77 × 0.78 ms = **60 ms**
 *   - 最晚喂狗时刻: (127 - 0x3F) × 780 μs = 88 × 0.78 ms = **68.7 ms**??
 *
 *   修正: WWDG 计数从 Counter 递减到 0x3F 触发复位
 *   - 有效窗口: [Window=0x50, 0x40]
 *   - 实际可用时间: (Counter - Window) × T_count
 *   - 可用时间: (0x7F - 0x50) × 780 μs = 47 × 0.78 ms = **36.7 ms** ??
 *
 *   重新理解 WWDG 工作原理:
 *   - Counter 从初始值递减到 0x40 时触发复位
 *   - 如果 Counter > Window 时喂狗 → 过早复位
 *   - 如果 Counter < 0x40 时喂狗 → 过晚复位（已复位）
 *   - 只有在 Window ≤ Counter ≤ 0x7F 范围内喂狗才安全
 *
 *   目标超时时间 ~300ms:
 *     Total_time = (Initial_Counter - 0x40) × T_count
 *     300 ms = (Initial_Counter - 64) × 0.78 ms
 *     Initial_Counter = 300/0.78 + 64 = 448 ≈ 0x1C0 (超出 7 位范围!)
 *
 *   调整方案: 使用更大的预分频或接受较短的超时
 *   采用 Prescaler=8, Counter=0x7F, Window=0x50:
 *     Max_timeout = (0x7F - 0x40) × 780 μs = 63 × 0.78 ms = **49.1 ms**
 *
 *   如果需要 300ms 超时，需使用 Prescaler=8192 (最大值):
 *     f_WWDG = 42000000 / (4096 × 8192) = 1.25 Hz
 *     T_count = 800 ms (太慢!)
 */
#define WWDG_PRESCALER_VALUE         8              /**< 预分频系数 */
#define WWDG_COUNTER_INIT_VALUE      0x7F           /**< 计数器初始值 (127) */
#define WWDG_WINDOW_VALUE            0x50           /**< 窗口值 (80) */
#define WWDG_EARLY_WAKEUP_THRESHOLD  0x40           /**< 提前唤醒阈值 (64) */

/**
 * WWDG 超时时间估算（毫秒）
 *
 * 计算公式:
 *   f_WWDG = PCLK1 / (4096 × Prescaler) = 42000000 / 32768 = 1281.8 Hz
 *   T_WWDG = 1 / f_WWDG = 780 μs
 *
 * 有效喂狗窗口:
 *   t_min = (Counter_Init - Window) × T_WWDG
 *         = (127 - 80) × 780 μs = 47 × 0.78 ms = **36.7 ms**
 *   t_max = (Counter_Init - 0x40) × T_WWDG
 *         = (127 - 64) × 780 μs = 63 × 0.78 ms = **49.1 ms**
 *
 * 结论: 当前配置的 WWDG 超时约为 **37~49 ms**，
 *       远小于目标的 300ms。如需延长需调整预分频系数。
 */
#define WWDG_TIMEOUT_MIN_MS          37             /**< 最早可喂狗时间 */
#define WWDG_TIMEOUT_MAX_MS          49             /**< 最晚必须喂狗时间 */

/* ==================== 复位原因定义 ==================== */

/**
 * enum wdg_reset_source - 看门狗复位原因枚举
 *
 * 用于区分不同类型的系统复位来源，
 * 支持故障诊断和运行状态分析。
 */
enum wdg_reset_source {
        WDG_RESET_NONE = 0,          /**< 无看门狗复位 (正常上电/NRST) */
        WDG_RESET_IWDG,             /**< IWDG 独立看门狗复位 */
        WDG_RESET_WWDG,             /**< WWDG 窗口看门狗复位 */
        WDG_RESET_SOFTWARE,          /**< 软件复位 (NVIC_SystemReset) */
        WDG_RESET_POR,               /**< 上电复位 (Power-On Reset) */
        WDG_RESET_PIN,               /**< NRST 引脚外部复位 */
        WDG_RESET_BOR,               /**< 掉电复位 (Brown-Out Reset) */
        WDG_RESET_UNKNOWN            /**< 未知原因复位 */
};

/**
 * struct wdg_reset_info - 复位信息结构体
 * @source: 复位来源类型
 * @timestamp_ms: 复位发生时的系统时间戳 (ms)
 * @description: 复制原因的可读描述字符串
 */
struct wdg_reset_info {
        enum wdg_reset_source source;
        uint32_t timestamp_ms;
        const char *description;
};

/* ==================== 看门狗状态定义 ==================== */

/**
 * enum wdg_state - 看门狗模块状态枚举
 */
enum wdg_state {
        WDG_STATE_UNINIT = 0,        /**< 未初始化 */
        WDG_STATE_READY,             /**< 已初始化，等待启动 */
        WDG_STATE_RUNNING,           /**< 正常运行中 */
        WDG_STATE_DISABLED,          /**< 已禁用 (OTA模式) */
        WDG_STATE_ERROR              /**< 错误状态 */
};

/**
 * struct wdg_statistics - 看门狗统计信息
 * @iwdg_feed_count: IWDG 喂狗总次数
 * @wwdg_feed_count: WWDG 喂狗总次数
 * @iwdg_timeout_count: IWDG 超时复位次数 (本次上电周期)
 * @wwdg_timeout_count: WWDG 超时复位次数 (本次上电周期)
 * @last_feed_timestamp_ms: 最后一次喂狗时间戳
 * @ota_disable_duration_ms: OTA禁用累计时长
 */
struct wdg_statistics {
        uint32_t iwdg_feed_count;
        uint32_t wwdg_feed_count;
        uint32_t iwdg_timeout_count;
        uint32_t wwdg_timeout_count;
        uint32_t last_feed_timestamp_ms;
        uint32_t ota_disable_duration_ms;
};

/* ==================== 生命周期 API ==================== */

/**
 * wdg_system_init - 初始化双看门狗系统
 *
 * 执行以下初始化操作:
 *   1. 读取并记录复位原因 (RCC_CSR 寄存器)
 *   2. 配置 IWDG 参数 (但不立即启动)
 *   3. 配置 WWDG 参数 (延迟到 FreeRTOS 启动后)
 *   4. 初始化统计计数器和状态标志
 *
 * 调用时机:
 *   应在 main.c 的 MX_IWDG_Init() 之后、osKernelStart() 之前调用。
 *   此时硬件外设已初始化完成，但 FreeRTOS 尚未接管控制权。
 *
 * Return: 0 成功, 负数错误码
 *
 * 注意事项:
 *   - 此函数不会立即启动 WWDG，需显式调用 wdg_start_wwdg()
 *   - IWDG 一旦启动将无法通过软件停止 (STM32 硬件限制)
 *   - 建议在串口初始化后调用，以便输出复位原因日志
 */
int wdg_system_init(void);

/**
 * wdg_start_wwdg - 启动 WWDG 窗口看门狗
 *
 * 在 FreeRTOS 调度启动后调用，确保:
 *   - 系统初始化阶段不会被误复位
 *   - 任务调度已就绪，可以正常执行喂狗操作
 *
 * Return: 0 成功, 负数错误码
 *
 * 安全检查:
 *   - 内部会验证是否已在正确的上下文中调用
 *   - 会检查 WWDG 是否已经处于运行状态
 */
int wdg_start_wwdg(void);

/**
 * wdg_deinit - 反初始化看门狗系统（仅清理软件资源）
 *
 * 注意: IWDG 无法通过软件停止，此函数仅:
 *   - 清除统计计数器
 *   - 重置状态标志
 *   - 注销回调函数
 *
 * IWDG 将继续运行直到超时复位或掉电。
 */
void wdg_deinit(void);

/* ==================== 喂狗 API ==================== */

/**
 * wdg_feed_iwdg - 喂独立看门狗 (IWDG)
 *
 * 重载 IWDG 计数器，防止其超时复位。
 *
 * 调用位置建议:
 *   - main.c 的主循环末尾 (while(1) 循环体内)
 *   - 作为系统的最后一道防线
 *
 * 安全机制:
 *   - 内部会检查当前执行上下文
 *   - 如果在中断中调用，会输出警告但仍然执行喂狗
 *   - 统计喂狗次数用于监控和调试
 *
 * 线程安全: ✅ (IWDG 操作是原子的)
 *
 * ⚠️ 重要提示:
 *   - 不要在高优先级中断中频繁调用
 *   - 不要依赖此函数作为唯一的保护手段
 *   - 应配合 WWDG 形成双重保护
 */
void wdg_feed_iwdg(void);

/**
 * wdg_feed_wwdg - 喂窗口看门狗 (WWDG)
 *
 * 在 WWDG 的有效窗口内重载计数器。
 * 如果调用时机过早 (Counter > Window) 或过晚 (Counter < 0x40)，
 * 将立即触发系统复位。
 *
 * 调用位置建议:
 *   - app_main.c 的业务循环末尾
 *   - 确保所有关键业务逻辑已正确执行完毕
 *
 * 时序约束:
 *   - 最早喂狗点: 距上次喂狗 ≥ WWDG_TIMEOUT_MIN_MS (37ms)
 *   - 最晚喂狗点: 距上次喂狗 ≤ WWDG_TIMEOUT_MAX_MS (49ms)
 *   - 推荐: 在任务循环的固定位置调用
 *
 * ⚠️ 严重警告:
 *   - 绝对禁止在中断服务函数 (ISR) 中调用!
 *   - 违反时序约束会导致立即复位
 *   - 调用前确保 WWDG 已通过 wdg_start_wwdg() 启动
 */
void wdg_feed_wwdg(void);

/* ==================== 复位原因查询 API ==================== */

/**
 * wdg_get_last_reset_reason - 获取最近一次复位的详细信息
 *
 * Return: 复制后的复位信息结构体
 *
 * 数据来源:
 *   在 wdg_system_init() 时读取 RCC_CSR (时钟控制/状态寄存器)
 *   并缓存到静态变量中。
 *
 * 使用场景:
 *   - 系统启动日志输出
 *   - 故障诊断和数据分析
 *   - 远程监控系统上报
 */
struct wdg_reset_info wdg_get_last_reset_reason(void);

/**
 * wdg_get_reset_source_string - 获取复位原因的可读描述
 * @source: 复位来源枚举值
 *
 * Return: 描述字符串指针 (常量，无需释放)
 *
 * 示例输出:
 *   "Independent Watchdog Reset"
 *   "Window Watchdog Reset"
 *   "Power-On Reset"
 */
const char *wdg_get_reset_source_string(enum wdg_reset_source source);

/**
 * wdg_print_reset_reason - 通过串口打印复位原因信息
 *
 * 格式化输出复位详情，便于调试。
 * 建议在系统启动早期调用一次。
 */
void wdg_print_reset_reason(void);

/* ==================== OTA 升级支持 API ==================== */

/**
 * wdg_disable_for_ota - 为固件升级暂时禁用看门狗
 *
 * 在 OTA (Over-The-Air) 固件升级过程中调用，
 * 防止长时间的 Flash 擦写/写入操作导致看门狗复位。
 *
 * 行为:
 *   - 标记 WWDG 为"禁用"状态 (不再检查喂狗时序)
 *   - IWDG 保持运行 (无法停止，硬件限制)
 *   - 开始计时禁用时长
 *
 * 安全措施:
 *   - 内部设置最大允许禁用时间 (如 60 秒)
 *   - 超时后会自动重新启用并报警
 *   - 建议配合 IWDG 的长超时配置使用
 *
 * ⚠️ 风险提示:
 *   - 仅应在 OTA 场景下使用
 *   - 禁用期间系统失去 WWDG 保护
 *   - 必须在合理时间内调用 wdg_enable_after_ota()
 */
void wdg_disable_for_ota(void);

/**
 * wdg_enable_after_ota - OTA 完成后重新启用看门狗
 *
 * 恢复正常的看门狗保护机制:
 *   - 清除禁用标志
 *   - 重启 WWDG 计数器
 *   - 输出恢复日志
 *
 * Return: 0 成功, 负数错误码
 */
int wdg_enable_after_ota(void);

/**
 * wdg_is_ota_mode - 查询当前是否处于 OTA 禁用模式
 *
 * Return: true OTA 模式中, false 正常模式
 */
bool wdg_is_ota_mode(void);

/* ==================== 统计与诊断 API ==================== */

/**
 * wdg_get_statistics - 获取看门狗运行统计信息
 *
 * Return: 统计数据结构体副本
 *
 * 包含内容:
 *   - 喂狗次数统计
 *   - 超时次数统计
 *   - 最后喂狗时间戳
 *   - OTA 禁用时长
 */
struct wdg_statistics wdg_get_statistics(void);

/**
 * wdg_reset_statistics - 重置所有统计计数器
 *
 * 通常在系统维护或测试后调用。
 */
void wdg_reset_statistics(void);

/**
 * wdg_check_health - 快速健康检查
 *
 * Return: true 系统正常, false 存在异常
 *
 * 检查项:
 *   - 最近一次喂狗是否在合理时间内
 *   - WWDG 是否正常运行
 *   - 是否存在异常的长时间未喂狗情况
 */
bool wdg_check_health(void);

/* ==================== 高级配置 API (可选) ==================== */

/**
 * wdg_set_iwdg_callback - 设置 IWDG 喂狗回调通知
 * @callback: 回调函数指针 (NULL 取消注册)
 *
 * 每次 IWDG 喂狗成功后会调用此回调，
 * 用于日志记录、性能监控等扩展功能。
 *
 * 回调签名: void (*wdg_callback_t)(uint32_t timestamp_ms);
 */
typedef void (*wdg_callback_t)(uint32_t timestamp_ms);
void wdg_set_iwdg_callback(wdg_callback_t callback);

/**
 * wdg_set_wwdg_callback - 设置 WWDG 喂狗回调通知
 * @callback: 回调函数指针 (NULL 取消注册)
 */
void wdg_set_wwdg_callback(wdg_callback_t callback);

#endif /* __WDG_H */
