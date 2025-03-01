// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Otto Pflüger
 */
#ifndef __SPRD_SIPC_H__
#define __SPRD_SIPC_H__

#include <linux/rpmsg.h>
#include <linux/rpmsg/sprd_sipc.h>

#define SMSG_OPEN_MAGIC		0xBEEE
#define SMSG_CLOSE_MAGIC	0xEDDD

struct sprd_sipc;
struct sipc_sipx;

struct sipc_queued_message {
	struct list_head node;
	u8 channel;
	u8 type;
	u16 cmd;
	u32 value;
};

enum {
	SIPC_OPEN,
	SIPC_REMOTE_OPEN,
	SIPC_FORCE_CLOSE,
};

struct sipc_channel {
	refcount_t refcount;
	struct work_struct state_work;
	struct completion remote_init;
	unsigned long state;
	u8 id;
	const char *name;
	struct device *parent;
	struct sprd_sipc *sipc;
	struct device_node *node;
	struct sipc_channel_ops *ops;
	void *priv;
};

struct sipc_channel_ops {
	void (*rx)(struct sipc_channel *channel, u8 type, u16 cmd, u32 value);
	int (*open)(struct sipc_channel *channel);
	void (*close)(struct sipc_channel *channel);
	void (*free)(struct sipc_channel *channel);
};

enum {
	SMSG_TYPE_OPEN = 1,
	SMSG_TYPE_CLOSE,
	SMSG_TYPE_DATA,
	SMSG_TYPE_EVENT,
	SMSG_TYPE_CMD,
	SMSG_TYPE_DONE,
};

int __sipc_send(struct sprd_sipc *sipc, u8 channel, u8 type, u16 cmd,
		u32 value);

static inline int sipc_send(struct sipc_channel *channel, u8 type, u16 cmd,
			    u32 value)
{
	return __sipc_send(channel->sipc, channel->id, type, cmd, value);
}

static inline void sipc_init_message(struct sipc_queued_message *msg,
				     struct sipc_channel *channel, u8 type,
				     u16 cmd, u32 value)
{
	INIT_LIST_HEAD(&msg->node);
	msg->channel = channel->id;
	msg->type = type;
	msg->cmd = cmd;
	msg->value = value;
}

void sipc_queue(struct sprd_sipc *sipc, struct sipc_queued_message *msg);
void sipc_cancel(struct sprd_sipc *sipc, struct sipc_queued_message *msg);

struct sipc_channel *sipc_find_channel(struct sprd_sipc *sipc,
				       const struct rpmsg_channel_info *chinfo);

int sipc_sbuf_init(struct sipc_channel *channel);
int sipc_sblock_init(struct sipc_channel *channel);

struct sipc_sipx *sipx_init(struct device *parent);
int sipx_channel_init(struct sipc_sipx *sipx, struct sipc_channel *channel);
void sipx_destroy(struct sipc_sipx *sipx);

#endif
