/**
 * @file lv_conf.h
 * @brief LVGL v8.3 配置文件 (STM32F407优化版)
 *
 * 优化目标:
 *   - 内存占用: <64KB RAM + <256KB Flash
 *   - 刷新率: 30FPS (320x240分辨率)
 *   - 功能集: 基础控件 + 动画 + 图表
 *
 * 使用方法:
 *   1. 在工程设置中定义: LV_CONF_INCLUDE_SIMPLE
 *   2. 或在lvgl.h前包含此文件
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>
#include <errno.h>

/*
 * ⚠️ FreeRTOS头文件包含 (用于LV_TICK_CUSTOM)
 * 必须在LV_TICK_CUSTOM_SYS_TIME_EXPR之前定义
 */
#ifdef __cplusplus
extern "C" {
#endif
#include "FreeRTOS.h"
#include "task.h"
#ifdef __cplusplus
}
#endif

/* ==================== 基础配置 ==================== */

#define LV_COLOR_DEPTH          16       /* RGB565 (匹配LCD硬件) */
#define LV_COLOR_16_SWAP        0        /* 字节序 (STM32小端) */
#define LV_COLOR_SCREEN_TRANSP  0        /* 无透明屏幕 */
#define LV_DISP_DEF_REFR_PERIOD 33       /* 30FPS刷新 (1000/30≈33ms) */
#define LV_DPI_DEF              130      /* 2.8寸屏DPI估算 */
#define LV_MEMCPY_MEMSET_STD    1        /* 使用标准库函数 */
#define LV_USE_LARGE_COORD      0        /* 禁用大坐标 (节省内存) */

/* ==================== 内存管理 ==================== */

/*
 * 内存配置:
 *   - STM32F407IGT6: 192KB SRAM
 *   - LVGL使用静态内存池 (禁止malloc)
 *   - 分配大小: 48KB (足够320x240双缓冲)
 */
#define LV_MEM_CUSTOM           0        /* 使用LVGL内置内存管理 */
#define LV_MEM_SIZE             (48U * 1024U)  /* 48KB内存池 */
#define LV_MEM_ADR              0        /* 自动分配 */
#define LV_MEM_BUF_MAX_NUM      16       /* 最大缓冲区数量 */
#define LV_MEMCPY_NUM           1        /* memcpy调用次数阈值 */

/* ==================== HAL (硬件抽象层) ==================== */

#define LV_TICK_CUSTOM          1        /* 自定义系统时钟 */
#define LV_TICK_CUSTOM_INCLUDE "FreeRTOS.h"  /* 使用FreeRTOS tick */

/*
 * 自定义tick获取函数:
 *   static uint32_t custom_tick_get(void) {
 *       return xTaskGetTickCount();
 *   }
 */
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (xTaskGetTickCount())

#define LV_USE_OS               LV_OS_FREERTOS   /* 使用FreeRTOS */
#define LV_OS_FREERTOS_MINIMAL_STACK_SIZE 2048   /* 最小任务栈 */
#define LV_OS_FREERTOS_TASK_PRIO_LOW            5  /* 低优先级 */
#define LV_OS_FREERTOS_TASK_PRIO_MID           10 /* 中优先级 */
#define LV_OS_FREERTOS_TASK_PRIO_HIGH          15 /* 高优先级 */

/* ==================== 显示功能 ==================== */

#define LV_USE_PERF_MONITOR     0        /* 禁用性能监控 (节省CPU) */
#define LV_USE_REFR_DEBUG       0        /* 禁用刷新调试 */
#define LV_USE_MEM_MONITOR      0        /* 禁用内存监控 */

/* 双缓冲 (减少撕裂，但增加内存需求) */
#define LV_DISP_DEFAULT_REFR_DOUBLE_BUFFERED 1  /* 启用双缓冲 */
#define LV_DISP_DEFAULT_REFR_SIZE              240  /* 行缓冲大小 */

/* ==================== 控件启用/禁用 ==================== */

/* 核心控件 (必须) */
#define LV_USE_ARC              1        /* 圆弧 (用于仪表盘) */
#define LV_USE_BAR              1        /* 进度条 */
#define LV_USE_BTN              1        /* 按钮 */
#define LV_USE_BTNMATRIX        1        /* 按钮矩阵 */
#define LV_USE_CANVAS           1        /* 画布 (绘图) */
#define LV_USE_CHECKBOX         1        /* 复选框 */
#define LV_USE_DROPDOWN         1        /* 下拉列表 */
#define LV_USE_IMG              1        /* 图片 (二次元立绘!) */
#define LV_USE_LABEL            1        /* 文本标签 */
#define LV_USE_LINE             1        /* 线段 */
#define LV_USE_ROLLER           1        /* 滚轮选择器 */
#define LV_USE_SLIDER           1        /* 滑块 */
#define LV_USE_SWITCH           1        /* 开关 */
#define LV_USE_TABLE            1        /* 表格 */
#define LV_USE_TEXTAREA         1        /* 多行文本输入 */

/* 高级控件 (可选) */
#define LV_USE_CALENDAR         0        /* 日历 (不需要) */
#define LV_USE_CHART            1        /* 图表 (温湿度曲线) */
#define LV_USE_COLORWHEEL       0        /* 色轮 (不需要) */
#define CV_USE_IMGBTN           1        /* 图片按钮 */
#define LV_USE_KEYBOARD         0        /* 键盘 (无物理键盘) */
#define LV_USE_LED              1        /* LED指示灯 */
#define LV_USE_LIST             1        /* 列表 */
#define LV_USE_METER            1        /* 仪表盘 */
#define LV_USE_MSGBOX           1        /* 消息框 */
#define LV_USE_SPINBOX          1        /* 数字调节器 */
#define LV_USE_SPINNER          1        /* 加载动画 */
#define LV_USE_TABVIEW          1        /* 标签页 */
#define LV_USE_TILEVIEW         0        /* 平铺视图 */
#define LV_USE_WIN              1        /* 窗口 */

/* ==================== 主题与样式 ==================== */

#define LV_USE_THEME_DEFAULT    1        /* 默认主题 */
#define LV_THEME_DEFAULT_DARK   0        /* 浅色模式 (平板风格) */
#define LV_THEME_DEFAULT_GROW   1        /* 允许扩展 */
#define LV_FONT_DEFAULT         &lv_font_montserrat_14

/* 额外主题 */
#define LV_USE_THEME_BASIC      0        /* 基础主题 */
#define LV_USE_THEME_MONO       0        /* 单色主题 */

/* ==================== 字体支持 ==================== */

#define LV_FONT_CUSTOM_DECLARE  /* 自定义字体声明位置 */

/* 内置字体 (按需启用) */
#define LV_FONT_MONTSERRAT_8    1        /* 8pt */
#define LV_FONT_MONTSERRAT_10   1        /* 10pt */
#define LV_FONT_MONTSERRAT_12   1        /* 12pt */
#define LV_FONT_MONTSERRAT_14   1        /* 14pt (默认) */
#define LV_FONT_MONTSERRAT_16   1        /* 16pt */
#define LV_FONT_MONTSERRAT_18   1        /* 18pt */
#define LV_FONT_MONTSERRAT_20   1        /* 20pt */
#define LV_FONT_MONTSERRAT_22   0        /* 22pt (禁用省Flash) */
#define LV_FONT_MONTSERRAT_24   0        /* 24pt */
#define LV_FONT_MONTSERRAT_26   0        /* 26pt */
#define LV_FONT_MONTSERRAT_28   0        /* 28pt */
#define LV_FONT_MONTSERRAT_30   0        /* 30pt */
#define LV_FONT_MONTSERRAT_32   0        /* 32pt */
#define LV_FONT_MONTSERRAT_34   0        /* 34pt */
#define LV_FONT_MONTSERRAT_36   0        /* 36pt */
#define LV_FONT_MONTSERRAT_38   0        /* 38pt */
#define LV_FONT_MONTSERRAT_40   0        /* 40pt */
#define LV_FONT_MONTSERRAT_42   0        /* 42pt */
#define LV_FONT_MONTSERRAT_44   0        /* 44pt */
#define LV_FONT_MONTSERRAT_46   0        /* 46pt */
#define LV_FONT_MONTSERRAT_48   0        /* 48pt */

/* 中文字体 (可选，需要c文件) */
#define LV_FONT_SIMSUN_16_CJK   0        /* 宋体16px中文 */

/*
 * ⚠️ 重要: 禁用Dejavu字体!
 *
 * Dejavu字体使用C99 designated initializer语法
 * 部分ARM GCC编译器版本不支持，会导致1000+编译错误:
 *   error: field name not in record or union initializer
 *   warning: excess elements in scalar initializer
 *
 * 解决方案: 仅使用Montserrat字体 (完全兼容)
 */
#define LV_FONT_DEJAVU_8        0        /* 禁用! C99语法不兼容 */
#define LV_FONT_DEJAVU_10       0
#define LV_FONT_DEJAVU_12       0
#define LV_FONT_DEJAVU_14       0
#define LV_FONT_DEJAVU_16       0
#define LV_FONT_DEJAVU_18       0
#define LV_FONT_DEJAVU_20       0
#define LV_FONT_DEJAVU_22       0
#define LV_FONT_DEJAVU_24       0
#define LV_FONT_DEJAVU_26       0
#define LV_FONT_DEJAVU_28       0
#define LV_FONT_DEJAVU_30       0
#define LV_FONT_DEJAVU_32       0
#define LV_FONT_DEJAVU_34       0
#define LV_FONT_DEJAVU_36       0
#define LV_FONT_DEJAVU_38       0
#define LV_FONT_DEJAVU_40       0

/* 禁用Dejavu特殊字体 (波斯语/希伯来语等) */
#define LV_FONT_DEJAVU_16_PERSIAN_HEBREW  0  /* ← 主要错误来源! */
#define LV_FONT_DEJAVU_16_ARABIC          0
#define LV_FONT_DEJAVU_16_CYRILLIC        0

/* 位图字体格式 */
#define LV_FONT_FMT_TXT_LARGE   0        /* 禁用大字体支持 */
#define LV_FONT_FMT_TXT_COLOR   1        /* 支持字体颜色 */

/* ==================== 动画效果 ==================== */

#define LV_USE_ANIMATION        1        /* 启用动画 */
#define ANIM_DEF_SPEED          200      /* 默认动画时长 (ms) */
#define LV_ANIM_PLAYTIME_DEF    200

/* ==================== 图形绘制 ==================== */

#define LV_DRAW_COMPLEX         1        /* 复杂图形 (圆角、阴影等) */
#define LV_SHADOW_CACHE_SIZE    0        /* 禁用阴影缓存 */
#define LV_IMG_CACHE_DEF_SIZE   0        /* 禁用图片缓存 */
#define LV_GRADIENT_MAX_STOPS   2        /* 渐变色最大节点数 */
#define LV_DGRADIENT_CACHE_CNT  0        /* 禁用渐变缓存 */

/* ==================== 文件系统 ==================== */

#define LV_USE_FS_STDIO         0        /* 不使用文件系统 */
#define LV_USE_FS_POSIX         0
#define LV_USE_FS_FATFS         0

/* ==================== 图像支持 ==================== */

#define LV_IMG_CF_INDEXED       1        /* 索引颜色 */
#define LV_IMG_CF_ALPHA         1        /* Alpha通道 */

/* PNG/JPEG/BMP解码 (需要额外库) */
#define LV_USE_PNG              0        /* 禁用PNG (节省Flash) */
#define LV_USE_BMP              1        /* 启用BMP (简单格式) */

/* ==================== 调试功能 ==================== */

#define LV_USE_ASSERT_NULL          1        /* NULL检查 */
#define LV_USE_ASSERT_MALLOC        1        /* 内存检查 */
#define LV_USE_ASSERT_STYLE         0        /* 样式检查 (调试时开启) */
#define LV_USE_ASSERT_MEM_INTEGRITY 0        /* 内存完整性 */
#define LV_USE_ASSERT_OBJ           0        /* 对象有效性 */

/* 日志输出 (重定向到我们的日志系统) */
#define LV_LOG_PRINTF   1        /* 使用printf输出 */
#define LV_LOG_LEVEL    LV_LOG_LEVEL_WARN  /* 输出警告以上级别 */

#endif /* LV_CONF_H */
