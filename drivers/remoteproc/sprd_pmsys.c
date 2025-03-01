// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 Otto Pflüger
 */

#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/remoteproc.h>
#include <linux/reset.h>

#include "sprd_common.h"

struct sprd_pmsys_info {
	u32 corereset_reg;
	u32 corereset_mask;
};

struct sprd_pmsys {
	struct device *dev;
	struct sprd_sipc_subdev sipc;
	struct reset_control *reset;
	struct regmap *aon_apb_regs;
	const struct sprd_pmsys_info *info;
	phys_addr_t mem_base;
	size_t mem_size;
	const char *sys_fw_name;
	void *bootmem;
	size_t bootmem_size;
};

static int sprd_pmsys_load(struct rproc *rproc, const struct firmware *fw)
{
	struct sprd_pmsys *p = rproc->priv;
	int ret = 0;
	void *mem;

	if (fw->size > p->bootmem_size) {
		dev_err(p->dev, "bootcode firmware too large\n");
		return -ENOMEM;
	}

	memcpy_toio(p->bootmem, fw->data, fw->size);

	mem = memremap(p->mem_base, p->mem_size, MEMREMAP_WC);
	if (!mem) {
		dev_err(p->dev, "failed to map memory region\n");
		return -EBUSY;
	}

	ret = request_firmware(&fw, p->sys_fw_name, p->dev);
	if (ret)
		goto out_unmap;

	if (fw->size > p->mem_size) {
		dev_err(p->dev, "pmsys firmware too large\n");
		ret = -ENOMEM;
		goto out_release;
	}

	memcpy(mem, fw->data, fw->size);

out_release:
	release_firmware(fw);
out_unmap:
	memunmap(mem);
	return ret;
}

static int sprd_pmsys_start(struct rproc *rproc)
{
	struct sprd_pmsys *p = rproc->priv;
	int ret;

	ret = reset_control_deassert(p->reset);
	if (ret < 0)
		return ret;

	/* start processor */
	regmap_clear_bits(p->aon_apb_regs, p->info->corereset_reg,
			  p->info->corereset_mask);

	return 0;
}

static int sprd_pmsys_stop(struct rproc *rproc)
{
	struct sprd_pmsys *p = rproc->priv;

	/* stop processor */
	regmap_set_bits(p->aon_apb_regs, p->info->corereset_reg,
			p->info->corereset_mask);

	reset_control_assert(p->reset);

	return 0;
}

static const struct rproc_ops sprd_pmsys_ops = {
	.load		= sprd_pmsys_load,
	.start		= sprd_pmsys_start,
	.stop		= sprd_pmsys_stop,
};

static int sprd_pmsys_parse_memory_region(struct sprd_pmsys *p)
{
	struct device_node *np;
	struct reserved_mem *rmem;

	np = of_parse_phandle(p->dev->of_node, "memory-region", 0);
	if (!np) {
		dev_err(p->dev, "no memory region specified\n");
		return -EINVAL;
	}

	rmem = of_reserved_mem_lookup(np);
	if (!rmem) {
		of_node_put(np);
		dev_err(p->dev, "failed to look up memory region\n");
		return -EINVAL;
	}

	p->mem_base = rmem->base;
	p->mem_size = rmem->size;

	of_node_put(np);

	return 0;
}

static int sprd_pmsys_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *bootmem;
	struct sprd_pmsys *p;
	struct rproc *rproc;
	const char *fw_name;
	int ret;

	ret = of_property_read_string_index(dev->of_node, "firmware-name",
					    0, &fw_name);
	if (ret < 0) {
		dev_err(&pdev->dev, "unable to read firmware-name\n");
		return ret;
	}

	rproc = devm_rproc_alloc(dev, dev->of_node->name, &sprd_pmsys_ops,
				 fw_name, sizeof(*p));
	if (!rproc)
		return -ENOMEM;

	platform_set_drvdata(pdev, rproc);

	p = rproc->priv;
	p->dev = dev;

	ret = of_property_read_string_index(dev->of_node, "firmware-name",
					    1, &p->sys_fw_name);
	if (ret < 0) {
		dev_err(p->dev, "unable to read second firmware-name\n");
		return ret;
	}

	p->info = of_device_get_match_data(dev);
	if (!p->info)
		return -EINVAL;

	p->bootmem = devm_platform_get_and_ioremap_resource(pdev, 0, &bootmem);
	if (IS_ERR(p->bootmem))
		return PTR_ERR(p->bootmem);

	p->bootmem_size = bootmem->end - bootmem->start + 1;

	ret = sprd_pmsys_parse_memory_region(p);
	if (ret < 0)
		return ret;

	p->aon_apb_regs = syscon_regmap_lookup_by_phandle(dev->of_node, "sprd,syscon-aon-apb");
	if (IS_ERR(p->aon_apb_regs)) {
		dev_err(p->dev, "failed to get aon-apb syscon handle\n");
		return PTR_ERR(p->aon_apb_regs);
	}

	p->reset = devm_reset_control_get_optional(dev, NULL);
	if (IS_ERR(p->reset)) {
		dev_err(p->dev, "failed to get pmsys reset\n");
		return PTR_ERR(p->reset);
	}

	ret = devm_rproc_add(dev, rproc);
	if (ret < 0) {
		dev_err(dev, "failed to add rproc\n");
		return ret;
	}

	sprd_rproc_add_sipc_subdev(rproc, dev->of_node, &p->sipc);

	return 0;
}

static void sprd_pmsys_remove(struct platform_device *pdev)
{
	struct rproc *rproc = platform_get_drvdata(pdev);
	struct sprd_pmsys *p = rproc->priv;

	sprd_rproc_remove_sipc_subdev(rproc, &p->sipc);
}

static const struct sprd_pmsys_info ums9230_pmsys_info = {
	.corereset_reg = 0x008c,
	.corereset_mask = BIT(0),
};

static const struct of_device_id sprd_pmsys_of_match[] = {
	{ .compatible = "sprd,ums9230-pmsys", .data = &ums9230_pmsys_info },
	{ }
};
MODULE_DEVICE_TABLE(of, sprd_pmsys_of_match);

static struct platform_driver sprd_pmsys_driver = {
	.probe = sprd_pmsys_probe,
	.remove = sprd_pmsys_remove,
	.driver = {
		.name = "sprd-pmsys",
		.of_match_table = sprd_pmsys_of_match,
	},
};

module_platform_driver(sprd_pmsys_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Unisoc PMSYS remoteproc driver");
