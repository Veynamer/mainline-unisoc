// SPDX-License-Identifier: GPL-2.0
/*
 * VBC V4 DSP command definitions
 *
 * Copyright (C) 2025 Otto Pflüger
 *
 * Based on Unisoc vendor kernel driver,
 * Copyright (C) 2018 Unisoc (Shanghai) Technologies Co., Ltd
 */

#ifndef __SPRD_VBC_V4_DSP_H
#define __SPRD_VBC_V4_DSP_H

#define VBC_NUM_IIS_CONTROLLERS	4
#define VBC_NUM_IIS_PORT_IDS	5
#define VBC_NUM_TX_IDS		3
#define VBC_NUM_TX_OUT_SEL	2
#define VBC_NUM_RX_IDS		4

enum {
	VBC_DAT_H24,
	VBC_DAT_L24,
	VBC_DAT_H16,
	VBC_DAT_L16,
};

enum {
	VBC_MIXER0_TX0,
	VBC_MIXER1_TX0,
	VBC_MIXER0_TX1,
	VBC_MIXER_ST,
	VBC_MIXER_FM,
	VBC_MIXER_NUM
};

enum {
	VBC_MUTEDG_TX0_DSP,
	VBC_MUTEDG_TX1_DSP,
	VBC_MUTEDG_AP01,
	VBC_MUTEDG_AP23,
	VBC_MUTEDG_NUM
};

enum {
	VBC_MIXERDG_TX0,
	VBC_MIXERDG_TX1,
	VBC_MIXERDG_NUM
};

enum {
	VBC_SMTHDG_TX0,
	VBC_SMTHDG_NUM
};

enum {
	VBC_IIS_WD_16BIT,
	VBC_IIS_WD_24BIT,
};

enum {
	VBC_IIS_LEFT_HIGH,
	VBC_IIS_RIGHT_HIGH,
};

struct vbc_simple_ctrl {
	u32 id;
	u32 value;
} __packed;

struct vbc_loopback_ctrl {
	u32 type;
	u32 voice_fmt;
	u32 amr_rate;
	u32 loop_mode;
} __packed;

struct vbc_iis_master_ctrl {
	u32 vbc_startup_reload;
	u32 enable;
} __packed;

struct vbc_mutedg_ctrl {
	u32 vbc_startup_reload;
	u32 id;
	u32 mute;
	u32 step;
} __packed;

struct vbc_dg_ctrl {
	u32 id;
	u32 left;
	u32 right;
} __packed;

struct vbc_smthdg_module_ctrl {
	u32 vbc_startup_reload;
	struct vbc_dg_ctrl dg;
	struct vbc_simple_ctrl step;
} __packed;

struct vbc_mixerdg_module_ctrl {
	u32 vbc_startup_reload;
	u32 id;
	struct vbc_dg_ctrl mainpath;
	struct vbc_dg_ctrl mixpath;
} __packed;

struct vbc_mixer_ctrl {
	u32 vbc_startup_reload;
	struct vbc_simple_ctrl type;
} __packed;

struct vbc_sbcenc_ctrl {
	u32 mode;
	u32 blocks;
	u32 sub_bands;
	u32 sampling_freq;
	u32 alloc_method;
	u32 min_bitpool;
	u32 max_bitpool;
} __packed;

struct vbc_ivsense_ctrl {
	u32 enable;
	u32 iv_rx_id;
} __packed;

struct vbc_voice_pcm_play_ctrl {
	u16 enable;
	u16 mode;
} __packed;

struct vbc_startup_params {
	char stream_name[32]; /* unused */
	u32 fe_id;
	u32 stream;
	u32 tx_id;
	u32 rx_id;
	u32 ref_rx_id;
	struct vbc_simple_ctrl rx_source[VBC_NUM_RX_IDS];
	struct vbc_simple_ctrl tx_out[VBC_NUM_TX_OUT_SEL];
	struct vbc_simple_ctrl mux_tx[VBC_NUM_TX_IDS];
	struct vbc_simple_ctrl mux_rx[VBC_NUM_RX_IDS];
	struct vbc_simple_ctrl iis_do[VBC_NUM_IIS_PORT_IDS];
	struct vbc_simple_ctrl tx_wd[VBC_NUM_TX_IDS];
	struct vbc_simple_ctrl tx_lr_mod[VBC_NUM_TX_IDS];
	struct vbc_simple_ctrl rx_wd[VBC_NUM_RX_IDS];
	struct vbc_simple_ctrl rx_lr_mod[VBC_NUM_RX_IDS];
	struct vbc_loopback_ctrl loopback;
	struct vbc_iis_master_ctrl iis_master;
	struct vbc_mutedg_ctrl mutedg[VBC_MUTEDG_NUM];
	struct vbc_smthdg_module_ctrl smthdg[VBC_SMTHDG_NUM];
	struct vbc_mixerdg_module_ctrl mixerdg[VBC_MIXERDG_NUM];
	u32 mixerdg_step;
	struct vbc_mixer_ctrl mixer[VBC_MIXER_NUM];
	struct vbc_sbcenc_ctrl sbcenc;
	struct vbc_ivsense_ctrl ivsense;
	struct vbc_simple_ctrl mst_sel[VBC_NUM_IIS_CONTROLLERS];
	u16 voice_record_type;
} __packed;

struct vbc_hw_params {
	char stream_name[32]; /* unused */
	u32 fe_id;
	u32 stream;
	u32 channels;
	u32 rate;
	u32 format;
} __packed;

enum {
	VBC_CTL_REG,
	VBC_CTL_MDG,
	VBC_CTL_SRC,
	VBC_CTL_DG,
	VBC_CTL_SMTHDG,
	VBC_CTL_SMTHDG_STEP,
	VBC_CTL_MIXERDG_MAIN,
	VBC_CTL_MIXERDG_MIX,
	VBC_CTL_MIXERDG_STEP,
	VBC_CTL_MIXER,
	VBC_CTL_MUX_ADC_SOURCE,
	VBC_CTL_MUX_DAC_OUT,
	VBC_CTL_MUX_ADC,
	VBC_CTL_MUX_FM,
	VBC_CTL_MUX_ST,
	VBC_CTL_MUX_LOOP_DA0,
	VBC_CTL_MUX_LOOP_DA1,
	VBC_CTL_MUX_LOOP_DA0_DA1,
	VBC_CTL_MUX_AUDRCD,
	VBC_CTL_MUX_TDM_AUDRCD23,
	VBC_CTL_MUX_AP01_DSP,
	VBC_CTL_MUX_IIS_TX,
	VBC_CTL_MUX_IIS_RX,
	VBC_CTL_IIS_PORT_DO,
	VBC_CTL_VOLUME,
	VBC_CTL_ADDER,
	VBC_CTL_LOOPBACK_TYPE,
	VBC_CTL_DATAPATH,
	VBC_CTL_CALL_MUTE,
	VBC_CTL_IIS_TX_WIDTH_SEL,
	VBC_CTL_IIS_TX_LRMOD_SEL,
	VBC_CTL_IIS_RX_WIDTH_SEL,
	VBC_CTL_IIS_RX_LRMOD_SEL,
	VBC_CTL_IIS_MASTER_START,
	VBC_CTL_SBCPARA_SET,
	VBC_CTL_MAIN_MIC_PATH_FROM,
	VBC_CTL_IVSENSE_FUNC,
	VBC_CTL_EXT_INNER_IIS_MST_SEL,
	VBC_CTL_IIS_MASTER_WIDTH_SET,
	VBC_CTL_VOICE_MIX_UL,
	VBC_CTL_FM_MUTE_EN,
	VBC_CTL_FM_MUTE,
	VBC_CTL_FM_MDG_STEP,
	VBC_CTL_AUX_MIC2_SEL,
	VBC_CTL_SMARTAMP_IV_EXCHANGE,
	VBC_CTL_HP_CROSSTALK_EN,
	VBC_CTL_HP_CROSSTALK_GAIN,
};

enum {
	VBC_DSP_FUNC_STARTUP = 4,
	VBC_DSP_FUNC_SHUTDOWN,
	VBC_DSP_FUNC_HW_PARAMS,
	VBC_DSP_FUNC_HW_TRIGGER,
	VBC_DSP_IO_KCTL_GET,
	VBC_DSP_IO_KCTL_SET,
	VBC_DSP_IO_SHAREMEM_GET,
	VBC_DSP_IO_SHAREMEM_SET,
};

enum {
	VBC_FE_NORMAL_AP01,
	VBC_FE_NORMAL_AP23,
	VBC_FE_CAPTURE_DSP,
	VBC_FE_FAST_PLAYBACK,
	VBC_FE_OFFLOAD,
	VBC_FE_VOICE,
	VBC_FE_VOIP,
	VBC_FE_FM,
	VBC_FE_LOOP,
	VBC_FE_PCM_A2DP,
	VBC_FE_OFFLOAD_A2DP,
	VBC_FE_BT_CAPTURE_AP,
	VBC_FE_FM_CAPTURE_AP,
	VBC_FE_VOICE_CAPTURE,
	VBC_FE_FM_CAPTURE_DSP,
	VBC_FE_BT_SCO_CAPTURE_DSP,
	VBC_FE_FM_DSP,
	VBC_FE_VOICE_PLAYBACK,
	VBC_FE_HFP,
	VBC_FE_RECOGNISE_CAPTURE,
	VBC_FE_INVALID,
};

enum {
	VBC_PROFILE_AUDIO_STRUCTURE,
	VBC_PROFILE_DSP,
	VBC_PROFILE_NXP,
	VBC_PROFILE_IVS_SMARTPA,
	VBC_SHM_VBC_REG,
};

#endif
