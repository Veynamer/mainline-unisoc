// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unisoc ISP driver - coefficient tables
 *
 * Copyright (C) 2025 Otto Pflüger
 */

#ifndef __SPRD_ISP_COEFFS_H
#define __SPRD_ISP_COEFFS_H

#include <linux/videodev2.h>

const u16 *sprd_isp_get_cce_matrix(struct v4l2_pix_format_mplane *fmt,
				   u16 *yuv_offset);

#endif
