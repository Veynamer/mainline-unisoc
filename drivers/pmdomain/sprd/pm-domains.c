// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Otto Pflüger
 *
 * Based on mtk-pm-domains.c
 * Copyright (c) 2020 Collabora Ltd.
 */
#include <linux/init.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regmap.h>

#include "ums9230-pm-domains.h"

struct sprd_pmu_domain {
	struct generic_pm_domain genpd;
	const struct sprd_pmu_domain_data *data;
	struct sprd_pmu *pmu;
};

struct sprd_pmu {
	struct device *dev;
	struct regmap *base;
	const struct sprd_pmu_soc_data *soc_data;
	struct genpd_onecell_data pd_data;
	struct generic_pm_domain *domains[];
};

#define to_sprd_pmu_domain(gpd) container_of(gpd, struct sprd_pmu_domain, genpd)

static int sprd_pmu_power_on(struct generic_pm_domain *genpd)
{
	struct sprd_pmu_domain *pd = to_sprd_pmu_domain(genpd);
	struct sprd_pmu *pmu = pd->pmu;

	if (SPRD_PD_CAPS(pd, SPRD_PD_SHUTDOWN))
		regmap_write(pmu->base, pd->data->pd_cfg_offs + 0x2000,
			     SPRD_PMU_FORCE_SHUTDOWN_MASK);

	if (pd->data->force_deep_sleep_offs)
		regmap_write(pmu->base, pd->data->force_deep_sleep_offs + 0x2000,
			     pd->data->force_deep_sleep_mask);

	msleep(50);

	return 0;
}

static int sprd_pmu_power_off(struct generic_pm_domain *genpd)
{
	struct sprd_pmu_domain *pd = to_sprd_pmu_domain(genpd);
	struct sprd_pmu *pmu = pd->pmu;

	if (pd->data->force_deep_sleep_offs)
		regmap_write(pmu->base, pd->data->force_deep_sleep_offs + 0x1000,
			     pd->data->force_deep_sleep_mask);

	if (SPRD_PD_CAPS(pd, SPRD_PD_SHUTDOWN))
		regmap_write(pmu->base, pd->data->pd_cfg_offs + 0x1000,
			     SPRD_PMU_FORCE_SHUTDOWN_MASK);

	msleep(50);

	return 0;
}

static struct
generic_pm_domain *sprd_pmu_add_one_domain(struct sprd_pmu *pmu, struct device_node *node)
{
	const struct sprd_pmu_domain_data *domain_data;
	struct sprd_pmu_domain *pd;
	int ret;
	u32 id;

	ret = of_property_read_u32(node, "reg", &id);
	if (ret) {
		dev_err(pmu->dev, "%pOF: failed to retrieve domain id from reg: %d\n",
			node, ret);
		return ERR_PTR(-EINVAL);
	}

	if (id >= pmu->soc_data->num_domains) {
		dev_err(pmu->dev, "%pOF: invalid domain id %d\n", node, id);
		return ERR_PTR(-EINVAL);
	}

	domain_data = &pmu->soc_data->domains_data[id];
	if (!domain_data->name) {
		dev_err(pmu->dev, "%pOF: undefined domain id %d\n", node, id);
		return ERR_PTR(-EINVAL);
	}

	pd = devm_kzalloc(pmu->dev, sizeof(*pd), GFP_KERNEL);
	if (!pd)
		return ERR_PTR(-ENOMEM);

	pd->data = domain_data;
	pd->pmu = pmu;

	if (pmu->domains[id]) {
		dev_err(pmu->dev,
			"power domain with id %d already exists, check your device-tree\n", id);
		return ERR_PTR(-EINVAL);
	}

	pd->genpd.name = pd->data->name;

	pd->genpd.power_off = sprd_pmu_power_off;
	pd->genpd.power_on = sprd_pmu_power_on;

	/*
	 * Initially turn on all domains to make the domains usable
	 * with !CONFIG_PM and to get the hardware in sync with the
	 * software.  The unused domains will be switched off during
	 * late_init time.
	 */
	ret = sprd_pmu_power_on(&pd->genpd);
	if (ret < 0) {
		dev_err(pmu->dev, "%pOF: failed to power on domain: %d\n",
			node, ret);
		return ERR_PTR(ret);
	}

	if (SPRD_PD_CAPS(pd, SPRD_PD_ALWAYS_ON))
		pd->genpd.flags |= GENPD_FLAG_ALWAYS_ON;
	if (SPRD_PD_CAPS(pd, SPRD_PD_ACTIVE_WAKEUP))
		pd->genpd.flags |= GENPD_FLAG_ACTIVE_WAKEUP;

	pm_genpd_init(&pd->genpd, NULL, true);

	pmu->domains[id] = &pd->genpd;

	return pmu->pd_data.domains[id];
}

static void sprd_pmu_remove_one_domain(struct sprd_pmu_domain *pd)
{
	int ret;

	/*
	 * We're in the error cleanup already, so we only complain,
	 * but won't emit another error on top of the original one.
	 */
	ret = pm_genpd_remove(&pd->genpd);
	if (ret < 0)
		dev_err(pd->pmu->dev,
			"failed to remove domain '%s' : %d - state may be inconsistent\n",
			pd->genpd.name, ret);
}

static void sprd_pmu_domain_cleanup(struct sprd_pmu *pmu)
{
	struct generic_pm_domain *genpd;
	struct sprd_pmu_domain *pd;
	int i;

	for (i = pmu->pd_data.num_domains - 1; i >= 0; i--) {
		genpd = pmu->pd_data.domains[i];
		if (genpd) {
			pd = to_sprd_pmu_domain(genpd);
			sprd_pmu_remove_one_domain(pd);
		}
	}
}

static const struct of_device_id sprd_pmu_of_match[] = {
	{
		.compatible = "sprd,ums9230-power-controller",
		.data = &ums9230_pmu_data,
	},
	{ }
};

static int sprd_pmu_add_subdomain(struct sprd_pmu *pmu, struct device_node *parent)
{
	struct generic_pm_domain *child_pd, *parent_pd;
	struct device_node *child;
	int ret;

	for_each_child_of_node(parent, child) {
		u32 id;

		ret = of_property_read_u32(parent, "reg", &id);
		if (ret) {
			dev_err(pmu->dev, "%pOF: failed to get parent domain id\n", child);
			goto err_put_node;
		}

		if (!pmu->pd_data.domains[id]) {
			ret = -EINVAL;
			dev_err(pmu->dev, "power domain with id %d does not exist\n", id);
			goto err_put_node;
		}

		parent_pd = pmu->pd_data.domains[id];

		child_pd = sprd_pmu_add_one_domain(pmu, child);
		if (IS_ERR(child_pd)) {
			ret = PTR_ERR(child_pd);
			dev_err_probe(pmu->dev, ret, "%pOF: failed to get child domain id\n",
				      child);
			goto err_put_node;
		}

		/* recursive call to add all subdomains */
		ret = sprd_pmu_add_subdomain(pmu, child);
		if (ret)
			goto err_put_node;

		ret = pm_genpd_add_subdomain(parent_pd, child_pd);
		if (ret) {
			dev_err(pmu->dev, "failed to add %s subdomain to parent %s\n",
				child_pd->name, parent_pd->name);
			goto err_put_node;
		} else {
			dev_dbg(pmu->dev, "%s add subdomain: %s\n", parent_pd->name,
				child_pd->name);
		}
	}

	return 0;

err_put_node:
	of_node_put(child);
	return ret;
}

static int sprd_pmu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	const struct sprd_pmu_soc_data *soc;
	struct device_node *node;
	struct device *parent;
	struct sprd_pmu *pmu;
	int ret;

	soc = of_device_get_match_data(&pdev->dev);
	if (!soc) {
		dev_err(&pdev->dev, "no power controller data\n");
		return -EINVAL;
	}

	pmu = devm_kzalloc(dev, struct_size(pmu, domains, soc->num_domains), GFP_KERNEL);
	if (!pmu)
		return -ENOMEM;

	pmu->dev = dev;
	pmu->soc_data = soc;

	pmu->pd_data.domains = pmu->domains;
	pmu->pd_data.num_domains = soc->num_domains;

	parent = dev->parent;
	if (!parent) {
		dev_err(dev, "no parent for syscon devices\n");
		return -ENODEV;
	}

	pmu->base = syscon_node_to_regmap(parent->of_node);
	if (IS_ERR(pmu->base)) {
		dev_err(dev, "no regmap available\n");
		return PTR_ERR(pmu->base);
	}

	ret = -ENODEV;
	for_each_available_child_of_node(np, node) {
		struct generic_pm_domain *domain;

		domain = sprd_pmu_add_one_domain(pmu, node);
		if (IS_ERR(domain)) {
			ret = PTR_ERR(domain);
			of_node_put(node);
			goto err_cleanup_domains;
		}

		ret = sprd_pmu_add_subdomain(pmu, node);
		if (ret) {
			of_node_put(node);
			goto err_cleanup_domains;
		}
	}

	if (ret) {
		dev_dbg(dev, "no power domains present\n");
		return ret;
	}

	ret = of_genpd_add_provider_onecell(np, &pmu->pd_data);
	if (ret) {
		dev_err(dev, "failed to add provider: %d\n", ret);
		goto err_cleanup_domains;
	}

	return 0;

err_cleanup_domains:
	sprd_pmu_domain_cleanup(pmu);
	return ret;
}

static struct platform_driver sprd_pm_domain_driver = {
	.probe = sprd_pmu_probe,
	.driver = {
		.name = "sprd-power-controller",
		.suppress_bind_attrs = true,
		.of_match_table = sprd_pmu_of_match,
	},
};
builtin_platform_driver(sprd_pm_domain_driver);
