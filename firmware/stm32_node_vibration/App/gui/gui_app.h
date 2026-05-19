/**
 * @file gui_app.h
 * @brief 平板APP风格主界面 (Enterprise V2.0)
 *
 * 界面设计:
 *   - 主屏幕: 5个功能卡片 (2行3列) + 右下角二次元立绘
 *   - 二级界面: 5个模块详细控制页面
 *   - 风格: iOS/Android平板APP风格
 *
 * 功能模块:
 *   1. 直流有刷电机控制 (Motor Control)
 *   2. NTC温度采集 (NTC Sensor)
 *   3. 温湿度显示 (Temp & Humidity)
 *   4. RS485通信 (RS485 Comm)
 *   5. CAN总线监控 (CAN Bus)
 *
 * 二次元元素:
 *   - 御姐立绘: 80x120像素, 右下角常驻
 *   - 头像: 32x32像素, 二级界面标题栏装饰
 *   - 动效: 点击卡片时眨眼/微笑动画
 */

#ifndef __GUI_APP_H
#define __GUI_APP_H

#include "../../lvgl.h"

/* ==================== 屏幕尺寸常量 ==================== */

#define GUI_SCREEN_WIDTH         320
#define GUI_SCREEN_HEIGHT        240
#define GUI_CARD_MARGIN          10      /* 卡片间距 */
#define GUI_CARD_WIDTH           90      /* 卡片宽度 */
#define GUI_CARD_HEIGHT          80      /* 卡片高度 */

/* 二次元立绘尺寸 */
#define ANIME_CHARACTER_WIDTH    80
#define ANIME_CHARACTER_HEIGHT   120
#define ANIME_AVATAR_SIZE        32

/* ==================== 颜色主题 (平板APP风格) ==================== */

/*
 * 配色方案: 粉白系 + 高饱和度强调色
 * 背景: 粉白 (#FEF0F5, RGB 254/240/245)
 * 卡片: 白色 (#FFFFFF) + 轻阴影
 * 文字: 深灰 (#212121)
 */
/* 使用lv_color_make确保RGB565转换正确 (避免lv_color_hex的位操作不确定性) */
#define THEME_BG_COLOR           lv_color_make(254, 240, 245)  /* #FEF0F5 粉白 */
#define THEME_CARD_COLOR         lv_color_make(255, 255, 255)  /* #FFFFFF 白色 */
#define THEME_TEXT_PRIMARY       lv_color_make( 33,  33,  33)  /* #212121 深灰 */
#define THEME_TEXT_SECONDARY     lv_color_make(117, 117, 117)  /* #757575 中灰 */

/* 模块配色 (每个模块独特颜色) */
#define COLOR_MOTOR              lv_color_hex(0x26A69A)  /* 蓝绿色 */
#define COLOR_NTC                lv_color_hex(0xFF7043)  /* 橙红色 */
#define COLOR_TEMP_HUMIDITY      lv_color_hex(0x29B6F6)  /* 青蓝色 */
#define COLOR_RS485              lv_color_hex(0xAB47BC)  /* 紫色 */
#define COLOR_CAN                lv_color_hex(0x1E88E5)  /* 深蓝色 */

/* ==================== 模块ID枚举 ==================== */

enum gui_module_id {
        MODULE_MOTOR = 0,        /* 直流有刷电机 */
        MODULE_NTC,              /* NTC温度采集 */
        MODULE_TEMP_HUMIDITY,    /* 温湿度传感器 */
        MODULE_RS485,            /* RS485通信 */
        MODULE_CAN,              /* CAN总线 */
        MODULE_COUNT             /* 模块总数 (5) */
};

/* ==================== API函数 ==================== */

/**
 * gui_app_init - 初始化GUI应用
 *
 * 创建流程:
 *   1. 初始化显示驱动和输入设备
 *   2. 创建主屏幕 (5个卡片 + 立绘)
 *   3. 创建5个二级屏幕
 *   4. 注册事件回调
 *
 * Return: 0 成功, 负值错误码
 */
int gui_app_init(void);

/**
 * gui_app_deinit - 销毁GUI资源
 *
 * Return: 0 成功
 */
int gui_app_deinit(void);

/**
 * gui_app_get_active_screen - 获取当前活动屏幕
 *
 * Return: 屏幕对象指针, NULL表示未初始化
 */
lv_obj_t *gui_app_get_active_screen(void);

/**
 * gui_app_show_module_screen - 显示指定模块的二级界面
 * @module_id: 模块ID (enum gui_module_id)
 */
void gui_app_show_module_screen(enum gui_module_id module_id);

/**
 * gui_app_show_main_screen - 返回主屏幕
 */
void gui_app_show_main_screen(void);

/**
 * gui_app_update_sensor_data - 更新温湿度传感器数据 (线程安全)
 * @temperature: 温度值 (°C, DS18B20)
 * @humidity: 湿度值 (%RH, 模拟)
 */
void gui_app_update_sensor_data(float temperature, float humidity);

/**
 * gui_app_update_ntc_data - 更新NTC传感器数据 (线程安全)
 * @temperature: NTC温度值 (°C)
 */
void gui_app_update_ntc_data(float temperature);

/**
 * gui_app_process_updates - 处理跨任务数据更新
 *
 * 必须在 lvgl_gui 任务上下文中周期调用 (在 lv_timer_handler() 之后)。
 * 消费 app_enterprise 任务写入的共享数据, 安全更新LVGL控件。
 */
void gui_app_process_updates(void);

/**
 * gui_app_update_motor_status - 更新电机状态显示
 * @speed: 当前转速 (RPM)
 * @is_running: 是否运行中
 * @direction: 方向 (1=正转, -1=反转)
 */
void gui_app_update_motor_status(uint32_t speed,
                                  bool is_running,
                                  int8_t direction,
                                  bool pid_active,
                                  int32_t target_rpm);

/*
 * 电机控制命令共享 (GUI任务 → app_enterprise任务)
 * 由gui_app.c定义, app_main.c通过extern访问。
 */
struct motor_cmd {
        bool start_request;
        bool stop_request;
        bool duty_changed;
        bool dir_changed;
        int8_t slider_percent;
        int8_t direction;
        bool pid_toggle_request;
        bool pid_active;
        int32_t target_rpm;
};
extern volatile struct motor_cmd motor_cmd_shared;

/**
 * gui_app_consume_motor_cmds - 消费GUI电机控制命令
 *
 * 必须在 app_enterprise 任务上下文中周期调用。
 * 读取GUI事件回调设置的共享命令, 安全调用 bsp_motor_* 函数。
 */
void gui_app_consume_motor_cmds(void);

/**
 * gui_app_touch_input_init - 初始化触摸输入设备
 *
 * 创建LVGL输入设备驱动并注册触摸扫描回调。
 * 必须在 gui_app_init() 之后、lvgl_gui 任务启动之前调用。
 *
 * Return: 0 成功, 负值错误码
 */
int gui_app_touch_input_init(void);

#endif /* __GUI_APP_H */
