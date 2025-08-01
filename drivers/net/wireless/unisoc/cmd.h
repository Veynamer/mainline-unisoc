// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Otto Pflüger
 */

#ifndef __SC23XX_CMD_H
#define __SC23XX_CMD_H

#include "sc23xx.h"

int sc23xx_cmd_open(struct sc23xx_vif *vif, const u8 *mac_addr);
int sc23xx_cmd_close(struct sc23xx_vif *vif);
int sc23xx_cmd_set_suspend(struct sc23xx_vif *vif, bool suspend);
int sc23xx_cmd_connect(struct sc23xx_vif *vif, struct cfg80211_connect_params *sme);
int sc23xx_cmd_disconnect(struct sc23xx_vif *vif, u16 reason_code);
int sc23xx_cmd_add_key(struct sc23xx_vif *vif, u8 key_index, bool pairwise,
		       const u8 *mac_addr, struct key_params *params);
int sc23xx_cmd_del_key(struct sc23xx_vif *vif, u8 key_index, bool pairwise,
		       const u8 *mac_addr);
int sc23xx_cmd_set_def_key(struct sc23xx_vif *vif, u8 key_index);
int sc23xx_cmd_set_probe_req_ie(struct sc23xx_vif *vif, const u8 *ie, u16 ie_len);
int sc23xx_cmd_scan(struct sc23xx_vif *vif,
		    struct cfg80211_scan_request *request);
int sc23xx_cmd_abort_scan(struct sc23xx_vif *vif);
int sc23xx_cmd_sched_scan_start(struct sc23xx_vif *vif,
				struct cfg80211_sched_scan_request *request);
int sc23xx_cmd_sched_scan_stop(struct sc23xx_vif *vif);
int sc23xx_cmd_set_rekey_data(struct sc23xx_vif *vif,
			      struct cfg80211_gtk_rekey_data *data);
int sc23xx_cmd_mgmt_tx(struct sc23xx_vif *vif, struct cfg80211_mgmt_tx_params *params,
		       u64 *cookie);
int sc23xx_cmd_set_mac_addr(struct sc23xx_vif *vif, const u8 *mac_addr);
int sc23xx_cmd_addba_rsp(struct sc23xx_dev *sdev, struct sc23xx_sta *sta,
			 u8 tid);
int sc23xx_cmd_set_regdom(struct sc23xx_dev *sdev, char *alpha2,
			  const struct ieee80211_reg_rule **rules,
			  unsigned int n_rules);
int sc23xx_download_config_section(struct sc23xx_dev *sdev, u32 section,
				   const void *data, u16 size);
int sc23xx_get_fw_info(struct sc23xx_dev *sdev);

struct sc23xx_set_multicast_cmd {
	struct work_struct work;
	struct sc23xx_vif *vif;
	u8 count;
	u8 addr_list[];
};

void sc23xx_cmd_set_multicast(struct work_struct *work);
void sc23xx_cmd_tx_addba_req(struct work_struct *work);

struct sc23xx_event {
	struct work_struct work;
	struct sc23xx_dev *sdev;
	struct sk_buff *skb;
};

void sc23xx_handle_event(struct work_struct *work);

#endif
