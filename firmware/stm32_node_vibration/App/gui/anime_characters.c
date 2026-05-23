/**
 * @file anime_characters.c
 * @brief 二次元御姐立绘图片数据实现
 *
 * 图片数据来源:
 *   - 原始AI生成图 → resize_images.py 下采样 → 80x120 RGB565
 *   - 每张主立绘约19KB Flash, 每张头像约2KB Flash
 *   - 总计: 3张主立绘 + 3张头像 ≈ 63KB Flash占用
 *
 * 编译方式:
 *   将此文件与 picture/*.c 一起编译
 *   或直接在工程中分别添加 picture/*.c 源文件
 */

#include "anime_characters.h"
#include "system_log/system_log.h"

/*
 * ======== 图片数据 (编译自 picture/ 目录下的 C 文件) ========
 *
 * 如果使用 CubeIDE/Keil, 将以下文件加入工程源文件列表:
 *   App/gui/picture/anime_sakoji_main.c
 *   App/gui/picture/anime_sakoji_avatar.c
 *   App/gui/picture/anime_taki_main.c
 *   App/gui/picture/anime_taki_avatar.c
 *   App/gui/picture/anime_rin_main.c
 *   App/gui/picture/anime_rin_avatar.c
 *
 * extern 声明已在 anime_characters.h 中完成
 */

/* ==================== 辅助函数实现 ==================== */

const void *anime_get_character_img(enum gui_module_id module_id,
                                    bool is_main)
{
        const void *result = NULL;

        switch (module_id) {
        case MODULE_MOTOR:
                result = is_main ?
                        (const void *)&anime_taki_main :
                        (const void *)&anime_taki_avatar;
                break;

        case MODULE_NTC:
                result = is_main ?
                        (const void *)&anime_taki_main :
                        (const void *)&anime_taki_avatar;
                break;

        case MODULE_TEMP_HUMIDITY:
                result = is_main ?
                        (const void *)&anime_rin_main :
                        (const void *)&anime_rin_avatar;
                break;

        case MODULE_RS485:
                result = is_main ?
                        (const void *)&anime_rin_main :
                        (const void *)&anime_rin_avatar;
                break;

        case MODULE_CAN:
                result = is_main ?
                        (const void *)&anime_sakoji_main :
                        (const void *)&anime_sakoji_avatar;
                break;

        default:
                pr_warn_with_tag("ANIME", "Unknown module_id=%d, "
                                 "returning NULL image\n",
                                 (int)module_id);
                result = NULL;
                break;
        }

        pr_debug_with_tag("ANIME",
                          "get_character_img: module=%d is_main=%d "
                          "addr=0x%p\n",
                          (int)module_id, (int)is_main, result);

        return result;
}

void anime_play_click_animation(lv_obj_t *character_img)
{
        if (!character_img) {
                pr_warn_with_tag("ANIME",
                                 "play_click_animation: NULL image\n");
                return;
        }

        pr_debug_with_tag("ANIME", "Click animation triggered\n");

        lv_anim_t a;

        lv_anim_init(&a);
        lv_anim_set_var(&a, character_img);
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_img_set_zoom);
        lv_anim_set_values(&a, LV_IMG_ZOOM_NONE, 243);
        lv_anim_set_time(&a, 100);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);

        lv_anim_set_values(&a, 243, LV_IMG_ZOOM_NONE);
        lv_anim_set_time(&a, 100);
        lv_anim_set_ready_cb(&a, NULL);
        lv_anim_start(&a);

        pr_debug_with_tag("ANIME",
                          "Click animation completed (95%% zoom bounce)\n");
}
