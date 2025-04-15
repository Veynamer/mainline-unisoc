// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2019 Spreadtrum Communications Inc.

#include <linux/clk.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/spinlock.h>

#include "sprd-mcdt.h"

/* MCDT registers definition */
#define MCDT_CH_BASE		0x0
#define MCDT_WTMK_BASE		0x60
#define MCDT_DMA_EN		0xb0

#define MCDT_INT_EN_BASE	0xb4
#define MCDT_INT_CLR_BASE	0xc0
#define MCDT_INT_RAW_BASE	0xcc
#define MCDT_INT_MSK_BASE	0xd8
#define MCDT_CH_FIFO_ST_BASE	0x134

#define MCDT_INT_MSK_CFG0	0x140
#define MCDT_INT_MSK_CFG1	0x144

/* Interrupt and FIFO status definition */
#define MCDT_CH_SHIFT		8

/* Channel water mark definition */
#define MCDT_CH_FIFO_AE_SHIFT	16
#define MCDT_CH_FIFO_AE_MASK	GENMASK(24, 16)
#define MCDT_CH_FIFO_AF_MASK	GENMASK(8, 0)

/* DMA enable definition */
#define MCDT_DAC_DMA_SHIFT	16

/* DMA channel select definition */
#define MCDT_DMA_CHN_SHIFT	4
#define MCDT_DMA_CHN_SEL_MASK	GENMASK(3, 0)

/* Channel FIFO definition */
#define MCDT_CH_FIFO_ADDR_SHIFT	16
#define MCDT_CH_FIFO_ADDR_MASK	GENMASK(9, 0)
#define MCDT_ADC_FIFO_SHIFT	16
#define MCDT_FIFO_LENGTH	512

#define MCDT_CHANNEL_NUM	20

struct sprd_mcdt_hw_info {
	u32 revision;
	u32 num_adc_channels;
	u32 num_dac_channels;
	u32 dac_fifo_addr_base;
	u32 adc_fifo_addr_base;
	u32 fifo_addr_stride;
	u32 fifo_clr_reg;
	u32 dma_dac_chn_base;
	u32 dma_adc_chn_base;
	u32 dma_dac_ack_base;
	u32 dma_adc_ack_base;
	u32 dma_adc_shift;
};

static const struct sprd_mcdt_hw_info sprd_mcdt_r1_info = {
	.num_adc_channels = 10,
	.num_dac_channels = 10,
	.dac_fifo_addr_base = 0xe4,
	.adc_fifo_addr_base = 0xe8,
	.fifo_addr_stride = 8,
	.fifo_clr_reg = 0x14c,
	.dma_dac_chn_base = 0x148,
	.dma_adc_chn_base = 0x150,
	.dma_dac_ack_base = 0x154,
	.dma_adc_ack_base = 0x15c,
	.dma_adc_shift = 0,
};

static const struct sprd_mcdt_hw_info sprd_mcdt_r2_info = {
	.num_adc_channels = 9,
	.num_dac_channels = 11,
	.dac_fifo_addr_base = 0xe4,
	.adc_fifo_addr_base = 0x110,
	.fifo_addr_stride = 4,
	.fifo_clr_reg = 0x158,
	.dma_dac_chn_base = 0x14c,
	.dma_adc_chn_base = 0x150,
	.dma_dac_ack_base = 0x15c,
	.dma_adc_ack_base = 0x160,
	.dma_adc_shift = 12,
};

enum sprd_mcdt_fifo_int {
	MCDT_ADC_FIFO_AE_INT,
	MCDT_ADC_FIFO_AF_INT,
	MCDT_DAC_FIFO_AE_INT,
	MCDT_DAC_FIFO_AF_INT,
	MCDT_ADC_FIFO_OV_INT,
	MCDT_DAC_FIFO_OV_INT
};

enum sprd_mcdt_fifo_sts {
	MCDT_ADC_FIFO_REAL_FULL,
	MCDT_ADC_FIFO_REAL_EMPTY,
	MCDT_ADC_FIFO_AF,
	MCDT_ADC_FIFO_AE,
	MCDT_DAC_FIFO_REAL_FULL,
	MCDT_DAC_FIFO_REAL_EMPTY,
	MCDT_DAC_FIFO_AF,
	MCDT_DAC_FIFO_AE
};

struct sprd_mcdt_dev {
	struct device *dev;
	const struct sprd_mcdt_hw_info *info;
	struct clk *clk;
	void __iomem *base;
	spinlock_t lock;
	struct sprd_mcdt_chan chan[MCDT_CHANNEL_NUM];
};

static LIST_HEAD(sprd_mcdt_chan_list);
static DEFINE_MUTEX(sprd_mcdt_list_mutex);

static void sprd_mcdt_update(struct sprd_mcdt_dev *mcdt, u32 reg, u32 val,
			     u32 mask)
{
	u32 orig = readl_relaxed(mcdt->base + reg);
	u32 tmp;

	tmp = (orig & ~mask) | val;
	writel_relaxed(tmp, mcdt->base + reg);
}

static void sprd_mcdt_dac_set_watermark(struct sprd_mcdt_dev *mcdt, u8 channel,
					u32 full, u32 empty)
{
	u32 reg = MCDT_WTMK_BASE + channel * 4;
	u32 water_mark =
		(empty << MCDT_CH_FIFO_AE_SHIFT) & MCDT_CH_FIFO_AE_MASK;

	water_mark |= full & MCDT_CH_FIFO_AF_MASK;
	sprd_mcdt_update(mcdt, reg, water_mark,
			 MCDT_CH_FIFO_AE_MASK | MCDT_CH_FIFO_AF_MASK);
}

static void sprd_mcdt_adc_set_watermark(struct sprd_mcdt_dev *mcdt, u8 channel,
					u32 full, u32 empty)
{
	u32 reg = MCDT_WTMK_BASE + (mcdt->info->num_dac_channels + channel) * 4;
	u32 water_mark =
		(empty << MCDT_CH_FIFO_AE_SHIFT) & MCDT_CH_FIFO_AE_MASK;

	water_mark |= full & MCDT_CH_FIFO_AF_MASK;
	sprd_mcdt_update(mcdt, reg, water_mark,
			 MCDT_CH_FIFO_AE_MASK | MCDT_CH_FIFO_AF_MASK);
}

static void sprd_mcdt_dac_dma_enable(struct sprd_mcdt_dev *mcdt, u8 channel,
				     bool enable)
{
	u32 shift = MCDT_DAC_DMA_SHIFT + channel;

	if (enable)
		sprd_mcdt_update(mcdt, MCDT_DMA_EN, BIT(shift), BIT(shift));
	else
		sprd_mcdt_update(mcdt, MCDT_DMA_EN, 0, BIT(shift));
}

static void sprd_mcdt_adc_dma_enable(struct sprd_mcdt_dev *mcdt, u8 channel,
				     bool enable)
{
	if (enable)
		sprd_mcdt_update(mcdt, MCDT_DMA_EN, BIT(channel), BIT(channel));
	else
		sprd_mcdt_update(mcdt, MCDT_DMA_EN, 0, BIT(channel));
}

static void sprd_mcdt_ap_int_enable(struct sprd_mcdt_dev *mcdt, u8 channel,
				    bool enable)
{
	if (enable)
		sprd_mcdt_update(mcdt, MCDT_INT_MSK_CFG0, BIT(channel),
				 BIT(channel));
	else
		sprd_mcdt_update(mcdt, MCDT_INT_MSK_CFG0, 0, BIT(channel));
}

static void sprd_mcdt_dac_write_fifo(struct sprd_mcdt_dev *mcdt, u8 channel,
				     u32 val)
{
	u32 reg = MCDT_CH_BASE + channel * 4;

	writel_relaxed(val, mcdt->base + reg);
}

static void sprd_mcdt_adc_read_fifo(struct sprd_mcdt_dev *mcdt, u8 channel,
				    u32 *val)
{
	u32 reg = MCDT_CH_BASE + (mcdt->info->num_dac_channels + channel) * 4;

	*val = readl_relaxed(mcdt->base + reg);
}

static void sprd_mcdt_dac_dma_chn_select(struct sprd_mcdt_dev *mcdt, u8 channel,
					 enum sprd_mcdt_dma_chan dma_chan)
{
	u32 reg, idx, shift;

	idx = dma_chan * MCDT_DMA_CHN_SHIFT;
	reg = mcdt->info->dma_dac_chn_base + (idx / 32) * 4;
	shift = idx % 32;

	sprd_mcdt_update(mcdt, reg, channel << shift,
			 MCDT_DMA_CHN_SEL_MASK << shift);
}

static void sprd_mcdt_adc_dma_chn_select(struct sprd_mcdt_dev *mcdt, u8 channel,
					 enum sprd_mcdt_dma_chan dma_chan)
{
	u32 reg, idx, shift;

	idx = dma_chan * MCDT_DMA_CHN_SHIFT + mcdt->info->dma_adc_shift;
	reg = mcdt->info->dma_adc_chn_base + (idx / 32) * 4;
	shift = idx % 32;

	sprd_mcdt_update(mcdt, reg, channel << shift,
			 MCDT_DMA_CHN_SEL_MASK << shift);
}

static void sprd_mcdt_dac_dma_ack_select(struct sprd_mcdt_dev *mcdt, u8 channel,
					 enum sprd_mcdt_dma_chan dma_chan)
{
	u32 reg, idx, shift;

	idx = channel * MCDT_DMA_CHN_SHIFT;
	reg = mcdt->info->dma_dac_ack_base + (idx / 32) * 4;
	shift = idx % 32;

	sprd_mcdt_update(mcdt, reg, dma_chan << shift,
			 MCDT_DMA_CHN_SEL_MASK << shift);
}

static void sprd_mcdt_adc_dma_ack_select(struct sprd_mcdt_dev *mcdt, u8 channel,
					 enum sprd_mcdt_dma_chan dma_chan)
{
	u32 reg, idx, shift;

	idx = channel * MCDT_DMA_CHN_SHIFT + mcdt->info->dma_adc_shift;
	reg = mcdt->info->dma_adc_ack_base + (idx / 32) * 4;
	shift = idx % 32;

	sprd_mcdt_update(mcdt, reg, dma_chan << shift,
			 MCDT_DMA_CHN_SEL_MASK << shift);
}

static bool sprd_mcdt_chan_fifo_sts(struct sprd_mcdt_dev *mcdt, u8 channel,
				    enum sprd_mcdt_fifo_sts fifo_sts)
{
	u32 reg, idx, shift;

	idx = channel * MCDT_CH_SHIFT;
	reg = MCDT_CH_FIFO_ST_BASE + (idx / 32) * 4;
	shift = idx % 32;

	return !!(readl_relaxed(mcdt->base + reg) & BIT(shift));
}

static void sprd_mcdt_dac_fifo_clear(struct sprd_mcdt_dev *mcdt, u8 channel)
{
	sprd_mcdt_update(mcdt, mcdt->info->fifo_clr_reg,
			 BIT(channel), BIT(channel));
}

static void sprd_mcdt_adc_fifo_clear(struct sprd_mcdt_dev *mcdt, u8 channel)
{
	u32 shift = MCDT_ADC_FIFO_SHIFT + channel;

	sprd_mcdt_update(mcdt, mcdt->info->fifo_clr_reg,
			 BIT(shift), BIT(shift));
}

static u32 sprd_mcdt_dac_fifo_avail(struct sprd_mcdt_dev *mcdt, u8 channel)
{
	u32 reg = mcdt->info->dac_fifo_addr_base +
		  channel * mcdt->info->fifo_addr_stride;
	u32 r_addr = (readl_relaxed(mcdt->base + reg) >>
		      MCDT_CH_FIFO_ADDR_SHIFT) & MCDT_CH_FIFO_ADDR_MASK;
	u32 w_addr = readl_relaxed(mcdt->base + reg) & MCDT_CH_FIFO_ADDR_MASK;

	if (w_addr >= r_addr)
		return 4 * (MCDT_FIFO_LENGTH - w_addr + r_addr);
	else
		return 4 * (r_addr - w_addr);
}

static u32 sprd_mcdt_adc_fifo_avail(struct sprd_mcdt_dev *mcdt, u8 channel)
{
	u32 reg = mcdt->info->adc_fifo_addr_base +
		  channel * mcdt->info->fifo_addr_stride;
	u32 r_addr = (readl_relaxed(mcdt->base + reg) >>
		      MCDT_CH_FIFO_ADDR_SHIFT) & MCDT_CH_FIFO_ADDR_MASK;
	u32 w_addr = readl_relaxed(mcdt->base + reg) & MCDT_CH_FIFO_ADDR_MASK;

	if (w_addr >= r_addr)
		return 4 * (w_addr - r_addr);
	else
		return 4 * (MCDT_FIFO_LENGTH - r_addr + w_addr);
}

static void sprd_mcdt_chan_int_en(struct sprd_mcdt_dev *mcdt, u8 channel,
				  enum sprd_mcdt_fifo_int int_type, bool enable)
{
	u32 reg, idx, shift;

	idx = channel * MCDT_CH_SHIFT + int_type;
	reg = MCDT_INT_EN_BASE + (idx / 32) * 4;
	shift = idx % 32;

	if (enable)
		sprd_mcdt_update(mcdt, reg, BIT(shift), BIT(shift));
	else
		sprd_mcdt_update(mcdt, reg, 0, BIT(shift));
}

static void sprd_mcdt_chan_int_clear(struct sprd_mcdt_dev *mcdt, u8 channel,
				     enum sprd_mcdt_fifo_int int_type)
{
	u32 reg, idx, shift;

	idx = channel * MCDT_CH_SHIFT + int_type;
	reg = MCDT_INT_CLR_BASE + (idx / 32) * 4;
	shift = idx % 32;

	sprd_mcdt_update(mcdt, reg, BIT(shift), BIT(shift));
}

static bool sprd_mcdt_chan_int_sts(struct sprd_mcdt_dev *mcdt, u8 channel,
				   enum sprd_mcdt_fifo_int int_type)
{
	u32 reg, idx, shift;

	idx = channel * MCDT_CH_SHIFT + int_type;
	reg = MCDT_INT_MSK_BASE + (idx / 32) * 4;
	shift = idx % 32;

	return !!(readl_relaxed(mcdt->base + reg) & BIT(shift));
}

static irqreturn_t sprd_mcdt_irq_handler(int irq, void *dev_id)
{
	struct sprd_mcdt_dev *mcdt = (struct sprd_mcdt_dev *)dev_id;
	int i;

	spin_lock(&mcdt->lock);

	for (i = 0; i < mcdt->info->num_adc_channels; i++) {
		if (sprd_mcdt_chan_int_sts(mcdt, i, MCDT_ADC_FIFO_AF_INT)) {
			struct sprd_mcdt_chan *chan =
				&mcdt->chan[i + mcdt->info->num_dac_channels];

			sprd_mcdt_chan_int_clear(mcdt, i, MCDT_ADC_FIFO_AF_INT);
			if (chan->cb)
				chan->cb->notify(chan->cb->data);
		}
	}

	for (i = 0; i < mcdt->info->num_dac_channels; i++) {
		if (sprd_mcdt_chan_int_sts(mcdt, i, MCDT_DAC_FIFO_AE_INT)) {
			struct sprd_mcdt_chan *chan = &mcdt->chan[i];

			sprd_mcdt_chan_int_clear(mcdt, i, MCDT_DAC_FIFO_AE_INT);
			if (chan->cb)
				chan->cb->notify(chan->cb->data);
		}
	}

	spin_unlock(&mcdt->lock);

	return IRQ_HANDLED;
}

/**
 * sprd_mcdt_chan_write - write data to the MCDT channel's fifo
 * @chan: the MCDT channel
 * @tx_buf: send buffer
 * @size: data size
 *
 * Note: We can not write data to the channel fifo when enabling the DMA mode,
 * otherwise the channel fifo data will be invalid.
 *
 * If there are not enough space of the channel fifo, it will return errors
 * to users.
 *
 * Returns 0 on success, or an appropriate error code on failure.
 */
int sprd_mcdt_chan_write(struct sprd_mcdt_chan *chan, char *tx_buf, u32 size)
{
	struct sprd_mcdt_dev *mcdt = chan->mcdt;
	unsigned long flags;
	int avail, i = 0, words = size / 4;
	u32 *buf = (u32 *)tx_buf;

	spin_lock_irqsave(&mcdt->lock, flags);

	if (chan->dma_enable) {
		dev_err(mcdt->dev,
			"Can not write data when DMA mode enabled\n");
		spin_unlock_irqrestore(&mcdt->lock, flags);
		return -EINVAL;
	}

	if (sprd_mcdt_chan_fifo_sts(mcdt, chan->id, MCDT_DAC_FIFO_REAL_FULL)) {
		dev_err(mcdt->dev, "Channel fifo is full now\n");
		spin_unlock_irqrestore(&mcdt->lock, flags);
		return -EBUSY;
	}

	avail = sprd_mcdt_dac_fifo_avail(mcdt, chan->id);
	if (size > avail) {
		dev_err(mcdt->dev,
			"Data size is larger than the available fifo size\n");
		spin_unlock_irqrestore(&mcdt->lock, flags);
		return -EBUSY;
	}

	while (i++ < words)
		sprd_mcdt_dac_write_fifo(mcdt, chan->id, *buf++);

	spin_unlock_irqrestore(&mcdt->lock, flags);
	return 0;
}
EXPORT_SYMBOL_GPL(sprd_mcdt_chan_write);

/**
 * sprd_mcdt_chan_read - read data from the MCDT channel's fifo
 * @chan: the MCDT channel
 * @rx_buf: receive buffer
 * @size: data size
 *
 * Note: We can not read data from the channel fifo when enabling the DMA mode,
 * otherwise the reading data will be invalid.
 *
 * Usually user need start to read data once receiving the fifo full interrupt.
 *
 * Returns data size of reading successfully, or an error code on failure.
 */
int sprd_mcdt_chan_read(struct sprd_mcdt_chan *chan, char *rx_buf, u32 size)
{
	struct sprd_mcdt_dev *mcdt = chan->mcdt;
	unsigned long flags;
	int i = 0, avail, words = size / 4;
	u32 *buf = (u32 *)rx_buf;

	spin_lock_irqsave(&mcdt->lock, flags);

	if (chan->dma_enable) {
		dev_err(mcdt->dev, "Can not read data when DMA mode enabled\n");
		spin_unlock_irqrestore(&mcdt->lock, flags);
		return -EINVAL;
	}

	if (sprd_mcdt_chan_fifo_sts(mcdt, chan->id, MCDT_ADC_FIFO_REAL_EMPTY)) {
		dev_err(mcdt->dev, "Channel fifo is empty\n");
		spin_unlock_irqrestore(&mcdt->lock, flags);
		return -EBUSY;
	}

	avail = sprd_mcdt_adc_fifo_avail(mcdt, chan->id);
	if (size > avail)
		words = avail / 4;

	while (i++ < words)
		sprd_mcdt_adc_read_fifo(mcdt, chan->id, buf++);

	spin_unlock_irqrestore(&mcdt->lock, flags);
	return words * 4;
}
EXPORT_SYMBOL_GPL(sprd_mcdt_chan_read);

/**
 * sprd_mcdt_chan_int_enable - enable the interrupt mode for the MCDT channel
 * @chan: the MCDT channel
 * @water_mark: water mark to trigger a interrupt
 * @cb: callback when a interrupt happened
 *
 * Now it only can enable fifo almost full interrupt for ADC channel and fifo
 * almost empty interrupt for DAC channel. Morevoer for interrupt mode, user
 * should use sprd_mcdt_chan_read() or sprd_mcdt_chan_write() to read or write
 * data manually.
 *
 * For ADC channel, user can start to read data once receiving one fifo full
 * interrupt. For DAC channel, user can start to write data once receiving one
 * fifo empty interrupt or just call sprd_mcdt_chan_write() to write data
 * directly.
 *
 * Returns 0 on success, or an error code on failure.
 */
int sprd_mcdt_chan_int_enable(struct sprd_mcdt_chan *chan, u32 water_mark,
			      struct sprd_mcdt_chan_callback *cb)
{
	struct sprd_mcdt_dev *mcdt = chan->mcdt;
	unsigned long flags;
	int ret;

	ret = pm_runtime_get_sync(mcdt->dev);
	if (ret < 0) {
		dev_err(mcdt->dev, "Failed to power on: %d\n", ret);
		return ret;
	}

	ret = 0;

	spin_lock_irqsave(&mcdt->lock, flags);

	if (chan->dma_enable || chan->int_enable) {
		dev_err(mcdt->dev, "Failed to set interrupt mode.\n");
		ret = -EINVAL;
		goto out;
	}

	switch (chan->type) {
	case SPRD_MCDT_ADC_CHAN:
		sprd_mcdt_adc_fifo_clear(mcdt, chan->id);
		sprd_mcdt_adc_set_watermark(mcdt, chan->id, water_mark,
					    MCDT_FIFO_LENGTH - 1);
		sprd_mcdt_chan_int_en(mcdt, chan->id,
				      MCDT_ADC_FIFO_AF_INT, true);
		sprd_mcdt_ap_int_enable(mcdt, chan->id, true);
		break;

	case SPRD_MCDT_DAC_CHAN:
		sprd_mcdt_dac_fifo_clear(mcdt, chan->id);
		sprd_mcdt_dac_set_watermark(mcdt, chan->id,
					    MCDT_FIFO_LENGTH - 1, water_mark);
		sprd_mcdt_chan_int_en(mcdt, chan->id,
				      MCDT_DAC_FIFO_AE_INT, true);
		sprd_mcdt_ap_int_enable(mcdt, chan->id, true);
		break;

	default:
		dev_err(mcdt->dev, "Unsupported channel type\n");
		ret = -EINVAL;
		goto out;
	}

	chan->cb = cb;
	chan->int_enable = true;

out:
	spin_unlock_irqrestore(&mcdt->lock, flags);
	if (ret)
		pm_runtime_put(mcdt->dev);

	return ret;
}
EXPORT_SYMBOL_GPL(sprd_mcdt_chan_int_enable);

/**
 * sprd_mcdt_chan_int_disable - disable the interrupt mode for the MCDT channel
 * @chan: the MCDT channel
 */
void sprd_mcdt_chan_int_disable(struct sprd_mcdt_chan *chan)
{
	struct sprd_mcdt_dev *mcdt = chan->mcdt;
	unsigned long flags;

	spin_lock_irqsave(&mcdt->lock, flags);

	if (!chan->int_enable) {
		spin_unlock_irqrestore(&mcdt->lock, flags);
		return;
	}

	switch (chan->type) {
	case SPRD_MCDT_ADC_CHAN:
		sprd_mcdt_chan_int_en(mcdt, chan->id,
				      MCDT_ADC_FIFO_AF_INT, false);
		sprd_mcdt_chan_int_clear(mcdt, chan->id, MCDT_ADC_FIFO_AF_INT);
		sprd_mcdt_ap_int_enable(mcdt, chan->id, false);
		break;

	case SPRD_MCDT_DAC_CHAN:
		sprd_mcdt_chan_int_en(mcdt, chan->id,
				      MCDT_DAC_FIFO_AE_INT, false);
		sprd_mcdt_chan_int_clear(mcdt, chan->id, MCDT_DAC_FIFO_AE_INT);
		sprd_mcdt_ap_int_enable(mcdt, chan->id, false);
		break;

	default:
		break;
	}

	chan->int_enable = false;
	spin_unlock_irqrestore(&mcdt->lock, flags);

	pm_runtime_put(mcdt->dev);
}
EXPORT_SYMBOL_GPL(sprd_mcdt_chan_int_disable);

/**
 * sprd_mcdt_chan_dma_enable - enable the DMA mode for the MCDT channel
 * @chan: the MCDT channel
 * @dma_chan: specify which DMA channel will be used for this MCDT channel
 * @water_mark: water mark to trigger a DMA request
 *
 * Enable the DMA mode for the MCDT channel, that means we can use DMA to
 * transfer data to the channel fifo and do not need reading/writing data
 * manually.
 *
 * Returns 0 on success, or an error code on failure.
 */
int sprd_mcdt_chan_dma_enable(struct sprd_mcdt_chan *chan,
			      enum sprd_mcdt_dma_chan dma_chan,
			      u32 water_mark)
{
	struct sprd_mcdt_dev *mcdt = chan->mcdt;
	unsigned long flags;
	int ret;

	ret = pm_runtime_get_sync(mcdt->dev);
	if (ret < 0) {
		dev_err(mcdt->dev, "Failed to power on: %d\n", ret);
		return ret;
	}

	ret = 0;

	spin_lock_irqsave(&mcdt->lock, flags);

	if (chan->dma_enable || chan->int_enable ||
	    dma_chan > SPRD_MCDT_DMA_CH4) {
		dev_err(mcdt->dev, "Failed to set DMA mode\n");
		ret = -EINVAL;
		goto out;
	}

	switch (chan->type) {
	case SPRD_MCDT_ADC_CHAN:
		sprd_mcdt_adc_fifo_clear(mcdt, chan->id);
		sprd_mcdt_adc_set_watermark(mcdt, chan->id,
					    water_mark, MCDT_FIFO_LENGTH - 1);
		sprd_mcdt_adc_dma_enable(mcdt, chan->id, true);
		sprd_mcdt_adc_dma_chn_select(mcdt, chan->id, dma_chan);
		sprd_mcdt_adc_dma_ack_select(mcdt, chan->id, dma_chan);
		break;

	case SPRD_MCDT_DAC_CHAN:
		sprd_mcdt_dac_fifo_clear(mcdt, chan->id);
		sprd_mcdt_dac_set_watermark(mcdt, chan->id,
					    MCDT_FIFO_LENGTH - 1, water_mark);
		sprd_mcdt_dac_dma_enable(mcdt, chan->id, true);
		sprd_mcdt_dac_dma_chn_select(mcdt, chan->id, dma_chan);
		sprd_mcdt_dac_dma_ack_select(mcdt, chan->id, dma_chan);
		break;

	default:
		dev_err(mcdt->dev, "Unsupported channel type\n");
		ret = -EINVAL;
		goto out;
	}

	chan->dma_enable = true;

out:
	spin_unlock_irqrestore(&mcdt->lock, flags);
	if (ret)
		pm_runtime_put(mcdt->dev);

	return ret;
}
EXPORT_SYMBOL_GPL(sprd_mcdt_chan_dma_enable);

/**
 * sprd_mcdt_chan_dma_disable - disable the DMA mode for the MCDT channel
 * @chan: the MCDT channel
 */
void sprd_mcdt_chan_dma_disable(struct sprd_mcdt_chan *chan)
{
	struct sprd_mcdt_dev *mcdt = chan->mcdt;
	unsigned long flags;

	spin_lock_irqsave(&mcdt->lock, flags);

	if (!chan->dma_enable) {
		spin_unlock_irqrestore(&mcdt->lock, flags);
		return;
	}

	switch (chan->type) {
	case SPRD_MCDT_ADC_CHAN:
		sprd_mcdt_adc_dma_enable(mcdt, chan->id, false);
		sprd_mcdt_adc_fifo_clear(mcdt, chan->id);
		break;

	case SPRD_MCDT_DAC_CHAN:
		sprd_mcdt_dac_dma_enable(mcdt, chan->id, false);
		sprd_mcdt_dac_fifo_clear(mcdt, chan->id);
		break;

	default:
		break;
	}

	pm_runtime_put(mcdt->dev);
	chan->dma_enable = false;
	spin_unlock_irqrestore(&mcdt->lock, flags);
}
EXPORT_SYMBOL_GPL(sprd_mcdt_chan_dma_disable);

/**
 * sprd_mcdt_request_chan - request one MCDT channel
 * @channel: channel id
 * @type: channel type, it can be one ADC channel or DAC channel
 *
 * Rreturn NULL if no available channel.
 */
struct sprd_mcdt_chan *sprd_mcdt_request_chan(u8 channel,
					      enum sprd_mcdt_channel_type type)
{
	struct sprd_mcdt_chan *temp;

	mutex_lock(&sprd_mcdt_list_mutex);

	list_for_each_entry(temp, &sprd_mcdt_chan_list, list) {
		if (temp->type == type && temp->id == channel) {
			list_del_init(&temp->list);
			break;
		}
	}

	if (list_entry_is_head(temp, &sprd_mcdt_chan_list, list))
		temp = NULL;

	mutex_unlock(&sprd_mcdt_list_mutex);

	return temp;
}
EXPORT_SYMBOL_GPL(sprd_mcdt_request_chan);

/**
 * sprd_mcdt_free_chan - free one MCDT channel
 * @chan: the channel to be freed
 */
void sprd_mcdt_free_chan(struct sprd_mcdt_chan *chan)
{
	struct sprd_mcdt_chan *temp;

	sprd_mcdt_chan_dma_disable(chan);
	sprd_mcdt_chan_int_disable(chan);

	mutex_lock(&sprd_mcdt_list_mutex);

	list_for_each_entry(temp, &sprd_mcdt_chan_list, list) {
		if (temp == chan) {
			mutex_unlock(&sprd_mcdt_list_mutex);
			return;
		}
	}

	list_add_tail(&chan->list, &sprd_mcdt_chan_list);
	mutex_unlock(&sprd_mcdt_list_mutex);
}
EXPORT_SYMBOL_GPL(sprd_mcdt_free_chan);

static void sprd_mcdt_init_chans(struct sprd_mcdt_dev *mcdt,
				 struct resource *res)
{
	int i;

	for (i = 0; i < MCDT_CHANNEL_NUM; i++) {
		struct sprd_mcdt_chan *chan = &mcdt->chan[i];

		if (i < mcdt->info->num_dac_channels) {
			chan->id = i;
			chan->type = SPRD_MCDT_DAC_CHAN;
			chan->fifo_phys = res->start + MCDT_CH_BASE + i * 4;
		} else {
			chan->id = i - mcdt->info->num_dac_channels;
			chan->type = SPRD_MCDT_ADC_CHAN;
			chan->fifo_phys = res->start + MCDT_CH_BASE + i * 4;
		}

		chan->mcdt = mcdt;
		INIT_LIST_HEAD(&chan->list);

		mutex_lock(&sprd_mcdt_list_mutex);
		list_add_tail(&chan->list, &sprd_mcdt_chan_list);
		mutex_unlock(&sprd_mcdt_list_mutex);
	}
}

static int sprd_mcdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sprd_mcdt_dev *mcdt;
	struct resource *res;
	int ret, irq;

	mcdt = devm_kzalloc(dev, sizeof(*mcdt), GFP_KERNEL);
	if (!mcdt)
		return -ENOMEM;

	mcdt->dev = dev;
	spin_lock_init(&mcdt->lock);
	platform_set_drvdata(pdev, mcdt);

	mcdt->info = of_device_get_match_data(dev);
	if (!mcdt->info)
		return -EINVAL;

	mcdt->clk = devm_clk_get_optional(dev, NULL);
	if (IS_ERR(mcdt->clk))
		return dev_err_probe(dev, PTR_ERR(mcdt->clk),
				     "cannot get clock");

	ret = clk_prepare_enable(mcdt->clk);
	if (ret) {
		dev_err(dev, "cannot enable clock\n");
		return ret;
	}

	pm_runtime_set_autosuspend_delay(dev, 3000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_set_active(dev);
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	mcdt->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(mcdt->base))
		return PTR_ERR(mcdt->base);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_request_irq(dev, irq, sprd_mcdt_irq_handler,
			       0, "sprd-mcdt", mcdt);
	if (ret) {
		dev_err(dev, "Failed to request MCDT IRQ\n");
		return ret;
	}

	sprd_mcdt_init_chans(mcdt, res);

	return 0;
}

static void sprd_mcdt_remove(struct platform_device *pdev)
{
	struct sprd_mcdt_chan *chan, *temp;

	mutex_lock(&sprd_mcdt_list_mutex);

	list_for_each_entry_safe(chan, temp, &sprd_mcdt_chan_list, list)
		list_del(&chan->list);

	mutex_unlock(&sprd_mcdt_list_mutex);
}

static int __maybe_unused sprd_mcdt_runtime_suspend(struct device *dev)
{
	struct sprd_mcdt_dev *mdev = dev_get_drvdata(dev);

	clk_disable_unprepare(mdev->clk);

	return 0;
}

static int __maybe_unused sprd_mcdt_runtime_resume(struct device *dev)
{
	struct sprd_mcdt_dev *mdev = dev_get_drvdata(dev);

	return clk_prepare_enable(mdev->clk);
}

static const struct dev_pm_ops sprd_mcdt_pm_ops = {
	SET_RUNTIME_PM_OPS(sprd_mcdt_runtime_suspend,
			   sprd_mcdt_runtime_resume, NULL)
};

static const struct of_device_id sprd_mcdt_of_match[] = {
	{ .compatible = "sprd,sc9860-mcdt", .data = &sprd_mcdt_r1_info },
	{ .compatible = "sprd,ums9230-mcdt", .data = &sprd_mcdt_r2_info },
	{ }
};
MODULE_DEVICE_TABLE(of, sprd_mcdt_of_match);

static struct platform_driver sprd_mcdt_driver = {
	.probe = sprd_mcdt_probe,
	.remove = sprd_mcdt_remove,
	.driver = {
		.name = "sprd-mcdt",
		.of_match_table = sprd_mcdt_of_match,
		.pm = &sprd_mcdt_pm_ops,
	},
};

module_platform_driver(sprd_mcdt_driver);

MODULE_DESCRIPTION("Spreadtrum Multi-Channel Data Transfer Driver");
MODULE_LICENSE("GPL v2");
