// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Otto Pflüger
 */

#include <linux/etherdevice.h>
#include <linux/if_arp.h>
#include <linux/rpmsg.h>
#include <net/pkt_sched.h>

struct rpmsg_wwan_net {
	struct rpmsg_endpoint *ept;
};

static int rpmsg_wwan_net_callback(struct rpmsg_device *rpdev, void *buf,
				   int len, void *priv, u32 addr)
{
	struct net_device *ndev = dev_get_drvdata(&rpdev->dev);
	struct sk_buff *skb;

	skb = netdev_alloc_skb(ndev, len);
	if (!skb)
		return -ENOMEM;

	skb_put_data(skb, buf, len);

	switch (skb->data[0] & 0xf0) {
	case 0x40:
		skb->protocol = htons(ETH_P_IP);
		break;
	case 0x60:
		skb->protocol = htons(ETH_P_IPV6);
		break;
	default:
		netdev_warn(ndev, "unexpected protocol, first byte %02x\n",
			    skb->data[0]);
		return 0;
	}

	netif_rx(skb);

	return 0;
}

static netdev_tx_t rpmsg_wwan_net_start_xmit(struct sk_buff *skb,
					     struct net_device *ndev)
{
	struct rpmsg_wwan_net *rpwwan = netdev_priv(ndev);
	int ret;

	ret = rpmsg_trysend(rpwwan->ept, skb->data, skb->len);
	if (ret) {
		netdev_err(ndev, "rpmsg tx failed: %d\n", ret);
		return NETDEV_TX_BUSY;
	}

	return NETDEV_TX_OK;
}

static const struct net_device_ops rpmsg_wwan_net_ops = {
	.ndo_start_xmit = rpmsg_wwan_net_start_xmit,
};

static void rpmsg_wwan_netdev_setup(struct net_device *ndev)
{
	ndev->netdev_ops = &rpmsg_wwan_net_ops;
	ndev->type = ARPHRD_RAWIP;
	ndev->flags = IFF_POINTOPOINT | IFF_NOARP;
	ndev->mtu = ETH_DATA_LEN;
	ndev->max_mtu = ETH_DATA_LEN;
	ndev->tx_queue_len = DEFAULT_TX_QUEUE_LEN;

	ndev->addr_assign_type = NET_ADDR_RANDOM;
	eth_random_addr(ndev->perm_addr);
}

static const struct device_type wwan_type = { .name = "wwan" };

static int rpmsg_wwan_net_probe(struct rpmsg_device *rpdev)
{
	struct rpmsg_wwan_net *rpwwan;
	struct net_device *ndev;
	int ret;

	ndev = alloc_netdev(sizeof(*rpwwan), "wwan%d", NET_NAME_ENUM,
			    rpmsg_wwan_netdev_setup);
	if (!ndev)
		return -ENOMEM;

	SET_NETDEV_DEV(ndev, &rpdev->dev);
	SET_NETDEV_DEVTYPE(ndev, &wwan_type);

	rpwwan = netdev_priv(ndev);
	rpwwan->ept = rpdev->ept;

	dev_set_drvdata(&rpdev->dev, ndev);

	ret = register_netdev(ndev);
	if (ret) {
		free_netdev(ndev);
		return ret;
	}

	return 0;
}

static void rpmsg_wwan_net_remove(struct rpmsg_device *rpdev)
{
	struct net_device *ndev = dev_get_drvdata(&rpdev->dev);

	unregister_netdev(ndev);
}

static const struct of_device_id rpmsg_wwan_net_of_table[] = {
	/* RPMSG channels for Unisoc SoCs with integrated LTE modem */
	{ .compatible = "sprd,seth" },
	{ }
};
MODULE_DEVICE_TABLE(of, rpmsg_wwan_net_of_table);

static struct rpmsg_driver rpmsg_wwan_net_driver = {
	.drv.name = "rpmsg-wwan-net",
	.drv.of_match_table = rpmsg_wwan_net_of_table,
	.probe = rpmsg_wwan_net_probe,
	.remove = rpmsg_wwan_net_remove,
	.callback = rpmsg_wwan_net_callback,
};
module_rpmsg_driver(rpmsg_wwan_net_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Simple RPMSG-based WWAN network driver");
MODULE_AUTHOR("Otto Pflüger <otto.pflueger@abscue.de>");
