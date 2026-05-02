/**
 * @file wdg.c
 * @brief 工业级双看门狗保护模块实现 (IWDG + WWDG)
 *
 * 完整实现独立看门狗和窗口看门狗的级联保护机制，
 * 提供工业级嵌入式系统所需的可靠性保障。
 *
 * 核心设计原则:
 *   1. 防御性编程: 所有公共 API 都有参数检查和状态验证
 *   2. 故障快速恢复: 双看门狗确保任何单点故障都能被检测到
 *   3. 可观测性: 详细的统计信息和复位原因记录
 *   4. 安全约束: 禁止在中断中喂 WWDG，防止时序破坏
 *   5. 可扩展性: 回调机制支持日志、监控等扩展功能
 *
 * 时序关系图:
 *
 *   时间轴 ──────────────────────────────────────────────►
 *
 *   IWDG (3秒窗口):
 *   |←─────────── 3秒超时窗口 ───────────→|
 *   |                                      |
 *   [喂狗]                            [必须再次喂狗]
 *   |                                      |
 *   |←──── 主循环周期 (~1ms) ────→|        |
 *
 *   WWDG (~49ms窗口):
 *   |←── 37ms ──→|←──── 12ms ────→|
 *   [禁止区域]   [有效喂狗窗口]    [复位]
 *                |                 |
 *           [可在此喂狗]      [过晚!]
 *
 * 级联保护:
 *   如果 WWDG 未及时喂狗 → WWDG 复位 → 但 IWDG 也未喂(主循环挂起) → IWDG 复位
 *   如果 IWDG 未及时喂狗 → 直接 IWDG 复位 (最终防线)
 */

#include "wdg.h"
#include "iwdg.h"
#include "wwdg.h"
#include "system_log/system_log.h"
#include <string.h>

/* ==================== 模块内部状态 ==================== */

/**
 * struct wdg_context - 看门狗模块全局上下文
 *
 * 包含所有运行时状态、配置参数和统计信息。
 * 采用静态单一实例模式，避免动态内存分配。
 */
static struct {
        /* 运行状态 */
        enum wdg_state state;              /**< 当前模块状态 */
        bool wwdg_started;                  /**< WWDG 是否已启动 */

        /* 复位信息缓存 */
        struct wdg_reset_info last_reset;  /**< 最近一次复位原因 */

        /* 统计计数器 */
        struct wdg_statistics stats;        /**< 运行统计数据 */

        /* 回调函数 */
        wdg_callback_t iwdg_callback;       /**< IWDG 喂狗回调 */
        wdg_callback_t wwdg_callback;       /**< WWDG 喂狗回调 */

        /* OTA 模式控制 */
        bool ota_mode;                      /**< 是否处于 OTA 禁用模式 */
        uint32_t ota_start_timestamp;       /**< OTA 禁用开始时间 */
        uint32_t ota_max_duration_ms;       /**< OTA 最大允许禁用时长 */

        /* 安全标志 */
        volatile bool in_isr;               /**< 当前是否在中断上下文中执行 */
} g_wdg_ctx = {0};

/* ==================== 内部辅助函数 ==================== */

/**
 * get_timestamp_ms - 获取当前系统时间戳 (毫秒)
 *
 * Return: 自启动以来的毫秒数
 *
 * 使用 HAL_GetTick() 作为时间源，
 * 该函数在 TIM6 中断中由 HAL_IncTick() 维护。
 */
static uint32_t get_timestamp_ms(void)
{
        return HAL_GetTick();
}

/**
 * detect_reset_source - 检测并解析复位原因
 *
 * 通过读取 STM32 的 RCC_CSR (时钟控制/状态寄存器)
 * 来确定最近一次复位的类型。
 *
 * RCC_CSR 关键位说明 (STM32F407 参考手册 RM0090):
 *   - Bit 29: LPWRRSTF  低功耗复位标志
 *   - Bit 28: WWDGRSTF   窗口看门狗复位标志
 *   - Bit 27: WDGRSTF     独立看门狗复位标志
 *   - Bit 26: SOFTRSTF    软件复位标志
 *   - Bit 25: PORRSTF      上电/掉电复位 (POR/PDR) 标志
 *   - Bit 24: PINRSTF      NRST 引脚复位标志
 *   - Bit 23: BORRSTF      掉电复位标志
 *   - Bit 0:  LSIRDYF     LSI 就绪标志
 *
 * 优先级判断顺序:
 *   IWDG > WWDG > Software > POR > PIN > BOR > Unknown
 *
 * Return: 复位原因枚举值
 */
static enum wdg_reset_source detect_reset_source(void)
{
        uint32_t csr_value;
        enum wdg_reset_source source = WDG_RESET_UNKNOWN;

        csr_value = RCC->CSR;

        if (csr_value & RCC_CSR_WDGRSTF) {
                source = WDG_RESET_IWDG;
        } else if (csr_value & RCC_CSR_WWDGRSTF) {
                source = WDG_RESET_WWDG;
        } else if (csr_value & RCC_CSR_SFTRSTF) {
                source = WDG_RESET_SOFTWARE;
        } else if (csr_value & RCC_CSR_PORRSTF) {
                source = WDG_RESET_POR;
        } else if (csr_value & RCC_CSR_PINRSTF) {
                source = WDG_RESET_PIN;
        } else if (csr_value & RCC_CSR_BORRSTF) {
                source = WDG_RESET_BOR;
        } else {
                source = WDG_RESET_NONE;  /* 正常上电或无特定原因 */
        }

        return source;
}

/**
 * clear_reset_flags - 清除所有复位标志位
 *
 * 在读取完复位原因后，必须清除这些标志位，
 * 否则下次复位时无法正确判断新的复位原因。
 *
 * 操作方法:
 *   向 RCC_CSR 的 RMVF 位 (Bit 24) 写入 1 即可清除所有标志。
 *   这是 STM32 的标准"写 1 清除" (Write-1-to-Clear) 机制。
 */
static void clear_reset_flags(void)
{
        __HAL_RCC_CLEAR_RESET_FLAGS();

        RCC->CSR |= RCC_CSR_RMVF;
}

/**
 * check_interrupt_context - 检查当前是否在中断上下文中
 *
 * Return: true 正在中断中, false 在线程上下文中
 *
 * 实现原理:
 *   通过检查 IPSR (中断程序状态寄存器) 判断：
 *   - IPSR = 0: Thread mode (线程模式)
 *   - IPSR ≠ 0: Handler mode (中断处理模式)
 *
 * 用途:
 *   防止在 ISR 中调用 WWDG 喂狗函数，
 *   因为 ISR 会打断正常的任务时序，导致 WWDG 窗口失效。
 */
static bool check_interrupt_context(void)
{
        return (__get_IPSR() != 0);
}

/* ==================== 复位原因检测实现 ==================== */

/**
 * analyze_and_cache_reset_reason - 分析并缓存复位原因
 *
 * 在系统初始化早期调用一次，
 * 将复位信息保存到 g_wdg_ctx.last_reset 中供后续查询。
 */
static void analyze_and_cache_reset_reason(void)
{
        enum wdg_reset_source source;

        source = detect_reset_source();

        g_wdg_ctx.last_reset.source = source;
        g_wdg_ctx.last_reset.timestamp_ms = 0;  /* 尚无时间戳可用 */
        g_wdg_ctx.last_reset.description =
                wdg_get_reset_source_string(source);

        if (source == WDG_RESET_IWDG) {
                g_wdg_ctx.stats.iwdg_timeout_count++;
        } else if (source == WDG_RESET_WWDG) {
                g_wdg_ctx.stats.wwdg_timeout_count++;
        }

        clear_reset_flags();
}

/* ==================== 公共 API 实现 ==================== */

int wdg_system_init(void)
{
        pr_info_with_tag("WDG", "====================================\n");
        pr_info_with_tag("WDG", " Dual Watchdog System Initializing\n");
        pr_info_with_tag("WDG", " Target: ATK-DMF407 (STM32F407)\n");
        pr_info_with_tag("WDG", " IWDG: LSI 40kHz, Timeout=3s\n");
        pr_info_with_tag("WDG", " WWDG: PCLK1/4096, Window=~37-49ms\n");
        pr_info_with_tag("WDG", "====================================\n");

        memset(&g_wdg_ctx, 0, sizeof(g_wdg_ctx));
        g_wdg_ctx.state = WDG_STATE_READY;
        g_wdg_ctx.wwdg_started = false;
        g_wdg_ctx.ota_mode = false;
        g_wdg_ctx.ota_max_duration_ms = 60000;  /* OTA 最大允许 60 秒 */

        analyze_and_cache_reset_reason();

        wdg_print_reset_reason();

        pr_info_with_tag("WDG",
                         "Watchdog system initialized (WWDG delayed start)\n");

        return 0;
}

int wdg_start_wwdg(void)
{
        if (g_wdg_ctx.state == WDG_STATE_UNINIT) {
                pr_error_with_tag("WDG", "System not initialized!\n");
                return -1;
        }

        if (g_wdg_ctx.wwdg_started) {
                pr_warn_with_tag("WDG", "WWDG already running\n");
                return 0;
        }

        if (g_wdg_ctx.ota_mode) {
                pr_warn_with_tag("WDG",
                                 "Cannot start WWDG in OTA disable mode\n");
                return -2;
        }

        g_wdg_ctx.wwdg_started = true;
        g_wdg_ctx.state = WDG_STATE_RUNNING;

        pr_info_with_tag("WDG",
                         "WWDG started successfully (window=%dms-%dms)\n",
                         WWDG_TIMEOUT_MIN_MS,
                         WWDG_TIMEOUT_MAX_MS);

        return 0;
}

void wdg_deinit(void)
{
        memset(&g_wdg_ctx.stats, 0, sizeof(g_wdg_ctx.stats));
        g_wdg_ctx.iwdg_callback = NULL;
        g_wdg_ctx.wwdg_callback = NULL;
        g_wdg_ctx.state = WDG_STATE_UNINIT;
        g_wdg_ctx.wwdg_started = false;

        pr_warn_with_tag("WDG",
                         "Watchdog system deinitialized "
                         "(IWDG still running!)\n");
}

void wdg_feed_iwdg(void)
{
        HAL_StatusTypeDef status;

        g_wdg_ctx.in_isr = check_interrupt_context();

        if (g_wdg_ctx.in_isr) {
                pr_warn_with_tag("WDG",
                                 "⚠️  IWDG feed called from ISR context!\n");
        }

        status = HAL_IWDG_Refresh(&hiwdg);

        if (status == HAL_OK) {
                g_wdg_ctx.stats.iwdg_feed_count++;
                g_wdg_ctx.stats.last_feed_timestamp_ms =
                        get_timestamp_ms();

                if (g_wdg_ctx.iwdg_callback) {
                        g_wdg_ctx.iwdg_callback(
                                g_wdg_ctx.stats.last_feed_timestamp_ms);
                }
        } else {
                pr_error_with_tag("WDG",
                                  "IWDG refresh failed (status=%d)\n",
                                  status);
        }
}

void wdg_feed_wwdg(void)
{
        HAL_StatusTypeDef status;

        if (!g_wdg_ctx.wwdg_started) {
                pr_warn_with_tag("WDG",
                                 "WWDG not started, feed ignored\n");
                return;
        }

        if (g_wdg_ctx.ota_mode) {
                return;
        }

        g_wdg_ctx.in_isr = check_interrupt_context();

        if (g_wdg_ctx.in_isr) {
                pr_error_with_tag("WDG",
                                  "🚨 FATAL: WWDG feed from ISR! "
                                  "System will reset!\n");

                for (volatile int i = 0; i < 1000000; i++) {
                        __NOP();
                }

                NVIC_SystemReset();
                return;
        }

        status = HAL_WWDG_Refresh(&hwwdg);

        if (status == HAL_OK) {
                g_wdg_ctx.stats.wwdg_feed_count++;

                if (g_wdg_ctx.wwdg_callback) {
                        g_wdg_ctx.wwdg_callback(
                                get_timestamp_ms());
                }
        } else {
                pr_error_with_tag("WDG",
                                  "WWDG refresh failed (status=%d)\n",
                                  status);
        }
}

struct wdg_reset_info wdg_get_last_reset_reason(void)
{
        return g_wdg_ctx.last_reset;
}

const char *wdg_get_reset_source_string(enum wdg_reset_source source)
{
        switch (source) {
        case WDG_RESET_NONE:
                return "Normal Power-On / No Watchdog Reset";
        case WDG_RESET_IWDG:
                return "Independent Watchdog (IWDG) Timeout Reset";
        case WDG_RESET_WWDG:
                return "Window Watchdog (WWDG) Timeout Reset";
        case WDG_RESET_SOFTWARE:
                return "Software Reset (NVIC_SystemReset)";
        case WDG_RESET_POR:
                return "Power-On Reset (POR/PDR)";
        case WDG_RESET_PIN:
                return "External Pin Reset (NRST Button)";
        case WDG_RESET_BOR:
                return "Brown-Out Reset (BOR)";
        default:
                return "Unknown Reset Source";
        }
}

void wdg_print_reset_reason(void)
{
        struct wdg_reset_info info = wdg_get_last_reset_reason();

        pr_info_with_tag("WDG", "\n");
        pr_info_with_tag("WDG", "╔════════════════════════════════╗\n");
        pr_info_with_tag("WDG", "║     RESET REASON REPORT       ║\n");
        pr_info_with_tag("WDG", "╠════════════════════════════════╣\n");
        pr_info_with_tag("WDG", "║ Source: %-22s ║\n",
                         info.description);
        pr_info_with_tag("WDG", "║ Code:   %-22d ║\n",
                         info.source);

        switch (info.source) {
        case WDG_RESET_IWDG:
                pr_info_with_tag("WDG",
                                 "║ ⚠️  System recovered from IWDG  ║\n");
                pr_info_with_tag("WDG",
                                 "║    timeout (possible deadlock) ║\n");
                break;
        case WDG_RESET_WWDG:
                pr_info_with_tag("WDG",
                                 "║ ⚠️  System recovered from WWDG  ║\n");
                pr_info_with_tag("WDG",
                                 "║    window violation            ║\n");
                break;
        case WDG_RESET_SOFTWARE:
                pr_info_with_tag("WDG",
                                 "║ ℹ️   Software-initiated reset   ║\n");
                break;
        default:
                pr_info_with_tag("WDG",
                                 "║ ✅   Normal startup             ║\n");
                break;
        }

        pr_info_with_tag("WDG", "╚════════════════════════════════╝\n");
        pr_info_with_tag("WDG", "\n");
}

void wdg_disable_for_ota(void)
{
        if (g_wdg_ctx.ota_mode) {
                pr_warn_with_tag("WDG",
                                 "Already in OTA disable mode\n");
                return;
        }

        g_wdg_ctx.ota_mode = true;
        g_wdg_ctx.ota_start_timestamp = get_timestamp_ms();
        g_wdg_ctx.state = WDG_STATE_DISABLED;

        pr_warn_with_tag("WDG",
                         "🔒 Watchdogs disabled for OTA update\n");
        pr_warn_with_tag("WDG",
                         "   Max allowed duration: %lu ms\n",
                         (unsigned long)g_wdg_ctx.ota_max_duration_ms);
        pr_warn_with_tag("WDG",
                         "   IWDG still active (hardware limit)\n");
}

int wdg_enable_after_ota(void)
{
        uint32_t duration;

        if (!g_wdg_ctx.ota_mode) {
                pr_info_with_tag("WDG",
                                 "Not in OTA mode, nothing to do\n");
                return 0;
        }

        duration = get_timestamp_ms() - g_wdg_ctx.ota_start_timestamp;
        g_wdg_ctx.stats.ota_disable_duration_ms += duration;

        g_wdg_ctx.ota_mode = false;
        g_wdg_ctx.state = WDG_STATE_RUNNING;

        pr_info_with_tag("WDG",
                         "🔓 Watchdogs re-enabled after OTA\n");
        pr_info_with_tag("WDG",
                         "   OTA duration: %lu ms\n",
                         (unsigned long)duration);

        return 0;
}

bool wdg_is_ota_mode(void)
{
        return g_wdg_ctx.ota_mode;
}

struct wdg_statistics wdg_get_statistics(void)
{
        return g_wdg_ctx.stats;
}

void wdg_reset_statistics(void)
{
        memset(&g_wdg_ctx.stats, 0, sizeof(g_wdg_ctx.stats));

        pr_info_with_tag("WDG", "Watchdog statistics reset\n");
}

bool wdg_check_health(void)
{
        uint32_t now;
        uint32_t elapsed;
        bool healthy = true;

        now = get_timestamp_ms();

        if (g_wdg_ctx.stats.last_feed_timestamp_ms > 0) {
                elapsed = now - g_wdg_ctx.stats.last_feed_timestamp_ms;

                if (elapsed > (WDG_IWDG_TIMEOUT_MS / 2)) {
                        pr_warn_with_tag("WDG",
                                         "Health warning: Last feed %lu ms ago\n",
                                         (unsigned long)elapsed);
                        healthy = false;
                }
        }

        if (!g_wdg_ctx.wwdg_started && !g_wdg_ctx.ota_mode) {
                pr_warn_with_tag("WDG",
                                 "Health warning: WWDG not started\n");
                healthy = false;
        }

        if (g_wdg_ctx.ota_mode) {
                uint32_t ota_elapsed;

                ota_elapsed = now - g_wdg_ctx.ota_start_timestamp;
                if (ota_elapsed > g_wdg_ctx.ota_max_duration_ms) {
                        pr_error_with_tag("WDG",
                                          "🚨 CRITICAL: OTA mode exceeded "
                                          "max duration (%lu ms)!\n",
                                          (unsigned long)ota_elapsed);
                        healthy = false;
                }
        }

        return healthy;
}

void wdg_set_iwdg_callback(wdg_callback_t callback)
{
        g_wdg_ctx.iwdg_callback = callback;
}

void wdg_set_wwdg_callback(wdg_callback_t callback)
{
        g_wdg_ctx.wwdg_callback = callback;
}
