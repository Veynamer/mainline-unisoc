// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Otto Pflüger
 *
 * The definitions in this file are mostly based on headers from the
 * Unisoc BSP WLAN module (modules/wcn/wlan/wlan_combo).
 * Copyright (C) 2021-2023 Unisoc (Shanghai) Technologies Co. Ltd
 */

#ifndef __SC23XX_PROTOCOL_H
#define __SC23XX_PROTOCOL_H

enum sc23xx_hdr_type {
	SC23XX_HDR_TYPE_CMD = 0,
	SC23XX_HDR_TYPE_EVENT,
	SC23XX_HDR_TYPE_DATA,
	SC23XX_HDR_TYPE_SPECIAL_DATA,
	SC23XX_HDR_TYPE_ADDR_LIST,
};

struct sc23xx_hdr {
#ifdef __LITTLE_ENDIAN_BITFIELD
	u8 type:3;
	u8 data_dir:1;
	u8 need_rsp:1;
	u8 ctx_id:3;
#else
	u8 ctx_id:3;
	u8 need_rsp:1;
	u8 data_dir:1;
	u8 type:3;
#endif
} __packed;

/* sc23xx_rx_data_hdr.flags_0 */
#define SC23XX_RX_LAST_MSDU	BIT(1)
#define SC23XX_RX_STA_LUT_IDX	GENMASK(15, 11)
/* sc23xx_rx_data_hdr.flags_1 */
#define SC23XX_RX_BROADCAST	BIT(4)
#define SC23XX_RX_QOS_FLAG	BIT(13)
/* sc23xx_rx_data_hdr.seq_info */
#define SC23XX_RX_TID		GENMASK(3, 0)
#define SC23XX_RX_SEQ_NUM	GENMASK(15, 4)

struct sc23xx_rx_data_hdr {
	struct sc23xx_hdr common;
	u8 offset;
	__le16 pkt_len;
	__le32 buf_addr_l;
	u8 buf_addr_h;
	u8 mpdu_index;
	__le16 flags_0;
	__le16 flags_1;
	__le16 seq_info;
	__le32 pn_l;
	__le16 pn_h;
} __packed;

enum {
	SC23XX_TX_CHECKSUM_OFFLOAD = BIT(0),
	SC23XX_TX_CHECKSUM_TCP = BIT(1),
	SC23XX_TX_NO_AGGREGATION = BIT(2),
};

struct sc23xx_tx_data_hdr {
	struct sc23xx_hdr common;
	u8 offset;
	u8 tx_flags;
	__le16 pkt_len;
#ifdef __LITTLE_ENDIAN_BITFIELD
	u8 msdu_tid:4;
	u8 mac_data_offset:4;
#else
	u8 mac_data_offset:4;
	u8 msdu_tid:4;
#endif
	u8 sta_lut_index;
	__le16 seq_info;
	__le16 tcp_udp_header_offset;
} __packed;

#define SC23XX_DMA_TXC_OFFSET		0x10
#define SC23XX_RX_DMA_HEADER_SIZE	0x1c

struct sc23xx_addr {
	__le32 l;
	u8 h;
} __packed;

struct sc23xx_rx_addr_tlv {
	u8 type;
	u8 addr_count;
	struct sc23xx_addr addr[];
} __packed;

struct sc23xx_rx_addr_free_list {
	struct sc23xx_hdr hdr;
	struct sc23xx_rx_addr_tlv data;
} __packed;

struct sc23xx_rx_addr_hdr {
	struct sc23xx_hdr common;
	__le32 timestamp;
	__le16 seq_num;
	u8 tlv_count;
} __packed;

#define SC23XX_TX_BUF_IN_USE BIT(7)

struct sc23xx_tx_addr_list {
	struct sc23xx_hdr hdr;
	u8 offset;
	u8 flags;
	__le16 count;
	__le16 reserved;
	struct sc23xx_addr addr[];
} __packed;

enum {
	SC23XX_CMD_STATUS_ARG_ERROR = -1,
	SC23XX_CMD_STATUS_GET_RESULT_ERROR = -2,
	SC23XX_CMD_STATUS_EXEC_ERROR = -3,
	SC23XX_CMD_STATUS_MALLOC_ERROR = -4,
	SC23XX_CMD_STATUS_WIFIMODE_ERROR = -5,
	SC23XX_CMD_STATUS_ERROR = -6,
	SC23XX_CMD_STATUS_CANNOT_EXEC_ERROR = -7,
	SC23XX_CMD_STATUS_NOT_SUPPORT_ERROR = -8,
	SC23XX_CMD_STATUS_CRC_ERROR = -9,
	SC23XX_CMD_STATUS_INI_INDEX_ERROR = -10,
	SC23XX_CMD_STATUS_LENGTH_ERROR = -11,
	SC23XX_CMD_STATUS_OTHER_ERROR = -127,
};

struct sc23xx_cmd_hdr {
	struct sc23xx_hdr common;
	u8 cmd;
	__le16 len; /* includes header size */
	__le32 timestamp;
	s8 status;
	u8 rsp_cnt;
	u8 reserved[2];
} __packed;

enum {
	CMD_GET_INFO = 0x01,
	CMD_SET_REGDOM = 0x02,
	CMD_OPEN = 0x03,
	CMD_CLOSE = 0x04,
	CMD_POWER_SAVE = 0x05,
	CMD_CONNECT = 0x0a,
	CMD_SCAN = 0x0b,
	CMD_SCHED_SCAN = 0x0c,
	CMD_DISCONNECT = 0x0d,
	CMD_KEY = 0x0e,
	CMD_TX_MGMT = 0x15,
	CMD_SET_IE = 0x19,
	CMD_NOTIFY_IP_ACQUIRED = 0x1a,
	CMD_MULTICAST_FILTER = 0x27,
	CMD_ADDBA_REQ = 0x28,
	CMD_SET_MAC = 0x40,
	CMD_BA = 0x44,
	CMD_TX_DATA = 0x48,
	CMD_DOWNLOAD_INI = 0x4c,

	EVT_CONNECT = 0x80,
	EVT_DISCONNECT = 0x81,
	EVT_SCAN_DONE = 0x82,
	EVT_MGMT_FRAME = 0x83,
	EVT_MGMT_TX_STATUS = 0x84,
	EVT_BA = 0xf3,
	EVT_STA_LUT_INDEX = 0xf5,
};

enum {
	SUBCMD_GET = 1,
	SUBCMD_SET,
	SUBCMD_ADD,
	SUBCMD_DEL,
	SUBCMD_FLUSH,
	SUBCMD_UPDATE,
	SUBCMD_ENABLE,
	SUBCMD_DISABLE,
	SUBCMD_REKEY,
};

struct sc23xx_reg_rule {
	__le32 start_freq_khz;
	__le32 end_freq_khz;
	__le32 max_bandwidth_khz;
	__le32 max_antenna_gain;
	__le32 max_eirp;
	__le32 flags;
	__le32 dfs_cac_ms;
} __packed;

struct sc23xx_req_open {
	u8 mode;
	u8 reserved;
	u8 mac[ETH_ALEN];
} __packed;

struct sc23xx_req_close {
	u8 mode;
} __packed;

enum {
	SC23XX_POWER_SAVE_SUSPEND_RESUME = 5,
};

struct sc23xx_req_scan {
	__le32 channel_mask;
	__le32 abort;
	__le16 ssids_len;
} __packed;

struct sc23xx_req_sched_scan {
	__le16 start;
	__le16 buf_flags;
} __packed;

enum {
	SC23XX_SCHED_SCAN_IFRC = BIT(0),
	SC23XX_SCHED_SCAN_SSID = BIT(1),
	SC23XX_SCHED_SCAN_MATCH_SSID = BIT(2),
	SC23XX_SCHED_SCAN_IE = BIT(4),
};

struct sc23xx_sched_scan_ie_hdr {
	__le16 type;
	__le16 len;
} __packed;

struct sc23xx_sched_scan_ifrc_ie {
	__le32 interval;
	__le32 flags;
	__le32 rssi_thold;
	u8 n_channels;
	u8 channels[38];
} __packed;

enum {
	SC23XX_AUTH_OPEN = 0,
	SC23XX_AUTH_SHARED = 1,
	SC23XX_AUTH_SAE = 4,
};

struct sc23xx_req_connect {
	__le32 wpa_versions;
	u8 bssid[ETH_ALEN];
	u8 channel;
	u8 auth_type;
	u8 pairwise_cipher;
	u8 group_cipher;
	u8 key_mgmt;
	u8 mfp_enable;
	u8 psk_len;
	u8 ssid_len;
	u8 psk[WLAN_MAX_KEY_LEN];
	u8 ssid[IEEE80211_MAX_SSID_LEN];
} __packed;

struct sc23xx_req_disconnect {
	__le16 reason_code;
} __packed;

struct sc23xx_req_add_key {
	u8 subcmd; /* SUBCMD_ADD */
	u8 key_index;
	u8 pairwise;
	u8 mac[ETH_ALEN];
	u8 key_seq[16];
	u8 cipher_type;
	u8 key_len;
} __packed;

struct sc23xx_req_del_key {
	u8 subcmd; /* SUBCMD_DEL */
	u8 key_index;
	u8 pairwise;
	u8 mac[ETH_ALEN];
} __packed;

struct sc23xx_req_set_def_key {
	u8 subcmd; /* SUBCMD_SET */
	u8 key_index;
} __packed;

struct sc23xx_req_set_rekey_data {
	u8 subcmd; /* SUBCMD_REKEY */
	u8 kek[NL80211_KEK_LEN];
	u8 kck[NL80211_KCK_LEN];
	u8 replay_ctr[NL80211_REPLAY_CTR_LEN];
} __packed;

struct sc23xx_req_mgmt_tx {
	u8 chan;
	u8 dont_wait_for_ack;
	__le32 wait;
	__le64 cookie;
	__le16 len;
} __packed;

enum {
	SC23XX_IE_BEACON = 0,
	SC23XX_IE_PROBE_REQ,
	SC23XX_IE_PROBE_RESP,
	SC23XX_IE_ASSOC_REQ,
	SC23XX_IE_ASSOC_RESP,
	SC23XX_IE_BEACON_HEAD,
	SC23XX_IE_BEACON_TAIL,
	SC23XX_IE_SAE,
};

struct sc23xx_req_tx_addba {
	u8 sta_lut_index;
	u8 addr[ETH_ALEN];
	u8 dialog_token;
	__le16 addba_param;
	__le16 timeout;
} __packed;

enum {
	SC23XX_DISABLE_SCAN_RANDOM_ADDR = 0,
	SC23XX_ENABLE_SCAN_RANDOM_ADDR,
	SC23XX_CONNECT_RANDOM_ADDR,
};

struct sc23xx_req_set_mac_addr {
	u8 mode;
	u8 mac_addr[ETH_ALEN];
} __packed;

enum {
	SC23XX_ADDBA_REQ = 0,
	SC23XX_ADDBA_RSP,
	SC23XX_DELBA_REQ,
	SC23XX_BAR,
	SC23XX_FILTER,
	SC23XX_DELBA_ALL,
	SC23XX_DELTXBA,
};

struct sc23xx_req_ba {
	u8 type;
	u8 tid;
	u8 addr[ETH_ALEN];
	u8 success;
} __packed;

struct sc23xx_rsp_fw_info_1 {
	__le32 chip_model;
	__le32 chip_version;
	__le32 fw_version;
	__le32 fw_std;
	__le32 fw_capa;
	u8 max_ap_assoc_sta;
	u8 max_acl_mac_addrs;
	u8 max_mc_mac_addrs;
	u8 wnm_ft_support;
} __packed;

struct sc23xx_rsp_fw_info_2 {
	__le16 ht_cap_info;
	__le16 ampdu_params;
	struct ieee80211_mcs_info ht_mcs;
	__le32 vht_cap_info;
	struct ieee80211_vht_mcs_info vht_mcs;
	__le32 antenna_tx;
	__le32 antenna_rx;
	u8 retry_short;
	u8 retry_long;
	u8 reserved[2];
	__le32 frag_threshold;
	__le32 rts_threshold;
} __packed;

enum {
	SC23XX_NORMAL_SCAN_DONE = 1,
	SC23XX_SCHED_SCAN_DONE,
	SC23XX_SCAN_ERROR,
	SC23XX_GSCAN_DONE,
	SC23XX_SCAN_ABORT_DONE,
};

enum {
	SC23XX_CONNECT_DONE = 0,
	SC23XX_ROAM_DONE = 2,
};

struct sc23xx_evt_connect {
	u8 type;
	u8 bssid[ETH_ALEN];
	u8 channel;
	s8 signal;
} __packed;

enum {
	SC23XX_MGMT_TYPE_NORMAL = 1,
	SC23XX_MGMT_TYPE_DEAUTH,
	SC23XX_MGMT_TYPE_DISASSOC,
	SC23XX_MGMT_TYPE_SCAN,
};

struct sc23xx_evt_mgmt_frame {
	u8 type;
	u8 channel;
	s8 signal;
	u8 reserved;
	u8 bssid[ETH_ALEN];
	__le16 len;
} __packed;

struct sc23xx_evt_mgmt_tx_status {
	__le64 cookie;
	u8 ack;
	__le16 len;
} __packed;

struct sc23xx_evt_ba {
	u8 type;
	u8 tid;
	u8 sta_lut_index;
	u8 reserved;
	union {
		struct {
			__le16 start;
			__le16 size;
		} win;
		struct {
			__le16 seq_num;
		} msdu;
	} __packed;
} __packed;

enum {
	SC23XX_DEL_LUT_IDX = 0,
	SC23XX_ADD_LUT_IDX,
	SC23XX_UPD_LUT_IDX,
};

struct sc23xx_evt_sta_lut_ind {
	u8 ctx_id;
	u8 action;
	u8 sta_lut_index;
	u8 addr[ETH_ALEN];
	u8 ht_enabled;
	u8 vht_enabled;
} __packed;

static inline u32 sc23xx_convert_wpa(enum nl80211_wpa_versions versions)
{
	u32 ret = 0;

	if (versions & NL80211_WPA_VERSION_1)
		ret |= BIT(0);
	if (versions & NL80211_WPA_VERSION_2)
		ret |= BIT(1);
	if (versions & NL80211_WPA_VERSION_3)
		ret |= BIT(3);

	return ret;
}

#define SC23XX_CRYPTO_SUITE_MASK	GENMASK(6, 0)
#define SC23XX_CRYPTO_VALID		BIT(7)

static inline u8 sc23xx_convert_cipher(u32 suite)
{
	u8 ret;

	switch (suite) {
	case WLAN_CIPHER_SUITE_WEP40:
		ret = 1;
		break;
	case WLAN_CIPHER_SUITE_WEP104:
		ret = 2;
		break;
	case WLAN_CIPHER_SUITE_TKIP:
		ret = 3;
		break;
	case WLAN_CIPHER_SUITE_CCMP:
		ret = 4;
		break;
	case WLAN_CIPHER_SUITE_SMS4:
		ret = 7;
		break;
	case WLAN_CIPHER_SUITE_AES_CMAC:
		ret = 8;
		break;
	case WLAN_CIPHER_SUITE_GCMP_256:
		ret = 9;
		break;
	default:
		return 0;
	}

	return ret | SC23XX_CRYPTO_VALID;
}

static inline u8 sc23xx_convert_akm(u32 suite)
{
	switch (suite) {
	case WLAN_AKM_SUITE_8021X:
	case WLAN_AKM_SUITE_PSK:
	case WLAN_AKM_SUITE_FT_8021X:
	case WLAN_AKM_SUITE_FT_PSK:
	case WLAN_AKM_SUITE_8021X_SHA256:
	case WLAN_AKM_SUITE_SAE:
	case WLAN_AKM_SUITE_OWE:
		return (suite & 0x7f) | SC23XX_CRYPTO_VALID;
	default:
		return 0;
	}
}

#endif
