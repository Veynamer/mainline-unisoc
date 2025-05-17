// SPDX-License-Identifier: GPL-2.0
/*
 * Unisoc CSI controller driver
 *
 * Copyright (C) 2025 Otto Pflüger
 */

#include <linux/clk.h>
#include <linux/regmap.h>

#include "camsys.h"
#include "csi-regs.h"

static const u32 sprd_csi_formats[] = {
	MEDIA_BUS_FMT_SBGGR8_1X8,
	MEDIA_BUS_FMT_SGBRG8_1X8,
	MEDIA_BUS_FMT_SGRBG8_1X8,
	MEDIA_BUS_FMT_SRGGB8_1X8,
	MEDIA_BUS_FMT_SBGGR10_1X10,
	MEDIA_BUS_FMT_SGBRG10_1X10,
	MEDIA_BUS_FMT_SGRBG10_1X10,
	MEDIA_BUS_FMT_SRGGB10_1X10,
};

static int sprd_csi_s_power(struct v4l2_subdev *sd, int enable)
{
	struct sprd_csi *csi = container_of(sd, struct sprd_csi, subdev);
	int ret;

	dev_dbg(csi->dev, "%s(%d)\n", __func__, enable);

	if (enable) {
		csi->hw->power_phy(csi, 1);

		ret = clk_prepare_enable(csi->camsys->cphy_cfg_en);
		if (ret)
			return ret;
		ret = clk_prepare_enable(csi->camsys->mipi_gate);
		if (ret)
			return ret;
		ret = clk_prepare_enable(csi->mipi_clk);
		if (ret)
			return ret;
		ret = clk_prepare_enable(csi->clk);
		if (ret)
			return ret;
	} else {
		clk_disable_unprepare(csi->clk);
		clk_disable_unprepare(csi->mipi_clk);
		clk_disable_unprepare(csi->camsys->mipi_gate);
		clk_disable_unprepare(csi->camsys->cphy_cfg_en);
		csi->hw->power_phy(csi, 0);
	}

	return 0;
}

static int sprd_csi_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct sprd_csi *csi = container_of(sd, struct sprd_csi, subdev);
	int ret;

	dev_dbg(csi->dev, "%s(%d)\n", __func__, enable);

	if (!media_pad_remote_pad_first(&csi->pads[SPRD_CSI_PAD_SINK]))
		return -ENOLINK;

	if (enable) {
		csi->hw->connect_dcam(csi, 1);

		writel(0, csi->regs + CSI_RST_CSI2_N);
		writel(1, csi->regs + CSI_RST_CSI2_N);
		writel(1, csi->regs + CSI_PHY_PD_N);
		writel(0, csi->regs + CSI_RST_DPHY_N);
		writel(1, csi->regs + CSI_RST_DPHY_N);

		csi->hw->setup_phy(csi, 1);

		ret = sprd_csi_port_get_lane_count(csi->phy_id);
		if (ret < 0)
			return ret;
		writel(ret - 1, csi->regs + CSI_LANE_NUMBER);
	} else {
		writel(0, csi->regs + CSI_PHY_PD_N);
		csi->hw->setup_phy(csi, 0);
		csi->hw->connect_dcam(csi, 0);
	}

	return 0;
}

static int sprd_csi_enum_mbus_code(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= ARRAY_SIZE(sprd_csi_formats))
		return -EINVAL;

	code->code = sprd_csi_formats[code->index];

	return 0;
}

static int sprd_csi_set_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *sd_state,
			    struct v4l2_subdev_format *fmt)
{
	struct v4l2_mbus_framefmt *sink_fmt, *src_fmt;
	int i;

	sink_fmt = v4l2_subdev_state_get_format(sd_state, SPRD_CSI_PAD_SINK);

	for (i = 0; i < ARRAY_SIZE(sprd_csi_formats); i++) {
		if (sprd_csi_formats[i] == fmt->format.code) {
			sink_fmt->code = fmt->format.code;
			break;
		}
	}

	sink_fmt->width = fmt->format.width;
	sink_fmt->height = fmt->format.height;

	/* Propagate the format to the source pad. */
	src_fmt = v4l2_subdev_state_get_format(sd_state, SPRD_CSI_PAD_SRC);
	*src_fmt = *sink_fmt;

	return 0;
}

static int sprd_csi_init_state(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *sd_state)
{
	struct v4l2_mbus_framefmt *sink_fmt, *src_fmt;

	sink_fmt = v4l2_subdev_state_get_format(sd_state, SPRD_CSI_PAD_SINK);
	src_fmt = v4l2_subdev_state_get_format(sd_state, SPRD_CSI_PAD_SRC);

	sink_fmt->width = 1280;
	sink_fmt->height = 960;
	sink_fmt->field = V4L2_FIELD_NONE;
	sink_fmt->code = sprd_csi_formats[0];

	*src_fmt = *sink_fmt;

	return 0;
}

static int sprd_csi_link_setup(struct media_entity *entity,
			       const struct media_pad *local,
			       const struct media_pad *remote, u32 flags)
{
	struct v4l2_subdev *sd = media_entity_to_v4l2_subdev(entity);
	struct sprd_csi *csi = container_of(sd, struct sprd_csi, subdev);

	if (flags & MEDIA_LNK_FL_ENABLED)
		if (media_pad_remote_pad_first(local))
			return -EBUSY;

	if ((local->flags & MEDIA_PAD_FL_SINK) &&
	    (flags & MEDIA_LNK_FL_ENABLED)) {
		struct v4l2_subdev *remote_sd;
		struct sprd_camsys_sensor_link *sl;

		if (media_pad_remote_pad_first(remote))
			return -EBUSY;

		remote_sd = media_entity_to_v4l2_subdev(remote->entity);
		sl = remote_sd->host_priv;

		dev_dbg(csi->dev, "using PHY %d for %s\n",
			sl->phy_id, remote->entity->name);
		csi->phy_id = sl->phy_id;
	}

	if ((local->flags & MEDIA_PAD_FL_SOURCE) &&
	    (flags & MEDIA_LNK_FL_ENABLED)) {
		struct v4l2_subdev *remote_sd;
		struct sprd_dcam *dcam;

		if (media_pad_remote_pad_first(remote))
			return -EBUSY;

		remote_sd = media_entity_to_v4l2_subdev(remote->entity);
		dcam = container_of(remote_sd, struct sprd_dcam, subdev);

		dev_dbg(csi->dev, "connecting csi%d to dcam%d\n",
			csi->index, dcam->index);
		csi->dcam_id = dcam->index;
	}

	return 0;
}

static const struct v4l2_subdev_core_ops sprd_csi_core_ops = {
	.s_power = sprd_csi_s_power,
};

static const struct v4l2_subdev_video_ops sprd_csi_video_ops = {
	.s_stream = sprd_csi_s_stream,
};

static const struct v4l2_subdev_pad_ops sprd_csi_pad_ops = {
	.enum_mbus_code = sprd_csi_enum_mbus_code,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = sprd_csi_set_fmt,
};

static const struct v4l2_subdev_ops sprd_csi_subdev_ops = {
	.core = &sprd_csi_core_ops,
	.video = &sprd_csi_video_ops,
	.pad = &sprd_csi_pad_ops,
};

static const struct v4l2_subdev_internal_ops sprd_csi_internal_ops = {
	.init_state = sprd_csi_init_state,
};

static const struct media_entity_operations sprd_csi_media_ops = {
	.link_setup = sprd_csi_link_setup,
	.link_validate = v4l2_subdev_link_validate,
};

int sprd_csi_register(struct sprd_camsys *cs, unsigned int index)
{
	struct sprd_csi *csi = &cs->csi[index];
	struct v4l2_subdev *sd = &csi->subdev;
	char name[16];
	int ret;

	csi->hw = cs->hw->csi_ops;
	csi->camsys = cs;
	csi->dev = &cs->pdev->dev;
	csi->index = index;

	v4l2_subdev_init(sd, &sprd_csi_subdev_ops);
	snprintf(sd->name, sizeof(sd->name), "sprd_csi%d", csi->index);

	snprintf(name, sizeof(name), "csi%d", csi->index);
	csi->regs = devm_platform_ioremap_resource_byname(cs->pdev, name);
	if (IS_ERR(csi->regs))
		return PTR_ERR(csi->regs);

	csi->clk = devm_clk_get(csi->dev, name);
	if (IS_ERR(csi->clk))
		return dev_err_probe(csi->dev, PTR_ERR(csi->clk),
				     "cannot get %s clock\n", name);

	snprintf(name, sizeof(name), "mipi_csi%d", csi->index);
	csi->mipi_clk = devm_clk_get(csi->dev, name);
	if (IS_ERR(csi->mipi_clk))
		return dev_err_probe(csi->dev, PTR_ERR(csi->mipi_clk),
				     "cannot get %s clock\n", name);

	sd->internal_ops = &sprd_csi_internal_ops;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sd->entity.ops = &sprd_csi_media_ops;
	sd->entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	sd->owner = THIS_MODULE;

	csi->pads[SPRD_CSI_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	csi->pads[SPRD_CSI_PAD_SRC].flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&sd->entity, SPRD_CSI_PAD_NUM, csi->pads);
	if (ret)
		return ret;

	ret = v4l2_subdev_init_finalize(sd);
	if (ret)
		goto err_cleanup_entity;

	ret = v4l2_device_register_subdev(&cs->v4l2_dev, sd);
	if (ret) {
		dev_err(csi->dev, "failed to register %s\n", sd->name);
		goto err_cleanup_subdev;
	}

	return 0;

err_cleanup_subdev:
	v4l2_subdev_cleanup(sd);
err_cleanup_entity:
	media_entity_cleanup(&sd->entity);
	return ret;
}

void sprd_csi_unregister(struct sprd_camsys *cs, unsigned int index)
{
	struct sprd_csi *csi = &cs->csi[index];

	v4l2_device_unregister_subdev(&csi->subdev);
	v4l2_subdev_cleanup(&csi->subdev);
	media_entity_cleanup(&csi->subdev.entity);
}
