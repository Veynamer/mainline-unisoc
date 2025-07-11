// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unisoc ISP driver
 *
 * Copyright (C) 2025 Otto Pflüger
 */

#include <linux/delay.h>

#include "isp.h"
#include "isp-coeffs.h"
#include "isp-hw-ums9230.h"

static const u32 isp_intc_base_map[SPRD_ISP_NUM_CONTEXTS] = {
	ISP_P0_INT_BASE,
	ISP_P1_INT_BASE,
	ISP_C0_INT_BASE,
	ISP_C1_INT_BASE,
};

#define ISP_CFG_RANGE(first, last) \
	((first) | (((last) + 4 - (first)) << 16))

#define ISP_CFG_REG(reg) ISP_CFG_RANGE(reg, reg)

/*
 * This array is passed to the hardware and defines which registers are updated
 * from the configuration memory. When implementing a module, make sure its
 * full register range is included here.
 */
static const u32 isp_cfg_map[] = {
	ISP_CFG_RANGE(ISP_COMMON_SPACE_SEL, ISP_COMMON_SCL_PATH_SEL),
	ISP_CFG_RANGE(ISP_CMC10_PARAM, ISP_CMC10_MATRIX4),
	ISP_CFG_REG(ISP_GAMMA_PARAM),
	ISP_CFG_RANGE(ISP_FGAMMA_R_BUF0, ISP_FGAMMA_R_BUF0 + 0x3fc),
	ISP_CFG_RANGE(ISP_FGAMMA_G_BUF0, ISP_FGAMMA_G_BUF0 + 0x3fc),
	ISP_CFG_RANGE(ISP_FGAMMA_B_BUF0, ISP_FGAMMA_B_BUF0 + 0x3fc),
	ISP_CFG_RANGE(ISP_FETCH_PARAM, ISP_FETCH_LINE_DLY_CTRL),
	ISP_CFG_RANGE(ISP_DISPATCH_CH0_BAYER, ISP_DISPATCH_CH0_SIZE),
	ISP_CFG_RANGE(ISP_YUV_SCALER_PRE_CAP_BASE + ISP_YUV_SCALER_CFG,
		      ISP_YUV_SCALER_PRE_CAP_BASE + ISP_YUV_SCALER_FACTOR_VER),
	ISP_CFG_RANGE(ISP_YUV_SCALER_PRE_CAP_LUMA_HCOEF0_BUF,
		      ISP_YUV_SCALER_PRE_CAP_LUMA_HCOEF0_BUF + 0x7c),
	ISP_CFG_RANGE(ISP_YUV_SCALER_PRE_CAP_CHROMA_HCOEF0_BUF,
		      ISP_YUV_SCALER_PRE_CAP_CHROMA_HCOEF0_BUF + 0x3c),
	ISP_CFG_RANGE(ISP_YUV_SCALER_PRE_CAP_LUMA_HCOEF1_BUF,
		      ISP_YUV_SCALER_PRE_CAP_LUMA_HCOEF1_BUF + 0x7c),
	ISP_CFG_RANGE(ISP_YUV_SCALER_PRE_CAP_CHROMA_HCOEF1_BUF,
		      ISP_YUV_SCALER_PRE_CAP_CHROMA_HCOEF1_BUF + 0x3c),
	ISP_CFG_RANGE(ISP_YUV_SCALER_PRE_CAP_LUMA_HCOEF2_BUF,
		      ISP_YUV_SCALER_PRE_CAP_LUMA_HCOEF2_BUF + 0x7c),
	ISP_CFG_RANGE(ISP_YUV_SCALER_PRE_CAP_CHROMA_HCOEF2_BUF,
		      ISP_YUV_SCALER_PRE_CAP_CHROMA_HCOEF2_BUF + 0x3c),
	ISP_CFG_RANGE(ISP_YUV_SCALER_PRE_CAP_LUMA_HCOEF3_BUF,
		      ISP_YUV_SCALER_PRE_CAP_LUMA_HCOEF3_BUF + 0x7c),
	ISP_CFG_RANGE(ISP_YUV_SCALER_PRE_CAP_CHROMA_HCOEF3_BUF,
		      ISP_YUV_SCALER_PRE_CAP_CHROMA_HCOEF3_BUF + 0x3c),
	ISP_CFG_RANGE(ISP_YUV_SCALER_PRE_CAP_LUMA_VCOEF0_BUF,
		      ISP_YUV_SCALER_PRE_CAP_LUMA_VCOEF0_BUF + 0xfc),
	ISP_CFG_RANGE(ISP_YUV_SCALER_PRE_CAP_LUMA_VCOEF1_BUF,
		      ISP_YUV_SCALER_PRE_CAP_LUMA_VCOEF1_BUF + 0xfc),
	ISP_CFG_RANGE(ISP_YUV_SCALER_PRE_CAP_LUMA_VCOEF2_BUF,
		      ISP_YUV_SCALER_PRE_CAP_LUMA_VCOEF2_BUF + 0xfc),
	ISP_CFG_RANGE(ISP_YUV_SCALER_PRE_CAP_LUMA_VCOEF3_BUF,
		      ISP_YUV_SCALER_PRE_CAP_LUMA_VCOEF3_BUF + 0xfc),
	ISP_CFG_RANGE(ISP_YUV_SCALER_PRE_CAP_CHROMA_VCOEF0_BUF,
		      ISP_YUV_SCALER_PRE_CAP_CHROMA_VCOEF0_BUF + 0xfc),
	ISP_CFG_RANGE(ISP_YUV_SCALER_PRE_CAP_CHROMA_VCOEF1_BUF,
		      ISP_YUV_SCALER_PRE_CAP_CHROMA_VCOEF1_BUF + 0xfc),
	ISP_CFG_RANGE(ISP_YUV_SCALER_PRE_CAP_CHROMA_VCOEF2_BUF,
		      ISP_YUV_SCALER_PRE_CAP_CHROMA_VCOEF2_BUF + 0xfc),
	ISP_CFG_RANGE(ISP_YUV_SCALER_PRE_CAP_CHROMA_VCOEF3_BUF,
		      ISP_YUV_SCALER_PRE_CAP_CHROMA_VCOEF3_BUF + 0xfc),
	ISP_CFG_RANGE(ISP_STORE_PRE_CAP_BASE + ISP_STORE_PARAM,
		      ISP_STORE_PRE_CAP_BASE + ISP_STORE_SLICE_V_PITCH),
};

static void isp_init_regs(struct sprd_isp *isp)
{
	u32 i, val;

	/* Set defaults */
	writel(0x2476, isp->base + ISP_AXI_ARBITER_WQOS);
	writel(0x0076, isp->base + ISP_AXI_ARBITER_RQOS);
	writel(0xffff0000, isp->base + ISP_CORE_PMU_EN);
	writel(0xffff0000, isp->base + ISP_COMMON_GCLK_CTRL_0);
	writel(0xffff0000, isp->base + ISP_COMMON_GCLK_CTRL_1);
	writel(0xffff0000, isp->base + ISP_COMMON_GCLK_CTRL_2);
	writel(0xff00, isp->base + ISP_COMMON_GCLK_CTRL_3);
	writel(0, isp->base + ISP_AXI_ISOLATION);
	writel(0, isp->base + ISP_ARBITER_ENDIAN0);
	writel(0, isp->base + ISP_ARBITER_ENDIAN1);
	writel(0xf10, isp->base + ISP_ARBITER_CHK_SUM_CLR);
	writel(0, isp->base + ISP_ARBITER_CHK_SUM0);
	writel(1, isp->base + ISP_COMMON_PMU_RAM_MASK);

	/* Disable unused modules */
	writel(ISP_GTM_HIST_BYPASS | ISP_GTM_MAP_BYPASS,
	       isp->base + ISP_GTM_GLB_CTRL);
	writel(ISP_FBD_RAW_BYPASS, isp->base + ISP_FBD_RAW_SEL);
	writel(ISP_GRGB_BYPASS, isp->base + ISP_GRGB_CTRL);
	writel(ISP_VST_BYPASS, isp->base + ISP_VST_PARA);
	writel(ISP_IVST_BYPASS, isp->base + ISP_IVST_PARA);
	writel(ISP_NLM_BYPASS, isp->base + ISP_NLM_PARA);
	writel(ISP_HSV_BYPASS, isp->base + ISP_HSV_PARAM);
	writel(ISP_PSTRZ_BYPASS, isp->base + ISP_PSTRZ_PARAM);
	writel(ISP_UVD_BYPASS, isp->base + ISP_UVD_PARAM);
	writel(ISP_PRECDN_BYPASS, isp->base + ISP_PRECDN_PARAM);
	writel(ISP_YNR_BYPASS, isp->base + ISP_YNR_CONTRL0);
	writel(ISP_HIST_BYPASS, isp->base + ISP_HIST_PARAM);
	writel(ISP_CDN_BYPASS, isp->base + ISP_CDN_PARAM);
	writel(ISP_EE_BYPASS, isp->base + ISP_EE_PARAM);
	writel(ISP_BCHS_BYPASS, isp->base + ISP_BCHS_PARAM);
	writel(ISP_POSTCDN_DOWNSAMPLE_BYPASS | ISP_POSTCDN_BYPASS,
	       isp->base + ISP_POSTCDN_COMMON_CTRL);
	writel(ISP_YGAMMA_BYPASS, isp->base + ISP_YGAMMA_PARAM);
	writel(ISP_IIRCNR_BYPASS, isp->base + ISP_IIRCNR_PARAM);
	writel(ISP_YRANDOM_BYPASS, isp->base + ISP_YRANDOM_PARAM1);
	writel(ISP_3DNR_MEM_CTRL_BYPASS, isp->base + ISP_3DNR_MEM_CTRL_PARAM0);
	writel(ISP_3DNR_BLEND_BYPASS, isp->base + ISP_3DNR_BLEND_CONTROL0);
	writel(ISP_3DNR_STORE_BYPASS, isp->base + ISP_3DNR_STORE_PARAM);
	writel(ISP_3DNR_MEM_CTRL_PRE_BYPASS,
	       isp->base + ISP_3DNR_MEM_CTRL_PRE_PARAM0);
	writel(ISP_FBC_3DNR_STORE_BYPASS, isp->base + ISP_FBC_3DNR_STORE_PARAM);
	writel(ISP_YUV_NF_BYPASS, isp->base + ISP_YUV_NF_CTRL);
	writel(ISP_THMB_BYPASS, isp->base + ISP_THMB_CFG);

	val = ISP_STORE_BYPASS;
	writel(val, isp->base + ISP_STORE_DEBUG_BASE + ISP_STORE_PARAM);
	writel(val, isp->base + ISP_STORE_VID_BASE + ISP_STORE_PARAM);

	writel(0, isp->base + ISP_YUV_SCALER_VID_BASE + ISP_YUV_SCALER_CFG);

	/* Enable the CFG module */
	val = FIELD_PREP(ISP_CFG_NUM_OF_MOD, ARRAY_SIZE(isp_cfg_map));
	val |= FIELD_PREP(ISP_CFG_SDW_MODE, 1);
	val |= ISP_CFG_TM_BYPASS;
	writel(val, isp->base + ISP_CFG_PARAMETER);

	for (i = 0; i < ARRAY_SIZE(isp_cfg_map); i++) {
		writel(isp_cfg_map[i], isp->base + ISP_CFG0_BUF + i * 4);
		writel(isp_cfg_map[i], isp->base + ISP_CFG1_BUF + i * 4);
	}

	for (i = 0; i < SPRD_ISP_NUM_CONTEXTS; i++) {
		struct sprd_isp_ctx *ctx = &isp->context[i];

		writel(ctx->cfg_dma_addr,
		       isp->base + ISP_CFG_PRE0_CMD_ADDR + i * 4);

		/* Keep modules that need configuration disabled by default */
		writel(ISP_CMC10_BYPASS, ctx->cfg_base + ISP_CMC10_PARAM);
		writel(ISP_GAMMA_BYPASS, ctx->cfg_base + ISP_GAMMA_PARAM);
	}

	/* Set default debayering parameters */
	writel(0, isp->base + ISP_CFAE_NEW_CFG0);

	val = FIELD_PREP(ISP_CFAE_GRID_THR, 500);
	writel(val, isp->base + ISP_CFAE_INTP_CFG0);

	val = ISP_CFAE_WEIGHT_CONTROL_BYPASS;
	val |= FIELD_PREP(ISP_CFAE_UNI_DIR_INTPLT_THR_NEW, 20);
	val |= FIELD_PREP(ISP_CFAE_STRONG_EDGE_THR, 127);
	writel(val, isp->base + ISP_CFAE_INTP_CFG1);

	val = FIELD_PREP(ISP_CFAE_SMOOTH_AREA_THR, 0);
	val |= FIELD_PREP(ISP_CFAE_CDCR_ADJ_FACTOR, 8);
	writel(val, isp->base + ISP_CFAE_INTP_CFG2);

	val = FIELD_PREP(ISP_CFAE_GRID_DIR_WEIGHT_T2, 8);
	val |= FIELD_PREP(ISP_CFAE_GRID_DIR_WEIGHT_T1, 8);
	val |= FIELD_PREP(ISP_CFAE_REDBLUE_HIGH_SAT_THR, 280);
	writel(val, isp->base + ISP_CFAE_INTP_CFG3);

	val = FIELD_PREP(ISP_CFAE_LOW_LUX_03_THR, 100);
	val |= FIELD_PREP(ISP_CFAE_ROUND_DIFF_03_THR, 100);
	writel(val, isp->base + ISP_CFAE_INTP_CFG4);

	val = FIELD_PREP(ISP_CFAE_LOW_LUX_12_THR, 200);
	val |= FIELD_PREP(ISP_CFAE_ROUND_DIFF_12_THR, 200);
	writel(val, isp->base + ISP_CFAE_INTP_CFG5);

	/* Enable interrupts */
	for (i = 0; i < SPRD_ISP_NUM_CONTEXTS; i++) {
		void __iomem *intc_base = isp->base + isp_intc_base_map[i];

		writel(0xffffffff, intc_base + ISP_INT_EN0);
	}
}

static int isp_set_formats(struct sprd_isp *isp, int ctx_id)
{
	struct sprd_isp_ctx *ctx = &isp->context[ctx_id];
	void __iomem *store_base = ctx->cfg_base + ISP_STORE_PRE_CAP_BASE;
	u32 val, color = ISP_DISPATCH_COLOR_YUV, bayer = 0;
	struct v4l2_pix_format_mplane *fmt;
	const u16 *cce_matrix;
	u16 y_offset;

	fmt = &ctx->input.fmt;
	switch (fmt->pixelformat) {
	case V4L2_PIX_FMT_YUV422M:
		val = ISP_FETCH_YUV422_3FRAME;
		break;
	case V4L2_PIX_FMT_YUYV:
		val = ISP_FETCH_YUYV_1FRAME;
		break;
	case V4L2_PIX_FMT_UYVY:
		val = ISP_FETCH_UYVY_1FRAME;
		break;
	case V4L2_PIX_FMT_YVYU:
		val = ISP_FETCH_YVYU_1FRAME;
		break;
	case V4L2_PIX_FMT_VYUY:
		val = ISP_FETCH_VYUY_1FRAME;
		break;
	case V4L2_PIX_FMT_NV16:
	case V4L2_PIX_FMT_NV16M:
		val = ISP_FETCH_YUV422_2FRAME;
		break;
	case V4L2_PIX_FMT_NV61:
	case V4L2_PIX_FMT_NV61M:
		val = ISP_FETCH_YVU422_2FRAME;
		break;
	case V4L2_PIX_FMT_SBGGR10P:
		color = ISP_DISPATCH_COLOR_RAW;
		bayer = ISP_BAYER_BGGR;
		val = ISP_FETCH_RAW_PACK_10;
		break;
	case V4L2_PIX_FMT_SGBRG10P:
		color = ISP_DISPATCH_COLOR_RAW;
		bayer = ISP_BAYER_GBRG;
		val = ISP_FETCH_RAW_PACK_10;
		break;
	case V4L2_PIX_FMT_SGRBG10P:
		color = ISP_DISPATCH_COLOR_RAW;
		bayer = ISP_BAYER_GRBG;
		val = ISP_FETCH_RAW_PACK_10;
		break;
	case V4L2_PIX_FMT_SRGGB10P:
		color = ISP_DISPATCH_COLOR_RAW;
		bayer = ISP_BAYER_RGGB;
		val = ISP_FETCH_RAW_PACK_10;
		break;
	case V4L2_PIX_FMT_NV12:
	case V4L2_PIX_FMT_NV12M:
		val = ISP_FETCH_YUV420_2FRAME;
		break;
	case V4L2_PIX_FMT_NV21:
	case V4L2_PIX_FMT_NV21M:
		val = ISP_FETCH_YVU420_2FRAME;
		break;
	default:
		return -EINVAL;
	}

	writel(val, ctx->cfg_base + ISP_FETCH_PARAM);
	writel(bayer, ctx->cfg_base + ISP_DISPATCH_CH0_BAYER);

	writel(8, ctx->cfg_base + ISP_FETCH_LINE_DLY_CTRL);

	val = FIELD_PREP(ISP_DEBUG_PATH_SEL, ISP_SCL_PATH_DISABLE);
	val |= FIELD_PREP(ISP_DISPATCH_COLOR, color);
	writel(val, ctx->cfg_base + ISP_COMMON_SPACE_SEL);

	fmt = &ctx->output.fmt;
	switch (fmt->pixelformat) {
	case V4L2_PIX_FMT_NV12:
	case V4L2_PIX_FMT_NV12M:
		val = ISP_STORE_YUV420_2FRAME;
		break;
	case V4L2_PIX_FMT_NV21:
	case V4L2_PIX_FMT_NV21M:
		val = ISP_STORE_YVU420_2FRAME;
		break;
	case V4L2_PIX_FMT_YUV420M:
		val = ISP_STORE_YUV420_3FRAME;
		break;
	default:
		return -EINVAL;
	}

	val |= ISP_STORE_SPEED_2X;
	writel(val, store_base + ISP_STORE_PARAM);

	val = ISP_SCL_RGB_PATH_DISABLE;
	val |= FIELD_PREP(ISP_THUMB_PATH_SEL, ISP_SCL_PATH_DISABLE);
	val |= FIELD_PREP(ISP_VID_PATH_SEL, ISP_SCL_PATH_DISABLE);
	val |= FIELD_PREP(ISP_PRE_CAP_PATH_SEL, ISP_SCL_PATH_NORMAL);
	writel(val, ctx->cfg_base + ISP_COMMON_SCL_PATH_SEL);

	cce_matrix = sprd_isp_get_cce_matrix(fmt, &y_offset);

	writel(0, isp->base + ISP_CCE_PARAM);

	val = FIELD_PREP(ISP_CCE_MATRIX_Y_R, cce_matrix[0]) |
	      FIELD_PREP(ISP_CCE_MATRIX_Y_G, cce_matrix[1]);
	writel(val, isp->base + ISP_CCE_MATRIX0);
	val = FIELD_PREP(ISP_CCE_MATRIX_Y_B, cce_matrix[2]) |
	      FIELD_PREP(ISP_CCE_MATRIX_U_R, cce_matrix[3]);
	writel(val, isp->base + ISP_CCE_MATRIX1);
	val = FIELD_PREP(ISP_CCE_MATRIX_U_G, cce_matrix[4]) |
	      FIELD_PREP(ISP_CCE_MATRIX_U_B, cce_matrix[5]);
	writel(val, isp->base + ISP_CCE_MATRIX2);
	val = FIELD_PREP(ISP_CCE_MATRIX_V_R, cce_matrix[6]) |
	      FIELD_PREP(ISP_CCE_MATRIX_V_G, cce_matrix[7]);
	writel(val, isp->base + ISP_CCE_MATRIX3);
	val = FIELD_PREP(ISP_CCE_MATRIX_V_B, cce_matrix[8]);
	writel(val, isp->base + ISP_CCE_MATRIX4);

	val = FIELD_PREP(ISP_CCE_Y_OFFSET, y_offset);
	writel(val, isp->base + ISP_CCE_SHIFT);

	return 0;
}

static void isp_config_ccm(struct sprd_isp *isp, int ctx_id,
			   union sprd_camsys_block_config *block)
{
	struct sprd_isp_ctx *ctx = &isp->context[ctx_id];
	struct sprd_camsys_ccm_config *ccm = &block->ccm;
	u32 val;

	if (block->header.flags & SPRD_CAMSYS_FLAG_BYPASS) {
		writel(ISP_CMC10_BYPASS, ctx->cfg_base + ISP_CMC10_PARAM);
		return;
	}

	writel(0, ctx->cfg_base + ISP_CMC10_PARAM);

	val = FIELD_PREP(ISP_CMC10_MATRIX_R_R, ccm->r_r) |
	      FIELD_PREP(ISP_CMC10_MATRIX_R_G, ccm->r_g);
	writel(val, ctx->cfg_base + ISP_CMC10_MATRIX0);

	val = FIELD_PREP(ISP_CMC10_MATRIX_R_B, ccm->r_b) |
	      FIELD_PREP(ISP_CMC10_MATRIX_G_R, ccm->g_r);
	writel(val, ctx->cfg_base + ISP_CMC10_MATRIX1);

	val = FIELD_PREP(ISP_CMC10_MATRIX_G_G, ccm->g_g) |
	      FIELD_PREP(ISP_CMC10_MATRIX_G_B, ccm->g_b);
	writel(val, ctx->cfg_base + ISP_CMC10_MATRIX2);

	val = FIELD_PREP(ISP_CMC10_MATRIX_B_R, ccm->b_r) |
	      FIELD_PREP(ISP_CMC10_MATRIX_B_G, ccm->b_g);
	writel(val, ctx->cfg_base + ISP_CMC10_MATRIX3);

	val = FIELD_PREP(ISP_CMC10_MATRIX_B_B, ccm->b_b);
	writel(val, ctx->cfg_base + ISP_CMC10_MATRIX4);
}

static void isp_config_gamma(struct sprd_isp *isp, int ctx_id,
			     union sprd_camsys_block_config *block)
{
	struct sprd_isp_ctx *ctx = &isp->context[ctx_id];
	struct sprd_camsys_gamma_config *gamma = &block->gamma;
	void *dest;
	u32 i;

	if (block->header.flags & SPRD_CAMSYS_FLAG_BYPASS) {
		writel(ISP_GAMMA_BYPASS, ctx->cfg_base + ISP_GAMMA_PARAM);
		return;
	}

	writel(0, ctx->cfg_base + ISP_GAMMA_PARAM);

	dest = ctx->cfg_base + ISP_FGAMMA_R_BUF0;
	for (i = 0; i < ARRAY_SIZE(gamma->r_tbl) - 1; i++) {
		writel((gamma->r_tbl[i] << 8) | gamma->r_tbl[i + 1], dest);
		dest += 4;
	}

	dest = ctx->cfg_base + ISP_FGAMMA_G_BUF0;
	for (i = 0; i < ARRAY_SIZE(gamma->r_tbl) - 1; i++) {
		writel((gamma->r_tbl[i] << 8) | gamma->r_tbl[i + 1], dest);
		dest += 4;
	}

	dest = ctx->cfg_base + ISP_FGAMMA_B_BUF0;
	for (i = 0; i < ARRAY_SIZE(gamma->r_tbl) - 1; i++) {
		writel((gamma->r_tbl[i] << 8) | gamma->r_tbl[i + 1], dest);
		dest += 4;
	}
}

static void isp_set_scaler_coefficient_bank(void __iomem *scaler_base,
					    void __iomem *hor_luma,
					    void __iomem *hor_chroma,
					    void __iomem *ver_luma,
					    void __iomem *ver_chroma,
					    u32 ver_tap_num)
{
	u32 i, val;

	/*
	 * TODO: implement interpolation
	 *
	 * At the moment, this just sets simplest possible coefficients to
	 * get working video.
	 */
	/*
	 * Note: half-word accesses work with the config buffer, but not with
	 * the actual hardware registers.
	 */
	for (i = 0; i < 256; i++)
		writew((i % 8) == 4 ? 0xff : 0, hor_luma + i * 2);

	for (i = 0; i < 128; i++)
		writew((i % 4) == 2 ? 0xff : 0, hor_chroma + i * 2);

	for (i = 0; i < 256; i++) {
		if ((i % ver_tap_num) == (ver_tap_num / 2))
			val = 0xff;
		else
			val = 0;

		writel(val, ver_luma + i * 4);
		writel(val, ver_chroma + i * 4);
	}
}

static void isp_config_scaler(struct sprd_isp_ctx *ctx)
{
	void __iomem *scaler_base = ctx->cfg_base + ISP_YUV_SCALER_PRE_CAP_BASE;
	u32 val, ver_tap_num;

	if (ctx->input.rect.height == ctx->output.rect.height &&
	    ctx->input.rect.width == ctx->output.rect.width) {
		val = ISP_YUV_SCALER_PATH_ENABLE;
		val |= ISP_YUV_SCALER_WORK_MODE_420_420;
		val |= ISP_YUV_SCALER_BYPASS_SCALE;
		val |= ISP_YUV_SCALER_BYPASS_ALL;
		writel(val, scaler_base + ISP_YUV_SCALER_CFG);
		return;
	}

	ver_tap_num = clamp((ctx->input.rect.height /
			     ctx->output.rect.height) * 2, 4, 8);

	val = ISP_YUV_SCALER_PATH_ENABLE;
	val |= ISP_YUV_SCALER_WORK_MODE_420_420;
	val |= FIELD_PREP(ISP_YUV_SCALER_Y_VER_TAP, ver_tap_num);
	val |= FIELD_PREP(ISP_YUV_SCALER_UV_VER_TAP, ver_tap_num);
	val |= FIELD_PREP(ISP_YUV_SCALER_DECI_Y, 1);
	val |= FIELD_PREP(ISP_YUV_SCALER_DECI_X, 1);
	writel(val, scaler_base + ISP_YUV_SCALER_CFG);

	val = FIELD_PREP(ISP_FACTOR_IN, ctx->input.total_width);
	val |= FIELD_PREP(ISP_FACTOR_OUT, ctx->output.total_width);
	writel(val, scaler_base + ISP_YUV_SCALER_FACTOR);

	val = FIELD_PREP(ISP_FACTOR_IN, ctx->input.rect.height);
	val |= FIELD_PREP(ISP_FACTOR_OUT, ctx->output.rect.height);
	writel(val, scaler_base + ISP_YUV_SCALER_FACTOR_VER);

	isp_set_scaler_coefficient_bank(scaler_base,
		ctx->cfg_base + ISP_YUV_SCALER_PRE_CAP_LUMA_HCOEF0_BUF,
		ctx->cfg_base + ISP_YUV_SCALER_PRE_CAP_CHROMA_HCOEF0_BUF,
		ctx->cfg_base + ISP_YUV_SCALER_PRE_CAP_LUMA_VCOEF0_BUF,
		ctx->cfg_base + ISP_YUV_SCALER_PRE_CAP_CHROMA_VCOEF0_BUF,
		ver_tap_num);
	isp_set_scaler_coefficient_bank(scaler_base,
		ctx->cfg_base + ISP_YUV_SCALER_PRE_CAP_LUMA_HCOEF1_BUF,
		ctx->cfg_base + ISP_YUV_SCALER_PRE_CAP_CHROMA_HCOEF1_BUF,
		ctx->cfg_base + ISP_YUV_SCALER_PRE_CAP_LUMA_VCOEF1_BUF,
		ctx->cfg_base + ISP_YUV_SCALER_PRE_CAP_CHROMA_VCOEF1_BUF,
		ver_tap_num);
	isp_set_scaler_coefficient_bank(scaler_base,
		ctx->cfg_base + ISP_YUV_SCALER_PRE_CAP_LUMA_HCOEF2_BUF,
		ctx->cfg_base + ISP_YUV_SCALER_PRE_CAP_CHROMA_HCOEF2_BUF,
		ctx->cfg_base + ISP_YUV_SCALER_PRE_CAP_LUMA_VCOEF2_BUF,
		ctx->cfg_base + ISP_YUV_SCALER_PRE_CAP_CHROMA_VCOEF2_BUF,
		ver_tap_num);
	isp_set_scaler_coefficient_bank(scaler_base,
		ctx->cfg_base + ISP_YUV_SCALER_PRE_CAP_LUMA_HCOEF3_BUF,
		ctx->cfg_base + ISP_YUV_SCALER_PRE_CAP_CHROMA_HCOEF3_BUF,
		ctx->cfg_base + ISP_YUV_SCALER_PRE_CAP_LUMA_VCOEF3_BUF,
		ctx->cfg_base + ISP_YUV_SCALER_PRE_CAP_CHROMA_VCOEF3_BUF,
		ver_tap_num);
}

static int isp_start_slice(struct sprd_isp *isp, int ctx_id,
			   dma_addr_t *in_addr, dma_addr_t *out_addr,
			   u32 *in_pitch, u32 *out_pitch)
{
	struct sprd_isp_ctx *ctx = &isp->context[ctx_id];
	void __iomem *scaler_base = ctx->cfg_base + ISP_YUV_SCALER_PRE_CAP_BASE;
	void __iomem *store_base = ctx->cfg_base + ISP_STORE_PRE_CAP_BASE;
	u32 pix = ctx->input.fmt.pixelformat;
	u32 val;

	val = FIELD_PREP(ISP_DIM_Y, ctx->input.rect.height);
	val |= FIELD_PREP(ISP_DIM_X, ctx->input.rect.width);
	writel(val, ctx->cfg_base + ISP_FETCH_SLICE_SIZE);
	writel(val, ctx->cfg_base + ISP_DISPATCH_CH0_SIZE);
	writel(val, scaler_base + ISP_YUV_SCALER_SRC_SIZE);
	writel(val, scaler_base + ISP_YUV_SCALER_TRIM0_SIZE);

	if (pix == V4L2_PIX_FMT_SBGGR10P || pix == V4L2_PIX_FMT_SGBRG10P ||
	    pix == V4L2_PIX_FMT_SGRBG10P || pix == V4L2_PIX_FMT_SRGGB10P) {
		u32 start = ctx->input.rect.left;
		u32 end = ctx->input.rect.left + ctx->input.rect.width;
		u32 start_word = ((start / 4) * 5 + (start % 4)) / 4;
		u32 end_word = DIV_ROUND_UP(DIV_ROUND_UP(end, 4) * 5, 4);
		u32 rel_pos = ctx->input.rect.left % 16;

		dev_dbg(isp->dev,
			"addr %08llx, mipi start %u end %u, rel_pos %u\n",
			in_addr[0], start_word, end_word, rel_pos);

		val = FIELD_PREP(ISP_FETCH_MIPI_WORD_NUM,
				 end_word - start_word);
		val |= FIELD_PREP(ISP_FETCH_MIPI_BYTE_REL_POS, rel_pos);
		writel(val, ctx->cfg_base + ISP_FETCH_MIPI_INFO);
	}

	writel(in_addr[0], ctx->cfg_base + ISP_FETCH_SLICE_Y_ADDR);
	writel(in_addr[1], ctx->cfg_base + ISP_FETCH_SLICE_U_ADDR);
	writel(in_addr[2], ctx->cfg_base + ISP_FETCH_SLICE_V_ADDR);
	writel(in_pitch[0], ctx->cfg_base + ISP_FETCH_SLICE_Y_PITCH);
	writel(in_pitch[1], ctx->cfg_base + ISP_FETCH_SLICE_U_PITCH);
	writel(in_pitch[2], ctx->cfg_base + ISP_FETCH_SLICE_V_PITCH);

	/*
	 * Configure the scaling ratio only for the first slice since it
	 * should not change until the next frame.
	 */
	if (ctx->slice_idx == 0)
		isp_config_scaler(ctx);

	val = FIELD_PREP(ISP_DIM_Y, ctx->output.rect.height);
	val |= FIELD_PREP(ISP_DIM_X, ctx->output.rect.width);
	writel(val, store_base + ISP_STORE_SLICE_SIZE);
	writel(val, scaler_base + ISP_YUV_SCALER_DES_SIZE);
	writel(val, scaler_base + ISP_YUV_SCALER_TRIM1_SIZE);

	writel(out_addr[0], store_base + ISP_STORE_SLICE_Y_ADDR);
	writel(out_addr[1], store_base + ISP_STORE_SLICE_U_ADDR);
	writel(out_addr[2], store_base + ISP_STORE_SLICE_V_ADDR);
	writel(out_pitch[0], store_base + ISP_STORE_SLICE_Y_PITCH);
	writel(out_pitch[1], store_base + ISP_STORE_SLICE_U_PITCH);
	writel(out_pitch[2], store_base + ISP_STORE_SLICE_V_PITCH);

	writel(1, isp->base + ISP_CFG_PRE0_START + ctx_id * 4);

	return 0;
}

static void isp_stop(struct sprd_isp *isp)
{
	u32 i, val;

	val = readl(isp->base + ISP_AXI_ITI2AXIM_CTRL);
	val |= ISP_AXI_STOP;
	writel(val, isp->base + ISP_AXI_ITI2AXIM_CTRL);

	udelay(10);
	for (i = 0; i < SPRD_ISP_NUM_CONTEXTS; i++) {
		void __iomem *intc_base = isp->base + isp_intc_base_map[i];

		writel(0xffffffff, intc_base + ISP_INT_CLR0);
	}

	reset_control_assert(isp->reset);
	reset_control_assert(isp->ahb_reset);
	udelay(10);
	reset_control_deassert(isp->ahb_reset);
	reset_control_deassert(isp->reset);
}

static irqreturn_t isp_handle_irq(int irq, void *devdata)
{
	struct sprd_isp *isp = devdata;
	u32 i, int_sts;

	if (irq == isp->irq[0])
		i = 0;
	else if (irq == isp->irq[1])
		i = 1;
	else
		return IRQ_NONE;

	for (; i < SPRD_ISP_NUM_CONTEXTS; i += 2) {
		void __iomem *intc_base = isp->base + isp_intc_base_map[i];

		int_sts = readl(intc_base + ISP_INT_INT0);
		writel(int_sts, intc_base + ISP_INT_CLR0);

		dev_dbg(isp->dev, "ctx%d status %08x\n", i, int_sts);

		if (int_sts & ISP_IRQ_ALL_DONE) {
			spin_lock(&isp->lock);
			sprd_isp_process(isp, i);
			spin_unlock(&isp->lock);
		}
	}

	return IRQ_HANDLED;
}

const struct sprd_isp_hardware ums9230_isp_hw = {
	.config_block = {
		[SPRD_ISP_BLOCK_CCM] = isp_config_ccm,
		[SPRD_ISP_BLOCK_GAMMA] = isp_config_gamma,
	},

	.init_regs = isp_init_regs,
	.set_formats = isp_set_formats,
	.start_slice = isp_start_slice,
	.stop = isp_stop,
	.handle_irq = isp_handle_irq,

	.max_width = 2560,
	.max_height = 9000,
};
