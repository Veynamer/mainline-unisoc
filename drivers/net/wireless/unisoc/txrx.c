// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Otto Pflüger
 */

#include <linux/of_reserved_mem.h>
#include <net/ip.h>

#include "cmd.h"
#include "txrx.h"
#include "vif.h"

#define SC23XX_MAX_DATA_RXLEN	1676
#define SC23XX_MAX_NUM_RX_BUF	500

/* The module parameters may be modified at runtime, but only
 * increasing them is guaranteed to have the desired effect.
 */

static unsigned int max_tx_buf_param = 200;

module_param_named(max_tx_buf, max_tx_buf_param, uint, 0600);
MODULE_PARM_DESC(max_tx_buf_param, "Maximum number of TX buffers");

static unsigned int num_rx_buf_param = 200;

module_param_named(num_rx_buf, num_rx_buf_param, uint, 0600);
MODULE_PARM_DESC(num_rx_buf_param, "Number of RX buffers to allocate");

struct sc23xx_buf {
	struct list_head listnode;
	struct hlist_node hashnode;
	struct sk_buff *skb;
	dma_addr_t dma_addr;
	struct sc23xx_addr addr;
};

static inline void sc23xx_buf_set_addr(struct sc23xx_buf *buf,
				       dma_addr_t dma_addr)
{
	buf->dma_addr = dma_addr;

	put_unaligned_le32((u32)dma_addr, &buf->addr.l);
	buf->addr.h = (u8)(dma_addr >> 32) | 0x80;
}

static inline dma_addr_t sc23xx_addr_to_dma(struct sc23xx_addr *addr)
{
	return (((u64)(addr->h & 0x7f) << 32) |
	       get_unaligned_le32(&addr->l));
}

void sc23xx_rx_now(struct sc23xx_dev *sdev, struct sk_buff *skb)
{
	struct sc23xx_rx_data_hdr *hdr = (void *)skb->data;
	struct sc23xx_vif *vif;
	struct net_device *ndev;
	int srcu_idx;
	u16 len;

	srcu_idx = srcu_read_lock(&sdev->vif_srcu);

	vif = sc23xx_get_vif(sdev, hdr->common.ctx_id);
	if (!vif)
		goto out_unlock;
	ndev = vif->wdev.netdev;

	len = le16_to_cpu(hdr->pkt_len);
	if (hdr->offset < sizeof(*hdr) || skb->len < (hdr->offset + len)) {
		netdev_err(ndev, "data packet offset/length invalid\n");
		dev_kfree_skb_any(skb);
		goto out_unlock;
	}

	skb_pull(skb, hdr->offset);
	skb_trim(skb, len);

	skb->dev = ndev;
	skb->protocol = eth_type_trans(skb, ndev);
	ndev->stats.rx_packets++;
	ndev->stats.rx_bytes += len;
	netif_rx(skb);

out_unlock:
	srcu_read_unlock(&sdev->vif_srcu, srcu_idx);
}

static void sc23xx_rx_single_addr(struct sc23xx_dev *sdev,
				  struct sc23xx_addr *addr)
{
	struct device *dev = wiphy_dev(sdev->wiphy);
	dma_addr_t dma_addr = sc23xx_addr_to_dma(addr);
	struct sc23xx_buf *buf;
	struct sc23xx_rx_data_hdr *hdr;

	hash_for_each_possible(sdev->rx_buf_table, buf, hashnode, dma_addr) {
		if (buf->dma_addr != dma_addr)
			continue;

		hash_del(&buf->hashnode);
		dma_unmap_single(dev, dma_addr, buf->skb->len,
				 DMA_BIDIRECTIONAL);
		atomic_dec(&sdev->rx_buf_count);
		wake_up(&sdev->tx_wait);

		if (!WARN_ON(!skb_pull(buf->skb, SC23XX_RX_DMA_HEADER_SIZE)) &&
		    !WARN_ON(buf->skb->len < sizeof(*hdr))) {
			hdr = (void *)buf->skb->data;
			hdr->offset -= SC23XX_RX_DMA_HEADER_SIZE;
			sc23xx_rx_reorder(sdev, buf->skb);
		}

		kfree(buf);
		return;
	}

	wiphy_err(sdev->wiphy, "RX buffer %010llx not found\n", dma_addr);
}

static void sc23xx_rx_addr_list(struct sc23xx_dev *sdev, struct sk_buff *skb)
{
	struct sc23xx_rx_addr_hdr *hdr;
	u8 i, j;

	hdr = skb_pull_data(skb, sizeof(*hdr));
	if (!hdr) {
		wiphy_err(sdev->wiphy, "RX address message too short\n");
		return;
	}

	spin_lock(&sdev->rx_buf_lock);
	for (i = 0; i < hdr->tlv_count; i++) {
		struct sc23xx_rx_addr_tlv *tlv;

		tlv = skb_pull_data(skb, sizeof(*tlv));
		if (!tlv || !skb_pull(skb, tlv->addr_count *
					sizeof(struct sc23xx_addr))) {
			wiphy_err(sdev->wiphy, "invalid RX address TLV\n");
			break;
		}

		if (tlv->type) {
			wiphy_warn(sdev->wiphy,
				   "unsupported RX address TLV type %d\n",
				   tlv->type);
			continue;
		}

		for (j = 0; j < tlv->addr_count; j++)
			sc23xx_rx_single_addr(sdev, &tlv->addr[j]);
	}
	spin_unlock(&sdev->rx_buf_lock);
}

/* Called with tx_buf_lock held */
static void sc23xx_allow_tx(struct sc23xx_dev *sdev, bool allow)
{
	struct sc23xx_vif *vif;
	int i;

	sdev->tx_blocked = !allow;

	for (i = 0; i < SC23XX_VIF_NUM; i++) {
		vif = srcu_dereference(sdev->vif[i], &sdev->vif_srcu);
		if (vif && test_bit(SC23XX_FLAG_OPENED, &vif->flags)) {
			if (allow)
				netif_wake_queue(vif->wdev.netdev);
			else
				netif_stop_queue(vif->wdev.netdev);
		}
	}
}

/* May be called from interrupt context */
static void sc23xx_tx_complete(struct sc23xx_dev *sdev, struct sk_buff *skb)
{
	struct sc23xx_tx_addr_list *list = (void *)skb->data;
	struct device *dev = wiphy_dev(sdev->wiphy);
	struct sc23xx_buf *buf;
	unsigned int i, count;
	unsigned long flags;
	bool found;

	if (skb->len < sizeof(*list)) {
		wiphy_err(sdev->wiphy, "TX complete message too short\n");
		return;
	}

	count = get_unaligned_le16(&list->count);
	if (skb->len < (sizeof(*list) + count * sizeof(struct sc23xx_addr))) {
		wiphy_err(sdev->wiphy, "TX complete message length invalid\n");
		return;
	}

	spin_lock_irqsave(&sdev->tx_buf_lock, flags);
	for (i = 0; i < count; i++) {
		struct sc23xx_addr *addr = &list->addr[i];
		dma_addr_t dma_addr = sc23xx_addr_to_dma(addr) -
				      SC23XX_DMA_TXC_OFFSET;

		found = false;

		hash_for_each_possible(sdev->tx_buf_table, buf, hashnode,
				       dma_addr) {
			if (buf->dma_addr != dma_addr)
				continue;

			hash_del(&buf->hashnode);
			dma_unmap_single(dev, dma_addr, buf->skb->len,
					 DMA_TO_DEVICE);
			dev_kfree_skb_any(buf->skb);

			sdev->tx_buf_count--;
			if (sdev->tx_blocked &&
			    sdev->tx_buf_count < max_tx_buf_param)
				sc23xx_allow_tx(sdev, true);

			list_add_tail(&buf->listnode, &sdev->tx_buf_cache);

			found = true;
			break;
		}

		if (!found)
			wiphy_err(sdev->wiphy,
				  "TX complete buffer %010llx not found\n",
				  dma_addr);
	}
	spin_unlock_irqrestore(&sdev->tx_buf_lock, flags);
}

/* May be called from interrupt context */
void sc23xx_rx_msg(struct sc23xx_dev *sdev, enum sc23xx_msg_type type,
		   struct sk_buff *skb)
{
	struct sc23xx_hdr *hdr;

	if (skb->len < sizeof(*hdr)) {
		wiphy_warn(sdev->wiphy, "packet too short\n");
		dev_kfree_skb_any(skb);
		return;
	}

	hdr = (struct sc23xx_hdr *)skb->data;

	if (type == SC23XX_MSG_TYPE_CMD) {
		if (hdr->type == SC23XX_HDR_TYPE_CMD) {
			skb_queue_tail(&sdev->rsp_q, skb);
			wake_up(&sdev->rsp_wait);
		} else if (hdr->type == SC23XX_HDR_TYPE_EVENT) {
			struct sc23xx_event *evt;

			evt = kmalloc(sizeof(*evt), GFP_ATOMIC);
			if (evt) {
				evt->sdev = sdev;
				evt->skb = skb;
				INIT_WORK(&evt->work, sc23xx_handle_event);
				queue_work(sdev->evt_wq, &evt->work);
			} else {
				dev_kfree_skb_any(skb);
			}
		} else {
			wiphy_warn(sdev->wiphy, "ignoring command packet with type %d\n",
				   hdr->type);
			dev_kfree_skb_any(skb);
		}
	} else if (hdr->type == SC23XX_HDR_TYPE_ADDR_LIST) {
		if (hdr->data_dir)
			sc23xx_rx_addr_list(sdev, skb);
		else
			sc23xx_tx_complete(sdev, skb);
		dev_kfree_skb_any(skb);
	} else if (hdr->type != SC23XX_HDR_TYPE_DATA &&
		   hdr->type != SC23XX_HDR_TYPE_SPECIAL_DATA) {
		wiphy_warn(sdev->wiphy, "ignoring data packet with type %d\n",
			   hdr->type);
		dev_kfree_skb_any(skb);
	} else {
		sc23xx_rx_reorder(sdev, skb);
	}
}
EXPORT_SYMBOL_GPL(sc23xx_rx_msg);

/* Called from ndo_start_xmit (softirqs disabled) */
void sc23xx_tx_prepare(struct sc23xx_vif *vif, struct sk_buff *skb)
{
	struct ethhdr *ethhdr = (void *)skb->data;
	struct sc23xx_tx_data_hdr *hdr;
	unsigned long flags;
	u16 tx_len = skb->len;
	u8 i, tx_flags = 0;

	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		u8 protocol = 0;

		if (skb->protocol == ETH_P_IPV6)
			protocol = ipv6_hdr(skb)->nexthdr;
		else if (skb->protocol == ETH_P_IP)
			protocol = ip_hdr(skb)->protocol;

		if (protocol == IPPROTO_TCP)
			tx_flags |= SC23XX_TX_CHECKSUM_OFFLOAD |
				    SC23XX_TX_CHECKSUM_TCP;
		else if (protocol == IPPROTO_UDP)
			tx_flags |= SC23XX_TX_CHECKSUM_OFFLOAD;
	}

	hdr = skb_push(skb, sizeof(*hdr));
	memset(hdr, 0, sizeof(*hdr));

	hdr->common.type = SC23XX_HDR_TYPE_DATA;
	hdr->common.ctx_id = vif->idx;
	put_unaligned_le16(tx_len, &hdr->pkt_len);
	hdr->offset = sizeof(*hdr);
	hdr->tx_flags = tx_flags;
	put_unaligned_le16(skb->transport_header - skb->mac_header,
			   &hdr->tcp_udp_header_offset);

	spin_lock_irqsave(&vif->sdev->sta_lock, flags);
	for (i = SC23XX_STA_IDX_MIN; i <= SC23XX_STA_IDX_MAX; i++) {
		struct sc23xx_sta *sta = &vif->sdev->sta[i - SC23XX_STA_IDX_MIN];

		if (!sta->valid || sta->ctx_id != vif->idx)
			continue;

		/* use first entry as fallback in STA mode */
		if (vif->mode != SC23XX_MODE_AP && !hdr->sta_lut_index)
			hdr->sta_lut_index = i;

		if (!memcmp(sta->addr, ethhdr->h_dest, ETH_ALEN)) {
			hdr->sta_lut_index = i;
			break;
		}
	}
	spin_unlock_irqrestore(&vif->sdev->sta_lock, flags);

	if (!hdr->sta_lut_index) {
		if (vif->mode == SC23XX_MODE_AP)
			hdr->sta_lut_index = SC23XX_STA_IDX_AP_MULTICAST;
		else
			netdev_err(vif->wdev.netdev,
			           "no STA index found - connection lost?\n");
	}
}

/* Called from ndo_start_xmit (softirqs disabled) */
void sc23xx_tx_data_dma(struct sc23xx_dev *sdev, struct sk_buff *skb)
{
	struct device *dev = wiphy_dev(sdev->wiphy);
	struct sc23xx_buf *buf;
	dma_addr_t dma_addr;

	if (WARN_ON(!skb_push(skb, sizeof(struct sc23xx_addr)))) {
		dev_kfree_skb_any(skb);
		return;
	}

	dma_addr = dma_map_single(dev, skb->data, skb->len,
				  DMA_TO_DEVICE);
	if (dma_mapping_error(dev, dma_addr)) {
		wiphy_err_ratelimited(sdev->wiphy, "tx DMA mapping error\n");
		dev_kfree_skb_any(skb);
		return;
	}

	spin_lock_irq(&sdev->tx_buf_lock);

	if (list_empty(&sdev->tx_buf_cache)) {
		buf = kmalloc(sizeof(*buf), GFP_ATOMIC);
		if (!buf) {
			dma_unmap_single(dev, dma_addr, skb->len,
					 DMA_TO_DEVICE);
			dev_kfree_skb_any(skb);
			spin_unlock(&sdev->tx_buf_lock);
			return;
		}
		INIT_LIST_HEAD(&buf->listnode);
		INIT_HLIST_NODE(&buf->hashnode);
	} else {
		buf = list_first_entry(&sdev->tx_buf_cache, struct sc23xx_buf,
				       listnode);
		list_del_init(&buf->listnode);
	}
	buf->skb = skb;
	sc23xx_buf_set_addr(buf, dma_addr);

	hash_add(sdev->tx_buf_table, &buf->hashnode, dma_addr);

	sdev->tx_buf_count++;
	if (sdev->tx_buf_count >= max_tx_buf_param)
		sc23xx_allow_tx(sdev, false);

	spin_unlock_irq(&sdev->tx_buf_lock);

	memcpy(skb->data, &buf->addr, sizeof(buf->addr));
	dma_sync_single_range_for_device(dev, dma_addr, 0, sizeof(buf->addr),
					 DMA_TO_DEVICE);

	spin_lock(&sdev->tx_queue_lock);
	list_add_tail(&buf->listnode, &sdev->tx_queue);
	spin_unlock(&sdev->tx_queue_lock);
	wake_up(&sdev->tx_wait);
}
EXPORT_SYMBOL_GPL(sc23xx_tx_data_dma);

static struct sc23xx_buf *sc23xx_alloc_rx_buf(struct sc23xx_dev *sdev)
{
	struct device *dev = wiphy_dev(sdev->wiphy);
	struct sc23xx_buf *buf;
	dma_addr_t dma_addr;

	buf = kmalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return NULL;

	INIT_HLIST_NODE(&buf->hashnode);

	buf->skb = dev_alloc_skb(SC23XX_MAX_DATA_RXLEN);
	skb_put(buf->skb, SC23XX_MAX_DATA_RXLEN);
	dma_addr = dma_map_single(dev, buf->skb->data,
				  SC23XX_MAX_DATA_RXLEN,
				  DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dev, dma_addr)) {
		dev_kfree_skb(buf->skb);
		kfree(buf);
		return NULL;
	}

	sc23xx_buf_set_addr(buf, dma_addr);

	memcpy(buf->skb->data, &buf->addr, sizeof(buf->addr));
	dma_sync_single_range_for_device(dev, dma_addr, 0,
					 sizeof(buf->addr),
					 DMA_BIDIRECTIONAL);

	spin_lock_irq(&sdev->rx_buf_lock);
	hash_add(sdev->rx_buf_table, &buf->hashnode, dma_addr);
	spin_unlock_irq(&sdev->rx_buf_lock);

	return buf;
}

bool sc23xx_rx_fill_needed(struct sc23xx_dev *sdev)
{
	return atomic_read(&sdev->open_count) > 0 &&
	       atomic_read(&sdev->rx_buf_count) <
	       min(num_rx_buf_param, SC23XX_MAX_NUM_RX_BUF);
}
EXPORT_SYMBOL_GPL(sc23xx_rx_fill_needed);

int sc23xx_rx_fill_addr_list(struct sc23xx_dev *sdev,
			     struct sc23xx_rx_addr_free_list *list,
			     unsigned int *len, unsigned int max_count)
{
	struct sc23xx_buf *buf;
	u16 i = 0;

	while (i < max_count && sc23xx_rx_fill_needed(sdev)) {
		atomic_inc(&sdev->rx_buf_count);
		buf = sc23xx_alloc_rx_buf(sdev);
		if (!buf) {
			atomic_dec(&sdev->rx_buf_count);
			break;
		}
		memcpy(&list->data.addr[i++], &buf->addr, sizeof(buf->addr));
	}

	if (i == 0)
		return -EAGAIN;

	list->hdr.type = SC23XX_HDR_TYPE_ADDR_LIST;
	list->hdr.data_dir = 1;
	list->hdr.need_rsp = 0;
	list->hdr.ctx_id = 0;
	list->data.type = 0;
	list->data.addr_count = i;

	*len = sizeof(*list) + i * sizeof(struct sc23xx_addr);

	return 0;
}
EXPORT_SYMBOL_GPL(sc23xx_rx_fill_addr_list);

int sc23xx_tx_fill_addr_list(struct sc23xx_dev *sdev,
			     struct sc23xx_tx_addr_list *list,
			     unsigned int *len, unsigned int max_count)
{
	struct sc23xx_buf *buf;
	u16 i = 0;

	spin_lock_bh(&sdev->tx_queue_lock);
	while (i < max_count && sc23xx_tx_fill_needed(sdev)) {
		buf = list_first_entry(&sdev->tx_queue, struct sc23xx_buf,
				       listnode);
		list_del_init(&buf->listnode);
		memcpy(&list->addr[i++], &buf->addr, sizeof(buf->addr));
	}
	spin_unlock_bh(&sdev->tx_queue_lock);

	if (i == 0)
		return -EAGAIN;

	list->hdr.type = SC23XX_HDR_TYPE_ADDR_LIST;
	list->hdr.data_dir = 0;
	list->hdr.need_rsp = 1;
	list->hdr.ctx_id = 0;
	list->offset = sizeof(*list);
	list->flags = SC23XX_TX_BUF_IN_USE;
	put_unaligned_le16(i, &list->count);
	put_unaligned_le16(0, &list->reserved);

	*len = sizeof(*list) + i * sizeof(struct sc23xx_addr);

	return 0;
}
EXPORT_SYMBOL_GPL(sc23xx_tx_fill_addr_list);

void sc23xx_free_txrx_buffers(struct sc23xx_dev *sdev)
{
	struct device *dev = wiphy_dev(sdev->wiphy);
	struct sc23xx_buf *buf, *tmp;
	struct hlist_node *htmp;
	int bkt;

	spin_lock_irq(&sdev->tx_buf_lock);
	list_for_each_entry_safe(buf, tmp, &sdev->tx_buf_cache, listnode)
		kfree(buf);
	INIT_LIST_HEAD(&sdev->tx_buf_cache);

	hash_for_each_safe(sdev->tx_buf_table, bkt, htmp, buf, hashnode) {
		hash_del(&buf->hashnode);
		dma_unmap_single(dev, buf->dma_addr, buf->skb->len,
				 DMA_TO_DEVICE);
		dev_kfree_skb(buf->skb);
		kfree(buf);
	}
	hash_init(sdev->tx_buf_table);
	sdev->tx_buf_count = 0;
	spin_unlock_irq(&sdev->tx_buf_lock);

	spin_lock_irq(&sdev->rx_buf_lock);
	hash_for_each_safe(sdev->rx_buf_table, bkt, htmp, buf, hashnode) {
		hash_del(&buf->hashnode);
		dma_unmap_single(dev, buf->dma_addr, buf->skb->len,
				 DMA_TO_DEVICE);
		dev_kfree_skb(buf->skb);
		kfree(buf);
	}
	hash_init(sdev->rx_buf_table);
	atomic_set(&sdev->rx_buf_count, 0);
	spin_unlock_irq(&sdev->rx_buf_lock);
}
