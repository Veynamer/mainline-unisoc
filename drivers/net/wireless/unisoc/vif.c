// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Otto Pflüger
 */

#include "cmd.h"
#include "txrx.h"
#include "vif.h"

static int sc23xx_iftype_to_mode(enum nl80211_iftype iftype,
				 enum sc23xx_vif_mode *mode,
				 u8 *idx)
{
	switch (iftype) {
	case NL80211_IFTYPE_STATION:
		*mode = SC23XX_MODE_STATION;
		*idx = SC23XX_VIF_STA;
		break;
	case NL80211_IFTYPE_AP:
		*mode = SC23XX_MODE_AP;
		*idx = SC23XX_VIF_AP;
		break;
	case NL80211_IFTYPE_P2P_DEVICE:
		*mode = SC23XX_MODE_P2P_DEVICE;
		*idx = SC23XX_VIF_P2P_DEVICE;
		break;
	case NL80211_IFTYPE_P2P_CLIENT:
		*mode = SC23XX_MODE_P2P_CLIENT;
		*idx = SC23XX_VIF_AP;
		break;
	case NL80211_IFTYPE_P2P_GO:
		*mode = SC23XX_MODE_P2P_GO;
		*idx = SC23XX_VIF_AP;
		break;
	case NL80211_IFTYPE_MONITOR:
		*mode = SC23XX_MODE_MONITOR;
		*idx = SC23XX_VIF_STA;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const char *sc23xx_vif_idx_to_string(u8 idx)
{
	switch (idx) {
	case SC23XX_VIF_STA:
		return "STA";
	case SC23XX_VIF_AP:
		return "AP, P2P or 2nd STA";
	case SC23XX_VIF_P2P_DEVICE:
		return "P2P device";
	default:
		break;
	}

	return "unknown type";
}

static int sc23xx_open(struct net_device *ndev)
{
	struct sc23xx_vif *vif = netdev_priv(ndev);
	int ret;

	ret = sc23xx_cmd_open(vif, ndev->dev_addr);
	if (ret)
		return ret;

	spin_lock_irq(&vif->sdev->tx_buf_lock);
	if (vif->sdev->tx_blocked)
		netif_stop_queue(ndev);
	else
		netif_start_queue(ndev);
	set_bit(SC23XX_FLAG_OPENED, &vif->flags);
	spin_unlock_irq(&vif->sdev->tx_buf_lock);

	atomic_inc(&vif->sdev->open_count);
	wake_up(&vif->sdev->tx_wait);

	return 0;
}

static int sc23xx_close(struct net_device *ndev)
{
	struct sc23xx_vif *vif = netdev_priv(ndev);
	int ret;

	flush_workqueue(vif->sdev->evt_wq);

	ret = sc23xx_cmd_close(vif);
	if (ret)
		return ret;

	clear_bit(SC23XX_FLAG_OPENED, &vif->flags);
	if (atomic_dec_return(&vif->sdev->open_count) == 0)
		sc23xx_free_txrx_buffers(vif->sdev);

	return 0;
}

static void sc23xx_set_multicast(struct net_device *ndev)
{
	struct sc23xx_vif *vif = netdev_priv(ndev);
	struct sc23xx_set_multicast_cmd *cmd;
	struct netdev_hw_addr *ha;
	u8 *addr;

	if (!vif->sdev->max_mc_mac_addrs ||
	    !test_bit(SC23XX_FLAG_OPENED, &vif->flags))
		return;

	if (ndev->mc.count > vif->sdev->max_mc_mac_addrs) {
		netdev_err(ndev, "too many multicast addresses set\n");
		return;
	}

	cmd = kmalloc(sizeof(*cmd) + ndev->mc.count * ETH_ALEN, GFP_ATOMIC);
	if (!cmd)
		return;

	INIT_WORK(&cmd->work, sc23xx_cmd_set_multicast);
	cmd->vif = vif;
	cmd->count = ndev->mc.count;

	addr = cmd->addr_list;
	netdev_for_each_mc_addr(ha, ndev) {
		memcpy(addr, ha->addr, ETH_ALEN);
		addr += ETH_ALEN;
	}

	queue_work(vif->sdev->evt_wq, &cmd->work);
}

static int sc23xx_set_mac_addr(struct net_device *ndev, void *addr)
{
	struct sc23xx_vif *vif = netdev_priv(ndev);
	struct sockaddr *sa = addr;
	int ret;

	if (test_bit(SC23XX_FLAG_OPENED, &vif->flags)) {
		ret = sc23xx_cmd_set_mac_addr(vif, sa->sa_data);
		if (ret)
			return ret;
	}

	eth_commit_mac_addr_change(ndev, addr);

	return 0;
}

static netdev_tx_t sc23xx_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct sc23xx_vif *vif = netdev_priv(ndev);

	if (vif->state != SC23XX_STATE_CONNECTED) {
		dev_kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += skb->len;

	sc23xx_tx_prepare(vif, skb);
	sc23xx_tx_data(vif->sdev, skb);

	return NETDEV_TX_OK;
}

static const struct net_device_ops sc23xx_netdev_ops = {
	.ndo_open = sc23xx_open,
	.ndo_stop = sc23xx_close,
	.ndo_start_xmit = sc23xx_start_xmit,
	.ndo_set_mac_address = sc23xx_set_mac_addr,
	.ndo_set_rx_mode = sc23xx_set_multicast,
};

struct sc23xx_vif *sc23xx_vif_new(struct sc23xx_dev *sdev, const char *name,
				  enum nl80211_iftype iftype)
{
	struct wireless_dev *wdev;
	struct net_device *ndev;
	struct sc23xx_vif *vif;
	u8 mac_addr[ETH_ALEN];
	int i, ret;

	ndev = alloc_netdev(sizeof(*vif), name, NET_NAME_UNKNOWN, ether_setup);
	if (!ndev)
		return ERR_PTR(-ENOMEM);

	vif = netdev_priv(ndev);
	mutex_init(&vif->scan_lock);
	vif->sdev = sdev;

	ret = sc23xx_iftype_to_mode(iftype, &vif->mode, &vif->idx);
	if (ret)
		goto err_free_netdev;

	mutex_lock(&sdev->vif_lock);
	for (i = 0; i < SC23XX_VIF_NUM; i++) {
		struct sc23xx_vif *other_vif;
		
		other_vif = srcu_dereference_check(sdev->vif[i],
					&sdev->vif_srcu,
					lockdep_is_held(&sdev->vif_lock));
		if (other_vif && !test_bit(SC23XX_FLAG_USE_ALT_MAC,
					   &other_vif->flags))
			set_bit(SC23XX_FLAG_USE_ALT_MAC, &vif->flags);
	}
	/* AP interface may also act as second STA interface */
	if (rcu_access_pointer(sdev->vif[vif->idx]) &&
	    vif->idx == SC23XX_VIF_STA) {
		vif->idx = SC23XX_VIF_AP;
		vif->mode = SC23XX_MODE_STATION_SECOND;
	}
	if (rcu_access_pointer(sdev->vif[vif->idx])) {
		wiphy_err(sdev->wiphy, "%s interface already exists\n",
			  sc23xx_vif_idx_to_string(vif->idx));
		mutex_unlock(&sdev->vif_lock);
		ret = -EINVAL;
		goto err_free_netdev;
	}
	rcu_assign_pointer(sdev->vif[vif->idx], vif);
	mutex_unlock(&sdev->vif_lock);

	wdev = &vif->wdev;
	wdev->wiphy = sdev->wiphy;
	wdev->netdev = ndev;
	wdev->iftype = iftype;

	SET_NETDEV_DEV(ndev, wiphy_dev(sdev->wiphy));
	ndev->needs_free_netdev = true;
	ndev->ieee80211_ptr = wdev;
	ndev->netdev_ops = &sc23xx_netdev_ops;
	ndev->needed_headroom = sizeof(struct sc23xx_tx_data_hdr) + ETH_HLEN;
	if (sdev->use_dma)
		ndev->needed_headroom += sizeof(struct sc23xx_addr);

	memcpy(mac_addr, sdev->mac_addr, ETH_ALEN);
	if (test_bit(SC23XX_FLAG_USE_ALT_MAC, &vif->flags))
		mac_addr[5] ^= 0x70;
	eth_hw_addr_set(ndev, mac_addr);

	ret = cfg80211_register_netdevice(ndev);
	if (ret)
		goto err_remove_vif;

	return vif;

err_remove_vif:
	mutex_lock(&sdev->vif_lock);
	sdev->vif[vif->idx] = NULL;
	mutex_unlock(&sdev->vif_lock);
err_free_netdev:
	free_netdev(ndev);
	return ERR_PTR(ret);
}

int sc23xx_vif_reopen(struct sc23xx_vif *vif, enum nl80211_iftype iftype)
{
	enum sc23xx_vif_mode mode;
	int ret;
	u8 idx;

	ret = sc23xx_iftype_to_mode(iftype, &mode, &idx);
	if (ret)
		return ret;

	mutex_lock(&vif->sdev->vif_lock);
	/* AP interface may also act as second STA interface */
	if (rcu_access_pointer(vif->sdev->vif[idx]) && idx == SC23XX_VIF_STA) {
		idx = SC23XX_VIF_AP;
		mode = SC23XX_MODE_STATION_SECOND;
	}
	if (rcu_access_pointer(vif->sdev->vif[idx])) {
		wiphy_err(vif->sdev->wiphy, "%s interface already exists\n",
			  sc23xx_vif_idx_to_string(idx));
		mutex_unlock(&vif->sdev->vif_lock);
		return -EINVAL;
	}

	if (!test_bit(SC23XX_FLAG_OPENED, &vif->flags)) {
		ret = sc23xx_cmd_close(vif);
		if (ret)
			return ret;
	}

	vif->idx = idx;
	rcu_assign_pointer(vif->sdev->vif[vif->idx], NULL);
	rcu_assign_pointer(vif->sdev->vif[idx], vif);
	mutex_unlock(&vif->sdev->vif_lock);

	vif->wdev.iftype = iftype;
	vif->mode = mode;

	if (test_bit(SC23XX_FLAG_OPENED, &vif->flags)) {
		ret = sc23xx_cmd_open(vif, vif->wdev.netdev->dev_addr);
		if (ret)
			return ret;
	}

	return 0;
}

void sc23xx_notify_scan_done(struct sc23xx_vif *vif, bool aborted)
{
	struct cfg80211_scan_info scan_info = { .aborted = aborted };

	mutex_lock(&vif->scan_lock);
	if (vif->scan_req) {
		cfg80211_scan_done(vif->scan_req, &scan_info);
		vif->scan_req = NULL;
	}
	mutex_unlock(&vif->scan_lock);
}

struct sc23xx_vif *sc23xx_get_vif(struct sc23xx_dev *sdev, int idx)
{
	struct sc23xx_vif *vif = NULL;

	if (idx < SC23XX_VIF_NUM)
		vif = srcu_dereference(sdev->vif[idx], &sdev->vif_srcu);

	if (!vif)
		wiphy_warn(sdev->wiphy, "invalid interface index %d\n", idx);

	return vif;
}
