// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2025 Vladimir Panferov
// Copyright (c) 2025 RadGoodNow
// Based on command information from the vendor device tree

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct td4160tm_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *reset_gpio;
};

static inline
struct td4160tm_panel *to_td4160tm_panel(struct drm_panel *panel)
{
	return container_of(panel, struct td4160tm_panel, panel);
}

static void td4160tm_panel_reset(struct td4160tm_panel *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(120);
}

static int td4160tm_panel_on(struct td4160tm_panel *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb0, 0x00);
	//OTP/Flash,Load,setting
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xd6, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb0, 0x03);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb0, 0x04);
	mipi_dsi_usleep_range(&dsi_ctx, 50000, 50100);
	
	/* cabc start */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xce, 0x15, 0x05);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xce, 0x16, 0x04);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xce, 0x17, 0x8c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xc1, 0x01, 0x81);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xce, 0x11, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xce, 0x12, 0x46);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xce, 0x11, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xce, 0x12, 0xb2);

	/* All gate on code */
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb0, 0x00);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xc0, 0x06, 0x0e);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xeb, 0x00, 0x07);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xeb, 0x01, 0x50);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xeb, 0x06, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xed, 0x08, 0x69);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xed, 0x09, 0x97);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xed, 0x0a, 0x0e);

	//Manufacture,Command,Access,Protect
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb0, 0x03);
	//TE ON
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x35, 0x00);
	//Unlock,Manufacture,Command,Access,Protect
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb0, 0x00);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb6, 0x30, 0x6a, 0x00, 0x86, 0xc3, 0x03);
	//CABC
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xff, 0x10);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xfb, 0x01);
	//Write_Display_Brightness 12bit
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x51, 0x0A, 0x5A);
	//backlight dimming enable
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x53, 0x2c);
	//Write_CABC
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x55, 0x03);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb0, 0x03);
	mipi_dsi_msleep(&dsi_ctx, 10);
	/*CABC end*/

	/* Display On*/
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x11, 0x00);
	mipi_dsi_msleep(&dsi_ctx, 120);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x29, 0x00);
	mipi_dsi_usleep_range(&dsi_ctx, 10000, 11000);

	return dsi_ctx.accum_err;
}

static int td4160tm_panel_off(struct td4160tm_panel *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x28);
	mipi_dsi_msleep(&dsi_ctx, 50);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x10);
	mipi_dsi_msleep(&dsi_ctx, 120);

	return dsi_ctx.accum_err;
}

static int td4160tm_panel_prepare(struct drm_panel *panel)
{
	struct td4160tm_panel *ctx = to_td4160tm_panel(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	td4160tm_panel_reset(ctx);

	ret = td4160tm_panel_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		return ret;
	}

	return 0;
}

static int td4160tm_panel_unprepare(struct drm_panel *panel)
{
	struct td4160tm_panel *ctx = to_td4160tm_panel(panel);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);

	return 0;
}

static int td4160tm_panel_disable(struct drm_panel *panel)
{
	struct td4160tm_panel *ctx = to_td4160tm_panel(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = td4160tm_panel_off(ctx);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	return 0;
}

static const struct drm_display_mode td4160tm_panel_mode = {
	.clock = 128000,
	.hdisplay = 720,
	.hsync_start = 720 + 41,
	.hsync_end = 720 + 41 + 2,
	.htotal = 720 + 41 + 2 + 23,
	.vdisplay = 1640,
	.vsync_start = 1640 + 135,
	.vsync_end = 1640 + 135 + 4,
	.vtotal = 1640 + 135 + 4 + 30,
	.width_mm = 69,
	.height_mm = 160,
	.type = DRM_MODE_TYPE_DRIVER,
};

static int td4160tm_panel_get_modes(struct drm_panel *panel,
				       struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &td4160tm_panel_mode);
}

static const struct drm_panel_funcs td4160tm_panel_panel_funcs = {
	.prepare = td4160tm_panel_prepare,
	.unprepare = td4160tm_panel_unprepare,
	.disable = td4160tm_panel_disable,
	.get_modes = td4160tm_panel_get_modes,
};

static int td4160tm_panel_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct td4160tm_panel *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_VIDEO_HSE;
	dsi->hs_rate = 974104000;
	dsi->lp_rate = 20000000;

	drm_panel_init(&ctx->panel, dev, &td4160tm_panel_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);
	ctx->panel.prepare_prev_first = true;

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get backlight\n");

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
	}

	return 0;
}

static void td4160tm_panel_remove(struct mipi_dsi_device *dsi)
{
	struct td4160tm_panel *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id td4160tm_panel_of_match[] = {
	{ .compatible = "omnivision,td4160-tianma-720x1640-panel" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, td4160tm_panel_of_match);

static struct mipi_dsi_driver td4160tm_panel_driver = {
	.probe = td4160tm_panel_probe,
	.remove = td4160tm_panel_remove,
	.driver = {
		.name = "panel-td4160-tm-720x1640",
		.of_match_table = td4160tm_panel_of_match,
	},
};
module_mipi_dsi_driver(td4160tm_panel_driver);

MODULE_AUTHOR("Vladimir Panferov");
MODULE_DESCRIPTION("DRM driver for Omnivision TD4160 Tianma 720x1640 video mode DSI panel");
MODULE_LICENSE("GPL");
