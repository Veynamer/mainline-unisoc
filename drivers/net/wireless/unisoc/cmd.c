// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Otto Pflüger
 */

#include "cmd.h"
#include "txrx.h"
#include "vif.h"

#define SC23XX_CMD_TIMEOUT	3000

static struct sk_buff *sc23xx_cmd_alloc_skb(u8 cmd, u16 len, u8 ctx_id)
{
	struct sk_buff *skb;
	struct sc23xx_cmd_hdr *hdr;

	skb = alloc_skb(sizeof(*hdr) + len, GFP_KERNEL);
	if (!skb)
		return NULL;

	hdr = skb_put_zero(skb, sizeof(*hdr));
	hdr->common.type = SC23XX_HDR_TYPE_CMD;
	hdr->common.data_dir = 0;
	hdr->common.need_rsp = 1;
	hdr->common.ctx_id = ctx_id;
	hdr->cmd = cmd;
	hdr->len = cpu_to_le16(sizeof(*hdr) + len);
	hdr->timestamp = cpu_to_le32((u32)ktime_to_ms(ktime_get()));

	return skb;
}

static bool sc23xx_get_ie(struct sk_buff *skb, const u8 **data, u16 *len)
{
	if (skb->len < 2)
		goto no_ie;
	*len = get_unaligned_le16(skb_pull_data(skb, 2));
	if (skb->len < *len)
		goto no_ie;
	*data = skb_pull_data(skb, *len);

	return true;
no_ie:
	*len = 0;
	*data = NULL;
	return false;
}

static int sc23xx_cmd_handle_error(struct sc23xx_dev *sdev,
				   struct sc23xx_cmd_hdr *hdr)
{
	const char *err;
	int ret = -EINVAL;

	/* The return codes might be inaccurate for some errors since
	 * not all names provided by Unisoc are equally meaningful.
	 */
	switch (hdr->status) {
	case SC23XX_CMD_STATUS_ARG_ERROR:
		err = "ARG_ERROR";
		break;
	case SC23XX_CMD_STATUS_GET_RESULT_ERROR:
		err = "GET_RESULT_ERROR";
		ret = -EREMOTEIO;
		break;
	case SC23XX_CMD_STATUS_EXEC_ERROR:
		err = "EXEC_ERROR";
		ret = -EREMOTEIO;
		break;
	case SC23XX_CMD_STATUS_MALLOC_ERROR:
		err = "MALLOC_ERROR";
		ret = -ENOMEM;
		break;
	case SC23XX_CMD_STATUS_WIFIMODE_ERROR:
		err = "WIFIMODE_ERROR";
		break;
	case SC23XX_CMD_STATUS_CANNOT_EXEC_ERROR:
		err = "CANNOT_EXEC_ERROR";
		ret = -EOPNOTSUPP;
		break;
	case SC23XX_CMD_STATUS_NOT_SUPPORT_ERROR:
		err = "NOT_SUPPORT_ERROR";
		ret = -EOPNOTSUPP;
		break;
	case SC23XX_CMD_STATUS_CRC_ERROR:
		err = "CRC_ERROR";
		break;
	case SC23XX_CMD_STATUS_INI_INDEX_ERROR:
		err = "INI_INDEX_ERROR";
		break;
	case SC23XX_CMD_STATUS_LENGTH_ERROR:
		err = "LENGTH_ERROR";
		break;
	case SC23XX_CMD_STATUS_OTHER_ERROR:
		err = "OTHER_ERROR";
		ret = -EREMOTEIO;
		break;
	default:
		err = "unknown error";
		ret = -EREMOTEIO;
		break;
	}

	wiphy_warn(sdev->wiphy, "command %d status: %s\n", hdr->cmd, err);

	return ret;
}

static int sc23xx_send_cmd_wait(struct sc23xx_dev *sdev, struct sk_buff *skb,
				struct sk_buff **ret_skb, size_t min_rsp_len)
{
	struct sc23xx_cmd_hdr *hdr, *rhdr;
	struct sk_buff *rskb;
	int ret;

	hdr = (void *)skb->data;
	wiphy_dbg(sdev->wiphy, "command %d\n", hdr->cmd);

	mutex_lock(&sdev->cmd_lock);

	ret = sc23xx_tx_cmd(sdev, skb);
	if (ret)
		goto out;

	wait_event_timeout(sdev->rsp_wait, !skb_queue_empty(&sdev->rsp_q),
			   msecs_to_jiffies(SC23XX_CMD_TIMEOUT));

	rskb = skb_dequeue(&sdev->rsp_q);
	if (!rskb) {
		wiphy_warn(sdev->wiphy, "command %d timeout\n", hdr->cmd);
		ret = -ETIMEDOUT;
		goto out;
	}

	/* Check that the response is correct and for the right command */
	rhdr = (void *)rskb->data;
	if (rskb->len < (sizeof(*rhdr) + min_rsp_len) ||
	    rskb->len < le16_to_cpu(rhdr->len) || rhdr->cmd != hdr->cmd ||
	    rhdr->timestamp != hdr->timestamp) {
		wiphy_err(sdev->wiphy,
			  "mismatch: rsp ctx=%d len=%d/%d cmd=%d ts=%d, req %d >=%ld %d %d\n",
			  rhdr->common.ctx_id, le16_to_cpu(rhdr->len), rskb->len,
			  rhdr->cmd, le32_to_cpu(rhdr->timestamp), hdr->common.ctx_id,
			  sizeof(*rhdr) + min_rsp_len, hdr->cmd,
			  le32_to_cpu(hdr->timestamp));
		dev_kfree_skb(rskb);
		skb_queue_purge(&sdev->rsp_q);
		ret = -EIO;
		goto out;
	}

	if (rhdr->status < 0) {
		ret = sc23xx_cmd_handle_error(sdev, rhdr);
		dev_kfree_skb(rskb);
		goto out;
	}

	skb_pull(rskb, sizeof(*rhdr));

	if (ret_skb)
		*ret_skb = rskb;
	else
		dev_kfree_skb(rskb);

out:
	dev_kfree_skb(skb);
	mutex_unlock(&sdev->cmd_lock);

	return ret;
}

int sc23xx_cmd_open(struct sc23xx_vif *vif, const u8 *mac_addr)
{
	struct sc23xx_req_open *req;
	struct sk_buff *skb;

	skb = sc23xx_cmd_alloc_skb(CMD_OPEN, sizeof(*req), vif->idx);
	if (!skb)
		return -ENOMEM;

	req = skb_put(skb, sizeof(*req));
	req->mode = vif->mode;
	memcpy(req->mac, mac_addr, ETH_ALEN);

	return sc23xx_send_cmd_wait(vif->sdev, skb, NULL, 0);
}

static int sc23xx_cmd_set_ie(struct sc23xx_vif *vif, u8 type,
			     const u8 *ie, u16 ie_len)
{
	struct sk_buff *skb;

	skb = sc23xx_cmd_alloc_skb(CMD_SET_IE, 3 + ie_len, vif->idx);
	if (!skb)
		return -ENOMEM;

	skb_put_u8(skb, type);
	put_unaligned_le16(ie_len, skb_put(skb, 2));
	skb_put_data(skb, ie, ie_len);

	return sc23xx_send_cmd_wait(vif->sdev, skb, NULL, 0);
}

int sc23xx_cmd_close(struct sc23xx_vif *vif)
{
	struct sc23xx_req_close *req;
	struct sk_buff *skb;

	skb = sc23xx_cmd_alloc_skb(CMD_CLOSE, sizeof(*req), vif->idx);
	if (!skb)
		return -ENOMEM;

	req = skb_put(skb, sizeof(*req));
	req->mode = vif->mode;

	return sc23xx_send_cmd_wait(vif->sdev, skb, NULL, 0);
}

int sc23xx_cmd_set_suspend(struct sc23xx_vif *vif, bool suspend)
{
	struct sk_buff *skb;

	skb = sc23xx_cmd_alloc_skb(CMD_POWER_SAVE, 2, vif->idx);
	if (!skb)
		return -ENOMEM;

	skb_put_u8(skb, SC23XX_POWER_SAVE_SUSPEND_RESUME);
	skb_put_u8(skb, !suspend);

	return sc23xx_send_cmd_wait(vif->sdev, skb, NULL, 0);
}

int sc23xx_cmd_connect(struct sc23xx_vif *vif, struct cfg80211_connect_params *sme)
{
	struct sc23xx_req_connect *req;
	struct sk_buff *skb;
	int ret;

	if (vif->state == SC23XX_STATE_CONNECTED)
		return -EALREADY;

	if (sme->key_len) {
		struct key_params params = {
			.key = sme->key,
			.key_len = sme->key_len,
			.cipher = sme->crypto.cipher_group,
		};

		ret = sc23xx_cmd_add_key(vif, sme->key_idx, false, NULL, &params);
		if (ret)
			return ret;

		ret = sc23xx_cmd_set_def_key(vif, sme->key_idx);
		if (ret)
			return ret;
	}

	ret = sc23xx_cmd_set_ie(vif, SC23XX_IE_ASSOC_REQ, sme->ie, sme->ie_len);
	if (ret)
		return ret;

	skb = sc23xx_cmd_alloc_skb(CMD_CONNECT, sizeof(*req), vif->idx);
	if (!skb)
		return -ENOMEM;

	req = skb_put_zero(skb, sizeof(*req));

	req->wpa_versions = cpu_to_le32(sc23xx_convert_wpa(sme->crypto.wpa_versions));

	if (sme->channel)
		req->channel = sme->channel->hw_value;
	else if (sme->channel_hint)
		req->channel = sme->channel_hint->hw_value;

	if (sme->bssid) {
		memcpy(req->bssid, sme->bssid, ETH_ALEN);
		memcpy(vif->bssid, sme->bssid, ETH_ALEN);
	} else if (sme->bssid_hint) {
		memcpy(req->bssid, sme->bssid_hint, ETH_ALEN);
		memcpy(vif->bssid, sme->bssid_hint, ETH_ALEN);
	} else {
		dev_kfree_skb(skb);
		return -EOPNOTSUPP;
	}

	switch (sme->auth_type) {
	case NL80211_AUTHTYPE_OPEN_SYSTEM:
		req->auth_type = SC23XX_AUTH_OPEN;
		break;
	case NL80211_AUTHTYPE_SHARED_KEY:
		req->auth_type = SC23XX_AUTH_SHARED;
		break;
	case NL80211_AUTHTYPE_SAE:
		req->auth_type = SC23XX_AUTH_SAE;
		break;
	default:
		if (sme->crypto.cipher_group == WLAN_CIPHER_SUITE_WEP40 ||
		    sme->crypto.cipher_group == WLAN_CIPHER_SUITE_WEP104)
			req->auth_type = SC23XX_AUTH_SHARED;
		else
			req->auth_type = SC23XX_AUTH_OPEN;
		break;
	}

	if (sme->crypto.n_ciphers_pairwise)
		req->pairwise_cipher = sc23xx_convert_cipher(sme->crypto.ciphers_pairwise[0]);

	req->group_cipher = sc23xx_convert_cipher(sme->crypto.cipher_group);

	if (sme->crypto.n_akm_suites)
		req->key_mgmt = sc23xx_convert_akm(sme->crypto.akm_suites[0]);

	req->mfp_enable = sme->mfp;

	if (sme->ssid_len) {
		memcpy(req->ssid, sme->ssid, sme->ssid_len);
		req->ssid_len = sme->ssid_len;
	}

	ret = sc23xx_send_cmd_wait(vif->sdev, skb, NULL, 0);
	if (ret)
		return ret;

	vif->state = SC23XX_STATE_CONNECTING;

	return 0;
}

int sc23xx_cmd_disconnect(struct sc23xx_vif *vif, u16 reason_code)
{
	struct sc23xx_req_disconnect *req;
	struct sk_buff *skb;

	skb = sc23xx_cmd_alloc_skb(CMD_DISCONNECT, sizeof(*req), vif->idx);
	if (!skb)
		return -ENOMEM;

	req = skb_put(skb, sizeof(*req));
	req->reason_code = cpu_to_le16(reason_code);

	return sc23xx_send_cmd_wait(vif->sdev, skb, NULL, 0);
}

int sc23xx_cmd_add_key(struct sc23xx_vif *vif, u8 key_index, bool pairwise,
		       const u8 *mac_addr, struct key_params *params)
{
	struct sc23xx_req_add_key *req;
	struct sk_buff *skb;

	if (params->seq_len > sizeof(req->key_seq)) {
		netdev_err(vif->wdev.netdev, "key seq_len %d too large\n",
			   params->seq_len);
		return -EINVAL;
	}

	if (params->key_len > U8_MAX) {
		netdev_err(vif->wdev.netdev, "key_len %d too large\n",
			   params->key_len);
		return -EINVAL;
	}

	skb = sc23xx_cmd_alloc_skb(CMD_KEY, sizeof(*req) + params->key_len, vif->idx);
	if (!skb)
		return -ENOMEM;

	req = skb_put_zero(skb, sizeof(*req));
	req->subcmd = SUBCMD_ADD;
	req->key_index = key_index;
	req->pairwise = pairwise;
	if (mac_addr)
		memcpy(req->mac, mac_addr, ETH_ALEN);
	if (params->seq)
		memcpy(req->key_seq, params->seq, params->seq_len);
	req->cipher_type = sc23xx_convert_cipher(params->cipher) &
			   SC23XX_CRYPTO_SUITE_MASK;
	req->key_len = params->key_len;

	if (params->key)
		skb_put_data(skb, params->key, params->key_len);

	return sc23xx_send_cmd_wait(vif->sdev, skb, NULL, 0);
}

int sc23xx_cmd_del_key(struct sc23xx_vif *vif, u8 key_index, bool pairwise,
		       const u8 *mac_addr)
{
	struct sc23xx_req_del_key *req;
	struct sk_buff *skb;

	skb = sc23xx_cmd_alloc_skb(CMD_KEY, sizeof(*req), vif->idx);
	if (!skb)
		return -ENOMEM;

	req = skb_put_zero(skb, sizeof(*req));
	req->subcmd = SUBCMD_DEL;
	req->key_index = key_index;
	req->pairwise = pairwise;
	if (mac_addr)
		memcpy(req->mac, mac_addr, ETH_ALEN);

	return sc23xx_send_cmd_wait(vif->sdev, skb, NULL, 0);
}

int sc23xx_cmd_set_def_key(struct sc23xx_vif *vif, u8 key_index)
{
	struct sc23xx_req_set_def_key *req;
	struct sk_buff *skb;

	skb = sc23xx_cmd_alloc_skb(CMD_KEY, sizeof(*req), vif->idx);
	if (!skb)
		return -ENOMEM;

	req = skb_put(skb, sizeof(*req));
	req->subcmd = SUBCMD_SET;
	req->key_index = key_index;

	return sc23xx_send_cmd_wait(vif->sdev, skb, NULL, 0);
}

int sc23xx_cmd_set_rekey_data(struct sc23xx_vif *vif,
			      struct cfg80211_gtk_rekey_data *data)
{
	struct sc23xx_req_set_rekey_data *req;
	struct sk_buff *skb;

	skb = sc23xx_cmd_alloc_skb(CMD_KEY, sizeof(*req), vif->idx);
	if (!skb)
		return -ENOMEM;

	req = skb_put(skb, sizeof(*req));
	req->subcmd = SUBCMD_REKEY;
	memcpy(req->kek, data->kek, NL80211_KEK_LEN);
	memcpy(req->kck, data->kck, NL80211_KCK_LEN);
	memcpy(req->replay_ctr, data->replay_ctr, NL80211_REPLAY_CTR_LEN);

	return sc23xx_send_cmd_wait(vif->sdev, skb, NULL, 0);
}

int sc23xx_cmd_set_probe_req_ie(struct sc23xx_vif *vif, const u8 *ie, u16 ie_len)
{
	return sc23xx_cmd_set_ie(vif, SC23XX_IE_PROBE_REQ, ie, ie_len);
}

int sc23xx_cmd_scan(struct sc23xx_vif *vif,
		    struct cfg80211_scan_request *request)
{
	struct sc23xx_req_scan *req;
	struct sk_buff *skb;
	unsigned int ssids_len = 0, n_chns_5g = 0, req_size;
	u32 chn_2g_mask = 0;
	u16 chns_5g[64];
	int i;

	for (i = 0; i < request->n_ssids; i++)
		ssids_len += 1 + request->ssids[i].ssid_len;

	req_size = sizeof(*req) + ssids_len + 2 * (1 + n_chns_5g);

	skb = sc23xx_cmd_alloc_skb(CMD_SCAN, req_size, vif->idx);
	if (!skb)
		return -ENOMEM;

	req = skb_put(skb, sizeof(*req));

	for (i = 0; i < request->n_channels; i++) {
		u16 ch_idx = request->channels[i]->hw_value;

		if (sc23xx_channel_is_2ghz(ch_idx))
			chn_2g_mask |= BIT(ch_idx - 1);
		else if (n_chns_5g < ARRAY_SIZE(chns_5g))
			chns_5g[n_chns_5g++] = ch_idx;
	}

	req->channel_mask = cpu_to_le32(chn_2g_mask);
	req->abort = cpu_to_le32(0);
	req->ssids_len = cpu_to_le16(ssids_len);

	for (i = 0; i < request->n_ssids; i++) {
		struct cfg80211_ssid *ssid = &request->ssids[i];

		skb_put_u8(skb, ssid->ssid_len);
		skb_put_data(skb, ssid->ssid, ssid->ssid_len);
	}

	put_unaligned_le16(n_chns_5g, skb_put(skb, 2));
	for (i = 0; i < n_chns_5g; i++)
		put_unaligned_le16(chns_5g[i], skb_put(skb, 2));

	return sc23xx_send_cmd_wait(vif->sdev, skb, NULL, 0);
}

int sc23xx_cmd_abort_scan(struct sc23xx_vif *vif)
{
	struct sc23xx_req_scan *req;
	struct sk_buff *skb;

	skb = sc23xx_cmd_alloc_skb(CMD_SCAN, sizeof(*req), vif->idx);
	if (!skb)
		return -ENOMEM;

	req = skb_put_zero(skb, sizeof(*req));
	req->abort = cpu_to_le32(1);

	return sc23xx_send_cmd_wait(vif->sdev, skb, NULL, 0);
}

int sc23xx_cmd_sched_scan_start(struct sc23xx_vif *vif,
				struct cfg80211_sched_scan_request *request)
{
	struct sc23xx_req_sched_scan *req;
	struct sc23xx_sched_scan_ie_hdr *ie;
	struct sc23xx_sched_scan_ifrc_ie *ifrc;
	struct sk_buff *skb;
	size_t len = sizeof(*req) + sizeof(*ie) + sizeof(*ifrc) +
		(request->n_ssids > 0 ? sizeof(*ie) : 0) +
		request->n_ssids * IEEE80211_MAX_SSID_LEN +
		(request->n_match_sets > 0 ? sizeof(*ie) : 0) +
		request->n_match_sets * IEEE80211_MAX_SSID_LEN +
		(request->ie_len > 0 ? sizeof(*ie) : 0) +
		request->ie_len;
	int i;

	skb = sc23xx_cmd_alloc_skb(CMD_SCHED_SCAN, len, vif->idx);
	if (!skb)
		return -ENOMEM;

	req = skb_put(skb, sizeof(*req));
	req->start = cpu_to_le16(1);
	req->buf_flags = cpu_to_le16(1);

	ie = skb_put(skb, sizeof(*ie));
	ie->type = cpu_to_le16(SC23XX_SCHED_SCAN_IFRC);
	ie->len = cpu_to_le16(sizeof(*ifrc));

	ifrc = skb_put(skb, sizeof(*ifrc));

	ifrc->interval = cpu_to_le32(request->scan_plans[0].interval);

	/* The firmware apparently understands nl80211_scan_flags */
	ifrc->flags = cpu_to_le32(request->flags);

	if (request->min_rssi_thold <= NL80211_SCAN_RSSI_THOLD_OFF)
		ifrc->rssi_thold = 0;
	else if (request->min_rssi_thold < -127)
		ifrc->rssi_thold = cpu_to_le32(-127);
	else
		ifrc->rssi_thold = cpu_to_le32(request->min_rssi_thold);

	ifrc->n_channels = min_t(u8, ARRAY_SIZE(ifrc->channels),
				 request->n_channels);
	for (i = 0; i < ifrc->n_channels; i++)
		ifrc->channels[i] = request->channels[i]->hw_value;

	if (request->n_ssids > 0) {
		ie = skb_put(skb, sizeof(*ie));
		put_unaligned_le16(SC23XX_SCHED_SCAN_SSID, &ie->type);
		put_unaligned_le16(request->n_ssids * IEEE80211_MAX_SSID_LEN,
				   &ie->len);
		for (i = 0; i < request->n_ssids; i++)
			skb_put_data(skb, request->ssids[i].ssid,
				     IEEE80211_MAX_SSID_LEN);
	}

	if (request->n_match_sets > 0) {
		ie = skb_put(skb, sizeof(*ie));
		put_unaligned_le16(SC23XX_SCHED_SCAN_MATCH_SSID, &ie->type);
		put_unaligned_le16(request->n_match_sets *
				   IEEE80211_MAX_SSID_LEN, &ie->len);
		for (i = 0; i < request->n_match_sets; i++)
			skb_put_data(skb, request->match_sets[i].ssid.ssid,
				     IEEE80211_MAX_SSID_LEN);
	}

	if (request->ie_len > 0) {
		ie = skb_put(skb, sizeof(*ie));
		put_unaligned_le16(SC23XX_SCHED_SCAN_IE, &ie->type);
		put_unaligned_le16(request->ie_len, &ie->len);
		skb_put_data(skb, request->ie, request->ie_len);
	}

	return sc23xx_send_cmd_wait(vif->sdev, skb, NULL, 0);
}

int sc23xx_cmd_sched_scan_stop(struct sc23xx_vif *vif)
{
	struct sc23xx_req_sched_scan *req;
	struct sk_buff *skb;

	skb = sc23xx_cmd_alloc_skb(CMD_SCHED_SCAN, sizeof(*req), vif->idx);
	if (!skb)
		return -ENOMEM;

	req = skb_put(skb, sizeof(*req));
	req->start = cpu_to_le16(0);
	req->buf_flags = cpu_to_le16(1);

	return sc23xx_send_cmd_wait(vif->sdev, skb, NULL, 0);
}

int sc23xx_cmd_mgmt_tx(struct sc23xx_vif *vif, struct cfg80211_mgmt_tx_params *params,
		       u64 *cookie)
{
	struct sc23xx_req_mgmt_tx *req;
	struct sk_buff *skb;

	if (params->len > U16_MAX) {
		netdev_err(vif->wdev.netdev, "MGMT tx length %ld too large\n",
			   params->len);
		return -EINVAL;
	}

	skb = sc23xx_cmd_alloc_skb(CMD_TX_MGMT, sizeof(*req) + params->len, vif->idx);
	if (!skb)
		return -ENOMEM;

	req = skb_put(skb, sizeof(*req));
	req->chan = params->chan->hw_value;
	req->dont_wait_for_ack = params->dont_wait_for_ack;
	put_unaligned_le32(params->wait, &req->wait);
	put_unaligned_le64(*cookie, &req->cookie);
	req->len = cpu_to_le16(params->len);

	skb_put_data(skb, params->buf, params->len);

	return sc23xx_send_cmd_wait(vif->sdev, skb, NULL, 0);
}

void sc23xx_cmd_set_multicast(struct work_struct *work)
{
	struct sc23xx_set_multicast_cmd *cmd =
		container_of(work, struct sc23xx_set_multicast_cmd, work);
	struct sk_buff *skb;
	int ret;

	skb = sc23xx_cmd_alloc_skb(CMD_MULTICAST_FILTER,
				   2 + cmd->count * ETH_ALEN,
				   cmd->vif->idx);
	if (!skb) {
		kfree(cmd);
		return;
	}

	skb_put_u8(skb, 1);
	skb_put_u8(skb, cmd->count);
	skb_put_data(skb, cmd->addr_list, cmd->count * ETH_ALEN);

	ret = sc23xx_send_cmd_wait(cmd->vif->sdev, skb, NULL, 0);
	if (ret)
		netdev_err(cmd->vif->wdev.netdev,
			   "failed to set multicast list\n");

	kfree(cmd);
}

int sc23xx_cmd_set_mac_addr(struct sc23xx_vif *vif, const u8 *mac_addr)
{
	struct sc23xx_req_set_mac_addr *req;
	struct sk_buff *skb;

	skb = sc23xx_cmd_alloc_skb(CMD_SET_MAC, sizeof(*req), vif->idx);
	if (!skb)
		return -ENOMEM;

	req = skb_put(skb, sizeof(*req));
	req->mode = SC23XX_CONNECT_RANDOM_ADDR;
	memcpy(req->mac_addr, mac_addr, ETH_ALEN);

	return sc23xx_send_cmd_wait(vif->sdev, skb, NULL, 0);
}

/* Runs on the ordered event WQ, so sta will never be modified concurrently */
void sc23xx_cmd_tx_addba_req(struct work_struct *work)
{
	struct sc23xx_sta *sta =
		container_of(work, struct sc23xx_sta, tx_ba_setup.work);
	struct sc23xx_dev *sdev = sta->sdev;
	u8 sta_lut_idx = SC23XX_STA_IDX_MIN + (sta - sdev->sta);
	struct sc23xx_req_tx_addba *req;
	struct sk_buff *skb, *rskb;
	u16 capab;
	int ret;

	if (!sta->ht_enabled || !sta->addba_retries)
		return;

	skb = sc23xx_cmd_alloc_skb(CMD_ADDBA_REQ, sizeof(*req), sta->ctx_id);
	if (!skb)
		return;

	req = skb_put(skb, sizeof(*req));

	req->sta_lut_index = sta_lut_idx;
	memcpy(req->addr, sta->addr, ETH_ALEN);
	req->dialog_token = 1;
	req->timeout = 0;

	capab = IEEE80211_ADDBA_PARAM_POLICY_MASK;
	/* only tid 0 is used at the moment */
	capab |= u16_encode_bits(SC23XX_TX_BA_WIN_SIZE,
				 IEEE80211_ADDBA_PARAM_BUF_SIZE_MASK);
	req->addba_param = cpu_to_le16(capab);

	ret = sc23xx_send_cmd_wait(sdev, skb, &rskb, 1);
	if (ret || rskb->data[0]) {
		if (!ret)
			dev_kfree_skb(rskb);
		sta->addba_retries--;
		wiphy_warn(sdev->wiphy,
			   "tx addba for sta %d failed, %d retries left\n",
			   sta_lut_idx, sta->addba_retries);
		if (sta->addba_retries)
			queue_delayed_work(sdev->evt_wq, &sta->tx_ba_setup,
					   msecs_to_jiffies(2000));
	} else {
		sta->addba_retries = 0;
	}
}

int sc23xx_cmd_addba_rsp(struct sc23xx_dev *sdev, struct sc23xx_sta *sta,
			 u8 tid)
{
	struct sc23xx_req_ba *req;
	struct sk_buff *skb;

	skb = sc23xx_cmd_alloc_skb(CMD_BA, sizeof(*req), sta->ctx_id);
	if (!skb)
		return -ENOMEM;

	req = skb_put(skb, sizeof(*req));
	req->type = SC23XX_ADDBA_RSP;
	req->tid = tid;
	memcpy(req->addr, sta->addr, ETH_ALEN);
	req->success = 1;

	return sc23xx_send_cmd_wait(sdev, skb, NULL, 0);
}

int sc23xx_cmd_set_regdom(struct sc23xx_dev *sdev, char *alpha2,
			  const struct ieee80211_reg_rule **rules,
			  unsigned int n_rules)
{
	struct sk_buff *skb;
	size_t req_size;
	int i;

	req_size = sizeof(u32) + 2 + n_rules * sizeof(struct sc23xx_reg_rule);

	skb = sc23xx_cmd_alloc_skb(CMD_SET_REGDOM, req_size, 0);
	if (!skb)
		return -ENOMEM;

	*(__le32 *)skb_put(skb, 4) = cpu_to_le32(n_rules);

	skb_put_data(skb, alpha2, 2);

	for (i = 0; i < n_rules; i++) {
		const struct ieee80211_freq_range *freq = &rules[i]->freq_range;
		const struct ieee80211_power_rule *pwr = &rules[i]->power_rule;
		struct sc23xx_reg_rule *r =
			skb_put(skb, sizeof(struct sc23xx_reg_rule));

		put_unaligned_le32(freq->start_freq_khz, &r->start_freq_khz);
		put_unaligned_le32(freq->end_freq_khz, &r->end_freq_khz);
		put_unaligned_le32(freq->max_bandwidth_khz,
				   &r->max_bandwidth_khz);

		put_unaligned_le32(pwr->max_antenna_gain, &r->max_antenna_gain);
		put_unaligned_le32(pwr->max_eirp, &r->max_eirp);

		put_unaligned_le32(rules[i]->flags, &r->flags);
		put_unaligned_le32(rules[i]->dfs_cac_ms, &r->dfs_cac_ms);
	}

	return sc23xx_send_cmd_wait(sdev, skb, NULL, 0);
}

int sc23xx_download_config_section(struct sc23xx_dev *sdev, u32 section,
				   const void *data, u16 size)
{
	struct sk_buff *skb;

	skb = sc23xx_cmd_alloc_skb(CMD_DOWNLOAD_INI, 4 + size, 0);
	if (!skb)
		return -ENOMEM;

	*(__le32 *)skb_put(skb, 4) = cpu_to_le32(section);
	skb_put_data(skb, data, size);

	return sc23xx_send_cmd_wait(sdev, skb, NULL, 0);
}

int sc23xx_get_fw_info(struct sc23xx_dev *sdev)
{
	struct sk_buff *skb, *rskb;
	struct sc23xx_rsp_fw_info_1 *info_1;
	struct sc23xx_rsp_fw_info_2 *info_2;
	u32 fw_std, fw_capa, ampdu_params, val;
	int ret;

	skb = sc23xx_cmd_alloc_skb(CMD_GET_INFO, 0, 0);
	if (!skb)
		return -ENOMEM;

	ret = sc23xx_send_cmd_wait(sdev, skb, &rskb, sizeof(*info_1));
	if (ret)
		return ret;

	info_1 = skb_pull_data(rskb, sizeof(*info_1));

	fw_std = le32_to_cpu(info_1->fw_std);
	fw_capa = le32_to_cpu(info_1->fw_capa);

	wiphy_dbg(sdev->wiphy, "Chip model: %x (version 0x%x)\n",
		  le32_to_cpu(info_1->chip_model),
		  le32_to_cpu(info_1->chip_version));
	wiphy_dbg(sdev->wiphy, "FW version: %d, std: 0x%x, capa: 0x%x\n",
		  le32_to_cpu(info_1->fw_version), fw_std, fw_capa);

	if (fw_capa & SC23XX_CAPA_MC_FILTER)
		sdev->max_mc_mac_addrs = info_1->max_mc_mac_addrs;

	sdev->wiphy->bands[NL80211_BAND_2GHZ] = &sdev->band_2ghz;

	if (fw_capa & SC23XX_CAPA_5G) {
		wiphy_dbg(sdev->wiphy, "5GHz band is supported\n");
		sdev->wiphy->bands[NL80211_BAND_5GHZ] = &sdev->band_5ghz;
	}

	if (fw_capa & SC23XX_CAPA_AP_SME) {
		wiphy_dbg(sdev->wiphy, "AP SME is supported\n");
		sdev->wiphy->flags |= WIPHY_FLAG_HAVE_AP_SME;
		sdev->wiphy->ap_sme_capa = 1;
	}

	if (fw_capa & SC23XX_CAPA_SCHED_SCAN) {
		wiphy_dbg(sdev->wiphy, "Scheduled scan is supported\n");
		sdev->wiphy->max_sched_scan_reqs = 1;
		sdev->wiphy->max_sched_scan_ssids = SC23XX_MAX_PFN_LIST_COUNT;
		sdev->wiphy->max_match_sets = SC23XX_MAX_PFN_LIST_COUNT;
		sdev->wiphy->max_sched_scan_ie_len = SC23XX_MAX_SCAN_IE_LEN;
	}

	info_2 = skb_pull_data(rskb, sizeof(*info_2));
	if (info_2) {
		if (info_2->ht_cap_info) {
			wiphy_dbg(sdev->wiphy, "HT capability info present\n");
			sdev->band_2ghz.ht_cap.cap = le16_to_cpu(info_2->ht_cap_info);
		}
		ampdu_params = le16_to_cpu(info_2->ampdu_params);
		sdev->band_2ghz.ht_cap.ampdu_factor = ampdu_params & 3;
		sdev->band_2ghz.ht_cap.ampdu_density = (ampdu_params >> 2) & 7;
		memcpy(&sdev->band_2ghz.ht_cap.mcs, &info_2->ht_mcs,
		       sizeof(struct ieee80211_mcs_info));

		if (info_2->ht_cap_info)
			sdev->band_5ghz.ht_cap.cap = le16_to_cpu(info_2->ht_cap_info);
		sdev->band_5ghz.ht_cap.ampdu_factor = ampdu_params & 3;
		sdev->band_5ghz.ht_cap.ampdu_density = (ampdu_params >> 2) & 7;
		memcpy(&sdev->band_5ghz.ht_cap.mcs, &info_2->ht_mcs,
		       sizeof(struct ieee80211_mcs_info));

		if (info_2->vht_cap_info) {
			wiphy_dbg(sdev->wiphy, "VHT capability info present\n");
			sdev->band_5ghz.vht_cap.cap = le32_to_cpu(info_2->vht_cap_info);
		}
		memcpy(&sdev->band_5ghz.vht_cap.vht_mcs, &info_2->vht_mcs,
		       sizeof(struct ieee80211_vht_mcs_info));

		if (info_2->antenna_tx) {
			val = le32_to_cpu(info_2->antenna_tx);
			wiphy_dbg(sdev->wiphy, "Available TX antennas: %d\n", val);
			sdev->wiphy->available_antennas_tx = val;
		}
		if (info_2->antenna_rx) {
			val = le32_to_cpu(info_2->antenna_rx);
			wiphy_dbg(sdev->wiphy, "Available RX antennas: %d\n", val);
			sdev->wiphy->available_antennas_rx = val;
		}
		if (info_2->retry_short) {
			wiphy_dbg(sdev->wiphy, "FW retry_short: %d\n",
				  info_2->retry_short);
			sdev->wiphy->retry_short = info_2->retry_short;
		}
		if (info_2->retry_long) {
			wiphy_dbg(sdev->wiphy, "FW retry_long: %d\n",
				  info_2->retry_long);
			sdev->wiphy->retry_long = info_2->retry_long;
		}
		val = le32_to_cpu(info_2->frag_threshold);
		if (val && val <= IEEE80211_MAX_FRAG_THRESHOLD) {
			wiphy_dbg(sdev->wiphy, "FW frag_threshold: %d\n", val);
			sdev->wiphy->frag_threshold = val;
		}
		val = le32_to_cpu(info_2->rts_threshold);
		if (val && val <= IEEE80211_MAX_RTS_THRESHOLD) {
			wiphy_dbg(sdev->wiphy, "FW rts_threshold: %d\n", val);
			sdev->wiphy->rts_threshold = val;
		}
	}

	dev_kfree_skb(rskb);

	return 0;
}

static void sc23xx_handle_connect(struct sc23xx_vif *vif, struct sk_buff *skb)
{
	struct wiphy *wiphy = vif->wdev.wiphy;
	struct sc23xx_evt_connect *hdr;
	struct cfg80211_roam_info roam_info = { };
	u16 req_ie_len, rsp_ie_len, bea_ie_len;
	const u8 *req_ie, *rsp_ie, *bea_ie;
	struct cfg80211_bss *bss = NULL;
	u8 type;

	if (skb->len < 3) {
		wiphy_err(wiphy, "connect event is too short\n");
		return;
	}

	type = *skb->data;
	if (type != SC23XX_CONNECT_DONE && type != SC23XX_ROAM_DONE) {
		u8 status_code = skb->data[2];

		if (!status_code)
			status_code = WLAN_STATUS_UNSPECIFIED_FAILURE;

		if (vif->state == SC23XX_STATE_CONNECTING) {
			cfg80211_connect_result(vif->wdev.netdev, vif->bssid,
						NULL, 0, NULL, 0, status_code,
						GFP_KERNEL);
		} else if (vif->state == SC23XX_STATE_CONNECTED) {
			cfg80211_disconnected(vif->wdev.netdev, status_code,
					      NULL, 0, false, GFP_KERNEL);
		}

		vif->state = SC23XX_STATE_DISCONNECTED;

		return;
	}

	hdr = skb_pull_data(skb, sizeof(*hdr));
	if (!hdr) {
		wiphy_err(wiphy, "connect event is too short\n");
		return;
	}

	sc23xx_get_ie(skb, &req_ie, &req_ie_len);
	sc23xx_get_ie(skb, &rsp_ie, &rsp_ie_len);

	if (sc23xx_get_ie(skb, &bea_ie, &bea_ie_len)) {
		int freq = sc23xx_channel_to_freq(hdr->channel);
		struct ieee80211_channel *channel = ieee80211_get_channel(wiphy, freq);

		if (!channel) {
			wiphy_err(wiphy, "connect on invalid channel %d (freq %d)\n",
				  hdr->channel, freq);
			return;
		}

		bss = cfg80211_inform_bss_frame(wiphy, channel, (void *)bea_ie, bea_ie_len,
						hdr->signal * 100, GFP_KERNEL);
	}

	if (hdr->type == SC23XX_CONNECT_DONE) {
		cfg80211_connect_bss(vif->wdev.netdev, hdr->bssid, bss,
				     req_ie, req_ie_len, rsp_ie, rsp_ie_len,
				     WLAN_STATUS_SUCCESS, GFP_KERNEL,
				     NL80211_TIMEOUT_UNSPECIFIED);
	} else {
		roam_info.req_ie = req_ie;
		roam_info.req_ie_len = req_ie_len;
		roam_info.resp_ie = rsp_ie;
		roam_info.resp_ie_len = rsp_ie_len;
		roam_info.links[0].bssid = hdr->bssid;
		roam_info.links[0].bss = bss;
		cfg80211_roamed(vif->wdev.netdev, &roam_info, GFP_KERNEL);
	}

	vif->state = SC23XX_STATE_CONNECTED;

}

static void sc23xx_handle_disconnect(struct sc23xx_vif *vif, struct sk_buff *skb)
{
	struct wiphy *wiphy = vif->wdev.wiphy;
	u16 status_code;

	if (skb->len < 2) {
		wiphy_err(wiphy, "disconnect event is too short\n");
		return;
	}

	status_code = le16_to_cpu(*(__le16 *)skb->data);

	if (!status_code)
		status_code = WLAN_STATUS_UNSPECIFIED_FAILURE;

	if (vif->state == SC23XX_STATE_CONNECTING) {
		cfg80211_connect_result(vif->wdev.netdev, vif->bssid,
					NULL, 0, NULL, 0, status_code,
					GFP_KERNEL);
	} else if (vif->state == SC23XX_STATE_CONNECTED) {
		cfg80211_disconnected(vif->wdev.netdev, status_code,
				      NULL, 0, false, GFP_KERNEL);
	}

	vif->state = SC23XX_STATE_DISCONNECTED;
}

static void sc23xx_handle_scan_done(struct sc23xx_vif *vif, struct sk_buff *skb)
{
	struct wiphy *wiphy = vif->wdev.wiphy;
	u8 type;

	if (skb->len == 0) {
		wiphy_err(wiphy, "scan done event is too short\n");
		return;
	}

	type = *skb->data;
	switch (type) {
	case SC23XX_NORMAL_SCAN_DONE:
		sc23xx_notify_scan_done(vif, false);
		break;
	case SC23XX_SCHED_SCAN_DONE:
		if (test_and_clear_bit(SC23XX_FLAG_SCHED_SCAN, &vif->flags))
			cfg80211_sched_scan_results(wiphy, 0);
		break;
	case SC23XX_SCAN_ERROR:
		if (test_and_clear_bit(SC23XX_FLAG_SCHED_SCAN, &vif->flags))
			cfg80211_sched_scan_stopped(wiphy, 0);
		fallthrough;
	case SC23XX_SCAN_ABORT_DONE:
		sc23xx_notify_scan_done(vif, true);
		break;
	default:
		wiphy_err(wiphy, "unhandled scan done event type %d\n", type);
		break;
	}
}

static void sc23xx_handle_mgmt_frame(struct sc23xx_vif *vif, struct sk_buff *skb)
{
	struct wiphy *wiphy = vif->wdev.wiphy;
	struct sc23xx_evt_mgmt_frame *hdr;
	struct ieee80211_channel *channel;
	struct cfg80211_bss *bss;
	int freq, frame_len;

	hdr = skb_pull_data(skb, sizeof(*hdr));
	if (!hdr) {
		wiphy_err(wiphy, "MGMT frame event is too short\n");
		return;
	}

	freq = sc23xx_channel_to_freq(hdr->channel);
	channel = ieee80211_get_channel(wiphy, freq);
	if (!channel) {
		wiphy_err(wiphy, "MGMT frame on invalid channel %d (freq %d)\n",
			  hdr->channel, freq);
		return;
	}

	frame_len = le16_to_cpu(hdr->len);
	if (skb->len < frame_len) {
		wiphy_err(wiphy, "MGMT frame: length %d but got only %d bytes\n",
			  frame_len, skb->len);
		return;
	}

	switch (hdr->type) {
	case SC23XX_MGMT_TYPE_NORMAL:
		cfg80211_rx_mgmt(&vif->wdev, freq, 0, skb->data, frame_len, 0);
		break;
	case SC23XX_MGMT_TYPE_DEAUTH:
	case SC23XX_MGMT_TYPE_DISASSOC:
		cfg80211_rx_unprot_mlme_mgmt(vif->wdev.netdev, skb->data, frame_len);
		break;
	case SC23XX_MGMT_TYPE_SCAN:
		bss = cfg80211_inform_bss_frame(wiphy, channel, (void *)skb->data, frame_len,
						hdr->signal * 100, GFP_KERNEL);
		cfg80211_put_bss(wiphy, bss);
		break;
	default:
		wiphy_err(wiphy, "unhandled MGMT frame type %d\n", hdr->type);
		break;
	}
}

static void sc23xx_handle_mgmt_tx_status(struct sc23xx_vif *vif, struct sk_buff *skb)
{
	struct wiphy *wiphy = vif->wdev.wiphy;
	struct sc23xx_evt_mgmt_tx_status *hdr;
	int frame_len;

	hdr = skb_pull_data(skb, sizeof(*hdr));
	if (!hdr) {
		wiphy_err(wiphy, "MGMT tx status event is too short\n");
		return;
	}

	frame_len = get_unaligned_le16(&hdr->len);
	if (skb->len < frame_len) {
		wiphy_err(wiphy, "MGMT tx status: length %d but got only %d bytes\n",
			  frame_len, skb->len);
		return;
	}

	cfg80211_mgmt_tx_status(&vif->wdev, get_unaligned_le64(&hdr->cookie),
				skb->data, frame_len, hdr->ack, GFP_KERNEL);
}

static void sc23xx_handle_ba_mgmt(struct sc23xx_dev *sdev, struct sk_buff *skb)
{
	struct sc23xx_evt_ba *hdr;
	struct sc23xx_sta *sta;

	hdr = skb_pull_data(skb, sizeof(*hdr));
	if (!hdr) {
		wiphy_err(sdev->wiphy, "BA event is too short\n");
		return;
	}

	if (hdr->sta_lut_index < SC23XX_STA_IDX_MIN ||
	    hdr->sta_lut_index > SC23XX_STA_IDX_MAX) {
		wiphy_err(sdev->wiphy, "BA event: STA index %d out of range\n",
			  hdr->sta_lut_index);
		return;
	}

	switch (hdr->type) {
	case SC23XX_ADDBA_REQ:
		sc23xx_rx_addba_req(sdev, hdr->sta_lut_index, hdr->tid,
				    le16_to_cpu(hdr->win.start),
				    le16_to_cpu(hdr->win.size));
		break;
	case SC23XX_DELBA_REQ:
		sc23xx_rx_delba_req(sdev, hdr->sta_lut_index, hdr->tid);
		break;
	case SC23XX_DELTXBA:
		wiphy_dbg(sdev->wiphy, "tx delba for sta %d\n",
			  hdr->sta_lut_index);
		sta = &sdev->sta[hdr->sta_lut_index - SC23XX_STA_IDX_MIN];
		sta->addba_retries = 3;
		if (sta->ht_enabled)
			queue_delayed_work(sdev->evt_wq, &sta->tx_ba_setup,
					   msecs_to_jiffies(1000));
		break;
	default:
		wiphy_warn(sdev->wiphy, "unhandled BA event: %d\n", hdr->type);
		break;
	}
}

static void sc23xx_handle_sta_lut_index(struct sc23xx_dev *sdev, struct sk_buff *skb)
{
	struct sc23xx_evt_sta_lut_ind *hdr;
	struct sc23xx_sta *sta;

	hdr = skb_pull_data(skb, sizeof(*hdr));
	if (!hdr) {
		wiphy_err(sdev->wiphy, "STA LUT index event is too short\n");
		return;
	}

	if (hdr->sta_lut_index < SC23XX_STA_IDX_MIN ||
	    hdr->sta_lut_index > SC23XX_STA_IDX_MAX) {
		wiphy_err(sdev->wiphy, "STA LUT index %d out of range\n",
			  hdr->sta_lut_index);
		return;
	}

	spin_lock_irq(&sdev->sta_lock);
	sta = &sdev->sta[hdr->sta_lut_index - SC23XX_STA_IDX_MIN];
	if (hdr->action == SC23XX_ADD_LUT_IDX ||
	    hdr->action == SC23XX_UPD_LUT_IDX) {
		sta->valid = true;
		sta->ht_enabled = hdr->ht_enabled;
		sta->addba_retries = 3;
		sta->ctx_id = hdr->ctx_id;
		memcpy(sta->addr, hdr->addr, ETH_ALEN);
	} else {
		sta->valid = false;
		sta->ht_enabled = false;
	}
	spin_unlock_irq(&sdev->sta_lock);

	if (sta->ht_enabled)
		queue_delayed_work(sdev->evt_wq, &sta->tx_ba_setup, 0);
	else
		cancel_delayed_work(&sta->tx_ba_setup);
}

void sc23xx_handle_event(struct work_struct *work)
{
	struct sc23xx_event *evt =
		container_of(work, struct sc23xx_event, work);
	struct sc23xx_cmd_hdr *hdr = (void *)evt->skb->data;
	struct sc23xx_vif *vif;
	u16 payload_len;
	int srcu_idx;

	if (evt->skb->len < sizeof(*hdr)) {
		wiphy_err(evt->sdev->wiphy, "event too short, only %d bytes\n",
			  evt->skb->len);
		goto out_free;
	}

	payload_len = le16_to_cpu(hdr->len);
	if (evt->skb->len < payload_len) {
		wiphy_err(evt->sdev->wiphy,
			  "event %d: length %d but got only %d bytes\n",
			  hdr->cmd, payload_len, evt->skb->len);
		goto out_free;
	}

	skb_trim(evt->skb, payload_len);
	skb_pull(evt->skb, sizeof(*hdr));

	switch (hdr->cmd) {
	case EVT_BA:
		sc23xx_handle_ba_mgmt(evt->sdev, evt->skb);
		goto out_free;
	case EVT_STA_LUT_INDEX:
		sc23xx_handle_sta_lut_index(evt->sdev, evt->skb);
		goto out_free;
	default:
		break;
	}

	srcu_idx = srcu_read_lock(&evt->sdev->vif_srcu);

	vif = sc23xx_get_vif(evt->sdev, hdr->common.ctx_id);
	if (!vif)
		goto out_unlock;

	switch (hdr->cmd) {
	case EVT_CONNECT:
		sc23xx_handle_connect(vif, evt->skb);
		break;
	case EVT_DISCONNECT:
		sc23xx_handle_disconnect(vif, evt->skb);
		break;
	case EVT_SCAN_DONE:
		sc23xx_handle_scan_done(vif, evt->skb);
		break;
	case EVT_MGMT_FRAME:
		sc23xx_handle_mgmt_frame(vif, evt->skb);
		break;
	case EVT_MGMT_TX_STATUS:
		sc23xx_handle_mgmt_tx_status(vif, evt->skb);
		break;
	default:
		netdev_dbg(vif->wdev.netdev, "event %d\n", hdr->cmd);
		break;
	}

out_unlock:
	srcu_read_unlock(&evt->sdev->vif_srcu, srcu_idx);

out_free:
	dev_kfree_skb(evt->skb);
	kfree(evt);
}
