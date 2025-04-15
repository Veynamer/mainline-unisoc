// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Otto Pflüger
 */

#include <linux/module.h>
#include <sound/soc.h>

#include "sprd-card-common.h"

SND_SOC_DAILINK_DEFS(fe_fast_playback,
	DAILINK_COMP_ARRAY(COMP_CPU("VBC_FE_FAST_PLAYBACK")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(fe_capture_dsp,
	DAILINK_COMP_ARRAY(COMP_CPU("VBC_FE_CAPTURE_DSP")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(fe_voice,
	DAILINK_COMP_ARRAY(COMP_CPU("VBC_FE_VOICE")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(voice_hostless,
	DAILINK_COMP_ARRAY(COMP_CPU("VBC_FE_VOICE")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(voice_capture,
	DAILINK_COMP_ARRAY(COMP_CPU("VBC_FE_VOICE_CAPTURE")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(be_iis0,
	DAILINK_COMP_ARRAY(COMP_CPU("VBC_IIS0")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(be_iis1,
	DAILINK_COMP_ARRAY(COMP_CPU("VBC_IIS1")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(be_iis2,
	DAILINK_COMP_ARRAY(COMP_CPU("VBC_IIS2")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(be_iis3,
	DAILINK_COMP_ARRAY(COMP_CPU("VBC_IIS3")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

static const struct snd_soc_pcm_stream voice_params = {
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rate_min = 8000,
	.rate_max = 8000,
	.channels_min = 1,
	.channels_max = 1,
};

static struct snd_soc_dai_link ums9230_card_dai_links[] = {
	/* DPCM frontend */
	{
		.name = "FE_FAST_PLAYBACK",
		.stream_name = "FE_FAST_PLAYBACK",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.playback_only = 1,
		SND_SOC_DAILINK_REG(fe_fast_playback),
	},
	{
		.name = "FE_CAPTURE_DSP",
		.stream_name = "FE_CAPTURE_DSP",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.capture_only = 1,
		SND_SOC_DAILINK_REG(fe_capture_dsp),
	},
	{
		.name = "FE_VOICE",
		.stream_name = "FE_VOICE",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		SND_SOC_DAILINK_REG(fe_voice),
	},

	/* Modem voice channels */
	{
		.name = "VOICE",
		.stream_name = "VOICE",
		.c2c_params = &voice_params,
		.num_c2c_params = 1,
		SND_SOC_DAILINK_REG(voice_hostless),
	},
	{
		.name = "VOICE_CAPTURE",
		.stream_name = "VOICE_CAPTURE",
		.capture_only = 1,
		SND_SOC_DAILINK_REG(voice_capture),
	},

	/* DPCM backend */
	{
		.name = "IIS0",
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			   SND_SOC_DAIFMT_CBP_CFP,
		.no_pcm = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(be_iis0),
	},
	{
		.name = "IIS1",
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			   SND_SOC_DAIFMT_CBP_CFP,
		.no_pcm = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(be_iis1),
	},
	{
		.name = "IIS2",
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			   SND_SOC_DAIFMT_CBP_CFP,
		.no_pcm = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(be_iis2),
	},
	{
		.name = "IIS3",
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			   SND_SOC_DAIFMT_CBP_CFP,
		.no_pcm = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(be_iis3),
	},
};

static struct snd_soc_card ums9230_card = {
	.dai_link = ums9230_card_dai_links,
	.num_links = ARRAY_SIZE(ums9230_card_dai_links),
	.owner = THIS_MODULE,
};

static int ums9230_card_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct snd_soc_card *card;
	int ret;

	card = &ums9230_card;
	card->dev = dev;

	ret = sprd_card_parse_of(card);
	if (ret)
		return ret;

	return devm_snd_soc_register_card(dev, card);
}

static const struct of_device_id ums9230_card_of_match[] = {
	{ .compatible = "sprd,ums9230-audio-card" },
	{ }
};
MODULE_DEVICE_TABLE(of, ums9230_card_of_match);

static struct platform_driver ums9230_card_driver = {
	.driver = {
		.name = "snd-soc-ums9230-card",
		.of_match_table = ums9230_card_of_match,
	},
	.probe = ums9230_card_probe,
};

module_platform_driver(ums9230_card_driver);

MODULE_DESCRIPTION("Unisoc ASoC Machine Driver");
MODULE_LICENSE("GPL");
