// SPDX-License-Identifier: GPL-2.0
#ifndef __RPROC_SPRD_COMMON_H__
#define __RPROC_SPRD_COMMON_H__

#include <linux/remoteproc.h>

#include "remoteproc_internal.h"

struct sprd_sipc;

struct sprd_sipc_subdev {
	struct rproc_subdev subdev;

	struct device *dev;
	struct device_node *node;
	struct sprd_sipc *sipc;
};

void sprd_rproc_add_sipc_subdev(struct rproc *rproc,
				struct device_node *np,
				struct sprd_sipc_subdev *subdev);
void sprd_rproc_remove_sipc_subdev(struct rproc *rproc,
				   struct sprd_sipc_subdev *subdev);

#endif
