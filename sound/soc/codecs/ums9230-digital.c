// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Otto Pflüger
 */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <sound/soc.h>

#define AUD_TOP_CTL		0x0000
#define AUD_AUD_CTL		0x0004
#define AUD_I2S_CTL		0x0008
#define AUD_DAC_CTL		0x000C
#define AUD_SDM_CTL0		0x0010
#define AUD_SDM_CTL1		0x0014
#define AUD_ADC_CTL		0x0018
#define AUD_LOOP_CTL		0x001C
#define AUD_AUD_STS0		0x0020
#define AUD_INT_CLR		0x0024
#define AUD_INT_EN		0x0028
#define AUDIF_FIFO_CTL		0x002C
#define AUD_DMIC_CTL		0x0030
#define AUD_ADC1_I2S_CTL	0x0034
#define AUD_DAC_SDM_L		0x0038
#define AUD_DAC_SDM_H		0x003C

struct ums9230_aud_priv {
	struct regmap *regmap;
	struct clk *clk;
};

static const struct snd_kcontrol_new ums9230_aud_controls[] = {
	SOC_SINGLE("Swap ADC Channels", AUD_I2S_CTL, 2, 1, 0),
	SOC_SINGLE("Swap DAC Channels", AUD_I2S_CTL, 1, 1, 0),
};

static const struct snd_soc_dapm_widget ums9230_aud_dapm_widgets[] = {
	SND_SOC_DAPM_AIF_IN("I2S RX", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("I2S TX", NULL, 0, SND_SOC_NOPM, 0, 0),

	SND_SOC_DAPM_OUTPUT("AUDIF_TX"),
	SND_SOC_DAPM_INPUT("AUDIF_RX"),

	SND_SOC_DAPM_SUPPLY("Digital DACL Switch", AUD_TOP_CTL, 0, 0,
			    NULL, 0),
	SND_SOC_DAPM_SUPPLY("Digital DACR Switch", AUD_TOP_CTL, 2, 0,
			    NULL, 0),
	SND_SOC_DAPM_SUPPLY("Digital ADCL Switch", AUD_TOP_CTL, 1, 0,
			    NULL, 0),
	SND_SOC_DAPM_SUPPLY("Digital ADCR Switch", AUD_TOP_CTL, 3, 0,
			    NULL, 0),
};

static const struct snd_soc_dapm_route ums9230_aud_dapm_routes[] = {
	/* Playback */
	{"I2S RX", NULL, "UMS9230 Playback"},
	{"AUDIF_TX", NULL, "I2S RX"},
	{"AUDIF_TX", NULL, "Digital DACL Switch"},
	{"AUDIF_TX", NULL, "Digital DACR Switch"},

	/* Capture */
	{"UMS9230 Capture", NULL, "I2S TX"},
	{"I2S TX", NULL, "AUDIF_RX"},
	{"AUDIF_RX", NULL, "Digital ADCL Switch"},
	{"AUDIF_RX", NULL, "Digital ADCR Switch"},
};

static const struct snd_soc_component_driver ums9230_codec = {
	.controls		= ums9230_aud_controls,
	.num_controls		= ARRAY_SIZE(ums9230_aud_controls),
	.dapm_widgets		= ums9230_aud_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(ums9230_aud_dapm_widgets),
	.dapm_routes		= ums9230_aud_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(ums9230_aud_dapm_routes),
};

static int ums9230_convert_playback_rate(int rate)
{
	switch (rate) {
	case 8000:
		return 10;
	case 9600:
		return 9;
	case 11025:
		return 8;
	case 12000:
		return 7;
	case 16000:
		return 6;
	case 22050:
		return 5;
	case 24000:
		return 4;
	case 32000:
		return 3;
	case 44100:
		return 2;
	case 48000:
		return 1;
	case 96000:
		return 0;
	default:
		return -EINVAL;
	}
}

static int ums9230_convert_capture_rate(int rate)
{
	switch (rate) {
	case 8000:
		return 2;
	case 16000:
		return 4;
	case 32000:
		return 8;
	case 48000:
		return 12;
	default:
		return -EINVAL;
	}
}

static int ums9230_digital_hw_params(struct snd_pcm_substream *substream,
				     struct snd_pcm_hw_params *params,
				     struct snd_soc_dai *dai)
{
	int fs_mode;

	switch (substream->stream) {
	case SNDRV_PCM_STREAM_PLAYBACK:
		fs_mode = ums9230_convert_playback_rate(params_rate(params));
		if (fs_mode < 0) {
			dev_err(dai->dev, "invalid playback rate %d\n",
				params_rate(params));
			return fs_mode;
		}

		snd_soc_component_update_bits(dai->component, AUD_DAC_CTL,
					      0xf, fs_mode);
		break;
	case SNDRV_PCM_STREAM_CAPTURE:
		fs_mode = ums9230_convert_capture_rate(params_rate(params));
		if (fs_mode < 0) {
			dev_err(dai->dev, "invalid capture rate %d\n",
				params_rate(params));
			return fs_mode;
		}

		snd_soc_component_update_bits(dai->component, AUD_ADC_CTL,
					      0xf, fs_mode);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct snd_soc_dai_ops ums9230_digital_dai_ops = {
	.hw_params = ums9230_digital_hw_params,
};

#define SPRD_PCM_DA_RATES	(SNDRV_PCM_RATE_8000_48000 |	\
				 SNDRV_PCM_RATE_12000 |		\
				 SNDRV_PCM_RATE_24000 |		\
				 SNDRV_PCM_RATE_96000)

#define SPRD_PCM_AD_RATES	(SNDRV_PCM_RATE_8000 |		\
				 SNDRV_PCM_RATE_16000 |		\
				 SNDRV_PCM_RATE_32000 |		\
				 SNDRV_PCM_RATE_48000)

#define SPRD_PCM_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE |	\
				 SNDRV_PCM_FMTBIT_S24_LE)

static struct snd_soc_dai_driver ums9230_aud_dais[] = {
	{
		.name = "UMS9230_AUDIF_OUT",
		.id = 0,
		.playback = {
			.stream_name = "UMS9230 Playback",
			.rates = SPRD_PCM_DA_RATES,
			.formats = SPRD_PCM_FORMATS,
			.channels_min = 1,
			.channels_max = 2,
		},
		.ops = &ums9230_digital_dai_ops,
	},
	{
		.name = "UMS9230_AUDIF_IN",
		.id = 1,
		.capture = {
			.stream_name = "UMS9230 Capture",
			.rates = SPRD_PCM_AD_RATES,
			.formats = SPRD_PCM_FORMATS,
			.channels_min = 1,
			.channels_max = 2,
		},
		.ops = &ums9230_digital_dai_ops,
	},
};

static const struct regmap_range ums9230_aud_wr_ranges[] = {
	regmap_reg_range(AUD_TOP_CTL, AUD_LOOP_CTL),
	regmap_reg_range(AUD_INT_CLR, AUD_DAC_SDM_H),
};

static const struct regmap_access_table ums9230_aud_wr_table = {
	.yes_ranges = ums9230_aud_wr_ranges,
	.n_yes_ranges = ARRAY_SIZE(ums9230_aud_wr_ranges),
};

static const struct regmap_config ums9230_aud_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = AUD_DAC_SDM_H,
	.cache_type = REGCACHE_FLAT,
	.wr_table = &ums9230_aud_wr_table,
};

static int ums9230_aud_dev_probe(struct platform_device *pdev)
{
	struct ums9230_aud_priv *priv;
	struct device *dev = &pdev->dev;
	void __iomem *base;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);

	priv->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(priv->clk)) {
		return dev_err_probe(dev, PTR_ERR(priv->clk),
				     "cannot get clock");
	}

	ret = clk_prepare_enable(priv->clk);
	if (ret) {
		dev_err(dev, "failed to enable clock\n");
		return ret;
	}

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	priv->regmap = devm_regmap_init_mmio(dev, base,
					     &ums9230_aud_regmap_config);
	if (IS_ERR(priv->regmap))
		return PTR_ERR(priv->regmap);

	pm_runtime_set_autosuspend_delay(dev, 3000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_set_active(dev);
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	return devm_snd_soc_register_component(dev, &ums9230_codec,
					       ums9230_aud_dais,
					       ARRAY_SIZE(ums9230_aud_dais));
}

static int __maybe_unused ums9230_aud_runtime_suspend(struct device *dev)
{
	struct ums9230_aud_priv *priv = dev_get_drvdata(dev);

	regcache_cache_only(priv->regmap, true);
	regcache_mark_dirty(priv->regmap);

	clk_disable_unprepare(priv->clk);

	return 0;
}

static int __maybe_unused ums9230_aud_runtime_resume(struct device *dev)
{
	struct ums9230_aud_priv *priv = dev_get_drvdata(dev);
	int ret;

	ret = clk_prepare_enable(priv->clk);
	if (ret) {
		dev_err(dev, "failed to enable clock\n");
		return ret;
	}

	regcache_cache_only(priv->regmap, false);
	regcache_sync(priv->regmap);

	return 0;
}

static const struct dev_pm_ops ums9230_aud_pm_ops = {
	SET_RUNTIME_PM_OPS(ums9230_aud_runtime_suspend,
			   ums9230_aud_runtime_resume, NULL)
};

static const struct of_device_id ums9230_aud_match_table[] = {
	{ .compatible = "sprd,ums9230-digital-codec" },
	{ }
};
MODULE_DEVICE_TABLE(of, ums9230_aud_match_table);

static struct platform_driver ums9230_aud_driver = {
	.probe = ums9230_aud_dev_probe,
	.driver = {
		.name = "ums9230-digital-codec",
		.of_match_table = ums9230_aud_match_table,
		.pm = &ums9230_aud_pm_ops,
	},
};

module_platform_driver(ums9230_aud_driver);

MODULE_DESCRIPTION("Unisoc UMS9230 digital audio codec driver");
MODULE_LICENSE("GPL");
