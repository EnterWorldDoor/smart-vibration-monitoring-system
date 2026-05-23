/**
 * @file lcd.c
 * @brief ATK-DMF407 LCD驱动实现 (ILI9341 / ST7789V 自动检测, FSMC 16-bit)
 *
 * 驱动架构:
 *   - FSMC并行接口: 16位数据总线, Bank4
 *   - 自动检测LCD控制器型号 (ILI9341 vs ST7789V)
 *   - 对应控制器初始化序列
 *   - LVGL集成: 提供flush回调接口
 */

#include "lcd.h"
#include "../../lvgl.h"
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

/* ==================== ST7789V寄存器定义 ==================== */

/* ST7789V通用命令 (与ILI9341兼容) */
#define ST7789_CMD_NOP                  0x00
#define ST7789_CMD_SOFT_RESET           0x01
#define ST7789_CMD_READ_ID              0x04    /* 读ID (不同于ILI9341的0xD3!) */
#define ST7789_CMD_SLEEP_OUT            0x11
#define ST7789_CMD_DISPLAY_ON           0x29
#define ST7789_CMD_COLUMN_ADDR_SET      0x2A    /* CASET */
#define ST7789_CMD_ROW_ADDR_SET         0x2B    /* PASET */
#define ST7789_CMD_MEMORY_WRITE         0x2C    /* RAMWR */
#define ST7789_CMD_MADCTL               0x36    /* 内存访问控制 */
#define ST7789_CMD_PIXEL_FORMAT_SET     0x3A    /* COLMOD */

/* ST7789V专用寄存器 */
#define ST7789_CMD_PORCH_CTRL           0xB2    /* 前后廊控制 */
#define ST7789_CMD_GATE_CTRL            0xB7    /* 栅极控制 */
#define ST7789_CMD_VCOM_SET             0xBB    /* VCOM设置 */
#define ST7789_CMD_LCM_CTRL             0xC0    /* LCM控制 */
#define ST7789_CMD_VDV_VRH_EN           0xC2    /* VDV/VRH使能 */
#define ST7789_CMD_VRH_SET              0xC3    /* VRH设置 */
#define ST7789_CMD_VDV_SET              0xC4    /* VDV设置 */
#define ST7789_CMD_FRAME_RATE_CTRL      0xC6    /* 帧率控制 */
#define ST7789_CMD_POWER_CTRL1          0xD0    /* 电源控制1 */
#define ST7789_CMD_POSITIVE_GAMMA       0xE0    /* 正伽马校正 */
#define ST7789_CMD_NEGATIVE_GAMMA       0xE1    /* 负伽马校正 */
#define ST7789_CMD_INVERSION_ON         0x21    /* 显示反相开 */

/* ==================== 控制器类型 ==================== */

enum lcd_controller {
        LCD_CTRL_UNKNOWN = 0,
        LCD_CTRL_ILI9341,
        LCD_CTRL_ST7789V
};

/* ==================== 模块内部状态 ==================== */

static struct {
        uint16_t current_width;
        uint16_t current_height;
        enum lcd_orientation orientation;
        enum lcd_controller controller;
        bool initialized;
} lcd_state = {
        .current_width = LCD_WIDTH,
        .current_height = LCD_HEIGHT,
        .orientation = LCD_ORIENTATION_LANDSCAPE,
        .controller = LCD_CTRL_UNKNOWN,
        .initialized = false
};

/* ==================== 底层硬件操作 ==================== */

/**
 * lcd_write_cmd - 通过FSMC发送ILI9341命令字节
 * @cmd: 命令操作码 (ILI9341寄存器索引)
 *
 * RS=0 (A10=0): 写入ILI9341索引寄存器
 */
static inline void lcd_write_cmd(uint8_t cmd)
{
        LCD_FSMC_CMD_ADDR = (uint16_t)cmd;
}

/**
 * lcd_write_data - 通过FSMC发送ILI9341数据 (16-bit)
 * @data: 16位数据 (RGB565像素值)
 *
 * RS=1 (A10=1): 写入ILI9341控制寄存器/GRAM
 */
static inline void lcd_write_data(uint16_t data)
{
        LCD_FSMC_DATA_ADDR = data;
}

/**
 * lcd_write_data8 - 发送8位数据 (用于初始化参数)
 * @data: 8位参数数据
 *
 * RS=1 (A10=1): 写入ILI9341控制寄存器
 */
static inline void lcd_write_data8(uint8_t data)
{
        LCD_FSMC_DATA_ADDR = (uint16_t)data;
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

/*
 * NOP-based delay for use before FreeRTOS scheduler starts.
 *
 * At 168MHz, the loop (NOP + SUBS + BNE) takes ~4 cycles = ~24ns.
 * 1ms = 1,000,000ns / 24ns ≈ 41,667 iterations.
 * Using 42,000 for safe margin (accounts for compiler -O2/-Os variations).
 */
static inline void lcd_delay_ms(uint32_t ms)
{
        volatile uint32_t count = ms * 42000;
        while (count--) {
                __NOP();
        }
}

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
        /*
         * 硬件复位: LCD RST引脚连接至MCU NRST (复位按钮)
         * MCU复位时LCD同步复位，无需软件控制GPIO。
         * PG15实际为DS18B20 1-Wire数据线，不可在此操作。
         */
        lcd_delay_ms(120);  /* 等待LCD内部电源和振荡器稳定 */

        /* Step 1: 软复位 (确保寄存器回到默认值) */
        lcd_write_cmd(ILI9341_CMD_SOFT_RESET);
        lcd_delay_ms(50);

        /* Step 2: 退出睡眠模式 */
        lcd_write_cmd(0x11);  /* SLPOUT */
        lcd_delay_ms(120);

        /* Step 3: 像素格式设置 - RGB565 (16位/像素) */
        lcd_write_cmd(ILI9341_CMD_PIXEL_FORMAT_SET);
        lcd_write_data8(0x55);  /* 16-bit/pixel */
        lcd_delay_ms(10);

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
        lcd_write_data8(0x00);  /* BGR=0, 配合LV_COLOR_16_SWAP */
        lcd_delay_ms(10);

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
        lcd_delay_ms(10);

        pr_info_with_tag("LCD", "ILI9341 initialization sequence completed\n");
}

/**
 * st7789v_init_sequence - ST7789V标准初始化流程
 *
 * 与ILI9341的主要差异:
 *   - ID读取命令: 0x04 vs 0xD3
 *   - 栅极/廊/VCOM控制: 不同的寄存器映射
 *   - Gamma校正: 14组参数 vs 15组
 *   - 电源管理: 简化的LCM+VDV控制
 *   - 需要开启显示反相 (INVON)
 *
 * 参考: Sitronix ST7789V2 datasheet v1.0
 */
static void st7789v_init_sequence(void)
{
        /* 供电稳定延时 */
        lcd_delay_ms(5);

        /* Step 1: 软件复位 */
        lcd_write_cmd(ST7789_CMD_SOFT_RESET);
        lcd_delay_ms(150);

        /* Step 2: 退出睡眠模式 */
        lcd_write_cmd(ST7789_CMD_SLEEP_OUT);
        lcd_delay_ms(120);

        /* Step 3: 像素格式设置 - RGB565 */
        lcd_write_cmd(ST7789_CMD_PIXEL_FORMAT_SET);
        lcd_write_data8(0x55);  /* 16-bit/pixel */
        lcd_delay_ms(10);

        /* Step 4: MADCTL (内存访问控制, 初始值, 稍后由set_orientation覆盖) */
        lcd_write_cmd(ST7789_CMD_MADCTL);
        lcd_write_data8(0x00);
        lcd_delay_ms(10);

        /* Step 5: 前后廊控制 (Porch Control) */
        lcd_write_cmd(ST7789_CMD_PORCH_CTRL);
        lcd_write_data8(0x0C);  /* 正常模式前廊 */
        lcd_write_data8(0x0C);  /* 正常模式后廊 */
        lcd_write_data8(0x00);  /* 空闲模式前廊 */
        lcd_write_data8(0x33);  /* 空闲模式后廊 */
        lcd_write_data8(0x33);  /* 部分模式后廊 */

        /* Step 6: 栅极控制 (Gate Control) */
        lcd_write_cmd(ST7789_CMD_GATE_CTRL);
        lcd_write_data8(0x35);  /* VGH=13.26V, VGL=-10.43V */

        /* Step 7: VCOM设置 */
        lcd_write_cmd(ST7789_CMD_VCOM_SET);
        lcd_write_data8(0x2B);  /* VCOM=0.725V */

        /* Step 8: LCM控制 (Power Control) */
        lcd_write_cmd(ST7789_CMD_LCM_CTRL);
        lcd_write_data8(0x2C);  /* AVDD=6.8V, AVCL=-4.8V */

        /* Step 9: VDV/VRH使能 */
        lcd_write_cmd(ST7789_CMD_VDV_VRH_EN);
        lcd_write_data8(0x01);  /* VDV使能 */
        lcd_write_data8(0xFF);  /* VRH=5.2V */

        /* Step 10: VRH设置 */
        lcd_write_cmd(ST7789_CMD_VRH_SET);
        lcd_write_data8(0x11);

        /* Step 11: VDV设置 */
        lcd_write_cmd(ST7789_CMD_VDV_SET);
        lcd_write_data8(0x20);

        /* Step 12: 帧率控制 - 60Hz */
        lcd_write_cmd(ST7789_CMD_FRAME_RATE_CTRL);
        lcd_write_data8(0x0F);

        /* Step 13: 正伽马校正 (14组参数) */
        lcd_write_cmd(ST7789_CMD_POSITIVE_GAMMA);
        lcd_write_data8(0xD0); lcd_write_data8(0x00); lcd_write_data8(0x05);
        lcd_write_data8(0x0E); lcd_write_data8(0x15); lcd_write_data8(0x0D);
        lcd_write_data8(0x37); lcd_write_data8(0x43); lcd_write_data8(0x47);
        lcd_write_data8(0x09); lcd_write_data8(0x15); lcd_write_data8(0x12);
        lcd_write_data8(0x16); lcd_write_data8(0x19);

        /* Step 14: 负伽马校正 (14组参数) */
        lcd_write_cmd(ST7789_CMD_NEGATIVE_GAMMA);
        lcd_write_data8(0xD0); lcd_write_data8(0x00); lcd_write_data8(0x05);
        lcd_write_data8(0x0D); lcd_write_data8(0x0C); lcd_write_data8(0x06);
        lcd_write_data8(0x2D); lcd_write_data8(0x44); lcd_write_data8(0x40);
        lcd_write_data8(0x0E); lcd_write_data8(0x1C); lcd_write_data8(0x18);
        lcd_write_data8(0x16); lcd_write_data8(0x19);

        /* Step 15: 显示反相开启 */
        lcd_write_cmd(ST7789_CMD_INVERSION_ON);

        /* Step 16: 开启显示 */
        lcd_write_cmd(ST7789_CMD_DISPLAY_ON);
        lcd_delay_ms(20);

        pr_info_with_tag("LCD", "ST7789V initialization sequence completed\n");
}

/* ==================== LCD控制器ID读取 ==================== */

/**
 * bsp_lcd_read_ili9341_id - 读取ILI9341控制器ID (命令 0xD3)
 *
 * Return: 24-bit ID, 读取失败返回0
 */
static uint32_t bsp_lcd_read_ili9341_id(void)
{
        uint8_t id[3] = {0};

        lcd_write_cmd(ILI9341_CMD_READ_ID4);

        (void)(lcd_read_data() & 0xFF);                 /* Dummy */
        id[0] = (uint8_t)(lcd_read_data() & 0xFF);      /* Manufacturer */
        id[1] = (uint8_t)(lcd_read_data() & 0xFF);      /* Version */
        id[2] = (uint8_t)(lcd_read_data() & 0xFF);      /* Module/Driver */

        return ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | id[2];
}

/**
 * bsp_lcd_read_st7789_id - 读取ST7789V控制器ID (命令 0x04)
 *
 * ST7789V Read ID (0x04) 返回:
 *   - Byte 0: 制造商ID (Sitronix = 0x85)
 *   - Byte 1: 模块版本
 *   - Byte 2: 模块ID (ST7789V = 0x52)
 *
 * Return: 24-bit ID, 读取失败返回0
 */
static uint32_t bsp_lcd_read_st7789_id(void)
{
        uint8_t id[3] = {0};

        lcd_write_cmd(ST7789_CMD_READ_ID);

        (void)(lcd_read_data() & 0xFF);                 /* Dummy */
        id[0] = (uint8_t)(lcd_read_data() & 0xFF);      /* Manufacturer */
        id[1] = (uint8_t)(lcd_read_data() & 0xFF);      /* Version */
        id[2] = (uint8_t)(lcd_read_data() & 0xFF);      /* Module/Driver */

        return ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | id[2];
}

/**
 * bsp_lcd_detect_controller - 自动检测LCD控制器型号
 *
 * 优先尝试ILI9341 (0xD3), 若返回0则尝试ST7789V (0x04)。
 * 返回检测到的控制器类型和ID值。
 *
 * @id: 输出参数, 控制器ID值
 * Return: enum lcd_controller
 */
static enum lcd_controller bsp_lcd_detect_controller(uint32_t *id)
{
        uint32_t ili9341_id, st7789_id;

        /* 尝试ILI9341 ID (命令0xD3) */
        ili9341_id = bsp_lcd_read_ili9341_id();
        if (ili9341_id != 0x000000) {
                *id = ili9341_id;
                if (ili9341_id == 0x000093)
                        pr_info_with_tag("LCD", "Detected: ILI9341 (ID=0x%06lX)\n",
                                         (unsigned long)ili9341_id);
                else
                        pr_info_with_tag("LCD", "Detected: ILI9341-compatible (ID=0x%06lX)\n",
                                         (unsigned long)ili9341_id);
                return LCD_CTRL_ILI9341;
        }

        /* 尝试ST7789V ID (命令0x04) */
        st7789_id = bsp_lcd_read_st7789_id();
        if (st7789_id != 0x000000) {
                *id = st7789_id;
                pr_info_with_tag("LCD", "Detected: ST7789V (ID=0x%06lX)\n",
                                 (unsigned long)st7789_id);
                return LCD_CTRL_ST7789V;
        }

        /*
         * 两个控制器ID都读不到 — FSMC读路径可能不工作,
         * 但FSMC写路径可能正常 (由三色诊断验证)。
         * 默认使用ILI9341初始化序列 (更广泛兼容)。
         */
        *id = 0;
        pr_warn_with_tag("LCD", "Cannot read LCD ID (FSMC read may be broken)\n");
        pr_warn_with_tag("LCD", "Defaulting to ILI9341 init sequence\n");
        return LCD_CTRL_UNKNOWN;
}

/* ==================== 公开API实现 ==================== */

int bsp_lcd_init(void)
{
        uint32_t lcd_id;
        enum lcd_controller detected;

        if (lcd_state.initialized) {
                pr_warn_with_tag("LCD", "Already initialized\n");
                return 0;
        }

        if (!__HAL_RCC_FSMC_IS_CLK_ENABLED()) {
                __HAL_RCC_FSMC_CLK_ENABLE();
        }

        /* FSMC使能后总线稳定延时 */
        lcd_delay_ms(10);

        /*
         * Step 1: 自动检测LCD控制器型号
         *
         * 发送ILI9341软复位让控制器进入已知状态后再读ID。
         * 两种控制器都响应0x01 (软复位), 不会造成损坏。
         */
        lcd_write_cmd(0x01);  /* 通用软复位 (ILI9341和ST7789V都支持) */
        lcd_delay_ms(150);

        detected = bsp_lcd_detect_controller(&lcd_id);

        /*
         * Step 2: 执行对应控制器的初始化序列
         */
        switch (detected) {
        case LCD_CTRL_ST7789V:
                st7789v_init_sequence();
                lcd_state.controller = LCD_CTRL_ST7789V;
                break;
        case LCD_CTRL_ILI9341:
        default:
                ili9341_init_sequence();
                lcd_state.controller = LCD_CTRL_ILI9341;
                break;
        }

        /* Step 3: 设置显示方向 */
        bsp_lcd_set_orientation(LCD_ORIENTATION_LANDSCAPE);

        /*
         * Step 4: 三色诊断 — 验证FSMC基本写入
         */
        pr_info_with_tag("LCD", "Color diagnostic: RED...\n");
        bsp_lcd_fill_screen(0xF800);
        lcd_delay_ms(500);

        pr_info_with_tag("LCD", "Color diagnostic: GREEN...\n");
        bsp_lcd_fill_screen(0x07E0);
        lcd_delay_ms(500);

        pr_info_with_tag("LCD", "Color diagnostic: BLUE...\n");
        bsp_lcd_fill_screen(0x001F);
        lcd_delay_ms(500);

        /* 最终设置为浅灰色背景 */
        bsp_lcd_fill_screen(LCD_COLOR_BG_LIGHT);

        /* 诊断边框: 上蓝 下绿 左红 右白 */
        bsp_lcd_fill_rect(0, 0, lcd_state.current_width, 2, 0x001F);
        bsp_lcd_fill_rect(0, lcd_state.current_height - 2,
                          lcd_state.current_width, 2, 0x07E0);
        bsp_lcd_fill_rect(0, 0, 2, lcd_state.current_height, 0xF800);
        bsp_lcd_fill_rect(lcd_state.current_width - 2, 0,
                          2, lcd_state.current_height, 0xFFFF);

        /* 开启背光 */
        bsp_lcd_set_backlight(100);

        lcd_state.initialized = true;

        pr_info_with_tag("LCD", "LCD ready: %dx%d orient=%d ctrl=%d ID=0x%06lX\n",
                         lcd_state.current_width,
                         lcd_state.current_height,
                         lcd_state.orientation,
                         (int)lcd_state.controller,
                         (unsigned long)lcd_id);

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

        uint8_t madctl_val = 0x00;  /* RGB模式, 配合LV_COLOR_16_SWAP=0 */

        switch (orient) {
        case LCD_ORIENTATION_PORTRAIT:
                /* 竖屏: 240x320 */
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
                /* 关闭背光 (LCD_BL = PH9) */
                HAL_GPIO_WritePin(GPIOH, GPIO_PIN_9, GPIO_PIN_RESET);
        } else if (brightness >= 100) {
                /* 最大亮度 */
                HAL_GPIO_WritePin(GPIOH, GPIO_PIN_9, GPIO_PIN_SET);
        } else {
                /* TODO: 如果需要PWM调光，在这里添加TIMx PWM代码 */
                /* 当前简化为全亮 */
                HAL_GPIO_WritePin(GPIOH, GPIO_PIN_9, GPIO_PIN_SET);
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

/* ==================== LVGL显示刷新回调 ==================== */

/**
 * lcd_flush_cb - LVGL显示刷新回调
 * @disp_drv: LVGL显示驱动指针
 * @area: 需要刷新的区域
 * @color_p: 像素颜色数组 (RGB565)
 *
 * LVGL调用此函数将帧缓冲区写入LCD
 *
 * 直接使用LVGL原生类型, 避免函数指针强制转换导致的
 * 未定义行为 (UB)。类型与 lv_disp_drv_t.flush_cb 完全匹配。
 */
void lcd_flush_cb(struct _lv_disp_drv_t *disp_drv,
                  const lv_area_t *area,
                  lv_color_t *color_p)
{
        uint16_t x = (uint16_t)area->x1;
        uint16_t y = (uint16_t)area->y1;
        uint16_t w = (uint16_t)(area->x2 - area->x1 + 1);
        uint16_t h = (uint16_t)(area->y2 - area->y1 + 1);

        bsp_lcd_set_window(x, y, w, h);
        bsp_lcd_write_data_batch((const uint16_t *)color_p,
                                 (uint32_t)w * h);

        lv_disp_flush_ready(disp_drv);
}
