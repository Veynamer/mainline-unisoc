// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Otto Pflüger
 */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/of.h>

#include "rpmsg_internal.h"
#include "sprd_sipc.h"

enum {
	SBLOCK_CMD_INIT = 1,
	SBLOCK_DONE_INIT = 2,
};

enum {
	SBLOCK_EVENT_SEND = 1,
	SBLOCK_EVENT_RELEASE = 2,
};

struct sblock_blk_header {
	__le32 addr;
	__le32 length;
} __packed;

struct sblock_blk_fifo {
	__le32 addr;
	__le32 count;
	__le32 size;
	__le32 blks;
	__le32 rdptr;
	__le32 wrptr;
} __packed;

struct sblock_pool_header {
	struct sblock_blk_fifo tx;
	struct sblock_blk_fifo rx;
} __packed;

struct sblock_smem_header {
	struct sblock_pool_header ring;
	struct sblock_pool_header pool;
} __packed;

struct sipc_sblock {
	struct rpmsg_endpoint ept;
	struct sipc_channel *channel;
	struct sipc_sblock_device *sdev;
	struct sipc_queued_message send_event;
	struct sipc_queued_message release_event;
	bool in_use;
	spinlock_t ept_lock;
	spinlock_t tx_lock;
	wait_queue_head_t block_avail;
	void *smem_virt;
	dma_addr_t smem_addr;
	u32 smem_size;
	u32 tx_blk_count;
	u32 tx_blk_size;
	u32 rx_blk_count;
	u32 rx_blk_size;
	void *tx_pool_virt;
	void *rx_pool_virt;
	void *tx_ring_virt;
	void *rx_ring_virt;
};

struct sipc_sblock_device {
	struct rpmsg_device rpdev;
	struct sipc_sblock *sblock;
};

static void sipc_sblock_destroy_ept(struct rpmsg_endpoint *ept)
{
	struct sipc_sblock *sblock = container_of(ept, struct sipc_sblock, ept);
	unsigned long flags;

	spin_lock_irqsave(&sblock->ept_lock, flags);
	sblock->in_use = false;
	spin_unlock_irqrestore(&sblock->ept_lock, flags);
}

static int __sipc_sblock_send(struct sipc_sblock *sblock,
			      void *data, int length, bool wait)
{
	struct sblock_smem_header *hdr = sblock->smem_virt;
	struct sblock_blk_header *pool_blk, *blk;
	u32 *ring_rdptr, *ring_wrptr, *pool_rdptr, *pool_wrptr;
	u32 ring_i, pool_i, blk_addr;
	unsigned long flags;
	int ret = 0;

	if (!test_bit(SIPC_REMOTE_OPEN, &sblock->channel->state))
		return -EBUSY;

	if (wait) {
		ret = wait_for_completion_interruptible_timeout(
						&sblock->channel->remote_init,
						15 * HZ);
		if (ret < 0)
			return ret;
		if (!ret)
			return -ETIMEDOUT;
	} else if (!completion_done(&sblock->channel->remote_init)) {
		return -EAGAIN;
	}

	if (length > sblock->tx_blk_size)
		return -EINVAL;

	pool_wrptr = &hdr->pool.tx.wrptr;
	pool_rdptr = &hdr->pool.tx.rdptr;
	ring_wrptr = &hdr->ring.tx.wrptr;
	ring_rdptr = &hdr->ring.tx.rdptr;

	spin_lock_irqsave(&sblock->tx_lock, flags);
	while ((pool_i = readl(pool_rdptr)) == readl(pool_wrptr)) {
		spin_unlock_irqrestore(&sblock->tx_lock, flags);

		if (!wait)
			return -EAGAIN;

		ret = wait_event_interruptible_timeout(
						sblock->block_avail,
						readl(pool_wrptr) != pool_i,
						15 * HZ);
		if (ret < 0)
			return ret;
		if (!ret)
			return -ETIMEDOUT;

		spin_lock_irqsave(&sblock->tx_lock, flags);
	}

	pool_blk = sblock->tx_pool_virt +
		   (pool_i % sblock->tx_blk_count) *
		   sizeof(struct sblock_blk_header);
	blk_addr = readl(&pool_blk->addr);

	writel(pool_i + 1, pool_rdptr);

	ring_i = readl(ring_wrptr);
	if ((ring_i - readl(ring_rdptr)) >= sblock->tx_blk_count) {
		dev_err(sblock->channel->parent,
			"channel %d: block available but ring is full\n",
			sblock->channel->id);
		ret = -EIO;
		goto out;
	}

	blk = sblock->tx_ring_virt +
	      (ring_i % sblock->tx_blk_count) *
	      sizeof(struct sblock_blk_header);
	blk->addr = blk_addr;
	blk->length = length;

	memcpy_toio(sblock->smem_virt + (blk->addr - sblock->smem_addr),
		    data, length);

	writel(ring_i + 1, ring_wrptr);

	sipc_queue(sblock->channel->sipc, &sblock->send_event);

	ret = 0;
out:
	spin_unlock_irqrestore(&sblock->tx_lock, flags);
	return ret;
}

static int sipc_sblock_send(struct rpmsg_endpoint *ept, void *data, int len)
{
	struct sipc_sblock *sblock = container_of(ept, struct sipc_sblock, ept);

	return __sipc_sblock_send(sblock, data, len, true);
}

static int sipc_sblock_sendto(struct rpmsg_endpoint *ept, void *data, int len, u32 dst)
{
	struct sipc_sblock *sblock = container_of(ept, struct sipc_sblock, ept);

	return __sipc_sblock_send(sblock, data, len, true);
}

static int sipc_sblock_trysend(struct rpmsg_endpoint *ept, void *data, int len)
{
	struct sipc_sblock *sblock = container_of(ept, struct sipc_sblock, ept);

	return __sipc_sblock_send(sblock, data, len, false);
}

static int sipc_sblock_trysendto(struct rpmsg_endpoint *ept, void *data, int len, u32 dst)
{
	struct sipc_sblock *sblock = container_of(ept, struct sipc_sblock, ept);

	return __sipc_sblock_send(sblock, data, len, false);
}

static ssize_t sipc_sblock_get_mtu(struct rpmsg_endpoint *ept)
{
	struct sipc_sblock *sblock = container_of(ept, struct sipc_sblock, ept);

	return sblock->tx_blk_size;
}

static __poll_t sipc_sblock_poll(struct rpmsg_endpoint *ept,
				 struct file *filp, poll_table *wait)
{
	struct sipc_sblock *sblock = container_of(ept, struct sipc_sblock, ept);
	struct sblock_smem_header *hdr = sblock->smem_virt;

	poll_wait(filp, &sblock->block_avail, wait);

	if (readl(&hdr->pool.tx.wrptr) != readl(&hdr->pool.tx.rdptr))
		return EPOLLOUT | EPOLLWRNORM;

	return 0;
}

static const struct rpmsg_endpoint_ops sipc_sblock_endpoint_ops = {
	.destroy_ept = sipc_sblock_destroy_ept,
	.send = sipc_sblock_send,
	.sendto = sipc_sblock_sendto,
	.trysend = sipc_sblock_trysend,
	.trysendto = sipc_sblock_trysendto,
	.poll = sipc_sblock_poll,
	.get_mtu = sipc_sblock_get_mtu,
};

static void sipc_sblock_rx_msg(struct sipc_channel *channel, u8 type, u16 cmd,
			       u32 value);
static int sipc_sblock_open(struct sipc_channel *channel);
static void sipc_sblock_close(struct sipc_channel *channel);
static void sipc_sblock_free(struct sipc_channel *channel);

struct sipc_channel_ops sipc_sblock_ops = {
	.rx = sipc_sblock_rx_msg,
	.open = sipc_sblock_open,
	.close = sipc_sblock_close,
	.free = sipc_sblock_free,
};

static bool sipc_sblock_rx_block(struct sipc_sblock *sblock)
{
	struct sblock_smem_header *hdr = sblock->smem_virt;
	struct sblock_blk_header *blk, *pool_blk;
	u32 *ring_rdptr, *ring_wrptr, *pool_rdptr, *pool_wrptr;
	u32 ring_i, pool_i, blk_addr, blk_length;
	unsigned long flags;

	pool_rdptr = &hdr->pool.rx.rdptr;
	pool_wrptr = &hdr->pool.rx.wrptr;
	ring_rdptr = &hdr->ring.rx.rdptr;
	ring_wrptr = &hdr->ring.rx.wrptr;

	if ((ring_i = readl(ring_rdptr)) == readl(ring_wrptr))
		return false;

	blk = sblock->rx_ring_virt +
	      (ring_i % sblock->rx_blk_count) *
	      sizeof(struct sblock_blk_header);
	blk_addr = readl(&blk->addr);
	blk_length = readl(&blk->length);

	spin_lock_irqsave(&sblock->ept_lock, flags);
	if (sblock->in_use) {
		sblock->ept.cb(sblock->ept.rpdev,
			       sblock->smem_virt + (blk_addr - sblock->smem_addr),
			       blk_length,
			       sblock->ept.priv,
			       sblock->channel->id);
	} else {
		spin_unlock_irqrestore(&sblock->ept_lock, flags);
		dev_dbg(sblock->channel->parent,
			"not ready to receive data on channel %d\n",
			sblock->channel->id);
		return false;
	}
	spin_unlock_irqrestore(&sblock->ept_lock, flags);

	writel(ring_i + 1, ring_rdptr);

	dev_dbg(sblock->channel->parent,
		"channel %d: block %08x len %08x in rx ring fifo\n",
		sblock->channel->id, blk_addr, blk_length);

	/* return to pool */
	pool_i = readl(pool_wrptr);
	if ((pool_i - readl(pool_rdptr)) >= sblock->rx_blk_count) {
		dev_err(sblock->channel->parent,
			"channel %d: pool full while releasing block\n",
			sblock->channel->id);
		return false;
	}

	pool_blk = sblock->rx_pool_virt +
		   (pool_i % sblock->rx_blk_count) *
		   sizeof(struct sblock_blk_header);
	writel(blk_addr, &pool_blk->addr);
	writel(sblock->rx_blk_size, &pool_blk->length);

	writel(pool_i + 1, pool_wrptr);

	sipc_queue(sblock->channel->sipc, &sblock->release_event);

	return true;
}

static struct rpmsg_endpoint *sipc_sblock_create_ept(struct rpmsg_device *rpdev,
						     rpmsg_rx_cb_t cb, void *priv,
						     struct rpmsg_channel_info chinfo)
{
	struct rpmsg_driver *rpdrv = to_rpmsg_driver(rpdev->dev.driver);
	struct sipc_sblock_device *sdev =
		container_of(rpdev, struct sipc_sblock_device, rpdev);
	struct sipc_sblock *sblock = sdev->sblock;
	struct sipc_channel *channel;
	struct rpmsg_endpoint *ept;
	unsigned long flags;

	if (chinfo.dst != sblock->channel->id &&
	    chinfo.src != sblock->channel->id) {
		channel = sipc_find_channel(sblock->channel->sipc, &chinfo);
		if (IS_ERR(channel)) {
			dev_err(&rpdev->dev,
				"error opening channel %s/%d/%d: %ld\n",
				chinfo.name, chinfo.src, chinfo.dst,
				PTR_ERR(channel));
			return NULL;
		}
		if (channel->ops != &sipc_sblock_ops) {
			dev_err(&rpdev->dev,
				"channel %s/%d/%d is not an sblock channel\n",
				chinfo.name, chinfo.src, chinfo.dst);
			return NULL;
		}
		sblock = channel->priv;
	}

	if (chinfo.src == RPMSG_ADDR_ANY)
		chinfo.src = sblock->channel->id;
	else if (chinfo.src != sblock->channel->id)
		return NULL;

	if (chinfo.dst == RPMSG_ADDR_ANY)
		chinfo.dst = sblock->channel->id;
	else if (chinfo.dst != sblock->channel->id)
		return NULL;

	spin_lock_irqsave(&sblock->ept_lock, flags);
	if (sblock->in_use) {
		dev_err(&rpdev->dev, "channel is in use\n");
		spin_unlock_irqrestore(&sblock->ept_lock, flags);
		return NULL;
	}
	ept = &sblock->ept;
	ept->rpdev = rpdev;
	ept->cb = cb;
	ept->priv = priv;
	ept->ops = &sipc_sblock_endpoint_ops;
	ept->addr = chinfo.dst;
	sblock->in_use = true;
	spin_unlock_irqrestore(&sblock->ept_lock, flags);

	/*
	 * If this endpoint is automatically created before the device
	 * is probed, wait for announce_create(). If not, receive buffered
	 * data immediately.
	 */
	if (!rpdrv->callback) {
		while (sipc_sblock_rx_block(sblock))
			;
	}

	return ept;
}

static int sipc_sblock_announce_create(struct rpmsg_device *rpdev)
{
	struct sipc_sblock_device *sdev =
		container_of(rpdev, struct sipc_sblock_device, rpdev);
	struct sipc_sblock *sblock = sdev->sblock;

	/* Receive buffered data */
	while (sipc_sblock_rx_block(sblock))
		;

	return 0;
}

static const struct rpmsg_device_ops sipc_sblock_rpdev_ops = {
	.create_ept = sipc_sblock_create_ept,
	.announce_create = sipc_sblock_announce_create,
};

static void sipc_sblock_device_release(struct device *dev)
{
	struct rpmsg_device *rpdev = to_rpmsg_device(dev);
	struct sipc_sblock_device *sdev =
		container_of(rpdev, struct sipc_sblock_device, rpdev);

	kfree(sdev);
}

static void sipc_sblock_rx_msg(struct sipc_channel *channel, u8 type, u16 cmd,
			       u32 value)
{
	struct sipc_sblock *sblock = channel->priv;

	if (type == SMSG_TYPE_CMD) {
		if (cmd != SBLOCK_CMD_INIT) {
			dev_warn(channel->parent,
				 "channel %d: unexpected command %04x\n",
				 channel->id, cmd);
			return;
		}
		sipc_send(channel, SMSG_TYPE_DONE, SBLOCK_DONE_INIT,
			  sblock->smem_addr);
		complete_all(&channel->remote_init);
		dev_dbg(channel->parent, "sblock channel %d initialized\n",
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
		while (sipc_sblock_rx_block(sblock))
			;
		break;
	case SBLOCK_EVENT_RELEASE:
		wake_up_interruptible_all(&sblock->block_avail);
		break;
	default:
		dev_warn(channel->parent,
			 "channel %d: unexpected event %04x\n",
			 channel->id, cmd);
		break;
	}
}

static int sipc_sblock_open(struct sipc_channel *channel)
{
	struct sipc_sblock *sblock = channel->priv;
	struct sipc_sblock_device *sdev;
	struct rpmsg_device *rpdev;

	sdev = kzalloc(sizeof(*sdev), GFP_KERNEL);
	if (!sdev)
		return -ENOMEM;

	sdev->sblock = sblock;

	rpdev = &sdev->rpdev;
	strscpy_pad(rpdev->id.name, channel->name, RPMSG_NAME_SIZE);
	rpdev->src = channel->id;
	rpdev->dst = channel->id;
	rpdev->ops = &sipc_sblock_rpdev_ops;
	rpdev->dev.parent = channel->parent;
	rpdev->dev.release = sipc_sblock_device_release;
	rpdev->dev.of_node = channel->node;

	sblock->sdev = sdev;

	return rpmsg_register_device(&sdev->rpdev);
}

static void sipc_sblock_close(struct sipc_channel *channel)
{
	struct sipc_sblock *sblock = channel->priv;

	device_unregister(&sblock->sdev->rpdev.dev);
	sblock->sdev = NULL;
}

static void sipc_sblock_free(struct sipc_channel *channel)
{
	struct sipc_sblock *sblock = channel->priv;

	sipc_cancel(sblock->channel->sipc, &sblock->send_event);
	sipc_cancel(sblock->channel->sipc, &sblock->release_event);

	dma_free_coherent(sblock->channel->parent, sblock->smem_size,
			  sblock->smem_virt, sblock->smem_addr);
	kfree(sblock);
}

int sipc_sblock_init(struct sipc_channel *channel)
{
	u32 tx_blk_count, tx_blk_size, rx_blk_count, rx_blk_size;
	struct sblock_smem_header *hdr;
	struct sblock_blk_header *blk;
	struct sipc_sblock *sblock;
	u32 blk_addr;
	int i, ret;

	if (!channel->name) {
		dev_err(channel->parent,
			"cannot initialize sblock on channel %d without name\n",
			channel->id);
		return -EINVAL;
	}

	ret = of_property_read_u32(channel->node, "sprd,tx-blk-count",
				   &tx_blk_count);
	ret |= of_property_read_u32(channel->node, "sprd,tx-blk-size",
				    &tx_blk_size);
	ret |= of_property_read_u32(channel->node, "sprd,rx-blk-count",
				    &rx_blk_count);
	ret |= of_property_read_u32(channel->node, "sprd,rx-blk-size",
				    &rx_blk_size);
	if (ret) {
		dev_err(channel->parent,
			"invalid or missing sblock properties on channel %d\n",
			channel->id);
		return -EINVAL;
	}

	sblock = kzalloc(sizeof(*sblock), GFP_KERNEL);
	if (!sblock)
		return -ENOMEM;

	spin_lock_init(&sblock->ept_lock);
	spin_lock_init(&sblock->tx_lock);
	init_waitqueue_head(&sblock->block_avail);
	sipc_init_message(&sblock->send_event, channel, SMSG_TYPE_EVENT,
			  SBLOCK_EVENT_SEND, 0);
	sipc_init_message(&sblock->release_event, channel, SMSG_TYPE_EVENT,
			  SBLOCK_EVENT_RELEASE, 0);

	sblock->smem_size = sizeof(struct sblock_smem_header) +
		(tx_blk_count + rx_blk_count) * sizeof(struct sblock_blk_header) +
		tx_blk_size * tx_blk_count + rx_blk_size * rx_blk_count;
	sblock->smem_virt = dma_alloc_coherent(channel->parent,
					       sblock->smem_size,
					       &sblock->smem_addr,
					       GFP_KERNEL);
	if (!sblock->smem_virt) {
		kfree(sblock);
		return -ENOMEM;
	}

	hdr = sblock->smem_virt;
	blk = sblock->smem_virt + sizeof(struct sblock_smem_header);
	blk_addr = sblock->smem_addr + sizeof(struct sblock_smem_header) +
		2 * (tx_blk_count + rx_blk_count) * sizeof(struct sblock_blk_header);

	sblock->tx_blk_count = tx_blk_count;
	sblock->tx_blk_size = tx_blk_size;
	sblock->rx_blk_count = rx_blk_count;
	sblock->rx_blk_size = rx_blk_size;

	sblock->tx_pool_virt = sblock->smem_virt +
			       sizeof(struct sblock_smem_header);

	writel(blk_addr, &hdr->pool.tx.addr);
	writel(tx_blk_count, &hdr->pool.tx.count);
	writel(tx_blk_size, &hdr->pool.tx.size);
	writel(sblock->tx_pool_virt - sblock->smem_virt + sblock->smem_addr,
	       &hdr->pool.tx.blks);
	writel(0, &hdr->pool.tx.rdptr);
	writel(tx_blk_count, &hdr->pool.tx.wrptr);
	for (i = 0; i < tx_blk_count; i++, blk++) {
		writel(blk_addr, &blk->addr);
		writel(tx_blk_size, &blk->length);
		blk_addr += tx_blk_size;
	}

	sblock->rx_pool_virt = sblock->tx_pool_virt +
			       tx_blk_count * sizeof(struct sblock_blk_header);

	writel(blk_addr, &hdr->pool.rx.addr);
	writel(rx_blk_count, &hdr->pool.rx.count);
	writel(rx_blk_size, &hdr->pool.rx.size);
	writel(sblock->rx_pool_virt - sblock->smem_virt + sblock->smem_addr,
	       &hdr->pool.rx.blks);
	writel(0, &hdr->pool.rx.rdptr);
	writel(rx_blk_count, &hdr->pool.rx.wrptr);
	for (i = 0; i < rx_blk_count; i++, blk++) {
		writel(blk_addr, &blk->addr);
		writel(rx_blk_size, &blk->length);
		blk_addr += rx_blk_size;
	}

	sblock->tx_ring_virt = sblock->rx_pool_virt +
			       rx_blk_count * sizeof(struct sblock_blk_header);

	writel(blk_addr, &hdr->ring.tx.addr);
	writel(tx_blk_count, &hdr->ring.tx.count);
	writel(tx_blk_size, &hdr->ring.tx.size);
	writel(sblock->tx_ring_virt - sblock->smem_virt + sblock->smem_addr,
	       &hdr->ring.tx.blks);
	writel(0, &hdr->ring.tx.rdptr);
	writel(0, &hdr->ring.tx.wrptr);

	sblock->rx_ring_virt = sblock->tx_ring_virt +
			       tx_blk_count * sizeof(struct sblock_blk_header);

	writel(blk_addr, &hdr->ring.rx.addr);
	writel(rx_blk_count, &hdr->ring.rx.count);
	writel(rx_blk_size, &hdr->ring.rx.size);
	writel(sblock->rx_ring_virt - sblock->smem_virt + sblock->smem_addr,
	       &hdr->ring.rx.blks);
	writel(0, &hdr->ring.rx.rdptr);
	writel(0, &hdr->ring.rx.wrptr);

	sblock->channel = channel;
	channel->priv = sblock;
	channel->ops = &sipc_sblock_ops;

	return 0;
}
