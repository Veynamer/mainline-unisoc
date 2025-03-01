// SPDX-License-Identifier: GPL-2.0
#ifndef _LINUX_RPMSG_SPRD_SIPC_H
#define _LINUX_RPMSG_SPRD_SIPC_H

#include <linux/device.h>

/*
 * This bit can be set in the destination or source address for an SIPX channel
 * and indicates that the alternative ACK pool is used.
 */
#define SIPX_ACK_POOL BIT(31)

struct sprd_sipc;

#if IS_ENABLED(CONFIG_RPMSG_SPRD_SIPC)

struct sprd_sipc *sprd_sipc_register(struct device *parent, struct device_node *node);
void sprd_sipc_unregister(struct sprd_sipc *sipc);

#else

static inline struct sprd_sipc *
sprd_sipc_register(struct device *parent, struct device_node *node)
{
	return NULL;
}

static inline void sprd_sipc_unregister(struct sprd_sipc *sipc)
{
}

#endif

#endif
