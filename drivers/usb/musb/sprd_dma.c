// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 Otto Pflüger
 *
 * Based on Spreadtrum downstream code,
 *  Copyright (c) 2018 Spreadtrum Co., Ltd.
 * and on musb_dma.c,
 *  Copyright 2005 Mentor Graphics Corporation
 *  Copyright (C) 2005-2007 by Texas Instruments
 */
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include "musb_core.h"
#include "sprd_dma.h"

#define MAX_NODES_PER_CHANNEL 2048

struct sprd_musb_dma_channel {
	struct dma_channel		channel;
	struct sprd_musb_dma_controller	*controller;
	u32				start_addr;
	u32				len;
	u8				idx;
	u8				epnum;
	u8				transmit;
};

struct sprd_musb_dma_controller {
	struct dma_controller		controller;
	struct sprd_musb_dma_channel	channel[MUSB_DMA_CHANNELS+1];
	void				*private_data;
	void __iomem			*base;
	u32				used_channels;
	void				*linklist;
	dma_addr_t 			linklist_dma_addr;
};

static void dma_channel_release(struct dma_channel *channel);

static void dma_controller_stop(struct sprd_musb_dma_controller *controller)
{
	struct musb *musb = controller->private_data;
	struct dma_channel *channel;
	u8 bit;

	if (controller->used_channels != 0) {
		dev_err(musb->controller,
			"Stopping DMA controller while channel active\n");

		for (bit = 1; bit <= MUSB_DMA_CHANNELS; bit++) {
			if (controller->used_channels & (1 << bit)) {
				channel = &controller->channel[bit].channel;
				dma_channel_release(channel);

				if (!controller->used_channels)
					break;
			}
		}
	}
}

static struct dma_channel *dma_channel_allocate(struct dma_controller *c,
				struct musb_hw_ep *hw_ep, u8 transmit)
{
	struct sprd_musb_dma_controller *controller = container_of(c,
			struct sprd_musb_dma_controller, controller);
	struct sprd_musb_dma_channel *musb_channel = NULL;
	struct dma_channel *channel = NULL;
	u8 bit = hw_ep->epnum + (transmit ? 0 : 15);

	if (controller->used_channels & (1 << bit))
		return NULL;

	controller->used_channels |= (1 << bit);
	musb_channel = &(controller->channel[bit]);
	musb_channel->controller = controller;
	musb_channel->idx = bit;
	musb_channel->epnum = hw_ep->epnum;
	musb_channel->transmit = transmit;
	channel = &(musb_channel->channel);
	channel->private_data = musb_channel;
	channel->status = MUSB_DMA_STATUS_FREE;
	channel->max_len = 0xfffc * MAX_NODES_PER_CHANNEL + 3;
	/* Tx => mode 1; Rx => mode 0 */
	channel->desired_mode = transmit;
	channel->actual_len = 0;

	return channel;
}

static void dma_channel_release(struct dma_channel *channel)
{
	struct sprd_musb_dma_channel *musb_channel = channel->private_data;

	channel->actual_len = 0;

	musb_channel->controller->used_channels &= ~(1 << musb_channel->idx);

	channel->status = MUSB_DMA_STATUS_UNKNOWN;
}

static void configure_channel(struct dma_channel *channel,
				dma_addr_t dma_addr, u32 len)
{
	struct sprd_musb_dma_channel *musb_channel = channel->private_data;
	struct sprd_musb_dma_controller *controller = musb_channel->controller;
	struct musb *musb = controller->private_data;
	void __iomem *mbase = controller->base;
	u8 bchannel = musb_channel->idx;
	struct sprd_musb_linklist_node *linklist;
	unsigned long ll_offset;
	dma_addr_t next_boundary;
	u64 list_addr;
	u32 tmp_len, csr = 0;
	int i = 0;

	dev_dbg(musb->controller, "%cx len=%u on channel %d\n",
		musb_channel->transmit ? 'T' : 'R', len, bchannel);

	ll_offset = sizeof(struct sprd_musb_linklist_node) *
		    MAX_NODES_PER_CHANNEL * (bchannel - 1);
	list_addr = (u64)controller->linklist_dma_addr + ll_offset;
	linklist = controller->linklist + ll_offset;

	do {
		if (len < 0x10000)
			tmp_len = len;
		else
			tmp_len = 0xfffc;

		next_boundary = (dma_addr | 0xfffffff) + 1;
		if ((dma_addr + tmp_len) > next_boundary)
			tmp_len = (u32)(next_boundary - dma_addr);

		linklist->addr = (u32)dma_addr;
		linklist->data_addr = 0;
		linklist->frag_len = 32;
		linklist->blk_len = tmp_len;
		linklist->list_end = tmp_len == len;
		linklist->sp = 0;
		linklist->ioc = linklist->list_end;
		linklist++;

		dma_addr += tmp_len;
		len -= tmp_len;
		i += 1;
	} while (len > 0 && i < MAX_NODES_PER_CHANNEL);

	wmb();

	/* set linklist pointer */
	musb_writel(mbase, MUSB_DMA_CHN_LLIST_PTR(bchannel), (u32)list_addr);
	musb_writel(mbase, MUSB_DMA_CHN_ADDR_H(bchannel), 0);

	if (musb_channel->transmit) {
		/* enable linklist end interrupt */
		csr = musb_readl(mbase, MUSB_DMA_CHN_INTR(bchannel));
		csr |= CHN_LLIST_INT_EN | CHN_CLEAR_INT_EN;
		musb_writel(mbase, MUSB_DMA_CHN_INTR(bchannel), csr);

		/* enable channel and trigger tx dma transfer */
		csr = musb_readl(mbase, MUSB_DMA_CHN_PAUSE(bchannel));
		if (csr & CHN_CLR)
			musb_writel(mbase, MUSB_DMA_CHN_PAUSE(bchannel), 0);
		csr = musb_readl(mbase, MUSB_DMA_CHN_CFG(bchannel));
		csr |= CHN_EN;
		musb_writel(mbase, MUSB_DMA_CHN_CFG(bchannel), csr);
	} else {
		/* enable linklist end and rx last interrupt */
		csr = musb_readl(mbase, MUSB_DMA_CHN_INTR(bchannel));
		csr |= CHN_USBRX_INT_EN | CHN_LLIST_INT_EN | CHN_CLEAR_INT_EN;
		musb_writel(mbase, MUSB_DMA_CHN_INTR(bchannel), csr);

		/* enable channel and trigger rx dma transfer */
		csr = musb_readl(mbase, MUSB_DMA_CHN_PAUSE(bchannel));
		if (csr & CHN_CLR)
			musb_writel(mbase, MUSB_DMA_CHN_PAUSE(bchannel), 0);
		csr = musb_readl(mbase, MUSB_DMA_CHN_CFG(bchannel));
		csr |= CHN_EN;
		musb_writel(mbase, MUSB_DMA_CHN_CFG(bchannel), csr);
	}
}

static int dma_channel_program(struct dma_channel *channel,
				u16 packet_sz, u8 mode,
				dma_addr_t dma_addr, u32 len)
{
	struct sprd_musb_dma_channel *musb_channel = channel->private_data;

	BUG_ON(channel->status == MUSB_DMA_STATUS_UNKNOWN ||
		channel->status == MUSB_DMA_STATUS_BUSY);

	channel->actual_len = 0;
	musb_channel->start_addr = (u32)dma_addr;
	musb_channel->len = len;
	channel->status = MUSB_DMA_STATUS_BUSY;

	configure_channel(channel, dma_addr, len);

	return true;
}

static int dma_channel_abort(struct dma_channel *channel)
{
	struct sprd_musb_dma_channel *musb_channel = channel->private_data;
	void __iomem *mbase = musb_channel->controller->base;
	struct musb *musb = musb_channel->controller->private_data;

	u8 bchannel = musb_channel->idx;
	int offset;
	u16 csr;

	if (channel->status == MUSB_DMA_STATUS_BUSY) {
		if (musb_channel->transmit) {
			offset = musb->io.ep_offset(musb_channel->epnum,
						MUSB_TXCSR);

			/*
			 * The programming guide says that we must clear
			 * the DMAENAB bit before the DMAMODE bit...
			 */
			csr = musb_readw(mbase, offset);
			csr &= ~(MUSB_TXCSR_AUTOSET | MUSB_TXCSR_DMAENAB);
			musb_writew(mbase, offset, csr);
			csr &= ~MUSB_TXCSR_DMAMODE;
			musb_writew(mbase, offset, csr);
		} else {
			offset = musb->io.ep_offset(musb_channel->epnum,
						MUSB_RXCSR);

			csr = musb_readw(mbase, offset);
			csr &= ~(MUSB_RXCSR_AUTOCLEAR |
				 MUSB_RXCSR_DMAENAB |
				 MUSB_RXCSR_DMAMODE);
			musb_writew(mbase, offset, csr);
		}

		musb_writel(mbase, MUSB_DMA_CHN_LLIST_PTR(bchannel), 0);
		musb_writel(mbase, MUSB_DMA_CHN_ADDR(bchannel), 0);
		channel->status = MUSB_DMA_STATUS_FREE;
	}

	return 0;
}

static void sprd_musb_dma_complete(struct musb *musb,
				   struct sprd_musb_dma_channel *musb_channel)
{
	struct dma_channel *channel = &musb_channel->channel;
	void __iomem *mbase = musb->mregs;
	u32 dma_addr;

	dma_addr = musb_readl(mbase, MUSB_DMA_CHN_ADDR(musb_channel->idx));

	/* Incomplete transfers are not possible when transmitting */
	if (musb_channel->transmit)
		channel->actual_len = musb_channel->len;
	else
		channel->actual_len = dma_addr - musb_channel->start_addr;

	/*
	 * When receiving data, dma_addr sometimes points to a location that
	 * is not related to the current transfer. When this happens, we assume
	 * that the transfer did not actually happen and reconfigure DMA to
	 * receive again.
	 */
	if (channel->actual_len > musb_channel->len) {
		dev_warn(musb->controller,
			 "unexpected dma_addr=%08x, start=%08x\n",
			 dma_addr, musb_channel->start_addr);
		configure_channel(channel, musb_channel->start_addr,
				  musb_channel->len);
		return;
	}

	channel->status = MUSB_DMA_STATUS_FREE;

	dev_dbg(musb->controller, "%cx actual_len=%lu on channel %d\n",
		musb_channel->transmit ? 'T' : 'R', channel->actual_len,
		musb_channel->idx);
	musb_dma_completion(musb, musb_channel->epnum,
			    musb_channel->transmit);
}

irqreturn_t sprd_musb_dma_interrupt(struct musb *musb, u32 int_hsdma)
{
	struct sprd_musb_dma_controller *controller = container_of(
			musb->dma_controller,
			struct sprd_musb_dma_controller,
			controller);
	struct sprd_musb_dma_channel *musb_channel = NULL;
	void __iomem *mbase = musb->mregs;
	u8 bchannel;
	u32 intr;

	for (bchannel = 1; bchannel <= MUSB_DMA_CHANNELS; bchannel++) {
		if (int_hsdma & (1 << (bchannel - 1))) {
			musb_channel = (struct sprd_musb_dma_channel *)
					&(controller->channel[bchannel]);

			intr = musb_readl(mbase, MUSB_DMA_CHN_INTR(bchannel));

			if (intr & CHN_START_INT_MASK_STATUS) {
				intr |= CHN_START_INT_CLR;
				musb_writel(mbase, MUSB_DMA_CHN_INTR(bchannel), intr);
			}

			if (intr & CHN_CLEAR_INT_MASK_STATUS) {
				musb_writel(mbase, MUSB_DMA_CHN_PAUSE(bchannel), 0);
				musb_writel(mbase, MUSB_DMA_CHN_CFG(bchannel), 0);
			}

			if (intr & CHN_LLIST_INT_MASK_STATUS) {
				intr |= CHN_LLIST_INT_CLR | CHN_START_INT_CLR |
					CHN_FRAG_INT_CLR | CHN_BLK_INT_CLR |
					CHN_USBRX_LAST_INT_CLR;
				musb_writel(mbase, MUSB_DMA_CHN_INTR(bchannel), intr);

				sprd_musb_dma_complete(musb, musb_channel);
			}
		}
	}

	return IRQ_HANDLED;
}
EXPORT_SYMBOL_GPL(sprd_musb_dma_interrupt);

void sprd_musb_dma_controller_destroy(struct dma_controller *c)
{
	struct sprd_musb_dma_controller *controller = container_of(c,
			struct sprd_musb_dma_controller, controller);
	struct musb *musb = controller->private_data;

	if (controller->linklist) {
		dma_free_coherent(musb->controller,
				  sizeof(struct sprd_musb_linklist_node) *
				  MAX_NODES_PER_CHANNEL * MUSB_DMA_CHANNELS,
				  controller->linklist,
				  controller->linklist_dma_addr);
	}

	dma_controller_stop(controller);

	kfree(controller);
}
EXPORT_SYMBOL_GPL(sprd_musb_dma_controller_destroy);

static struct sprd_musb_dma_controller *
dma_controller_alloc(struct musb *musb, void __iomem *base)
{
	struct sprd_musb_dma_controller *controller;

	controller = kzalloc(sizeof(*controller), GFP_KERNEL);
	if (!controller)
		return NULL;

	controller->private_data = musb;
	controller->base = base;

	controller->controller.channel_alloc = dma_channel_allocate;
	controller->controller.channel_release = dma_channel_release;
	controller->controller.channel_program = dma_channel_program;
	controller->controller.channel_abort = dma_channel_abort;

	controller->linklist = dma_alloc_coherent(musb->controller,
			sizeof(struct sprd_musb_linklist_node) *
			MAX_NODES_PER_CHANNEL * MUSB_DMA_CHANNELS,
			&controller->linklist_dma_addr, GFP_KERNEL);
	if (!controller->linklist) {
		dev_err(musb->controller, "Failed to allocate DMA list\n");
		sprd_musb_dma_controller_destroy(&controller->controller);
		return NULL;
	}

	return controller;
}

struct dma_controller *
sprd_musb_dma_controller_create(struct musb *musb, void __iomem *base)
{
	struct sprd_musb_dma_controller *controller;
	int i;

	controller = dma_controller_alloc(musb, base);
	if (!controller)
		return NULL;

	/* reset DMA to avoid unexpected problems if USB was already enabled */
	for (i = 1; i <= MUSB_DMA_CHANNELS; i++) {
		musb_writel(base, MUSB_DMA_CHN_PAUSE(i), CHN_CLR);
		musb_writel(base, MUSB_DMA_CHN_CFG(i), 0);
		musb_writel(base, MUSB_DMA_CHN_INTR(i), CHN_CLEAR_INT_EN |
			CHN_LLIST_INT_CLR | CHN_START_INT_CLR |
			CHN_FRAG_INT_CLR | CHN_BLK_INT_CLR |
			CHN_USBRX_LAST_INT_CLR);
		musb_writel(base, MUSB_DMA_CHN_ADDR(i), 0);
		musb_writel(base, MUSB_DMA_CHN_LLIST_PTR(i), 0);
		musb_writel(base, MUSB_DMA_CHN_ADDR_H(i), 0);
	}

	return &controller->controller;
}
EXPORT_SYMBOL_GPL(sprd_musb_dma_controller_create);
