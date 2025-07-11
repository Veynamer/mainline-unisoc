// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unisoc ISP driver - coefficient tables
 *
 * Copyright (C) 2025 Otto Pflüger
 */

#include "isp-coeffs.h"

struct cce_matrix_info {
	u16 full[9];
	u16 lim[9];
};

static const struct cce_matrix_info cce_matrix_bt601 = {
	.full = {
		0x004d, 0x0096, 0x001d,
		0x07d5, 0x07ab, 0x0080,
		0x0080, 0x0795, 0x07eb,
	},
	.lim = {
		0x0042, 0x0081, 0x0019,
		0x07da, 0x07b6, 0x0070,
		0x0070, 0x07a2, 0x07ee,
	},
};

static const struct cce_matrix_info cce_matrix_bt709 = {
	.full = {
		0x0036, 0x00b7, 0x0012,
		0x07e3, 0x079d, 0x0080,
		0x0080, 0x078c, 0x07f4,
	},
	.lim = {
		0x002f, 0x009d, 0x0010,
		0x07e6, 0x07a9, 0x0071,
		0x0070, 0x079a, 0x07f6,
	},
};

static const struct cce_matrix_info cce_matrix_bt2020 = {
	.full = {
		0x0043, 0x00ae, 0x000f,
		0x07dc, 0x07a4, 0x0080,
		0x0080, 0x078a, 0x07f6,
	},
	.lim = {
		0x003a, 0x0095, 0x000d,
		0x07e1, 0x07af, 0x0070,
		0x0070, 0x0799, 0x07f7,
	},
};

static const struct cce_matrix_info cce_matrix_smpte240m = {
	.full = {
		0x0036, 0x00b4, 0x0016,
		0x07e2, 0x079e, 0x0080,
		0x0080, 0x078e, 0x07f2,
	},
	.lim = {
		0x002f, 0x009a, 0x0013,
		0x07e6, 0x07aa, 0x0070,
		0x0070, 0x079c, 0x07f4,
	},
};

const u16 *sprd_isp_get_cce_matrix(struct v4l2_pix_format_mplane *fmt,
				   u16 *y_offset)
{
	const struct cce_matrix_info *m;
	u32 ycbcr_enc, quantization;

	ycbcr_enc = fmt->ycbcr_enc ? :
			V4L2_MAP_YCBCR_ENC_DEFAULT(fmt->colorspace);

	quantization = fmt->quantization ? :
			V4L2_MAP_QUANTIZATION_DEFAULT(false, fmt->colorspace,
						      fmt->ycbcr_enc);

	switch (ycbcr_enc) {
	default:
	case V4L2_YCBCR_ENC_601:
		m = &cce_matrix_bt601;
		break;
	case V4L2_YCBCR_ENC_709:
		m = &cce_matrix_bt709;
		break;
	case V4L2_YCBCR_ENC_BT2020:
		m = &cce_matrix_bt2020;
		break;
	case V4L2_YCBCR_ENC_SMPTE240M:
		m = &cce_matrix_smpte240m;
		break;
	}

	if (quantization == V4L2_QUANTIZATION_FULL_RANGE) {
		*y_offset = 0;
		return m->full;
	} else {
		*y_offset = 16;
		return m->lim;
	}
}
