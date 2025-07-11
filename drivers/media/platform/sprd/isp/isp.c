// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unisoc ISP driver
 *
 * Copyright (C) 2025 Otto Pflüger
 */

#include <linux/clk.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <uapi/linux/sprd-isp.h>

#include "isp.h"

static void sprd_isp_update_crop(struct sprd_isp *isp,
				 struct sprd_isp_ctx *ctx)
{
	unsigned int slice_width;

	ctx->output.rect.left = 0;
	ctx->output.rect.top = 0;
	ctx->output.total_width = ctx->output.fmt.width;
	ctx->output.rect.height = ctx->output.fmt.height;

	ctx->input.rect.left = ctx->crop_rect.left;
	ctx->input.rect.top = ctx->crop_rect.top;
	ctx->input.total_width = ctx->crop_rect.width;
	ctx->input.rect.height = ctx->crop_rect.height;

	if (ctx->input.total_width > ctx->output.total_width)
		ctx->num_slices = DIV_ROUND_UP(ctx->input.total_width,
					       isp->hw->max_width);
	else
		ctx->num_slices = DIV_ROUND_UP(ctx->output.total_width,
					       isp->hw->max_width);

	ctx->slice_idx = 0;

	slice_width = DIV_ROUND_UP(ctx->input.total_width, ctx->num_slices);
	ctx->input.rect.width = min(ALIGN(slice_width + 16, 2),
				    ctx->input.total_width);

	slice_width = DIV_ROUND_UP(ctx->output.total_width, ctx->num_slices);
	ctx->output.rect.width = min(ALIGN(slice_width + 16, 2),
				     ctx->output.total_width);

	dev_dbg(isp->dev, "new frame: %ux%u -> %ux%u\n",
		ctx->input.total_width, ctx->input.rect.height,
		ctx->output.total_width, ctx->output.rect.height);
	dev_dbg(isp->dev, "slice 1/%u: %u+%u -> %u+%u\n",
		ctx->num_slices, ctx->input.rect.left, ctx->input.rect.width,
		ctx->output.rect.left, ctx->output.rect.width);
}

void sprd_isp_schedule(struct sprd_isp *isp, int ctx_id)
{
	struct sprd_isp_ctx *ctx = &isp->context[ctx_id];
	dma_addr_t in_addr[3], out_addr[3];
	u32 in_pitch[3], out_pitch[3];

	lockdep_assert_held(&isp->lock);

	if (ctx->running)
		return;

	if (sprd_isp_get_addrs(&ctx->input, in_addr, in_pitch) &&
	    sprd_isp_get_addrs(&ctx->output, out_addr, out_pitch)) {
		isp->hw->start_slice(isp, ctx_id, in_addr, out_addr,
				     in_pitch, out_pitch);
		ctx->running = true;
	}
}

void sprd_isp_process(struct sprd_isp *isp, int ctx_id)
{
	struct sprd_isp_ctx *ctx = &isp->context[ctx_id];

	lockdep_assert_held(&isp->lock);

	ctx->slice_idx++;

	if (ctx->slice_idx < ctx->num_slices) {
		unsigned int remaining;

		ctx->input.rect.left = ctx->crop_rect.left +
			ALIGN(ctx->input.total_width *
			      ctx->slice_idx / ctx->num_slices, 2);
		ctx->output.rect.left =
			ALIGN(ctx->output.total_width *
			      ctx->slice_idx / ctx->num_slices, 2);

		remaining = ctx->input.total_width - ctx->input.rect.left;
		if (remaining < ctx->input.rect.width)
			ctx->input.rect.width = remaining;

		remaining = ctx->output.total_width - ctx->output.rect.left;
		if (remaining < ctx->output.rect.width)
			ctx->output.rect.width = remaining;

		dev_dbg(isp->dev,
			"slice %u/%u: %u+%u -> %u+%u\n",
			ctx->slice_idx + 1, ctx->num_slices,
			ctx->input.rect.left, ctx->input.rect.width,
			ctx->output.rect.left, ctx->output.rect.width);
	} else {
		sprd_isp_video_done(&ctx->input);
		sprd_isp_video_done(&ctx->output);
		ctx->sequence++;
		sprd_isp_update_crop(isp, ctx);
		sprd_isp_configure(&ctx->config);
	}

	ctx->running = false;
	sprd_isp_schedule(isp, ctx_id);
}

int sprd_isp_start(struct sprd_isp *isp, int ctx_id)
{
	struct sprd_isp_ctx *ctx = &isp->context[ctx_id];
	int ret;

	lockdep_assert_held(&isp->lock);

	ctx->sequence = 0;

	ret = isp->hw->set_formats(isp, ctx_id);
	if (ret)
		return ret;

	sprd_isp_update_crop(isp, ctx);
	sprd_isp_configure(&ctx->config);
	sprd_isp_schedule(isp, ctx_id);

	return 0;
}

void sprd_isp_try_stop(struct sprd_isp *isp, int ctx_id, int node_idx)
{
	struct sprd_isp_ctx *ctx = &isp->context[ctx_id];
	unsigned long flags;
	int i;

	spin_lock_irqsave(&isp->lock, flags);

	/*
	 * If the context being stopped has pending buffers that are not in
	 * use by the ISP, cancel them immediately.
	 */
	if (!ctx->running) {
		sprd_isp_video_cancel(&ctx->input);
		sprd_isp_video_cancel(&ctx->output);
	}

	ctx->streaming &= ~BIT(node_idx);

	/* Only stop the ISP if no other context is streaming. */
	for (i = 0; i < SPRD_ISP_NUM_CONTEXTS; i++) {
		if (isp->context[i].streaming)
			goto out;
	}

	isp->hw->stop(isp);

	/* Return remaining buffers that were being processed. */
	for (i = 0; i < SPRD_ISP_NUM_CONTEXTS; i++) {
		ctx = &isp->context[i];

		sprd_isp_video_cancel(&ctx->input);
		sprd_isp_video_cancel(&ctx->output);
	}

out:
	spin_unlock_irqrestore(&isp->lock, flags);
}

static const struct v4l2_subdev_pad_ops sprd_isp_pad_ops = {
	.link_validate = v4l2_subdev_link_validate_default,
};

static const struct v4l2_subdev_ops sprd_isp_subdev_ops = {
	.pad = &sprd_isp_pad_ops,
};

static int sprd_isp_ctx_register(struct sprd_isp *isp, int ctx_id)
{
	struct sprd_isp_ctx *ctx = &isp->context[ctx_id];
	struct v4l2_subdev *sd = &ctx->sd;
	struct sprd_isp_config_node *cfg;
	struct sprd_isp_video_node *node;
	int ret;

	ctx->cfg_base = dmam_alloc_coherent(isp->dev, SPRD_ISP_REG_SIZE,
					    &ctx->cfg_dma_addr, GFP_KERNEL);
	if (!ctx->cfg_base)
		return -ENOMEM;

	v4l2_subdev_init(sd, &sprd_isp_subdev_ops);
	sd->entity.function = MEDIA_ENT_F_PROC_VIDEO_ISP;
	sd->owner = THIS_MODULE;
	sd->dev = isp->dev;
	snprintf(sd->name, sizeof(sd->name), "sprd_isp_%d", ctx_id);

	ctx->pad[SPRD_ISP_CONFIG].flags = MEDIA_PAD_FL_SINK;
	ctx->pad[SPRD_ISP_INPUT].flags = MEDIA_PAD_FL_SINK;
	ctx->pad[SPRD_ISP_OUTPUT].flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&sd->entity, SPRD_ISP_NUM_NODES,
				     ctx->pad);
	if (ret)
		return ret;

	ret = v4l2_device_register_subdev(&isp->v4l2_dev, sd);
	if (ret)
		goto err_cleanup_entity;

	cfg = &ctx->config;
	cfg->isp = isp;
	cfg->ctx_id = ctx_id;
	snprintf(cfg->vdev.name, sizeof(cfg->vdev.name),
		 "%s_config", sd->name);
	ret = sprd_isp_config_register(cfg);
	if (ret)
		goto err_unregister_subdev;

	node = &ctx->input;
	node->isp = isp;
	node->ctx_id = ctx_id;
	node->index = SPRD_ISP_INPUT;
	node->vdev.device_caps = V4L2_CAP_VIDEO_OUTPUT_MPLANE | V4L2_CAP_STREAMING;
	node->vdev.vfl_dir = VFL_DIR_TX;
	node->vb2_queue.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	snprintf(node->vdev.name, sizeof(node->vdev.name),
		 "%s_input", sd->name);
	ret = sprd_isp_video_register(node);
	if (ret)
		goto err_unregister_config;

	node = &ctx->output;
	node->isp = isp;
	node->ctx_id = ctx_id;
	node->index = SPRD_ISP_OUTPUT;
	node->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_STREAMING;
	node->vdev.vfl_dir = VFL_DIR_RX;
	node->vb2_queue.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	snprintf(node->vdev.name, sizeof(node->vdev.name),
		 "%s_output", sd->name);
	ret = sprd_isp_video_register(node);
	if (ret)
		goto err_unregister_input;

	return 0;

err_unregister_input:
	sprd_isp_video_unregister(&ctx->input);
err_unregister_config:
	sprd_isp_config_unregister(&ctx->config);
err_unregister_subdev:
	v4l2_device_unregister_subdev(sd);
err_cleanup_entity:
	media_entity_cleanup(&sd->entity);
	return ret;
}

static void sprd_isp_ctx_unregister(struct sprd_isp *isp, int ctx_id)
{
	struct sprd_isp_ctx *ctx = &isp->context[ctx_id];

	sprd_isp_video_unregister(&ctx->output);
	sprd_isp_video_unregister(&ctx->input);
	sprd_isp_config_unregister(&ctx->config);
	v4l2_device_unregister_subdev(&ctx->sd);
	media_entity_cleanup(&ctx->sd.entity);
}

static int sprd_isp_probe(struct platform_device *pdev)
{
	struct sprd_isp *isp;
	int i, ret;

	isp = devm_kzalloc(&pdev->dev, sizeof(*isp), GFP_KERNEL);
	if (!isp)
		return -ENOMEM;

	spin_lock_init(&isp->lock);
	isp->dev = &pdev->dev;

	platform_set_drvdata(pdev, isp);

	isp->hw = of_device_get_match_data(&pdev->dev);
	if (!isp->hw)
		return -EINVAL;

	isp->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(isp->base))
		return PTR_ERR(isp->base);

	for (i = 0; i < SPRD_ISP_NUM_IRQS; i++) {
		char name[32];

		isp->irq[i] = platform_get_irq(pdev, i);
		if (isp->irq[i] <= 0)
			return -EINVAL;

		snprintf(name, sizeof(name), "sprd-isp-ch%d", i);

		ret = devm_request_irq(&pdev->dev, isp->irq[i],
				       isp->hw->handle_irq,
				       0, name, isp);
		if (ret) {
			dev_err(&pdev->dev, "failed to request irq[%d]\n", i);
			return ret;
		}
	}

	isp->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(isp->clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(isp->clk),
				     "failed to get clock");

	ret = clk_prepare_enable(isp->clk);
	if (ret) {
		dev_err(&pdev->dev, "failed to enable clock\n");
		return ret;
	}

	isp->reset = devm_reset_control_get_optional(&pdev->dev, "core");
	if (IS_ERR(isp->reset)) {
		ret = dev_err_probe(&pdev->dev, PTR_ERR(isp->reset),
				    "failed to get core reset");
		goto err_disable_clk;
	}

	isp->ahb_reset = devm_reset_control_get_optional(&pdev->dev, "ahb");
	if (IS_ERR(isp->ahb_reset)) {
		ret = dev_err_probe(&pdev->dev, PTR_ERR(isp->ahb_reset),
				    "failed to get AHB reset");
		goto err_disable_clk;
	}

	isp->mdev.dev = &pdev->dev;
	strscpy(isp->mdev.model, "sprd-isp", sizeof(isp->mdev.model));
	media_device_init(&isp->mdev);

	isp->v4l2_dev.mdev = &isp->mdev;
	strscpy(isp->v4l2_dev.name, "sprd-isp", sizeof(isp->v4l2_dev.name));

	ret = v4l2_device_register(&pdev->dev, &isp->v4l2_dev);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register V4L2 device\n");
		goto err_cleanup_mdev;
	}

	for (i = 0; i < SPRD_ISP_NUM_CONTEXTS; i++) {
		ret = sprd_isp_ctx_register(isp, i);
		if (ret)
			goto err_unregister_contexts;
	}

	ret = media_device_register(&isp->mdev);
	if (ret)
		goto err_unregister_contexts;

	isp->hw->init_regs(isp);

	pm_runtime_set_autosuspend_delay(&pdev->dev, 200);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_set_active(&pdev->dev);
	ret = devm_pm_runtime_enable(&pdev->dev);
	if (ret)
		goto err_unregister_mdev;

	return 0;

err_unregister_mdev:
	media_device_unregister(&isp->mdev);
err_unregister_contexts:
	while (i-- > 0)
		sprd_isp_ctx_unregister(isp, i);
	v4l2_device_unregister(&isp->v4l2_dev);
err_cleanup_mdev:
	media_device_cleanup(&isp->mdev);
err_disable_clk:
	clk_disable_unprepare(isp->clk);
	return ret;
}

static void sprd_isp_remove(struct platform_device *pdev)
{
	struct sprd_isp *isp = platform_get_drvdata(pdev);
	int i;

	media_device_unregister(&isp->mdev);

	for (i = SPRD_ISP_NUM_CONTEXTS - 1; i >= 0; i--)
		sprd_isp_ctx_unregister(isp, i);

	v4l2_device_unregister(&isp->v4l2_dev);
	media_device_cleanup(&isp->mdev);
}

static int sprd_isp_runtime_suspend(struct device *dev)
{
	struct sprd_isp *isp = dev_get_drvdata(dev);

	clk_disable_unprepare(isp->clk);

	return 0;
}

static int sprd_isp_runtime_resume(struct device *dev)
{
	struct sprd_isp *isp = dev_get_drvdata(dev);
	int ret;

	ret = clk_prepare_enable(isp->clk);
	if (ret) {
		dev_err(dev, "failed to enable clock\n");
		return ret;
	}

	isp->hw->init_regs(isp);

	return 0;
}

static const struct dev_pm_ops sprd_isp_pm_ops = {
	SET_RUNTIME_PM_OPS(sprd_isp_runtime_suspend,
			   sprd_isp_runtime_resume, NULL)
};

static const struct of_device_id sprd_isp_of_match[] = {
	{ .compatible = "sprd,ums9230-isp", .data = &ums9230_isp_hw },
	{ }
};
MODULE_DEVICE_TABLE(of, sprd_isp_of_match);

static struct platform_driver sprd_isp_driver = {
	.probe = sprd_isp_probe,
	.remove = sprd_isp_remove,
	.driver = {
		.name = "sprd-isp",
		.of_match_table = sprd_isp_of_match,
		.pm = &sprd_isp_pm_ops,
	},
};

module_platform_driver(sprd_isp_driver);

MODULE_DESCRIPTION("Unisoc ISP driver");
MODULE_LICENSE("GPL");
