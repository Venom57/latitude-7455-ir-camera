// SPDX-License-Identifier: GPL-2.0
/*
 * Himax HM1092 IR sensor driver (Dell Latitude 7455).
 *
 * There is no datasheet. Everything here comes from Dell's Windows driver
 * pack (decoded with ../tools):
 *   - power: enable GPIO, MCLK 24 MHz, reset released after MCLK
 *     (CAMI_RES_QRD.bin)
 *   - I2C address 0x24, chip ID 0x1091 at 0x0000 (SCFG_AUX_QRD.bin)
 *   - one mode, 560x360 RAW10 at 30 fps, and its register list
 *     (com.qti.sensormodule.hm1092.bin)
 * The register map is SMIA-style: 0x0100 streaming, 0x0104 group hold,
 * 0x0202 coarse exposure, 0x0340 frame length, 0x0342 line length,
 * 0x0601 test pattern.
 *
 * The sensor is monochrome (IR), so it reports MEDIA_BUS_FMT_Y10_1X10.
 *
 * IR emitter: Windows (qccamflash8380.sys) switches it with PM8550 gpio9
 * through qcpmic on start-of-frame: on at frame 1, off at frame 3 of every
 * four, i.e. two frames lit and two dark. Here it is an optional LED
 * (led-names = "ir", a gpio-leds node on that pin). The sensor driver does
 * not see start-of-frame, so the toggles are scheduled every two frame
 * periods (computed from line length, frame length and pixel rate) against
 * absolute deadlines from stream start, which keeps them from drifting.
 * emitter_mode: 0 off, 1 on while streaming, 2 Windows pulse (default).
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/workqueue.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>

#include "hm1092_mode_560x360.h"

#define HM1092_MCLK			24000000
#define HM1092_LINK_FREQ		180000000ULL	/* 24 MHz / 0x0c * 0x5a */
#define HM1092_PIXEL_RATE		36000000	/* resolutionData */
#define HM1092_DATA_LANES		1

#define HM1092_REG_CHIP_ID		0x0000
#define HM1092_CHIP_ID			0x1091

#define HM1092_REG_MODE_SELECT		CCI_REG8(0x0100)
#define HM1092_MODE_STANDBY		0x00
#define HM1092_MODE_STREAMING		0x01

#define HM1092_REG_GROUP_HOLD		CCI_REG8(0x0104)

#define HM1092_REG_EXPOSURE		0x0202
#define HM1092_EXPOSURE_MIN		1
#define HM1092_EXPOSURE_MARGIN		4
/* Windows starts at 0x00be (8.5 ms) and lets AEC raise it; with no AEC here,
 * default to the full frame so emitter-lit frames are bright enough. */
#define HM1092_EXPOSURE_DEF		(HM1092_FRAME_LENGTH_DEF - HM1092_EXPOSURE_MARGIN)

#define HM1092_REG_FRAME_LENGTH		0x0340
#define HM1092_FRAME_LENGTH_DEF		0x02ee
#define HM1092_FRAME_LENGTH_MAX		0xffff
#define HM1092_LINE_LENGTH		0x0650

#define HM1092_REG_TEST_PATTERN		CCI_REG8(0x0601)

#define HM1092_WIDTH			560
#define HM1092_HEIGHT			360
#define HM1092_MBUS_CODE		MEDIA_BUS_FMT_Y10_1X10

/* Frames per emitter half-period: Windows lights 2 frames, then 2 dark. */
#define HM1092_EMITTER_FRAMES		2

static int emitter_mode = 2;
module_param(emitter_mode, int, 0644);
MODULE_PARM_DESC(emitter_mode,
		 "IR emitter while streaming: 0 off, 1 on, 2 pulse 2 frames on / 2 off like Windows (default)");

static const char * const hm1092_supply_names[] = { "avdd", "dvdd", "dovdd" };

static const s64 hm1092_link_freqs[] = { HM1092_LINK_FREQ };

/* Values 0-4 from the sensor module's testPatternData (SMIA numbering). */
static const char * const hm1092_test_pattern_menu[] = {
	"Disabled",
	"Solid Colour",
	"Colour Bars",
	"Fade to Grey Colour Bars",
	"PN9",
};

struct hm1092 {
	struct device *dev;
	struct regmap *regmap;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *vblank;

	struct clk *mclk;
	struct regulator_bulk_data supplies[ARRAY_SIZE(hm1092_supply_names)];
	struct gpio_desc *reset;

	struct led_classdev *emitter;	/* NULL if the DT has none */
	struct delayed_work emitter_work;
	bool emitter_lit;
	ktime_t emitter_t0;		/* stream start */
	u64 emitter_n;			/* toggles done */
};

static inline struct hm1092 *to_hm1092(struct v4l2_subdev *sd)
{
	return container_of(sd, struct hm1092, sd);
}

/*
 * The sensor does not auto-increment the register address (a 4-byte read at
 * 0x0000 returns 10 ff ff ff), so 16-bit registers are accessed a byte at a
 * time. Windows writes the low byte first; do the same.
 */
static void hm1092_write16(struct hm1092 *hm, u16 reg, u16 val, int *err)
{
	cci_write(hm->regmap, CCI_REG8(reg + 1), val & 0xff, err);
	cci_write(hm->regmap, CCI_REG8(reg), val >> 8, err);
}

static int hm1092_read16(struct hm1092 *hm, u16 reg, u16 *val)
{
	u64 hi, lo;
	int ret = 0;

	cci_read(hm->regmap, CCI_REG8(reg), &hi, &ret);
	cci_read(hm->regmap, CCI_REG8(reg + 1), &lo, &ret);
	*val = hi << 8 | lo;
	return ret;
}

/*
 * led_set_brightness(), not _sync(): the PMIC GPIO behind gpio-leds does not
 * sleep, so the LED only has a non-blocking brightness_set and _sync() would
 * store the value and return -ENOTSUPP without touching the pin.
 */
static void hm1092_emitter_set(struct hm1092 *hm, bool on)
{
	led_set_brightness(hm->emitter, on ? hm->emitter->max_brightness : LED_OFF);
	hm->emitter_lit = on;
}

/* Current frame period: (height + vblank) lines of line_length pixels. */
static u64 hm1092_frame_ns(struct hm1092 *hm)
{
	u64 lines = HM1092_HEIGHT + READ_ONCE(hm->vblank->val);

	return div_u64(lines * HM1092_LINE_LENGTH * NSEC_PER_SEC,
		       HM1092_PIXEL_RATE);
}

/* Queue the next toggle at t0 + n * half-period, never in the past. */
static void hm1092_emitter_schedule(struct hm1092 *hm)
{
	u64 half = HM1092_EMITTER_FRAMES * hm1092_frame_ns(hm);
	ktime_t next = ktime_add_ns(hm->emitter_t0, (hm->emitter_n + 1) * half);
	s64 wait = ktime_to_ns(ktime_sub(next, ktime_get()));

	schedule_delayed_work(&hm->emitter_work,
			      wait > 0 ? nsecs_to_jiffies(wait) : 0);
}

static void hm1092_emitter_work(struct work_struct *work)
{
	struct hm1092 *hm = container_of(to_delayed_work(work), struct hm1092,
					 emitter_work);

	hm1092_emitter_set(hm, !hm->emitter_lit);
	hm->emitter_n++;
	hm1092_emitter_schedule(hm);
}

static void hm1092_emitter_start(struct hm1092 *hm)
{
	if (!hm->emitter || emitter_mode <= 0)
		return;
	hm1092_emitter_set(hm, true);
	if (emitter_mode == 2) {
		hm->emitter_t0 = ktime_get();
		hm->emitter_n = 0;
		hm1092_emitter_schedule(hm);
	}
}

static void hm1092_emitter_stop(struct hm1092 *hm)
{
	if (!hm->emitter)
		return;
	cancel_delayed_work_sync(&hm->emitter_work);
	hm1092_emitter_set(hm, false);
}

static int hm1092_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct hm1092 *hm = container_of(ctrl->handler, struct hm1092, ctrls);
	int ret = 0;

	if (ctrl->id == V4L2_CID_VBLANK) {
		s64 max = HM1092_HEIGHT + ctrl->val - HM1092_EXPOSURE_MARGIN;

		ret = __v4l2_ctrl_modify_range(hm->exposure,
					       hm->exposure->minimum, max,
					       hm->exposure->step,
					       min_t(s64, hm->exposure->default_value, max));
		if (ret)
			return ret;
	}

	if (!pm_runtime_get_if_in_use(hm->dev))
		return 0;

	cci_write(hm->regmap, HM1092_REG_GROUP_HOLD, 1, &ret);
	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		hm1092_write16(hm, HM1092_REG_EXPOSURE, ctrl->val, &ret);
		break;
	case V4L2_CID_VBLANK:
		hm1092_write16(hm, HM1092_REG_FRAME_LENGTH,
			       HM1092_HEIGHT + ctrl->val, &ret);
		break;
	case V4L2_CID_TEST_PATTERN:
		cci_write(hm->regmap, HM1092_REG_TEST_PATTERN, ctrl->val, &ret);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	cci_write(hm->regmap, HM1092_REG_GROUP_HOLD, 0, &ret);

	pm_runtime_put(hm->dev);
	return ret;
}

static const struct v4l2_ctrl_ops hm1092_ctrl_ops = {
	.s_ctrl = hm1092_set_ctrl,
};

static int hm1092_init_controls(struct hm1092 *hm)
{
	struct v4l2_ctrl_handler *h = &hm->ctrls;
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl *ctrl;
	int vblank_def = HM1092_FRAME_LENGTH_DEF - HM1092_HEIGHT;
	int hblank = HM1092_LINE_LENGTH - HM1092_WIDTH;
	int ret;

	v4l2_ctrl_handler_init(h, 8);

	ctrl = v4l2_ctrl_new_int_menu(h, &hm1092_ctrl_ops, V4L2_CID_LINK_FREQ,
				      0, 0, hm1092_link_freqs);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	v4l2_ctrl_new_std(h, &hm1092_ctrl_ops, V4L2_CID_PIXEL_RATE,
			  HM1092_PIXEL_RATE, HM1092_PIXEL_RATE, 1,
			  HM1092_PIXEL_RATE);

	ctrl = v4l2_ctrl_new_std(h, &hm1092_ctrl_ops, V4L2_CID_HBLANK,
				 hblank, hblank, 1, hblank);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	hm->vblank = v4l2_ctrl_new_std(h, &hm1092_ctrl_ops, V4L2_CID_VBLANK,
				       vblank_def,
				       HM1092_FRAME_LENGTH_MAX - HM1092_HEIGHT,
				       1, vblank_def);

	hm->exposure = v4l2_ctrl_new_std(h, &hm1092_ctrl_ops, V4L2_CID_EXPOSURE,
					 HM1092_EXPOSURE_MIN,
					 HM1092_FRAME_LENGTH_DEF - HM1092_EXPOSURE_MARGIN,
					 1, HM1092_EXPOSURE_DEF);

	v4l2_ctrl_new_std_menu_items(h, &hm1092_ctrl_ops, V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(hm1092_test_pattern_menu) - 1,
				     0, 0, hm1092_test_pattern_menu);

	ret = v4l2_fwnode_device_parse(hm->dev, &props);
	if (ret)
		return ret;
	v4l2_ctrl_new_fwnode_properties(h, &hm1092_ctrl_ops, &props);

	if (h->error)
		return h->error;

	hm->sd.ctrl_handler = h;
	return 0;
}

static void hm1092_fill_format(struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = HM1092_WIDTH;
	fmt->height = HM1092_HEIGHT;
	fmt->code = HM1092_MBUS_CODE;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_601;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int hm1092_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct hm1092 *hm = to_hm1092(sd);
	int ret;

	ret = pm_runtime_resume_and_get(hm->dev);
	if (ret)
		return ret;

	ret = cci_multi_reg_write(hm->regmap, hm1092_mode_560x360_regs,
				  ARRAY_SIZE(hm1092_mode_560x360_regs), NULL);
	if (ret) {
		dev_err(hm->dev, "writing the mode registers failed: %d\n", ret);
		goto err;
	}

	ret = __v4l2_ctrl_handler_setup(hm->sd.ctrl_handler);
	if (ret)
		goto err;

	ret = cci_write(hm->regmap, HM1092_REG_MODE_SELECT,
			HM1092_MODE_STREAMING, NULL);
	if (ret)
		goto err;

	hm1092_emitter_start(hm);
	return 0;

err:
	pm_runtime_put(hm->dev);
	return ret;
}

static int hm1092_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct hm1092 *hm = to_hm1092(sd);

	hm1092_emitter_stop(hm);
	cci_write(hm->regmap, HM1092_REG_MODE_SELECT, HM1092_MODE_STANDBY, NULL);
	pm_runtime_put(hm->dev);
	return 0;
}

static int hm1092_set_format(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state,
			     struct v4l2_subdev_format *fmt)
{
	/* One fixed mode. */
	hm1092_fill_format(&fmt->format);
	*v4l2_subdev_state_get_format(state, fmt->pad) = fmt->format;
	return 0;
}

static int hm1092_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;
	code->code = HM1092_MBUS_CODE;
	return 0;
}

static int hm1092_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index || fse->code != HM1092_MBUS_CODE)
		return -EINVAL;
	fse->min_width = fse->max_width = HM1092_WIDTH;
	fse->min_height = fse->max_height = HM1092_HEIGHT;
	return 0;
}

static int hm1092_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	hm1092_fill_format(v4l2_subdev_state_get_format(state, 0));
	return 0;
}

static const struct v4l2_subdev_video_ops hm1092_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops hm1092_pad_ops = {
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = hm1092_set_format,
	.enum_mbus_code = hm1092_enum_mbus_code,
	.enum_frame_size = hm1092_enum_frame_size,
	.enable_streams = hm1092_enable_streams,
	.disable_streams = hm1092_disable_streams,
};

static const struct v4l2_subdev_ops hm1092_subdev_ops = {
	.video = &hm1092_video_ops,
	.pad = &hm1092_pad_ops,
};

static const struct media_entity_operations hm1092_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static const struct v4l2_subdev_internal_ops hm1092_internal_ops = {
	.init_state = hm1092_init_state,
};

/* Windows order (CAMI_RES_QRD.bin): enable, 1 ms, MCLK, release reset, wait. */
static int hm1092_power_on(struct device *dev)
{
	struct hm1092 *hm = to_hm1092(dev_get_drvdata(dev));
	int ret;

	gpiod_set_value_cansleep(hm->reset, 1);

	ret = regulator_bulk_enable(ARRAY_SIZE(hm->supplies), hm->supplies);
	if (ret) {
		dev_err(dev, "enabling supplies failed: %d\n", ret);
		return ret;
	}
	usleep_range(1000, 2000);

	ret = clk_prepare_enable(hm->mclk);
	if (ret) {
		dev_err(dev, "enabling MCLK failed: %d\n", ret);
		regulator_bulk_disable(ARRAY_SIZE(hm->supplies), hm->supplies);
		return ret;
	}
	usleep_range(1000, 2000);

	gpiod_set_value_cansleep(hm->reset, 0);
	msleep(10);
	return 0;
}

static int hm1092_power_off(struct device *dev)
{
	struct hm1092 *hm = to_hm1092(dev_get_drvdata(dev));

	gpiod_set_value_cansleep(hm->reset, 1);
	clk_disable_unprepare(hm->mclk);
	regulator_bulk_disable(ARRAY_SIZE(hm->supplies), hm->supplies);
	return 0;
}

static int hm1092_check_hwcfg(struct hm1092 *hm)
{
	struct v4l2_fwnode_endpoint bus_cfg = { .bus_type = V4L2_MBUS_CSI2_DPHY };
	struct fwnode_handle *ep;
	unsigned long freq_bitmap;
	int ret;

	ep = fwnode_graph_get_next_endpoint(dev_fwnode(hm->dev), NULL);
	if (!ep)
		return dev_err_probe(hm->dev, -EPROBE_DEFER, "no endpoint\n");

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return dev_err_probe(hm->dev, ret, "parsing endpoint failed\n");

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != HM1092_DATA_LANES) {
		ret = dev_err_probe(hm->dev, -EINVAL, "%u data lanes; need %u\n",
				    bus_cfg.bus.mipi_csi2.num_data_lanes,
				    HM1092_DATA_LANES);
		goto out;
	}

	ret = v4l2_link_freq_to_bitmap(hm->dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       hm1092_link_freqs,
				       ARRAY_SIZE(hm1092_link_freqs),
				       &freq_bitmap);
out:
	v4l2_fwnode_endpoint_free(&bus_cfg);
	return ret;
}

static int hm1092_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct hm1092 *hm;
	unsigned long rate;
	u16 id;
	int i, ret;

	hm = devm_kzalloc(dev, sizeof(*hm), GFP_KERNEL);
	if (!hm)
		return -ENOMEM;
	hm->dev = dev;

	v4l2_i2c_subdev_init(&hm->sd, client, &hm1092_subdev_ops);

	ret = hm1092_check_hwcfg(hm);
	if (ret)
		return ret;

	hm->mclk = devm_v4l2_sensor_clk_get(dev, NULL);
	if (IS_ERR(hm->mclk))
		return dev_err_probe(dev, PTR_ERR(hm->mclk), "no MCLK\n");
	rate = clk_get_rate(hm->mclk);
	if (rate != HM1092_MCLK)
		return dev_err_probe(dev, -EINVAL, "MCLK is %lu Hz, need %u\n",
				     rate, HM1092_MCLK);

	for (i = 0; i < ARRAY_SIZE(hm1092_supply_names); i++)
		hm->supplies[i].supply = hm1092_supply_names[i];
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(hm->supplies),
				      hm->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "supplies\n");

	hm->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(hm->reset))
		return dev_err_probe(dev, PTR_ERR(hm->reset), "reset gpio\n");

	/* Optional IR emitter: leds = <&...>; led-names = "ir"; */
	hm->emitter = devm_led_get(dev, "ir");
	if (IS_ERR(hm->emitter)) {
		if (PTR_ERR(hm->emitter) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		dev_info(dev, "no IR emitter LED (%ld); capturing without it\n",
			 PTR_ERR(hm->emitter));
		hm->emitter = NULL;
	} else {
		INIT_DELAYED_WORK(&hm->emitter_work, hm1092_emitter_work);
		led_set_brightness(hm->emitter, LED_OFF);
	}

	hm->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(hm->regmap))
		return PTR_ERR(hm->regmap);

	ret = hm1092_power_on(dev);
	if (ret)
		return ret;

	ret = hm1092_read16(hm, HM1092_REG_CHIP_ID, &id);
	if (ret) {
		dev_err(dev, "reading the chip ID failed: %d\n", ret);
		goto power_off;
	}
	if (id != HM1092_CHIP_ID) {
		dev_err(dev, "chip ID 0x%04x, expected 0x%04x\n", id,
			HM1092_CHIP_ID);
		ret = -ENXIO;
		goto power_off;
	}
	dev_info(dev, "HM1092 found, chip ID 0x%04x, IR emitter %s\n", id,
		 hm->emitter ? dev_name(hm->emitter->dev) : "none");

	ret = hm1092_init_controls(hm);
	if (ret)
		goto free_ctrls;

	hm->sd.internal_ops = &hm1092_internal_ops;
	hm->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	hm->sd.entity.ops = &hm1092_entity_ops;
	hm->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	hm->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&hm->sd.entity, 1, &hm->pad);
	if (ret)
		goto free_ctrls;

	hm->sd.state_lock = hm->ctrls.lock;
	ret = v4l2_subdev_init_finalize(&hm->sd);
	if (ret)
		goto cleanup_entity;

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	ret = v4l2_async_register_subdev_sensor(&hm->sd);
	if (ret) {
		dev_err(dev, "registering the subdev failed: %d\n", ret);
		goto cleanup_subdev;
	}

	pm_runtime_idle(dev);
	return 0;

cleanup_subdev:
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
	v4l2_subdev_cleanup(&hm->sd);
cleanup_entity:
	media_entity_cleanup(&hm->sd.entity);
free_ctrls:
	v4l2_ctrl_handler_free(&hm->ctrls);
power_off:
	hm1092_power_off(dev);
	return ret;
}

static void hm1092_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct hm1092 *hm = to_hm1092(sd);

	hm1092_emitter_stop(hm);
	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&hm->ctrls);
	pm_runtime_disable(hm->dev);
	if (!pm_runtime_status_suspended(hm->dev)) {
		hm1092_power_off(hm->dev);
		pm_runtime_set_suspended(hm->dev);
	}
}

static DEFINE_RUNTIME_DEV_PM_OPS(hm1092_pm_ops, hm1092_power_off,
				 hm1092_power_on, NULL);

static const struct of_device_id hm1092_of_match[] = {
	{ .compatible = "himax,hm1092" },
	{ }
};
MODULE_DEVICE_TABLE(of, hm1092_of_match);

static struct i2c_driver hm1092_driver = {
	.driver = {
		.name = "hm1092",
		.pm = pm_sleep_ptr(&hm1092_pm_ops),
		.of_match_table = hm1092_of_match,
	},
	.probe = hm1092_probe,
	.remove = hm1092_remove,
};
module_i2c_driver(hm1092_driver);

MODULE_DESCRIPTION("Himax HM1092 IR sensor driver");
MODULE_LICENSE("GPL");
