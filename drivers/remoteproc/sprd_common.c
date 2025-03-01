// SPDX-License-Identifier: GPL-2.0
/*
 * Unisoc remoteproc helpers
 *
 * Copyright (C) 2024 Otto Pflüger
 *
 * Based on parts taken from qcom_common.c
 */

#include <linux/rpmsg/sprd_sipc.h>

#include "sprd_common.h"

#define to_sipc_subdev(_sd) container_of(_sd, struct sprd_sipc_subdev, subdev)

static int sipc_subdev_prepare(struct rproc_subdev *rpsubdev)
{
	struct sprd_sipc_subdev *sd = to_sipc_subdev(rpsubdev);

	sd->sipc = sprd_sipc_register(sd->dev, sd->node);

	return PTR_ERR_OR_ZERO(sd->sipc);
}

static void sipc_subdev_stop(struct rproc_subdev *rpsubdev, bool crashed)
{
	struct sprd_sipc_subdev *sd = to_sipc_subdev(rpsubdev);

	sprd_sipc_unregister(sd->sipc);
	sd->sipc = NULL;
}

static void sipc_subdev_unprepare(struct rproc_subdev *rpsubdev)
{
	struct sprd_sipc_subdev *sd = to_sipc_subdev(rpsubdev);

	/* unregister if stop wasn't called due to a start failure */
	if (sd->sipc) {
		sprd_sipc_unregister(sd->sipc);
		sd->sipc = NULL;
	}
}

void sprd_rproc_add_sipc_subdev(struct rproc *rproc,
				struct device_node *np,
				struct sprd_sipc_subdev *sd)
{
	struct device *dev = &rproc->dev;

	sd->node = of_get_child_by_name(np, "sipc");
	if (!sd->node)
		return;

	sd->dev = dev;
	sd->subdev.prepare = sipc_subdev_prepare;
	sd->subdev.unprepare = sipc_subdev_unprepare;
	sd->subdev.stop = sipc_subdev_stop;

	rproc_add_subdev(rproc, &sd->subdev);
}
EXPORT_SYMBOL_GPL(sprd_rproc_add_sipc_subdev);

void sprd_rproc_remove_sipc_subdev(struct rproc *rproc, struct sprd_sipc_subdev *sd)
{
	if (!sd->node)
		return;

	rproc_remove_subdev(rproc, &sd->subdev);
	of_node_put(sd->node);
}
EXPORT_SYMBOL_GPL(sprd_rproc_remove_sipc_subdev);

MODULE_DESCRIPTION("Unisoc remoteproc helper driver");
MODULE_LICENSE("GPL");
