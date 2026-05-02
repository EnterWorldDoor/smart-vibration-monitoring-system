/**
 * @file lcd.h
 * @brief ATK-DMF407 LCD驱动接口 (ILI9341, FSMC 16-bit)
 *
 * 硬件平台:
 *   MCU: STM32F407IGT6
 *   接口: FSMC Bank4, 16-bit数据总线
 *   分辨率: 320x240 (2.8寸TFT)
 *   驱动: ILI9341 / ST7789
 *
 * 功能特性:
 *   - FSMC高速并行通信 (16位颜色，565格式)
 *   - 基础绘图API (点/线/矩形/填充)
 *   - 背光PWM控制
 *   - 方向旋转支持 (0°/90°/180°/270°)
 */

#ifndef __BSP_LCD_H
#define __BSP_LCD_H

#include <stdint.h>
#include <stdbool.h>
#include <errno.h>

/* LVGL类型依赖 (flush回调需要 lv_disp_drv_t, lv_area_t, lv_color_t) */
#include "lvgl.h"

/* ==================== LCD硬件参数 ==================== */

#define LCD_WIDTH               320     /* 水平像素 */
#define LCD_HEIGHT              240     /* 垂直像素 */
#define LCD_PIXEL_FORMAT        LV_COLOR_FORMAT_RGB565  /* RGB565格式 */
#define LCD_BPP                 16      /* 每像素位数 */

/* FSMC地址映射 (Bank4, A10作为地址线) */
#define LCD_FSMC_BASE           0x6C000000
#define LCD_FSMC_CMD_ADDR       (*(volatile uint16_t *)(LCD_FSMC_BASE | (1 << 10)))
#define LCD_FSMC_DATA_ADDR      (*(volatile uint16_t *)LCD_FSMC_BASE)

/* ==================== 颜色定义 (RGB565) ==================== */

#define LCD_COLOR_WHITE         0xFFFF
#define LCD_COLOR_BLACK         0x0000
#define LCD_COLOR_RED           0xF800
#define LCD_COLOR_GREEN         0x07E0
#define LCD_COLOR_BLUE          0x001F
#define LCD_COLOR_CYAN          0x07FF
#define LCD_COLOR_MAGENTA       0xF81F
#define LCD_COLOR_YELLOW        0xFFE0
#define LCD_COLOR_GRAY          0x7BEF

/* 浅色系 (平板APP风格) */
#define LCD_COLOR_BG_LIGHT      0xEFEF   /* 浅灰色背景 */
#define LCD_COLOR_CARD_BG       0xFFFF   /* 卡片背景白 */
#define LCD_COLOR_ACCENT_BLUE   0x2D6F   /* 蓝绿色强调 */
#define LCD_COLOR_ACCENT_ORANGE 0xFC00   /* 橙红色强调 */
#define LCD_COLOR_ACCENT_CYAN   0x07FF   /* 青蓝色 */
#define LCD_COLOR_ACCENT_PURPLE 0x801F   /* 紫色 */
#define LCD_COLOR_TEXT_DARK     0x0000   /* 深色文字 */
#define LCD_COLOR_TEXT_GRAY     0x8410   /* 灰色文字 */

/* ==================== 显示方向 ==================== */

enum lcd_orientation {
        LCD_ORIENTATION_PORTRAIT = 0,       /* 竖屏 (默认) */
        LCD_ORIENTATION_LANDSCAPE = 1,      /* 横屏90° */
        LCD_ORIENTATION_PORTRAIT_INV = 2,   /* 竖屏180° */
        LCD_ORIENTATION_LANDSCAPE_INV = 3   /* 横屏270° */
};

/* ==================== API函数 ==================== */

/**
 * bsp_lcd_init - 初始化LCD显示屏
 *
 * 初始化流程:
 *   1. 配置FSMC GPIO (已在MX_FSMC_Init完成)
 *   2. 复位LCD模块
 *   3. 发送ILI9341初始化命令序列
 *   4. 设置显示方向和颜色格式
 *   5. 开启背光
 *
 * Return: 0 成功, 负值错误码
 */
int bsp_lcd_init(void);

/**
 * bsp_lcd_deinit - 关闭LCD显示
 *
 * Return: 0 成功
 */
int bsp_lcd_deinit(void);

/**
 * bsp_lcd_set_orientation - 设置显示方向
 * @orient: 显示方向枚举值
 *
 * Return: 0 成功, -EINVAL 参数错误
 */
int bsp_lcd_set_orientation(enum lcd_orientation orient);

/**
 * bsp_lcd_get_width - 获取当前显示宽度
 *
 * Return: 像素宽度 (考虑方向后)
 */
uint16_t bsp_lcd_get_width(void);

/**
 * bsp_lcd_get_height - 获取当前显示高度
 *
 * Return: 像素高度 (考虑方向后)
 */
uint16_t bsp_lcd_get_height(void);

/**
 * bsp_lcd_fill_screen - 填充整个屏幕
 * @color: RGB565颜色值
 */
void bsp_lcd_fill_screen(uint16_t color);

/**
 * bsp_lcd_draw_pixel - 绘制单个像素点
 * @x: X坐标
 * @y: Y坐标
 * @color: RGB565颜色值
 */
void bsp_lcd_draw_pixel(uint16_t x, uint16_t y, uint16_t color);

/**
 * bsp_lcd_fill_rect - 填充矩形区域
 * @x: 左上角X坐标
 * @y: 左上角Y坐标
 * @w: 宽度
 * @h: 高度
 * @color: RGB565颜色值
 */
void bsp_lcd_fill_rect(uint16_t x, uint16_t y,
                       uint16_t w, uint16_t h,
                       uint16_t color);

/**
 * bsp_lcd_set_window - 设置绘图窗口 (加速批量写入)
 * @x: 起始X坐标
 * @y: 起始Y坐标
 * @w: 窗口宽度
 * @h: 窗口高度
 *
 * 设置后可通过直接写LCD_FSMC_DATA_ADDR快速填充
 */
void bsp_lcd_set_window(uint16_t x, uint16_t y,
                        uint16_t w, uint16_t h);

/**
 * bsp_lcd_write_data_batch - 批量写入像素数据 (DMA优化)
 * @data: 像素数据指针 (RGB565格式)
 * @len: 像素数量
 *
 * 用于LVGL flush回调，高效刷新显存
 */
void bsp_lcd_write_data_batch(const uint16_t *data, uint32_t len);

/**
 * bsp_lcd_set_backlight - 设置背光亮度
 * @brightness: 亮度百分比 (0~100)
 *
 * 使用TIMx PWM控制背光LED
 */
void bsp_lcd_set_backlight(uint8_t brightness);

/**
 * bsp_lcd_on - 开启显示 (唤醒)
 */
void bsp_lcd_on(void);

/**
 * bsp_lcd_off - 关闭显示 (休眠省电)
 */
void bsp_lcd_off(void);

/**
 * lcd_flush_cb - LVGL显示刷新回调 (flush callback)
 * @disp_drv: 显示驱动指针 (LVGL传入)
 *
 * 由LVGL定时器处理器(lv_timer_handler)调用:
 *   1. LVGL标记脏区域(dirty area)
 *   2. 调用此回调将帧缓冲区写入LCD显存
 *   3. 调用 lv_disp_flush_ready() 通知LVGL刷新完成
 *
 * 实现位置: App/gui/gui_app.c
 */
void lcd_flush_cb(lv_disp_drv_t *disp_drv, const lv_area_t *area,
                   lv_color_t *color_p);

#endif /* __BSP_LCD_H */
