// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Otto Pflüger
 */

#include <linux/dma-mapping.h>
#include <linux/idr.h>
#include <linux/kthread.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>

#include "sprd_sipc.h"

struct sipc_msg {
	u8 channel;
	u8 type;
	u16 cmd;
	u32 value;
} __packed;

struct sprd_sipc {
	struct device dev;
	struct mbox_client mbox_client;
	struct mbox_chan *mbox_chan;
	struct mbox_chan *mbox_oob_chan;
	struct workqueue_struct *wq;

	struct sipc_sipx *sipx;
	struct idr channels;
	bool exiting;

	spinlock_t queue_lock;
	struct list_head queue;
	bool tx_in_progress;
};

int __sipc_send(struct sprd_sipc *sipc, u8 channel, u8 type, u16 cmd, u32 value)
{
	struct sipc_msg *msg;
	int ret;

	msg = kmalloc(sizeof(*msg), GFP_ATOMIC);
	msg->channel = channel;
	msg->type = type;
	msg->cmd = cmd;
	msg->value = value;

	ret = mbox_send_message(sipc->mbox_chan, msg);
	if (ret < 0) {
		dev_err(&sipc->dev,
			"failed to send %02x %04x on channel %d: %d\n",
			type, cmd, channel, ret);
		kfree(msg);
		return ret;
	}

	return 0;
}

void sipc_queue(struct sprd_sipc *sipc, struct sipc_queued_message *msg)
{
	unsigned long flags;

	spin_lock_irqsave(&sipc->queue_lock, flags);
	if (list_empty(&msg->node)) {
		if (sipc->tx_in_progress) {
			list_add_tail(&msg->node, &sipc->queue);
		} else {
			__sipc_send(sipc, msg->channel, msg->type, msg->cmd,
				    msg->value);
			sipc->tx_in_progress = true;
		}
	}
	spin_unlock_irqrestore(&sipc->queue_lock, flags);
}

void sipc_cancel(struct sprd_sipc *sipc, struct sipc_queued_message *msg)
{
	unsigned long flags;

	spin_lock_irqsave(&sipc->queue_lock, flags);
	if (!list_empty(&msg->node))
		list_del_init(&msg->node);
	spin_unlock_irqrestore(&sipc->queue_lock, flags);
}

static void sipc_channel_state_work(struct work_struct *work)
{
	struct sipc_channel *channel =
		container_of(work, struct sipc_channel, state_work);

	if (!test_bit(SIPC_FORCE_CLOSE, &channel->state) &&
	    test_bit(SIPC_REMOTE_OPEN, &channel->state) &&
	    !test_and_set_bit(SIPC_OPEN, &channel->state)) {
		if (channel->ops) {
			int ret = channel->ops->open(channel);
			if (ret) {
				dev_err(channel->parent,
					"failed to open channel %d: %d\n",
					channel->id, ret);
			}
		}

		dev_dbg(channel->parent, "channel %d: opened\n", channel->id);
	}

	if ((test_bit(SIPC_FORCE_CLOSE, &channel->state) ||
	     !test_bit(SIPC_REMOTE_OPEN, &channel->state)) &&
	    test_and_clear_bit(SIPC_OPEN, &channel->state)) {
		if (channel->ops)
			channel->ops->close(channel);

		reinit_completion(&channel->remote_init);

		dev_dbg(channel->parent, "channel %d: closed\n", channel->id);
	}
}

static void sipc_free_channel(struct sipc_channel *channel)
{
	if (channel->ops)
		channel->ops->free(channel);
	of_node_put(channel->node);
	kfree(channel);
}

static struct sipc_channel *sipc_alloc_channel(struct sprd_sipc *sipc,
					       u8 id, struct device_node *node,
					       const char *name)
{
	struct sipc_channel *channel;

	channel = kzalloc(sizeof(*channel), GFP_KERNEL);
	if (!channel)
		return NULL;

	channel->parent = &sipc->dev;
	channel->sipc = sipc;
	channel->id = id;
	channel->node = of_node_get(node);
	channel->name = name;
	init_completion(&channel->remote_init);
	INIT_WORK(&channel->state_work, sipc_channel_state_work);

	return channel;
}

struct sipc_channel *sipc_find_channel(struct sprd_sipc *sipc,
				       const struct rpmsg_channel_info *chinfo)
{
	struct sipc_channel *channel;
	int id;

	idr_for_each_entry(&sipc->channels, channel, id) {
		if ((channel->name && !strncmp(chinfo->name,
					       channel->name,
					       RPMSG_NAME_SIZE)) ||
		    chinfo->dst == id || chinfo->src == id) {
			return channel;
		}
	}

	return ERR_PTR(-ENODEV);
}

static void sipc_rx_callback(struct mbox_client *mbox_client, void *msg_data)
{
	struct sprd_sipc *sipc =
		container_of(mbox_client, struct sprd_sipc, mbox_client);
	struct sipc_msg *msg = msg_data;
	struct sipc_channel *channel;

	channel = idr_find(&sipc->channels, msg->channel);

	if (msg->type == SMSG_TYPE_OPEN) {
		dev_dbg(&sipc->dev, "remote side is opening channel %d\n",
			msg->channel);

		if (channel) {
			if (!test_and_set_bit(SIPC_REMOTE_OPEN,
					      &channel->state))
				queue_work(sipc->wq, &channel->state_work);

			sipc_send(channel, SMSG_TYPE_OPEN, SMSG_OPEN_MAGIC, 0);
		}
	} else if (msg->type == SMSG_TYPE_CLOSE) {
		dev_dbg(&sipc->dev, "remote side is closing channel %d\n",
			msg->channel);

		if (channel) {
			if (test_and_clear_bit(SIPC_REMOTE_OPEN,
					       &channel->state))
				queue_work(sipc->wq, &channel->state_work);
		}
	} else if (channel && channel->ops) {
		channel->ops->rx(channel, msg->type,
				 msg->cmd, msg->value);
	}
}

static void sipc_tx_done(struct mbox_client *mbox_client, void *msg_data, int r)
{
	struct sprd_sipc *sipc =
		container_of(mbox_client, struct sprd_sipc, mbox_client);
	struct sipc_msg *msg = msg_data;

	if (r)
		dev_warn(mbox_client->dev,
			 "message %02x %04x on channel %d was not delivered: %d\n",
			 msg->type, msg->cmd, msg->channel, r);

	kfree(msg_data);

	spin_lock(&sipc->queue_lock);
	if (list_empty(&sipc->queue)) {
		sipc->tx_in_progress = false;
	} else {
		struct sipc_queued_message *qmsg =
			list_first_entry(&sipc->queue,
					 struct sipc_queued_message,
					 node);

		list_del_init(&qmsg->node);
		__sipc_send(sipc, qmsg->channel, qmsg->type, qmsg->cmd,
			    qmsg->value);
	}
	spin_unlock(&sipc->queue_lock);
}

static int sipc_populate(struct sprd_sipc *sipc)
{
	struct sipc_channel *channel;
	struct device_node *child;
	const char *name, *chtype;
	int ret;
	u32 id;

	for_each_available_child_of_node(sipc->dev.of_node, child) {
		ret = of_property_read_u32(child, "reg", &id);
		if (ret)
			continue;

		ret = of_property_read_string(child, "sprd,channel-type",
					      &chtype);
		if (ret)
			continue;

		ret = of_property_read_string(child, "sprd,channel-name",
					      &name);
		if (ret)
			name = NULL;

		channel = sipc_alloc_channel(sipc, id, child, name);
		if (!channel)
			return -ENOMEM;

		if (!strcmp(chtype, "sbuf")) {
			ret = sipc_sbuf_init(channel);
		} else if (!strcmp(chtype, "sblock")) {
			ret = sipc_sblock_init(channel);
		} else if (!strcmp(chtype, "sipx")) {
			ret = 0;
			if (!sipc->sipx) {
				sipc->sipx = sipx_init(&sipc->dev);
				if (IS_ERR(sipc->sipx)) {
					ret = PTR_ERR(sipc->sipx);
					sipc->sipx = NULL;
				}
			}
			if (!ret)
				ret = sipx_channel_init(sipc->sipx, channel);
		} else {
			ret = 0;
		}

		if (ret) {
			dev_err(&sipc->dev,
				"failed to initialize channel %d: %d\n",
				id, ret);
			sipc_free_channel(channel);
			continue;
		}

		ret = idr_alloc(&sipc->channels, channel, id, id + 1,
				GFP_KERNEL);
		if (ret < 0) {
			dev_err(&sipc->dev, "duplicate channel ID %d?\n", id);
			return -EINVAL;
		}
	}

	return 0;
}

static void sipc_depopulate(struct sprd_sipc *sipc)
{
	struct sipc_channel *channel;
	int id;

	idr_for_each_entry(&sipc->channels, channel, id) {
		if (!test_and_set_bit(SIPC_FORCE_CLOSE, &channel->state))
			queue_work(sipc->wq, &channel->state_work);
	}

	flush_workqueue(sipc->wq);

	idr_for_each_entry(&sipc->channels, channel, id) {
		if (channel->ops)
			channel->ops->free(channel);
	}
	idr_destroy(&sipc->channels);

	if (sipc->sipx) {
		sipx_destroy(sipc->sipx);
		sipc->sipx = NULL;
	}
}

static void sprd_sipc_release(struct device *dev)
{
	struct sprd_sipc *sipc = container_of(dev, struct sprd_sipc, dev);

	WARN_ON(!idr_is_empty(&sipc->channels));
	WARN_ON(!list_empty(&sipc->queue));

	mbox_free_channel(sipc->mbox_chan);
	mbox_free_channel(sipc->mbox_oob_chan);

	of_reserved_mem_device_release(dev);
	destroy_workqueue(sipc->wq);
	kfree(sipc);
}

struct sprd_sipc *sprd_sipc_register(struct device *parent, struct device_node *node)
{
	struct sprd_sipc *sipc;
	struct device *dev;
	int ret, count;

	sipc = kzalloc(sizeof(*sipc), GFP_KERNEL);
	if (!sipc)
		return ERR_PTR(-ENOMEM);

	spin_lock_init(&sipc->queue_lock);
	idr_init(&sipc->channels);
	INIT_LIST_HEAD(&sipc->queue);

	sipc->wq = alloc_ordered_workqueue("%s-%pOFn", 0, dev_name(parent), node);
	if (!sipc->wq) {
		kfree(sipc);
		return ERR_PTR(-ENOMEM);
	}

	dev = &sipc->dev;
	dev->parent = parent;
	dev->release = sprd_sipc_release;
	dev->of_node = node;
	dev->coherent_dma_mask = DMA_BIT_MASK(32);
	dev->dma_mask = &dev->coherent_dma_mask;
	dev_set_name(dev, "%s:%pOFn", dev_name(parent), node);

	ret = device_register(dev);
	if (ret) {
		dev_err(dev, "failed to register\n");
		put_device(dev);
		return ERR_PTR(ret);
	}

	ret = of_dma_configure(dev, node->parent, true);
	if (ret) {
		dev_err(dev, "failed to initialize address mapping: %d\n", ret);
		goto unregister;
	}

	ret = of_reserved_mem_device_init(dev);
	if (ret) {
		dev_err(dev, "failed to initialize shared memory: %d\n", ret);
		goto unregister;
	}

	sipc->mbox_client.dev = dev;
	sipc->mbox_client.rx_callback = sipc_rx_callback;
	sipc->mbox_client.tx_done = sipc_tx_done;

	count = of_count_phandle_with_args(node, "mboxes", "#mbox-cells");
	if (count <= 0) {
		dev_err(dev, "no mailboxes specified\n");
		ret = count ?: -ENOENT;
		goto unregister;
	}

	if (count >= 2) {
		sipc->mbox_oob_chan = mbox_request_channel(&sipc->mbox_client, 1);
		if (IS_ERR(sipc->mbox_oob_chan)) {
			ret = PTR_ERR(sipc->mbox_oob_chan);
			sipc->mbox_oob_chan = NULL;
			dev_err(dev, "failed to request supplementary mailbox: %d\n", ret);
			goto unregister;
		}
	}

	sipc->mbox_chan = mbox_request_channel(&sipc->mbox_client, 0);
	if (IS_ERR(sipc->mbox_chan)) {
		ret = PTR_ERR(sipc->mbox_chan);
		sipc->mbox_chan = NULL;
		dev_err(dev, "failed to request mailbox: %d\n", ret);
		goto unregister;
	}

	ret = sipc_populate(sipc);
	if (ret) {
		dev_err(dev, "failed to populate channels: %d\n", ret);
		goto depopulate;
	}

	return sipc;

depopulate:
	sipc_depopulate(sipc);
unregister:
	device_unregister(dev);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(sprd_sipc_register);

void sprd_sipc_unregister(struct sprd_sipc *sipc)
{
	sipc_depopulate(sipc);
	device_unregister(&sipc->dev);
}
EXPORT_SYMBOL_GPL(sprd_sipc_unregister);

MODULE_DESCRIPTION("Spreadtrum IPC driver");
MODULE_LICENSE("GPL");
