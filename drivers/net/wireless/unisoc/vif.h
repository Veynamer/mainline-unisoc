// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Otto Pflüger
 */

#ifndef __SC23XX_VIF_H
#define __SC23XX_VIF_H

#include "sc23xx.h"

enum sc23xx_vif_mode {
	SC23XX_MODE_NONE = 0,
	SC23XX_MODE_STATION = 1,
	SC23XX_MODE_AP = 2,
	SC23XX_MODE_P2P_DEVICE = 4,
	SC23XX_MODE_P2P_CLIENT = 5,
	SC23XX_MODE_P2P_GO = 6,
	SC23XX_MODE_MONITOR = 7,
	SC23XX_MODE_STATION_SECOND = 10,
};

enum sc23xx_vif_flags {
	SC23XX_FLAG_OPENED = 0,
	SC23XX_FLAG_USE_ALT_MAC = 1,
	SC23XX_FLAG_SCHED_SCAN = 2,
};

enum sc23xx_connect_state {
	SC23XX_STATE_DISCONNECTED,
	SC23XX_STATE_CONNECTING,
	SC23XX_STATE_CONNECTED,
};

struct sc23xx_vif {
	struct wireless_dev wdev;
	struct sc23xx_dev *sdev;

	enum sc23xx_vif_mode mode;
	unsigned long flags;
	u8 idx;

	struct cfg80211_scan_request *scan_req;
	struct mutex scan_lock;

	enum sc23xx_connect_state state;
	u8 bssid[ETH_ALEN];
};

struct sc23xx_vif *sc23xx_vif_new(struct sc23xx_dev *sdev, const char *name,
				  enum nl80211_iftype iftype);
int sc23xx_vif_reopen(struct sc23xx_vif *vif, enum nl80211_iftype iftype);
void sc23xx_notify_scan_done(struct sc23xx_vif *vif, bool aborted);
struct sc23xx_vif *sc23xx_get_vif(struct sc23xx_dev *sdev, int idx);

#endif
