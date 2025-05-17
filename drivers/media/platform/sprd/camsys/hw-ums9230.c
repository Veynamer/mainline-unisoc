// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unisoc camera subsystem media device driver - UMS9230 specific code
 *
 * Copyright (C) 2025 Otto Pflüger
 */

#include <linux/regmap.h>

#include "camsys.h"
#include "dcam-regs-ums9230.h"

/* Platform-specific syscon registers */
#define UMS9230_MM_AHB_CSI_PHY_SEL			0x0030

#define UMS9230_MM_AHB_DCAM_CSI_SEL(id)			(0x0048 + (id) * 4)
#define UMS9230_MM_AHB_DCAM_CSI_SEL_FORCE		BIT(11)
#define UMS9230_MM_AHB_DCAM_CSI_SEL_INDEX		GENMASK(10, 9)
#define UMS9230_MM_AHB_DCAM_CSI_SEL_FORCE_TRIGGER	BIT(8)

#define UMS9230_ANLG_PHY_G10_CSI_2P2_RO_CTRL		0x000C
#define UMS9230_ANLG_PHY_G10_CSI_2P2_CTRL		0x0038
#define UMS9230_ANLG_PHY_G10_CSI_2P2			BIT(4)

#define UMS9230_ANLG_PHY_G10_CSI_BIST_TEST		0x00B4

static void ums9230_csi_power_phy(struct sprd_csi *csi, int enable)
{
	u32 val;

	switch (csi->index) {
	case 0:
		val = BIT(26);
		break;
	case 1:
		val = BIT(24);
		break;
	case 2:
		val = BIT(25);
		break;
	default:
		return;
	}

	regmap_assign_bits(csi->camsys->anlg_phy_regs,
			   UMS9230_ANLG_PHY_G10_CSI_BIST_TEST,
			   val, enable);
}

static void ums9230_csi_setup_phy(struct sprd_csi *csi, int enable)
{
	u32 mask, val, id = enable ? csi->index + 1 : 0;
	int shift1, shift2 = -1;

	switch (csi->phy_id) {
	case CAMSYS_CSI_M_S:
		if (enable)
			regmap_set_bits(csi->camsys->anlg_phy_regs,
					UMS9230_ANLG_PHY_G10_CSI_2P2_CTRL,
					UMS9230_ANLG_PHY_G10_CSI_2P2);
		shift1 = 0;
		shift2 = 2;
		break;
	case CAMSYS_CSI_M:
		if (enable)
			regmap_clear_bits(csi->camsys->anlg_phy_regs,
					  UMS9230_ANLG_PHY_G10_CSI_2P2_CTRL,
					  UMS9230_ANLG_PHY_G10_CSI_2P2);
		shift1 = 0;
		break;
	case CAMSYS_CSI_S:
		if (enable)
			regmap_clear_bits(csi->camsys->anlg_phy_regs,
					  UMS9230_ANLG_PHY_G10_CSI_2P2_CTRL,
					  UMS9230_ANLG_PHY_G10_CSI_2P2);
		shift1 = 2;
		break;
	case CAMSYS_CSI_4LANE:
		shift1 = 4;
		break;
	case CAMSYS_CSI_RO_M_S:
		if (enable)
			regmap_set_bits(csi->camsys->anlg_phy_regs,
					UMS9230_ANLG_PHY_G10_CSI_2P2_RO_CTRL,
					UMS9230_ANLG_PHY_G10_CSI_2P2);
		shift1 = 6;
		shift2 = 8;
		break;
	case CAMSYS_CSI_RO_M:
		if (enable)
			regmap_clear_bits(csi->camsys->anlg_phy_regs,
					  UMS9230_ANLG_PHY_G10_CSI_2P2_RO_CTRL,
					  UMS9230_ANLG_PHY_G10_CSI_2P2);
		shift1 = 6;
		break;
	case CAMSYS_CSI_RO_S:
		if (enable)
			regmap_clear_bits(csi->camsys->anlg_phy_regs,
					  UMS9230_ANLG_PHY_G10_CSI_2P2_RO_CTRL,
					  UMS9230_ANLG_PHY_G10_CSI_2P2);
		shift1 = 8;
		break;
	default:
		return;
	}

	mask = 3 << shift1;
	val = id << shift1;
	if (shift2 >= 0) {
		mask |= 3 << shift2;
		val |= id << shift2;
	}

	regmap_update_bits(csi->camsys->cam_ahb_regs,
			   UMS9230_MM_AHB_CSI_PHY_SEL, mask, val);
}

static void ums9230_csi_connect_dcam(struct sprd_csi *csi, int enable)
{
	u32 val, reg = UMS9230_MM_AHB_DCAM_CSI_SEL(csi->dcam_id);
	u32 index = enable ? csi->index : 3;

	val = UMS9230_MM_AHB_DCAM_CSI_SEL_FORCE |
	      FIELD_PREP(UMS9230_MM_AHB_DCAM_CSI_SEL_INDEX, index) |
	      UMS9230_MM_AHB_DCAM_CSI_SEL_FORCE_TRIGGER;
	regmap_update_bits(csi->camsys->cam_ahb_regs, reg,
			   UMS9230_MM_AHB_DCAM_CSI_SEL_FORCE |
			   UMS9230_MM_AHB_DCAM_CSI_SEL_INDEX |
			   UMS9230_MM_AHB_DCAM_CSI_SEL_FORCE_TRIGGER,
			   val);

	regmap_clear_bits(csi->camsys->cam_ahb_regs, reg,
			  UMS9230_MM_AHB_DCAM_CSI_SEL_FORCE_TRIGGER);
}

static const struct sprd_csi_hw_ops ums9230_csi_ops = {
	.power_phy = ums9230_csi_power_phy,
	.setup_phy = ums9230_csi_setup_phy,
	.connect_dcam = ums9230_csi_connect_dcam,
};

static void ums9230_dcam_set_path(struct sprd_dcam *dcam, u32 mbus_code,
				  u32 width, u32 height)
{
	u32 val, hwfmt, imgtype, bayer;

	switch (mbus_code) {
	default:
	case MEDIA_BUS_FMT_SBGGR8_1X8:
	case MEDIA_BUS_FMT_SGBRG8_1X8:
	case MEDIA_BUS_FMT_SGRBG8_1X8:
	case MEDIA_BUS_FMT_SRGGB8_1X8:
		hwfmt = DCAM_HWFMT_RAW_8;
		imgtype = DCAM_IMG_TYPE_RAW8;
		bayer = DCAM_BAYER_8BIT;
		break;
	case MEDIA_BUS_FMT_SBGGR10_1X10:
	case MEDIA_BUS_FMT_SGBRG10_1X10:
	case MEDIA_BUS_FMT_SGRBG10_1X10:
	case MEDIA_BUS_FMT_SRGGB10_1X10:
		hwfmt = DCAM_HWFMT_RAW_PACK_10;
		imgtype = DCAM_IMG_TYPE_RAW10;
		bayer = DCAM_BAYER_10BIT;
		break;
	}

	switch (mbus_code) {
	default:
	case MEDIA_BUS_FMT_SBGGR8_1X8:
	case MEDIA_BUS_FMT_SBGGR10_1X10:
		bayer |= DCAM_BAYER_BGGR;
		break;
	case MEDIA_BUS_FMT_SGBRG8_1X8:
	case MEDIA_BUS_FMT_SGBRG10_1X10:
		bayer |= DCAM_BAYER_GBRG;
		break;
	case MEDIA_BUS_FMT_SGRBG8_1X8:
	case MEDIA_BUS_FMT_SGRBG10_1X10:
		bayer |= DCAM_BAYER_GRBG;
		break;
	case MEDIA_BUS_FMT_SRGGB8_1X8:
	case MEDIA_BUS_FMT_SRGGB10_1X10:
		bayer |= DCAM_BAYER_RGGB;
		break;
	}

	val = DCAM_MIPI_CAP_MODE | DCAM_MIPI_CAP_RAW;
	writel(val, dcam->regs + DCAM_MIPI_CAP_CFG);

	val = readl(dcam->regs + DCAM_BAYER_INFO_CFG);
	val &= ~(DCAM_BAYER_BITS | DCAM_BAYER_PATTERN);
	val |= bayer;
	writel(val, dcam->regs + DCAM_BAYER_INFO_CFG);

	val = FIELD_PREP(DCAM_DIM_Y, height - 1) |
	      FIELD_PREP(DCAM_DIM_X, width - 1);
	writel(val, dcam->regs + DCAM_MIPI_CAP_END);

	val = DCAM_FULL_SRC_SEL | FIELD_PREP(DCAM_FULL_HWFMT, hwfmt);
	writel(val, dcam->regs + DCAM_FULL_CFG);

	val = imgtype | FIELD_PREP(DCAM_IMAGE_MODE, 1);
	writel(val, dcam->regs + DCAM_IMAGE_CONTROL);
}

static void ums9230_dcam_config_ae(struct sprd_dcam *dcam,
				   union sprd_camsys_block_config *block)
{
	struct sprd_camsys_ae_config *ae = &block->ae;
	u32 val;

	if (block->header.flags & SPRD_CAMSYS_FLAG_BYPASS) {
		dcam->stats.blocks_enabled &= ~SPRD_CAMSYS_STATS_AE;
		return;
	}

	dcam->stats.blocks_enabled |= SPRD_CAMSYS_STATS_AE;

	/* Protect the stats buffer from out-of-bounds access. */
	if (ae->blk_num_x > SPRD_CAMSYS_AE_MAX_BLOCKS_X ||
	    ae->blk_num_y > SPRD_CAMSYS_AE_MAX_BLOCKS_Y) {
		dev_dbg(dcam->dev,
			"not setting invalid AE config: %dx%d blocks\n",
			ae->blk_num_x, ae->blk_num_y);
		return;
	}

	val = FIELD_PREP(DCAM_DIM_Y, ae->offset_y) |
	      FIELD_PREP(DCAM_DIM_X, ae->offset_x);
	writel(val, dcam->regs + DCAM_AEM_OFFSET);

	val = FIELD_PREP(DCAM_AEM_BLK_NUM_Y, ae->blk_num_y) |
	      FIELD_PREP(DCAM_AEM_BLK_NUM_X, ae->blk_num_x);
	writel(val, dcam->regs + DCAM_AEM_BLK_NUM);

	val = FIELD_PREP(DCAM_AEM_BLK_HEIGHT, ae->blk_height) |
	      FIELD_PREP(DCAM_AEM_BLK_WIDTH, ae->blk_width);
	writel(val, dcam->regs + DCAM_AEM_BLK_SIZE);

	val = FIELD_PREP(DCAM_AEM_RED_THR_LOW, ae->r_low) |
	      FIELD_PREP(DCAM_AEM_RED_THR_HIGH, ae->r_high);
	writel(val, dcam->regs + DCAM_AEM_RED_THR);

	val = FIELD_PREP(DCAM_AEM_GREEN_THR_LOW, ae->g_low) |
	      FIELD_PREP(DCAM_AEM_GREEN_THR_HIGH, ae->g_high);
	writel(val, dcam->regs + DCAM_AEM_GREEN_THR);

	val = FIELD_PREP(DCAM_AEM_BLUE_THR_LOW, ae->b_low) |
	      FIELD_PREP(DCAM_AEM_BLUE_THR_HIGH, ae->b_high);
	writel(val, dcam->regs + DCAM_AEM_BLUE_THR);
}

static void ums9230_dcam_config_awb(struct sprd_dcam *dcam,
				    union sprd_camsys_block_config *block)
{
	struct sprd_camsys_awb_config *awb = &block->awb;
	u32 val;

	if (block->header.flags & SPRD_CAMSYS_FLAG_BYPASS) {
		writel(DCAM_ISP_AWBC_BYPASS,
		       dcam->regs + DCAM_ISP_AWBC_GAIN0);
		return;
	}

	val = FIELD_PREP(DCAM_ISP_AWBC_GAIN_B, awb->gain_b) |
	      FIELD_PREP(DCAM_ISP_AWBC_GAIN_R, awb->gain_r);
	writel(val, dcam->regs + DCAM_ISP_AWBC_GAIN0);

	val = FIELD_PREP(DCAM_ISP_AWBC_GAIN_GB, awb->gain_gb) |
	      FIELD_PREP(DCAM_ISP_AWBC_GAIN_GR, awb->gain_gr);
	writel(val, dcam->regs + DCAM_ISP_AWBC_GAIN1);

	val = FIELD_PREP(DCAM_ISP_AWBC_OFFSET_B, awb->offset_b) |
	      FIELD_PREP(DCAM_ISP_AWBC_OFFSET_R, awb->offset_r);
	writel(val, dcam->regs + DCAM_ISP_AWBC_OFFSET0);

	val = FIELD_PREP(DCAM_ISP_AWBC_OFFSET_GB, awb->offset_gb) |
	      FIELD_PREP(DCAM_ISP_AWBC_OFFSET_GR, awb->offset_gr);
	writel(val, dcam->regs + DCAM_ISP_AWBC_OFFSET1);
}

static void ums9230_dcam_config_blc(struct sprd_dcam *dcam,
				    union sprd_camsys_block_config *block)
{
	struct sprd_camsys_blc_config *blc = &block->blc;
	u32 val;

	if (block->header.flags & SPRD_CAMSYS_FLAG_BYPASS) {
		writel(DCAM_BLC_BYPASS, dcam->regs + DCAM_BLC_PARA_R_B);
		return;
	}

	val = FIELD_PREP(DCAM_BLC_PARA_B, blc->b) |
	      FIELD_PREP(DCAM_BLC_PARA_R, blc->r);
	writel(val, dcam->regs + DCAM_BLC_PARA_R_B);

	val = FIELD_PREP(DCAM_BLC_PARA_GB, blc->gb) |
	      FIELD_PREP(DCAM_BLC_PARA_GR, blc->gr);
	writel(val, dcam->regs + DCAM_BLC_PARA_G);
}

static void ums9230_dcam_set_capture_buf(struct sprd_dcam *dcam,
					 struct sprd_capture_buffer *buf)
{
	u32 val;

	val = readl(dcam->regs + DCAM_FULL_CFG);
	if (buf)
		val |= DCAM_FULL_PATH_EN;
	else
		val &= ~DCAM_FULL_PATH_EN;
	writel(val, dcam->regs + DCAM_FULL_CFG);

	if (buf)
		writel(buf->addr, dcam->regs + DCAM_FULL_BASE_WADDR);
}

static void ums9230_dcam_set_stats_buf(struct sprd_dcam *dcam,
				       struct sprd_stats_buffer *buf)
{
	if (buf && (dcam->stats.blocks_enabled & SPRD_CAMSYS_STATS_AE)) {
		writel(0, dcam->regs + DCAM_AEM_FRM_CTRL0);
		writel(DCAM_AEM_FRM_SINGLE_START,
		       dcam->regs + DCAM_AEM_FRM_CTRL1);

		writel(buf->ae_addr, dcam->regs + DCAM_AEM_BASE_WADDR);
	} else {
		writel(DCAM_AEM_BYPASS, dcam->regs + DCAM_AEM_FRM_CTRL0);
	}
}

static void ums9230_dcam_start(struct sprd_dcam *dcam)
{
	u32 val;

	val = UMS9230_DCAM_IRQ_CAP_SOF |
	      UMS9230_DCAM_IRQ_DCAM_OVF |
	      UMS9230_DCAM_IRQ_CAP_LINE_ERR |
	      UMS9230_DCAM_IRQ_CAP_FRM_ERR |
	      UMS9230_DCAM_IRQ_FULL_PATH_TX_DONE |
	      UMS9230_DCAM_IRQ_AEM_TX_DONE |
	      UMS9230_DCAM_IRQ_MMU_INT;
	writel(val, dcam->regs + DCAM_INT_EN);

	/* Update shadow registers now to start capturing the first frame. */
	writel(DCAM_FORCE_COPY_ALL, dcam->regs + DCAM_CONTROL);

	val = readl(dcam->regs + DCAM_MIPI_CAP_CFG);
	val |= DCAM_MIPI_CAP_EN;
	writel(val, dcam->regs + DCAM_MIPI_CAP_CFG);

	writel(1, dcam->regs + DCAM_APB_SRAM_CTRL);
}

static void ums9230_dcam_stop(struct sprd_dcam *dcam)
{
	unsigned long timeout;
	u32 val;

	writel(0, dcam->regs + DCAM_MIPI_CAP_CFG);
	writel(0xffffffff, dcam->regs + DCAM_PATH_STOP);
	writel(0, dcam->regs + DCAM_INT_EN);
	writel(0xffffffff, dcam->regs + DCAM_INT_CLR);

	timeout = jiffies + msecs_to_jiffies(2000);
	while (time_before(jiffies, timeout)) {
		val = readl(dcam->regs + DCAM_PATH_BUSY);
		if (!val)
			break;
		usleep_range(500, 1000);
	}

	if (val)
		dev_warn(dcam->dev, "dcam%d stop timeout\n", dcam->index);
}

static irqreturn_t ums9230_handle_irq(int irq, void *irqdata)
{
	struct sprd_dcam *dcam = irqdata;
	u32 val;

	val = readl(dcam->regs + DCAM_INT_MASK);
	writel(val, dcam->regs + DCAM_INT_CLR);

	dev_dbg(dcam->dev, "dcam%d IRQ: %08x\n", dcam->index, val);

	if (val & UMS9230_DCAM_IRQ_FULL_PATH_TX_DONE)
		sprd_capture_done(&dcam->capture, dcam->sequence - 1);

	if (val & UMS9230_DCAM_IRQ_AEM_TX_DONE)
		sprd_stats_done(&dcam->stats, dcam->sequence - 1,
				SPRD_CAMSYS_STATS_AE);

	/*
	 * It is important to handle SOF after TX_DONE so that the buffers
	 * are always returned on time.
	 */
	if (val & UMS9230_DCAM_IRQ_CAP_SOF) {
		sprd_dcam_sof(dcam);

		/*
		 * Tell the hardware to update the shadow registers once the
		 * next frame is ready to be processed.
		 */
		writel(DCAM_AUTO_COPY_ALL, dcam->regs + DCAM_CONTROL);
	}

	if (val & UMS9230_DCAM_IRQ_DCAM_OVF)
		dev_err(dcam->dev, "dcam%d overflow\n", dcam->index);
	if (val & UMS9230_DCAM_IRQ_CAP_LINE_ERR)
		dev_err(dcam->dev, "dcam%d line error\n", dcam->index);
	if (val & UMS9230_DCAM_IRQ_CAP_FRM_ERR)
		dev_err(dcam->dev, "dcam%d frame error\n", dcam->index);
	if (val & UMS9230_DCAM_IRQ_MMU_INT)
		dev_err(dcam->dev, "dcam%d mmu error\n", dcam->index);

	return IRQ_HANDLED;
}

static const struct sprd_dcam_hw_ops ums9230_dcam_ops = {
	.config_block = {
		[SPRD_DCAM_BLOCK_AE] = ums9230_dcam_config_ae,
		[SPRD_DCAM_BLOCK_AWB] = ums9230_dcam_config_awb,
		[SPRD_DCAM_BLOCK_BLC] = ums9230_dcam_config_blc,
	},

	.set_path = ums9230_dcam_set_path,
	.set_capture_buf = ums9230_dcam_set_capture_buf,
	.set_stats_buf = ums9230_dcam_set_stats_buf,
	.start = ums9230_dcam_start,
	.stop = ums9230_dcam_stop,
	.handle_irq = ums9230_handle_irq,
};

static const struct sprd_camsys_fmt_config ums9230_camsys_formats[] = {
	SPRD_CAMSYS_FMT(SBGGR8_1X8, SBGGR8, 1, 8),
	SPRD_CAMSYS_FMT(SGBRG8_1X8, SGBRG8, 1, 8),
	SPRD_CAMSYS_FMT(SGRBG8_1X8, SGRBG8, 1, 8),
	SPRD_CAMSYS_FMT(SRGGB8_1X8, SRGGB8, 1, 8),
	SPRD_CAMSYS_FMT(SBGGR10_1X10, SBGGR10P, 4, 10),
	SPRD_CAMSYS_FMT(SGBRG10_1X10, SGBRG10P, 4, 10),
	SPRD_CAMSYS_FMT(SGRBG10_1X10, SGRBG10P, 4, 10),
	SPRD_CAMSYS_FMT(SRGGB10_1X10, SRGGB10P, 4, 10),
};

const struct sprd_camsys_hardware ums9230_camsys_info = {
	.csi_ops = &ums9230_csi_ops,
	.dcam_ops = &ums9230_dcam_ops,
	.max_width = 8048,
	.max_height = 6036,
	.formats = ums9230_camsys_formats,
	.num_formats = ARRAY_SIZE(ums9230_camsys_formats),
};
