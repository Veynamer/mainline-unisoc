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
	SBUF_CMD_INIT = 1,
	SBUF_DONE_INIT = 2,
};

enum {
	SBUF_EVENT_WRPTR = 1,
	SBUF_EVENT_RDPTR = 2,
};

struct sbuf_buffer_info {
	__le32 addr;
	__le32 size;
	__le32 rdptr;
	__le32 wrptr;
} __packed;

struct sbuf_ring_header {
	struct sbuf_buffer_info tx;
	struct sbuf_buffer_info rx;
} __packed;

struct sbuf_smem_header {
	__le32 num_rings;
	struct sbuf_ring_header rings[];
} __packed;

struct sipc_sbuf {
	struct device dev;
	struct sipc_channel *channel;
	struct sipc_queued_message *rdptr_event;
	struct sipc_queued_message *wrptr_event;
	u32 num_buffers;
	void *rx_buffer;
	void *smem_virt;
	dma_addr_t smem_addr;
	u32 smem_size;
	u32 tx_buf_size;
	u32 rx_buf_size;

	spinlock_t devices_lock;
	struct idr devices;
};

struct sipc_sbuf_device {
	struct rpmsg_device rpdev;
	struct rpmsg_endpoint ept;
	struct sipc_sbuf *sbuf;
	bool in_use;
	spinlock_t ept_lock;
	spinlock_t tx_lock;
	wait_queue_head_t tx_wait;
};

#define rpdev_to_sbuf_device(d) container_of(d, struct sipc_sbuf_device, rpdev)
#define ept_to_sbuf_device(e) container_of(e, struct sipc_sbuf_device, ept)

static void sipc_sbuf_destroy_ept(struct rpmsg_endpoint *ept)
{
	struct sipc_sbuf_device *sdev = ept_to_sbuf_device(ept);
	unsigned long flags;

	spin_lock_irqsave(&sdev->ept_lock, flags);
	sdev->in_use = false;
	spin_unlock_irqrestore(&sdev->ept_lock, flags);
}

static int __sipc_sbuf_send(struct sipc_sbuf_device *sdev,
			    void *data, int total, bool wait)
{
	struct sipc_sbuf *sbuf = sdev->sbuf;
	struct sbuf_smem_header *hdr = sbuf->smem_virt;
	struct sbuf_ring_header *ring = &hdr->rings[sdev->rpdev.dst];
	ktime_t timeout = ktime_add_us(ktime_get(), 15000000);
	u32 pos, offset, taillen;
	unsigned long flags;
	void *fifo;

	if (!test_bit(SIPC_REMOTE_OPEN, &sbuf->channel->state))
		return -EBUSY;

	if (wait) {
		int ret = wait_for_completion_interruptible_timeout(
						&sbuf->channel->remote_init,
						15 * HZ);
		if (ret < 0)
			return ret;
		if (!ret)
			return -ETIMEDOUT;
	} else if (!completion_done(&sbuf->channel->remote_init)) {
		return -EAGAIN;
	}

	if (total > sbuf->tx_buf_size)
		return -EINVAL;

	spin_lock_irqsave(&sdev->tx_lock, flags);
	pos = readl(&ring->tx.wrptr);
	while (pos + total > readl(&ring->tx.rdptr) + sbuf->tx_buf_size) {
		spin_unlock_irqrestore(&sdev->tx_lock, flags);

		if (!wait)
			return -EAGAIN;
		/*
		 * The remote processor only sends RDPTR events when the FIFO
		 * is completely full, so busy-polling is needed here since
		 * the rpmsg API does not allow partial writes.
		 */
		if (ktime_compare(ktime_get(), timeout) > 0)
			return -ETIMEDOUT;

		usleep_range(10000, 100000);

		spin_lock_irqsave(&sdev->tx_lock, flags);
	}

	fifo = sbuf->smem_virt + readl(&ring->tx.addr) - sbuf->smem_addr;
	offset = pos % sbuf->tx_buf_size;
	taillen = min_t(u32, total, sbuf->tx_buf_size - offset);

	memcpy_toio(fifo + offset, data, taillen);
	memcpy_toio(fifo, data + taillen, total - taillen);

	pos += total;
	writel(pos, &ring->tx.wrptr);

	sipc_queue(sbuf->channel->sipc, &sbuf->wrptr_event[sdev->rpdev.dst]);

	spin_unlock_irqrestore(&sdev->tx_lock, flags);

	return 0;
}

static int sipc_sbuf_send(struct rpmsg_endpoint *ept, void *data, int len)
{
	struct sipc_sbuf_device *sdev = ept_to_sbuf_device(ept);

	return __sipc_sbuf_send(sdev, data, len, true);
}

static int sipc_sbuf_sendto(struct rpmsg_endpoint *ept, void *data,
			    int len, u32 dst)
{
	struct sipc_sbuf_device *sdev = ept_to_sbuf_device(ept);

	return __sipc_sbuf_send(sdev, data, len, true);
}

static int sipc_sbuf_trysend(struct rpmsg_endpoint *ept, void *data, int len)
{
	struct sipc_sbuf_device *sdev = ept_to_sbuf_device(ept);

	return __sipc_sbuf_send(sdev, data, len, false);
}

static int sipc_sbuf_trysendto(struct rpmsg_endpoint *ept, void *data,
			       int len, u32 dst)
{
	struct sipc_sbuf_device *sdev = ept_to_sbuf_device(ept);

	return __sipc_sbuf_send(sdev, data, len, false);
}

static __poll_t sipc_sbuf_poll(struct rpmsg_endpoint *ept,
			       struct file *filp, poll_table *wait)
{
	struct sipc_sbuf_device *sdev = ept_to_sbuf_device(ept);
	struct sbuf_smem_header *hdr = sdev->sbuf->smem_virt;
	struct sbuf_ring_header *ring = &hdr->rings[sdev->rpdev.dst];

	poll_wait(filp, &sdev->tx_wait, wait);

	/*
	 * The remote side does not send any RDPTR events when the FIFO is
	 * not full, so we can only check whether the FIFO is full here.
	 * If not enough space is available, trysend() will return -EAGAIN.
	 * Unfortunately, this may cause busy loops if the FIFO is only
	 * partially full.
	 */
	if ((readl(&ring->tx.wrptr) - readl(&ring->tx.rdptr)) <
	    sdev->sbuf->tx_buf_size)
		return EPOLLOUT | EPOLLWRNORM;

	return 0;
}

static ssize_t sipc_sbuf_get_mtu(struct rpmsg_endpoint *ept)
{
	struct sipc_sbuf_device *sdev = ept_to_sbuf_device(ept);
	struct sipc_sbuf *sbuf = sdev->sbuf;

	return sbuf->tx_buf_size;
}

static const struct rpmsg_endpoint_ops sipc_sbuf_endpoint_ops = {
	.destroy_ept = sipc_sbuf_destroy_ept,
	.send = sipc_sbuf_send,
	.sendto = sipc_sbuf_sendto,
	.trysend = sipc_sbuf_trysend,
	.trysendto = sipc_sbuf_trysendto,
	.poll = sipc_sbuf_poll,
	.get_mtu = sipc_sbuf_get_mtu,
};

static bool sipc_sbuf_try_rx(struct sipc_sbuf *sbuf,
			     struct sipc_sbuf_device *sdev, int id)
{
	struct sbuf_smem_header *hdr = sbuf->smem_virt;
	struct sbuf_ring_header *ring = &hdr->rings[id];
	u32 pos, offset, taillen, total;
	unsigned long flags;
	void *fifo;

	pos = readl(&ring->rx.rdptr);
	total = readl(&ring->rx.wrptr) - pos;
	if (total == 0)
		return false;

	dev_dbg(&sbuf->dev, "rx %08x len %08x in fifo %d\n", pos, total, id);

	if (total > sbuf->rx_buf_size) {
		dev_err(&sbuf->dev, "FIFO %d overrun\n", id);
		writel(readl(&ring->rx.wrptr), &ring->rx.rdptr);
		sipc_queue(sbuf->channel->sipc, &sbuf->rdptr_event[id]);
		return false;
	}

	fifo = sbuf->smem_virt + (readl(&ring->rx.addr) - sbuf->smem_addr);
	offset = pos % sbuf->rx_buf_size;
	taillen = min(total, sbuf->rx_buf_size - offset);

	if (!sdev) {
		spin_lock_irqsave(&sbuf->devices_lock, flags);
		sdev = idr_find(&sbuf->devices, id);
		spin_unlock_irqrestore(&sbuf->devices_lock, flags);
	}

	if (sdev) {
		spin_lock_irqsave(&sdev->ept_lock, flags);
		if (sdev->in_use) {
			memcpy_fromio(sbuf->rx_buffer, fifo + offset, taillen);
			memcpy_fromio(sbuf->rx_buffer + taillen, fifo,
				      total - taillen);
			sdev->ept.cb(&sdev->rpdev, sbuf->rx_buffer, total,
				     sdev->ept.priv, id);
		} else {
			spin_unlock_irqrestore(&sdev->ept_lock, flags);
			dev_dbg(&sbuf->dev,
				"not ready to receive data from %d\n", id);
			return false;
		}
		spin_unlock_irqrestore(&sdev->ept_lock, flags);
	} else {
		dev_dbg(&sbuf->dev, "channel %d not regsistered yet\n", id);
		return false;
	}

	writel(pos + total, &ring->rx.rdptr);
	/*
	 * If the remote side has written more data to the FIFO and does not
	 * see the rdptr update, it will not send another wrptr event. Ensure
	 * that the receiving loop may only exit when all writes to rdptr are
	 * seen by the remote side. Even with the memory barrier in place, it
	 * seems that some memory updates can arrive late, so add a small
	 * delay just in case.
	 */
	mb();
	udelay(20);

	/* If the FIFO was full, tell the remote side to continue writing */
	if (readl(&ring->rx.wrptr) - pos >= sbuf->rx_buf_size)
		sipc_queue(sbuf->channel->sipc, &sbuf->rdptr_event[id]);

	return true;
}

static struct rpmsg_endpoint *sipc_sbuf_create_ept(struct rpmsg_device *rpdev,
						   rpmsg_rx_cb_t cb, void *priv,
						   struct rpmsg_channel_info chinfo)
{
	struct rpmsg_driver *rpdrv = to_rpmsg_driver(rpdev->dev.driver);
	struct sipc_sbuf_device *sdev = rpdev_to_sbuf_device(rpdev);
	struct rpmsg_endpoint *ept;
	unsigned long flags;

	if ((chinfo.src != RPMSG_ADDR_ANY && chinfo.src != rpdev->src) ||
	    (chinfo.dst != RPMSG_ADDR_ANY && chinfo.dst != rpdev->dst)) {
		dev_err(&rpdev->dev, "custom endpoint address not allowed\n");
		return NULL;
	}

	chinfo.src = rpdev->src;
	chinfo.dst = rpdev->dst;

	spin_lock_irqsave(&sdev->ept_lock, flags);
	if (sdev->in_use) {
		dev_err(&rpdev->dev, "channel is in use\n");
		spin_unlock_irqrestore(&sdev->ept_lock, flags);
		return NULL;
	}
	ept = &sdev->ept;
	ept->rpdev = rpdev;
	ept->cb = cb;
	ept->priv = priv;
	ept->ops = &sipc_sbuf_endpoint_ops;
	ept->addr = chinfo.src;
	sdev->in_use = true;
	spin_unlock_irqrestore(&sdev->ept_lock, flags);

	/*
	 * If this endpoint is automatically created before the device
	 * is probed, wait for announce_create(). If not, receive buffered
	 * data immediately.
	 */
	if (!rpdrv->callback) {
		while (sipc_sbuf_try_rx(sdev->sbuf, sdev, chinfo.src))
			;
	}

	return ept;
}

static int sipc_sbuf_announce_create(struct rpmsg_device *rpdev)
{
	struct sipc_sbuf_device *sdev = rpdev_to_sbuf_device(rpdev);

	/* Receive buffered data */
	while (sipc_sbuf_try_rx(sdev->sbuf, sdev, rpdev->src))
		;

	return 0;
}

static const struct rpmsg_device_ops sipc_sbuf_rpdev_ops = {
	.create_ept = sipc_sbuf_create_ept,
	.announce_create = sipc_sbuf_announce_create,
};

static void sipc_sbuf_rpdev_release(struct device *dev)
{
	struct rpmsg_device *rpdev = to_rpmsg_device(dev);
	struct sipc_sbuf_device *sdev = rpdev_to_sbuf_device(rpdev);

	kfree(rpdev->driver_override);
	kfree(sdev);
}

static void sipc_sbuf_tx_avail_notify(struct sipc_sbuf *sbuf, int id)
{
	struct sipc_sbuf_device *sdev;

	spin_lock(&sbuf->devices_lock);
	sdev = idr_find(&sbuf->devices, id);
	spin_unlock(&sbuf->devices_lock);
	if (!sdev)
		return;

	wake_up_interruptible_all(&sdev->tx_wait);
}

static void sipc_sbuf_rx_msg(struct sipc_channel *channel, u8 type, u16 cmd,
			     u32 value)
{
	struct sipc_sbuf *sbuf = channel->priv;

	if (type == SMSG_TYPE_CMD) {
		if (cmd != SBUF_CMD_INIT) {
			dev_warn(&sbuf->dev, "unexpected command %04x\n", cmd);
			return;
		}
		sipc_send(channel, SMSG_TYPE_DONE, SBUF_DONE_INIT, sbuf->smem_addr);
		complete_all(&channel->remote_init);
		dev_dbg(&sbuf->dev, "initialized\n");
		return;
	}

	if (type != SMSG_TYPE_EVENT) {
		dev_warn(&sbuf->dev, "unexpected message type %02x\n", type);
		return;
	}

	if (value > sbuf->num_buffers) {
		dev_warn(&sbuf->dev, "unexpected buffer ID %d\n", value);
		return;
	}

	switch (cmd) {
	case SBUF_EVENT_WRPTR:
		/* Continue receiving until read/write pointers meet */
		while (sipc_sbuf_try_rx(sbuf, NULL, value))
			;
		break;
	case SBUF_EVENT_RDPTR:
		sipc_sbuf_tx_avail_notify(sbuf, value);
		break;
	default:
		dev_warn(&sbuf->dev, "unexpected event %04x\n", cmd);
		break;
	}
}

static int sipc_sbuf_open(struct sipc_channel *channel)
{
	struct sipc_sbuf *sbuf = channel->priv;
	struct sipc_sbuf_device *sdev;
	struct rpmsg_device *rpdev;
	struct device_node *child;
	unsigned long flags;
	u32 rx_buf, tx_buf;
	const char *name;
	int ret;

	for_each_available_child_of_node(sbuf->channel->node, child) {
		ret = of_property_read_u32_index(child, "reg", 0, &rx_buf);
		if (ret) {
			dev_warn(&sbuf->dev, "%pOF: cannot read reg property\n",
				 child);
			continue;
		}

		ret = of_property_read_u32_index(child, "reg", 1, &tx_buf);
		if (ret)
			tx_buf = rx_buf;

		if (rx_buf >= sbuf->num_buffers || tx_buf >= sbuf->num_buffers) {
			dev_warn(&sbuf->dev, "%pOF: buffer ID out of range\n",
				 child);
			continue;
		}

		ret = of_property_read_string(child, "sprd,channel-name", &name);
		if (ret) {
			dev_warn(&sbuf->dev, "%pOF: cannot read channel name\n",
				 child);
			continue;
		}

		sdev = kzalloc(sizeof(*sdev), GFP_KERNEL);
		if (!sdev) {
			ret = -ENOMEM;
			goto unregister;
		}

		spin_lock_init(&sdev->ept_lock);
		spin_lock_init(&sdev->tx_lock);
		init_waitqueue_head(&sdev->tx_wait);
		sdev->sbuf = sbuf;

		rpdev = &sdev->rpdev;
		strscpy_pad(rpdev->id.name, name, RPMSG_NAME_SIZE);
		rpdev->src = rx_buf;
		rpdev->dst = tx_buf;
		rpdev->ops = &sipc_sbuf_rpdev_ops;

		rpdev->dev.of_node = child;
		rpdev->dev.parent = &sbuf->dev;
		rpdev->dev.release = sipc_sbuf_rpdev_release;

		ret = rpmsg_register_device(rpdev);
		if (ret) {
			dev_err(&sbuf->dev, "failed to register %d/%d\n",
				rx_buf, tx_buf);
			/* rpmsg_register_device calls put_device on error */
			continue;
		}

		spin_lock_irqsave(&sbuf->devices_lock, flags);
		ret = idr_alloc(&sbuf->devices, sdev, rx_buf, rx_buf + 1,
				GFP_ATOMIC);
		spin_unlock_irqrestore(&sbuf->devices_lock, flags);
		if (ret < 0) {
			device_unregister(&rpdev->dev);
			goto unregister;
		}
	}

	return 0;

unregister:
	spin_lock_irqsave(&sbuf->devices_lock, flags);
	idr_for_each_entry(&sbuf->devices, sdev, rx_buf)
		device_unregister(&sdev->rpdev.dev);
	idr_destroy(&sbuf->devices);
	spin_unlock_irqrestore(&sbuf->devices_lock, flags);

	return ret;
}

static void sipc_sbuf_close(struct sipc_channel *channel)
{
	struct sipc_sbuf *sbuf = channel->priv;
	struct sipc_sbuf_device *sdev;
	unsigned long flags;
	int id;

	spin_lock_irqsave(&sbuf->devices_lock, flags);
	idr_for_each_entry(&sbuf->devices, sdev, id) {
		idr_remove(&sbuf->devices, id);
		spin_unlock_irqrestore(&sbuf->devices_lock, flags);
		device_unregister(&sdev->rpdev.dev);
		spin_lock_irqsave(&sbuf->devices_lock, flags);
	}
	spin_unlock_irqrestore(&sbuf->devices_lock, flags);
}

static void sipc_sbuf_free(struct sipc_channel *channel)
{
	struct sipc_sbuf *sbuf = channel->priv;
	int id;

	for (id = 0; id < sbuf->num_buffers; id++) {
		sipc_cancel(channel->sipc, &sbuf->wrptr_event[id]);
		sipc_cancel(channel->sipc, &sbuf->rdptr_event[id]);
	}

	device_unregister(&sbuf->dev);
}

static void sipc_sbuf_release(struct device *dev)
{
	struct sipc_sbuf *sbuf = container_of(dev, struct sipc_sbuf, dev);

	dma_free_coherent(sbuf->dev.parent, sbuf->smem_size, sbuf->smem_virt,
			  sbuf->smem_addr);
	kfree(sbuf->rx_buffer);
	kfree(sbuf);
}

struct sipc_channel_ops sipc_sbuf_ops = {
	.rx = sipc_sbuf_rx_msg,
	.open = sipc_sbuf_open,
	.close = sipc_sbuf_close,
	.free = sipc_sbuf_free,
};

int sipc_sbuf_init(struct sipc_channel *channel)
{
	u32 buf_addr, tx_buf_size, rx_buf_size;
	struct sbuf_ring_header *ring;
	struct sbuf_smem_header *hdr;
	struct sipc_sbuf *sbuf;
	int i, ret;

	sbuf = kzalloc(sizeof(*sbuf), GFP_KERNEL);
	if (!sbuf)
		return -ENOMEM;

	device_initialize(&sbuf->dev);
	sbuf->dev.parent = channel->parent;
	sbuf->dev.release = sipc_sbuf_release;
	sbuf->dev.of_node = channel->node;
	dev_set_name(&sbuf->dev, "%s.%d.sbuf", dev_name(sbuf->dev.parent),
		     channel->id);
	sbuf->channel = channel;
	channel->priv = sbuf;

	spin_lock_init(&sbuf->devices_lock);
	idr_init(&sbuf->devices);

	ret = of_property_read_u32(channel->node, "sprd,tx-buf-size",
				   &tx_buf_size);
	ret |= of_property_read_u32(channel->node, "sprd,rx-buf-size",
				    &rx_buf_size);
	if (ret) {
		dev_err(&sbuf->dev, "invalid or missing sbuf properties\n");
		put_device(&sbuf->dev);
		return -EINVAL;
	}

	ret = of_property_read_u32(channel->node, "sprd,num-buffers",
				   &sbuf->num_buffers);
	if (ret || !sbuf->num_buffers)
		sbuf->num_buffers = 1;

	sbuf->rdptr_event = kmalloc_array(sbuf->num_buffers,
					  sizeof(struct sipc_queued_message),
					  GFP_KERNEL);
	if (!sbuf->rdptr_event) {
		put_device(&sbuf->dev);
		return -ENOMEM;
	}

	sbuf->wrptr_event = kmalloc_array(sbuf->num_buffers,
					  sizeof(struct sipc_queued_message),
					  GFP_KERNEL);
	if (!sbuf->wrptr_event) {
		put_device(&sbuf->dev);
		return -ENOMEM;
	}

	for (i = 0; i < sbuf->num_buffers; i++) {
		sipc_init_message(&sbuf->rdptr_event[i], channel,
				  SMSG_TYPE_EVENT, SBUF_EVENT_RDPTR, i);
		sipc_init_message(&sbuf->wrptr_event[i], channel,
				  SMSG_TYPE_EVENT, SBUF_EVENT_WRPTR, i);
	}

	sbuf->rx_buffer = kmalloc(rx_buf_size, GFP_KERNEL);
	if (!sbuf->rx_buffer) {
		put_device(&sbuf->dev);
		return -ENOMEM;
	}

	sbuf->smem_size = sizeof(struct sbuf_smem_header) +
			  sizeof(struct sbuf_ring_header) * sbuf->num_buffers +
			  (tx_buf_size + rx_buf_size) * sbuf->num_buffers;
	sbuf->smem_virt = dma_alloc_coherent(sbuf->dev.parent, sbuf->smem_size,
					     &sbuf->smem_addr, GFP_KERNEL);
	if (!sbuf->smem_virt) {
		put_device(&sbuf->dev);
		return -ENOMEM;
	}

	sbuf->tx_buf_size = tx_buf_size;
	sbuf->rx_buf_size = rx_buf_size;

	hdr = sbuf->smem_virt;
	writel(sbuf->num_buffers, &hdr->num_rings);
	buf_addr = sbuf->smem_addr + sizeof(struct sbuf_smem_header) +
		   sizeof(struct sbuf_ring_header) * sbuf->num_buffers;
	for (i = 0, ring = hdr->rings; i < sbuf->num_buffers; i++, ring++) {
		writel(buf_addr, &ring->tx.addr);
		writel(tx_buf_size, &ring->tx.size);
		writel(0, &ring->tx.rdptr);
		writel(0, &ring->tx.wrptr);
		buf_addr += tx_buf_size;
		writel(buf_addr, &ring->rx.addr);
		writel(rx_buf_size, &ring->rx.size);
		writel(0, &ring->rx.rdptr);
		writel(0, &ring->rx.wrptr);
		buf_addr += rx_buf_size;
	}

	ret = device_add(&sbuf->dev);
	if (ret) {
		put_device(&sbuf->dev);
		return ret;
	}

	channel->ops = &sipc_sbuf_ops;

	return 0;
}
