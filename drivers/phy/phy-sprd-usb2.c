// SPDX-License-Identifier: GPL-2.0
/*
 * Unisoc USB2 PHY driver
 *
 * Copyright (C) 2024 Otto Pflüger
 */

#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

/* otg_test_reg */
#define BIT_AON_USB2_PHY_IDDIG		BIT(3)
#define BIT_AON_OTG_VBUS_VALID_PHYREG	BIT(24)

/* otg_ctrl_reg */
#define BIT_AON_UTMI_WIDTH_SEL		BIT(30)

/* pll_reg */
#define BIT_ANLG_USB20_ISO_SW_EN	BIT(0)

/* pd_reg */
#define BIT_ANLG_USB20_PS_PD_L		BIT(3)
#define BIT_ANLG_USB20_PS_PD_S		BIT(4)

/* utmi_ctl1_reg */
#define BIT_ANLG_USB20_RESERVED		GENMASK(15, 0) /* undocumented */
#define BIT_ANLG_USB20_VBUSVLDEXT	BIT(16)
#define BIT_ANLG_USB20_DATABUS16_8	BIT(28)

/* utmi_ctl2_reg */
#define BIT_ANLG_USB20_DMPULLDOWN	BIT(3)
#define BIT_ANLG_USB20_DPPULLDOWN	BIT(4)

#define DEFAULT_EYE_PATTERN	0x04f3d1c0

struct sprd_hsphy_data {
	/* AON APB regs */
	u32 otg_test_reg;
	u32 otg_ctrl_reg;

	/* analog regs */
	u32 pll_reg;
	u32 pd_reg;
	u32 utmi_ctl1_reg;
	u32 utmi_ctl2_reg;
	u32 trimming_reg;
	u32 reg_sel_cfg_reg;
	u32 reg_sel_mask;
};

struct sprd_hsphy {
	struct device *dev;
	struct regmap *aon_apb;
	struct regmap *ana_regs;
	const struct sprd_hsphy_data *data;
};

static int sprd_hsphy_init(struct phy *phy)
{
	struct sprd_hsphy *hsphy = phy_get_drvdata(phy);

	dev_dbg(hsphy->dev, "%s()\n", __func__);

	regmap_set_bits(hsphy->aon_apb, hsphy->data->otg_ctrl_reg,
			BIT_AON_UTMI_WIDTH_SEL);
	regmap_set_bits(hsphy->ana_regs, hsphy->data->utmi_ctl1_reg,
			BIT_ANLG_USB20_DATABUS16_8);

	regmap_write(hsphy->ana_regs, hsphy->data->trimming_reg,
		     DEFAULT_EYE_PATTERN);

	return 0;
}

static int sprd_hsphy_power_on(struct phy *phy)
{
	struct sprd_hsphy *hsphy = phy_get_drvdata(phy);

	dev_dbg(hsphy->dev, "%s()\n", __func__);

	regmap_clear_bits(hsphy->ana_regs, hsphy->data->pll_reg,
			  BIT_ANLG_USB20_ISO_SW_EN);
	regmap_clear_bits(hsphy->ana_regs, hsphy->data->pd_reg,
			  BIT_ANLG_USB20_PS_PD_L | BIT_ANLG_USB20_PS_PD_S);

	return 0;
}

static int sprd_hsphy_power_off(struct phy *phy)
{
	struct sprd_hsphy *hsphy = phy_get_drvdata(phy);

	dev_dbg(hsphy->dev, "%s()\n", __func__);

	regmap_clear_bits(hsphy->aon_apb, hsphy->data->otg_test_reg,
			  BIT_AON_OTG_VBUS_VALID_PHYREG);
	regmap_clear_bits(hsphy->ana_regs, hsphy->data->utmi_ctl1_reg,
			  BIT_ANLG_USB20_VBUSVLDEXT);

	regmap_set_bits(hsphy->ana_regs, hsphy->data->pll_reg,
			BIT_ANLG_USB20_ISO_SW_EN);
	regmap_set_bits(hsphy->ana_regs, hsphy->data->pd_reg,
			BIT_ANLG_USB20_PS_PD_L | BIT_ANLG_USB20_PS_PD_S);

	return 0;
}

static int sprd_hsphy_set_mode(struct phy *phy, enum phy_mode mode, int submode)
{
	struct sprd_hsphy *hsphy = phy_get_drvdata(phy);

	switch (mode) {
	case PHY_MODE_USB_HOST:
		dev_dbg(hsphy->dev, "%s(host)\n", __func__);
		regmap_clear_bits(hsphy->aon_apb, hsphy->data->otg_ctrl_reg,
				  BIT_AON_USB2_PHY_IDDIG);
		regmap_set_bits(hsphy->ana_regs, hsphy->data->reg_sel_cfg_reg,
				hsphy->data->reg_sel_mask);
		regmap_set_bits(hsphy->ana_regs, hsphy->data->utmi_ctl2_reg,
				BIT_ANLG_USB20_DMPULLDOWN |
				BIT_ANLG_USB20_DPPULLDOWN);
		regmap_update_bits(hsphy->ana_regs, hsphy->data->utmi_ctl1_reg,
				   BIT_ANLG_USB20_RESERVED, 0x200);
		break;

	case PHY_MODE_USB_DEVICE:
		dev_dbg(hsphy->dev, "%s(device)\n", __func__);
		regmap_set_bits(hsphy->aon_apb, hsphy->data->otg_ctrl_reg,
				BIT_AON_USB2_PHY_IDDIG);
		regmap_set_bits(hsphy->ana_regs, hsphy->data->reg_sel_cfg_reg,
				hsphy->data->reg_sel_mask);
		regmap_clear_bits(hsphy->ana_regs, hsphy->data->utmi_ctl2_reg,
				  BIT_ANLG_USB20_DMPULLDOWN |
				  BIT_ANLG_USB20_DPPULLDOWN);
		regmap_update_bits(hsphy->ana_regs, hsphy->data->utmi_ctl1_reg,
				   BIT_ANLG_USB20_RESERVED, 0);
		break;

	default:
		dev_dbg(hsphy->dev, "%s(other)\n", __func__);
		break;
	}

	/* enable to activate mode */
	regmap_set_bits(hsphy->aon_apb, hsphy->data->otg_test_reg,
			BIT_AON_OTG_VBUS_VALID_PHYREG);
	regmap_set_bits(hsphy->ana_regs, hsphy->data->utmi_ctl1_reg,
			BIT_ANLG_USB20_VBUSVLDEXT);

	return 0;
}

static const struct phy_ops sprd_hsphy_ops = {
	.init = sprd_hsphy_init,
	.power_on = sprd_hsphy_power_on,
	.power_off = sprd_hsphy_power_off,
	.set_mode = sprd_hsphy_set_mode,
	.owner = THIS_MODULE,
};

static const struct regmap_config sprd_hsphy_regmap_cfg = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
};

static int sprd_hsphy_probe(struct platform_device *pdev)
{
	struct sprd_hsphy *hsphy;
	struct phy_provider *provider;
	struct phy *phy;
	void __iomem *base;

	hsphy = devm_kzalloc(&pdev->dev, sizeof(*hsphy), GFP_KERNEL);
	if (!hsphy)
		return -ENOMEM;

	hsphy->dev = &pdev->dev;

	hsphy->data = of_device_get_match_data(hsphy->dev);
	if (!hsphy->data)
		return -EINVAL;

	hsphy->aon_apb = syscon_regmap_lookup_by_phandle(hsphy->dev->of_node,
							 "sprd,syscon-aon-apb");
	if (IS_ERR(hsphy->aon_apb))
		return dev_err_probe(hsphy->dev, PTR_ERR(hsphy->aon_apb),
				     "failed to get AON APB syscon\n");

	base = devm_platform_get_and_ioremap_resource(pdev, 0, NULL);
	if (IS_ERR(base))
		return PTR_ERR(base);

	hsphy->ana_regs = devm_regmap_init_mmio(hsphy->dev, base,
						&sprd_hsphy_regmap_cfg);
	if (IS_ERR(hsphy->ana_regs))
		return PTR_ERR(hsphy->ana_regs);

	phy = devm_phy_create(hsphy->dev, NULL, &sprd_hsphy_ops);
	if (IS_ERR(phy))
		return dev_err_probe(hsphy->dev, PTR_ERR(phy),
				     "failed to create phy\n");

	phy_set_drvdata(phy, hsphy);

	provider = devm_of_phy_provider_register(hsphy->dev,
						 of_phy_simple_xlate);
	if (IS_ERR(provider))
		return dev_err_probe(hsphy->dev, PTR_ERR(provider),
				     "failed to register phy provider\n");

	return 0;
}

static const struct sprd_hsphy_data ums9230_data = {
	/* AON APB regs */
	.otg_test_reg		= 0x0204,
	.otg_ctrl_reg		= 0x0208,

	/* analog g2 regs */
	.pll_reg		= 0x001c,
	.pd_reg			= 0x0008,
	.utmi_ctl1_reg		= 0x0004,
	.utmi_ctl2_reg		= 0x000c,
	.trimming_reg		= 0x0010,
	.reg_sel_cfg_reg	= 0x0020,
	.reg_sel_mask		= BIT(2) | BIT(1),
};

static const struct of_device_id sprd_hsphy_of_match[] = {
	{ .compatible = "sprd,ums9230-hsphy", .data = &ums9230_data },
	{ }
};
MODULE_DEVICE_TABLE(of, sprd_hsphy_of_match);

static struct platform_driver sprd_hsphy_driver = {
	.probe	= sprd_hsphy_probe,
	.driver	= {
		.name		= "sprd-usb2-phy",
		.of_match_table	= sprd_hsphy_of_match,
	},
};

module_platform_driver(sprd_hsphy_driver);

MODULE_AUTHOR("Otto Pflüger <otto.pflueger@abscue.de>");
MODULE_DESCRIPTION("Unisoc USB2 PHY driver");
MODULE_LICENSE("GPL");
