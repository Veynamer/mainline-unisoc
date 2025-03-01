// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Otto Pflüger
 */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/of.h>

#include "rpmsg_internal.h"
#include "sprd_sipc.h"

/* Required for modem network interfaces */
#define SIPX_BLOCK_SIZE		1600

enum {
	SBLOCK_CMD_INIT = 1,
	SBLOCK_DONE_INIT = 2,
};

enum {
	SBLOCK_EVENT_SEND = 1,
	SBLOCK_EVENT_RELEASE = 2,
};

#define SIPX_BLK_INDEX	((u32)GENMASK(15, 0))
#define SIPX_BLK_LENGTH	((u32)GENMASK(26, 16))
#define SIPX_BLK_OFFSET	((u32)GENMASK(31, 27))

struct sipx_fifo_header {
	__le32 blks;
	__le32 blk_size;
	__le32 count;
	__le32 rdptr;
	__le32 wrptr;
	__le32 addr;
} __packed;

struct sipx_mem_header {
	struct sipx_fifo_header dl;
	struct sipx_fifo_header dl_ack;
	struct sipx_fifo_header ul;
	struct sipx_fifo_header ul_ack;
} __packed;

struct sipx_shared_mem_header {
	struct sipx_mem_header pool;
	__le32 channel_count;
	__le32 channels[255];
} __packed;

struct sipc_sipx {
	struct device *parent;
	spinlock_t tx_lock;
	wait_queue_head_t block_avail;
	void *smem_virt;
	dma_addr_t smem_addr;
	u32 smem_size;
	u32 dl_blk_count;
	u32 dl_ack_count;
	u32 ul_blk_count;
	u32 ul_ack_count;
	__le32 *dl_pool_virt;
	__le32 *dl_ack_pool_virt;
	__le32 *ul_pool_virt;
	__le32 *ul_ack_pool_virt;
	void *dl_blks;
	void *dl_ack_blks;
	void *ul_blks;
	void *ul_ack_blks;
};

struct sipx_channel {
	struct rpmsg_endpoint ept;
	struct sipc_sipx *sipx;
	struct sipc_channel *channel;
	struct sipx_channel_device *sdev;
	struct sipc_queued_message send_event;
	struct sipc_queued_message release_event;
	bool in_use;
	spinlock_t ept_lock;
	void *smem_virt;
	dma_addr_t smem_addr;
	u32 smem_size;
	__le32 *dl_ring_virt;
	__le32 *dl_ack_ring_virt;
	__le32 *ul_ring_virt;
	__le32 *ul_ack_ring_virt;
};

struct sipx_channel_device {
	struct rpmsg_device rpdev;
	struct sipx_channel *ch;
};

static void sipx_ch_destroy_ept(struct rpmsg_endpoint *ept)
{
	struct sipx_channel *sipx_ch =
		container_of(ept, struct sipx_channel, ept);
	unsigned long flags;

	spin_lock_irqsave(&sipx_ch->ept_lock, flags);
	sipx_ch->in_use = false;
	spin_unlock_irqrestore(&sipx_ch->ept_lock, flags);
}

static int __sipx_ch_send(struct sipx_channel *sipx_ch, void *data, int length,
			  u32 dst, bool wait)
{
	struct sipx_mem_header *pool_hdr = sipx_ch->sipx->smem_virt;
	struct sipx_mem_header *ring_hdr = sipx_ch->smem_virt;
	u32 *ring_rdptr, *ring_wrptr, *pool_rdptr, *pool_wrptr;
	u32 ring_i, pool_i, blk_count, blk_desc;
	unsigned long flags;
	__le32 *pool, *ring;
	void *blks;
	int ret = 0;

	if (!test_bit(SIPC_REMOTE_OPEN, &sipx_ch->channel->state))
		return -EBUSY;

	if (wait) {
		ret = wait_for_completion_interruptible_timeout(
						&sipx_ch->channel->remote_init,
						15 * HZ);
		if (ret < 0)
			return ret;
		if (!ret)
			return -ETIMEDOUT;
	} else if (!completion_done(&sipx_ch->channel->remote_init)) {
		return -EAGAIN;
	}

	if (length > SIPX_BLOCK_SIZE)
		return -EINVAL;

	if (dst & SIPX_ACK_POOL) {
		pool_rdptr = &pool_hdr->ul_ack.rdptr;
		pool_wrptr = &pool_hdr->ul_ack.wrptr;
		ring_rdptr = &ring_hdr->ul_ack.rdptr;
		ring_wrptr = &ring_hdr->ul_ack.wrptr;
		blk_count = sipx_ch->sipx->ul_ack_count;
		pool = sipx_ch->sipx->ul_ack_pool_virt;
		ring = sipx_ch->ul_ack_ring_virt;
		blks = sipx_ch->sipx->ul_ack_blks;
	} else {
		pool_rdptr = &pool_hdr->ul.rdptr;
		pool_wrptr = &pool_hdr->ul.wrptr;
		ring_rdptr = &ring_hdr->ul.rdptr;
		ring_wrptr = &ring_hdr->ul.wrptr;
		blk_count = sipx_ch->sipx->ul_blk_count;
		pool = sipx_ch->sipx->ul_pool_virt;
		ring = sipx_ch->ul_ring_virt;
		blks = sipx_ch->sipx->ul_blks;
	}

	spin_lock_irqsave(&sipx_ch->sipx->tx_lock, flags);
	while ((pool_i = readl(pool_rdptr)) == readl(pool_wrptr)) {
		spin_unlock_irqrestore(&sipx_ch->sipx->tx_lock, flags);

		if (!wait)
			return -EAGAIN;

		ret = wait_event_interruptible_timeout(
						sipx_ch->sipx->block_avail,
						readl(pool_wrptr) != pool_i,
						15 * HZ);
		if (ret < 0)
			return ret;
		if (!ret)
			return -ETIMEDOUT;

		spin_lock_irqsave(&sipx_ch->sipx->tx_lock, flags);
	}

	blk_desc = readl(&pool[pool_i % blk_count]);

	writel(pool_i + 1, pool_rdptr);

	ring_i = readl(ring_wrptr);
	if ((ring_i - readl(ring_rdptr)) >= blk_count) {
		dev_err(sipx_ch->channel->parent,
			"channel %d: block available but ring is full\n",
			sipx_ch->channel->id);
		ret = -EIO;
		goto out;
	}

	blk_desc &= ~(SIPX_BLK_LENGTH | SIPX_BLK_OFFSET);
	blk_desc |= FIELD_PREP(SIPX_BLK_LENGTH, length);

	dev_dbg(sipx_ch->channel->parent,
		"channel %d: tx block %04x len %03x offset %02x from %spool\n",
		sipx_ch->channel->id, FIELD_GET(SIPX_BLK_INDEX, blk_desc),
		FIELD_GET(SIPX_BLK_LENGTH, blk_desc),
		FIELD_GET(SIPX_BLK_OFFSET, blk_desc),
		(dst & SIPX_ACK_POOL) ? "ack " : "");

	memcpy_toio(blks + FIELD_GET(SIPX_BLK_INDEX, blk_desc) *
		    SIPX_BLOCK_SIZE, data, length);

	writel(blk_desc, &ring[ring_i % blk_count]);
	writel(ring_i + 1, ring_wrptr);

	sipc_queue(sipx_ch->channel->sipc, &sipx_ch->send_event);

	ret = 0;
out:
	spin_unlock_irqrestore(&sipx_ch->sipx->tx_lock, flags);
	return ret;
}

static int sipx_ch_send(struct rpmsg_endpoint *ept, void *data, int len)
{
	struct sipx_channel *sipx_ch = container_of(ept, struct sipx_channel, ept);

	return __sipx_ch_send(sipx_ch, data, len, ept->addr, true);
}

static int sipx_ch_sendto(struct rpmsg_endpoint *ept, void *data, int len, u32 dst)
{
	struct sipx_channel *sipx_ch = container_of(ept, struct sipx_channel, ept);

	return __sipx_ch_send(sipx_ch, data, len, dst, true);
}

static int sipx_ch_trysend(struct rpmsg_endpoint *ept, void *data, int len)
{
	struct sipx_channel *sipx_ch = container_of(ept, struct sipx_channel, ept);

	return __sipx_ch_send(sipx_ch, data, len, ept->addr, false);
}

static int sipx_ch_trysendto(struct rpmsg_endpoint *ept, void *data, int len, u32 dst)
{
	struct sipx_channel *sipx_ch = container_of(ept, struct sipx_channel, ept);

	return __sipx_ch_send(sipx_ch, data, len, dst, false);
}

static ssize_t sipx_ch_get_mtu(struct rpmsg_endpoint *ept)
{
	return SIPX_BLOCK_SIZE;
}

static const struct rpmsg_endpoint_ops sipx_ch_endpoint_ops = {
	.destroy_ept = sipx_ch_destroy_ept,
	.send = sipx_ch_send,
	.sendto = sipx_ch_sendto,
	.trysend = sipx_ch_trysend,
	.trysendto = sipx_ch_trysendto,
	.get_mtu = sipx_ch_get_mtu,
};

static void sipx_ch_rx_msg(struct sipc_channel *channel, u8 type, u16 cmd,
			       u32 value);
static int sipx_ch_open(struct sipc_channel *channel);
static void sipx_ch_close(struct sipc_channel *channel);
static void sipx_ch_free(struct sipc_channel *channel);

struct sipc_channel_ops sipx_ch_ops = {
	.rx = sipx_ch_rx_msg,
	.open = sipx_ch_open,
	.close = sipx_ch_close,
	.free = sipx_ch_free,
};

static struct rpmsg_endpoint *sipx_ch_create_ept(struct rpmsg_device *rpdev,
						     rpmsg_rx_cb_t cb, void *priv,
						     struct rpmsg_channel_info chinfo)
{
	struct sipx_channel_device *sdev =
		container_of(rpdev, struct sipx_channel_device, rpdev);
	struct sipx_channel *sipx_ch = sdev->ch;
	struct sipc_channel *channel;
	struct rpmsg_endpoint *ept;
	unsigned long flags;

	if (chinfo.dst != sipx_ch->channel->id &&
	    chinfo.src != sipx_ch->channel->id) {
		channel = sipc_find_channel(sipx_ch->channel->sipc, &chinfo);
		if (IS_ERR(channel)) {
			dev_err(&rpdev->dev,
				"error opening channel %s/%d/%d: %ld\n",
				chinfo.name, chinfo.src, chinfo.dst,
				PTR_ERR(channel));
			return NULL;
		}
		if (channel->ops != &sipx_ch_ops) {
			dev_err(&rpdev->dev,
				"channel %s/%d/%d is not an sipx channel\n",
				chinfo.name, chinfo.src, chinfo.dst);
			return NULL;
		}
		sipx_ch = channel->priv;
	}

	if (chinfo.src == RPMSG_ADDR_ANY)
		chinfo.src = sipx_ch->channel->id;
	else if (chinfo.src != sipx_ch->channel->id)
		return NULL;

	if (chinfo.dst == RPMSG_ADDR_ANY)
		chinfo.dst = sipx_ch->channel->id;
	else if (chinfo.dst != sipx_ch->channel->id)
		return NULL;

	spin_lock_irqsave(&sipx_ch->ept_lock, flags);
	if (sipx_ch->in_use) {
		dev_err(&rpdev->dev, "channel is in use\n");
		spin_unlock_irqrestore(&sipx_ch->ept_lock, flags);
		return NULL;
	}
	ept = &sipx_ch->ept;
	ept->rpdev = rpdev;
	ept->cb = cb;
	ept->priv = priv;
	ept->ops = &sipx_ch_endpoint_ops;
	ept->addr = chinfo.dst;
	sipx_ch->in_use = true;
	spin_unlock_irqrestore(&sipx_ch->ept_lock, flags);

	return ept;
}

static const struct rpmsg_device_ops sipx_ch_rpdev_ops = {
	.create_ept = sipx_ch_create_ept,
};

static void sipx_ch_device_release(struct device *dev)
{
	struct rpmsg_device *rpdev = to_rpmsg_device(dev);
	struct sipx_channel_device *sdev =
		container_of(rpdev, struct sipx_channel_device, rpdev);

	kfree(sdev);
}

static bool sipx_ch_rx_block(struct sipx_channel *sipx_ch, u32 src)
{
	struct sipx_mem_header *pool_hdr = sipx_ch->sipx->smem_virt;
	struct sipx_mem_header *ring_hdr = sipx_ch->smem_virt;
	u32 *ring_rdptr, *ring_wrptr, *pool_rdptr, *pool_wrptr;
	u32 ring_i, pool_i, blk_count, blk_desc;
	__le32 *pool, *ring;
	void *blks;

	if (src & SIPX_ACK_POOL) {
		pool_rdptr = &pool_hdr->dl_ack.rdptr;
		pool_wrptr = &pool_hdr->dl_ack.wrptr;
		ring_rdptr = &ring_hdr->dl_ack.rdptr;
		ring_wrptr = &ring_hdr->dl_ack.wrptr;
		blk_count = sipx_ch->sipx->dl_ack_count;
		pool = sipx_ch->sipx->dl_ack_pool_virt;
		ring = sipx_ch->dl_ack_ring_virt;
		blks = sipx_ch->sipx->dl_ack_blks;
	} else {
		pool_rdptr = &pool_hdr->dl.rdptr;
		pool_wrptr = &pool_hdr->dl.wrptr;
		ring_rdptr = &ring_hdr->dl.rdptr;
		ring_wrptr = &ring_hdr->dl.wrptr;
		blk_count = sipx_ch->sipx->dl_blk_count;
		pool = sipx_ch->sipx->dl_pool_virt;
		ring = sipx_ch->dl_ring_virt;
		blks = sipx_ch->sipx->dl_blks;
	}

	if ((ring_i = readl(ring_rdptr)) == readl(ring_wrptr))
		return false;

	blk_desc = readl(&ring[ring_i % blk_count]);

	writel(ring_i + 1, ring_rdptr);

	dev_dbg(sipx_ch->channel->parent,
		"channel %d: block %04x len %03x offset %02x in dl %sfifo\n",
		sipx_ch->channel->id, FIELD_GET(SIPX_BLK_INDEX, blk_desc),
		FIELD_GET(SIPX_BLK_LENGTH, blk_desc),
		FIELD_GET(SIPX_BLK_OFFSET, blk_desc),
		(src & SIPX_ACK_POOL) ? "ack " : "");

	spin_lock(&sipx_ch->ept_lock);
	if (sipx_ch->in_use) {
		void *blk_addr = blks +
			FIELD_GET(SIPX_BLK_INDEX, blk_desc) * SIPX_BLOCK_SIZE +
			FIELD_GET(SIPX_BLK_OFFSET, blk_desc);

		sipx_ch->ept.cb(sipx_ch->ept.rpdev,
				blk_addr,
				FIELD_GET(SIPX_BLK_LENGTH, blk_desc),
				sipx_ch->ept.priv,
				src);
	}
	spin_unlock(&sipx_ch->ept_lock);

	/* return to pool */
	pool_i = readl(pool_wrptr);
	if ((pool_i - readl(pool_rdptr)) >= blk_count) {
		dev_err(sipx_ch->channel->parent,
			"channel %d: pool full while releasing block\n",
			sipx_ch->channel->id);
		return false;
	}

	writel(blk_desc, &pool[pool_i % blk_count]);
	writel(pool_i + 1, pool_wrptr);

	sipc_queue(sipx_ch->channel->sipc, &sipx_ch->release_event);

	return true;
}

static void sipx_ch_rx_msg(struct sipc_channel *channel, u8 type, u16 cmd,
			   u32 value)
{
	struct sipx_channel *sipx_ch = channel->priv;

	if (type == SMSG_TYPE_CMD) {
		if (cmd != SBLOCK_CMD_INIT) {
			dev_warn(channel->parent,
				 "channel %d: unexpected command %04x\n",
				 channel->id, cmd);
			return;
		}
		sipc_send(channel, SMSG_TYPE_DONE, SBLOCK_DONE_INIT,
			  sipx_ch->sipx->smem_addr);
		complete_all(&channel->remote_init);
		dev_dbg(channel->parent, "sipx channel %d initialized\n",
			channel->id);
		return;
	}

	if (type != SMSG_TYPE_EVENT) {
		dev_warn(channel->parent,
			 "channel %d: unexpected message type %02x\n",
			 channel->id, type);
		return;
	}

	switch (cmd) {
	case SBLOCK_EVENT_SEND:
		/* Continue receiving until read/write pointers meet */
		while (sipx_ch_rx_block(sipx_ch, channel->id))
			;
		while (sipx_ch_rx_block(sipx_ch, channel->id | SIPX_ACK_POOL))
			;
		break;
	case SBLOCK_EVENT_RELEASE:
		wake_up_interruptible_all(&sipx_ch->sipx->block_avail);
		break;
	default:
		dev_warn(channel->parent,
			 "channel %d: unexpected event %04x\n",
			 channel->id, cmd);
		break;
	}
}

static int sipx_ch_open(struct sipc_channel *channel)
{
	struct sipx_channel *sipx_ch = channel->priv;
	struct sipx_channel_device *sdev;
	struct rpmsg_device *rpdev;

	sdev = kzalloc(sizeof(*sdev), GFP_KERNEL);
	if (!sdev)
		return -ENOMEM;

	sdev->ch = sipx_ch;

	rpdev = &sdev->rpdev;
	strscpy_pad(rpdev->id.name, channel->name, RPMSG_NAME_SIZE);
	rpdev->src = channel->id;
	rpdev->dst = channel->id;
	rpdev->ops = &sipx_ch_rpdev_ops;
	rpdev->dev.parent = channel->parent;
	rpdev->dev.release = sipx_ch_device_release;
	rpdev->dev.of_node = channel->node;

	sipx_ch->sdev = sdev;

	return rpmsg_register_device(&sdev->rpdev);
}

static void sipx_ch_close(struct sipc_channel *channel)
{
	struct sipx_channel *sipx_ch = channel->priv;

	device_unregister(&sipx_ch->sdev->rpdev.dev);
	sipx_ch->sdev = NULL;
}

static void sipx_ch_free(struct sipc_channel *channel)
{
	struct sipx_channel *sipx_ch = channel->priv;

	sipc_cancel(sipx_ch->channel->sipc, &sipx_ch->send_event);
	sipc_cancel(sipx_ch->channel->sipc, &sipx_ch->release_event);

	dma_free_coherent(sipx_ch->channel->parent, sipx_ch->smem_size,
			  sipx_ch->smem_virt, sipx_ch->smem_addr);
	kfree(sipx_ch);
}

static void sipx_add_ring(struct sipx_channel *sipx_ch,
			  struct sipx_fifo_header *hdr, __le32 **ring_virt,
			  u32 blk_count)
{

	writel(SIPX_BLOCK_SIZE, &hdr->blk_size);
	writel(blk_count, &hdr->count);
	writel(0, &hdr->rdptr);
	writel(0, &hdr->wrptr);
	writel((void *)*ring_virt - sipx_ch->smem_virt +
	       sipx_ch->smem_addr, &hdr->addr);
}

int sipx_channel_init(struct sipc_sipx *sipx, struct sipc_channel *channel)
{
	struct sipx_shared_mem_header *shared_hdr = sipx->smem_virt;
	struct sipx_mem_header *hdr;
	struct sipx_channel *sipx_ch;
	__le32 *ring_virt;

	if (!channel->name) {
		dev_err(channel->parent,
			"cannot initialize sipx on channel %d without name\n",
			channel->id);
		return -EINVAL;
	}

	sipx_ch = kzalloc(sizeof(*sipx_ch), GFP_KERNEL);
	if (!sipx_ch)
		return -ENOMEM;

	spin_lock_init(&sipx_ch->ept_lock);
	sipc_init_message(&sipx_ch->send_event, channel, SMSG_TYPE_EVENT,
			  SBLOCK_EVENT_SEND, 0);
	sipc_init_message(&sipx_ch->release_event, channel, SMSG_TYPE_EVENT,
			  SBLOCK_EVENT_RELEASE, 0);

	sipx_ch->sipx = sipx;

	sipx_ch->smem_size = sizeof(struct sipx_mem_header) +
			     (sipx->dl_blk_count + sipx->dl_ack_count +
			      sipx->ul_blk_count + sipx->ul_ack_count) * 4;
	sipx_ch->smem_virt = dma_alloc_coherent(channel->parent,
						sipx_ch->smem_size,
						&sipx_ch->smem_addr,
						GFP_KERNEL);
	if (!sipx_ch->smem_virt) {
		kfree(sipx_ch);
		return -ENOMEM;
	}

	hdr = sipx_ch->smem_virt;

	sipx_ch->dl_ring_virt = ring_virt =
		sipx_ch->smem_virt + sizeof(struct sipx_mem_header);
	sipx_add_ring(sipx_ch, &hdr->dl, &ring_virt, sipx->dl_blk_count);

	sipx_ch->dl_ack_ring_virt = ring_virt;
	sipx_add_ring(sipx_ch, &hdr->dl_ack, &ring_virt, sipx->dl_ack_count);

	sipx_ch->ul_ring_virt = ring_virt;
	sipx_add_ring(sipx_ch, &hdr->ul, &ring_virt, sipx->ul_blk_count);

	sipx_ch->ul_ack_ring_virt = ring_virt;
	sipx_add_ring(sipx_ch, &hdr->ul_ack, &ring_virt, sipx->ul_ack_count);

	writel(sipx_ch->smem_addr, &shared_hdr->channels[channel->id]);

	sipx_ch->channel = channel;
	channel->priv = sipx_ch;
	channel->ops = &sipx_ch_ops;

	return 0;
}

static void sipx_add_pool(struct sipc_sipx *sipx, struct sipx_fifo_header *hdr,
			  __le32 **pool_virt, u32 blk_count, u32 *blk_addr)
{
	u32 i;

	writel(*blk_addr, &hdr->blks);
	writel(SIPX_BLOCK_SIZE, &hdr->blk_size);
	writel(blk_count, &hdr->count);
	writel(0, &hdr->rdptr);
	writel(blk_count, &hdr->wrptr);
	writel((void *)*pool_virt - sipx->smem_virt +
	       sipx->smem_addr, &hdr->addr);

	for (i = 0; i < blk_count; i++)
		writel(FIELD_PREP(SIPX_BLK_INDEX, i), (*pool_virt)++);

	*blk_addr += blk_count * SIPX_BLOCK_SIZE;
}

struct sipc_sipx *sipx_init(struct device *parent)
{
	struct sipx_shared_mem_header *hdr;
	struct sipc_sipx *sipx;
	__le32 *pool_virt;
	u32 blk_addr;
	int ret;

	sipx = kzalloc(sizeof(*sipx), GFP_KERNEL);
	if (!sipx)
		return ERR_PTR(-ENOMEM);

	ret = of_property_read_u32(parent->of_node, "sprd,sipx-dl-blk-count",
				   &sipx->dl_blk_count);
	ret |= of_property_read_u32(parent->of_node, "sprd,sipx-dl-ack-count",
				    &sipx->dl_ack_count);
	ret |= of_property_read_u32(parent->of_node, "sprd,sipx-ul-blk-count",
				    &sipx->ul_blk_count);
	ret |= of_property_read_u32(parent->of_node, "sprd,sipx-ul-ack-count",
				    &sipx->ul_ack_count);
	if (ret) {
		dev_err(parent, "invalid or missing sipx properties\n");
		kfree(sipx);
		return ERR_PTR(-EINVAL);
	}

	spin_lock_init(&sipx->tx_lock);
	init_waitqueue_head(&sipx->block_avail);

	sipx->smem_size = sizeof(struct sipx_shared_mem_header) +
			  (sipx->dl_blk_count + sipx->dl_ack_count +
			   sipx->ul_blk_count + sipx->ul_ack_count) *
			  (SIPX_BLOCK_SIZE + 4);
	sipx->smem_virt = dma_alloc_coherent(parent, sipx->smem_size,
					     &sipx->smem_addr, GFP_KERNEL);
	if (!sipx->smem_virt) {
		kfree(sipx);
		return ERR_PTR(-ENOMEM);
	}

	sipx->parent = get_device(parent);

	hdr = sipx->smem_virt;

	pool_virt = sipx->smem_virt + sizeof(*hdr);
	blk_addr = sipx->smem_addr + sizeof(*hdr) +
		   (sipx->dl_blk_count + sipx->dl_ack_count +
		    sipx->ul_blk_count + sipx->ul_ack_count) * 4;

	sipx->dl_pool_virt = pool_virt;
	sipx->dl_blks = sipx->smem_virt + (blk_addr - sipx->smem_addr);
	sipx_add_pool(sipx, &hdr->pool.dl, &pool_virt,
		      sipx->dl_blk_count, &blk_addr);

	sipx->dl_ack_pool_virt = pool_virt;
	sipx->dl_ack_blks = sipx->smem_virt + (blk_addr - sipx->smem_addr);
	sipx_add_pool(sipx, &hdr->pool.dl_ack, &pool_virt,
		      sipx->dl_ack_count, &blk_addr);

	sipx->ul_pool_virt = pool_virt;
	sipx->ul_blks = sipx->smem_virt + (blk_addr - sipx->smem_addr);
	sipx_add_pool(sipx, &hdr->pool.ul, &pool_virt,
		      sipx->ul_blk_count, &blk_addr);

	sipx->ul_ack_pool_virt = pool_virt;
	sipx->ul_ack_blks = sipx->smem_virt + (blk_addr - sipx->smem_addr);
	sipx_add_pool(sipx, &hdr->pool.ul_ack, &pool_virt,
		      sipx->ul_ack_count, &blk_addr);

	writel(ARRAY_SIZE(hdr->channels), &hdr->channel_count);

	return sipx;
}

void sipx_destroy(struct sipc_sipx *sipx)
{
	dma_free_coherent(sipx->parent, sipx->smem_size, sipx->smem_virt,
			  sipx->smem_addr);
	put_device(sipx->parent);
	kfree(sipx);
}
