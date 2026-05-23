/**
 * @file gui_app.c
 * @brief 平板APP风格GUI实现 (二次元御姐主题)
 *
 * 界面架构:
 *   ┌─────────────────────────────────────┐
 *   │  [电机卡片] [NTC卡片] [温湿度卡片] │
 *   │                                     │
 *   │  [RS232]                  [御姐立绘]│ ← 右下角80x120
 *   │  [时间戳]                           │
 *   │  [CAN]                              │
 *   └─────────────────────────────────────┘
 *
 * 二级界面结构:
 *   ┌─────────────────────────────────────┐
 *   │ [←返回]  模块名称        [头像]     │ ← 标题栏
 *   ├─────────────────────────────────────┤
 *   │                                     │
 *   │          内容区域                    │
 *   │          (模块特定控件)              │
 *   │                                     │
 *   └─────────────────────────────────────┘
 *
 * 二次元角色设定:
 *   - 丰川祥子 (MyGO!!!!!) - 酷帅吉他手
 *   - 椎名立希 (BanG Dream!) - 高冷键盘手
 *   - 凛 (Blue Archive) - 元气步枪手
 *   - 每个界面使用不同角色，增加趣味性
 */

#include "gui_app.h"
#include "anime_characters.h"
#include "../../bsp/motor/bsp_motor.h"
#include "lcd.h"
#include "../../bsp/touch/bsp_touch.h"
#include "system_log/system_log.h"
#include <stdio.h>
#include <string.h>

/* CJK font extern (compiled from lvgl/font/lv_font_simsun_16_cjk.c) */
extern const lv_font_t lv_font_simsun_16_cjk;

/* ==================== 模块内部状态 ==================== */

static struct {
        lv_obj_t *main_screen;            /* 主屏幕 */
        lv_obj_t *module_screens[MODULE_COUNT]; /* 5个二级屏幕 */
        lv_obj_t *active_screen;          /* 当前活动屏幕 */

        /* 主屏幕控件 */
        lv_obj_t *cards[MODULE_COUNT];    /* 5个功能卡片 */
        lv_obj_t *anime_character;        /* 御姐立绘对象 */
        lv_obj_t *anime_avatar;           /* 左下角小头像 */

        /* 温湿度界面控件 */
        lv_obj_t *label_temp;
        lv_obj_t *label_humidity;
        lv_obj_t *chart_temp_humidity;

        /* NTC界面控件 */
        lv_obj_t *label_ntc_temp;

        /* 电机界面控件 */
        lv_obj_t *slider_motor_speed;
        lv_obj_t *label_speed_value;
        lv_obj_t *btn_start_stop;
        lv_obj_t *btn_direction;
        lv_obj_t *btn_pid_toggle;     /* PID开关按钮 */
        lv_obj_t *lbl_slider_hint;    /* 滑块提示标签 */

        bool initialized;
} gui = {0};

/*
 * 跨任务传感器数据共享 (解决LVGL单线程访问冲突)
 *
 * 问题: app_enterprise任务调用gui_app_update_sensor_data()修改
 *       LVGL控件(label_temp/label_humidity), 但LVGL独占lvgl_gui任务。
 *       LV_USE_OS=0时, LVGL非线程安全。
 *
 * 方案: 使用volatile共享结构体传递数据,
 *       lvgl_gui任务在调用lv_timer_handler()后消费数据更新控件。
 */
static struct {
        float temperature;
        float humidity;
        volatile bool pending;
} gui_sensor_shared = {0};

/* NTC温度数据共享 (线程安全) */
static struct {
        float temperature;
        volatile bool pending;
} gui_ntc_shared = {0};

/*
 * 电机控制命令共享 (GUI任务 → app_enterprise任务)
 *
 * GUI事件回调 (lvgl_gui任务) 设置命令标志和参数,
 * app_enterprise任务在 gui_app_process_updates() 中消费命令,
 * 调用 bsp_motor_* 函数执行实际操作。
 */
volatile struct motor_cmd motor_cmd_shared = {0, 0, 0, 0, 0, 1, 0, 0, 0};

/* ==================== 前向声明 (Forward Declarations) ==================== */

/*
 * 事件处理函数 - 声明在使用之前
 * LVGL v8要求回调函数在使用前必须可见
 */
static void card_click_event_handler(lv_event_t *e);
static void btn_back_event_handler(lv_event_t *e);
static lv_obj_t *create_title_bar(lv_obj_t *screen, enum gui_module_id module_id,
                                  const char *title);
static void motor_slider_event_cb(lv_event_t *e);
static void motor_start_stop_event_cb(lv_event_t *e);
static void motor_direction_event_cb(lv_event_t *e);
static void motor_pid_toggle_event_cb(lv_event_t *e);

/* ==================== 样式定义 ==================== */

/*
 * 全局样式对象 (静态分配，避免动态内存)
 */
static lv_style_t style_card;
static lv_style_t style_card_pressed;
static lv_style_t style_title;
static lv_style_t style_title_cjk;       /* 中文标题样式 */
static lv_style_t style_text_large;
static lv_style_t style_btn_primary;
static lv_style_t style_btn_secondary;
static lv_style_t style_timestamp;       /* 时间戳样式 */

/**
 * init_styles - 初始化所有UI样式
 *
 * 定义平板APP风格的视觉语言:
 *   - 圆角矩形卡片 (8px圆角)
 *   - 轻微阴影效果
 *   - 清晰的层次结构
 */
static void init_styles(void)
{
        /*
         * 卡片默认样式
         * 白色背景 + 圆角 + 边框
         */
        lv_style_init(&style_card);
        lv_style_set_radius(&style_card, 8);
        lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
        lv_style_set_bg_color(&style_card, THEME_CARD_COLOR);
        lv_style_set_border_width(&style_card, 1);
        lv_style_set_border_color(&style_card,
                                  lv_color_hex(0xE0E0E0));
        lv_style_set_pad_all(&style_card, 10);

        /*
         * 卡片按下样式 (缩放动画)
         */
        lv_style_init(&style_card_pressed);
        lv_style_set_bg_opa(&style_card_pressed, LV_OPA_COVER);
        lv_style_set_bg_color(&style_card_pressed, THEME_CARD_COLOR);
        lv_style_set_border_width(&style_card_pressed, 1);
        lv_style_set_border_color(&style_card_pressed, lv_color_hex(0xE0E0E0));
        lv_style_set_pad_all(&style_card_pressed, 10);
        lv_style_set_transform_zoom(&style_card_pressed, 950);  /* 缩小到95% */

        /*
         * 标题文字样式 (粗体, 16px Montserrat - Latin)
         */
        lv_style_init(&style_title);
        lv_style_set_text_font(&style_title,
                               &lv_font_montserrat_16);
        lv_style_set_text_color(&style_title,
                                THEME_TEXT_PRIMARY);

        /*
         * 中文标题文字样式 (16px SimSun CJK)
         */
        lv_style_init(&style_title_cjk);
        lv_style_set_text_font(&style_title_cjk,
                               &lv_font_simsun_16_cjk);
        lv_style_set_text_color(&style_title_cjk,
                                THEME_TEXT_PRIMARY);

        /*
         * 时间戳样式 (12px Montserrat, 灰色)
         */
        lv_style_init(&style_timestamp);
        lv_style_set_text_font(&style_timestamp,
                               &lv_font_montserrat_12);
        lv_style_set_text_color(&style_timestamp,
                                THEME_TEXT_SECONDARY);

        /*
         * 大号数字样式 (用于传感器数值显示)
         */
        lv_style_init(&style_text_large);
        lv_style_set_text_font(&style_text_large,
                               &lv_font_montserrat_20);  /* v8: 22pt未启用,使用20pt */
        lv_style_set_text_color(&style_text_large,
                                THEME_TEXT_PRIMARY);

        /*
         * 主要按钮样式 (实心填充)
         */
        lv_style_init(&style_btn_primary);
        lv_style_set_radius(&style_btn_primary, 20);
        lv_style_set_bg_opa(&style_btn_primary, LV_OPA_COVER);
        lv_style_set_bg_color(&style_btn_primary,
                               COLOR_MOTOR);  /* 默认蓝绿色 */
        lv_style_set_text_color(&style_btn_primary,
                                 lv_color_white());

        /*
         * 次要按钮样式 (描边)
         */
        lv_style_init(&style_btn_secondary);
        lv_style_set_radius(&style_btn_secondary, 20);
        lv_style_set_bg_opa(&style_btn_secondary, LV_OPA_TRANSP);
        lv_style_set_border_width(&style_btn_secondary, 2);
        lv_style_set_border_color(&style_btn_secondary,
                                   COLOR_MOTOR);
        lv_style_set_text_color(&style_btn_secondary,
                                 COLOR_MOTOR);
}

/* ==================== 卡片创建辅助函数 ==================== */

/**
 * create_module_card - 创建功能卡片
 * @parent: 父容器
 * @module_id: 模块ID
 * @title: 卡片标题文字
 * @icon_symbol: LV_SYMBOL_* 图标符号
 * @accent_color: 强调色
 *
 * Return: 卡片对象指针
 */
static lv_obj_t *create_module_card(lv_obj_t *parent,
                                    enum gui_module_id module_id,
                                    const char *title,
                                    const char *icon_symbol,
                                    lv_color_t accent_color)
{
        /*
         * 创建卡片容器 (可点击按钮)
         */
        lv_obj_t *card = lv_btn_create(parent);
        lv_obj_set_size(card, GUI_CARD_WIDTH, GUI_CARD_HEIGHT);
        lv_obj_add_style(card, &style_card, 0);
        lv_obj_add_style(card, &style_card_pressed, LV_STATE_PRESSED);

        /* 设置点击事件回调 */
        lv_obj_set_user_data(card, (void *)(uintptr_t)module_id);
        lv_obj_add_event_cb(card, card_click_event_handler,
                            LV_EVENT_CLICKED, NULL);

        /*
         * 布局: 垂直排列 (图标 + 文字)
         */
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_EVENLY,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        /*
         * 图标标签 (使用LVGL内置符号或Emoji)
         */
        lv_obj_t *icon_label = lv_label_create(card);
        if (icon_symbol) {
                lv_label_set_text(icon_label, icon_symbol);
        } else {
                lv_label_set_text(icon_label, LV_SYMBOL_SETTINGS);
        }
        lv_label_set_recolor(icon_label, true);  /* 允许颜色标记 */
        lv_obj_set_style_text_color(icon_label, accent_color, 0);
        lv_obj_set_style_text_font(icon_label,
                                   &lv_font_montserrat_20, 0);  /* v8: 最大启用字体20pt */

        /*
         * 标题文字 (使用中文字体)
         */
        lv_obj_t *title_label = lv_label_create(card);
        lv_label_set_text(title_label, title);
        lv_obj_add_style(title_label, &style_title_cjk, 0);
        lv_label_set_long_mode(title_label,
                               LV_LABEL_LONG_DOT);

        return card;
}

/**
 * card_click_event_handler - 卡片点击事件处理
 * @e: 事件对象
 *
 * 点击后切换到对应的二级界面，并播放过渡动画
 */
static void card_click_event_handler(lv_event_t *e)
{
        lv_obj_t *card = lv_event_get_target(e);
        uintptr_t id = (uintptr_t)lv_obj_get_user_data(card);

        pr_debug_with_tag("GUI", "Card clicked: module_id=%lu\n",
                          (unsigned long)id);

        if (id < MODULE_COUNT) {
                /*
                 * 仅电机卡片允许进入二级界面, 其余模块暂时禁用。
                 * LCD屏幕质量差, 其他界面存在显示异常, 先集中调试电机控制。
                 */
                if (id == MODULE_MOTOR)
                        gui_app_show_module_screen((enum gui_module_id)id);
#if 0
                gui_app_show_module_screen((enum gui_module_id)id);
#endif
        }
}

/**
 * btn_back_event_handler - 返回按钮事件处理
 * @e: 事件对象
 *
 * 点击返回按钮后，切换回主屏幕
 */
static void btn_back_event_handler(lv_event_t *e)
{
        lv_obj_t *btn = lv_event_get_target(e);

        pr_info_with_tag("GUI", "Back button pressed — returning to main screen\n");

        /* 视觉反馈: 按钮短暂变色 */
        lv_obj_set_style_bg_color(btn, THEME_TEXT_SECONDARY, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_30, 0);

        /* 切换回主屏幕 */
        gui_app_show_main_screen();
}

/**
 * create_title_bar - 创建统一的二级界面标题栏
 * @screen: 父屏幕
 * @module_id: 模块ID (用于选择对应角色头像)
 * @title: 标题文字
 *
 * 标题栏布局: [←返回] ——— 标题 ——— [32x32头像]
 * 背景白色, 底部分隔线
 *
 * Return: 内容区域对象 (标题栏下方的可用空间)
 */
static lv_obj_t *create_title_bar(lv_obj_t *screen, enum gui_module_id module_id,
                                  const char *title)
{
        /* 标题栏容器 */
        lv_obj_t *header = lv_obj_create(screen);
        lv_obj_set_size(header, GUI_SCREEN_WIDTH, 40);
        lv_obj_set_pos(header, 0, 0);
        lv_obj_set_style_bg_color(header, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
        lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(header, 1, 0);
        lv_obj_set_style_border_color(header, lv_color_hex(0xE0E0E0), 0);
        lv_obj_clear_flag(header, LV_OBJ_FLAG_CLICKABLE);

        /* 返回按钮 (加大尺寸+可见边框确保触摸命中) */
        lv_obj_t *btn_back = lv_btn_create(header);
        lv_obj_set_size(btn_back, 72, 34);
        lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 5, 0);
        lv_obj_set_style_bg_color(btn_back, lv_color_hex(0xEEEEEE), 0);
        lv_obj_set_style_border_width(btn_back, 1, 0);
        lv_obj_set_style_border_color(btn_back, lv_color_hex(0xCCCCCC), 0);
        lv_obj_add_event_cb(btn_back, btn_back_event_handler,
                            LV_EVENT_CLICKED, NULL);
        lv_obj_t *lbl_back = lv_label_create(btn_back);
        lv_label_set_text(lbl_back, LV_SYMBOL_LEFT "返回");
        lv_obj_set_style_text_font(lbl_back, &lv_font_simsun_16_cjk, 0);
        lv_obj_center(lbl_back);

        /* 标题文字 */
        lv_obj_t *lbl_title = lv_label_create(header);
        lv_label_set_text(lbl_title, title);
        lv_obj_add_style(lbl_title, &style_title_cjk, 0);
        lv_obj_align(lbl_title, LV_ALIGN_CENTER, 0, 0);

        /* 角色头像 (32x32) */
        const void *avatar = anime_get_character_img(module_id, false);
        if (avatar) {
                lv_obj_t *img_avatar = lv_img_create(header);
                lv_img_set_src(img_avatar, avatar);
                lv_obj_set_size(img_avatar, ANIME_AVATAR_SIZE, ANIME_AVATAR_SIZE);
                lv_obj_align(img_avatar, LV_ALIGN_RIGHT_MID, -10, 0);
        }

        /* 内容区域 (标题栏下方) */
        lv_obj_t *content = lv_obj_create(screen);
        lv_obj_set_size(content, GUI_SCREEN_WIDTH, GUI_SCREEN_HEIGHT - 40);
        lv_obj_set_pos(content, 0, 40);
        lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(content, 0, 0);
        lv_obj_clear_flag(content, LV_OBJ_FLAG_CLICKABLE);

        return content;
}

/* ==================== 二次元立绘创建 ==================== */

/**
 * create_anime_character - 创建二次元御姐立绘
 * @parent: 父容器 (通常是主屏幕)
 *
 * 立绘规格:
 *   - 尺寸: 80x120像素
 *   - 位置: 屏幕右下角 (x=240, y=120)
 *   - 风格: 日系简约线条
 *   - 功能: 纯装饰性，不影响交互
 *
 * 实现方式:
 *   使用lv_img控件加载C数组格式的位图图片
 *   如果没有真实图片文件，显示带边框的占位符
 *
 * Return: 图片对象指针
 */
static lv_obj_t *create_anime_character(lv_obj_t *parent)
{
        lv_obj_t *img = lv_img_create(parent);

        lv_obj_set_size(img, ANIME_CHARACTER_WIDTH, ANIME_CHARACTER_HEIGHT);
        lv_obj_align(img, LV_ALIGN_BOTTOM_RIGHT, -5, -5);

        lv_img_set_src(img, &anime_sakoji_main);

        pr_info_with_tag("GUI", "Anime character loaded: 80x120 Sakiko photo\n");

        return img;
}

/* ==================== 主屏幕创建 ==================== */

/**
 * create_main_screen - 创建主屏幕界面 (手动布局)
 *
 * 布局方案 (320x240横屏, 粉白背景):
 *   ┌──────────────────────────────────────────┐
 *   │  ┌─────┐  ┌─────┐  ┌─────┐              │
 *   │  │电机 │  │NTC │  │温湿度│              │ ← y=5
 *   │  └─────┘  └─────┘  └─────┘              │
 *   │                                          │
 *   │  ┌─────┐                       ┌──────┐ │
 *   │  │RS232│                       │御姐  │ │ ← y=88
 *   │  └─────┘                       │立绘  │ │
 *   │  [时间戳]                      └──────┘ │
 *   │  ┌─────┐                                │
 *   │  │CAN  │                                │ ← y=183
 *   │  └─────┘                                │
 *   └──────────────────────────────────────────┘
 */
static lv_obj_t *create_main_screen(void)
{
        int row1_y = 5;
        int row1_card_h = 75;
        int row2_y = 88;
        int row2_card_h = 65;
        int col0_x = 12;
        int col1_x = 115;
        int col2_x = 218;

        /*
         * 创建屏幕对象
         */
        lv_obj_t *screen = lv_scr_act();

        /* 设置背景颜色 (粉白) — 不调用remove_style_all避免破坏LVGL默认主题 */
        lv_obj_set_style_bg_color(screen, THEME_BG_COLOR, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

        /*
         * 第一行: 3个功能卡片 (手动定位)
         */

        /* 卡片1: 直流有刷电机 (蓝绿色) */
        gui.cards[MODULE_MOTOR] = create_module_card(
                screen, MODULE_MOTOR,
                "直流电机", LV_SYMBOL_POWER, COLOR_MOTOR);
        lv_obj_set_pos(gui.cards[MODULE_MOTOR], col0_x, row1_y);
        lv_obj_set_size(gui.cards[MODULE_MOTOR],
                        GUI_CARD_WIDTH, row1_card_h);

        /* 卡片2: NTC温度采集 (橙红色) */
        gui.cards[MODULE_NTC] = create_module_card(
                screen, MODULE_NTC,
                "NTC温度", LV_SYMBOL_BELL, COLOR_NTC);
        lv_obj_set_pos(gui.cards[MODULE_NTC], col1_x, row1_y);
        lv_obj_set_size(gui.cards[MODULE_NTC],
                        GUI_CARD_WIDTH, row1_card_h);

        /* 卡片3: 温湿度传感器 (青蓝色) */
        gui.cards[MODULE_TEMP_HUMIDITY] = create_module_card(
                screen, MODULE_TEMP_HUMIDITY,
                "温湿度",
                "\xEF\xBC\x92\xE2\x98\x83",  /* Unicode: 🌒 (云+水滴) */
                COLOR_TEMP_HUMIDITY);
        lv_obj_set_pos(gui.cards[MODULE_TEMP_HUMIDITY], col2_x, row1_y);
        lv_obj_set_size(gui.cards[MODULE_TEMP_HUMIDITY],
                        GUI_CARD_WIDTH, row1_card_h);

        /*
         * 第二行左侧: RS232通信 (紫色)
         */
        gui.cards[MODULE_RS485] = create_module_card(
                screen, MODULE_RS485,
                "RS232", LV_SYMBOL_USB, COLOR_RS485);
        lv_obj_set_pos(gui.cards[MODULE_RS485], col0_x, row2_y);
        lv_obj_set_size(gui.cards[MODULE_RS485],
                        GUI_CARD_WIDTH, row2_card_h);

        /*
         * 左下角御姐小头像 (RS232下方, 32x32)
         */
        gui.anime_avatar = lv_img_create(screen);
        lv_img_set_src(gui.anime_avatar, &anime_sakoji_avatar);
        lv_obj_set_pos(gui.anime_avatar, col0_x + 28,
                        row2_y + row2_card_h + 6);

        /*
         * CAN总线卡片 (小头像下方, 紧凑高度)
         */
        gui.cards[MODULE_CAN] = create_module_card(
                screen, MODULE_CAN,
                "CAN总线", LV_SYMBOL_BELL, COLOR_CAN);
        lv_obj_set_pos(gui.cards[MODULE_CAN], col0_x,
                        row2_y + row2_card_h + 5 + 36);
        lv_obj_set_size(gui.cards[MODULE_CAN],
                        GUI_CARD_WIDTH, 40);

        /*
         * 创建二次元御姐立绘 (右下角常驻)
         */
        gui.anime_character = create_anime_character(screen);

        pr_info_with_tag("GUI", "Main screen created with 5 cards + anime + timestamp\n");

        return screen;
}

/* ==================== 二级界面创建函数 ==================== */

/**
 * create_motor_screen - 电机控制界面
 *
 * 界面元素:
 *   - 标题栏: [←返回] 电机控制 [头像]
 *   - 转速滑块: 0~100% PWM
 *   - 启停开关: 开始/停止按钮
 *   - 方向切换: 正转/反转按钮
 *   - 状态显示: 当前转速、运行方向
 */
static lv_obj_t *create_motor_screen(void)
{
        lv_obj_t *screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(screen, THEME_BG_COLOR, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

        lv_obj_t *content = create_title_bar(screen, MODULE_MOTOR,
                                             "直流有刷电机");
        lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(content, 20, 0);

        /* 转速滑块 */
        lv_obj_t *lbl_slider = lv_label_create(content);
        lv_label_set_text(lbl_slider, "转速调节 (PWM)");
        lv_obj_set_style_text_font(lbl_slider, &lv_font_simsun_16_cjk, 0);
        lv_obj_set_style_text_color(lbl_slider, THEME_TEXT_SECONDARY, 0);
        gui.lbl_slider_hint = lbl_slider;

        gui.slider_motor_speed = lv_slider_create(content);
        lv_obj_set_width(gui.slider_motor_speed, 250);
        lv_slider_set_range(gui.slider_motor_speed, 0, 100);
        lv_slider_set_value(gui.slider_motor_speed, 0, LV_ANIM_OFF);  /* v8: 需要anim参数 */
        lv_obj_set_style_bg_color(gui.slider_motor_speed,
                                   COLOR_MOTOR, LV_PART_INDICATOR);
	lv_obj_add_event_cb(gui.slider_motor_speed,
	                    motor_slider_event_cb,
	                    LV_EVENT_VALUE_CHANGED, NULL);

        /* 转速数值显示 */
        gui.label_speed_value = lv_label_create(content);
        lv_label_set_text(gui.label_speed_value, "0 RPM");
        lv_obj_add_style(gui.label_speed_value, &style_text_large, 0);

        /* 启停按钮 */
        gui.btn_start_stop = lv_btn_create(content);
        lv_obj_set_size(gui.btn_start_stop, 150, 45);
        lv_obj_add_style(gui.btn_start_stop, &style_btn_primary, 0);
        lv_obj_t *lbl_start = lv_label_create(gui.btn_start_stop);
        lv_label_set_text(lbl_start, LV_SYMBOL_PLAY "  启动电机");
        lv_obj_set_style_text_font(lbl_start, &lv_font_simsun_16_cjk, 0);

	lv_obj_add_event_cb(gui.btn_start_stop,
	                    motor_start_stop_event_cb,
	                    LV_EVENT_CLICKED, NULL);
        /* 方向按钮组 */
        lv_obj_t *btn_row = lv_obj_create(content);
        lv_obj_set_size(btn_row, 260, 45);
        lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
        lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        gui.btn_direction = lv_btn_create(btn_row);
        lv_obj_set_size(gui.btn_direction, 110, 40);
        lv_obj_add_style(gui.btn_direction, &style_btn_secondary, 0);
        lv_obj_t *lbl_dir = lv_label_create(gui.btn_direction);
        lv_label_set_text(lbl_dir, LV_SYMBOL_REFRESH " 正转");
        lv_obj_set_style_text_font(lbl_dir, &lv_font_simsun_16_cjk, 0);

	lv_obj_add_event_cb(gui.btn_direction,
	                    motor_direction_event_cb,
	                    LV_EVENT_CLICKED, NULL);

	/* PID开关按钮 */
	gui.btn_pid_toggle = lv_btn_create(btn_row);
	lv_obj_set_size(gui.btn_pid_toggle, 80, 40);
	lv_obj_add_style(gui.btn_pid_toggle, &style_btn_secondary, 0);
	lv_obj_t *lbl_pid = lv_label_create(gui.btn_pid_toggle);
	lv_label_set_text(lbl_pid, "PID OFF");
	lv_obj_set_style_text_font(lbl_pid, &lv_font_simsun_16_cjk, 0);
	lv_obj_add_event_cb(gui.btn_pid_toggle,
	                    motor_pid_toggle_event_cb,
	                    LV_EVENT_CLICKED, NULL);

        pr_debug_with_tag("GUI", "Motor control screen created\n");

        return screen;
}


/* ==================== 电机界面事件处理 ==================== */

static void motor_slider_event_cb(lv_event_t *e)
{
	lv_obj_t *slider = lv_event_get_target(e);
	int32_t val = (int32_t)lv_slider_get_value(slider);

	motor_cmd_shared.slider_percent = (int8_t)val;
	motor_cmd_shared.duty_changed = true;
}

static void motor_start_stop_event_cb(lv_event_t *e)
{
	(void)e;
	motor_cmd_shared.start_request = true;
}

static void motor_direction_event_cb(lv_event_t *e)
{
	(void)e;
	motor_cmd_shared.direction = -motor_cmd_shared.direction;
	motor_cmd_shared.dir_changed = true;
}

static void motor_pid_toggle_event_cb(lv_event_t *e)
{
	(void)e;
	motor_cmd_shared.pid_toggle_request = true;
}

/*
 * gui_app_consume_motor_cmds - 消费电机控制命令 (由enterprise任务调用)
 *
 * 在enterprise任务上下文中执行, 安全调用bsp_motor_*函数。
 */
void gui_app_consume_motor_cmds(void)
{
	if (!motor_cmd_shared.start_request &&
	    !motor_cmd_shared.stop_request &&
	    !motor_cmd_shared.duty_changed &&
	    !motor_cmd_shared.dir_changed)
		return;

	enum motor_state state;

	bsp_motor_get_state(&state);

	if (motor_cmd_shared.dir_changed) {
		bsp_motor_set_direction(motor_cmd_shared.direction);
		motor_cmd_shared.dir_changed = false;
	}

	if (motor_cmd_shared.start_request) {
		motor_cmd_shared.start_request = false;
		if (state == MOTOR_STATE_IDLE) {
			bsp_motor_start();
			int32_t duty = (int32_t)motor_cmd_shared.slider_percent
				* BSP_MOTOR_PWM_MAX_DUTY / 100;
			if (duty > 0)
				bsp_motor_set_duty(duty);
		} else if (state == MOTOR_STATE_RUNNING) {
			bsp_motor_stop();
		}
	}

	if (motor_cmd_shared.duty_changed) {
		motor_cmd_shared.duty_changed = false;
		/*
		 * PID模式下不直接设置duty, PID控制器自行调节。
		 * 非PID模式保持Phase 1行为: slider=占空比%。
		 */
		if (state == MOTOR_STATE_RUNNING &&
		    !motor_cmd_shared.pid_active) {
			int32_t duty = (int32_t)motor_cmd_shared.slider_percent
				* BSP_MOTOR_PWM_MAX_DUTY / 100;
			bsp_motor_set_duty(duty);
		}
	}

	if (motor_cmd_shared.stop_request) {
		motor_cmd_shared.stop_request = false;
		bsp_motor_stop();
	}

	if (motor_cmd_shared.pid_toggle_request) {
		motor_cmd_shared.pid_toggle_request = false;
		motor_cmd_shared.pid_active = !motor_cmd_shared.pid_active;
	}
}

/**
 * create_temp_humidity_screen - 温湿度显示界面
 *
 * 界面元素:
 *   - 大号温度数值显示 (实时更新)
 *   - 大号湿度数值显示 (实时更新)
 *   - 温度趋势曲线图 (最近60秒数据)
 *   - 刷新按钮
 *   - 传感器状态指示灯
 */
static lv_obj_t *create_temp_humidity_screen(void)
{
        lv_obj_t *screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(screen, THEME_BG_COLOR, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

        lv_obj_t *content = create_title_bar(screen, MODULE_TEMP_HUMIDITY,
                                             "温湿度监控");
        lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(content, LV_FLEX_ALIGN_SPACE_EVENLY,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_all(content, 15, 0);

        /*
         * 左侧: 温度显示卡片
         */
        lv_obj_t *card_temp = lv_obj_create(content);
        lv_obj_set_size(card_temp, 140, 170);
        lv_obj_add_style(card_temp, &style_card, 0);

        lv_obj_t *icon_temp = lv_label_create(card_temp);
        lv_label_set_text(icon_temp, LV_SYMBOL_BELL);
        lv_obj_set_style_text_color(icon_temp, COLOR_NTC, 0);
        lv_obj_set_style_text_font(icon_temp,
                                   &lv_font_montserrat_20, 0);
        lv_obj_align(icon_temp, LV_ALIGN_TOP_MID, 0, 10);

        gui.label_temp = lv_label_create(card_temp);
        lv_label_set_text(gui.label_temp, "--.- °C");
        lv_obj_add_style(gui.label_temp, &style_text_large, 0);
        lv_obj_align(gui.label_temp, LV_ALIGN_CENTER, 0, 10);

        lv_obj_t *unit_temp = lv_label_create(card_temp);
        lv_label_set_text(unit_temp, "温度");
        lv_obj_set_style_text_font(unit_temp, &lv_font_simsun_16_cjk, 0);
        lv_obj_set_style_text_color(unit_temp, THEME_TEXT_SECONDARY, 0);
        lv_obj_align(unit_temp, LV_ALIGN_BOTTOM_MID, 0, -10);

        /*
         * 右侧: 湿度显示卡片
         */
        lv_obj_t *card_hum = lv_obj_create(content);
        lv_obj_set_size(card_hum, 140, 170);
        lv_obj_add_style(card_hum, &style_card, 0);

        lv_obj_t *icon_hum = lv_label_create(card_hum);
        lv_label_set_text(icon_hum, "\xE2\x92\x92");  /* 水滴符号 */
        lv_obj_set_style_text_color(icon_hum, COLOR_TEMP_HUMIDITY, 0);
        lv_obj_set_style_text_font(icon_hum,
                                   &lv_font_montserrat_20, 0);
        lv_obj_align(icon_hum, LV_ALIGN_TOP_MID, 0, 10);

        gui.label_humidity = lv_label_create(card_hum);
        lv_label_set_text(gui.label_humidity, "--.- %RH");
        lv_obj_add_style(gui.label_humidity, &style_text_large, 0);
        lv_obj_align(gui.label_humidity, LV_ALIGN_CENTER, 0, 10);

        lv_obj_t *unit_hum = lv_label_create(card_hum);
        lv_label_set_text(unit_hum, "湿度");
        lv_obj_set_style_text_font(unit_hum, &lv_font_simsun_16_cjk, 0);
        lv_obj_set_style_text_color(unit_hum, THEME_TEXT_SECONDARY, 0);
        lv_obj_align(unit_hum, LV_ALIGN_BOTTOM_MID, 0, -10);

        pr_debug_with_tag("GUI", "Temp/Humidity screen created\n");

        return screen;
}

/* ==================== 其他二级界面 (简化版) ==================== */

/**
 * create_ntc_screen - NTC温度采集界面
 */
static lv_obj_t *create_ntc_screen(void)
{
        lv_obj_t *screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(screen, THEME_BG_COLOR, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

        lv_obj_t *content = create_title_bar(screen, MODULE_NTC,
                                             "NTC温度采集");
        lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        /* NTC图标 */
        lv_obj_t *icon_ntc = lv_label_create(content);
        lv_label_set_text(icon_ntc, LV_SYMBOL_BELL);
        lv_obj_set_style_text_color(icon_ntc, COLOR_NTC, 0);
        lv_obj_set_style_text_font(icon_ntc, &lv_font_montserrat_20, 0);

        /* NTC温度数值 */
        gui.label_ntc_temp = lv_label_create(content);
        lv_label_set_text(gui.label_ntc_temp, "--.- °C");
        lv_obj_add_style(gui.label_ntc_temp, &style_text_large, 0);

        /* 说明文字 */
        lv_obj_t *hint = lv_label_create(content);
        lv_label_set_text(hint, "NTC热敏电阻 (ADC3_IN9 / PF3)");
        lv_obj_set_style_text_font(hint, &lv_font_simsun_16_cjk, 0);
        lv_obj_set_style_text_color(hint, THEME_TEXT_SECONDARY, 0);

        return screen;
}

/**
 * create_rs485_screen - RS232通信界面
 */
static lv_obj_t *create_rs485_screen(void)
{
        lv_obj_t *screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(screen, THEME_BG_COLOR, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

        lv_obj_t *content = create_title_bar(screen, MODULE_RS485,
                                             "RS232通信");

        lv_obj_t *msg = lv_label_create(content);
        lv_label_set_text(msg, "RS232总线配置\n\n"
                           "波特率: 115200 bps\n"
                           "数据位: 8\n"
                           "停止位: 1\n"
                           "校验: None\n\n"
                           "(UART通讯协议)");
        lv_obj_center(msg);
        lv_obj_set_style_text_color(msg, THEME_TEXT_SECONDARY, 0);

        return screen;
}

/**
 * create_can_screen - CAN总线监控界面
 */
static lv_obj_t *create_can_screen(void)
{
        lv_obj_t *screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(screen, THEME_BG_COLOR, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

        lv_obj_t *content = create_title_bar(screen, MODULE_CAN,
                                             "CAN总线监控");

        lv_obj_t *msg = lv_label_create(content);
        lv_label_set_text(msg, "CAN总线状态\n\n"
                           "波特率: 500 kbps\n"
                           "工作模式: Normal\n"
                           "错误计数: TX=0 RX=0\n\n"
                           "(接收帧数: 0)");
        lv_obj_set_style_text_font(msg, &lv_font_simsun_16_cjk, 0);
        lv_obj_center(msg);
        lv_obj_set_style_text_color(msg, THEME_TEXT_SECONDARY, 0);

        return screen;
}

/* ==================== 公开API实现 ==================== */

int gui_app_init(void)
{
        if (gui.initialized) {
                pr_warn_with_tag("GUI", "Already initialized\n");
                return 0;
        }

        /*
         * 注意: lv_init() 和显示驱动注册已在 main.c 中完成
         * 此处仅初始化UI样式和创建界面
         */

        /*
         * Step 1: 初始化UI样式
         */
        init_styles();

        /*
         * Step 2: 创建主屏幕
         */
        gui.main_screen = create_main_screen();
        gui.active_screen = gui.main_screen;

        /*
         * Step 3: 创建所有二级屏幕
         */
        gui.module_screens[MODULE_MOTOR] = create_motor_screen();
        gui.module_screens[MODULE_NTC] = create_ntc_screen();
        gui.module_screens[MODULE_TEMP_HUMIDITY] =
                create_temp_humidity_screen();
        gui.module_screens[MODULE_RS485] = create_rs485_screen();
        gui.module_screens[MODULE_CAN] = create_can_screen();

        gui.initialized = true;

        pr_info_with_tag("GUI", "GUI Application initialized successfully\n");
        pr_info_with_tag("GUI", "  Screen: %dx%d (Landscape mode)\n",
                         GUI_SCREEN_WIDTH, GUI_SCREEN_HEIGHT);
        pr_info_with_tag("GUI", "  Modules: %d cards + anime character\n",
                         MODULE_COUNT);

        return 0;
}

int gui_app_deinit(void)
{
        if (!gui.initialized) {
                return -ENODEV;
        }

        /* LVGL会自动清理所有对象 */

        gui.initialized = false;

        pr_info_with_tag("GUI", "GUI Application deinitialized\n");
        return 0;
}

void gui_app_show_module_screen(enum gui_module_id module_id)
{
        if (module_id >= MODULE_COUNT || !gui.initialized) {
                return;
        }

        gui.active_screen = gui.module_screens[module_id];
        lv_scr_load(gui.active_screen);

        pr_debug_with_tag("GUI", "Switched to module screen: %d\n", module_id);
}

void gui_app_show_main_screen(void)
{
        if (!gui.initialized) {
                return;
        }

        gui.active_screen = gui.main_screen;
        lv_scr_load(gui.active_screen);

        pr_debug_with_tag("GUI", "Returned to main screen\n");
}

lv_obj_t *gui_app_get_active_screen(void)
{
        if (!gui.initialized)
                return NULL;

        return gui.active_screen;
}

void gui_app_update_sensor_data(float temperature, float humidity)
{
        gui_sensor_shared.temperature = temperature;
        gui_sensor_shared.humidity = humidity;
        gui_sensor_shared.pending = true;
}

void gui_app_update_ntc_data(float temperature)
{
        gui_ntc_shared.temperature = temperature;
        gui_ntc_shared.pending = true;
}

/**
 * gui_app_process_updates - 处理跨任务数据更新 (由lvgl_gui任务周期调用)
 *
 * 消费 app_enterprise 任务写入的共享数据,
 * 在LVGL任务上下文中安全地更新控件。
 */
void gui_app_process_updates(void)
{
        if (!gui.initialized)
                return;

        /*
         * 消费传感器数据更新
         */
        if (gui_sensor_shared.pending) {
                if (gui.label_temp && gui.label_humidity) {
                        char buf_temp[16];
                        char buf_hum[24];
                        float t = gui_sensor_shared.temperature;
                        float h = gui_sensor_shared.humidity;
                        int t_dec = (int)(t * 10.0f + 0.5f);
                        int h_dec = (int)(h * 10.0f + 0.5f);

                        snprintf(buf_temp, sizeof(buf_temp), "%d.%d C",
                                 t_dec / 10, t_dec % 10);
                        snprintf(buf_hum, sizeof(buf_hum), "%d.%d %%",
                                 h_dec / 10, h_dec % 10);

                        lv_label_set_text(gui.label_temp, buf_temp);
                        lv_label_set_text(gui.label_humidity, buf_hum);
                }
                gui_sensor_shared.pending = false;
        }

        /*
         * 消费NTC温度数据更新
         */
        if (gui_ntc_shared.pending) {
                if (gui.label_ntc_temp) {
                        char buf[16];
                        float t = gui_ntc_shared.temperature;
                        int t_int = (int)t;
                        int t_frac = (int)((t - (float)(int)t) * 10.0f + 0.5f);
                        if (t_frac < 0) t_frac = -t_frac;

                        snprintf(buf, sizeof(buf), "%d.%d °C",
                                 t_int, t_frac);
                        lv_label_set_text(gui.label_ntc_temp, buf);
                }
                gui_ntc_shared.pending = false;
        }
}

void gui_app_update_motor_status(uint32_t speed,
                                  bool is_running,
                                  int8_t direction,
                                  bool pid_active,
                                  int32_t target_rpm)
{
        if (!gui.initialized || !gui.label_speed_value) {
                return;
        }

        static char buf_speed[32];
        if (pid_active && is_running) {
                snprintf(buf_speed, sizeof(buf_speed), "%lu / %ld RPM",
                         (unsigned long)speed, (long)target_rpm);
        } else {
                snprintf(buf_speed, sizeof(buf_speed), "%lu RPM",
                         (unsigned long)speed);
        }
        lv_label_set_text(gui.label_speed_value, buf_speed);

        /* 更新启停按钮文字 */
        if (gui.btn_start_stop) {
                lv_obj_t *lbl = lv_obj_get_child(gui.btn_start_stop, 0);
                if (lbl) {
                        if (is_running)
                                lv_label_set_text(lbl,
                                        LV_SYMBOL_STOP "  停止电机");
                        else
                                lv_label_set_text(lbl,
                                        LV_SYMBOL_PLAY "  启动电机");
                }
        }

        /* 更新PID按钮文字 */
        if (gui.btn_pid_toggle) {
                lv_obj_t *lbl = lv_obj_get_child(gui.btn_pid_toggle, 0);
                if (lbl) {
                        if (pid_active)
                                lv_label_set_text(lbl, "PID ON");
                        else
                                lv_label_set_text(lbl, "PID OFF");
                }
        }

        /* 更新滑块提示 */
        if (gui.lbl_slider_hint) {
                if (pid_active)
                        lv_label_set_text(gui.lbl_slider_hint,
                                "目标转速 (RPM)");
                else
                        lv_label_set_text(gui.lbl_slider_hint,
                                "转速调节 (PWM)");
        }

        /* 更新方向按钮文字 */
        if (gui.btn_direction) {
                lv_obj_t *lbl = lv_obj_get_child(gui.btn_direction, 0);
                if (lbl) {
                        if (direction > 0)
                                lv_label_set_text(lbl,
                                        LV_SYMBOL_REFRESH " 正转");
                        else
                                lv_label_set_text(lbl,
                                        LV_SYMBOL_REFRESH " 反转");
                }
        }

        pr_debug_with_tag("GUI", "Motor status updated: speed=%lu running=%d dir=%d\n",
                          (unsigned long)speed, is_running, direction);
}

/* ==================== 触摸输入设备集成 ==================== */

/**
 * touch_read_cb - LVGL触摸输入读取回调
 * @indev_drv: LVGL输入设备驱动
 * @data: 输出触摸数据
 *
 * 由LVGL周期调用 (每30ms), 读取触摸坐标并返回。
 */
static void touch_read_cb(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
        struct bsp_touch_data touch;
        int ret;

        (void)indev_drv;

        ret = bsp_touch_scan(&touch);

        if (ret > 0 && touch.num_points > 0) {
                data->state = LV_INDEV_STATE_PR;
                data->point.x = touch.points[0].x;
                data->point.y = touch.points[0].y;
        } else {
                data->state = LV_INDEV_STATE_REL;
        }
}

/**
 * gui_app_touch_input_init - 初始化触摸输入设备
 *
 * 创建LVGL输入设备驱动并注册触摸扫描回调。
 * 必须先调用 gui_app_init() 创建屏幕对象。
 *
 * Return: 0 成功, 负值错误码
 */
int gui_app_touch_input_init(void)
{
        int ret;

        if (!gui.initialized) {
                pr_error_with_tag("GUI",
                                 "Cannot init touch: GUI not initialized\n");
                return -EINVAL;
        }

        /*
         * 初始化触摸硬件驱动
         */
        ret = bsp_touch_init();
        if (ret != 0) {
                pr_warn_with_tag("GUI",
                                "Touch HW init failed: %d, "
                                "GUI will operate without touch\n", ret);
                return ret;
        }

        /*
         * 注册LVGL输入设备
         */
        static lv_indev_drv_t indev_drv;
        lv_indev_drv_init(&indev_drv);
        indev_drv.type = LV_INDEV_TYPE_POINTER;
        indev_drv.read_cb = touch_read_cb;
        lv_indev_drv_register(&indev_drv);

        pr_info_with_tag("GUI", "Touch input device registered with LVGL\n");

        return 0;
}

/* ==================== FreeRTOS GUI任务 ==================== */

/**
 * gui_task_entry - LVGL GUI刷新任务 (FreeRTOS任务入口)
 *
 * 任务功能:
 *   1. 初始化LVGL库和显示驱动
 *   2. 创建平板APP风格界面
 *   3. 周期性调用 lv_timer_handler() 处理UI事件和动画
 *
 * 调度参数:
 *   - 栈大小: 8KB (GUI任务需要较大栈空间)
 *   - 优先级: AboveNormal (保证界面流畅)
 *   - 周期: 5ms (200Hz，远超30FPS需求)
 *
 * 注意:
 *   - 此函数由 freertos.c 中的 osThreadNew() 调用
 *   - 仅在定义了 USE_GUI 宏时才编译
 */
void gui_task_entry(void *argument)
{
        (void)argument;

        pr_info_with_tag("GUI", "GUI refresh task running (200Hz)\n\n");

        while (1) {
                lv_timer_handler();

                /*
                 * 处理跨任务数据更新 (传感器数据等)
                 * 必须在lv_timer_handler()之后调用,
                 * 确保LVGL控件可安全访问。
                 */
                gui_app_process_updates();

                vTaskDelay(pdMS_TO_TICKS(5));
        }

        vTaskDelete(NULL);
}
