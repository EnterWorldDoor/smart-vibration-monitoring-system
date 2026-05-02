/**
 * @file gui_app.c
 * @brief 平板APP风格GUI实现 (二次元御姐主题)
 *
 * 界面架构:
 *   ┌─────────────────────────────────────┐
 *   │  [电机卡片] [NTC卡片] [温湿度卡片] │
 *   │                                     │
 *   │  [RS485]    [CAN]      [御姐立绘]   │ ← 右下角80x120
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
#include "lcd.h"
#include "system_log/system_log.h"

/* ==================== 模块内部状态 ==================== */

static struct {
        lv_obj_t *main_screen;            /* 主屏幕 */
        lv_obj_t *module_screens[MODULE_COUNT]; /* 5个二级屏幕 */

        /* 主屏幕控件 */
        lv_obj_t *cards[MODULE_COUNT];    /* 5个功能卡片 */
        lv_obj_t *anime_character;        /* 御姐立绘对象 */

        /* 温湿度界面控件 */
        lv_obj_t *label_temp;
        lv_obj_t *label_humidity;
        lv_obj_t *chart_temp_humidity;

        /* 电机界面控件 */
        lv_obj_t *slider_motor_speed;
        lv_obj_t *label_speed_value;
        lv_obj_t *btn_start_stop;
        lv_obj_t *btn_direction;

        bool initialized;
} gui = {0};

/* ==================== 前向声明 (Forward Declarations) ==================== */

/*
 * 事件处理函数 - 声明在使用之前
 * LVGL v8要求回调函数在使用前必须可见
 */
static void card_click_event_handler(lv_event_t *e);
static void btn_back_event_handler(lv_event_t *e);

/* ==================== 样式定义 ==================== */

/*
 * 全局样式对象 (静态分配，避免动态内存)
 */
static lv_style_t style_card;
static lv_style_t style_card_pressed;
static lv_style_t style_title;
static lv_style_t style_text_large;
static lv_style_t style_btn_primary;
static lv_style_t style_btn_secondary;

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
         * 标题文字样式 (粗体, 16px)
         */
        lv_style_init(&style_title);
        lv_style_set_text_font(&style_title,
                               &lv_font_montserrat_16);
        lv_style_set_text_color(&style_title,
                                THEME_TEXT_PRIMARY);

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
         * 标题文字
         */
        lv_obj_t *title_label = lv_label_create(card);
        lv_label_set_text(title_label, title);
        lv_obj_add_style(title_label, &style_title, 0);
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
                gui_app_show_module_screen((enum gui_module_id)id);
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
        (void)e;  /* 未使用参数 */

        pr_debug_with_tag("GUI", "Back button pressed\n");

        /* 切换回主屏幕 */
        gui_app_show_main_screen();
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
        /*
         * 创建图片容器
         */
        lv_obj_t *img_container = lv_obj_create(parent);
        lv_obj_set_size(img_container,
                        ANIME_CHARACTER_WIDTH,
                        ANIME_CHARACTER_HEIGHT);

        /* 定位到右下角 */
        lv_obj_align(img_container, LV_ALIGN_BOTTOM_RIGHT, -5, -5);

        /* 设置透明背景 (不遮挡其他控件) */
        lv_obj_set_style_bg_opa(img_container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(img_container, 1, 0);
        lv_obj_set_style_border_color(img_container,
                                       lv_color_hex(0xFF80AB), 0);  /* 粉色边框 */

        /*
         * 创建占位符文本 (提示用户替换为真实图片)
         * 实际项目中应使用:
         *   lv_img_set_src(img, &anime_img_data);
         * 其中 anime_img_data 由图片转换工具生成
         */
        lv_obj_t *placeholder = lv_label_create(img_container);
        lv_label_set_text(placeholder, "祥子\n立绘\n80x120");
        lv_obj_center(placeholder);
        lv_label_set_long_mode(placeholder, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(placeholder,
                                    LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(placeholder,
                                    lv_color_hex(0xEC407A), 0);  /* 粉红色 */
        lv_obj_set_style_text_font(placeholder,
                                   &lv_font_montserrat_10, 0);

        /*
         * TODO: 替换为真实的二次元立绘图片
         *
         * 推荐角色 (丰川祥子风格):
         *   - 发型: 黑色长直发/双马尾
         *   - 服装: 黑色制服/摇滚风
         *   - 表情: 酷帅/微笑 (两种状态)
         *   - 姿势: 抱吉他/站立
         *
         * 图片格式转换命令:
         *   $ python lvgl_img_converter.py \
         *       --input sakoji.png \
         *       --output anime_sakoji.c \
         *       --format RGB565 \
         *       --size 80x120
         *
         * 使用示例:
         *   LV_IMG_DECLARE(anime_sakoji);
         *   lv_img_set_src(img, &anime_sakoji);
         */

        return img_container;
}

/* ==================== 主屏幕创建 ==================== */

/**
 * create_main_screen - 创建主屏幕界面
 *
 * 布局方案 (320x240横屏):
 *   ┌──────────────────────────────────────────┐
 *   │  ┌─────┐  ┌─────┐  ┌─────┐              │
 *   │  │电机 │  │NTC │  │温湿度│              │
 *   │  └─────┘  └─────┘  └─────┘              │
 *   │                                          │
 *   │  ┌─────┐  ┌─────┐              ┌──────┐ │
 *   │  │RS485│  │CAN │              │御姐  │ │
 *   │  └─────┘  └─────┘              │立绘  │ │
 *   │                                └──────┘ │
 *   └──────────────────────────────────────────┘
 */
static lv_obj_t *create_main_screen(void)
{
        /*
         * 创建屏幕对象
         */
        lv_obj_t *screen = lv_scr_act();
        lv_obj_remove_style_all(screen);  /* 清除默认样式 */

        /* 设置背景颜色 (浅灰磨砂质感) */
        lv_obj_set_style_bg_color(screen, THEME_BG_COLOR, 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

        /*
         * 创建Flex布局容器 (自动排列卡片)
         */
        lv_obj_t *container = lv_obj_create(screen);
        lv_obj_set_size(container, GUI_SCREEN_WIDTH, GUI_SCREEN_HEIGHT);
        lv_obj_set_pos(container, 0, 0);
        lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(container, LV_FLEX_ALIGN_SPACE_EVENLY,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_all(container, GUI_CARD_MARGIN, 0);
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);

        /*
         * 第一行: 3个功能卡片
         */

        /* 卡片1: 直流有刷电机 (蓝绿色) */
        gui.cards[MODULE_MOTOR] = create_module_card(
                container,
                MODULE_MOTOR,
                "直流电机",
                LV_SYMBOL_POWER,
                COLOR_MOTOR
        );

        /* 卡片2: NTC温度采集 (橙红色) */
        gui.cards[MODULE_NTC] = create_module_card(
                container,
                MODULE_NTC,
                "NTC温度",
                LV_SYMBOL_BELL,  /* v8: THERMOMETER未定义,使用BELL替代 */
                COLOR_NTC
        );

        /* 卡片3: 温湿度传感器 (青蓝色) */
        gui.cards[MODULE_TEMP_HUMIDITY] = create_module_card(
                container,
                MODULE_TEMP_HUMIDITY,
                "温湿度",
                "\xEF\xBC\x92\xE2\x98\x83",  /* Unicode: 🌒 (云+水滴) */
                COLOR_TEMP_HUMIDITY
        );

        /*
         * 第二行: 2个卡片 + 占位符(给立绘留位置)
         */

        /* 卡片4: RS485通信 (紫色) */
        gui.cards[MODULE_RS485] = create_module_card(
                container,
                MODULE_RS485,
                "RS485",
                LV_SYMBOL_USB,
                COLOR_RS485
        );

        /* 卡片5: CAN总线 (深蓝色) */
        gui.cards[MODULE_CAN] = create_module_card(
                container,
                MODULE_CAN,
                "CAN总线",
                LV_SYMBOL_BELL,
                COLOR_CAN
        );

        /*
         * 创建二次元御姐立绘 (右下角常驻)
         */
        gui.anime_character = create_anime_character(screen);

        pr_info_with_tag("GUI", "Main screen created with %d cards + anime character\n",
                         MODULE_COUNT);

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
        lv_obj_remove_style_all(screen);

        /* 背景 */
        lv_obj_set_style_bg_color(screen, THEME_BG_COLOR, 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

        /*
         * 标题栏
         */
        lv_obj_t *header = lv_obj_create(screen);
        lv_obj_set_size(header, GUI_SCREEN_WIDTH, 40);
        lv_obj_set_pos(header, 0, 0);
        lv_obj_set_style_bg_color(header, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(header, 1, 0);
        lv_obj_set_style_border_color(header, lv_color_hex(0xE0E0E0), 0);

        /* 返回按钮 */
        lv_obj_t *btn_back = lv_btn_create(header);
        lv_obj_set_size(btn_back, 60, 30);
        lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_add_event_cb(btn_back, btn_back_event_handler,
                            LV_EVENT_CLICKED, NULL);
        lv_obj_t *lbl_back = lv_label_create(btn_back);
        lv_label_set_text(lbl_back, LV_SYMBOL_LEFT " 返回");

        /* 标题文字 */
        lv_obj_t *title = lv_label_create(header);
        lv_label_set_text(title, "直流有刷电机");
        lv_obj_add_style(title, &style_title, 0);
        lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

        /*
         * 内容区域
         */
        lv_obj_t *content = lv_obj_create(screen);
        lv_obj_set_size(content, GUI_SCREEN_WIDTH, GUI_SCREEN_HEIGHT - 40);
        lv_obj_set_pos(content, 0, 40);
        lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
        lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(content, 20, 0);

        /* 转速滑块 */
        lv_obj_t *lbl_slider = lv_label_create(content);
        lv_label_set_text(lbl_slider, "转速调节 (PWM)");
        lv_obj_set_style_text_color(lbl_slider, THEME_TEXT_SECONDARY, 0);

        gui.slider_motor_speed = lv_slider_create(content);
        lv_obj_set_width(gui.slider_motor_speed, 250);
        lv_slider_set_range(gui.slider_motor_speed, 0, 100);
        lv_slider_set_value(gui.slider_motor_speed, 0, LV_ANIM_OFF);  /* v8: 需要anim参数 */
        lv_obj_set_style_bg_color(gui.slider_motor_speed,
                                   COLOR_MOTOR, LV_PART_INDICATOR);

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

        pr_debug_with_tag("GUI", "Motor control screen created\n");

        return screen;
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
        lv_obj_remove_style_all(screen);
        lv_obj_set_style_bg_color(screen, THEME_BG_COLOR, 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

        /* 标题栏 (同上模式) */
        lv_obj_t *header = lv_obj_create(screen);
        lv_obj_set_size(header, GUI_SCREEN_WIDTH, 40);
        lv_obj_set_pos(header, 0, 0);
        lv_obj_set_style_bg_color(header, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(header, 1, 0);

        lv_obj_t *btn_back = lv_btn_create(header);
        lv_obj_set_size(btn_back, 60, 30);
        lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_add_event_cb(btn_back, btn_back_event_handler,
                            LV_EVENT_CLICKED, NULL);
        lv_obj_t *lbl_back = lv_label_create(btn_back);
        lv_label_set_text(lbl_back, LV_SYMBOL_LEFT " 返回");

        lv_obj_t *title = lv_label_create(header);
        lv_label_set_text(title, "温湿度监控");
        lv_obj_add_style(title, &style_title, 0);
        lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

        /*
         * 数据展示区域
         */
        lv_obj_t *content = lv_obj_create(screen);
        lv_obj_set_size(content, GUI_SCREEN_WIDTH, GUI_SCREEN_HEIGHT - 40);
        lv_obj_set_pos(content, 0, 40);
        lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
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
        lv_label_set_text(icon_temp, LV_SYMBOL_BELL);  /* v8: 使用BELL替代THERMOMETER */
        lv_obj_set_style_text_color(icon_temp, COLOR_NTC, 0);
        lv_obj_set_style_text_font(icon_temp,
                                   &lv_font_montserrat_20, 0);  /* v8: 最大启用字体20pt */
        lv_obj_align(icon_temp, LV_ALIGN_TOP_MID, 0, 10);

        gui.label_temp = lv_label_create(card_temp);
        lv_label_set_text(gui.label_temp, "--.- °C");
        lv_obj_add_style(gui.label_temp, &style_text_large, 0);
        lv_obj_align(gui.label_temp, LV_ALIGN_CENTER, 0, 10);

        lv_obj_t *unit_temp = lv_label_create(card_temp);
        lv_label_set_text(unit_temp, "温度");
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
                                   &lv_font_montserrat_20, 0);  /* v8: 最大启用字体20pt */
        lv_obj_align(icon_hum, LV_ALIGN_TOP_MID, 0, 10);

        gui.label_humidity = lv_label_create(card_hum);
        lv_label_set_text(gui.label_humidity, "--.- %RH");
        lv_obj_add_style(gui.label_humidity, &style_text_large, 0);
        lv_obj_align(gui.label_humidity, LV_ALIGN_CENTER, 0, 10);

        lv_obj_t *unit_hum = lv_label_create(card_hum);
        lv_label_set_text(unit_hum, "湿度");
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
        lv_obj_remove_style_all(screen);
        lv_obj_set_style_bg_color(screen, THEME_BG_COLOR, 0);

        /* 标题栏 */
        lv_obj_t *header = lv_obj_create(screen);
        lv_obj_set_size(header, GUI_SCREEN_WIDTH, 40);
        lv_obj_set_style_bg_color(header, lv_color_hex(0xFFFFFF), 0);

        lv_obj_t *btn_back = lv_btn_create(header);
        lv_obj_set_size(btn_back, 60, 30);
        lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_add_event_cb(btn_back, btn_back_event_handler,
                            LV_EVENT_CLICKED, NULL);
        lv_obj_t *lbl = lv_label_create(btn_back);
        lv_label_set_text(lbl, LV_SYMBOL_LEFT " 返回");

        lv_obj_t *title = lv_label_create(header);
        lv_label_set_text(title, "NTC温度采集");
        lv_obj_add_style(title, &style_title, 0);
        lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

        /* 内容区占位 */
        lv_obj_t *content = lv_obj_create(screen);
        lv_obj_set_size(content, GUI_SCREEN_WIDTH, GUI_SCREEN_HEIGHT - 40);
        lv_obj_set_pos(content, 0, 40);
        lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);

        lv_obj_t *msg = lv_label_create(content);
        lv_label_set_text(msg, "NTC温度传感器数据\n\n"
                           "当前温度: --.- °C\n\n"
                           "(硬件接口待连接)");
        lv_obj_center(msg);
        lv_obj_set_style_text_color(msg, THEME_TEXT_SECONDARY, 0);

        return screen;
}

/**
 * create_rs485_screen - RS485通信界面
 */
static lv_obj_t *create_rs485_screen(void)
{
        lv_obj_t *screen = lv_obj_create(NULL);
        lv_obj_remove_style_all(screen);
        lv_obj_set_style_bg_color(screen, THEME_BG_COLOR, 0);

        lv_obj_t *header = lv_obj_create(screen);
        lv_obj_set_size(header, GUI_SCREEN_WIDTH, 40);
        lv_obj_set_style_bg_color(header, lv_color_hex(0xFFFFFF), 0);

        lv_obj_t *btn_back = lv_btn_create(header);
        lv_obj_set_size(btn_back, 60, 30);
        lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_add_event_cb(btn_back, btn_back_event_handler,
                            LV_EVENT_CLICKED, NULL);
        lv_obj_t *lbl = lv_label_create(btn_back);
        lv_label_set_text(lbl, LV_SYMBOL_LEFT " 返回");

        lv_obj_t *title = lv_label_create(header);
        lv_label_set_text(title, "RS485通信");
        lv_obj_add_style(title, &style_title, 0);
        lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

        lv_obj_t *content = lv_obj_create(screen);
        lv_obj_set_size(content, GUI_SCREEN_WIDTH, GUI_SCREEN_HEIGHT - 40);
        lv_obj_set_pos(content, 0, 40);
        lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);

        lv_obj_t *msg = lv_label_create(content);
        lv_label_set_text(msg, "RS485总线配置\n\n"
                           "波特率: 9600 bps\n"
                           "数据位: 8\n"
                           "停止位: 1\n"
                           "校验: None\n\n"
                           "(Modbus RTU协议)");
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
        lv_obj_remove_style_all(screen);
        lv_obj_set_style_bg_color(screen, THEME_BG_COLOR, 0);

        lv_obj_t *header = lv_obj_create(screen);
        lv_obj_set_size(header, GUI_SCREEN_WIDTH, 40);
        lv_obj_set_style_bg_color(header, lv_color_hex(0xFFFFFF), 0);

        lv_obj_t *btn_back = lv_btn_create(header);
        lv_obj_set_size(btn_back, 60, 30);
        lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_add_event_cb(btn_back, btn_back_event_handler,
                            LV_EVENT_CLICKED, NULL);
        lv_obj_t *lbl = lv_label_create(btn_back);
        lv_label_set_text(lbl, LV_SYMBOL_LEFT " 返回");

        lv_obj_t *title = lv_label_create(header);
        lv_label_set_text(title, "CAN总线监控");
        lv_obj_add_style(title, &style_title, 0);
        lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

        lv_obj_t *content = lv_obj_create(screen);
        lv_obj_set_size(content, GUI_SCREEN_WIDTH, GUI_SCREEN_HEIGHT - 40);
        lv_obj_set_pos(content, 0, 40);
        lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);

        lv_obj_t *msg = lv_label_create(content);
        lv_label_set_text(msg, "CAN总线状态\n\n"
                           "波特率: 500 kbps\n"
                           "工作模式: Normal\n"
                           "错误计数: TX=0 RX=0\n\n"
                           "(接收帧数: 0)");
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
         * Step 1: 初始化LVGL库
         */
        lv_init();

        /*
         * Step 2: 注册显示驱动 (FSMC LCD)
         */
        static lv_disp_draw_buf_t draw_buf;
        static lv_color_t buf1[GUI_SCREEN_WIDTH * 50];  /* 行缓冲 */
        static lv_color_t buf2[GUI_SCREEN_WIDTH * 50];  /* 双缓冲 */

        lv_disp_draw_buf_init(&draw_buf, buf1, buf2,
                              GUI_SCREEN_WIDTH * 50);

        static lv_disp_drv_t disp_drv;
        lv_disp_drv_init(&disp_drv);
        disp_drv.hor_res = GUI_SCREEN_WIDTH;
        disp_drv.ver_res = GUI_SCREEN_HEIGHT;
        disp_drv.flush_cb = lcd_flush_cb;  /* LCD刷新回调 */
        disp_drv.draw_buf = &draw_buf;
        disp_drv.user_data = NULL;

        lv_disp_drv_register(&disp_drv);

        /*
         * Step 3: 初始化UI样式
         */
        init_styles();

        /*
         * Step 4: 创建主屏幕
         */
        gui.main_screen = create_main_screen();

        /*
         * Step 5: 创建所有二级屏幕
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

        lv_scr_load(gui.module_screens[module_id]);

        pr_debug_with_tag("GUI", "Switched to module screen: %d\n", module_id);
}

void gui_app_show_main_screen(void)
{
        if (!gui.initialized) {
                return;
        }

        lv_scr_load(gui.main_screen);

        pr_debug_with_tag("GUI", "Returned to main screen\n");
}

void gui_app_update_sensor_data(float temperature, float humidity)
{
        if (!gui.initialized || !gui.label_temp || !gui.label_humidity) {
                return;
        }

        static char buf_temp[16];
        static char buf_hum[16];

        snprintf(buf_temp, sizeof(buf_temp), "%5.1f °C", temperature);
        snprintf(buf_hum, sizeof(buf_hum), "%5.1f %%RH", humidity);

        lv_label_set_text(gui.label_temp, buf_temp);
        lv_label_set_text(gui.label_humidity, buf_hum);

        pr_debug_with_tag("GUI", "Sensor data updated: T=%.1f H=%.1f\n",
                          temperature, humidity);
}

void gui_app_update_motor_status(uint32_t speed,
                                  bool is_running,
                                  int8_t direction)
{
        if (!gui.initialized || !gui.label_speed_value) {
                return;
        }

        static char buf_speed[16];
        snprintf(buf_speed, sizeof(buf_speed), "%lu RPM",
                 (unsigned long)speed);

        lv_label_set_text(gui.label_speed_value, buf_speed);

        pr_debug_with_tag("GUI", "Motor status updated: speed=%lu running=%d dir=%d\n",
                          (unsigned long)speed, is_running, direction);
}

/* ==================== LVGL显示驱动回调 ==================== */

/**
 * lcd_flush_cb - LCD显存刷新回调 (LVGL调用)
 *
 * 此函数由LVGL在渲染完成后调用，
 * 负责将帧缓冲区的像素数据写入LCD硬件。
 *
 * @disp_drv: 显示驱动指针
 * @area: 待刷新的区域坐标
 * @color_p: 像素数据缓冲区指针
 *
 * 实现流程:
 *   1. 设置FSMC窗口寻址范围 (bsp_lcd_set_window)
 *   2. 批量写入像素数据 (bsp_lcd_write_data_batch)
 *   3. 通知LVGL刷新完成 (lv_disp_flush_ready)
 */
void lcd_flush_cb(lv_disp_drv_t *disp_drv,
                  const lv_area_t *area,
                  lv_color_t *color_p)
{
        /*
         * 参数校验
         */
        if (!disp_drv || !area || !color_p) {
                return;
        }

        int32_t x1 = area->x1;
        int32_t y1 = area->y1;
        int32_t x2 = area->x2;
        int32_t y2 = area->y2;
        int32_t w = x2 - x1 + 1;
        int32_t h = y2 - y1 + 1;

        /* 边界检查 */
        if ((w <= 0) || (h <= 0)) {
                lv_disp_flush_ready(disp_drv);
                return;
        }

        /*
         * 设置FSMC写入窗口
         */
        bsp_lcd_set_window((uint16_t)x1, (uint16_t)y1,
                          (uint16_t)w, (uint16_t)h);

        /*
         * 批量写入像素数据到LCD显存
         * 注意: color_p 是 RGB565 格式的像素数组
         */
        bsp_lcd_write_data_batch((const uint16_t *)color_p,
                                (uint32_t)(w * h));

        /*
         * 通知LVGL刷新已完成
         * 必须调用! 否则LVGL会认为刷新未完成
         */
        lv_disp_flush_ready(disp_drv);
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

        pr_info_with_tag("GUI", "+======================================+\n");
        pr_info_with_tag("GUI", "|  LVGL GUI Task Started                |\n");
        pr_info_with_tag("GUI", "|  =================================   |\n");
        pr_info_with_tag("GUI", "|  ✅ Screen : 320x240 (Landscape)     |\n");
        pr_info_with_tag("GUI", "|  ✅ Driver : FSMC 16-bit ILI9341    |\n");
        pr_info_with_tag("GUI", "|  ✅ Theme  : Tablet APP Style       |\n");
        pr_info_with_tag("GUI", "|  ✅ Anime  : 80x120 Character       |\n");
        pr_info_with_tag("GUI", "+======================================+\n\n");

        /*
         * Step 1: 初始化LCD硬件
         */
        int ret = bsp_lcd_init();
        if (ret != 0) {
                pr_error_with_tag("GUI", "LCD init failed: %d\n", ret);
                vTaskDelete(NULL);  /* 删除自身 */
                return;
        }

        /*
         * Step 2: 初始化LVGL库
         */
        lv_init();

        /*
         * Step 3: 注册显示驱动 (FSMC LCD)
         */
        static lv_disp_draw_buf_t draw_buf;
        static lv_color_t buf1[GUI_SCREEN_WIDTH * 50];  /* 行缓冲 */
        static lv_color_t buf2[GUI_SCREEN_WIDTH * 50];  /* 双缓冲 */

        lv_disp_draw_buf_init(&draw_buf, buf1, buf2,
                              GUI_SCREEN_WIDTH * 50);

        static lv_disp_drv_t disp_drv;
        lv_disp_drv_init(&disp_drv);
        disp_drv.hor_res = GUI_SCREEN_WIDTH;
        disp_drv.ver_res = GUI_SCREEN_HEIGHT;
        disp_drv.flush_cb = lcd_flush_cb;  /* LCD刷新回调 */
        disp_drv.draw_buf = &draw_buf;
        disp_drv.user_data = NULL;

        lv_disp_drv_register(&disp_drv);

        pr_info_with_tag("GUI", "LVGL display driver registered\n");

        /*
         * Step 4: 初始化UI样式并创建界面
         */
        ret = gui_app_init();
        if (ret != 0) {
                pr_error_with_tag("GUI", "GUI app init failed: %d\n", ret);
                vTaskDelete(NULL);
                return;
        }

        pr_info_with_tag("GUI", "GUI application initialized successfully\n\n");

        /*
         * Step 5: 主循环 - LVGL定时器处理
         *
         * lv_timer_handler() 会:
         *   - 处理所有注册的定时器 (动画、滚动等)
         *   - 调用脏区域标记的控件重绘回调
         *   - 触发 lcd_flush_cb() 刷新LCD显存
         *
         * 周期说明:
         *   - 5ms周期 = 200Hz (理论最大帧率)
         *   - 实际帧率取决于UI复杂度和LCD刷新速度
         *   - 对于320x240分辨率，30FPS足够流畅
         */
        uint32_t gui_loop_count = 0;

        while (1) {
                /*
                 * 调用LVGL定时器处理器
                 */
                lv_timer_handler();

                gui_loop_count++;

                /*
                 * 周期性统计输出 (每200次循环 ≈ 1秒)
                 */
                if (gui_loop_count % 200 == 0) {
                        pr_debug_with_tag("GUI",
                                          "GUI loop count: %lu\n",
                                          (unsigned long)gui_loop_count);
                }

                /*
                 * 任务延时 (5ms周期)
                 * 使用vTaskDelay释放CPU给其他任务
                 */
                vTaskDelay(pdMS_TO_TICKS(5));  /* GUI任务周期: 5ms (200Hz) */
        }

        /*
         * 此处不可达 (while(1)无限循环)
         * 如果退出循环，删除任务自身
         */
        vTaskDelete(NULL);
}
