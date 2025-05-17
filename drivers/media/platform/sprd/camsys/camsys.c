// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unisoc camera subsystem media device driver
 *
 * Copyright (C) 2025 Otto Pflüger
 */

#include <linux/clk.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <media/v4l2-mc.h>
#include <media/v4l2-fwnode.h>

#include "camsys.h"

static int sprd_camsys_link_entities(struct sprd_camsys *cs)
{
	unsigned int i, j;
	int ret;

	for (i = 0; i < SPRD_CSI_NUM; i++) {
		for (j = 0; j < SPRD_DCAM_NUM; j++) {
			ret = media_create_pad_link(
				&cs->csi[i].subdev.entity,
				SPRD_CSI_PAD_SRC,
				&cs->dcam[j].subdev.entity,
				SPRD_DCAM_PAD_INPUT, 0);
			if (ret < 0) {
				dev_err(&cs->pdev->dev,
					"Failed to link CSI%d to DCAM%d\n",
					i, j);
				return ret;
			}
		}
	}

	for (i = 0; i < SPRD_DCAM_NUM; i++) {
		ret = media_create_pad_link(&cs->dcam[i].config.vdev.entity,
					    SPRD_CONFIG_PAD_SRC,
					    &cs->dcam[i].subdev.entity,
					    SPRD_DCAM_PAD_CONFIG,
					    MEDIA_LNK_FL_IMMUTABLE |
					    MEDIA_LNK_FL_ENABLED);
		if (ret < 0) {
			dev_err(&cs->pdev->dev,
				"Failed to link config vdev to DCAM%d\n", i);
			return ret;
		}

		ret = media_create_pad_link(&cs->dcam[i].subdev.entity,
					    SPRD_DCAM_PAD_CAPTURE,
					    &cs->dcam[i].capture.vdev.entity,
					    SPRD_CAPTURE_PAD_SINK,
					    MEDIA_LNK_FL_IMMUTABLE |
					    MEDIA_LNK_FL_ENABLED);
		if (ret < 0) {
			dev_err(&cs->pdev->dev,
				"Failed to link DCAM%d to capture vdev\n", i);
			return ret;
		}

		ret = media_create_pad_link(&cs->dcam[i].subdev.entity,
					    SPRD_DCAM_PAD_STATS,
					    &cs->dcam[i].stats.vdev.entity,
					    SPRD_STATS_PAD_SINK,
					    MEDIA_LNK_FL_IMMUTABLE |
					    MEDIA_LNK_FL_ENABLED);
		if (ret < 0) {
			dev_err(&cs->pdev->dev,
				"Failed to link DCAM%d to stats vdev\n", i);
			return ret;
		}
	}

	return 0;
}

static int sprd_camsys_parse_ports(struct sprd_camsys *cs)
{
	struct device_node *node, *remote;
	struct v4l2_fwnode_endpoint vep = { };
	int ret;

	for_each_endpoint_of_node(cs->mdev.dev->of_node, node) {
		struct sprd_camsys_sensor_link *sl;

		remote = of_graph_get_remote_port_parent(node);
		if (!remote) {
			dev_err(&cs->pdev->dev, "%pOF: cannot get remote\n",
				node);
			ret = -EINVAL;
			goto err_out;
		}

		sl = v4l2_async_nf_add_fwnode(&cs->notifier,
					      of_fwnode_handle(remote),
					      struct sprd_camsys_sensor_link);
		of_node_put(remote);
		if (IS_ERR(sl)) {
			ret = PTR_ERR(sl);
			goto err_out;
		}

		ret = v4l2_fwnode_endpoint_parse(of_fwnode_handle(node), &vep);
		if (ret)
			goto err_out;

		sl->phy_id = vep.base.port;

		ret = sprd_csi_port_get_lane_count(vep.base.port);
		if (ret < 0) {
			dev_err(&cs->pdev->dev, "%pOF: invalid PHY id %d\n",
				node, vep.base.port);
			goto err_out;
		}

		if (vep.bus.mipi_csi2.num_data_lanes != ret) {
			dev_err(&cs->pdev->dev,
				"%pOF: port should have %d data lanes\n",
				node, ret);
			ret = -EINVAL;
			goto err_out;
		}
	}

	return 0;

err_out:
	of_node_put(node);
	return ret;
}

static int sprd_camsys_subdev_bound(struct v4l2_async_notifier *notifier,
				    struct v4l2_subdev *subdev,
				    struct v4l2_async_connection *async)
{
	struct sprd_camsys_sensor_link *sl =
		container_of(async, struct sprd_camsys_sensor_link, async);

	subdev->host_priv = sl;

	return 0;
}

static int sprd_camsys_subdev_complete(struct v4l2_async_notifier *notifier)
{
	struct sprd_camsys *cs =
		container_of(notifier, struct sprd_camsys, notifier);
	struct v4l2_subdev *sd;
	struct media_entity *sensor;
	unsigned int i, sensor_pad;
	int ret;

	list_for_each_entry(sd, &cs->v4l2_dev.subdevs, list) {
		struct sprd_camsys_sensor_link *sl = sd->host_priv;

		if (!sl)
			continue;

		sensor = &sd->entity;

		for (i = 0; i < sensor->num_pads; i++) {
			if (sensor->pads[i].flags & MEDIA_PAD_FL_SOURCE)
				break;
		}
		if (i == sensor->num_pads) {
			dev_err(cs->mdev.dev,
				"No source pad in external entity\n");
			return -EINVAL;
		}
		sensor_pad = i;

		for (i = 0; i < SPRD_CSI_NUM; i++) {
			ret = media_create_pad_link(sensor, sensor_pad,
				&cs->csi[i].subdev.entity,
				SPRD_CSI_PAD_SINK, 0);
			if (ret < 0) {
				dev_err(cs->mdev.dev,
					"Failed to link %s to CSI%d\n",
					sensor->name, i);
				return ret;
			}
		}
	}

	ret = v4l2_device_register_subdev_nodes(&cs->v4l2_dev);
	if (ret < 0)
		return ret;

	return media_device_register(&cs->mdev);
}

static const struct v4l2_async_notifier_operations sprd_camsys_notifier_ops = {
	.bound = sprd_camsys_subdev_bound,
	.complete = sprd_camsys_subdev_complete,
};

static const struct media_device_ops sprd_camsys_media_ops = {
	.link_notify = v4l2_pipeline_link_notify,
};

static int sprd_camsys_probe(struct platform_device *pdev)
{
	struct sprd_camsys *cs;
	unsigned int i;
	int ret;

	cs = devm_kzalloc(&pdev->dev, sizeof(*cs), GFP_KERNEL);
	if (!cs)
		return -ENOMEM;

	cs->hw = of_device_get_match_data(&pdev->dev);
	if (!cs->hw)
		return -EINVAL;

	cs->pdev = pdev;
	platform_set_drvdata(pdev, cs);

	cs->cam_ahb_regs = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						"sprd,syscon-mm-ahb");
	if (IS_ERR(cs->cam_ahb_regs)) {
		dev_err(&pdev->dev, "cannot get MM AHB syscon\n");
		return PTR_ERR(cs->cam_ahb_regs);
	}

	cs->anlg_phy_regs = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						"sprd,syscon-anlg-phy");
	if (IS_ERR(cs->anlg_phy_regs)) {
		dev_err(&pdev->dev, "cannot get PHY syscon\n");
		return PTR_ERR(cs->anlg_phy_regs);
	}

	cs->dcam_clk = devm_clk_get(&pdev->dev, "dcam");
	if (IS_ERR(cs->dcam_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(cs->dcam_clk),
				     "cannot get DCAM clock\n");

	cs->cphy_cfg_en = devm_clk_get(&pdev->dev, "cphy_cfg_en");
	if (IS_ERR(cs->cphy_cfg_en))
		return dev_err_probe(&pdev->dev, PTR_ERR(cs->cphy_cfg_en),
				     "cannot get CPHY_CFG_EN clock\n");

	cs->mipi_gate = devm_clk_get(&pdev->dev, "mipi_gate");
	if (IS_ERR(cs->mipi_gate))
		return dev_err_probe(&pdev->dev, PTR_ERR(cs->mipi_gate),
				     "cannot get MIPI gate clock\n");

	strscpy(cs->mdev.model, "sprd-camsys", sizeof(cs->mdev.model));
	cs->mdev.dev = &pdev->dev;
	cs->mdev.ops = &sprd_camsys_media_ops;
	media_device_init(&cs->mdev);

	strscpy(cs->v4l2_dev.name, "sprd-camsys", sizeof(cs->v4l2_dev.name));
	cs->v4l2_dev.mdev = &cs->mdev;

	ret = v4l2_device_register(&pdev->dev, &cs->v4l2_dev);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to register V4L2 device: %d\n", ret);
		goto err_cleanup_mdev;
	}

	v4l2_async_nf_init(&cs->notifier, &cs->v4l2_dev);
	cs->notifier.ops = &sprd_camsys_notifier_ops;

	ret = sprd_camsys_parse_ports(cs);
	if (ret)
		goto err_unregister_v4l2;

	for (i = 0; i < SPRD_CSI_NUM; i++) {
		ret = sprd_csi_register(cs, i);
		if (ret)
			goto err_unregister_csi;
	}

	for (i = 0; i < SPRD_DCAM_NUM; i++) {
		ret = sprd_dcam_register(cs, i);
		if (ret)
			goto err_unregister_dcam;
	}

	ret = sprd_camsys_link_entities(cs);
	if (ret)
		goto err_unregister_dcam;

	ret = v4l2_async_nf_register(&cs->notifier);
	if (ret) {
		dev_err(&pdev->dev,
			"failed to register async subdev notifier: %d\n",
			ret);
		goto err_unregister_dcam;
	}

	return 0;

err_unregister_dcam:
	while (i--)
		sprd_dcam_unregister(cs, i);
	i = SPRD_CSI_NUM;
err_unregister_csi:
	while (i--)
		sprd_csi_unregister(cs, i);
err_unregister_v4l2:
	v4l2_device_unregister(&cs->v4l2_dev);
err_cleanup_mdev:
	media_device_cleanup(&cs->mdev);

	return ret;
}

static void sprd_camsys_remove(struct platform_device *pdev)
{
	struct sprd_camsys *cs = platform_get_drvdata(pdev);
	unsigned int i;

	v4l2_async_nf_unregister(&cs->notifier);
	v4l2_async_nf_cleanup(&cs->notifier);

	for (i = 0; i < SPRD_DCAM_NUM; i++)
		sprd_dcam_unregister(cs, i);
	for (i = 0; i < SPRD_CSI_NUM; i++)
		sprd_csi_unregister(cs, i);

	v4l2_device_unregister(&cs->v4l2_dev);
	media_device_unregister(&cs->mdev);
	media_device_cleanup(&cs->mdev);
}

static const struct of_device_id sprd_camsys_of_match[] = {
	{ .compatible = "sprd,ums9230-camsys", .data = &ums9230_camsys_info },
	{ }
};
MODULE_DEVICE_TABLE(of, sprd_camsys_of_match);

static struct platform_driver sprd_camsys_driver = {
	.probe = sprd_camsys_probe,
	.remove = sprd_camsys_remove,
	.driver = {
		.name = "sprd-camsys",
		.of_match_table = sprd_camsys_of_match,
	},
};

module_platform_driver(sprd_camsys_driver);

MODULE_DESCRIPTION("Unisoc Camera Subsystem media device driver");
MODULE_LICENSE("GPL");
