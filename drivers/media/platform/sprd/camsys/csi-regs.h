/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Unisoc CSI receiver register definitions
 * 
 * Copyright (C) 2025 Otto Pflüger
 * Copyright (C) 2021-2022 UNISOC Communications Inc.
 */

#ifndef __SPRD_CSI_REGS_H
#define __SPRD_CSI_REGS_H

/* CSI module */
#define CSI_IP_REVISION					0x0000
#define CSI_LANE_NUMBER					0x0004
#define CSI_PHY_PD_N					0x0008
#define CSI_RST_DPHY_N					0x000C
#define CSI_RST_CSI2_N					0x0010
#define CSI_IPG_MODE_CFG				0x0014

#define CSI_PHY_STATE					0x0018
#define CSI_ERR0					0x0020
#define CSI_ERR1					0x0024
#define CSI_MASK0					0x0028
#define CSI_MASK1					0x002C
#define CSI_ERR0_CLR					0x0030
#define CSI_ERR1_CLR					0x0034
#define CSI_CAL_DONE					0x0038
#define CSI_CAL_FAILED					0x003C
#define CSI_MSK_CAL_DONE				0x0040
#define CSI_MSK_CAL_FAILED				0x0044

#define CSI_PHY_TEST_CTRL0				0x0048
#define CSI_PHY_REG_SEL					BIT(2)
#define CSI_PHY_TESTCLK					BIT(1)
#define CSI_PHY_TESTCLR					BIT(0)

#define CSI_PHY_TEST_CTRL1				0x004C
#define CSI_PHY_TESTEN					BIT(16)

#define CSI_IPG_RAW10_CFG0				0x0050
#define CSI_IPG_RAW10_CFG1				0x0054
#define CSI_IPG_RAW10_CFG2				0x0058
#define CSI_IPG_RAW10_CFG3				0x005C
#define CSI_IPG_YUV422_8_CFG0				0x0060
#define CSI_IPG_YUV422_8_CFG4				0x0064
#define CSI_IPG_YUV422_8_CFG8				0x0068
#define CSI_IPG_OTHER_CFG0				0x006C

#define CSI_PHY_SEL					0x0070
#define CSI_CPHY_ERR2					0x0074
#define CSI_CPHY_ERR2_MASK				0x0078
#define CSI_CPHY_ERR2_CLR				0x007C
#define CSI_PHY_TRANS_FLAG				0x0080
#define CSI_PHY_PH_ERR_NUM				0x0084
#define CSI_PHY_DEBUG_EN				0x0088
#define CSI_PHY_LINE_START_DELAY			0x008C
#define CSI_PHY_IPG_CFG_ADD				0x0090

#endif
