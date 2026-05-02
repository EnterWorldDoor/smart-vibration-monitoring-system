/**
 * @file lcd.c
 * @brief ATK-DMF407 LCD驱动实现 (ILI9341, FSMC 16-bit)
 *
 * 驱动架构:
 *   - FSMC并行接口: 16位数据总线, Bank4
 *   - ILI9341命令集: 寄存器配置+显存写入
 *   - LVGL集成: 提供flush回调接口
 *
 * 性能优化:
 *   - 窗口寻址模式: 减少坐标设置开销
 *   - 批量数据传输: 连续写像素，提高吞吐量
 *   - FSMC时序调优: DataSetupTime=255 (保守值)
 */

#include "lcd.h"
#include "fsmc.h"
#include "gpio.h"
#include "system_log/system_log.h"

/* ==================== ILI9341寄存器定义 ==================== */

#define ILI9341_CMD_NOP                 0x00
#define ILI9341_CMD_SOFT_RESET          0x01
#define ILI9341_CMD_READ_ID4            0xD3    /* 读ID */
#define ILI9341_CMD_DISPLAY_OFF         0x28
#define ILI9341_CMD_DISPLAY_ON          0x29

/* 地址设置 */
#define ILI9341_CMD_COLUMN_ADDR_SET     0x2A    /* CASET */
#define ILI9341_CMD_PAGE_ADDR_SET       0x2B    /* PASET */
#define ILI9341_CMD_MEMORY_WRITE        0x2C    /* RAMWR */

/* 控制参数 */
#define ILI9341_CMD_PIXEL_FORMAT_SET    0x3A    /* COLMOD */
#define ILI9341_CMD_MADCTL              0x36    /* 内存访问控制 */

/* 时序和电源 */
#define ILI9341_CMD_FRAME_RATE_CTRL     0xB1    /* 帧率控制 */
#define ILI9341_CMD_DISPLAY_FUNCTION    0xB6    /* 显示功能控制 */
#define ILI9341_CMD_POWER_CTRL1         0xC0    /* PVGH控制 */
#define ILI9341_CMD_POWER_CTRL2         0xC1    /* PVGL控制 */
#define ILI9341_CMD_VCOM_CTRL1          0xC5    /* VCOM控制1 */
#define ILI9341_CMD_VCOM_CTRL2          0xC7    /* VCOM控制2 */
#define ILI9341_CMD_POWER_CTRLA         0xCB    /* 功率控制A */
#define ILI9341_CMD_POWER_CTRLB         0xCF    /* 功率控制B */

/* Gamma校正 */
#define ILI9341_CMD_POSITIVE_GAMMA      0xE0
#define ILI9341_CMD_NEGATIVE_GAMMA      0xE1

/* 驱动时序控制 */
#define ILI9341_CMD_DRIVER_TIMING_A     0xE8
#define ILI9341_CMD_DRIVER_TIMING_B     0xEA

/* ==================== 模块内部状态 ==================== */

static struct {
        uint16_t current_width;
        uint16_t current_height;
        enum lcd_orientation orientation;
        bool initialized;
} lcd_state = {
        .current_width = LCD_WIDTH,
        .current_height = LCD_HEIGHT,
        .orientation = LCD_ORIENTATION_LANDSCAPE,
        .initialized = false
};

/* ==================== 底层硬件操作 ==================== */

/**
 * lcd_write_cmd - 通过FSMC发送ILI9341命令
 * @cmd: 命令字节
 *
 * FSMC地址线A10=1表示命令周期
 */
static inline void lcd_write_cmd(uint8_t cmd)
{
        LCD_FSMC_CMD_ADDR = cmd;
}

/**
 * lcd_write_data - 通过FSMC发送ILI9341数据
 * @data: 16位数据 (RGB565)
 *
 * FSMC地址线A10=0表示数据周期
 */
static inline void lcd_write_data(uint16_t data)
{
        LCD_FSMC_DATA_ADDR = data;
}

/**
 * lcd_write_data8 - 发送8位数据 (用于初始化参数)
 * @data: 8位数据
 */
static inline void lcd_write_data8(uint8_t data)
{
        LCD_FSMC_DATA_ADDR = data;
}

/**
 * lcd_read_data - 读取LCD数据 (调试用)
 *
 * Return: 16位数据
 */
static inline uint16_t lcd_read_data(void)
{
        return LCD_FSMC_DATA_ADDR;
}

/* ==================== ILI9341初始化序列 ==================== */

/**
 * ili9341_init_sequence - ILI9341标准初始化流程
 *
 * 初始化步骤:
 *   1. 软复位
 *   2. 退出睡眠模式
 *   3. 配置像素格式 (RGB565, 16位)
 *   4. 设置内存访问方向 (MADCTL)
 *   5. 配置帧率和时序
 *   6. 电源管理配置
 *   7. Gamma校正
 *   8. 开启显示
 *
 * 参考ILI9341 datasheet (v1.11) Section 8.3
 */
static void ili9341_init_sequence(void)
{
        HAL_Delay(100);  /* 等待LCD上电稳定 */

        /* Step 1: 软复位 */
        lcd_write_cmd(ILI9341_CMD_SOFT_RESET);
        HAL_Delay(50);

        /* Step 2: 退出睡眠模式 */
        lcd_write_cmd(0x11);  /* SLPOUT */
        HAL_Delay(120);

        /* Step 3: 像素格式设置 - RGB565 (16位/像素) */
        lcd_write_cmd(ILI9341_CMD_PIXEL_FORMAT_SET);
        lcd_write_data8(0x55);  /* 16-bit/pixel */
        HAL_Delay(10);

        /* Step 4: 内存访问控制 (MADCTL) */
        /*
         * Bit[7]: MY  (行地址顺序)  0=从顶到底
         * Bit[6]: MX  (列地址顺序)  0=从左到右
         * Bit[5]: MV  (行列交换)     0=正常
         * Bit[4]: ML  (垂直刷新顺序) 0=自上而下
         * Bit[3]: BGR (颜色顺序)     1=RGB顺序
         * Bit[2]: MH  (水平刷新顺序) 0=自左而右
         */
        lcd_write_cmd(ILI9341_CMD_MADCTL);
        lcd_write_data8(0x08);  /* BGR模式 */
        HAL_Delay(10);

        /* Step 5: 帧率控制 (Framerate) */
        lcd_write_cmd(ILI9341_CMD_FRAME_RATE_CTRL);
        lcd_write_data8(0x00);
        lcd_write_data8(0x18);  /* 79Hz帧率 */

        /* Step 6: 显示功能控制 */
        lcd_write_cmd(ILI9341_CMD_DISPLAY_FUNCTION);
        lcd_write_data8(0x08);
        lcd_write_data8(0x82);  /* PTDE=1, GON=1 */
        lcd_write_data8(0x27);

        /* Step 7: 电源控制 */
        /* Power Control 1 (GVDD = 4.6V) */
        lcd_write_cmd(ILI9341_CMD_POWER_CTRL1);
        lcd_write_data8(0x23);  /* VRH[5:0] */

        /* Power Control 2 (VCI1 = VCI x 0.73) */
        lcd_write_cmd(ILI9341_CMD_POWER_CTRL2);
        lcd_write_data8(0x10);  /* SAP[2:0], BT[3:0] */

        /* VCOM Control 1 (VCOMH = 4.025V, VCOML = -0.95V) */
        lcd_write_cmd(ILI9341_CMD_VCOM_CTRL1);
        lcd_write_data8(0x3E);  /* VMH[6:0] */
        lcd_write_data8(0x28);  /* VML[6:0] */

        /* VCOM Control 2 (VMF = 60) */
        lcd_write_cmd(ILI9341_CMD_VCOM_CTRL2);
        lcd_write_data8(0x86);

        /* Power Control A (AVDD=6.6V, AVCL=-4.4V, VDS=2.31V) */
        lcd_write_cmd(ILI9341_CMD_POWER_CTRLA);
        lcd_write_data8(0x39);
        lcd_write_data8(0x2C);
        lcd_write_data8(0x00);
        lcd_write_data8(0x34);
        lcd_write_data8(0x02);

        /* Power Control B */
        lcd_write_cmd(ILI9341_CMD_POWER_CTRLB);
        lcd_write_data8(0x00);
        lcd_write_data8(0xC1);
        lcd_write_data8(0x30);

        /* Driver Timing Control A */
        lcd_write_cmd(ILI9341_CMD_DRIVER_TIMING_A);
        lcd_write_data8(0x85);
        lcd_write_data8(0x01);
        lcd_write_data8(0x78);

        /* Driver Timing Control B */
        lcd_write_cmd(ILI9341_CMD_DRIVER_TIMING_B);
        lcd_write_data8(0x00);
        lcd_write_data8(0x00);

        /* Step 8: Gamma校正 (Positive) */
        lcd_write_cmd(ILI9341_CMD_POSITIVE_GAMMA);
        lcd_write_data8(0x0F);
        lcd_write_data8(0x31);
        lcd_write_data8(0x2B);
        lcd_write_data8(0x0C);
        lcd_write_data8(0x0E);
        lcd_write_data8(0x08);
        lcd_write_data8(0x4E);
        lcd_write_data8(0xF1);
        lcd_write_data8(0x37);
        lcd_write_data8(0x07);
        lcd_write_data8(0x10);
        lcd_write_data8(0x03);
        lcd_write_data8(0x0E);
        lcd_write_data8(0x09);
        lcd_write_data8(0x00);

        /* Gamma校正 (Negative) */
        lcd_write_cmd(ILI9341_CMD_NEGATIVE_GAMMA);
        lcd_write_data8(0x00);
        lcd_write_data8(0x0E);
        lcd_write_data8(0x14);
        lcd_write_data8(0x03);
        lcd_write_data8(0x11);
        lcd_write_data8(0x07);
        lcd_write_data8(0x31);
        lcd_write_data8(0xC1);
        lcd_write_data8(0x48);
        lcd_write_data8(0x08);
        lcd_write_data8(0x0F);
        lcd_write_data8(0x0C);
        lcd_write_data8(0x31);
        lcd_write_data8(0x36);
        lcd_write_data8(0x0F);

        /* Step 9: 开启显示 */
        lcd_write_cmd(ILI9341_CMD_DISPLAY_ON);
        HAL_Delay(10);

        pr_info_with_tag("LCD", "ILI9341 initialization sequence completed\n");
}

/* ==================== 公开API实现 ==================== */

int bsp_lcd_init(void)
{
        if (lcd_state.initialized) {
                pr_warn_with_tag("LCD", "Already initialized\n");
                return 0;
        }

        /*
         * 确保FSMC已初始化 (在MX_FSMC_Init中完成GPIO配置)
         * 这里只做LCD芯片级初始化
         */
        if (!__HAL_RCC_FSMC_IS_CLK_ENABLED()) {
                __HAL_RCC_FSMC_CLK_ENABLE();
        }

        /* 复位LCD模块 (如果硬件支持) */
        /* 注意: ATK-DMF407的LCD复位引脚可能连接到开发板复位电路 */

        /* 执行ILI9341初始化序列 */
        ili9341_init_sequence();

        /* 设置默认横屏模式 (平板APP风格) */
        bsp_lcd_set_orientation(LCD_ORIENTATION_LANDSCAPE);

        /* 清屏为浅灰色背景 */
        bsp_lcd_fill_screen(LCD_COLOR_BG_LIGHT);

        /* 开启背光 (100%亮度) */
        bsp_lcd_set_backlight(100);

        lcd_state.initialized = true;

        pr_info_with_tag("LCD", "LCD initialized: %dx%d, orientation=%d\n",
                         lcd_state.current_width,
                         lcd_state.current_height,
                         lcd_state.orientation);

        return 0;
}

int bsp_lcd_deinit(void)
{
        if (!lcd_state.initialized) {
                return -ENODEV;
        }

        /* 关闭显示 */
        lcd_write_cmd(ILI9341_CMD_DISPLAY_OFF);

        /* 关闭背光 */
        bsp_lcd_set_backlight(0);

        lcd_state.initialized = false;

        pr_info_with_tag("LCD", "LCD deinitialized\n");
        return 0;
}

int bsp_lcd_set_orientation(enum lcd_orientation orient)
{
        if (orient > LCD_ORIENTATION_LANDSCAPE_INV) {
                return -EINVAL;
        }

        uint8_t madctl_val = 0x08;  /* 默认BGR模式 */

        switch (orient) {
        case LCD_ORIENTATION_PORTRAIT:
                /* 竖屏: 240x320 */
                madctl_val |= 0x00;  /* MY=0, MX=0, MV=0 */
                lcd_state.current_width = 240;
                lcd_state.current_height = 320;
                break;

        case LCD_ORIENTATION_LANDSCAPE:
                /* 横屏90°: 320x240 (推荐) */
                madctl_val |= 0x20;  /* MV=1 (行列交换) */
                madctl_val |= 0x40;  /* MX=1 (列反向) */
                lcd_state.current_width = 320;
                lcd_state.current_height = 240;
                break;

        case LCD_ORIENTATION_PORTRAIT_INV:
                /* 竖屏180°: 240x320 */
                madctl_val |= 0xC0;  /* MY=1, MX=1 */
                lcd_state.current_width = 240;
                lcd_state.current_height = 320;
                break;

        case LCD_ORIENTATION_LANDSCAPE_INV:
                /* 横屏270°: 320x240 */
                madctl_val |= 0x80;  /* MY=1 */
                madctl_val |= 0x20;  /* MV=1 */
                lcd_state.current_width = 320;
                lcd_state.current_height = 240;
                break;

        default:
                return -EINVAL;
        }

        lcd_write_cmd(ILI9341_CMD_MADCTL);
        lcd_write_data8(madctl_val);

        lcd_state.orientation = orient;

        pr_debug_with_tag("LCD", "Orientation set to %d (%dx%d)\n",
                          orient,
                          lcd_state.current_width,
                          lcd_state.current_height);

        return 0;
}

uint16_t bsp_lcd_get_width(void)
{
        return lcd_state.current_width;
}

uint16_t bsp_lcd_get_height(void)
{
        return lcd_state.current_height;
}

void bsp_lcd_fill_screen(uint16_t color)
{
        bsp_lcd_set_window(0, 0,
                           lcd_state.current_width,
                           lcd_state.current_height);

        uint32_t total_pixels = (uint32_t)lcd_state.current_width *
                                lcd_state.current_height;

        for (uint32_t i = 0; i < total_pixels; i++) {
                lcd_write_data(color);
        }
}

void bsp_lcd_draw_pixel(uint16_t x, uint16_t y, uint16_t color)
{
        if ((x >= lcd_state.current_width) ||
            (y >= lcd_state.current_height)) {
                return;
        }

        bsp_lcd_set_window(x, y, 1, 1);
        lcd_write_data(color);
}

void bsp_lcd_fill_rect(uint16_t x, uint16_t y,
                       uint16_t w, uint16_t h,
                       uint16_t color)
{
        if ((x >= lcd_state.current_width) ||
            (y >= lcd_state.current_height)) {
                return;
        }

        /* 边界裁剪 */
        if ((x + w) > lcd_state.current_width) {
                w = lcd_state.current_width - x;
        }
        if ((y + h) > lcd_state.current_height) {
                h = lcd_state.current_height - y;
        }

        bsp_lcd_set_window(x, y, w, h);

        uint32_t total_pixels = (uint32_t)w * h;
        for (uint32_t i = 0; i < total_pixels; i++) {
                lcd_write_data(color);
        }
}

void bsp_lcd_set_window(uint16_t x, uint16_t y,
                        uint16_t w, uint16_t h)
{
        /* 设置列地址 (CASET) */
        lcd_write_cmd(ILI9341_CMD_COLUMN_ADDR_SET);
        lcd_write_data8((x >> 8) & 0xFF);
        lcd_write_data8(x & 0xFF);
        lcd_write_data8(((x + w - 1) >> 8) & 0xFF);
        lcd_write_data8((x + w - 1) & 0xFF);

        /* 设置行地址 (PASET) */
        lcd_write_cmd(ILI9341_CMD_PAGE_ADDR_SET);
        lcd_write_data8((y >> 8) & 0xFF);
        lcd_write_data8(y & 0xFF);
        lcd_write_data8(((y + h - 1) >> 8) & 0xFF);
        lcd_write_data8((y + h - 1) & 0xFF);

        /* 准备写入显存 (RAMWR) */
        lcd_write_cmd(ILI9341_CMD_MEMORY_WRITE);
}

void bsp_lcd_write_data_batch(const uint16_t *data, uint32_t len)
{
        for (uint32_t i = 0; i < len; i++) {
                LCD_FSMC_DATA_ADDR = data[i];
        }
}

void bsp_lcd_set_backlight(uint8_t brightness)
{
        /*
         * 背光控制方式 (根据硬件设计选择):
         * 方案1: GPIO直接控制 (高电平亮/低电平灭)
         * 方案2: TIMx PWM控制 (可调亮度)
         *
         * ATK-DMF407通常使用GPIO控制或PWM
         * 这里使用简化实现: GPIO PB0 或 TIM4_CH2
         */

        if (brightness == 0) {
                /* 关闭背光 */
                HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET);
        } else if (brightness >= 100) {
                /* 最大亮度 */
                HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_SET);
        } else {
                /* TODO: 如果需要PWM调光，在这里添加TIMx PWM代码 */
                /* 当前简化为全亮 */
                HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_SET);
        }
}

void bsp_lcd_on(void)
{
        lcd_write_cmd(ILI9341_CMD_DISPLAY_ON);
        bsp_lcd_set_backlight(100);

        pr_debug_with_tag("LCD", "Display ON\n");
}

void bsp_lcd_off(void)
{
        bsp_lcd_set_backlight(0);
        lcd_write_cmd(ILI9341_CMD_DISPLAY_OFF);

        pr_debug_with_tag("LCD", "Display OFF (sleep mode)\n");
}
