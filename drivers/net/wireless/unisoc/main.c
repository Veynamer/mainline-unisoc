// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Otto Pflüger
 */

#include <linux/firmware.h>
#include <linux/rtnetlink.h>

#include "sc23xx.h"

#include "cmd.h"
#include "txrx.h"
#include "vif.h"

#define SC23XX_CONFIG_MAGIC 0x4353	/* "SC" (little-endian) */

#define CHAN2G(_idx, _freq) { \
	.band = NL80211_BAND_2GHZ, \
	.center_freq = (_freq), \
	.hw_value = (_idx), \
	.max_power = 30, \
}

#define CHAN5G(_idx, _freq) { \
	.band = NL80211_BAND_5GHZ, \
	.center_freq = (_freq), \
	.hw_value = (_idx), \
	.max_power = 30, \
}

#define RATE(_bitrate) { \
	.bitrate = (_bitrate), \
	.hw_value = (_bitrate / 10), \
}

#define RATE_EXT(_idx, _bitrate) { \
	.bitrate = (_bitrate), \
	.hw_value = 0x80 + (_idx), \
}

static struct ieee80211_channel sc23xx_2ghz_channels[] = {
	CHAN2G(1, 2412), CHAN2G(2, 2417), CHAN2G(3, 2422),
	CHAN2G(4, 2427), CHAN2G(5, 2432), CHAN2G(6, 2437),
	CHAN2G(7, 2442), CHAN2G(8, 2447), CHAN2G(9, 2452),
	CHAN2G(10, 2457), CHAN2G(11, 2462), CHAN2G(12, 2467),
	CHAN2G(13, 2472), CHAN2G(14, 2484),
};

static struct ieee80211_rate sc23xx_rates[] = {
	RATE(10), RATE(20), RATE(55), RATE(110),
	RATE(60), RATE(90), RATE(120), RATE(180),
	RATE(240), RATE(360), RATE(480), RATE(540),

	RATE_EXT(0, 65), RATE_EXT(1, 130),
	RATE_EXT(2, 195), RATE_EXT(3, 260),
	RATE_EXT(4, 390), RATE_EXT(5, 520),
	RATE_EXT(6, 585), RATE_EXT(7, 650),

	RATE_EXT(8, 130), RATE_EXT(9, 260),
	RATE_EXT(10, 390), RATE_EXT(11, 520),
	RATE_EXT(12, 780), RATE_EXT(13, 1040),
	RATE_EXT(14, 1170), RATE_EXT(15, 1300),
};

static struct ieee80211_channel sc23xx_5ghz_channels[] = {
	CHAN5G(36, 5180), CHAN5G(40, 5200), CHAN5G(44, 5220),
	CHAN5G(48, 5240), CHAN5G(52, 5260), CHAN5G(56, 5280),
	CHAN5G(60, 5300), CHAN5G(64, 5320), CHAN5G(100, 5500),
	CHAN5G(104, 5520), CHAN5G(108, 5540), CHAN5G(112, 5560),
	CHAN5G(116, 5580), CHAN5G(120, 5600), CHAN5G(124, 5620),
	CHAN5G(128, 5640), CHAN5G(132, 5660), CHAN5G(136, 5680),
	CHAN5G(140, 5700), CHAN5G(144, 5720), CHAN5G(149, 5745),
	CHAN5G(153, 5765), CHAN5G(157, 5785), CHAN5G(161, 5805),
	CHAN5G(165, 5825),
};

static const u32 sc23xx_cipher_suites[] = {
	WLAN_CIPHER_SUITE_WEP40,
	WLAN_CIPHER_SUITE_WEP104,
	WLAN_CIPHER_SUITE_TKIP,
	WLAN_CIPHER_SUITE_CCMP,
	WLAN_CIPHER_SUITE_SMS4,
	WLAN_CIPHER_SUITE_AES_CMAC,
	WLAN_CIPHER_SUITE_GCMP_256,
	WLAN_CIPHER_SUITE_BIP_GMAC_256,
};

static const struct ieee80211_txrx_stypes sc23xx_mgmt_stypes[NUM_NL80211_IFTYPES] = {
	[NL80211_IFTYPE_STATION] = {
		.tx = 0xffff,
		.rx = BIT(IEEE80211_STYPE_ACTION >> 4) |
		BIT(IEEE80211_STYPE_PROBE_REQ >> 4)
	},
	[NL80211_IFTYPE_AP] = {
		.tx = 0xffff,
		.rx = BIT(IEEE80211_STYPE_ASSOC_REQ >> 4) |
		BIT(IEEE80211_STYPE_REASSOC_REQ >> 4) |
		BIT(IEEE80211_STYPE_PROBE_REQ >> 4) |
		BIT(IEEE80211_STYPE_DISASSOC >> 4) |
		BIT(IEEE80211_STYPE_AUTH >> 4) |
		BIT(IEEE80211_STYPE_DEAUTH >> 4) |
		BIT(IEEE80211_STYPE_ACTION >> 4)
	},
};

#ifdef CONFIG_PM

static const struct wiphy_wowlan_support sc23xx_wowlan_support = {
	.flags = WIPHY_WOWLAN_ANY,
};

static int sc23xx_set_suspend(struct sc23xx_dev *sdev, bool suspend)
{
	struct sc23xx_vif *vif;
	int ret = 0, srcu_idx, i;

	srcu_idx = srcu_read_lock(&sdev->vif_srcu);
	for (i = 0; i < SC23XX_VIF_NUM; i++) {
		vif = srcu_dereference(sdev->vif[i], &sdev->vif_srcu);
		if (vif && test_bit(SC23XX_FLAG_OPENED, &vif->flags)) {
			ret = sc23xx_cmd_set_suspend(vif, suspend);
			break;
		}
	}
	srcu_read_unlock(&sdev->vif_srcu, srcu_idx);

	return ret;
}

static int sc23xx_suspend(struct wiphy *wiphy, struct cfg80211_wowlan *wow)
{
	struct sc23xx_dev *sdev = wiphy_priv(wiphy);

	wiphy_dbg(wiphy, "WoWLAN requested: %s\n", wow ? "yes" : "no");

	return sc23xx_set_suspend(sdev, true);
}

static int sc23xx_resume(struct wiphy *wiphy)
{
	struct sc23xx_dev *sdev = wiphy_priv(wiphy);

	return sc23xx_set_suspend(sdev, false);
}

#endif /* CONFIG_PM */

static int sc23xx_scan(struct wiphy *wiphy, struct cfg80211_scan_request *request)
{
	struct sc23xx_vif *vif = netdev_priv(request->wdev->netdev);
	int ret;

	mutex_lock(&vif->scan_lock);
	if (vif->scan_req) {
		ret = -EBUSY;
		goto err_out;
	}

	vif->scan_req = request;
	mutex_unlock(&vif->scan_lock);

	if (request->ie_len > 0) {
		ret = sc23xx_cmd_set_probe_req_ie(vif, request->ie,
						  request->ie_len);
		if (ret)
			goto err_out;
	}

	ret = sc23xx_cmd_scan(vif, request);
	if (ret) {
		sc23xx_notify_scan_done(vif, true);
		return ret;
	}

	return 0;

err_out:
	mutex_unlock(&vif->scan_lock);
	return ret;
}

static void sc23xx_abort_scan(struct wiphy *wiphy, struct wireless_dev *wdev)
{
	struct sc23xx_vif *vif = netdev_priv(wdev->netdev);

	sc23xx_cmd_abort_scan(vif);
}

static int sc23xx_connect(struct wiphy *wiphy, struct net_device *netdev,
			  struct cfg80211_connect_params *sme)
{
	struct sc23xx_vif *vif = netdev_priv(netdev);

	return sc23xx_cmd_connect(vif, sme);
}

static int sc23xx_disconnect(struct wiphy *wiphy, struct net_device *netdev,
			     u16 reason_code)
{
	struct sc23xx_vif *vif = netdev_priv(netdev);

	return sc23xx_cmd_disconnect(vif, reason_code);
}

static int sc23xx_add_key(struct wiphy *wiphy, struct net_device *netdev, int link_id,
			  u8 key_index, bool pairwise, const u8 *mac_addr,
			  struct key_params *params)
{
	struct sc23xx_vif *vif = netdev_priv(netdev);

	return sc23xx_cmd_add_key(vif, key_index, pairwise, mac_addr, params);
}

static int sc23xx_del_key(struct wiphy *wiphy, struct net_device *netdev, int link_id,
			  u8 key_index, bool pairwise, const u8 *mac_addr)
{
	struct sc23xx_vif *vif = netdev_priv(netdev);

	if (!test_bit(SC23XX_FLAG_OPENED, &vif->flags))
		return 0;

	return sc23xx_cmd_del_key(vif, key_index, pairwise, mac_addr);
}

static int sc23xx_set_default_key(struct wiphy *wiphy, struct net_device *netdev,
				  int link_id, u8 key_index, bool unicast,
				  bool multicast)
{
	struct sc23xx_vif *vif = netdev_priv(netdev);

	return sc23xx_cmd_set_def_key(vif, key_index);
}

static struct wireless_dev *sc23xx_add_virtual_intf(struct wiphy *wiphy,
						    const char *name,
						    unsigned char name_assign_type,
						    enum nl80211_iftype type,
						    struct vif_params *params)
{
	struct sc23xx_dev *sdev = wiphy_priv(wiphy);
	struct sc23xx_vif *vif;

	vif = sc23xx_vif_new(sdev, name, type);
	if (IS_ERR(vif))
		return ERR_CAST(vif);

	return &vif->wdev;
}

static int sc23xx_del_virtual_intf(struct wiphy *wiphy, struct wireless_dev *wdev)
{
	struct sc23xx_vif *vif = netdev_priv(wdev->netdev);
	struct sc23xx_dev *sdev = wiphy_priv(wiphy);

	mutex_lock(&sdev->vif_lock);
	rcu_assign_pointer(sdev->vif[vif->idx], NULL);
	mutex_unlock(&sdev->vif_lock);
	synchronize_srcu(&sdev->vif_srcu);

	cfg80211_unregister_wdev(wdev);

	return 0;
}

static int sc23xx_change_virtual_intf(struct wiphy *wiphy, struct net_device *ndev,
				      enum nl80211_iftype type,
				      struct vif_params *params)
{
	struct sc23xx_vif *vif = netdev_priv(ndev);

	return sc23xx_vif_reopen(vif, type);
}

static int sc23xx_mgmt_tx(struct wiphy *wiphy, struct wireless_dev *wdev,
			  struct cfg80211_mgmt_tx_params *params, u64 *cookie)
{
	struct sc23xx_vif *vif = netdev_priv(wdev->netdev);

	return sc23xx_cmd_mgmt_tx(vif, params, cookie);
}

static int sc23xx_sched_scan_start(struct wiphy *wiphy, struct net_device *ndev,
				   struct cfg80211_sched_scan_request *request)
{
	struct sc23xx_vif *vif = netdev_priv(ndev);
	int ret;

	if (test_and_set_bit(SC23XX_FLAG_SCHED_SCAN, &vif->flags))
		return -EBUSY;

	ret = sc23xx_cmd_sched_scan_start(vif, request);
	if (ret) {
		clear_bit(SC23XX_FLAG_SCHED_SCAN, &vif->flags);
		return ret;
	}

	return 0;
}

static int sc23xx_sched_scan_stop(struct wiphy *wiphy, struct net_device *ndev,
				  u64 reqid)
{
	struct sc23xx_vif *vif = netdev_priv(ndev);
	int ret;

	ret = sc23xx_cmd_sched_scan_stop(vif);
	if (ret)
		return ret;

	clear_bit(SC23XX_FLAG_SCHED_SCAN, &vif->flags);

	return 0;
}

static int sc23xx_set_rekey_data(struct wiphy *wiphy, struct net_device *ndev,
				 struct cfg80211_gtk_rekey_data *data)
{
	struct sc23xx_vif *vif = netdev_priv(ndev);

	return sc23xx_cmd_set_rekey_data(vif, data);
}

static const struct cfg80211_ops sc23xx_ops = {
#ifdef CONFIG_PM
	.suspend = sc23xx_suspend,
	.resume = sc23xx_resume,
#endif
	.scan = sc23xx_scan,
	.abort_scan = sc23xx_abort_scan,
	.connect = sc23xx_connect,
	.disconnect = sc23xx_disconnect,
	.add_key = sc23xx_add_key,
	.del_key = sc23xx_del_key,
	.set_default_key = sc23xx_set_default_key,
	.add_virtual_intf = sc23xx_add_virtual_intf,
	.del_virtual_intf = sc23xx_del_virtual_intf,
	.change_virtual_intf = sc23xx_change_virtual_intf,
	.mgmt_tx = sc23xx_mgmt_tx,
	.sched_scan_start = sc23xx_sched_scan_start,
	.sched_scan_stop = sc23xx_sched_scan_stop,
	.set_rekey_data = sc23xx_set_rekey_data,
};

static void sc23xx_reg_notify(struct wiphy *wiphy,
			      struct regulatory_request *request)
{
	struct sc23xx_dev *sdev = wiphy_priv(wiphy);
	const struct ieee80211_reg_rule *reg_rules[39];
	unsigned int n_reg_rules = 0;
	enum nl80211_band band;
	int i;

	wiphy_dbg(wiphy, "Updating regulatory domain: %c%c\n",
		  request->alpha2[0], request->alpha2[1]);

	for (band = 0; band < NUM_NL80211_BANDS &&
		       n_reg_rules < ARRAY_SIZE(reg_rules); band++) {
		if (!wiphy->bands[band])
			continue;

		for (i = 0; i < wiphy->bands[band]->n_channels &&
			    n_reg_rules < ARRAY_SIZE(reg_rules); i++) {
			const struct ieee80211_reg_rule *reg_rule;
			struct ieee80211_channel *ch;

			ch = &wiphy->bands[band]->channels[i];

			reg_rule = freq_reg_info(wiphy,
						 MHZ_TO_KHZ(ch->center_freq));
			if (IS_ERR(reg_rule))
				continue;

			reg_rules[n_reg_rules++] = reg_rule;
		}
	}

	sc23xx_cmd_set_regdom(sdev, request->alpha2, reg_rules, n_reg_rules);
}

void *sc23xx_alloc_device(struct device *dev, size_t size,
			  const struct sc23xx_bus_ops *bus_ops)
{
	struct sc23xx_dev *sdev;
	struct wiphy *wiphy;
	int i;

	wiphy = wiphy_new(&sc23xx_ops, size);
	if (!wiphy) {
		dev_err(dev, "failed to allocate wiphy\n");
		return ERR_PTR(-ENOMEM);
	}

	set_wiphy_dev(wiphy, dev);

	sdev = wiphy_priv(wiphy);
	sdev->wiphy = wiphy;
	sdev->bus_ops = bus_ops;
	device_get_mac_address(dev, sdev->mac_addr);

	sdev->evt_wq = alloc_ordered_workqueue("%s-events", 0,
					       wiphy_name(wiphy));
	if (!sdev->evt_wq) {
		wiphy_err(sdev->wiphy, "failed to allocate event workqueue\n");
		wiphy_free(sdev->wiphy);
		return ERR_PTR(-ENOMEM);
	}

	mutex_init(&sdev->cmd_lock);
	mutex_init(&sdev->vif_lock);
	init_srcu_struct(&sdev->vif_srcu);
	spin_lock_init(&sdev->sta_lock);
	spin_lock_init(&sdev->tx_queue_lock);
	spin_lock_init(&sdev->tx_buf_lock);
	spin_lock_init(&sdev->rx_buf_lock);
	init_waitqueue_head(&sdev->rsp_wait);
	init_waitqueue_head(&sdev->tx_wait);
	skb_queue_head_init(&sdev->rsp_q);
	INIT_LIST_HEAD(&sdev->tx_queue);
	INIT_LIST_HEAD(&sdev->tx_buf_cache);
	hash_init(sdev->tx_buf_table);
	hash_init(sdev->rx_buf_table);

	for (i = 0; i < SC23XX_STA_IDX_NUM; i++) {
		sdev->sta[i].sdev = sdev;
		INIT_DELAYED_WORK(&sdev->sta[i].tx_ba_setup,
				  sc23xx_cmd_tx_addba_req);
	}

	wiphy->features |= NL80211_FEATURE_SAE;
	wiphy->flags |= WIPHY_FLAG_HAS_REMAIN_ON_CHANNEL;
	wiphy->max_remain_on_channel_duration = 5000;
	wiphy->max_num_pmkids = SC23XX_MAX_NUM_PMKIDS;
	wiphy->max_scan_ssids = SC23XX_MAX_SCAN_SSIDS;
	wiphy->max_scan_ie_len = SC23XX_MAX_SCAN_IE_LEN;
	wiphy->cipher_suites = sc23xx_cipher_suites;
	wiphy->n_cipher_suites = ARRAY_SIZE(sc23xx_cipher_suites);
	wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;
	wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION) | BIT(NL80211_IFTYPE_AP);
	wiphy->mgmt_stypes = sc23xx_mgmt_stypes;
#ifdef CONFIG_PM
	wiphy->wowlan = &sc23xx_wowlan_support;
#endif
	wiphy->reg_notifier = sc23xx_reg_notify;

	sdev->band_2ghz.channels = sc23xx_2ghz_channels;
	sdev->band_2ghz.n_channels = ARRAY_SIZE(sc23xx_2ghz_channels);
	sdev->band_2ghz.bitrates = sc23xx_rates;
	sdev->band_2ghz.n_bitrates = ARRAY_SIZE(sc23xx_rates);
	sdev->band_2ghz.ht_cap.ht_supported = true;
	sdev->band_2ghz.ht_cap.cap =
		IEEE80211_HT_CAP_SUP_WIDTH_20_40 |
		IEEE80211_HT_CAP_SGI_20 |
		IEEE80211_HT_CAP_SGI_40;

	sdev->band_5ghz.channels = sc23xx_5ghz_channels;
	sdev->band_5ghz.n_channels = ARRAY_SIZE(sc23xx_5ghz_channels);
	sdev->band_5ghz.bitrates = sc23xx_rates + 4;
	sdev->band_5ghz.n_bitrates = ARRAY_SIZE(sc23xx_rates) - 4;
	sdev->band_5ghz.ht_cap.ht_supported = true;
	sdev->band_5ghz.ht_cap.cap =
		IEEE80211_HT_CAP_SUP_WIDTH_20_40 |
		IEEE80211_HT_CAP_SM_PS |
		IEEE80211_HT_CAP_SGI_20 |
		IEEE80211_HT_CAP_SGI_40;
	sdev->band_5ghz.vht_cap.vht_supported = true;
	sdev->band_5ghz.vht_cap.cap =
		IEEE80211_VHT_CAP_MAX_MPDU_LENGTH_7991 |
		IEEE80211_VHT_CAP_SU_BEAMFORMEE_CAPABLE |
		IEEE80211_VHT_CAP_BEAMFORMEE_STS_SHIFT |
		IEEE80211_VHT_CAP_MU_BEAMFORMEE_CAPABLE |
		IEEE80211_VHT_CAP_MAX_A_MPDU_LENGTH_EXPONENT_SHIFT |
		IEEE80211_VHT_CAP_VHT_TXOP_PS;
	sdev->band_5ghz.vht_cap.vht_mcs.rx_mcs_map = 0xfff0;
	sdev->band_5ghz.vht_cap.vht_mcs.tx_mcs_map = 0xfff0;

	return sdev;
}
EXPORT_SYMBOL_GPL(sc23xx_alloc_device);

void sc23xx_free_device(struct sc23xx_dev *sdev)
{
	destroy_workqueue(sdev->evt_wq);
	mutex_destroy(&sdev->cmd_lock);
	mutex_destroy(&sdev->vif_lock);
	cleanup_srcu_struct(&sdev->vif_srcu);
	wiphy_free(sdev->wiphy);
}
EXPORT_SYMBOL_GPL(sc23xx_free_device);

int sc23xx_load_config(struct sc23xx_dev *sdev, const char *fw_name)
{
	const struct firmware *fw;
	const u8 *data;
	size_t len;
	int ret;
	u32 i = 1;

	ret = request_firmware(&fw, fw_name, wiphy_dev(sdev->wiphy));
	if (ret)
		return ret;

	data = fw->data;
	len = fw->size;

	while (len) {
		u16 section_size;

		if (len < 4) {
			wiphy_err(sdev->wiphy, "config section header is incomplete\n");
			ret = -EINVAL;
			break;
		}

		if (get_unaligned_le16(data) != SC23XX_CONFIG_MAGIC) {
			wiphy_err(sdev->wiphy, "config header has invalid magic\n");
			ret = -EINVAL;
			break;
		}

		section_size = get_unaligned_le16(data + 2);
		data += 4;
		len -= 4;

		if (section_size == 0) {
			i++;
			continue;
		}

		if (len < section_size) {
			wiphy_err(sdev->wiphy, "config section %d incomplete\n", i);
			ret = -EINVAL;
			break;
		}

		wiphy_dbg(sdev->wiphy, "downloading section %d (%d bytes)\n", i, section_size);

		ret = sc23xx_download_config_section(sdev, i, data, section_size);
		if (ret) {
			wiphy_err(sdev->wiphy, "failed to download config section %d\n", i);
			break;
		}

		data += section_size;
		len -= section_size;
		i++;
	}

	release_firmware(fw);

	return ret;
}
EXPORT_SYMBOL_GPL(sc23xx_load_config);

int sc23xx_register_device(struct sc23xx_dev *sdev)
{
	struct sc23xx_vif *vif;
	int ret;

	ret = sc23xx_get_fw_info(sdev);
	if (ret) {
		wiphy_err(sdev->wiphy, "failed to get firmware info\n");
		return ret;
	}

	ret = wiphy_register(sdev->wiphy);
	if (ret) {
		wiphy_err(sdev->wiphy, "failed to register wiphy\n");
		return ret;
	}

	rtnl_lock();
	vif = sc23xx_vif_new(sdev, "wlan%d", NL80211_IFTYPE_STATION);
	rtnl_unlock();
	if (IS_ERR(vif)) {
		wiphy_err(sdev->wiphy, "failed to create initial interface\n");
		wiphy_unregister(sdev->wiphy);
		return PTR_ERR(vif);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(sc23xx_register_device);

void sc23xx_unregister_device(struct sc23xx_dev *sdev)
{
	struct sc23xx_vif *vif;
	struct sc23xx_sta *sta;
	int i, tid;

	rtnl_lock();
	for (i = 0; i < SC23XX_VIF_NUM; i++) {
		mutex_lock(&sdev->vif_lock);
		vif = rcu_replace_pointer(sdev->vif[i], NULL,
					  lockdep_is_held(&sdev->vif_lock));
		mutex_unlock(&sdev->vif_lock);
		if (vif) {
			synchronize_srcu(&sdev->vif_srcu);
			cfg80211_unregister_wdev(&vif->wdev);
		}
	}
	rtnl_unlock();

	drain_workqueue(sdev->evt_wq);

	spin_lock_irq(&sdev->sta_lock);
	for (i = 0; i < SC23XX_STA_IDX_NUM; i++) {
		sta = &sdev->sta[i];
		for (tid = 0; tid < SC23XX_MAX_TID_NUM; tid++) {
			kfree(sta->r[tid].buf);
			sta->r[tid].buf = NULL;
			timer_shutdown(&sta->r[tid].timer);
		}
	}
	spin_unlock_irq(&sdev->sta_lock);

	wiphy_unregister(sdev->wiphy);
	sc23xx_free_txrx_buffers(sdev);
}
EXPORT_SYMBOL_GPL(sc23xx_unregister_device);

MODULE_AUTHOR("Otto Pflüger");
MODULE_DESCRIPTION("Unisoc SC23xx wireless driver");
MODULE_LICENSE("GPL");
