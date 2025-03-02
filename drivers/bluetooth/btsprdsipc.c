// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Otto Pflüger
 * Based on btqcomsmd.c and some UART drivers
 */

#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/rpmsg.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "h4_recv.h"

struct btsprdsipc {
	struct hci_dev *hdev;
	struct rpmsg_endpoint *ept;
	struct sk_buff *rx_skb;
	size_t rf_cfg_len;
	u8 *rf_cfg;
	size_t pskey_cfg_len;
	u8 *pskey_cfg;
};

static const struct h4_recv_pkt sprd_recv_pkts[] = {
	{ H4_RECV_ACL,      .recv = hci_recv_frame },
	{ H4_RECV_SCO,      .recv = hci_recv_frame },
	{ H4_RECV_EVENT,    .recv = hci_recv_frame },
};

static int btsprdsipc_callback(struct rpmsg_device *rpdev, void *data,
			       int count, void *priv, u32 addr)
{
	struct btsprdsipc *bts = dev_get_drvdata(&rpdev->dev);

	bts->rx_skb = h4_recv_buf(bts->hdev, bts->rx_skb, data, count,
				  sprd_recv_pkts, ARRAY_SIZE(sprd_recv_pkts));
	if (IS_ERR(bts->rx_skb)) {
		int err = PTR_ERR(bts->rx_skb);
		bt_dev_err(bts->hdev, "frame reassembly failed (%d)", err);
		return count;
	}

	bts->hdev->stat.byte_rx += count;
	return count;
}

static int btsprdsipc_send(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct btsprdsipc *bts = hci_get_drvdata(hdev);
	unsigned int len;
	int ret;

	/* Prepend skb with frame type */
	memcpy(skb_push(skb, 1), &hci_skb_pkt_type(skb), 1);

	/* The content length needs to be aligned to 8 bytes */
	len = ALIGN(skb->len, 8);
	if (len != skb->len) {
		ret = __skb_pad(skb, len - skb->len, false);
		if (ret)
			return ret;
	}

	ret = rpmsg_send(bts->ept, skb->data, len);
	if (ret) {
		hdev->stat.err_tx++;
	} else {
		hdev->stat.byte_tx += skb->len;

		switch (hci_skb_pkt_type(skb)) {
		case HCI_COMMAND_PKT:
			hdev->stat.cmd_tx++;
			break;
		case HCI_ACLDATA_PKT:
			hdev->stat.acl_tx++;
			break;
		case HCI_SCODATA_PKT:
			hdev->stat.sco_tx++;
			break;
		}

		kfree_skb(skb);
	}

	return ret;
}

static int btsprdsipc_open(struct hci_dev *hdev)
{
	return 0;
}

static int btsprdsipc_close(struct hci_dev *hdev)
{
	return 0;
}

static int btsprdsipc_set_bdaddr(struct hci_dev *hdev, const bdaddr_t *bdaddr)
{
	const u8 enable_cmd[] = { 0, 9, 1 };
	struct btsprdsipc *bts = hci_get_drvdata(hdev);
	struct sk_buff *skb;

	/* It seems that this must always be the first command sent to the
	 * controller in order to actually initialize the BDADDR, so all
	 * initialization is done below and not in setup().
	 */
	memcpy(bts->pskey_cfg + 20, bdaddr, 6);
	skb = __hci_cmd_sync(hdev, 0xfca0, bts->pskey_cfg_len, bts->pskey_cfg,
			     HCI_INIT_TIMEOUT);
	if (IS_ERR(skb))
		goto err;
	kfree_skb(skb);

	skb = __hci_cmd_sync(hdev, 0xfca2, bts->rf_cfg_len, bts->rf_cfg,
			     HCI_INIT_TIMEOUT);
	if (IS_ERR(skb))
		goto err;
	kfree_skb(skb);

	skb = __hci_cmd_sync(hdev, 0xfca1, sizeof(enable_cmd), enable_cmd,
			     HCI_INIT_TIMEOUT);
	if (IS_ERR(skb))
		goto err;
	kfree_skb(skb);

	skb = __hci_cmd_sync(hdev, HCI_OP_RESET, 0, NULL, HCI_INIT_TIMEOUT);
	if (IS_ERR(skb))
		goto err;
	kfree_skb(skb);

	return 0;

err:
	bt_dev_err(hdev, "initialization sequence failed (%ld)", PTR_ERR(skb));
	return PTR_ERR(skb);
}

static int btsprdsipc_probe(struct rpmsg_device *rpdev)
{
	const struct firmware *fw;
	struct btsprdsipc *bts;
	struct hci_dev *hdev;
	int ret;

	bts = devm_kzalloc(&rpdev->dev, sizeof(*bts), GFP_KERNEL);
	if (!bts)
		return -ENOMEM;

	ret = request_firmware(&fw, "sprd/bt_config_pskey.bin", &rpdev->dev);
	if (ret < 0) {
		dev_err(&rpdev->dev, "firmware request for pskey config failed (%d)", ret);
		return ret;
	}

	bts->pskey_cfg_len = fw->size;
	bts->pskey_cfg = devm_kmemdup(&rpdev->dev, fw->data, fw->size, GFP_KERNEL);
	release_firmware(fw);
	if (!bts->pskey_cfg)
		return -ENOMEM;

	ret = request_firmware(&fw, "sprd/bt_config_rf.bin", &rpdev->dev);
	if (ret < 0) {
		dev_err(&rpdev->dev, "firmware request for rf config failed (%d)", ret);
		return ret;
	}

	bts->rf_cfg_len = fw->size;
	bts->rf_cfg = devm_kmemdup(&rpdev->dev, fw->data, fw->size, GFP_KERNEL);
	release_firmware(fw);
	if (!bts->rf_cfg)
		return -ENOMEM;

	hdev = hci_alloc_dev();
	if (!hdev)
		return -ENOMEM;

	hci_set_drvdata(hdev, bts);
	bts->hdev = hdev;
	bts->ept = rpdev->ept;
	SET_HCIDEV_DEV(hdev, &rpdev->dev);

	hdev->manufacturer = 1855;
	hdev->bus = HCI_SMD;
	hdev->open = btsprdsipc_open;
	hdev->close = btsprdsipc_close;
	hdev->send = btsprdsipc_send;
	hdev->set_bdaddr = btsprdsipc_set_bdaddr;

	hci_set_quirk(hdev, HCI_QUIRK_USE_BDADDR_PROPERTY);

	ret = hci_register_dev(hdev);
	if (ret < 0)
		goto hci_free_dev;

	dev_set_drvdata(&rpdev->dev, bts);

	return 0;

hci_free_dev:
	hci_free_dev(hdev);

	return ret;
}

static void btsprdsipc_remove(struct rpmsg_device *rpdev)
{
	struct btsprdsipc *bts = dev_get_drvdata(&rpdev->dev);

	hci_unregister_dev(bts->hdev);
	hci_free_dev(bts->hdev);
}

static const struct of_device_id btsprdsipc_of_match[] = {
	{ .compatible = "sprd,bluetooth-sipc", },
	{ },
};
MODULE_DEVICE_TABLE(of, btsprdsipc_of_match);

static struct rpmsg_driver btsprdsipc_driver = {
	.probe = btsprdsipc_probe,
	.remove = btsprdsipc_remove,
	.callback = btsprdsipc_callback,
	.drv = {
		.name = "btsprdsipc",
		.of_match_table = btsprdsipc_of_match,
	},
};

module_rpmsg_driver(btsprdsipc_driver);

MODULE_AUTHOR("Otto Pflüger <otto.pflueger@abscue.de>");
MODULE_DESCRIPTION("Unisoc SIPC HCI driver");
MODULE_LICENSE("GPL v2");
