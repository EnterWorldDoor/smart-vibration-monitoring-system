# 🎨 二次元立绘图片转换指南 (LVGL C数组格式)

## 📋 快速开始

### **步骤1: 准备图片素材**

#### 推荐图片来源

| 角色 | 推荐搜索关键词 | 图片尺寸 | 风格要求 |
|------|--------------|---------|---------|
| **丰川祥子** | `豊川祥子 立ち絵` / `Sakiko Tomochin fanart` | 80×120px | 黑色长发/双马尾，摇滚制服，酷帅表情 |
| **椎名立希** | `椎名立希 立ち絵` / `Taki Shiina` | 80×120px | 银灰短发，白色校服，高冷气质 |
| **凛** | `凛 ブルアカ 立ち絵` / `Rin Blue Archive` | 80×120px | 蓝色双马尾，军装风格，元气表情 |

#### 图片要求

```
✅ 必须满足:
   - 格式: PNG (支持透明通道)
   - 背景: 透明 (Alpha=0)
   - 尺寸: 80×120像素 (主立绘) 或 32×32像素 (头像)
   - 色彩模式: RGB或RGBA
   - 文件大小: <100KB (优化后)

❌ 禁止:
   - JPEG格式 (无透明通道)
   - 白色/彩色背景 (必须透明)
   - 过大分辨率 (>200×200)
   - 复杂渐变 (增加文件大小)
```

---

### **步骤2: 使用LVGL Image Converter转换**

#### 方法A: 在线工具 (推荐新手)

1. 打开浏览器访问:
   ```
   https://lvgl.io/tools/imageconverter
   ```

2. 上传PNG图片

3. 配置转换选项:
   ```
   □ Color format: [RGB565]        ← 选择此项 (节省内存)
     其他选项:
       - RGB888 (真彩色, 3字节/像素, 大)
       - L8 (灰度, 不适合彩色立绘)
       - A8 (仅Alpha通道, 不适用)
       - Indexed (索引颜色, 需要调色板)

   □ Output format: [C array]      ← 生成C代码

   ☑ Add magic to header           ← 勾选 (自动检测文件类型)
   ```

4. 点击 **Convert** 按钮

5. 下载生成的 `.c` 文件

#### 方法B: 命令行工具 (推荐批量处理)

```bash
# 安装Python工具
pip install lv_img_converter

# 转换单个图片
lv_img_converter.py sakoji_main.png \
    --output anime_sakoji_main.c \
    --format rgb565 \
    --size 80x120

# 批量转换所有角色
for img in *.png; do
    name=$(basename "$img" .png)
    lv_img_converter.py "$img" \
        --output "anime_${name}.c" \
        --format rgb565
done
```

#### 方法C: Node.js工具

```bash
# 安装
npm install -g lv_img

# 使用
lv_img convert sakoji.png -f rgb565 -o sakoji.c
```

---

### **步骤3: 替换占位符数据**

#### 生成的C文件内容示例

```c
/* ======== LVGL Image Converter 自动生成 ======== */

#include "lvgl.h"

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

const LV_ATTRIBUTE_MEM_ALIGN uint8_t anime_sakoji_main_map[] = {
    /* Pixel data in RGB565 format */
    0xF8, 0x00,  /* 第1个像素 */
    0xF8, 0x00,  /* 第2个像素 */
    0xFF, 0xEC,  /* 第3个像素 (粉色) */
    /* ... 共19200字节 (80×120×2) ... */
};

const lv_img_dsc_t anime_sakoji_main = {
    .header_cf = LV_IMG_CF_TRUE_COLOR,
    .header_always_zero = 0,
    .header_w = 80,
    .header_h = 120,
    .data_size = 19200,  /* 80 * 120 * 2 */
    .data = anime_sakoji_main_map,
};
```

#### 替换到项目中

```bash
# 1. 将生成的.c文件复制到项目目录
cp sakoji_main.c d:/smartSystem/firmware/stm32_node_vibration/app/gui/

# 2. 编辑 anime_characters.c
# 删除占位符数据，替换为真实数据:

/* 旧代码 (删除):
const uint8_t anime_sakoji_main_map[] = { 0xFF, 0xEC, ... };
*/

/* 新代码 (粘贴):
#include "sakoji_main.c"  // 或直接复制数据
*/
```

---

### **步骤4: 在GUI中使用图片**

#### 在gui_app.c中加载立绘

```c
#include "anime_characters.h"

static lv_obj_t *create_anime_character(lv_obj_t *parent)
{
    /*
     * 创建图片控件
     */
    lv_obj_t *img = lv_img_create(parent);
    
    /*
     * 设置图片源 (使用丰川祥子主立绘)
     */
    lv_img_set_src(img, &anime_sakoji_main);
    
    /*
     * 设置大小和位置
     */
    lv_obj_set_size(img, ANIME_CHARACTER_WIDTH, ANIME_CHARACTER_HEIGHT);
    lv_obj_align(img, LV_ALIGN_BOTTOM_RIGHT, -5, -5);
    
    return img;
}

/*
 * 根据当前界面动态切换角色
 */
void update_anime_character(enum gui_module_id current_module)
{
    const void *img_src = anime_get_character_img(current_module, true);
    
    if (img_src && gui.anime_character) {
        lv_img_set_src(gui.anime_character, img_src);
        
        /* 播放切换动画 */
        anime_play_click_animation(gui.anime_character);
    }
}
```

---

## 🔧 高级优化技巧

### **1. 减小Flash占用 (Indexed颜色)**

如果Flash空间紧张，使用**索引颜色**模式：

```
Color format: [Indexed]
Max colors: 256 (8-bit调色板)

优势:
  ✅ 文件大小减少60-70%
  ✅ 适合简单配色立绘
  
劣势:
  ❌ 可能出现色彩失真
  ❌ 渐变效果变差
```

### **2. 外部存储器加载 (SD卡/SPI Flash)**

对于多张高清立绘（>500KB），建议从外部存储器加载：

```c
/* 使用LVGL文件系统 */
lv_fs_file_t f;
lv_fs_open(&f, "S:sakoji.png", LV_FS_MODE_RD);

/* 动态解码 */
lv_img_decoder_open(&decoder, &dsc, src);
lv_img_decoder_read_line(&decoder, &dsc, x, y, buf, len);
lv_img_decoder_close(&decoder, &dsrc);
```

### **3. 动画帧优化**

为点击动画准备多个帧：

```
sakoji_normal.png  → 正常状态
sakoji_smile.png   → 微笑状态 (点击时)
sakoji_blink.png   → 眨眼状态 (随机触发)
sakoji_surprise.png → 惊讶状态 (错误提示)
```

在代码中切换：
```c
void on_card_click(lv_event_t *e) {
    lv_img_set_src(anime_img, &anime_sakoji_smile);
    
    /* 2秒后恢复 */
    lv_timer_create(restore_normal_cb, 2000, NULL);
}
```

---

## 🎯 推荐素材资源

### 免费立绘资源网站

| 网站 | URL | 特点 |
|------|-----|------|
| **Pixiv** | https://www.pixiv.net | 最大的二次元插画社区 |
| **Booru** | https://safebooru.org | 标签化图片搜索引擎 |
| **Zerochan** | https://www.zerochan.net | 动漫角色专区 |

### 搜索标签 (Tag)

```
#豊川祥子 #立ち絵 #MyGO!!!! #バンドリ
#椎名立希 #ガルパ #BanGDreams
#凛 #ブルアカ #BlueArchive #シャーレ
```

### 版权注意

⚠️ **重要**: 
- 个人学习项目: 通常可免费使用
- 商业项目: 需获得作者授权或购买授权
- 推荐使用CC0/Public Domain素材

---

## ❓ 常见问题

### Q1: 转换后图片显示异常？

```
A: 检查以下几点:
   1. Color format是否选择RGB565?
   2. 图片尺寸是否匹配 (80×120)?
   3. 是否添加了magic number?
   4. lv_disp_drv的color_format设置是否一致?
```

### Q2: 编译报错"array too large"?

```
A: 解决方案:
   1. 降低图片分辨率 (80×120→64×96)
   2. 使用Indexed颜色模式
   3. 将数组移到外部SPI Flash
   4. 吭用部分不常用的角色
```

### Q3: 如何制作透明背景？

```
A: 使用图像编辑软件:
   Photoshop: 图层→透明度→删除背景
   GIMP: Layer→Transparency→Add Alpha Channel
   在线工具: https://www.remove.bg
```

### Q4: 多张图片Flash不够用？

```
A: 优化策略:
   1. 仅保留主界面立绘 (1张80×120)
   2. 二级界面用32×32头像代替
   3. 使用Indexed颜色(减少60%大小)
   4. 运行时从SD卡动态加载
```

---

## ✅ 检查清单

完成图片集成前，请确认：

- [ ] PNG图片已准备好 (80×120 + 32×32)
- [ ] 背景已设为透明
- [ ] 已使用LVGL Image Converter转换为C数组
- [ ] 生成的数据已复制到 `anime_characters.c`
- [ ] 占位符数据已删除/注释
- [ ] `gui_app.c` 中已调用 `lv_img_set_src()`
- [ ] 编译无错误、无警告
- [ ] LCD上正确显示立绘图片

---

## 📞 技术支持

如遇到问题，请检查：

1. **LVGL官方文档**
   - Image Converter: https://docs.lvgl.io/latest/tools/imageconverter.html
   - Image Object: https://docs.lvgl.io/latest/widgets/obj/lv_img.html

2. **项目Issue**
   - GitHub Issues: 提交具体错误信息

3. **社区论坛**
   - LVGL Forum: https://forum.lvgl.io/

---

**祝您创建出精美的二次元界面！🎉**
