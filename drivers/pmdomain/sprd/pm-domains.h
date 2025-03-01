// SPDX-License-Identifier: GPL-2.0-only

#ifndef __SOC_SPRD_PM_DOMAINS_H
#define __SOC_SPRD_PM_DOMAINS_H

#define SPRD_PD_ALWAYS_ON		BIT(0)
#define SPRD_PD_SHUTDOWN		BIT(1)
#define SPRD_PD_ACTIVE_WAKEUP		BIT(2)
#define SPRD_PD_CAPS(_pd, _x)		((_pd)->data->caps & (_x))

#define SPRD_PMU_FORCE_SHUTDOWN_MASK	GENMASK(25, 24)

struct sprd_pmu_domain_data {
	const char *name;
	u16 caps;
	u32 pd_cfg_offs;
	u32 force_deep_sleep_offs;
	u32 force_deep_sleep_mask;
};

struct sprd_pmu_soc_data {
	const struct sprd_pmu_domain_data *domains_data;
	int num_domains;
};

#endif
