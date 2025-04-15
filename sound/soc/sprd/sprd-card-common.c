// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Otto Pflüger
 */

#include <linux/module.h>
#include <linux/of_reserved_mem.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "sprd-card-common.h"

static int sprd_hw_params_fixup_24bit(struct snd_soc_pcm_runtime *rtd,
				      struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *fmt = hw_param_mask(params,
					SNDRV_PCM_HW_PARAM_FORMAT);

	rate->min = rate->max = 48000;
	channels->min = channels->max = 2;
	snd_mask_none(fmt);
	snd_mask_set(fmt, SNDRV_PCM_FORMAT_S24_LE);

	return 0;
}

static int sprd_hw_params_fixup_16bit(struct snd_soc_pcm_runtime *rtd,
				      struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *fmt = hw_param_mask(params,
					SNDRV_PCM_HW_PARAM_FORMAT);

	rate->min = rate->max = 48000;
	channels->min = channels->max = 2;
	snd_mask_none(fmt);
	snd_mask_set(fmt, SNDRV_PCM_FORMAT_S16_LE);

	return 0;
}

int sprd_card_parse_of(struct snd_soc_card *card)
{
	struct device_node *np;
	struct device_node *codec = NULL;
	struct device_node *platform = NULL;
	struct device *dev = card->dev;
	struct snd_soc_dai_link *link;
	const char *link_name;
	int ret, i;
	u32 val;

	ret = of_reserved_mem_device_init(card->dev);
	if (ret)
		dev_warn(card->dev,
			 "no reserved DMA memory for sound card device\n");

	ret = snd_soc_of_parse_card_name(card, "model");
	if (ret) {
		dev_err(dev, "error parsing card name: %d\n", ret);
		return ret;
	}

	/* DAPM routes */
	if (of_property_present(dev->of_node, "audio-routing")) {
		ret = snd_soc_of_parse_audio_routing(card, "audio-routing");
		if (ret)
			return ret;
	}

	if (of_property_present(dev->of_node, "widgets")) {
		ret = snd_soc_of_parse_audio_simple_widgets(card, "widgets");
		if (ret)
			return ret;
	}

	ret = snd_soc_of_parse_pin_switches(card, "pin-switches");
	if (ret)
		return ret;

	ret = snd_soc_of_parse_aux_devs(card, "aux-devs");
	if (ret)
		return ret;

	platform = of_parse_phandle(dev->of_node, "sprd,pcm-platform", 0);
	if (!platform) {
		dev_err(dev, "sprd,pcm-platform property missing or invalid\n");
		return -EINVAL;
	}

	/* Enable platform PCM DMA for frontend DAIs that need it */
	for_each_card_prelinks(card, i, link) {
		if (link->no_pcm || link->c2c_params)
			link->num_platforms = 0;
		else
			link->platforms->of_node = platform;
	}

	/* Codec links */
	for_each_available_child_of_node(dev->of_node, np) {
		ret = of_property_read_string(np, "link-name", &link_name);
		if (ret) {
			dev_err(card->dev, "error getting codec link name\n");
			goto err_put_np;
		}

		for_each_card_prelinks(card, i, link) {
			if (!strcmp(link_name, link->name))
				break;
		}

		if (i >= card->num_links) {
			dev_err(card->dev, "link %s does not exist\n", link_name);
			ret = -EINVAL;
			goto err_put_np;
		}

		ret = of_property_read_u32(np, "sprd,fixed-fmt", &val);
		if (!ret) {
			if (val == 24) {
				link->be_hw_params_fixup =
					sprd_hw_params_fixup_24bit;
			} else if (val == 16) {
				link->be_hw_params_fixup =
					sprd_hw_params_fixup_16bit;
			} else {
				dev_err(card->dev, "%d-bit I2S not supported\n",
					val);
				ret = -EINVAL;
				goto err_put_np;
			}
		}

		link->dai_fmt = snd_soc_daifmt_parse_format(np, NULL);
		link->dai_fmt |= SND_SOC_DAIFMT_CBP_CFP;

		codec = of_get_child_by_name(np, "codec");
		if (codec) {
			ret = snd_soc_of_get_dai_link_codecs(dev, codec, link);
			if (ret < 0) {
				dev_err_probe(card->dev, ret,
					      "%s: codec dai not found\n", link->name);
				goto err_put_codec;
			}
			of_node_put(codec);
		}
	}

	return 0;
err_put_codec:
	of_node_put(codec);
err_put_np:
	of_node_put(np);
	return ret;
}
EXPORT_SYMBOL_GPL(sprd_card_parse_of);

MODULE_DESCRIPTION("ASoC Unisoc card helper functions");
MODULE_LICENSE("GPL");
