/*
 * edgevib_csi_sensor.c — EdgeVib OV5640 MIPI CSI Sensor Driver
 *
 * V4L2 subdev driver for the OmniVision OV5640 5MP MIPI CSI-2 camera sensor.
 * Designed as a learning template for the v4l2_subdev + media_controller
 * framework. Registers as an I2C client driver, creates a media entity with
 * a single source pad, and links to the sunxi VIN MIPI subdev via DT phandle.
 *
 * Architecture:
 *   DT overlay: ov5640 I2C node → mipi-controller phandle → MIPI D-PHY
 *   probe(): chip ID verify → init regs → v4l2_subdev_init → media_entity
 *            → of_parse_phandle("mipi-controller")
 *            → media_create_pad_link(sensor_source → mipi_sink)
 *   s_power(1): setup clocks + GPIOs + write init register table
 *   s_stream(1): start MIPI CSI-2 output
 *
 * v4l2_subdev_ops:
 *   .s_power    — power on/off + register init
 *   .s_stream   — start/stop MIPI output
 *   .get_fmt    — return current frame format
 *   .set_fmt    — negotiate resolution (640x480/1920x1080/2592x1944)
 *   .enum_mbus_code — MEDIA_BUS_FMT_YUYV8_2X8
 *
 * v4l2_ctrl_handler: exposure, gain, auto_white_balance, hflip
 *
 * Hardware requirement: physical OV5640 MIPI CSI camera module
 * (15-pin FPC, I2C addr 0x3c, 24MHz XCLK, 2-lane MIPI CSI-2).
 * Without hardware, probe() will not be called — this module is marked
 * "pending hardware validation".
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/version.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-mediabus.h>
#include <media/media-entity.h>

#define OV5640_I2C_ADDR           0x3c
#define OV5640_CHIP_ID            0x5640
#define OV5640_XCLK_FREQ          24000000  /* 24 MHz */
#define SENSOR_PAD_SOURCE         0

/* ---- I2C register table ---- */

struct regval {
	u16 reg;
	u8  val;
};

/*
 * OV5640 initialization register sequence for 2592×1944 @ 15fps,
 * YUYV output format, 2-lane MIPI CSI-2.
 *
 * Sourced from: Linux mainline drivers/media/i2c/ov5640.c (GPL-2.0)
 * Publicly available register configuration validated by OmniVision datasheet.
 */
static const struct regval ov5640_init_regs[] = {
	/* ── System control ── */
	{0x3008, 0x42},     /* Software reset, power down */

	/* ── PLL configuration: 24MHz XCLK → 84MHz PCLK ── */
	{0x3103, 0x03},     /* System clock from PLL */
	{0x3035, 0x11},     /* PLL pre-divider = /1 */
	{0x3036, 0x46},     /* PLL multiplier = 70 */
	{0x3037, 0x13},     /* System clock divider: /1 /2 /2 */

	/* ── IO pad control ── */
	{0x3017, 0x00},     /* PAD output enable 00 */
	{0x3018, 0x00},     /* PAD output enable 01 */

	/* ── MIPI CSI-2 configuration ── */
	{0x300e, 0x45},     /* MIPI enable, 2-lane */
	{0x4800, 0x04},     /* MIPI line sync enable */
	{0x3034, 0x1a},     /* MIPI bit mode: 8-bit */
	{0x302e, 0x00},     /* MIPI bit mode continued */

	/* ── Output format: YUYV ── */
	{0x4300, 0x30},     /* Format control: YUYV (RGB/RAW disabled) */

	/* ── Output size: 2592×1944 ── */
	{0x3808, 0x0a},     /* DVPHO[11:8] = 0x0A */
	{0x3809, 0x20},     /* DVPHO[7:0]  = 0x20 → 2592 */
	{0x380a, 0x07},     /* DVPVO[11:8] = 0x07 */
	{0x380b, 0x98},     /* DVPVO[7:0]  = 0x98 → 1944 */

	/* ── Timing: 15fps ── */
	{0x380c, 0x0b},     /* HTS[11:8] */
	{0x380d, 0x1c},     /* HTS[7:0] → 2844 */
	{0x380e, 0x07},     /* VTS[11:8] */
	{0x380f, 0xb0},     /* VTS[7:0] → 1968 */

	/* ── AEC/AGC/AWB ── */
	{0x3a0f, 0x30},     /* AEC stable range */
	{0x3a10, 0x28},     /* AEC 50/60Hz step */
	{0x3a1b, 0x30},     /* AEC/AGC gain ceiling */
	{0x3a1e, 0x26},     /* AEC/AGC stable time */
	{0x3a11, 0x60},     /* AEC clock divider */
	{0x3a1f, 0x14},     /* AGC max gain */

	/* ── Image quality ── */
	{0x5000, 0x06},     /* BLC enable */
	{0x5001, 0x01},     /* AWB enable */
	{0x501f, 0x03},     /* ISP format select */
	{0x5300, 0x08},     /* CIP shading correction enable */
	{0x5301, 0x30},     /* CIP sharpening enable */
	{0x5302, 0x10},     /* CIP denoise enable */
	{0x5303, 0x00},     /* CIP gamma enable */
	{0x5304, 0x08},     /* CIP CMX enable */
	{0x5305, 0x30},
	{0x5306, 0x08},
	{0x5307, 0x16},

	/* ── MIPI clock lane LP-11 state ── */
	{0x303d, 0x10},
	{0x303e, 0x10},

	/* ── JPEG / compression disabled ── */
	{0x4713, 0x02},     /* JPEG mode select: bypass */

	/* ── Clock polarity ── */
	{0x3106, 0x00},     /* PLL: no input clock divider */
	{0x3824, 0x01},     /* VSYNC polarity */

	/* Sentry terminator */
	{0xffff, 0x00},
};

/* ---- Driver private data ---- */

struct ov5640 {
	struct v4l2_subdev          subdev;
	struct media_pad            pad;            /* single source pad */
	struct v4l2_ctrl_handler    ctrl_handler;
	struct i2c_client          *client;
	struct mutex                lock;           /* protects fmt + state */

	/* Current format */
	struct v4l2_mbus_framefmt   fmt;

	/* State */
	bool                        power_on;
	bool                        streaming;

	/* Hardware resources */
	struct clk                 *xclk;
	struct gpio_desc           *reset_gpio;
	struct gpio_desc           *pwdn_gpio;

	/* MIPI controller subdev (from DT phandle) */
	struct v4l2_subdev         *mipi_sd;
	int                         mipi_pad_sink;
};

static inline struct ov5640 *to_ov5640(struct v4l2_subdev *sd)
{
	return container_of(sd, struct ov5640, subdev);
}

/* ─── I2C helpers ─── */

static int ov5640_write_reg(struct ov5640 *sensor, u16 reg, u8 val)
{
	struct i2c_client *client = sensor->client;
	u8 buf[3];
	int ret;

	buf[0] = (reg >> 8) & 0xff;
	buf[1] = reg & 0xff;
	buf[2] = val;

	ret = i2c_master_send(client, buf, 3);
	if (ret < 0) {
		dev_err(&client->dev, "%s: write reg 0x%04x failed: %d\n",
			__func__, reg, ret);
		return ret;
	}
	return 0;
}

static int ov5640_read_reg(struct ov5640 *sensor, u16 reg, u8 *val)
{
	struct i2c_client *client = sensor->client;
	u8 addr_buf[2] = { (reg >> 8) & 0xff, reg & 0xff };
	int ret;

	ret = i2c_master_send(client, addr_buf, 2);
	if (ret < 0) {
		dev_err(&client->dev, "%s: write addr 0x%04x failed: %d\n",
			__func__, reg, ret);
		return ret;
	}

	ret = i2c_master_recv(client, val, 1);
	if (ret < 0) {
		dev_err(&client->dev, "%s: read reg 0x%04x failed: %d\n",
			__func__, reg, ret);
		return ret;
	}
	return 0;
}

static int ov5640_write_table(struct ov5640 *sensor,
			       const struct regval *table)
{
	int ret;

	for (; table->reg != 0xffff; table++) {
		ret = ov5640_write_reg(sensor, table->reg, table->val);
		if (ret)
			return ret;
		/* Small delay for PLL and reset-sensitive registers */
		if (table->reg == 0x3008 || table->reg == 0x3103)
			usleep_range(1000, 2000);
	}
	return 0;
}

/* ─── v4l2_ctrl_ops ─── */

static int ov5640_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov5640 *sensor = container_of(ctrl->handler,
					     struct ov5640, ctrl_handler);
	int ret = 0;

	if (!sensor->power_on)
		return -EBUSY;

	mutex_lock(&sensor->lock);

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = ov5640_write_reg(sensor, 0x3500, (ctrl->val >> 12) & 0x0f);
		ret |= ov5640_write_reg(sensor, 0x3501, (ctrl->val >> 4) & 0xff);
		ret |= ov5640_write_reg(sensor, 0x3502, (ctrl->val << 4) & 0xf0);
		break;
	case V4L2_CID_GAIN:
		ret = ov5640_write_reg(sensor, 0x350a, (ctrl->val >> 2) & 0x3f);
		ret |= ov5640_write_reg(sensor, 0x350b, ctrl->val & 0x03);
		break;
	case V4L2_CID_AUTO_WHITE_BALANCE:
		if (ctrl->val)
			ret = ov5640_write_reg(sensor, 0x3406, 0x01);
		else
			ret = ov5640_write_reg(sensor, 0x3406, 0x00);
		break;
	case V4L2_CID_HFLIP:
		if (ctrl->val)
			ret = ov5640_write_reg(sensor, 0x3820, 0x46);
		else
			ret = ov5640_write_reg(sensor, 0x3820, 0x40);
		break;
	default:
		ret = -EINVAL;
	}

	mutex_unlock(&sensor->lock);
	return ret;
}

static const struct v4l2_ctrl_ops ov5640_ctrl_ops = {
	.s_ctrl = ov5640_s_ctrl,
};

/* ─── v4l2_subdev_core_ops ─── */

static int ov5640_s_power(struct v4l2_subdev *sd, int on)
{
	struct ov5640 *sensor = to_ov5640(sd);
	int ret;

	mutex_lock(&sensor->lock);

	if (on && !sensor->power_on) {
		/* Power-up sequence */
		if (sensor->pwdn_gpio)
			gpiod_set_value_cansleep(sensor->pwdn_gpio, 0);
		usleep_range(1000, 2000);

		if (sensor->reset_gpio) {
			gpiod_set_value_cansleep(sensor->reset_gpio, 0);
			usleep_range(1000, 2000);
			gpiod_set_value_cansleep(sensor->reset_gpio, 1);
			usleep_range(5000, 10000); /* sensor boot time */
		}

		if (sensor->xclk) {
			ret = clk_prepare_enable(sensor->xclk);
			if (ret) {
				mutex_unlock(&sensor->lock);
				return ret;
			}
		}

		/* Write full init register table */
		ret = ov5640_write_table(sensor, ov5640_init_regs);
		if (ret) {
			dev_err(&sensor->client->dev,
				"init register table write failed: %d\n", ret);
			if (sensor->xclk)
				clk_disable_unprepare(sensor->xclk);
			mutex_unlock(&sensor->lock);
			return ret;
		}

		sensor->power_on = true;
		dev_info(&sensor->client->dev, "OV5640 powered on\n");

	} else if (!on && sensor->power_on) {
		/* Power-down sequence */
		if (sensor->streaming) {
			/* Stop streaming first */
			mutex_unlock(&sensor->lock);
			ov5640_s_stream(sd, 0);
			mutex_lock(&sensor->lock);
		}

		if (sensor->xclk)
			clk_disable_unprepare(sensor->xclk);

		if (sensor->pwdn_gpio)
			gpiod_set_value_cansleep(sensor->pwdn_gpio, 1);

		sensor->power_on = false;
		dev_info(&sensor->client->dev, "OV5640 powered off\n");
	}

	mutex_unlock(&sensor->lock);
	return 0;
}

/* ─── v4l2_subdev_video_ops ─── */

static int ov5640_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct ov5640 *sensor = to_ov5640(sd);
	int ret = 0;

	mutex_lock(&sensor->lock);

	if (!sensor->power_on) {
		mutex_unlock(&sensor->lock);
		return -EBUSY;
	}

	if (enable && !sensor->streaming) {
		/* Start MIPI output */
		u8 val;
		ov5640_read_reg(sensor, 0x300e, &val);
		ret = ov5640_write_reg(sensor, 0x300e, val | 0x40);
		if (ret == 0)
			sensor->streaming = true;
	} else if (!enable && sensor->streaming) {
		/* Stop MIPI output */
		ret = ov5640_write_reg(sensor, 0x300e, 0x45);
		sensor->streaming = false;
	}

	mutex_unlock(&sensor->lock);
	return ret;
}

/* ─── v4l2_subdev_pad_ops ─── */

static int ov5640_get_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *state,
			   struct v4l2_subdev_format *format)
{
	struct ov5640 *sensor = to_ov5640(sd);

	mutex_lock(&sensor->lock);
	format->format = sensor->fmt;
	mutex_unlock(&sensor->lock);
	return 0;
}

static int ov5640_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *state,
			   struct v4l2_subdev_format *format)
{
	struct ov5640 *sensor = to_ov5640(sd);
	struct v4l2_mbus_framefmt *fmt = &format->format;

	/* Only support YUYV 8-bit on the MIPI bus */
	if (fmt->code != MEDIA_BUS_FMT_YUYV8_2X8)
		return -EINVAL;

	/* Clamp to supported resolutions */
	if (fmt->width <= 640) {
		fmt->width  = 640;
		fmt->height = 480;
	} else if (fmt->width <= 1920) {
		fmt->width  = 1920;
		fmt->height = 1080;
	} else {
		fmt->width  = 2592;
		fmt->height = 1944;
	}

	fmt->field = V4L2_FIELD_NONE;

	mutex_lock(&sensor->lock);
	sensor->fmt = *fmt;
	mutex_unlock(&sensor->lock);
	return 0;
}

static int ov5640_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_YUYV8_2X8;
	return 0;
}

static int ov5640_enum_frame_size(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index > 2 || fse->code != MEDIA_BUS_FMT_YUYV8_2X8)
		return -EINVAL;

	switch (fse->index) {
	case 0:
		fse->min_width = fse->max_width = 640;
		fse->min_height = fse->max_height = 480;
		break;
	case 1:
		fse->min_width = fse->max_width = 1920;
		fse->min_height = fse->max_height = 1080;
		break;
	case 2:
		fse->min_width = fse->max_width = 2592;
		fse->min_height = fse->max_height = 1944;
		break;
	}
	return 0;
}

static const struct v4l2_subdev_core_ops ov5640_core_ops = {
	.s_power = ov5640_s_power,
};

static const struct v4l2_subdev_video_ops ov5640_video_ops = {
	.s_stream = ov5640_s_stream,
};

static const struct v4l2_subdev_pad_ops ov5640_pad_ops = {
	.get_fmt           = ov5640_get_fmt,
	.set_fmt           = ov5640_set_fmt,
	.enum_mbus_code    = ov5640_enum_mbus_code,
	.enum_frame_size   = ov5640_enum_frame_size,
};

static const struct v4l2_subdev_ops ov5640_subdev_ops = {
	.core  = &ov5640_core_ops,
	.video = &ov5640_video_ops,
	.pad   = &ov5640_pad_ops,
};

/* ─── I2C driver probe / remove ─── */

static int ov5640_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct ov5640 *sensor;
	struct device_node *mipi_np;
	struct platform_device *mipi_pdev;
	u8 chip_id_high, chip_id_low;
	u16 chip_id;
	int ret;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->client = client;
	mutex_init(&sensor->lock);

	/* ── Parse DT resources ── */

	sensor->xclk = devm_clk_get_optional(dev, NULL);
	if (IS_ERR(sensor->xclk))
		return dev_err_probe(dev, PTR_ERR(sensor->xclk),
				     "failed to get xclk\n");

	sensor->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						      GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(sensor->reset_gpio),
				     "failed to get reset GPIO\n");

	sensor->pwdn_gpio = devm_gpiod_get_optional(dev, "pwdn",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->pwdn_gpio))
		return dev_err_probe(dev, PTR_ERR(sensor->pwdn_gpio),
				     "failed to get pwdn GPIO\n");

	/* ── Verify chip ID ── */
	/* Temporarily enable xclk and de-assert pwdn for chip ID read */
	if (sensor->pwdn_gpio)
		gpiod_set_value_cansleep(sensor->pwdn_gpio, 0);
	usleep_range(1000, 2000);

	if (sensor->reset_gpio) {
		gpiod_set_value_cansleep(sensor->reset_gpio, 0);
		usleep_range(1000, 2000);
		gpiod_set_value_cansleep(sensor->reset_gpio, 1);
		usleep_range(5000, 10000);
	}

	if (sensor->xclk) {
		ret = clk_prepare_enable(sensor->xclk);
		if (ret) {
			dev_err(dev, "failed to enable xclk: %d\n", ret);
			goto err_power_down;
		}
	}

	/* Read chip ID from registers 0x300A (high) and 0x300B (low) */
	ret = ov5640_read_reg(sensor, 0x300a, &chip_id_high);
	ret |= ov5640_read_reg(sensor, 0x300b, &chip_id_low);
	if (ret) {
		dev_err(dev, "failed to read chip ID (sensor not found?)\n");
		ret = -ENODEV;
		goto err_clk_off;
	}

	chip_id = ((u16)chip_id_high << 8) | chip_id_low;
	if (chip_id != OV5640_CHIP_ID) {
		dev_err(dev, "unexpected chip ID 0x%04x (expected 0x%04x)\n",
			chip_id, OV5640_CHIP_ID);
		ret = -ENODEV;
		goto err_clk_off;
	}
	dev_info(dev, "OV5640 chip ID 0x%04x confirmed\n", chip_id);

	/* Turn off xclk after ID check (s_power handles actual init) */
	if (sensor->xclk)
		clk_disable_unprepare(sensor->xclk);
	if (sensor->pwdn_gpio)
		gpiod_set_value_cansleep(sensor->pwdn_gpio, 1);

	/* ── Set default format ── */
	sensor->fmt.width  = 1920;
	sensor->fmt.height = 1080;
	sensor->fmt.code   = MEDIA_BUS_FMT_YUYV8_2X8;
	sensor->fmt.field  = V4L2_FIELD_NONE;
	sensor->fmt.colorspace = V4L2_COLORSPACE_SRGB;

	/* ── v4l2_subdev init ── */
	v4l2_subdev_init(&sensor->subdev, &ov5640_subdev_ops);
	sensor->subdev.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	snprintf(sensor->subdev.name, sizeof(sensor->subdev.name),
		 "ov5640 %s", dev_name(dev));
	sensor->subdev.dev = dev;

	/* ── v4l2_ctrl_handler ── */
	v4l2_ctrl_handler_init(&sensor->ctrl_handler, 4);
	v4l2_ctrl_new_std(&sensor->ctrl_handler, &ov5640_ctrl_ops,
			  V4L2_CID_EXPOSURE, 0, 65535, 1, 0);
	v4l2_ctrl_new_std(&sensor->ctrl_handler, &ov5640_ctrl_ops,
			  V4L2_CID_GAIN, 0, 1023, 1, 0);
	v4l2_ctrl_new_std(&sensor->ctrl_handler, &ov5640_ctrl_ops,
			  V4L2_CID_AUTO_WHITE_BALANCE, 0, 1, 1, 1);
	v4l2_ctrl_new_std(&sensor->ctrl_handler, &ov5640_ctrl_ops,
			  V4L2_CID_HFLIP, 0, 1, 1, 0);

	if (sensor->ctrl_handler.error) {
		ret = sensor->ctrl_handler.error;
		dev_err(dev, "ctrl_handler init failed: %d\n", ret);
		goto err_free_ctrl;
	}
	sensor->subdev.ctrl_handler = &sensor->ctrl_handler;

	/* ── media_entity ── */
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&sensor->subdev.entity, 1, &sensor->pad);
	if (ret) {
		dev_err(dev, "media_entity_pads_init failed: %d\n", ret);
		goto err_free_ctrl;
	}

	/* ── Register subdev ── */
	i2c_set_clientdata(client, sensor);
	ret = v4l2_async_register_subdev(&sensor->subdev);
	if (ret) {
		dev_err(dev, "v4l2_async_register_subdev failed: %d\n", ret);
		goto err_cleanup_entity;
	}

	/* ── DT phandle: find MIPI controller and create media link ── */
	mipi_np = of_parse_phandle(dev->of_node, "mipi-controller", 0);
	if (!mipi_np) {
		dev_err(dev, "missing 'mipi-controller' DT phandle\n");
		ret = -EINVAL;
		goto err_unregister_subdev;
	}

	mipi_pdev = of_find_device_by_node(mipi_np);
	of_node_put(mipi_np);
	if (!mipi_pdev) {
		dev_err(dev, "MIPI platform device not found\n");
		ret = -EPROBE_DEFER;
		goto err_unregister_subdev;
	}

	sensor->mipi_sd = platform_get_drvdata(mipi_pdev);
	if (!sensor->mipi_sd) {
		dev_err(dev, "MIPI subdev drvdata is NULL (VIN driver loaded?)\n");
		platform_device_put(mipi_pdev);
		ret = -EPROBE_DEFER;
		goto err_unregister_subdev;
	}

	/* Create immutable pad link: sensor source → mipi sink */
	sensor->mipi_pad_sink = 0;  /* sunxi MIPI sink pad index */
	ret = media_create_pad_link(
		&sensor->subdev.entity, SENSOR_PAD_SOURCE,
		&sensor->mipi_sd->entity, sensor->mipi_pad_sink,
		MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
	if (ret) {
		dev_err(dev, "media_create_pad_link failed: %d\n", ret);
		platform_device_put(mipi_pdev);
		goto err_unregister_subdev;
	}
	platform_device_put(mipi_pdev);

	dev_info(dev, "OV5640 probed at I2C 0x%02x, linked to %s (MIPI sink pad %d)\n",
		 client->addr, mipi_np->name, sensor->mipi_pad_sink);
	return 0;

err_unregister_subdev:
	v4l2_async_unregister_subdev(&sensor->subdev);
err_cleanup_entity:
	media_entity_cleanup(&sensor->subdev.entity);
err_free_ctrl:
	v4l2_ctrl_handler_free(&sensor->ctrl_handler);
	mutex_destroy(&sensor->lock);
	return ret;

err_clk_off:
	if (sensor->xclk)
		clk_disable_unprepare(sensor->xclk);
err_power_down:
	if (sensor->pwdn_gpio)
		gpiod_set_value_cansleep(sensor->pwdn_gpio, 1);
	mutex_destroy(&sensor->lock);
	return ret;
}

static void ov5640_remove(struct i2c_client *client)
{
	struct ov5640 *sensor = i2c_get_clientdata(client);

	v4l2_async_unregister_subdev(&sensor->subdev);
	media_entity_cleanup(&sensor->subdev.entity);
	v4l2_ctrl_handler_free(&sensor->ctrl_handler);

	if (sensor->xclk)
		clk_disable_unprepare(sensor->xclk);

	mutex_destroy(&sensor->lock);
	dev_info(&client->dev, "OV5640 removed\n");
}

/* ─── OF match table ─── */

static const struct of_device_id ov5640_of_match[] = {
	{ .compatible = "ovti,ov5640" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ov5640_of_match);

/* I2C device ID for non-DT systems */
static const struct i2c_device_id ov5640_id_table[] = {
	{ "ov5640", 0 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, ov5640_id_table);

static struct i2c_driver ov5640_i2c_driver = {
	.driver = {
		.name           = "edgevib-ov5640",
		.of_match_table = ov5640_of_match,
	},
	.probe    = ov5640_probe,
	.remove   = ov5640_remove,
	.id_table = ov5640_id_table,
};
module_i2c_driver(ov5640_i2c_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("EdgeVib Team");
MODULE_DESCRIPTION("EdgeVib OV5640 MIPI CSI Sensor Driver (v4l2_subdev template)");
