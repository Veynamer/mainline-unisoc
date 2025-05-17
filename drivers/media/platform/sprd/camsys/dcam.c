// SPDX-License-Identifier: GPL-2.0
/*
 * Unisoc camera frontend driver
 *
 * Copyright (C) 2025 Otto Pflüger
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/reset.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>

#include "camsys.h"

static void sprd_dcam_next_frame(struct sprd_dcam *dcam)
{
	struct sprd_stats_buffer *stats_buf;
	struct sprd_capture_buffer *cap_buf;

	sprd_config_next_frame(dcam);

	stats_buf = sprd_stats_next_frame(&dcam->stats);
	dcam->hw->set_stats_buf(dcam, stats_buf);

	cap_buf = sprd_capture_next_frame(&dcam->capture);
	dcam->hw->set_capture_buf(dcam, cap_buf);

	dev_dbg(dcam->dev, "dcam%d new frame: cap %s, stats %s\n",
		dcam->index, cap_buf ? "on" : "off", stats_buf ? "on" : "off");
}

void sprd_dcam_sof(struct sprd_dcam *dcam)
{
	struct v4l2_event event = {
		.type = V4L2_EVENT_FRAME_SYNC,
	};

	event.u.frame_sync.frame_sequence = dcam->sequence++;
	v4l2_event_queue(dcam->subdev.devnode, &event);

	/*
	 * Prepare for the next frame while the current frame is being
	 * processed.
	 */
	sprd_dcam_next_frame(dcam);
}

static int sprd_dcam_s_power(struct v4l2_subdev *sd, int enable)
{
	struct sprd_dcam *dcam = container_of(sd, struct sprd_dcam, subdev);
	int ret;

	dev_dbg(dcam->dev, "%s(%d)\n", __func__, enable);

	if (enable) {
		ret = clk_prepare_enable(dcam->camsys->dcam_clk);
		if (ret)
			return ret;
	} else {
		clk_disable_unprepare(dcam->camsys->dcam_clk);
	}

	return 0;
}

static int sprd_dcam_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct sprd_dcam *dcam = container_of(sd, struct sprd_dcam, subdev);
	const struct v4l2_mbus_framefmt *sink_fmt;
	struct v4l2_subdev_state *sd_state;

	dev_dbg(dcam->dev, "%s(%d)\n", __func__, enable);

	if (enable) {
		if (!media_pad_remote_pad_first(&dcam->pads[SPRD_DCAM_PAD_INPUT]))
			return -ENOLINK;

		reset_control_assert(dcam->reset);
		udelay(10);
		reset_control_deassert(dcam->reset);

		sd_state = v4l2_subdev_lock_and_get_active_state(sd);
		sink_fmt = v4l2_subdev_state_get_format(sd_state,
							SPRD_DCAM_PAD_INPUT);

		dcam->hw->set_path(dcam, sink_fmt->code,
				   sink_fmt->width, sink_fmt->height);

		v4l2_subdev_unlock_state(sd_state);

		/* Prepare for the first frame. */
		dcam->sequence = 0;
		sprd_dcam_next_frame(dcam);

		dcam->hw->start(dcam);
	} else {
		dcam->hw->stop(dcam);

		reset_control_assert(dcam->reset);
	}

	return 0;
}

static int sprd_dcam_enum_mbus_code(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *sd_state,
				    struct v4l2_subdev_mbus_code_enum *code)
{
	struct sprd_dcam *dcam = container_of(sd, struct sprd_dcam, subdev);
	u32 mbus_code = 0;
	int i, n = -1;

	for (i = 0; i < dcam->camsys->hw->num_formats; i++) {
		if (mbus_code == dcam->camsys->hw->formats[i].mbus_code)
			continue;

		mbus_code = dcam->camsys->hw->formats[i].mbus_code;
		if (++n == code->index) {
			code->code = mbus_code;
			return 0;
		}
	}

	return -EINVAL;
}

static int sprd_dcam_set_fmt(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *sd_state,
			     struct v4l2_subdev_format *fmt)
{
	struct sprd_dcam *dcam = container_of(sd, struct sprd_dcam, subdev);
	struct v4l2_mbus_framefmt *sink_fmt, *src_fmt;
	int i;

	sink_fmt = v4l2_subdev_state_get_format(sd_state, SPRD_DCAM_PAD_INPUT);

	for (i = 0; i < dcam->camsys->hw->num_formats; i++) {
		if (dcam->camsys->hw->formats[i].mbus_code ==
		    fmt->format.code) {
			sink_fmt->code = fmt->format.code;
			break;
		}
	}

	sink_fmt->width = clamp_t(u32, fmt->format.width, 1,
				  dcam->camsys->hw->max_width);
	sink_fmt->height = clamp_t(u32, fmt->format.height, 1,
				   dcam->camsys->hw->max_height);

	/* Propagate the format to the source pad. */
	src_fmt = v4l2_subdev_state_get_format(sd_state, SPRD_DCAM_PAD_CAPTURE);
	*src_fmt = *sink_fmt;

	return 0;
}

static int sprd_dcam_init_state(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state)
{
	struct sprd_dcam *dcam = container_of(sd, struct sprd_dcam, subdev);
	struct v4l2_mbus_framefmt *sink_fmt, *src_fmt;

	sink_fmt = v4l2_subdev_state_get_format(sd_state, SPRD_DCAM_PAD_INPUT);
	src_fmt = v4l2_subdev_state_get_format(sd_state, SPRD_DCAM_PAD_CAPTURE);

	sink_fmt->width = 1280;
	sink_fmt->height = 960;
	sink_fmt->field = V4L2_FIELD_NONE;
	sink_fmt->code = dcam->camsys->hw->formats[0].mbus_code;

	*src_fmt = *sink_fmt;

	sink_fmt = v4l2_subdev_state_get_format(sd_state, SPRD_DCAM_PAD_CONFIG);
	src_fmt = v4l2_subdev_state_get_format(sd_state, SPRD_DCAM_PAD_STATS);

	sink_fmt->width = 0;
	sink_fmt->height = 0;
	sink_fmt->field = V4L2_FIELD_NONE;
	sink_fmt->code = MEDIA_BUS_FMT_METADATA_FIXED;

	*src_fmt = *sink_fmt;

	return 0;
}

static int sprd_dcam_subs_evt(struct v4l2_subdev *sd, struct v4l2_fh *fh,
			      struct v4l2_event_subscription *sub)
{
	if (sub->type != V4L2_EVENT_FRAME_SYNC)
		return -EINVAL;

	/* V4L2_EVENT_FRAME_SYNC doesn't require an id, so zero should be set */
	if (sub->id != 0)
		return -EINVAL;

	return v4l2_event_subscribe(fh, sub, 0, NULL);
}

static const struct v4l2_subdev_internal_ops sprd_dcam_internal_ops = {
	.init_state = sprd_dcam_init_state,
};

static const struct media_entity_operations sprd_dcam_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static const struct v4l2_subdev_core_ops sprd_dcam_core_ops = {
	.s_power = sprd_dcam_s_power,
	.subscribe_event = sprd_dcam_subs_evt,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_video_ops sprd_dcam_video_ops = {
	.s_stream = sprd_dcam_s_stream,
};

static const struct v4l2_subdev_pad_ops sprd_dcam_pad_ops = {
	.enum_mbus_code = sprd_dcam_enum_mbus_code,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = sprd_dcam_set_fmt,
};

static const struct v4l2_subdev_ops sprd_dcam_subdev_ops = {
	.core = &sprd_dcam_core_ops,
	.video = &sprd_dcam_video_ops,
	.pad = &sprd_dcam_pad_ops,
};

int sprd_dcam_register(struct sprd_camsys *cs, unsigned int index)
{
	struct sprd_dcam *dcam = &cs->dcam[index];
	struct v4l2_subdev *sd = &dcam->subdev;
	struct sprd_config_vdev *cfg;
	struct sprd_capture_vdev *cap;
	struct sprd_stats_vdev *stats;
	char name[16];
	int ret;

	dcam->hw = cs->hw->dcam_ops;
	dcam->camsys = cs;
	dcam->dev = &cs->pdev->dev;
	dcam->index = index;

	v4l2_subdev_init(sd, &sprd_dcam_subdev_ops);
	snprintf(sd->name, sizeof(sd->name), "sprd_dcam%d", index);

	snprintf(name, sizeof(name), "dcam%d", index);
	dcam->reset = devm_reset_control_get_exclusive(dcam->dev, name);
	if (IS_ERR(dcam->reset))
		return dev_err_probe(dcam->dev, PTR_ERR(dcam->reset),
				     "failed to get %s reset\n", name);

	ret = platform_get_irq_byname(cs->pdev, name);
	if (ret < 0) {
		dev_err(dcam->dev, "failed to get %s irq\n", name);
		return ret;
	}

	dcam->irq = ret;

	ret = devm_request_irq(dcam->dev, dcam->irq, dcam->hw->handle_irq, 0,
			       sd->name, dcam);
	if (ret < 0) {
		dev_err(dcam->dev, "failed to request irq: %d\n", ret);
		return ret;
	}

	dcam->regs = devm_platform_ioremap_resource_byname(cs->pdev, name);
	if (IS_ERR(dcam->regs))
		return PTR_ERR(dcam->regs);

	sd->internal_ops = &sprd_dcam_internal_ops;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_HAS_EVENTS;
	sd->entity.ops = &sprd_dcam_media_ops;
	sd->entity.function = MEDIA_ENT_F_PROC_VIDEO_ISP;
	sd->owner = THIS_MODULE;

	dcam->pads[SPRD_DCAM_PAD_INPUT].flags = MEDIA_PAD_FL_SINK;
	dcam->pads[SPRD_DCAM_PAD_CONFIG].flags = MEDIA_PAD_FL_SINK;
	dcam->pads[SPRD_DCAM_PAD_CAPTURE].flags = MEDIA_PAD_FL_SOURCE;
	dcam->pads[SPRD_DCAM_PAD_STATS].flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&sd->entity, SPRD_DCAM_PAD_NUM,
				     dcam->pads);
	if (ret)
		return ret;

	ret = v4l2_subdev_init_finalize(sd);
	if (ret)
		goto err_cleanup_entity;

	ret = v4l2_device_register_subdev(&cs->v4l2_dev, sd);
	if (ret) {
		dev_err(dcam->dev, "failed to register %s\n", sd->name);
		goto err_cleanup_subdev;
	}

	cfg = &dcam->config;
	cfg->camsys = cs;
	snprintf(cfg->vdev.name, sizeof(cfg->vdev.name), "sprd_dcam%d_config",
		 dcam->index);

	ret = sprd_config_vdev_register(cfg);
	if (ret)
		goto err_unregister_subdev;

	cap = &dcam->capture;
	cap->camsys = cs;
	snprintf(cap->vdev.name, sizeof(cap->vdev.name), "sprd_dcam%d_full",
		 dcam->index);

	ret = sprd_capture_vdev_register(cap);
	if (ret)
		goto err_unregister_config;

	stats = &dcam->stats;
	stats->camsys = cs;
	snprintf(stats->vdev.name, sizeof(stats->vdev.name),
		 "sprd_dcam%d_stats", dcam->index);

	ret = sprd_stats_vdev_register(stats);
	if (ret)
		goto err_unregister_capture;

	return 0;

err_unregister_capture:
	sprd_capture_vdev_unregister(&dcam->capture);
err_unregister_config:
	sprd_config_vdev_unregister(&dcam->config);
err_unregister_subdev:
	v4l2_device_unregister_subdev(sd);
err_cleanup_subdev:
	v4l2_subdev_cleanup(sd);
err_cleanup_entity:
	media_entity_cleanup(&sd->entity);
	return ret;
}

void sprd_dcam_unregister(struct sprd_camsys *cs, unsigned int index)
{
	struct sprd_dcam *dcam = &cs->dcam[index];

	sprd_stats_vdev_unregister(&dcam->stats);
	sprd_capture_vdev_unregister(&dcam->capture);
	sprd_config_vdev_unregister(&dcam->config);
	v4l2_device_unregister_subdev(&dcam->subdev);
	v4l2_subdev_cleanup(&dcam->subdev);
	media_entity_cleanup(&dcam->subdev.entity);
}
