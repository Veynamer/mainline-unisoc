// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Otto Pflüger
 */

#include <linux/io.h>
#include <linux/mailbox_client.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/soc/sprd/agdsp.h>

#define UMS9230_AON_AGDSP_ACCESS_REG	0x014C
#define UMS9230_AON_AGDSP_ACCESS_EN	BIT(5)

/* These sizes are required by the DSP firmware */
#define CMD_PARAM_BUF_SIZE	1024
#define SMSG_BUF_SIZE		64

struct agdsp_ring_msg {
	u16 cmd;
	u16 channel;
	u32 param[4];
} __packed;

struct agdsp_smem {
	u8 param_buf[CMD_PARAM_BUF_SIZE];
	struct agdsp_ring_msg tx_msg[SMSG_BUF_SIZE];
	struct agdsp_ring_msg rx_msg[SMSG_BUF_SIZE];
	u32 tx_rdptr;
	u32 tx_wrptr;
	u32 rx_rdptr;
	u32 rx_wrptr;
} __packed;

struct agdsp_shm_params {
	u32 id;
	u32 type;
	u32 phy_addr;
	u32 size;
} __packed;

struct sprd_agdsp_ipc {
	struct generic_pm_domain genpd;
	struct regmap *aon_apb_regs;
	struct mbox_client mbox_client;
	struct mbox_chan *mbox_chan;
	phys_addr_t smem_phys;
	struct agdsp_smem __iomem *smem_virt;
	spinlock_t msg_lock;
	struct mutex cmd_lock;
	u16 last_cmd;
	u16 last_channel;
	struct completion cmd_response;
};

static u64 agdsp_wakeup_msg = 1;

static void agdsp_ipc_callback(struct mbox_client *mbox_client, void *msg_data)
{
	struct sprd_agdsp_ipc *ipc =
		container_of(mbox_client, struct sprd_agdsp_ipc, mbox_client);
	u32 rdptr;

	if (msg_data)
		dev_dbg(ipc->mbox_client.dev, "mbox rx: %016llx\n",
			*((u64 *)msg_data));

	rdptr = readl_relaxed(&ipc->smem_virt->rx_rdptr);
	while (rdptr != readl_relaxed(&ipc->smem_virt->rx_wrptr)) {
		struct agdsp_ring_msg msg;
		memcpy_fromio(&msg,
			      &ipc->smem_virt->rx_msg[rdptr % SMSG_BUF_SIZE],
			      sizeof(msg));
		dev_dbg(ipc->mbox_client.dev,
			"message in buffer: %04x %04x %08x %08x %08x %08x\n",
			msg.cmd, msg.channel, msg.param[0], msg.param[1],
			msg.param[2], msg.param[3]);
		if (msg.cmd == ipc->last_cmd &&
		    msg.channel == ipc->last_channel)
			complete_all(&ipc->cmd_response);
		rdptr += 1;
	}
	writel_relaxed(rdptr, &ipc->smem_virt->rx_rdptr);
}

int sprd_agdsp_send_msg(struct sprd_agdsp_ipc *ipc, u16 channel, u16 cmd,
			u32 param0, u32 param1, u32 param2, u32 param3)
{
	struct agdsp_ring_msg msg;
	unsigned long flags;
	u32 wrptr;
	int ret;

	msg.channel = channel;
	msg.cmd = cmd;
	msg.param[0] = param0;
	msg.param[1] = param1;
	msg.param[2] = param2;
	msg.param[3] = param3;

	spin_lock_irqsave(&ipc->msg_lock, flags);

	dev_dbg(ipc->mbox_client.dev,
		"sending message: %04x %04x %08x %08x %08x %08x\n",
		cmd, channel, param0, param1, param2, param3);

	wrptr = readl_relaxed(&ipc->smem_virt->tx_wrptr);
	memcpy_toio(&ipc->smem_virt->tx_msg[wrptr % SMSG_BUF_SIZE],
		    &msg, sizeof(msg));
	writel(wrptr + 1, &ipc->smem_virt->tx_wrptr);

	ret = mbox_send_message(ipc->mbox_chan, &agdsp_wakeup_msg);

	spin_unlock_irqrestore(&ipc->msg_lock, flags);

	if (ret < 0)
		return ret;

	return 0;
}
EXPORT_SYMBOL_GPL(sprd_agdsp_send_msg);

int sprd_agdsp_send_cmd(struct sprd_agdsp_ipc *ipc, u16 channel, u16 cmd,
			s32 id, s32 stream, void *params, size_t param_size)
{
	int ret;

	if (param_size > CMD_PARAM_BUF_SIZE)
		return -E2BIG;

	mutex_lock(&ipc->cmd_lock);

	memcpy_toio(ipc->smem_virt->param_buf, params, param_size);

	ipc->last_cmd = cmd;
	ipc->last_channel = channel;
	reinit_completion(&ipc->cmd_response);

	ret = sprd_agdsp_send_msg(ipc, channel, cmd, id, stream,
				  ipc->smem_phys, 0);
	if (ret)
		goto out;

	ret = wait_for_completion_timeout(&ipc->cmd_response, 5 * HZ);
	if (ret == 0) {
		ret = -ETIMEDOUT;
		goto out;
	}

	ret = 0;

out:
	mutex_unlock(&ipc->cmd_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(sprd_agdsp_send_cmd);

int sprd_agdsp_shm_cmd(struct sprd_agdsp_ipc *ipc, u16 channel, u16 cmd,
		       s32 id, s32 stream, u32 type, dma_addr_t addr,
		       size_t size)
{
	struct agdsp_shm_params params = {
		.id = id,
		.type = type,
		.phy_addr = addr,
		.size = size,
	};

	return sprd_agdsp_send_cmd(ipc, channel, cmd, id, stream, &params,
				   sizeof(params));
}
EXPORT_SYMBOL_GPL(sprd_agdsp_shm_cmd);

static int agdsp_power_on(struct generic_pm_domain *genpd)
{
	struct sprd_agdsp_ipc *ipc =
		container_of(genpd, struct sprd_agdsp_ipc, genpd);
	int ret;

	regmap_set_bits(ipc->aon_apb_regs, UMS9230_AON_AGDSP_ACCESS_REG,
			UMS9230_AON_AGDSP_ACCESS_EN);

	ret = mbox_send_message(ipc->mbox_chan, &agdsp_wakeup_msg);
	if (ret < 0)
		return ret;

	usleep_range(100, 200);
	dev_dbg(ipc->mbox_client.dev, "power on complete\n");

	return 0;
}

static int agdsp_power_off(struct generic_pm_domain *genpd)
{
	struct sprd_agdsp_ipc *ipc =
		container_of(genpd, struct sprd_agdsp_ipc, genpd);

	regmap_clear_bits(ipc->aon_apb_regs, UMS9230_AON_AGDSP_ACCESS_REG,
			  UMS9230_AON_AGDSP_ACCESS_EN);

	return 0;
}

static int sprd_agdsp_probe(struct platform_device *pdev)
{
	struct sprd_agdsp_ipc *ipc;
	struct device_node *region;
	struct reserved_mem *rmem;
	int ret;

	ipc = devm_kzalloc(&pdev->dev, sizeof(*ipc), GFP_KERNEL);
	if (!ipc)
		return -ENOMEM;

	spin_lock_init(&ipc->msg_lock);
	mutex_init(&ipc->cmd_lock);
	init_completion(&ipc->cmd_response);

	ipc->aon_apb_regs = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
							"sprd,syscon-aon-apb");
	if (IS_ERR(ipc->aon_apb_regs)) {
		dev_err(&pdev->dev, "failed to get AON APB syscon\n");
		return PTR_ERR(ipc->aon_apb_regs);
	}

	region = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
	if (IS_ERR(region)) {
		dev_err(&pdev->dev, "failed to parse memory-region handle\n");
		return PTR_ERR(region);
	}

	rmem = of_reserved_mem_lookup(region);
	of_node_put(region);

	if (!rmem) {
		dev_err(&pdev->dev, "failed to look up memory region\n");
		return -ENOMEM;
	}

	if (rmem->size < sizeof(struct agdsp_smem)) {
		dev_err(&pdev->dev, "reserved memory region is too small\n");
		return -ENOMEM;
	}

	ipc->smem_phys = rmem->base;
	ipc->smem_virt = devm_ioremap(&pdev->dev, rmem->base, rmem->size);
	if (!ipc->smem_virt) {
		dev_err(&pdev->dev, "failed to map memory region\n");
		return -ENOMEM;
	}

	ipc->genpd.name = dev_name(&pdev->dev);
	ipc->genpd.power_on = agdsp_power_on;
	ipc->genpd.power_off = agdsp_power_off;

	ret = pm_genpd_init(&ipc->genpd, NULL, true);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to initialize power domain\n");
		return ret;
	}

	ret = of_genpd_add_provider_simple(pdev->dev.of_node, &ipc->genpd);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to add power domain provider\n");
		return ret;
	}

	ipc->mbox_client.dev = &pdev->dev;
	ipc->mbox_client.rx_callback = agdsp_ipc_callback;

	/* Receive any messages that might be in the buffer */
	agdsp_ipc_callback(&ipc->mbox_client, NULL);

	ipc->mbox_chan = mbox_request_channel(&ipc->mbox_client, 0);
	if (IS_ERR(ipc->mbox_chan)) {
		of_genpd_del_provider(pdev->dev.of_node);
		pm_genpd_remove(&ipc->genpd);
		return dev_err_probe(&pdev->dev, PTR_ERR(ipc->mbox_chan),
				     "failed to request mailbox\n");
	}

	platform_set_drvdata(pdev, ipc);

	return 0;
}

static void sprd_agdsp_remove(struct platform_device *pdev)
{
	struct sprd_agdsp_ipc *ipc = platform_get_drvdata(pdev);

	mbox_free_channel(ipc->mbox_chan);
}

static const struct of_device_id sprd_agdsp_of_match[] = {
	{ .compatible = "sprd,ums9230-agdsp" },
	{ }
};
MODULE_DEVICE_TABLE(of, sprd_agdsp_of_match);

static struct platform_driver sprd_agdsp_driver = {
	.probe = sprd_agdsp_probe,
	.remove = sprd_agdsp_remove,
	.driver = {
		.name = "sprd-agdsp",
		.of_match_table = sprd_agdsp_of_match,
	},
};
module_platform_driver(sprd_agdsp_driver);

struct sprd_agdsp_ipc *sprd_get_agdsp_ipc(struct device_node *node,
					  const char *name)
{
	struct device_node *agdsp_np;
	struct device *dev;

	agdsp_np = of_parse_phandle(node, name, 0);
	if (!agdsp_np)
		return ERR_PTR(-ENODEV);

	dev = driver_find_device_by_of_node(&sprd_agdsp_driver.driver,
					    agdsp_np);
	of_node_put(agdsp_np);
	if (!dev)
		return ERR_PTR(-EPROBE_DEFER);

	return dev_get_drvdata(dev);
}
EXPORT_SYMBOL_GPL(sprd_get_agdsp_ipc);

void sprd_put_agdsp_ipc(struct sprd_agdsp_ipc *ipc)
{
	put_device(ipc->mbox_client.dev);
}
EXPORT_SYMBOL_GPL(sprd_put_agdsp_ipc);

MODULE_DESCRIPTION("Unisoc audio DSP communication driver");
MODULE_LICENSE("GPL");
