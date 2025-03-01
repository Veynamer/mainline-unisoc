// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 Otto Pflüger
 */

#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/remoteproc.h>
#include <linux/reset.h>

#include "sprd_common.h"

struct sprd_modem_region_header {
	char name[24];
	__le64 base;
	__le64 size;
	__le64 flags;
} __packed;

struct sprd_modem_image_header {
	__be32 magic;
	__be32 version;
	__le32 length;
	__le32 unused;
	u8 rsvmem_info[4 * 48];
	__le32 bootcode_len;
	u8 bootcode[200];
	u32 unused2;
	struct sprd_modem_region_header region_info[8];
	/* more fields exist, but are not used here */
} __packed;

struct sprd_modem_info {
	u32 corereset_reg;
	u32 corereset_mask;
};

struct sprd_modem {
	struct device *dev;
	struct sprd_sipc_subdev sipc;
	struct reset_control *reset;
	struct regmap *aon_apb_regs;
	const struct sprd_modem_info *info;
	phys_addr_t mem_base;
	size_t mem_size;
	void *bootmem;
};

struct sprd_modem_region_def {
	const char *name;
	const char *filename;
	bool zero;
};

const struct sprd_modem_region_def sprd_modem_region_defs[] = {
	{ .name = "modem", .filename = NULL /* use main image */ },
	{ .name = "cpcmdline", .zero = true },
	{ .name = "deltanv", .filename = "sprd/l_deltanv.bin" },
	{ .name = "fixnv", .filename = "sprd/l_fixnv.bin" },
	{ .name = "runnv", .filename = "sprd/l_runtimenv.bin" },
	{ .name = "gdsp", .filename = "sprd/l_gdsp.bin" },
	{ .name = "ldsp", .filename = "sprd/l_ldsp.bin" },
	{ /* sentinel */ }
};

static int sprd_modem_load_region(struct sprd_modem *m,
				  const struct firmware *fw,
				  void *mem, int idx,
				  const struct sprd_modem_region_def *rdef,
				  struct sprd_modem_region_header *rhdr)
{
	u64 region_start, region_end, region_size;
	int ret;

	if (rdef->filename)
		dev_dbg(m->dev, "loading file %s into region %s\n", rdef->filename, rdef->name);
	else
		dev_dbg(m->dev, "loading base image into region %s\n", rdef->name);

	region_start = le64_to_cpu(rhdr->base);
	region_size = le64_to_cpu(rhdr->size);
	region_end = region_start + region_size;

	if (region_end > (m->mem_base + m->mem_size) || region_start < m->mem_base) {
		dev_err(m->dev, "region %d (%s) outside memory range\n", idx, rdef->name);
		return -EINVAL;
	}

	if (rdef->filename) {
		ret = request_firmware(&fw, rdef->filename, m->dev);
		if (ret < 0)
			return ret;
	}

	if (rdef->zero) {
		memset(mem + (region_start - m->mem_base), 0, region_size);
	} else if (fw->size > region_size) {
		if (rdef->filename)
			release_firmware(fw);
		dev_err(m->dev, "firmware too large for region %s\n", rdef->name);
		return -ENOMEM;
	} else {
		memcpy(mem + (region_start - m->mem_base), fw->data, fw->size);
	}

	if (rdef->filename)
		release_firmware(fw);

	return 0;
}

static int sprd_modem_load(struct rproc *rproc, const struct firmware *fw)
{
	struct sprd_modem_image_header *hdr = (void *)fw->data;
	const struct sprd_modem_region_def *rdef;
	struct sprd_modem_region_header *rhdr;
	struct sprd_modem *m = rproc->priv;
	int i, ret = 0;
	void *mem;

	if (le32_to_cpu(hdr->bootcode_len) * 4 > sizeof(hdr->bootcode)) {
		dev_err(m->dev, "invalid bootcode size in image\n");
		return -EINVAL;
	}

	memcpy_toio(m->bootmem, hdr->bootcode, le32_to_cpu(hdr->bootcode_len) * 4);

	mem = memremap(m->mem_base, m->mem_size, MEMREMAP_WC);
	if (!mem) {
		dev_err(m->dev, "failed to map memory region\n");
		return -EBUSY;
	}

	for (i = 0; i < ARRAY_SIZE(hdr->region_info); i++) {
		rhdr = &hdr->region_info[i];
		if (!rhdr->base)
			break;

		dev_dbg(m->dev, "found region %-*s at %08llx\n",
			(int)sizeof(rhdr->name), rhdr->name, le64_to_cpu(rhdr->base));

		for (rdef = sprd_modem_region_defs; rdef->name; rdef++) {
			if (!strncmp(rhdr->name, rdef->name, sizeof(rhdr->name))) {
				ret = sprd_modem_load_region(m, fw, mem, i, rdef, rhdr);
				if (ret < 0)
					goto out;
			}
		}
	}

out:
	memunmap(mem);
	return ret;
}

static int sprd_modem_start(struct rproc *rproc)
{
	struct sprd_modem *m = rproc->priv;
	int ret;

	pm_runtime_enable(m->dev);
	ret = pm_runtime_resume_and_get(m->dev);
	if (ret < 0)
		goto err_pm_disable;

	ret = reset_control_deassert(m->reset);
	if (ret < 0)
		goto err_pm_put;

	/* start processor */
	regmap_clear_bits(m->aon_apb_regs, m->info->corereset_reg,
			  m->info->corereset_mask);

	return 0;

err_pm_put:
	pm_runtime_put_sync(m->dev);
err_pm_disable:
	pm_runtime_disable(m->dev);
	return ret;
}

static int sprd_modem_stop(struct rproc *rproc)
{
	struct sprd_modem *m = rproc->priv;

	/* stop processor */
	regmap_set_bits(m->aon_apb_regs, m->info->corereset_reg,
			m->info->corereset_mask);

	reset_control_assert(m->reset);

	pm_runtime_put_sync_suspend(m->dev);
	pm_runtime_disable(m->dev);

	return 0;
}

static const struct rproc_ops sprd_modem_ops = {
	.load		= sprd_modem_load,
	.start		= sprd_modem_start,
	.stop		= sprd_modem_stop,
};

static int sprd_modem_parse_memory_region(struct sprd_modem *m)
{
	struct device_node *np;
	struct reserved_mem *rmem;

	np = of_parse_phandle(m->dev->of_node, "memory-region", 0);
	if (!np) {
		dev_err(m->dev, "no memory region specified\n");
		return -EINVAL;
	}

	rmem = of_reserved_mem_lookup(np);
	if (!rmem) {
		of_node_put(np);
		dev_err(m->dev, "failed to look up memory region\n");
		return -EINVAL;
	}

	m->mem_base = rmem->base;
	m->mem_size = rmem->size;

	of_node_put(np);

	return 0;
}

static int sprd_modem_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sprd_modem *m;
	struct rproc *rproc;
	const char *fw_name;
	int ret;

	ret = of_property_read_string_index(dev->of_node, "firmware-name",
					    0, &fw_name);
	if (ret < 0) {
		dev_err(&pdev->dev, "unable to read firmware-name\n");
		return ret;
	}

	rproc = devm_rproc_alloc(dev, dev->of_node->name, &sprd_modem_ops,
				 fw_name, sizeof(*m));
	if (!rproc)
		return -ENOMEM;

	platform_set_drvdata(pdev, rproc);

	m = rproc->priv;
	m->dev = dev;

	m->info = of_device_get_match_data(dev);
	if (!m->info)
		return -EINVAL;

	m->bootmem = devm_platform_get_and_ioremap_resource(pdev, 0, NULL);
	if (IS_ERR(m->bootmem))
		return PTR_ERR(m->bootmem);

	ret = sprd_modem_parse_memory_region(m);
	if (ret < 0)
		return ret;

	m->aon_apb_regs = syscon_regmap_lookup_by_phandle(dev->of_node, "sprd,syscon-aon-apb");
	if (IS_ERR(m->aon_apb_regs)) {
		dev_err(m->dev, "failed to get aon-apb syscon handle\n");
		return PTR_ERR(m->aon_apb_regs);
	}

	m->reset = devm_reset_control_get(dev, NULL);
	if (IS_ERR(m->reset)) {
		dev_err(m->dev, "failed to get modem reset\n");
		return PTR_ERR(m->reset);
	}

	device_set_wakeup_capable(dev, true);

	if (of_property_read_bool(dev->of_node, "wakeup-source"))
		device_wakeup_enable(dev);

	ret = devm_rproc_add(dev, rproc);
	if (ret < 0) {
		dev_err(dev, "failed to add rproc\n");
		return ret;
	}

	sprd_rproc_add_sipc_subdev(rproc, dev->of_node, &m->sipc);

	return 0;
}

static void sprd_modem_remove(struct platform_device *pdev)
{
	struct rproc *rproc = platform_get_drvdata(pdev);
	struct sprd_modem *m = rproc->priv;

	sprd_rproc_remove_sipc_subdev(rproc, &m->sipc);
}

static const struct sprd_modem_info ums9230_modem_info = {
	.corereset_reg = 0x0174,
	.corereset_mask = BIT(10),
};

static const struct of_device_id sprd_modem_of_match[] = {
	{ .compatible = "sprd,ums9230-modem", .data = &ums9230_modem_info },
	{ }
};
MODULE_DEVICE_TABLE(of, sprd_modem_of_match);

static int sprd_modem_suspend(struct device *dev)
{
	struct rproc *rproc = dev_get_drvdata(dev);

	if (device_may_wakeup(dev))
		return 0;

	if (rproc->state == RPROC_RUNNING)
		return sprd_modem_stop(rproc->priv);

	return 0;
}

static int sprd_modem_resume(struct device *dev)
{
	struct rproc *rproc = dev_get_drvdata(dev);

	if (device_may_wakeup(dev))
		return 0;

	if (rproc->state == RPROC_RUNNING)
		return sprd_modem_start(rproc->priv);

	return 0;
}

static struct dev_pm_ops sprd_modem_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(sprd_modem_suspend, sprd_modem_resume)
};

static struct platform_driver sprd_modem_driver = {
	.probe = sprd_modem_probe,
	.remove = sprd_modem_remove,
	.driver = {
		.name = "sprd-modem",
		.of_match_table = sprd_modem_of_match,
		.pm = &sprd_modem_pm_ops,
	},
};

module_platform_driver(sprd_modem_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Unisoc modem remoteproc driver");
