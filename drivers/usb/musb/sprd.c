// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 Otto Pflüger
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/usb/role.h>
#include "musb_core.h"
#include "sprd_dma.h"

struct sprd_glue {
	struct device *dev;
	struct musb *musb;
	struct platform_device *musb_pdev;
	struct phy *phy;
	struct clk *clk;
	enum usb_role role;
	struct usb_role_switch *role_sw;
};

static int sprd_otg_switch_set(struct sprd_glue *glue, enum usb_role role)
{
	struct musb *musb = glue->musb;
	u8 devctl;

	if (role == glue->role)
		return 0;

	devctl = musb_readb(musb->mregs, MUSB_DEVCTL);

	if (glue->role != USB_ROLE_NONE) {
		/* force disconnect */
		musb->is_active = 0;
		devctl &= ~MUSB_DEVCTL_SESSION;
		musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);
		phy_power_off(glue->phy);
		/* wait for disconnect event */
		msleep(20);
	}

	switch (role) {
	case USB_ROLE_HOST:
		musb->is_active = 1;
		musb_set_state(musb, OTG_STATE_A_IDLE);
		MUSB_HST_MODE(musb);
		devctl |= MUSB_DEVCTL_SESSION;
		musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);
		phy_power_on(glue->phy);
		phy_set_mode(glue->phy, PHY_MODE_USB_HOST);
		break;
	case USB_ROLE_DEVICE:
		musb_set_state(musb, OTG_STATE_B_IDLE);
		MUSB_DEV_MODE(musb);
		phy_power_on(glue->phy);
		phy_set_mode(glue->phy, PHY_MODE_USB_DEVICE);
		break;
	case USB_ROLE_NONE:
	default:
		/* keep phy turned off */
		break;
	}

	glue->role = role;

	return 0;
}

static int musb_usb_role_sx_set(struct usb_role_switch *sw, enum usb_role role)
{
	return sprd_otg_switch_set(usb_role_switch_get_drvdata(sw), role);
}

static enum usb_role musb_usb_role_sx_get(struct usb_role_switch *sw)
{
	struct sprd_glue *glue = usb_role_switch_get_drvdata(sw);

	return glue->role;
}

static int sprd_otg_switch_init(struct sprd_glue *glue)
{
	struct usb_role_switch_desc role_sx_desc = { 0 };

	role_sx_desc.set = musb_usb_role_sx_set;
	role_sx_desc.get = musb_usb_role_sx_get;
	role_sx_desc.allow_userspace_control = true;
	role_sx_desc.fwnode = dev_fwnode(glue->dev);
	role_sx_desc.driver_data = glue;
	glue->role_sw = usb_role_switch_register(glue->dev, &role_sx_desc);

	return PTR_ERR_OR_ZERO(glue->role_sw);
}

static void sprd_otg_switch_exit(struct sprd_glue *glue)
{
	return usb_role_switch_unregister(glue->role_sw);
}

static irqreturn_t sprd_musb_interrupt(int irq, void *__hci)
{
	unsigned long flags;
	irqreturn_t retval = IRQ_NONE;
	struct musb *musb = __hci;
	u32 int_dma;

	spin_lock_irqsave(&musb->lock, flags);
	musb->int_usb = musb_readb(musb->mregs, MUSB_INTRUSB);
	musb->int_rx = musb_readw(musb->mregs, MUSB_INTRRX);
	musb->int_tx = musb_readw(musb->mregs, MUSB_INTRTX);

	int_dma = musb_readl(musb->mregs, MUSB_DMA_INTR_MASK_STATUS);

	if (musb->int_usb || musb->int_tx || musb->int_rx)
		retval = musb_interrupt(musb);

	if (int_dma)
		retval |= sprd_musb_dma_interrupt(musb, int_dma);

	spin_unlock_irqrestore(&musb->lock, flags);

	return retval;
}

static int sprd_musb_set_mode(struct musb *musb, u8 mode)
{
	struct device *dev = musb->controller;
	struct sprd_glue *glue = dev_get_drvdata(dev->parent);
	enum usb_role new_role;

	switch (mode) {
	case MUSB_HOST:
		new_role = USB_ROLE_HOST;
		break;
	case MUSB_PERIPHERAL:
		new_role = USB_ROLE_DEVICE;
		break;
	case MUSB_OTG:
		new_role = USB_ROLE_NONE;
		break;
	default:
		dev_err(glue->dev, "Invalid mode request\n");
		return -EINVAL;
	}

	sprd_otg_switch_set(glue, new_role);

	return 0;
}

static int sprd_musb_init(struct musb *musb)
{
	struct device *dev = musb->controller;
	struct sprd_glue *glue = dev_get_drvdata(dev->parent);
	int ret;

	glue->musb = musb;
	musb->phy = glue->phy;
	musb->is_host = false;
	musb->isr = sprd_musb_interrupt;

	ret = phy_init(glue->phy);
	if (ret)
		return ret;

	if (musb->port_mode == MUSB_OTG) {
		ret = sprd_otg_switch_init(glue);
		if (ret)
			goto err_otg_switch_init;
	}

	return 0;

err_otg_switch_init:
	phy_exit(glue->phy);

	return ret;
}

static int sprd_musb_exit(struct musb *musb)
{
	struct device *dev = musb->controller;
	struct sprd_glue *glue = dev_get_drvdata(dev->parent);

	sprd_otg_switch_set(glue, USB_ROLE_NONE);
	sprd_otg_switch_exit(glue);
	phy_exit(glue->phy);
	clk_disable_unprepare(glue->clk);

	return 0;
}

static const struct musb_platform_ops sprd_musb_ops = {
	.quirks = MUSB_DMA_SPRD,
	.init = sprd_musb_init,
	.exit = sprd_musb_exit,
	.dma_init = sprd_musb_dma_controller_create,
	.dma_exit = sprd_musb_dma_controller_destroy,
	.set_mode = sprd_musb_set_mode,
};

#define SPRD_MUSB_MAX_EP_NUM	16
#define SPRD_MUSB_RAM_BITS	13

static struct musb_fifo_cfg sprd_musb_mode_cfg[] = {
	MUSB_EP_FIFO_DOUBLE(1, FIFO_TX, 512),
	MUSB_EP_FIFO_SINGLE(1, FIFO_RX, 512),
	MUSB_EP_FIFO_DOUBLE(2, FIFO_TX, 512),
	MUSB_EP_FIFO_DOUBLE(2, FIFO_RX, 512),
	MUSB_EP_FIFO_DOUBLE(3, FIFO_TX, 512),
	MUSB_EP_FIFO_DOUBLE(3, FIFO_RX, 512),
	MUSB_EP_FIFO_DOUBLE(4, FIFO_TX, 512),
	MUSB_EP_FIFO_DOUBLE(4, FIFO_RX, 512),
	MUSB_EP_FIFO_SINGLE(5, FIFO_TX, 1024),
	MUSB_EP_FIFO_SINGLE(5, FIFO_RX, 4096),
	MUSB_EP_FIFO_DOUBLE(6, FIFO_TX, 1024),
	MUSB_EP_FIFO_DOUBLE(6, FIFO_RX, 512),
	MUSB_EP_FIFO_DOUBLE(7, FIFO_TX, 1024),
	MUSB_EP_FIFO_DOUBLE(7, FIFO_RX, 512),
	MUSB_EP_FIFO_DOUBLE(8, FIFO_TX, 1024),
	MUSB_EP_FIFO_DOUBLE(8, FIFO_RX, 512),
	MUSB_EP_FIFO_DOUBLE(9, FIFO_TX, 1024),
	MUSB_EP_FIFO_DOUBLE(9, FIFO_RX, 512),
	MUSB_EP_FIFO_DOUBLE(10, FIFO_TX, 512),
	MUSB_EP_FIFO_DOUBLE(10, FIFO_RX, 512),
	MUSB_EP_FIFO_DOUBLE(11, FIFO_TX, 512),
	MUSB_EP_FIFO_DOUBLE(11, FIFO_RX, 512),
	MUSB_EP_FIFO_DOUBLE(12, FIFO_TX, 512),
	MUSB_EP_FIFO_DOUBLE(12, FIFO_RX, 512),
	MUSB_EP_FIFO_DOUBLE(13, FIFO_TX, 8),
	MUSB_EP_FIFO_DOUBLE(13, FIFO_RX, 8),
	MUSB_EP_FIFO_DOUBLE(14, FIFO_TX, 8),
	MUSB_EP_FIFO_DOUBLE(14, FIFO_RX, 8),
	MUSB_EP_FIFO_DOUBLE(15, FIFO_TX, 8),
	MUSB_EP_FIFO_DOUBLE(15, FIFO_RX, 8),
};

static const struct musb_hdrc_config sprd_musb_hdrc_config = {
	.fifo_cfg = sprd_musb_mode_cfg,
	.fifo_cfg_size = ARRAY_SIZE(sprd_musb_mode_cfg),
	.multipoint = true,
	.dyn_fifo = true,
	.num_eps = SPRD_MUSB_MAX_EP_NUM,
	.ram_bits = SPRD_MUSB_RAM_BITS,
};

static const struct platform_device_info sprd_dev_info = {
	.name = "musb-hdrc",
	.id = PLATFORM_DEVID_AUTO,
	.dma_mask = DMA_BIT_MASK(32),
};

static int sprd_musb_probe(struct platform_device *pdev)
{
	struct musb_hdrc_platform_data *pdata;
	struct sprd_glue *glue;
	struct platform_device_info pinfo;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	int ret;

	glue = devm_kzalloc(dev, sizeof(*glue), GFP_KERNEL);
	if (!glue)
		return -ENOMEM;

	glue->dev = dev;
	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	ret = of_platform_populate(np, NULL, NULL, dev);
	if (ret)
		return dev_err_probe(dev, ret,
				"failed to create child devices at %p\n", np);

	glue->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(glue->clk))
		return dev_err_probe(dev, PTR_ERR(glue->clk),
				"failed to get clock\n");

	pdata->config = &sprd_musb_hdrc_config;
	pdata->platform_ops = &sprd_musb_ops;
	pdata->mode = usb_get_dr_mode(dev);

	if (IS_ENABLED(CONFIG_USB_MUSB_HOST))
		pdata->mode = USB_DR_MODE_HOST;
	else if (IS_ENABLED(CONFIG_USB_MUSB_GADGET))
		pdata->mode = USB_DR_MODE_PERIPHERAL;

	glue->role = USB_ROLE_NONE;

	glue->phy = devm_of_phy_get_by_index(dev, dev->of_node, 0);
	if (IS_ERR(glue->phy)) {
		return dev_err_probe(dev, PTR_ERR(glue->phy),
			"failed to get phy\n");
	}

	platform_set_drvdata(pdev, glue);

	ret = clk_prepare_enable(glue->clk);
	if (ret)
		return ret;

	pinfo = sprd_dev_info;
	pinfo.parent = dev;
	pinfo.res = pdev->resource;
	pinfo.num_res = pdev->num_resources;
	pinfo.data = pdata;
	pinfo.size_data = sizeof(*pdata);
	pinfo.fwnode = of_fwnode_handle(np);
	pinfo.of_node_reused = true;

	glue->musb_pdev = platform_device_register_full(&pinfo);
	if (IS_ERR(glue->musb_pdev)) {
		ret = PTR_ERR(glue->musb_pdev);
		dev_err(dev, "failed to register musb device: %d\n", ret);
		goto err_device_register;
	}

	return 0;

err_device_register:
	clk_disable_unprepare(glue->clk);

	return ret;
}

static void sprd_musb_remove(struct platform_device *pdev)
{
	struct sprd_glue *glue = platform_get_drvdata(pdev);

	platform_device_unregister(glue->musb_pdev);
}

#ifdef CONFIG_OF
static const struct of_device_id sprd_musb_match[] = {
	{ .compatible = "sprd,musb" },
	{ }
};
MODULE_DEVICE_TABLE(of, sprd_musb_match);
#endif

static struct platform_driver sprd_musb_driver = {
	.probe = sprd_musb_probe,
	.remove = sprd_musb_remove,
	.driver = {
		   .name = "musb-sprd",
		   .of_match_table = of_match_ptr(sprd_musb_match),
	},
};

module_platform_driver(sprd_musb_driver);

MODULE_DESCRIPTION("Unisoc MUSB Glue Layer");
MODULE_LICENSE("GPL v2");
