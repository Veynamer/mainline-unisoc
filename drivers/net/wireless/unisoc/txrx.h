// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Otto Pflüger
 */

#ifndef __SC23XX_TXRX_H
#define __SC23XX_TXRX_H

#include "sc23xx.h"

void sc23xx_rx_reorder(struct sc23xx_dev *sdev, struct sk_buff *skb);
void sc23xx_rx_addba_req(struct sc23xx_dev *sdev, u8 sta_lut_idx, u8 tid,
			 u16 win_start, u16 win_size);
void sc23xx_rx_delba_req(struct sc23xx_dev *sdev, u8 sta_lut_idx, u8 tid);
void sc23xx_rx_now(struct sc23xx_dev *sdev, struct sk_buff *skb);
void sc23xx_tx_prepare(struct sc23xx_vif *vif, struct sk_buff *skb);
void sc23xx_free_txrx_buffers(struct sc23xx_dev *sdev);

#endif
