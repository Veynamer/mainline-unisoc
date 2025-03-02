// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Otto Pflüger
 */

#include <linux/of_device.h>
#include <linux/rpmsg.h>

#include "sc23xx.h"
#include "txrx.h"

#define SC23XX_SIPC_MAX_RX_BUF_ADDRS 52
#define SC23XX_SIPC_MAX_TX_BUF_ADDRS 48

struct sc23xx_sipc {
	struct sc23xx_dev sdev; /* must be first */
	struct rpmsg_endpoint *cmd, *data0, *data1;
	bool exit;
};

static int sc23xx_sipc_callback(struct rpmsg_device *rpdev, void *buf,
				int len, void *priv, u32 addr)
{
	struct sc23xx_sipc *sipc_priv = dev_get_drvdata(&rpdev->dev);
	enum sc23xx_msg_type type = (unsigned long)priv;
	struct sk_buff *skb;

	if (sipc_priv->exit)
		return 0;

	skb = alloc_skb(len, GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;

	skb_put_data(skb, buf, len);

	sc23xx_rx_msg(&sipc_priv->sdev, type, skb);

	return 0;
}

static int sc23xx_sipc_tx_cmd(struct sc23xx_dev *sdev, struct sk_buff *skb)
{
	struct sc23xx_sipc *sipc_priv = container_of(sdev, struct sc23xx_sipc, sdev);

	return rpmsg_send(sipc_priv->cmd, skb->data, skb->len);
}

static const struct sc23xx_bus_ops sc23xx_sipc_ops = {
	.tx_cmd = sc23xx_sipc_tx_cmd,
	.tx_data = sc23xx_tx_data_dma,
};

static int sc23xx_sipc_tx_task(void *priv)
{
	struct sc23xx_sipc *sipc_priv = priv;
	struct sc23xx_dev *sdev = &sipc_priv->sdev;
	struct sc23xx_tx_addr_list *tx_list;
	struct sc23xx_rx_addr_free_list *rx_list;
	unsigned int len;
	int ret;

	tx_list = kzalloc(sizeof(*tx_list) + SC23XX_SIPC_MAX_TX_BUF_ADDRS *
			  sizeof(struct sc23xx_addr), GFP_KERNEL);
	if (!tx_list)
		return 0;

	rx_list = kzalloc(sizeof(*rx_list) + SC23XX_SIPC_MAX_RX_BUF_ADDRS *
			  sizeof(struct sc23xx_addr), GFP_KERNEL);
	if (!rx_list)
		goto free_tx_list;

	for (;;) {
		wait_event_interruptible(sdev->tx_wait,
					 sc23xx_tx_fill_needed(sdev) ||
					 sc23xx_rx_fill_needed(sdev) ||
					 kthread_should_stop());
		if (kthread_should_stop())
			break;

		if (!sc23xx_rx_fill_addr_list(sdev, rx_list, &len,
					      SC23XX_SIPC_MAX_RX_BUF_ADDRS)) {
			ret = rpmsg_send(sipc_priv->data0, rx_list, len);
			if (ret)
				wiphy_err(sdev->wiphy, "rx refill failed: %d\n",
					  ret);
		}

		if (!sc23xx_tx_fill_addr_list(sdev, tx_list, &len,
					      SC23XX_SIPC_MAX_TX_BUF_ADDRS)) {
			ret = rpmsg_send(sipc_priv->data0, tx_list, len);
			if (ret)
				wiphy_err(sdev->wiphy, "data tx failed: %d\n",
					  ret);
		}
	}

	kfree(rx_list);
free_tx_list:
	kfree(tx_list);
	return 0;
}

static int sc23xx_sipc_probe(struct rpmsg_device *rpdev)
{
	struct sc23xx_sipc *sipc_priv;
	struct rpmsg_channel_info chinfo;
	struct rpmsg_endpoint *ept;
	int ret;

	/* rpmsg bus does not set DMA mask, so do it here */
	rpdev->dev.dma_mask = &rpdev->dev.coherent_dma_mask;

	ret = of_dma_configure(&rpdev->dev, rpdev->dev.parent->of_node, true);
	if (ret) {
		dev_err(&rpdev->dev, "failed to configure DMA: %d\n", ret);
		return ret;
	}

	sipc_priv = sc23xx_alloc_device(&rpdev->dev, sizeof(*sipc_priv),
					&sc23xx_sipc_ops);
	if (IS_ERR(sipc_priv))
		return PTR_ERR(sipc_priv);

	dev_set_drvdata(&rpdev->dev, sipc_priv);

	sipc_priv->sdev.use_dma = true;

	strscpy_pad(chinfo.name, "sblock_wifi_cmd", sizeof(chinfo.name));
	chinfo.src = rpdev->src;
	chinfo.dst = RPMSG_ADDR_ANY;
	ept = rpmsg_create_ept(rpdev, sc23xx_sipc_callback,
			       (void *)SC23XX_MSG_TYPE_CMD,
			       chinfo);
	if (!ept) {
		dev_err(&rpdev->dev, "failed to create cmd endpoint\n");
		ret = -ENODEV;
		goto err_free;
	}
	sipc_priv->cmd = ept;

	strscpy_pad(chinfo.name, "sblock_wifi_data0", sizeof(chinfo.name));
	chinfo.src = RPMSG_ADDR_ANY;
	chinfo.dst = RPMSG_ADDR_ANY;
	ept = rpmsg_create_ept(rpdev, sc23xx_sipc_callback,
			       (void *)SC23XX_MSG_TYPE_DATA,
			       chinfo);
	if (!ept) {
		dev_err(&rpdev->dev, "failed to create data0 endpoint\n");
		ret = -ENODEV;
		goto err_destroy_cmd;
	}
	sipc_priv->data0 = ept;

	strscpy_pad(chinfo.name, "sblock_wifi_data1", sizeof(chinfo.name));
	chinfo.src = RPMSG_ADDR_ANY;
	chinfo.dst = RPMSG_ADDR_ANY;
	ept = rpmsg_create_ept(rpdev, sc23xx_sipc_callback,
			       (void *)SC23XX_MSG_TYPE_DATA,
			       chinfo);
	if (!ept) {
		dev_err(&rpdev->dev, "failed to create data1 endpoint\n");
		ret = -ENODEV;
		goto err_destroy_data0;
	}
	sipc_priv->data1 = ept;

	ret = sc23xx_load_config(&sipc_priv->sdev, "sprd/wifi_board_config.bin");
	if (ret)
		goto err_destroy_data1;

	ret = sc23xx_register_device(&sipc_priv->sdev);
	if (ret)
		goto err_destroy_data1;

	sipc_priv->sdev.tx_thread = kthread_run(sc23xx_sipc_tx_task, sipc_priv,
						"%s-tx", dev_name(&rpdev->dev));
	if (IS_ERR(sipc_priv->sdev.tx_thread)) {
		wiphy_err(sipc_priv->sdev.wiphy, "failed to create TX thread\n");
		goto err_unregister;
	}

	return 0;

err_unregister:
	sc23xx_unregister_device(&sipc_priv->sdev);
err_destroy_data1:
	rpmsg_destroy_ept(sipc_priv->data1);
err_destroy_data0:
	rpmsg_destroy_ept(sipc_priv->data0);
err_destroy_cmd:
	rpmsg_destroy_ept(sipc_priv->cmd);
err_free:
	sc23xx_free_device(&sipc_priv->sdev);
	return ret;
}

static void sc23xx_sipc_remove(struct rpmsg_device *rpdev)
{
	struct sc23xx_sipc *sipc_priv = dev_get_drvdata(&rpdev->dev);

	sipc_priv->exit = true;
	kthread_stop(sipc_priv->sdev.tx_thread);

	sc23xx_unregister_device(&sipc_priv->sdev);

	rpmsg_destroy_ept(sipc_priv->data1);
	rpmsg_destroy_ept(sipc_priv->data0);
	rpmsg_destroy_ept(sipc_priv->cmd);

	sc23xx_free_device(&sipc_priv->sdev);
}

static const struct of_device_id sc23xx_sipc_of_match[] = {
	{ .compatible = "sprd,umw2631-wlan" },
	{ }
};
MODULE_DEVICE_TABLE(of, sc23xx_sipc_of_match);

static struct rpmsg_driver sc23xx_sipc_driver = {
	.probe = sc23xx_sipc_probe,
	.remove = sc23xx_sipc_remove,
	.drv = {
		.name = "sc23xx-wlan-sipc",
		.of_match_table = sc23xx_sipc_of_match,
	},
};

module_rpmsg_driver(sc23xx_sipc_driver);

MODULE_AUTHOR("Otto Pflüger");
MODULE_DESCRIPTION("Unisoc SC23xx integrated wireless driver");
MODULE_LICENSE("GPL");
