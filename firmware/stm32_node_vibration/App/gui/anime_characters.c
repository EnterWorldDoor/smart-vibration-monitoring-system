/**
 * @file anime_characters.c
 * @brief 二次元御姐立绘图片数据 (LVGL C数组格式)
 *
 * ⚠️ 重要提示:
 *   下方数据为**占位符**，需要替换为真实图片！
 *
 * 图片转换步骤:
 *   ┌─────────────────────────────────────────────┐
 *   │ 1. 准备PNG图片 (80x120 或 32x32 像素)      │
 *   │    - 推荐格式: PNG with Alpha channel       │
 *   │    - 背景透明: ✅ 必须                      │
 *   │    - 颜色模式: RGB或RGBA                     │
 *   │                                             │
 *   │ 2. 使用LVGL Image Converter转换              │
 *   │    工具地址: https://lvgl.io/tools/imageconverter │
 *   │                                             │
 *   │ 3. 转换设置:                               │
 *   │    Color format: RGB565 (True color)        │
 *   │    Output format: C array                   │
 *   │                                             │
 *   │ 4. 生成的文件内容复制到此处                 │
 *   └─────────────────────────────────────────────┘
 *
 * 在线转换工具:
 *   https://lvgl.io/tools/imageconverter
 *
 * 命令行工具 (Python):
 *   $ pip install lv_img_converter
 *   $ lv_img_converter.py sakoji.png --output sakoji.c \
 *     --format rgb565 --size 80x120
 */

#include "anime_characters.h"
#include "system_log/system_log.h"  /* pr_debug_with_tag */

/* ==================== 占位符图片数据 ==================== */

/*
 * ⚠️ 以下为示例占位符数据 (纯色方块)
 *
 * 实际使用时请替换为从LVGL Image Converter生成的真实图片数据！
 *
 * 示例数据说明:
 *   - 每个像素2字节 (RGB565格式)
 *   - 总大小 = 宽 × 高 × 2 字节
 *   - 80×120 = 19,200 字节 (约19KB)
 *   - 32×32 = 2,048 字节 (约2KB)
 */

/* ======== 角色1: 丰川祥子 (80x120 主立绘) ======== */

/*
 * 占位符: 粉色半透明渐变背景 + "祥子"文字区域
 * 实际应替换为: 黑发双马尾吉他手立绘
 */
const uint8_t anime_sakoji_main_map[] = {
        /*
         * 此处应为 LVGL Image Converter 生成的 C 数组
         * 格式示例:
         *
         * static const uint8_t anime_sakoji_main_map[] = {
         *     0x00, 0x00, 0xF8, 0x00, ...  // 像素数据 (RGB565)
         * };
         *
         * 总字节数: 80 * 120 * 2 = 19200 bytes
         */

        /* TODO: 替换为真实图片数据 */
        0xFF, 0xEC,  /* 浅粉色 (左上角) */
        0xFF, 0xEC,
        /* ... (省略中间数据) ... */
        0xEC, 0x40,  /* 深粉色 (右下角) */
};

const lv_img_dsc_t anime_sakoji_main = {
        .header.always_zero = 0,
        .header.w = 80,
        .header.h = 120,
        .header.cf = LV_IMG_CF_TRUE_COLOR,
        .data = anime_sakoji_main_map,
        .data_size = sizeof(anime_sakoji_main_map),
};

/* ======== 角色1: 丰川祥子微笑状态 (80x120) ======== */

const uint8_t anime_sakoji_smile_map[] = {
        /* TODO: 微笑表情的祥子立绘 */
        0xFF, 0xEC,
        /* ... */
};

const lv_img_dsc_t anime_sakoji_smile = {
        .header.always_zero = 0,
        .header.w = 80,
        .header.h = 120,
        .header.cf = LV_IMG_CF_TRUE_COLOR,
        .data = anime_sakoji_smile_map,
        .data_size = sizeof(anime_sakoji_smile_map),
};

/* ======== 角色1: 丰川祥子头像 (32x32) ======== */

const uint8_t anime_sakoji_avatar_map[] = {
        /* TODO: 32x32小头像 */
        0xFF, 0xEC,
        /* ... */
};

const lv_img_dsc_t anime_sakoji_avatar = {
        .header.always_zero = 0,
        .header.w = 32,
        .header.h = 32,
        .header.cf = LV_IMG_CF_TRUE_COLOR,
        .data = anime_sakoji_avatar_map,
        .data_size = sizeof(anime_sakoji_avatar_map),
};

/* ======== 角色2: 椎名立希 (80x120) ======== */

const uint8_t anime_taki_main_map[] = {
        /* TODO: 银灰短发高冷键盘手 */
        0xE0, 0xE0,  /* 银灰色 */
        /* ... */
};

const lv_img_dsc_t anime_taki_main = {
        .header.always_zero = 0,
        .header.w = 80,
        .header.h = 120,
        .header.cf = LV_IMG_CF_TRUE_COLOR,
        .data = anime_taki_main_map,
        .data_size = sizeof(anime_taki_main_map),
};

/* ======== 角色2: 椎名立希头像 (32x32) ======== */

const uint8_t anime_taki_avatar_map[] = {
        /* TODO: 32x32头像 */
};

const lv_img_dsc_t anime_taki_avatar = {
        .header.always_zero = 0,
        .header.w = 32,
        .header.h = 32,
        .header.cf = LV_IMG_CF_TRUE_COLOR,
        .data = anime_taki_avatar_map,
        .data_size = sizeof(anime_taki_avatar_map),
};

/* ======== 角色3: 凛 (Blue Archive) (80x120) ======== */

const uint8_t anime_rin_main_map[] = {
        /* TODO: 蓝色双马尾元气步枪手 */
        0x07, 0xFF,  /* 天蓝色 */
        /* ... */
};

const lv_img_dsc_t anime_rin_main = {
        .header.always_zero = 0,
        .header.w = 80,
        .header.h = 120,
        .header.cf = LV_IMG_CF_TRUE_COLOR,
        .data = anime_rin_main_map,
        .data_size = sizeof(anime_rin_main_map),
};

/* ======== 角色3: 凛头像 (32x32) ======== */

const uint8_t anime_rin_avatar_map[] = {
        /* TODO: 32x32头像 */
};

const lv_img_dsc_t anime_rin_avatar = {
        .header.always_zero = 0,
        .header.w = 32,
        .header.h = 32,
        .header.cf = LV_IMG_CF_TRUE_COLOR,
        .data = anime_rin_avatar_map,
        .data_size = sizeof(anime_rin_avatar_map),
};

/* ==================== 辅助函数实现 ==================== */

const void *anime_get_character_img(enum gui_module_id module_id,
                                    bool is_main)
{
        switch (module_id) {
        case MODULE_MOTOR:
                /* 电机界面 → 丰川祥子 (酷帅=力量感) */
                return is_main ?
                        (const void *)&anime_sakoji_main :
                        (const void *)&anime_sakoji_avatar;

        case MODULE_NTC:
                /* NTC界面 → 椎名立希 (冷静=精确测量) */
                return is_main ?
                        (const void *)&anime_taki_main :
                        (const void *)&anime_taki_avatar;

        case MODULE_TEMP_HUMIDITY:
                /* 温湿度界面 → 凛 (活泼=变化多端) */
                return is_main ?
                        (const void *)&anime_rin_main :
                        (const void *)&anime_rin_avatar;

        case MODULE_RS485:
                /* RS485界面 → 祥子微笑 (通信成功) */
                return is_main ?
                        (const void *)&anime_sakoji_smile :
                        (const void *)&anime_sakoji_avatar;

        case MODULE_CAN:
                /* CAN界面 → 立希认真 (总线严谨) */
                return is_main ?
                        (const void *)&anime_taki_main :
                        (const void *)&anime_taki_avatar;

        default:
                return NULL;
        }
}

void anime_play_click_animation(lv_obj_t *character_img)
{
        /*
         * 动画实现思路:
         *
         * 方案1: 使用LVGL内置动画API
         *   lv_anim_t a;
         *   lv_anim_init(&a);
         *   a.var = character_img;
         *   a.exec_cb = (lv_anim_exec_xcb_t)lv_obj_set_zoom;
         *   a.time = 100;
         *   a.values = {256, 243};  // 100% → 95%
         *   a.path_cb = lv_anim_path_ease_out;
         *   lv_anim_start(&a);
         *
         * 方案2: 定时器切换图片
         *   lv_timer_create(anim_timer_cb, 50, NULL);
         *
         * 当前简化实现: 仅输出日志
         */
        pr_debug_with_tag("ANIME", "Click animation triggered\n");

        /* TODO: 实现完整的缩放+表情切换动画 */
}
