// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Otto Pflüger
 */

#include "cmd.h"
#include "txrx.h"

#define SC23XX_BA_TIMEOUT	(HZ / 10)

#define SC23XX_SEQ_INVALID	0xffff
#define SC23XX_SEQ_NUM_MASK	0xfff
#define SC23XX_SEQ_HALF_RANGE	((SC23XX_SEQ_NUM_MASK + 1) >> 1)

static void reorder_buf_advance(struct sc23xx_reorder_data *r,
				unsigned int count, bool force)
{
	struct list_head *head;
	struct sk_buff *skb;
	struct sc23xx_rx_data_hdr *hdr;

	while (count--) {
		bool last_msdu = false;

		r->seq_start = (r->seq_start + 1) & SC23XX_SEQ_NUM_MASK;
		r->buf_pos = (r->buf_pos + 1) % r->win_size;
		head = &r->buf[r->buf_pos];

		while (!list_empty(head)) {
			skb = list_first_entry(head, struct sk_buff, list);
			skb_list_del_init(skb);
			if (last_msdu) {
				wiphy_dbg(r->sdev->wiphy,
					  "dropping duplicate frame 0x%03x\n",
					  r->seq_start);
				dev_kfree_skb_any(skb);
			} else {
				hdr = (void *)skb->data;
				last_msdu = le16_get_bits(hdr->flags_0,
							  SC23XX_RX_LAST_MSDU);
				sc23xx_rx_now(r->sdev, skb);
			}
		}

		if (!last_msdu && !force)
			break;
	}
}

static void reorder_insert(struct sc23xx_reorder_data *r, struct sk_buff *skb,
			   u16 seq_num, bool last_msdu)
{
	u16 offset;

	if (r->seq_start == SC23XX_SEQ_INVALID) {
		r->seq_start = (seq_num + !!last_msdu) & SC23XX_SEQ_NUM_MASK;
		wiphy_dbg(r->sdev->wiphy, "receiving first frame\n");
		sc23xx_rx_now(r->sdev, skb);
		return;
	}

	offset = (seq_num - r->seq_start) & SC23XX_SEQ_NUM_MASK;
	if (offset >= SC23XX_SEQ_HALF_RANGE) {
		wiphy_dbg(r->sdev->wiphy, "dropping old frame\n");
		dev_kfree_skb_any(skb);
		return;
	}

	if (offset > r->win_size) {
		reorder_buf_advance(r, offset - r->win_size, true);
		offset = r->win_size;
	}

	if (seq_num == r->seq_start) {
		sc23xx_rx_now(r->sdev, skb);
		if (last_msdu)
			reorder_buf_advance(r, r->win_size, false);
		mod_timer(&r->timer, jiffies + SC23XX_BA_TIMEOUT);
	} else {
		u16 idx = (r->buf_pos + offset) % r->win_size;

		INIT_LIST_HEAD(&skb->list);
		list_add_tail(&skb->list, &r->buf[idx]);
	}
}

static void reorder_timeout(struct timer_list *t)
{
	struct sc23xx_reorder_data *r = timer_container_of(r, t, timer);
	unsigned int count;

	spin_lock_irq(&r->sdev->sta_lock);

	if (!r->buf)
		goto out;

	/* Drop all frames in the buffer, keeping the free space at the end. */
	count = r->win_size;
	while (count && list_empty(&r->buf[(r->buf_pos + count) % r->win_size]))
		count--;

	/* If the whole buffer is empty, nothing needs to be done. */
	if (!count)
		goto out;

	wiphy_warn(r->sdev->wiphy, "timeout, some RX frames lost\n");
	reorder_buf_advance(r, count, true);

out:
	spin_unlock_irq(&r->sdev->sta_lock);
}

void sc23xx_rx_reorder(struct sc23xx_dev *sdev, struct sk_buff *skb)
{
	struct sc23xx_rx_data_hdr *hdr = (void *)skb->data;
	u16 flags_0, flags_1, seq_info, sta_lut_idx;
	struct sc23xx_reorder_data *r;
	struct sc23xx_sta *sta;

	if (skb->len < sizeof(*hdr)) {
		wiphy_err(sdev->wiphy, "data packet too short\n");
		goto drop;
	}

	flags_0 = le16_to_cpu(hdr->flags_0);
	flags_1 = le16_to_cpu(hdr->flags_1);
	seq_info = le16_to_cpu(hdr->seq_info);

	wiphy_dbg(sdev->wiphy,
		  "msdu info: tid %d seqno 0x%03x flags 0x%04x%04x\n",
		  u16_get_bits(seq_info, SC23XX_RX_TID),
		  u16_get_bits(seq_info, SC23XX_RX_SEQ_NUM),
		  flags_1, flags_0);

	if ((flags_1 & SC23XX_RX_BROADCAST) ||
	    !(flags_1 & SC23XX_RX_QOS_FLAG)) {
		wiphy_dbg(sdev->wiphy,
			  "rx: broadcast, multicast or non-QoS frame\n");
		goto receive;
	}

	sta_lut_idx = u16_get_bits(flags_0, SC23XX_RX_STA_LUT_IDX);
	if (sta_lut_idx < SC23XX_STA_IDX_MIN ||
	    sta_lut_idx > SC23XX_STA_IDX_MAX) {
		wiphy_err(sdev->wiphy, "rx: STA index %d out of range\n",
			  sta_lut_idx);
		goto drop;
	}

	spin_lock(&sdev->sta_lock);

	sta = &sdev->sta[sta_lut_idx - SC23XX_STA_IDX_MIN];
	if (!sta->valid) {
		wiphy_dbg(sdev->wiphy, "rx: STA %d not found, dropping\n",
			  sta_lut_idx);
		spin_unlock(&sdev->sta_lock);
		goto drop;
	}

	r = &sta->r[u16_get_bits(seq_info, SC23XX_RX_TID)];
	if (!r->buf) {
		wiphy_dbg(sdev->wiphy, "rx: no BA session, receiving\n");
		spin_unlock(&sdev->sta_lock);
		goto receive;
	}

	reorder_insert(r, skb, u16_get_bits(seq_info, SC23XX_RX_SEQ_NUM),
		       u16_get_bits(flags_0, SC23XX_RX_LAST_MSDU));

	spin_unlock(&sdev->sta_lock);

	return;

receive:
	sc23xx_rx_now(sdev, skb);
	return;

drop:
	dev_kfree_skb_any(skb);
}

void sc23xx_rx_addba_req(struct sc23xx_dev *sdev, u8 sta_lut_idx, u8 tid,
			 u16 win_start, u16 win_size)
{
	struct sc23xx_reorder_data *r;
	struct list_head *reorder_buf;
	struct sc23xx_sta *sta;
	int i, ret;

	wiphy_dbg(sdev->wiphy, "rx addba: sta %d tid %d: 0x%03x size 0x%04x\n",
		  sta_lut_idx, tid, win_start, win_size);

	reorder_buf = kcalloc(win_size, sizeof(struct list_head), GFP_KERNEL);
	if (!reorder_buf)
		return;

	for (i = 0; i < win_size; i++)
		INIT_LIST_HEAD(&reorder_buf[i]);

	spin_lock_irq(&sdev->sta_lock);

	sta = &sdev->sta[sta_lut_idx - SC23XX_STA_IDX_MIN];
	if (sta->valid) {
		r = &sta->r[tid];
		kfree(r->buf);

		/* The firmware provides a starting sequence number in
		 * win_start, but we cannot use it because this function runs
		 * from a delayed event handler. The starting position of the
		 * buffer has to be initialized when the first frame arrives
		 * after this handler is finished.
		 */
		r->seq_start = SC23XX_SEQ_INVALID;
		r->win_size = win_size;
		r->sdev = sdev;

		if (!r->buf)
			timer_setup(&r->timer, reorder_timeout, 0);
		r->buf = reorder_buf;
	} else {
		wiphy_err(sdev->wiphy, "addba for unknown sta %d\n",
			  sta_lut_idx);
		spin_unlock_irq(&sdev->sta_lock);
		kfree(reorder_buf);
		return;
	}

	spin_unlock_irq(&sdev->sta_lock);

	ret = sc23xx_cmd_addba_rsp(sdev, sta, tid);
	if (ret)
		wiphy_err(sdev->wiphy, "failed to send addba rsp: %d\n", ret);
}

void sc23xx_rx_delba_req(struct sc23xx_dev *sdev, u8 sta_lut_idx, u8 tid)
{
	struct sc23xx_sta *sta;

	wiphy_dbg(sdev->wiphy, "rx delba: sta %d tid %d\n", sta_lut_idx, tid);

	spin_lock_irq(&sdev->sta_lock);
	sta = &sdev->sta[sta_lut_idx - SC23XX_STA_IDX_MIN];
	kfree(sta->r[tid].buf);
	sta->r[tid].buf = NULL;
	timer_shutdown(&sta->r[tid].timer);
	spin_unlock_irq(&sdev->sta_lock);
}
