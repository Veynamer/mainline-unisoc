// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Otto Pflüger
 */

#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/soc/sprd/agdsp.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "sprd-mcdt.h"
#include "sprd-pcm-dma.h"
#include "vbc-v4-dsp.h"

enum {
	/* frontend DAI IDs are defined by DSP (see vbc-v4-dsp.h) */

	/* backend DAI IDs are arbitrary but should not conflict with FE IDs */
	VBC_IIS0 = VBC_FE_INVALID,
	VBC_IIS1,
	VBC_IIS2,
	VBC_IIS3,
};

struct sprd_vbc_priv {
	struct device *dev;
	struct sprd_agdsp_ipc *ipc;
	struct sprd_mcdt_chan *mcdt_voice_capture;
	struct sprd_mcdt_chan *mcdt_playback;
	struct sprd_mcdt_chan *mcdt_capture;
	struct vbc_startup_params params;
	u32 iis_lrmod[VBC_NUM_IIS_PORT_IDS];
	u32 iis_rx[VBC_NUM_IIS_PORT_IDS];
	u32 current_fe_tx[VBC_NUM_TX_IDS];
	u32 current_fe_rx[VBC_NUM_RX_IDS];
	u8 voice_mute;
	u8 voice_started;
};

static int vbc_cmd_set_ctrl(struct sprd_vbc_priv *vbc, u32 ctl, u32 id,
			    u32 value)
{
	struct vbc_simple_ctrl params = { .id = id, .value = value };

	return sprd_agdsp_send_cmd(vbc->ipc, AGDSP_CH_VBC_CTL,
				   VBC_DSP_IO_KCTL_SET, ctl, -1,
				   &params, sizeof(params));
}

static u32 vbc_get_fe_tx_id(u32 dai_id)
{
	switch (dai_id) {
	case VBC_FE_VOICE:
		return 1;
	default:
		return 0;
	}
}

static u32 vbc_get_fe_rx_id(u32 dai_id)
{
	switch (dai_id) {
	case VBC_FE_VOICE:
		return 2;
	default:
		return 0;
	}
}

static const char * const vbc_tx_enum_texts[] = {
	"TX0", "TX1", "TX2"
};

static const char * const vbc_rx_enum_texts[] = {
	"RX0", "RX1", "RX2", "RX3"
};

static const char * const vbc_iis_enum_texts[] = {
	"IIS0", "IIS1", "IIS2", "IIS3"
};

#define DECLARE_VBC_TX_CONTROL(id)					\
	static SOC_ENUM_SINGLE_DECL(vbc_tx##id##_demux_enum,		\
				    SND_SOC_NOPM, id,			\
				    vbc_iis_enum_texts);		\
	static const struct snd_kcontrol_new vbc_tx##id##_demux =	\
		SOC_DAPM_ENUM_EXT("TX" #id " Port Select",		\
				  vbc_tx##id##_demux_enum,		\
				  vbc_tx_mux_get,			\
				  vbc_tx_mux_put)

#define DECLARE_VBC_RX_CONTROL(id) 					\
	static SOC_ENUM_SINGLE_DECL(vbc_rx##id##_mux_enum,		\
				    SND_SOC_NOPM, id,			\
				    vbc_iis_enum_texts);		\
	static const struct snd_kcontrol_new vbc_rx##id##_mux =		\
		SOC_DAPM_ENUM_EXT("RX" #id " Port Select",		\
				  vbc_rx##id##_mux_enum,		\
				  vbc_rx_mux_get,			\
				  vbc_rx_mux_put)

#define VBC_IIS_TX_ROUTES(tx_name) \
	{ "VBC_IIS0 Playback", "IIS0", tx_name " SEL" },	\
	{ "VBC_IIS1 Playback", "IIS1", tx_name " SEL" },	\
	{ "VBC_IIS2 Playback", "IIS2", tx_name " SEL" },	\
	{ "VBC_IIS3 Playback", "IIS3", tx_name " SEL" }

#define VBC_IIS_RX_ROUTES(rx_name) \
	{ rx_name " SEL", "IIS0", "VBC_IIS0 Capture" },	\
	{ rx_name " SEL", "IIS1", "VBC_IIS1 Capture" },	\
	{ rx_name " SEL", "IIS2", "VBC_IIS2 Capture" },	\
	{ rx_name " SEL", "IIS3", "VBC_IIS3 Capture" }

#define SPRD_PCM_RATES		(SNDRV_PCM_RATE_8000_48000 |	\
				 SNDRV_PCM_RATE_12000 |		\
				 SNDRV_PCM_RATE_24000 |		\
				 SNDRV_PCM_RATE_96000 |		\
				 SNDRV_PCM_RATE_192000)

#define SPRD_BE_DAI(_id)					\
	{							\
		.name = #_id,					\
		.id = (_id),					\
		.playback = {					\
			.stream_name = #_id " Playback",	\
			.channels_min = 1,			\
			.channels_max = 2,			\
			.rates = SPRD_PCM_RATES,		\
			.formats = SNDRV_PCM_FMTBIT_S16_LE,	\
		},						\
		.capture = {					\
			.stream_name = #_id " Capture",		\
			.channels_min = 1,			\
			.channels_max = 2,			\
			.rates = SPRD_PCM_RATES,		\
			.formats = SNDRV_PCM_FMTBIT_S16_LE,	\
		},						\
		.ops = &sprd_be_dai_ops,			\
	}

static int vbc_tx_mux_get(struct snd_kcontrol *kcontrol,
			  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_context *dapm =
		snd_soc_dapm_kcontrol_dapm(kcontrol);
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	struct snd_soc_component *c = snd_soc_dapm_to_component(dapm);
	struct sprd_vbc_priv *vbc = dev_get_drvdata(c->dev);

	ucontrol->value.integer.value[0] = vbc->params.mux_tx[e->shift_l].value;

	return 0;
}

static int vbc_tx_mux_put(struct snd_kcontrol *kcontrol,
			  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_context *dapm =
		snd_soc_dapm_kcontrol_dapm(kcontrol);
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	struct snd_soc_component *c = snd_soc_dapm_to_component(dapm);
	struct sprd_vbc_priv *vbc = dev_get_drvdata(c->dev);
	unsigned int value = ucontrol->value.integer.value[0];
	int i, ret;

	if (value > VBC_NUM_IIS_PORT_IDS || e->shift_l > VBC_NUM_TX_IDS)
		return -EINVAL;

	/* Return -EBUSY if the output port is in use by another FE */
	for (i = 0; i < VBC_NUM_TX_IDS; i++) {
		if (i != e->shift_l &&
		    vbc->current_fe_tx[i] != VBC_FE_INVALID &&
		    vbc->params.mux_tx[i].value ==
		    vbc->params.mux_tx[e->shift_l].value)
			return -EBUSY;
	}

	vbc->params.mux_tx[e->shift_l].value = value;
	vbc->params.iis_do[value].value = e->shift_l;

	snd_soc_dapm_mux_update_power(dapm, kcontrol, value, e, NULL);

	ret = vbc_cmd_set_ctrl(vbc, VBC_CTL_MUX_IIS_TX, e->shift_l, value);
	if (ret)
		return ret;

	return vbc_cmd_set_ctrl(vbc, VBC_CTL_IIS_PORT_DO, value, e->shift_l);
}

static int vbc_rx_mux_get(struct snd_kcontrol *kcontrol,
			  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_context *dapm =
		snd_soc_dapm_kcontrol_dapm(kcontrol);
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	struct snd_soc_component *c = snd_soc_dapm_to_component(dapm);
	struct sprd_vbc_priv *vbc = dev_get_drvdata(c->dev);

	ucontrol->value.integer.value[0] = vbc->params.mux_rx[e->shift_l].value;

	return 0;
}

static int vbc_rx_mux_put(struct snd_kcontrol *kcontrol,
			  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_context *dapm =
		snd_soc_dapm_kcontrol_dapm(kcontrol);
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	struct snd_soc_component *c = snd_soc_dapm_to_component(dapm);
	struct sprd_vbc_priv *vbc = dev_get_drvdata(c->dev);
	unsigned int value = ucontrol->value.integer.value[0];
	int i;

	if (value > VBC_NUM_IIS_PORT_IDS || e->shift_l > VBC_NUM_RX_IDS)
		return -EINVAL;

	/* Return -EBUSY if the input port is in use by another FE */
	for (i = 0; i < VBC_NUM_RX_IDS; i++) {
		if (i != e->shift_l &&
		    vbc->current_fe_rx[i] != VBC_FE_INVALID &&
		    vbc->params.mux_rx[i].value ==
		    vbc->params.mux_rx[e->shift_l].value)
			return -EBUSY;
	}

	vbc->params.mux_rx[e->shift_l].value = value;
	vbc->iis_rx[value] = e->shift_l;

	snd_soc_dapm_mux_update_power(dapm, kcontrol, value, e, NULL);

	return vbc_cmd_set_ctrl(vbc, VBC_CTL_MUX_IIS_RX, e->shift_l, value);
}

static int vbc_voice_mute_get(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct sprd_vbc_priv *vbc = dev_get_drvdata(c->dev);

	ucontrol->value.integer.value[0] = (vbc->voice_mute >> mc->shift) & 1;

	return 0;
}

static int vbc_voice_mute_put(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct sprd_vbc_priv *vbc = dev_get_drvdata(c->dev);
	bool value = ucontrol->value.integer.value[0];

	vbc->voice_mute &= ~(1 << mc->shift);
	vbc->voice_mute |= value << mc->shift;

	return vbc_cmd_set_ctrl(vbc, VBC_CTL_CALL_MUTE, mc->shift, value);
}

static int vbc_voice_record_type_get(struct snd_kcontrol *kcontrol,
				     struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct sprd_vbc_priv *vbc = dev_get_drvdata(c->dev);

	ucontrol->value.integer.value[0] =
		(vbc->params.voice_record_type >> mc->shift) & 1;

	return 0;
}

static int vbc_voice_record_type_put(struct snd_kcontrol *kcontrol,
				     struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct sprd_vbc_priv *vbc = dev_get_drvdata(c->dev);
	bool value = ucontrol->value.integer.value[0];

	vbc->params.voice_record_type &= ~(1 << mc->shift);
	vbc->params.voice_record_type |= value << mc->shift;

	return 0;
}

static int vbc_fe_tx_switch_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_context *dapm =
		snd_soc_dapm_kcontrol_dapm(kcontrol);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct snd_soc_component *c = snd_soc_dapm_to_component(dapm);
	struct sprd_vbc_priv *vbc = dev_get_drvdata(c->dev);
	u32 tx_id = vbc_get_fe_tx_id(mc->shift);

	ucontrol->value.integer.value[0] =
		vbc->current_fe_tx[tx_id] == mc->shift;

	return 0;
}

static int vbc_fe_tx_switch_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_context *dapm =
		snd_soc_dapm_kcontrol_dapm(kcontrol);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct snd_soc_component *c = snd_soc_dapm_to_component(dapm);
	struct sprd_vbc_priv *vbc = dev_get_drvdata(c->dev);
	u32 tx_id = vbc_get_fe_tx_id(mc->shift);

	if (ucontrol->value.integer.value[0]) {
		int i = 0;

		/* Do nothing if already turned on */
		if (vbc->current_fe_tx[tx_id] == mc->shift)
			return 0;

		/*
		 * Return -EBUSY if any active FE is connected to the same
		 * output port.
		 */
		for (i = 0; i < VBC_NUM_TX_IDS; i++) {
			if (vbc->current_fe_tx[i] != VBC_FE_INVALID &&
			    vbc->params.mux_tx[i].value ==
			    vbc->params.mux_tx[tx_id].value)
				return -EBUSY;
		}

		vbc->current_fe_tx[tx_id] = mc->shift;

		snd_soc_dapm_mixer_update_power(dapm, kcontrol, 1, NULL);
	} else {
		if (vbc->current_fe_tx[tx_id] == mc->shift) {
			vbc->current_fe_tx[tx_id] = VBC_FE_INVALID;
			snd_soc_dapm_mixer_update_power(dapm, kcontrol,
							0, NULL);
		}
	}

	return 0;
}

static int vbc_fe_rx_switch_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_context *dapm =
		snd_soc_dapm_kcontrol_dapm(kcontrol);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct snd_soc_component *c = snd_soc_dapm_to_component(dapm);
	struct sprd_vbc_priv *vbc = dev_get_drvdata(c->dev);
	u32 rx_id = vbc_get_fe_rx_id(mc->shift);

	ucontrol->value.integer.value[0] =
		vbc->current_fe_rx[rx_id] == mc->shift;

	return 0;
}

static int vbc_fe_rx_switch_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_context *dapm =
		snd_soc_dapm_kcontrol_dapm(kcontrol);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct snd_soc_component *c = snd_soc_dapm_to_component(dapm);
	struct sprd_vbc_priv *vbc = dev_get_drvdata(c->dev);
	u32 rx_id = vbc_get_fe_rx_id(mc->shift);

	if (ucontrol->value.integer.value[0]) {
		int i = 0;

		/* Do nothing if already turned on */
		if (vbc->current_fe_rx[rx_id] == mc->shift)
			return 0;

		/*
		 * Return -EBUSY if any active FE is connected to the same
		 * input port.
		 */
		for (i = 0; i < VBC_NUM_RX_IDS; i++) {
			if (vbc->current_fe_rx[i] != VBC_FE_INVALID &&
			    vbc->params.mux_rx[i].value ==
			    vbc->params.mux_rx[rx_id].value)
				return -EBUSY;
		}

		vbc->current_fe_rx[rx_id] = mc->shift;

		snd_soc_dapm_mixer_update_power(dapm, kcontrol, 1, NULL);
	} else {
		if (vbc->current_fe_rx[rx_id] == mc->shift) {
			vbc->current_fe_rx[rx_id] = VBC_FE_INVALID;
			snd_soc_dapm_mixer_update_power(dapm, kcontrol,
							0, NULL);
		}
	}

	return 0;
}

static const char *vbc_mixer_mode_texts[] = {
	"NOT_MIX", "INTERCHANGE", "HALF_ADD", "HALF_SUB",
	"DATA_INV", "INTERCHANGE_INV", "HALF_ADD_INV", "HALF_SUB_INV",
};

static SOC_ENUM_SINGLE_DECL(vbc_mixer0_tx0_enum, SND_SOC_NOPM,
			    VBC_MIXER0_TX0, vbc_mixer_mode_texts);
static SOC_ENUM_SINGLE_DECL(vbc_mixer1_tx0_enum, SND_SOC_NOPM,
			    VBC_MIXER1_TX0, vbc_mixer_mode_texts);
static SOC_ENUM_SINGLE_DECL(vbc_mixer0_tx1_enum, SND_SOC_NOPM,
			    VBC_MIXER0_TX1, vbc_mixer_mode_texts);
static SOC_ENUM_SINGLE_DECL(vbc_mixer_st_enum, SND_SOC_NOPM,
			    VBC_MIXER_ST, vbc_mixer_mode_texts);
static SOC_ENUM_SINGLE_DECL(vbc_mixer_fm_enum, SND_SOC_NOPM,
			    VBC_MIXER_FM, vbc_mixer_mode_texts);

static int vbc_mixer_mode_get(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct sprd_vbc_priv *vbc = dev_get_drvdata(c->dev);

	ucontrol->value.integer.value[0] =
		vbc->params.mixer[e->shift_l].type.value;

	return 0;
}

static int vbc_mixer_mode_put(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct sprd_vbc_priv *vbc = dev_get_drvdata(c->dev);

	vbc->params.mixer[e->shift_l].type.value =
		ucontrol->value.integer.value[0];

	return sprd_agdsp_send_cmd(vbc->ipc, AGDSP_CH_VBC_CTL,
				   VBC_DSP_IO_KCTL_SET, VBC_CTL_MIXER, -1,
				   &vbc->params.mixer[e->shift_l],
				   sizeof(struct vbc_mixer_ctrl));
}

static const struct snd_kcontrol_new sprd_vbc_controls[] = {
	SOC_SINGLE_EXT("Voice Mute Uplink", SND_SOC_NOPM, 0, 1, 0,
		       vbc_voice_mute_get,
		       vbc_voice_mute_put),
	SOC_SINGLE_EXT("Voice Mute Downlink", SND_SOC_NOPM, 1, 1, 0,
		       vbc_voice_mute_get,
		       vbc_voice_mute_put),

	SOC_SINGLE_EXT("Voice Record Uplink", SND_SOC_NOPM, 1, 1, 0,
		       vbc_voice_record_type_get,
		       vbc_voice_record_type_put),
	SOC_SINGLE_EXT("Voice Record Downlink", SND_SOC_NOPM, 0, 1, 0,
		       vbc_voice_record_type_get,
		       vbc_voice_record_type_put),

	SOC_ENUM_EXT("TX0 MIXER0 Mode", vbc_mixer0_tx0_enum,
		     vbc_mixer_mode_get, vbc_mixer_mode_put),
	SOC_ENUM_EXT("TX0 MIXER1 Mode", vbc_mixer1_tx0_enum,
		     vbc_mixer_mode_get, vbc_mixer_mode_put),
	SOC_ENUM_EXT("TX1 MIXER0 Mode", vbc_mixer0_tx1_enum,
		     vbc_mixer_mode_get, vbc_mixer_mode_put),
};

DECLARE_VBC_TX_CONTROL(0);
DECLARE_VBC_TX_CONTROL(1);
DECLARE_VBC_TX_CONTROL(2);
DECLARE_VBC_RX_CONTROL(0);
DECLARE_VBC_RX_CONTROL(1);
DECLARE_VBC_RX_CONTROL(2);
DECLARE_VBC_RX_CONTROL(3);

static const struct snd_kcontrol_new vbc_fe_fast_playback_switch =
	SOC_SINGLE_EXT("Switch", SND_SOC_NOPM, VBC_FE_FAST_PLAYBACK, 1, 0,
		       vbc_fe_tx_switch_get, vbc_fe_tx_switch_put);

static const struct snd_kcontrol_new vbc_fe_capture_dsp_switch =
	SOC_SINGLE_EXT("Switch", SND_SOC_NOPM, VBC_FE_CAPTURE_DSP, 1, 0,
		       vbc_fe_rx_switch_get, vbc_fe_rx_switch_put);

static const struct snd_kcontrol_new vbc_fe_voice_tx_switch =
	SOC_SINGLE_EXT("Switch", SND_SOC_NOPM, VBC_FE_VOICE, 1, 0,
		       vbc_fe_tx_switch_get, vbc_fe_tx_switch_put);

static const struct snd_kcontrol_new vbc_fe_voice_rx_switch =
	SOC_SINGLE_EXT("Switch", SND_SOC_NOPM, VBC_FE_VOICE, 1, 0,
		       vbc_fe_rx_switch_get, vbc_fe_rx_switch_put);

static const struct snd_soc_dapm_widget sprd_vbc_widgets[] = {
	/* Backend routing */
	SND_SOC_DAPM_DEMUX("TX0 SEL", SND_SOC_NOPM, 0, 0, &vbc_tx0_demux),
	SND_SOC_DAPM_DEMUX("TX1 SEL", SND_SOC_NOPM, 0, 0, &vbc_tx1_demux),
	SND_SOC_DAPM_DEMUX("TX2 SEL", SND_SOC_NOPM, 0, 0, &vbc_tx2_demux),
	SND_SOC_DAPM_MUX("RX0 SEL", SND_SOC_NOPM, 0, 0, &vbc_rx0_mux),
	SND_SOC_DAPM_MUX("RX1 SEL", SND_SOC_NOPM, 0, 0, &vbc_rx1_mux),
	SND_SOC_DAPM_MUX("RX2 SEL", SND_SOC_NOPM, 0, 0, &vbc_rx2_mux),
	SND_SOC_DAPM_MUX("RX3 SEL", SND_SOC_NOPM, 0, 0, &vbc_rx3_mux),

	/* Frontend switches */
	SND_SOC_DAPM_SWITCH("FE_FAST_PLAYBACK TX", SND_SOC_NOPM, 0, 0,
			    &vbc_fe_fast_playback_switch),
	SND_SOC_DAPM_SWITCH("FE_CAPTURE_DSP RX", SND_SOC_NOPM, 0, 0,
			    &vbc_fe_capture_dsp_switch),
	SND_SOC_DAPM_SWITCH("FE_VOICE TX", SND_SOC_NOPM, 0, 0,
			    &vbc_fe_voice_tx_switch),
	SND_SOC_DAPM_SWITCH("FE_VOICE RX", SND_SOC_NOPM, 0, 0,
			    &vbc_fe_voice_rx_switch),
};

static const struct snd_soc_dapm_route sprd_vbc_routes[] = {
	VBC_IIS_TX_ROUTES("TX0"),
	VBC_IIS_TX_ROUTES("TX1"),
	VBC_IIS_TX_ROUTES("TX2"),

	VBC_IIS_RX_ROUTES("RX0"),
	VBC_IIS_RX_ROUTES("RX1"),
	VBC_IIS_RX_ROUTES("RX2"),
	VBC_IIS_RX_ROUTES("RX3"),

	{ "FE_FAST_PLAYBACK TX", "Switch", "VBC_FE_FAST_PLAYBACK Playback" },
	{ "FE_VOICE TX", "Switch", "VBC_FE_VOICE Playback" },

	{ "TX0 SEL", NULL, "FE_FAST_PLAYBACK TX" },
	{ "TX1 SEL", NULL, "FE_VOICE TX" },

	{ "FE_CAPTURE_DSP RX", "Switch", "RX0 SEL" },
	{ "FE_VOICE RX", "Switch", "RX2 SEL" },

	{ "VBC_FE_CAPTURE_DSP Capture", NULL, "FE_CAPTURE_DSP RX" },
	{ "VBC_FE_VOICE Capture", NULL, "FE_VOICE RX" },
};

static int sprd_vbc_fe_startup(struct snd_pcm_substream *substream,
			       struct snd_soc_dai *fe_dai)
{
	struct sprd_vbc_priv *vbc = dev_get_drvdata(fe_dai->dev);
	int ret;

	vbc->params.fe_id = fe_dai->id;
	vbc->params.stream = substream->stream;
	vbc->params.tx_id = vbc_get_fe_tx_id(fe_dai->id);
	vbc->params.rx_id = vbc_get_fe_rx_id(fe_dai->id);

	ret = sprd_agdsp_send_cmd(vbc->ipc, AGDSP_CH_VBC_CTL,
				  VBC_DSP_FUNC_STARTUP, fe_dai->id,
				  substream->stream, &vbc->params,
				  sizeof(vbc->params));
	if (ret)
		return ret;

	/*
	 * The voice stream needs to be triggered after startup() has
	 * been called for both directions. Otherwise, voice calls do
	 * not work. When FE_VOICE is used as a codec-to-codec DAI, the
	 * ASoC core does not call trigger() for it.
	 */
	if (fe_dai->id == VBC_FE_VOICE) {
		vbc->voice_started |= 1 << substream->stream;

		if (vbc->voice_started == 3) {
			ret = sprd_agdsp_send_msg(vbc->ipc, AGDSP_CH_VBC_CTL,
						  VBC_DSP_FUNC_HW_TRIGGER,
						  VBC_FE_VOICE, 0, 1, 0);
			if (ret)
				return ret;

			ret = sprd_agdsp_send_msg(vbc->ipc, AGDSP_CH_VBC_CTL,
						  VBC_DSP_FUNC_HW_TRIGGER,
						  VBC_FE_VOICE, 1, 1, 0);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static void sprd_vbc_fe_shutdown(struct snd_pcm_substream *substream,
				 struct snd_soc_dai *fe_dai)
{
	struct sprd_vbc_priv *vbc = dev_get_drvdata(fe_dai->dev);
	int ret;

	vbc->params.fe_id = fe_dai->id;
	vbc->params.stream = substream->stream;
	vbc->params.tx_id = 0;
	vbc->params.rx_id = 0;

	if (fe_dai->id == VBC_FE_VOICE)
		vbc->voice_started &= ~(1 << substream->stream);

	ret = sprd_agdsp_send_cmd(vbc->ipc, AGDSP_CH_VBC_CTL,
				  VBC_DSP_FUNC_SHUTDOWN, fe_dai->id,
				  substream->stream, &vbc->params,
				  sizeof(vbc->params));
	if (ret < 0)
		dev_err(fe_dai->dev, "shutdown command failed: %d\n", ret);
}

static int sprd_vbc_fe_trigger(struct snd_pcm_substream *substream, int cmd,
			       struct snd_soc_dai *fe_dai)
{
	struct sprd_vbc_priv *vbc = dev_get_drvdata(fe_dai->dev);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		return sprd_agdsp_send_msg(vbc->ipc, AGDSP_CH_VBC_CTL,
					   VBC_DSP_FUNC_HW_TRIGGER,
					   fe_dai->id, substream->stream,
					   1, 0);

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	default:
		return 0;
	}
}

static struct sprd_mcdt_chan *sprd_vbc_get_mcdt(
					struct sprd_vbc_priv *vbc,
					int dai_id,
					enum sprd_mcdt_dma_chan *mcdt_dma,
					u32 *mcdt_dma_watermark,
					const char **mcdt_dma_name)
{
	struct sprd_mcdt_chan *chan;
	enum sprd_mcdt_dma_chan dma;
	const char *name;
	u32 watermark;

	switch (dai_id) {
	case VBC_FE_VOICE_CAPTURE:
		dma = SPRD_MCDT_DMA_CH2;
		watermark = 160;
		name = "voice_c";
		chan = vbc->mcdt_voice_capture;
		break;
	case VBC_FE_FAST_PLAYBACK:
		dma = SPRD_MCDT_DMA_CH4;
		watermark = 320;
		name = "fast_p";
		chan = vbc->mcdt_playback;
		break;
	case VBC_FE_CAPTURE_DSP:
		dma = SPRD_MCDT_DMA_CH4;
		watermark = 320;
		name = "dspcap_c";
		chan = vbc->mcdt_capture;
		break;
	case VBC_FE_VOICE:
		/* MCDT channel is controlled by modem */
		return NULL;
	default:
		dev_err(vbc->dev, "unexpected FE DAI ID %d\n", dai_id);
		return ERR_PTR(-EINVAL);
	}

	if (mcdt_dma)
		*mcdt_dma = dma;
	if (mcdt_dma_watermark)
		*mcdt_dma_watermark = watermark;
	if (mcdt_dma_name)
		*mcdt_dma_name = name;

	return chan;
}

static int sprd_vbc_fe_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params,
				 struct snd_soc_dai *fe_dai)
{
	struct sprd_vbc_priv *vbc = dev_get_drvdata(fe_dai->dev);
	struct sprd_mcdt_chan *mcdt_chan = NULL;
	enum sprd_mcdt_dma_chan mcdt_dma;
	struct sprd_pcm_dma_params *dma_data;
	int channels = params_channels(params);
	int rate = params_rate(params);
	struct vbc_hw_params hw_params = {
		.fe_id = fe_dai->id,
		.stream = substream->stream,
		.channels = channels,
	};
	u32 watermark;
	int ret;

	dma_data = kzalloc(sizeof(*dma_data), GFP_KERNEL);
	if (!dma_data)
		return -ENOMEM;

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		hw_params.format = VBC_DAT_L16;
		break;
	default:
		dev_err(fe_dai->dev, "unsupported data format\n");
		kfree(dma_data);
		return -EINVAL;
	}

	switch (params_rate(params)) {
	case 48000:
		hw_params.rate = 0;
		break;
	case 44100:
		hw_params.rate = 1;
		break;
	case 32000:
		hw_params.rate = 2;
		break;
	case 24000:
		hw_params.rate = 3;
		break;
	case 22050:
		hw_params.rate = 4;
		break;
	case 16000:
		hw_params.rate = 5;
		break;
	case 12000:
		hw_params.rate = 6;
		break;
	case 11025:
		hw_params.rate = 7;
		break;
	case 9600:
		hw_params.rate = 8;
		break;
	case 8000:
		hw_params.rate = 9;
		break;
	case 96000:
		hw_params.rate = 10;
		break;
	case 192000:
		hw_params.rate = 11;
		break;
	default:
		dev_err(fe_dai->dev, "unsupported rate: %d\n", 
			params_rate(params));
		kfree(dma_data);
		return -EINVAL;
	}

	mcdt_chan = sprd_vbc_get_mcdt(vbc, fe_dai->id, &mcdt_dma, &watermark,
				      &dma_data->chan_name[0]);
	if (IS_ERR(mcdt_chan)) {
		kfree(dma_data);
		return PTR_ERR(mcdt_chan);
	}

	if (mcdt_chan) {
		sprd_mcdt_chan_dma_enable(mcdt_chan, mcdt_dma, watermark);
		dma_data->datawidth[0] = DMA_SLAVE_BUSWIDTH_4_BYTES;
		dma_data->dev_phys[0] = mcdt_chan->fifo_phys;
		dma_data->fragment_len[0] = 80;
		dma_data->hw_channels = 1;
		snd_soc_dai_set_dma_data(fe_dai, substream, dma_data);
	} else {
		kfree(dma_data);
	}

	ret = sprd_agdsp_send_cmd(vbc->ipc, AGDSP_CH_VBC_CTL,
				  VBC_DSP_FUNC_HW_PARAMS, fe_dai->id,
				  substream->stream, &hw_params,
				  sizeof(hw_params));
	if (ret)
		return ret;

	/*
	 * The DSP sometimes seems to ignore hw_params for playback.
	 * To make sure that the correct rate is used, set it using a
	 * different method.
	 */
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		ret = vbc_cmd_set_ctrl(vbc, VBC_CTL_SRC,
				       fe_dai->id == VBC_FE_VOICE ? 1 : 0,
				       rate);
		if (ret)
			return ret;
	}

	return 0;
}

static int sprd_vbc_fe_hw_free(struct snd_pcm_substream *substream,
			       struct snd_soc_dai *fe_dai)
{
	struct sprd_vbc_priv *vbc = dev_get_drvdata(fe_dai->dev);
	struct sprd_pcm_dma_params *dma_data =
		snd_soc_dai_get_dma_data(fe_dai, substream);
	struct sprd_mcdt_chan *mcdt_chan;

	if (!dma_data)
		return 0;

	mcdt_chan = sprd_vbc_get_mcdt(vbc, fe_dai->id, NULL, NULL, NULL);
	if (mcdt_chan && !IS_ERR(mcdt_chan))
		sprd_mcdt_chan_dma_disable(mcdt_chan);
	snd_soc_dai_set_dma_data(fe_dai, substream, NULL);
	kfree(dma_data);

	return 0;
}

static const struct snd_soc_dai_ops sprd_fe_dai_ops = {
	.startup = sprd_vbc_fe_startup,
	.shutdown = sprd_vbc_fe_shutdown,
	.trigger = sprd_vbc_fe_trigger,
	.hw_params = sprd_vbc_fe_hw_params,
	.hw_free = sprd_vbc_fe_hw_free,
};

static int sprd_vbc_be_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params,
				 struct snd_soc_dai *be_dai)
{
	bool is_playback = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
	struct sprd_vbc_priv *vbc = dev_get_drvdata(be_dai->dev);
	struct vbc_simple_ctrl *width_params, *lrmod_params;
	u32 idx, width_ctl, lrmod_ctl, width;
	int ret;

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		width = VBC_IIS_WD_16BIT;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		width = VBC_IIS_WD_24BIT;
		break;
	default:
		dev_err(be_dai->dev, "unsupported backend data format\n");
		return -EINVAL;
	}

	if (be_dai->id < VBC_IIS0) {
		dev_err(be_dai->dev, "unrecognized backend DAI %d\n",
			be_dai->id);
		return -EINVAL;
	}

	if (is_playback) {
		idx = vbc->params.iis_do[be_dai->id - VBC_IIS0].value;
		width_ctl = VBC_CTL_IIS_TX_WIDTH_SEL;
		width_params = &vbc->params.tx_wd[idx];
		lrmod_ctl = VBC_CTL_IIS_TX_LRMOD_SEL;
		lrmod_params = &vbc->params.tx_lr_mod[idx];
	} else {
		idx = vbc->iis_rx[be_dai->id - VBC_IIS0];
		width_ctl = VBC_CTL_IIS_RX_WIDTH_SEL;
		width_params = &vbc->params.rx_wd[idx];
		lrmod_ctl = VBC_CTL_IIS_RX_LRMOD_SEL;
		lrmod_params = &vbc->params.rx_lr_mod[idx];
	}

	width_params->value = width;
	lrmod_params->value = vbc->iis_lrmod[be_dai->id - VBC_IIS0];

	ret = sprd_agdsp_send_cmd(vbc->ipc, AGDSP_CH_VBC_CTL,
				  VBC_DSP_IO_KCTL_SET, width_ctl, -1,
				  width_params, sizeof(*width_params));
	if (ret)
		return ret;

	ret = sprd_agdsp_send_cmd(vbc->ipc, AGDSP_CH_VBC_CTL,
				  VBC_DSP_IO_KCTL_SET, lrmod_ctl, -1,
				  lrmod_params, sizeof(*lrmod_params));
	if (ret)
		return ret;

	return 0;
}

static int sprd_vbc_be_set_fmt(struct snd_soc_dai *be_dai, unsigned int fmt)
{
	struct sprd_vbc_priv *vbc = dev_get_drvdata(be_dai->dev);
	u32 lrmod;

	if ((fmt & SND_SOC_DAIFMT_FORMAT_MASK) != SND_SOC_DAIFMT_I2S)
		return -EINVAL;

	/* I2S master mode is not implemented yet */
	if ((fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) != SND_SOC_DAIFMT_BC_FC)
		return -EINVAL;

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		lrmod = VBC_IIS_LEFT_HIGH;
		break;
	case SND_SOC_DAIFMT_NB_IF:
		lrmod = VBC_IIS_RIGHT_HIGH;
		break;
	default:
		return -EINVAL;
	}

	if (be_dai->id < VBC_IIS0) {
		dev_err(be_dai->dev, "unrecognized backend DAI %d\n",
			be_dai->id);
		return -EINVAL;
	}

	vbc->iis_lrmod[be_dai->id - VBC_IIS0] = lrmod;

	/*
	 * The actual hardware update happens in hw_params() to account
	 * for routing changes after the format has been set.
	 */

	return 0;
}

static const struct snd_soc_dai_ops sprd_be_dai_ops = {
	.hw_params = sprd_vbc_be_hw_params,
	.set_fmt = sprd_vbc_be_set_fmt,
};

static struct snd_soc_dai_driver sprd_vbc_dais[] = {
	/* Frontend DAIs */
	{
		.name = "VBC_FE_FAST_PLAYBACK",
		.id = VBC_FE_FAST_PLAYBACK,
		.playback = {
			.stream_name = "VBC_FE_FAST_PLAYBACK Playback",
			.channels_min = 2,
			.channels_max = 2,
			.rates = SPRD_PCM_RATES,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
		.ops = &sprd_fe_dai_ops,
	},
	{
		.name = "VBC_FE_CAPTURE_DSP",
		.id = VBC_FE_CAPTURE_DSP,
		.capture = {
			.stream_name = "VBC_FE_CAPTURE_DSP Capture",
			.channels_min = 2,
			.channels_max = 2,
			.rates = SPRD_PCM_RATES,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
		.ops = &sprd_fe_dai_ops,
	},
	{
		.name = "VBC_FE_VOICE",
		.id = VBC_FE_VOICE,
		.playback = {
			.stream_name = "VBC_FE_VOICE Playback",
			.channels_min = 1,
			.channels_max = 1,
			.rates = SNDRV_PCM_RATE_8000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
		.capture = {
			.stream_name = "VBC_FE_VOICE Capture",
			.channels_min = 1,
			.channels_max = 1,
			.rates = SNDRV_PCM_RATE_8000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
		.ops = &sprd_fe_dai_ops,
	},
	{
		.name = "VBC_FE_VOICE_CAPTURE",
		.id = VBC_FE_VOICE_CAPTURE,
		.capture = {
			.stream_name = "VBC_FE_VOICE_CAPTURE Capture",
			.channels_min = 1,
			.channels_max = 1,
			.rates = SNDRV_PCM_RATE_8000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
		.ops = &sprd_fe_dai_ops,
	},

	/* Backend DAIs */
	SPRD_BE_DAI(VBC_IIS0),
	SPRD_BE_DAI(VBC_IIS1),
	SPRD_BE_DAI(VBC_IIS2),
	SPRD_BE_DAI(VBC_IIS3),
};

static const struct snd_soc_component_driver sprd_vbc_component = {
	.name			= "sprd-vbc-component",
	.controls		= sprd_vbc_controls,
	.num_controls		= ARRAY_SIZE(sprd_vbc_controls),
	.dapm_widgets		= sprd_vbc_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(sprd_vbc_widgets),
	.dapm_routes		= sprd_vbc_routes,
	.num_dapm_routes	= ARRAY_SIZE(sprd_vbc_routes),
};

static void sprd_vbc_init_params(struct sprd_vbc_priv *vbc)
{
	unsigned int i;

	for (i = 0; i < VBC_NUM_TX_OUT_SEL; i++)
		vbc->params.tx_out[i].id = i;

	for (i = 0; i < VBC_NUM_TX_IDS; i++) {
		vbc->params.mux_tx[i].id = i;
		vbc->params.tx_wd[i].id = i;
		vbc->params.tx_lr_mod[i].id = i;
		vbc->current_fe_tx[i] = VBC_FE_INVALID;
	}

	for (i = 0; i < VBC_NUM_RX_IDS; i++) {
		vbc->params.rx_source[i].id = i;
		vbc->params.mux_rx[i].id = i;
		vbc->params.rx_wd[i].id = i;
		vbc->params.rx_lr_mod[i].id = i;
		vbc->current_fe_rx[i] = VBC_FE_INVALID;
	}

	for (i = 0; i < VBC_NUM_IIS_PORT_IDS; i++)
		vbc->params.iis_do[i].id = i;

	for (i = 0; i < VBC_MUTEDG_NUM; i++)
		vbc->params.mutedg[i].id = i;

	for (i = 0; i < VBC_SMTHDG_NUM; i++) {
		vbc->params.smthdg[i].dg.id = i;
		vbc->params.smthdg[i].step.id = i;
	}

	for (i = 0; i < VBC_MIXERDG_NUM; i++) {
		vbc->params.mixerdg[i].id = i;
		vbc->params.mixerdg[i].mainpath.id = i;
		vbc->params.mixerdg[i].mixpath.id = i;
	}

	for (i = 0; i < VBC_MIXER_NUM; i++)
		vbc->params.mixer[i].type.id = i;

	for (i = 0; i < VBC_NUM_IIS_CONTROLLERS; i++)
		vbc->params.mst_sel[i].id = i;
}

static int sprd_vbc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sprd_vbc_priv *vbc;
	int ret;

	vbc = devm_kzalloc(dev, sizeof(*vbc), GFP_KERNEL);
	if (!vbc)
		return -ENOMEM;

	vbc->dev = dev;
	platform_set_drvdata(pdev, vbc);

	vbc->ipc = sprd_get_agdsp_ipc(dev->of_node, "sprd,dsp");
	if (IS_ERR(vbc->ipc))
		return dev_err_probe(dev, PTR_ERR(vbc->ipc),
				     "failed to get DSP handle\n");

	ret = of_reserved_mem_device_init(dev);
	if (ret) {
		dev_err(dev, "cannot get DSP shared memory region\n");
		goto free_ipc;
	}

	vbc->mcdt_voice_capture =
		sprd_mcdt_request_chan(2, SPRD_MCDT_ADC_CHAN);
	if (!vbc->mcdt_voice_capture) {
		dev_err(dev, "cannot get MCDT voice capture channel\n");
		ret = -ENODEV;
		goto free_ipc;
	}

	vbc->mcdt_playback =
		sprd_mcdt_request_chan(4, SPRD_MCDT_DAC_CHAN);
	if (!vbc->mcdt_playback) {
		dev_err(dev, "cannot get MCDT playback channel\n");
		ret = -ENODEV;
		goto free_voice_capture;
	}

	vbc->mcdt_capture =
		sprd_mcdt_request_chan(4, SPRD_MCDT_ADC_CHAN);
	if (!vbc->mcdt_capture) {
		dev_err(dev, "cannot get MCDT capture channel\n");
		ret = -ENODEV;
		goto free_playback;
	}

	sprd_vbc_init_params(vbc);

	return devm_snd_soc_register_component(dev, &sprd_vbc_component,
					       sprd_vbc_dais,
					       ARRAY_SIZE(sprd_vbc_dais));

free_playback:
	sprd_mcdt_free_chan(vbc->mcdt_playback);
free_voice_capture:
	sprd_mcdt_free_chan(vbc->mcdt_voice_capture);
free_ipc:
	sprd_put_agdsp_ipc(vbc->ipc);

	return ret;
}

static void sprd_vbc_remove(struct platform_device *pdev)
{
	struct sprd_vbc_priv *vbc = platform_get_drvdata(pdev);

	sprd_mcdt_free_chan(vbc->mcdt_capture);
	sprd_mcdt_free_chan(vbc->mcdt_playback);
	sprd_mcdt_free_chan(vbc->mcdt_voice_capture);
	sprd_put_agdsp_ipc(vbc->ipc);
}

static const struct of_device_id sprd_vbc_of_match[] = {
	{ .compatible = "sprd,ums9230-vbc" },
	{ }
};
MODULE_DEVICE_TABLE(of, sprd_vbc_of_match);

static struct platform_driver sprd_vbc_driver = {
	.probe = sprd_vbc_probe,
	.remove = sprd_vbc_remove,
	.driver = {
		.name = "sprd-vbc-v4",
		.owner = THIS_MODULE,
		.of_match_table = sprd_vbc_of_match,
	},
};

module_platform_driver(sprd_vbc_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Unisoc VBC v4 audio DSP driver");
