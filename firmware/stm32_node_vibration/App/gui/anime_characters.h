/**
 * @file anime_characters.h
 * @brief 二次元御姐立绘图片资源声明
 *
 * 角色列表:
 *   1. 丰川祥子 (Sakiko) - MyGO!!!!! 酷帅吉他手
 *      - 尺寸: 80x120 像素 (主立绘) / 32x32 像素 (头像)
 *
 *   2. 椎名立希 (Taki) - BanG Dream! 高冷键盘手
 *      - 尺寸: 80x120 像素 (主立绘) / 32x32 像素 (头像)
 *
 *   3. 凛 (Rin) - Blue Archive 元气步枪手
 *      - 尺寸: 80x120 像素 (主立绘) / 32x32 像素 (头像)
 *
 * 图片文件位置:
 *   App/gui/picture/anime_sakoji_main.c  - 丰川祥子主立绘
 *   App/gui/picture/anime_sakoji_avatar.c - 丰川祥子头像
 *   App/gui/picture/anime_taki_main.c     - 椎名立希主立绘
 *   App/gui/picture/anime_taki_avatar.c   - 椎名立希头像
 *   App/gui/picture/anime_rin_main.c      - 凛主立绘
 *   App/gui/picture/anime_rin_avatar.c    - 凛头像
 */

#ifndef __ANIME_CHARACTERS_H
#define __ANIME_CHARACTERS_H

#include "../../lvgl.h"
#include "gui_app.h"

/* ==================== 角色0: 御姐立绘 (主立绘 80x120, 程序生成) ==================== */

extern const lv_img_dsc_t anime_oneesan_main;

/* ==================== 角色0: 御姐 (头像 32x32) ==================== */

extern const lv_img_dsc_t anime_oneesan_avatar;

/* ==================== 角色1: 丰川祥子 (主立绘 80x120) ==================== */

extern const lv_img_dsc_t anime_sakoji_main;

/* ==================== 角色1: 丰川祥子 (头像 32x32) ==================== */

extern const lv_img_dsc_t anime_sakoji_avatar;

/* ==================== 角色2: 椎名立希 (主立绘 80x120) ==================== */

extern const lv_img_dsc_t anime_taki_main;

/* ==================== 角色2: 椎名立希 (头像 32x32) ==================== */

extern const lv_img_dsc_t anime_taki_avatar;

/* ==================== 角色3: 凛 (主立绘 80x120) ==================== */

extern const lv_img_dsc_t anime_rin_main;

/* ==================== 角色3: 凛 (头像 32x32) ==================== */

extern const lv_img_dsc_t anime_rin_avatar;

/* ==================== 辅助函数 ==================== */

/**
 * anime_get_character_img - 根据模块ID获取对应角色图片
 * @module_id: 模块ID
 * @is_main: true=返回主立绘(80x120), false=返回头像(32x32)
 *
 * Return: lv_img_dsc_t指针, NULL表示无效模块
 */
const void *anime_get_character_img(enum gui_module_id module_id,
                                    bool is_main);

/**
 * anime_play_click_animation - 播放点击卡片动画
 * @character_img: 当前显示的图片对象
 */
void anime_play_click_animation(lv_obj_t *character_img);

#endif /* __ANIME_CHARACTERS_H */
