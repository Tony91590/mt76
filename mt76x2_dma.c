/*
 * Copyright (C) 2014 Felix Fietkau <nbd@openwrt.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "mt76x2.h"
#include "mt76x2_dma.h"

int
mt76x2_tx_queue_mcu(struct mt76x2_dev *dev, enum mt76_txq_id qid,
		    struct sk_buff *skb, int cmd, int seq)
{
	struct mt76_queue *q = &dev->mt76.q_tx[qid];
	dma_addr_t addr;
	u32 tx_info;
	int idx;

	tx_info = MT_MCU_MSG_TYPE_CMD |
		  MT76_SET(MT_MCU_MSG_CMD_TYPE, cmd) |
		  MT76_SET(MT_MCU_MSG_CMD_SEQ, seq) |
		  MT76_SET(MT_MCU_MSG_PORT, CPU_TX_PORT) |
		  MT76_SET(MT_MCU_MSG_LEN, skb->len);

	addr = dma_map_single(dev->mt76.dev, skb->data, skb->len, DMA_TO_DEVICE);
	if (dma_mapping_error(dev->mt76.dev, addr))
		return -ENOMEM;

	spin_lock_bh(&q->lock);
	idx = mt76_queue_add_buf(dev, q, addr, skb->len, 0, 0, tx_info);
	q->entry[idx].skb = skb;
	mt76_queue_kick(dev, q);
	spin_unlock_bh(&q->lock);

	return 0;
}

int
mt76x2_tx_queue_skb(struct mt76_dev *cdev, struct mt76_queue *q,
		    struct sk_buff *skb, struct mt76_txwi_cache *t,
			struct mt76_wcid *wcid, struct ieee80211_sta *sta)
{
	struct mt76x2_dev *dev = container_of(cdev, struct mt76x2_dev, mt76);
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	dma_addr_t addr;
	u32 tx_info = 0;
	int idx, ret, len;
	int qsel = MT_QSEL_EDCA;

	ret = mt76_insert_hdr_pad(skb);
	if (ret)
		return ret;

	if (info->flags & IEEE80211_TX_CTL_RATE_CTRL_PROBE)
		qsel = 0;

	len = skb->len + sizeof(struct mt76x2_txwi);
	len += mt76x2_mac_skb_tx_overhead(dev, skb);
	tx_info = MT76_SET(MT_TXD_INFO_LEN, len) |
		  MT76_SET(MT_TXD_INFO_QSEL, qsel) |
		  MT_TXD_INFO_80211;

	if (!wcid || wcid->hw_key_idx == 0xff)
		tx_info |= MT_TXD_INFO_WIV;

	addr = dma_map_single(dev->mt76.dev, skb->data, skb->len, DMA_TO_DEVICE);
	if (dma_mapping_error(dev->mt76.dev, addr))
		return -ENOMEM;

	idx = mt76_queue_add_buf(dev, q, t->dma_addr, sizeof(struct mt76x2_txwi),
				 addr, skb->len, tx_info);
	q->entry[idx].skb = skb;
	q->entry[idx].txwi = t;

	return idx;
}

static void
mt76x2_tx_cleanup_entry(struct mt76_dev *mdev, struct mt76_queue *q,
			struct mt76_queue_entry *e)
{
	struct mt76x2_dev *dev = container_of(mdev, struct mt76x2_dev, mt76);

	if (e->txwi) {
		mt76x2_mac_queue_txdone(dev, e->skb, &e->txwi->txwi);
	} else {
		dev_kfree_skb_any(e->skb);
	}
}

static void
mt76x2_tx_cleanup(struct mt76x2_dev *dev, struct mt76_queue *q, bool flush)
{
	if (!q->desc)
		return;

	mt76_queue_tx_cleanup(dev, q, flush, mt76x2_tx_cleanup_entry);
}

static int
mt76x2_init_tx_queue(struct mt76x2_dev *dev, struct mt76_queue *q,
		   int idx, int n_desc)
{
	int ret;

	q->regs = dev->mt76.regs + MT_TX_RING_BASE + idx * MT_RING_SIZE;
	q->ndesc = n_desc;

	ret = mt76_queue_alloc(dev, q);
	if (ret)
		return ret;

	mt76x2_irq_enable(dev, MT_INT_TX_DONE(idx));

	return 0;
}

void mt76x2_queue_rx_skb(struct mt76_dev *mdev, enum mt76_rxq_id q,
			 struct sk_buff *skb)
{
	struct mt76x2_dev *dev = container_of(mdev, struct mt76x2_dev, mt76);
	void *rxwi = skb->data;

	if (q == MT_RXQ_MCU) {
		skb_queue_tail(&dev->mcu.res_q, skb);
		wake_up(&dev->mcu.wait);
		return;
	}

	skb_pull(skb, sizeof(struct mt76x2_rxwi));
	if (mt76x2_mac_process_rx(dev, skb, rxwi)) {
	    dev_kfree_skb(skb);
	    return;
	}

	mt76x2_rx(dev, skb);
}

static int
mt76x2_init_rx_queue(struct mt76x2_dev *dev, struct mt76_queue *q,
		   int idx, int n_desc, int bufsize)
{
	int ret;

	q->regs = dev->mt76.regs + MT_RX_RING_BASE + idx * MT_RING_SIZE;
	q->ndesc = n_desc;
	q->buf_size = bufsize;

	ret = mt76_queue_alloc(dev, q);
	if (ret)
		return ret;

	mt76x2_irq_enable(dev, MT_INT_RX_DONE(idx));

	return 0;
}

static void
mt76x2_tx_tasklet(unsigned long data)
{
	struct mt76x2_dev *dev = (struct mt76x2_dev *) data;
	int i;

	mt76x2_mac_process_tx_status_fifo(dev);

	for (i = ARRAY_SIZE(dev->mt76.q_tx) - 1; i >= 0; i--)
		mt76x2_tx_cleanup(dev, &dev->mt76.q_tx[i], false);

	mt76x2_mac_poll_tx_status(dev, false);
	mt76x2_irq_enable(dev, MT_INT_TX_DONE_ALL);
}

int mt76x2_dma_init(struct mt76x2_dev *dev)
{
	static const u8 wmm_queue_map[] = {
		[IEEE80211_AC_BE] = 0,
		[IEEE80211_AC_BK] = 1,
		[IEEE80211_AC_VI] = 2,
		[IEEE80211_AC_VO] = 3,
	};
	int ret;
	int i;
	struct mt76_txwi_cache __maybe_unused *t;
	struct mt76_queue *q;

	BUILD_BUG_ON(sizeof(t->txwi) < sizeof(struct mt76x2_txwi));
	BUILD_BUG_ON(sizeof(struct mt76x2_rxwi) > MT_RX_HEADROOM);

	mt76_dma_attach(&dev->mt76);

	init_waitqueue_head(&dev->mcu.wait);
	skb_queue_head_init(&dev->mcu.res_q);

	tasklet_init(&dev->tx_tasklet, mt76x2_tx_tasklet, (unsigned long) dev);

	mt76_wr(dev, MT_WPDMA_RST_IDX, ~0);

	for (i = 0; i < ARRAY_SIZE(wmm_queue_map); i++) {
		ret = mt76x2_init_tx_queue(dev, &dev->mt76.q_tx[i], wmm_queue_map[i],
					 MT_TX_RING_SIZE);
		if (ret)
			return ret;
	}

	ret = mt76x2_init_tx_queue(dev, &dev->mt76.q_tx[MT_TXQ_PSD],
				 MT_TX_HW_QUEUE_MGMT, MT_TX_RING_SIZE);
	if (ret)
		return ret;

	ret = mt76x2_init_tx_queue(dev, &dev->mt76.q_tx[MT_TXQ_MCU],
				 MT_TX_HW_QUEUE_MCU, MT_MCU_RING_SIZE);
	if (ret)
		return ret;


	ret = mt76x2_init_rx_queue(dev, &dev->mt76.q_rx[MT_RXQ_MCU], 1,
				   MT_MCU_RING_SIZE, MT_RX_BUF_SIZE);
	if (ret)
		return ret;

	q = &dev->mt76.q_rx[MT_RXQ_MAIN];
	q->buf_offset = MT_RX_HEADROOM - sizeof(struct mt76x2_rxwi);
	ret = mt76x2_init_rx_queue(dev, q, 0, MT_RX_RING_SIZE, MT_RX_BUF_SIZE);
	if (ret)
		return ret;

	return mt76_init_queues(dev);
}

void mt76x2_dma_cleanup(struct mt76x2_dev *dev)
{
	tasklet_kill(&dev->tx_tasklet);
	mt76_dma_cleanup(&dev->mt76);
}
