// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Spreadtrum Communications Inc.
 */

#ifndef __SPRD_DMA_H__
#define __SPRD_DMA_H__

#define MUSB_DMA_PAUSE			0x1000
#define MUSB_DMA_FRAG_WAIT		0x1004
#define MUSB_DMA_INTR_RAW_STATUS	0x1008
#define MUSB_DMA_INTR_MASK_STATUS	0x100C
#define MUSB_DMA_REQ_STATUS		0x1010
#define MUSB_DMA_EN_STATUS		0x1014
#define MUSB_DMA_DEBUG_STATUS		0x1018

/* multi LL which is supported by r4p0 */
#define MUSB_DMA_MULT_LL_Q_CTRL_STATUS	0x1080
#define MUSB_DMA_MULT_LL_CTRL		0x1084
#define MUSB_DMA_TX_CMD_QUEUE_LOW	0x1088
#define MUSB_DMA_TX_CMD_QUEUE_HIGH	0x108C
#define MUSB_DMA_TX_CMPLT_QUEUE_LOW	0x1090
#define MUSB_DMA_TX_CMPLT_QUEUE_HIGH	0x1094
#define MUSB_DMA_RX_CMD_QUEUE_LOW	0x1098
#define MUSB_DMA_RX_CMD_QUEUE_HIGH	0x109C
#define MUSB_DMA_RX_CMPLT_QUEUE_LOW	0x10A0
#define MUSB_DMA_RX_CMPLT_QUEUE_HIGH	0x10A4

#define MUSB_DMA_CHN_PAUSE(n)		(0x1C00 + ((n) - 1) * 0x20)
#define MUSB_DMA_CHN_CFG(n)		(0x1C04 + ((n) - 1) * 0x20)
#define MUSB_DMA_CHN_INTR(n)		(0x1C08 + ((n) - 1) * 0x20)
#define MUSB_DMA_CHN_ADDR(n)		(0x1C0C + ((n) - 1) * 0x20)
#define MUSB_DMA_CHN_LEN(n)		(0x1C10 + ((n) - 1) * 0x20)
#define MUSB_DMA_CHN_LLIST_PTR(n)	(0x1C14 + ((n) - 1) * 0x20)
#define MUSB_DMA_CHN_ADDR_H(n)		(0x1C18 + ((n) - 1) * 0x20)
#define MUSB_DMA_CHN_REQ(n)		(0x1C1C + ((n) - 1) * 0x20)

#define CHN_EN			BIT(0)
#define CHN_LLIST_INT_EN	BIT(2)
#define CHN_START_INT_EN	BIT(3)
#define CHN_USBRX_INT_EN	BIT(4)
#define CHN_CLEAR_INT_EN	BIT(5)

#define CHN_LLIST_INT_MASK_STATUS	BIT(18)
#define CHN_START_INT_MASK_STATUS	BIT(19)
#define CHN_USBRX_INT_MASK_STATUS	BIT(20)
#define CHN_CLEAR_INT_MASK_STATUS	BIT(21)

#define CHN_CLR			BIT(15)
#define CHN_CLR_STATUS		BIT(31)

#define CHN_FRAG_INT_CLR	BIT(24)
#define CHN_BLK_INT_CLR		BIT(25)
#define CHN_LLIST_INT_CLR	BIT(26)
#define CHN_START_INT_CLR	BIT(27)
#define CHN_USBRX_LAST_INT_CLR	BIT(28)

#define MUSB_DMA_CHANNELS	30

/* MUSB_DMA_MULT_LL_Q_CTRL_STATUS bit defines */
#define BIT_TX_CMD_DEPTH_MASK	GENMASK(23, 20)
#define BIT_TX_CMPLT_DEPTH_MASK	GENMASK(19, 16)
#define BIT_TX_CMD_FULL		BIT(15)
#define BIT_TX_CMPLT_EMPTY	BIT(14)
#define BIT_TX_CMD_CLR		BIT(13)
#define BIT_TX_CMPLT_CLR	BIT(12)
#define BIT_RX_CMD_DEPTH_MASK	GENMASK(11, 8)
#define BIT_RX_CMPLT_DEPTH_MASK	GENMASK(7, 4)
#define BIT_RX_CMD_FULL		BIT(3)
#define BIT_RX_CMPLT_EMPTY	BIT(2)
#define BIT_RX_CMD_CLR		BIT(1)
#define BIT_RX_CMPLT_CLR	BIT(0)

/* MUSB_DMA_MULT_LL_CTRL bit defines */
#define BIT_TX_CMD_QUEUE_WR	BIT(11)
#define BIT_TX_CMPLT_QUEUE_RD	BIT(10)
#define BIT_RX_CMD_QUEUE_WR	BIT(9)
#define BIT_RX_CMPLT_QUEUE_RD	BIT(8)
#define BIT_TX_IPA_CHN_MASK	GENMASK(7, 4)
#define BIT_RX_IPA_CHN_MASK	GENMASK(3, 0)

struct sprd_musb_dma_controller;

struct sprd_musb_linklist_node {
	u32	addr;

	u16	frag_len;
	u16	blk_len;

	u32	list_end	:  1;
	u32	sp		:  1;
	u32	ioc		:  1;
	u32	reserved	:  5;
	u32	data_addr	:  4;
	u32	pad		: 20;

	u32	reserved1;
} __packed;

irqreturn_t sprd_musb_dma_interrupt(struct musb *musb, u32 int_hsdma);
struct dma_controller *sprd_musb_dma_controller_create(struct musb *musb,
						       void __iomem *base);
void sprd_musb_dma_controller_destroy(struct dma_controller *c);

#endif
