/**
 * @file anime_characters.h
 * @brief 二次元御姐立绘图片资源 (LVGL C数组格式)
 *
 * 角色列表:
 *   1. 丰川祥子 (Sakiko Tomochin) - MyGO!!!!! 酷帅吉他手
 *      - 发型: 黑色长直发/双马尾
 *      - 服装: 黑色摇滚制服
 *      - 表情: 酷帅/微笑 (两种状态)
 *      - 尺寸: 80x120 像素
 *
 *   2. 椎名立希 (Taki Shiina) - BanG Dream! 高冷键盘手
 *      - 发型: 银灰色短发
 *      - 服装: 白色校服
 *      - 表情: 冷淡/认真
 *      - 尺寸: 32x32 像素 (头像)
 *
 *   3. 凛 (Rin) - Blue Archive 元气步枪手
 *      - 发型: 蓝色双马尾
 *      - 服装: 军装风格
 *      - 表情: 开心/元气
 *      - 尺寸: 32x32 像素 (头像)
 *
 * 图片格式:
 *   - LVGL CF_INDEXED_1BIT (单色位图，节省Flash)
 *   - 或 LVGL CF_TRUE_COLOR (RGB565真彩色)
 *
 * 使用方法:
 *   1. 使用 LVGL Image Converter 工具将PNG转换为C数组
 *   2. 替换下方的占位符数据
 *   3. 在GUI代码中使用 lv_img_set_src() 加载
 */

#ifndef __ANIME_CHARACTERS_H
#define __ANIME_CHARACTERS_H

#include "lvgl.h"
#include "gui_app.h"                 /* enum gui_module_id definition */

/* ==================== 图片声明宏 ==================== */

/*
 * LV_IMG_DECLARE 宏用于声明外部定义的图片数据
 * 它会创建一个 lv_img_dsc_t 类型的外部变量引用
 */

/* ==================== 角色1: 丰川祥子 (主立绘) ==================== */

/**
 * anime_sakoji_main - 丰川祥子主立绘 (80x120像素)
 *
 * 位置: 主界面右下角常驻
 * 风格: 日系简约线条，低饱和度配色
 * 背景: 透明 (Alpha通道)
 *
 * 推荐图片来源:
 *   - MyGO!!!!! 官方立绘
 *   - Pixiv 搜索 "豊川祥子 立ち絵"
 *   - 自行绘制 (使用Clip Studio Paint/SAI)
 */
LV_IMG_DECLARE(anime_sakoji_main);

/**
 * anime_sakoji_smile - 丰川祥子微笑状态 (80x120像素)
 *
 * 用途: 点击卡片时的动画帧
 * 变化: 嘴角上扬，眼睛微眯
 */
LV_IMG_DECLARE(anime_sakoji_smile);

/**
 * anime_sakoji_avatar - 丰川祥子头像 (32x32像素)
 *
 * 位置: 二级界面标题栏右侧
 * 用途: 装饰性小图标
 */
LV_IMG_DECLARE(anime_sakoji_avatar);

/* ==================== 角色2: 椎名立希 ==================== */

/**
 * anime_taki_main - 椎名立希主立绘 (80x120像素)
 *
 * 特征: 银灰短发，白色校服，高冷气质
 * 适用界面: NTC温度采集界面
 */
LV_IMG_DECLARE(anime_taki_main);

/**
 * anime_taki_avatar - 椎名立希头像 (32x32像素)
 */
LV_IMG_DECLARE(anime_taki_avatar);

/* ==================== 角色3: 凛 (Blue Archive) ==================== */

/**
 * anime_rin_main - 凛主立绘 (80x120像素)
 *
 * 特征: 蓝色双马尾，军装风格，元气表情
 * 适用界面: 温湿度传感器界面
 */
LV_IMG_DECLARE(anime_rin_main);

/**
 * anime_rin_avatar - 凛头像 (32x32像素)
 */
LV_IMG_DECLARE(anime_rin_avatar);

/* ==================== 辅助函数 ==================== */

/**
 * anime_get_character_img - 根据模块ID获取对应角色图片
 * @module_id: 模块ID (enum gui_module_id)
 * @is_main: true=返回主立绘(80x120), false=返回头像(32x32)
 *
 * Return: lv_img_dsc_t指针, NULL表示无效模块
 *
 * 角色分配策略:
 *   MODULE_MOTOR       → 丰川祥子 (酷帅=电机力量感)
 *   MODULE_NTC         → 椎名立希 (冷静=温度精确测量)
 *   MODULE_TEMP_HUMIDITY → 凛 (活泼=温湿度变化)
 *   MODULE_RS485       → 丰川祥子微笑 (通信成功)
 *   MODULE_CAN         → 椎名立希认真 (总线严谨)
 */
const void *anime_get_character_img(enum gui_module_id module_id,
                                    bool is_main);

/**
 * anime_play_click_animation - 播放点击卡片动画
 * @character_img: 当前显示的图片对象
 *
 * 动画效果:
 *   1. 缩放至95% (100ms)
 *   2. 切换为微笑表情 (可选)
 *   3. 恢复原始大小 (100ms)
 *   4. 闪烁效果 (透明度变化)
 */
void anime_play_click_animation(lv_obj_t *character_img);

#endif /* __ANIME_CHARACTERS_H */
