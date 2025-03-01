// SPDX-License-Identifier: GPL-2.0-only

#ifndef __SOC_SPRD_UMS9230_PM_DOMAINS_H
#define __SOC_SPRD_UMS9230_PM_DOMAINS_H

#include "pm-domains.h"
#include <dt-bindings/power/sprd,ums9230-power.h>

static const struct sprd_pmu_domain_data sprd_pmu_domain_data_ums9230[] = {
	[UMS9230_POWER_DOMAIN_GPU_TOP] = {
		.name = "gpu_top",
		.caps = SPRD_PD_SHUTDOWN,
		.pd_cfg_offs = 0x02c4,
	},
	[UMS9230_POWER_DOMAIN_MM] = {
		.name = "mm",
		.caps = SPRD_PD_SHUTDOWN,
		.pd_cfg_offs = 0x02e8,
	},
	[UMS9230_POWER_DOMAIN_PUBCP] = {
		.name = "pubcp",
		.caps = SPRD_PD_SHUTDOWN | SPRD_PD_ACTIVE_WAKEUP,
		.pd_cfg_offs = 0x0330,
		.force_deep_sleep_offs = 0x0818,
		.force_deep_sleep_mask = BIT(1),
	},
	[UMS9230_POWER_DOMAIN_WCN] = {
		.name = "wcn",
		.caps = SPRD_PD_SHUTDOWN | SPRD_PD_ACTIVE_WAKEUP,
		.pd_cfg_offs = 0x03a8,
		.force_deep_sleep_offs = 0x0818,
		.force_deep_sleep_mask = BIT(7),
	},
};

static const struct sprd_pmu_soc_data ums9230_pmu_data = {
	.domains_data = sprd_pmu_domain_data_ums9230,
	.num_domains = ARRAY_SIZE(sprd_pmu_domain_data_ums9230),
};

#endif
