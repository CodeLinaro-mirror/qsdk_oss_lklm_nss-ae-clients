/*
 **************************************************************************
 * Copyright (c) 2017, The Linux Foundation. All rights reserved.
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all copies.
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
 * OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 **************************************************************************
 */

/*
 * nss_connmgr_dtls_ctx_dev.c
 *	NSS DTLS Manager context device
 */

#include <linux/version.h>
#include <linux/types.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <net/ipv6.h>
#include <linux/if_arp.h>
#include <linux/etherdevice.h>
#include <linux/udp.h>
#include <linux/ipv6.h>
#include <crypto/aes.h>
#include <crypto/sha.h>

#include <nss_api_if.h>
#include <nss_dynamic_interface.h>
#include <nss_dtls_cmn.h>
#include <nss_dtlsmgr.h>

#include "nss_dtlsmgr_private.h"

/*
 * nss_dtlsmgr_ctx_dev_event()
 *	Event Callback to receive events from NSS.
 */
void nss_dtlsmgr_ctx_dev_event(void *if_ctx, struct nss_cmn_msg *ncm)
{
	struct nss_dtls_cmn_msg *ndcm = (struct nss_dtls_cmn_msg *)ncm;
	struct nss_dtlsmgr_ctx_data *data = (struct nss_dtlsmgr_ctx_data *)if_ctx;
	struct nss_dtls_cmn_ctx_stats *msg_stats = &ndcm->msg.stats;
	struct nss_dtlsmgr_stats *stats;
	struct nss_dtlsmgr_ctx *ctx;
	bool encap;
	int i;

	if (ncm->type != NSS_DTLS_CMN_MSG_TYPE_SYNC_STATS) {
		nss_dtlsmgr_warn("%p: unsupported message type(%d)", data, ncm->type);
		return;
	}

	if ((data->di_type != NSS_DYNAMIC_INTERFACE_TYPE_DTLS_CMN_INNER) &&
	    (data->di_type != NSS_DYNAMIC_INTERFACE_TYPE_DTLS_CMN_OUTER)) {
		nss_dtlsmgr_warn("%p: unsupported interface type(%d)", data, data->di_type);
		return;
	}

	stats = &data->stats;

	if (data->di_type == NSS_DYNAMIC_INTERFACE_TYPE_DTLS_CMN_INNER) {
		ctx = container_of(data, struct nss_dtlsmgr_ctx, encap);
		encap = true;
	} else {
		ctx = container_of(data, struct nss_dtlsmgr_ctx, decap);
		encap = false;
	}

	dev_hold(ctx->dev);

	stats->tx_packets += msg_stats->pkt.tx_packets;
	stats->tx_bytes += msg_stats->pkt.tx_bytes;

	stats->rx_packets += msg_stats->pkt.rx_packets;
	stats->rx_bytes += msg_stats->pkt.rx_bytes;
	stats->rx_dropped += nss_cmn_rx_dropped_sum(&msg_stats->pkt);
	stats->rx_single_rec += msg_stats->rx_single_rec;
	stats->rx_multi_rec += msg_stats->rx_multi_rec;

	stats->fail_crypto_resource += msg_stats->fail_crypto_resource;
	stats->fail_crypto_enqueue += msg_stats->fail_crypto_enqueue;
	stats->fail_headroom += msg_stats->fail_headroom;
	stats->fail_tailroom += msg_stats->fail_tailroom;
	stats->fail_ver += msg_stats->fail_ver;
	stats->fail_epoch += msg_stats->fail_epoch;
	stats->fail_dtls_record += msg_stats->fail_dtls_record;
	stats->fail_capwap += msg_stats->fail_capwap;
	stats->fail_replay += msg_stats->fail_replay;
	stats->fail_replay_dup += msg_stats->fail_replay_dup;
	stats->fail_replay_win += msg_stats->fail_replay_win;
	stats->fail_queue += msg_stats->fail_queue;
	stats->fail_queue_nexthop += msg_stats->fail_queue_nexthop;
	stats->fail_pbuf_alloc += msg_stats->fail_pbuf_alloc;
	stats->fail_pbuf_linear += msg_stats->fail_pbuf_linear;
	stats->fail_pbuf_stats += msg_stats->fail_pbuf_stats;
	stats->fail_pbuf_align += msg_stats->fail_pbuf_align;
	stats->fail_ctx_active += msg_stats->fail_ctx_active;
	stats->fail_hwctx_active += msg_stats->fail_hwctx_active;
	stats->fail_cipher += msg_stats->fail_cipher;
	stats->fail_auth += msg_stats->fail_auth;
	stats->fail_seq_ovf += msg_stats->fail_seq_ovf;
	stats->fail_blk_len += msg_stats->fail_blk_len;
	stats->fail_hash_len += msg_stats->fail_hash_len;

	stats->fail_hw.len_error += msg_stats->fail_hw.len_error;
	stats->fail_hw.token_error += msg_stats->fail_hw.token_error;
	stats->fail_hw.bypass_error += msg_stats->fail_hw.bypass_error;
	stats->fail_hw.config_error += msg_stats->fail_hw.config_error;
	stats->fail_hw.algo_error += msg_stats->fail_hw.algo_error;
	stats->fail_hw.hash_ovf_error += msg_stats->fail_hw.hash_ovf_error;
	stats->fail_hw.ttl_error += msg_stats->fail_hw.ttl_error;
	stats->fail_hw.csum_error += msg_stats->fail_hw.csum_error;
	stats->fail_hw.timeout_error += msg_stats->fail_hw.timeout_error;

	for (i = 0; i < NSS_DTLS_CMN_CLE_MAX; i++)
		stats->fail_cle[i] += msg_stats->fail_cle[i];

	if (data->notify)
		data->notify(ctx->dev, &data->stats, encap);

	dev_put(ctx->dev);
}

/*
 * nss_dtlsmgr_ctx_dev_rx()
 *	Receive and process packet from NSS.
 */
void nss_dtlsmgr_ctx_dev_rx(struct net_device *dev, struct sk_buff *skb, struct napi_struct *napi)
{
	struct nss_dtlsmgr_ctx *ctx;
	struct nss_dtlsmgr_stats *stats;
	uint32_t meta;

	BUG_ON(!dev);
	BUG_ON(!skb);

	dev_hold(dev);
	ctx = netdev_priv(dev);
	NSS_DTLSMGR_VERIFY_MAGIC(ctx);

	stats = &ctx->decap.stats;

	/*
	 * Get DTLS metadata
	 */
	meta = ntohl(*(uint32_t *)skb->data);
	if (NSS_DTLSMGR_METADATA_CTYPE(meta) != NSS_DTLSMGR_CTYPE_APP) {
		nss_dtlsmgr_warn("%p: dropping non app dtls pkt(%p)", ctx, skb);
		goto free;
	}

	if (NSS_DTLSMGR_METADATA_ERROR(meta) != NSS_DTLSMGR_METADATA_ERROR_OK) {
		nss_dtlsmgr_warn("%p: dropping error dtls pkt(%p)", ctx, skb);
		goto free;
	}

	/*
	 * Remove four bytes at start of buffer containing the DTLS metadata.
	 */
	skb_pull(skb, NSS_DTLSMGR_METADATA_LEN);
	skb_reset_network_header(skb);
	skb->pkt_type = PACKET_HOST;
	skb->skb_iif = dev->ifindex;
	skb->dev = dev;

	switch (ip_hdr(skb)->version) {
	case IPVERSION:
		skb->protocol = htons(ETH_P_IP);
		break;

	case 6:
		skb->protocol = htons(ETH_P_IPV6);
		break;

	default:
		nss_dtlsmgr_warn("%p: received non-IP packet, dropping", ctx);
		goto free;
	}

	netif_receive_skb(skb);
	dev_put(dev);
	return;
free:
	dev_kfree_skb_any(skb);
	stats->rx_dropped++;
	dev_put(dev);
	return;
}

/*
 * nss_dtlsmgr_ctx_dev_tx()
 *	Transmit packet to DTLS node in NSS firmware.
 */
static netdev_tx_t nss_dtlsmgr_ctx_dev_tx(struct sk_buff *skb, struct net_device *dev)
{
	struct nss_dtlsmgr_ctx *ctx = netdev_priv(dev);
	struct nss_dtlsmgr_ctx_data *encap;
	struct nss_dtlsmgr_stats *stats;
	bool expand_skb = false, drop;
	int nhead, ntail;
	uint8_t proto;

	NSS_DTLSMGR_VERIFY_MAGIC(ctx);
	encap = &ctx->encap;
	stats = &encap->stats;
	nhead = dev->needed_headroom;
	ntail = dev->needed_tailroom;

	/*
	 * Check if skb is shared
	 */
	if (unlikely(skb_shared(skb))) {
		nss_dtlsmgr_trace("%p: shared skb is not supported on (%s): %p", ctx, dev->name, skb);
		goto free;
	}

	/*
	 * Check if packet is given starting from network header
	 */
	if (skb->data != skb_network_header(skb)) {
		nss_dtlsmgr_trace("%p: skb data is not starting from IP header(%s)", ctx, dev->name);
		goto free;
	}

	/*
	 * For all these cases
	 * - create a writable copy of buffer
	 * - increase the head room
	 * - increase the tail room
	 */
	if (skb_cloned(skb) || (skb_headroom(skb) < nhead) || (skb_tailroom(skb) < ntail)) {
		expand_skb = true;
	}

	if (expand_skb && pskb_expand_head(skb, nhead, ntail, GFP_ATOMIC)) {
		nss_dtlsmgr_trace("%p: unable to expand buffer for (%s)", ctx, dev->name);
		stats->fail_headroom++;
		goto free;
	}

	proto = ntohs(skb->protocol);

	switch (skb->protocol) {
	case ETH_P_IP:
		drop = (encap->flow.flags & NSS_DTLSMGR_HDR_IPV6) == NSS_DTLSMGR_HDR_IPV6;
		break;

	case htons(ETH_P_IPV6):
		drop = (encap->flow.flags & NSS_DTLSMGR_HDR_IPV6) != NSS_DTLSMGR_HDR_IPV6;
		break;

	default:
		drop = true;
	}

	if (drop) {
		nss_dtlsmgr_warn("%p: dtls encap not supported, proto(%d), flags(%u)", ctx, proto, encap->flow.flags);
		goto free;
	}

	if (nss_dtls_cmn_tx_buf(skb, encap->ifnum, encap->nss_ctx) != NSS_TX_SUCCESS) {
		nss_dtlsmgr_trace("%p: unable to tx buffer for (%u)", ctx, encap->ifnum);
		return NETDEV_TX_BUSY;
	}

	return NETDEV_TX_OK;
free:
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

/*
 * nss_dtlsmgr_ctx_dev_close()
 *	Stop packet transmission on the DTLS network device.
 */
static int nss_dtlsmgr_ctx_dev_close(struct net_device *dev)
{
	netif_stop_queue(dev);
	return 0;
}

/*
 * nss_dtlsmgr_ctx_dev_open()
 *	Start processing packets on the DTLS network device.
 */
static int nss_dtlsmgr_ctx_dev_open(struct net_device *dev)
{
	netif_start_queue(dev);
	return 0;
}

/*
 * nss_dtlsmgr_ctx_dev_free()
 *	Free an existing DTLS context device.
 */
static void nss_dtlsmgr_ctx_dev_free(struct net_device *dev)
{
	nss_dtlsmgr_trace("%p: free dtls context device(%s)", dev, dev->name);
	free_netdev(dev);
}

/*
 * nss_dtlsmgr_ctx_dev_stats64()
 *	Report packet statistics to Linux.
 */
static struct rtnl_link_stats64 *nss_dtlsmgr_ctx_dev_stats64(struct net_device *dev, struct rtnl_link_stats64 *stats)
{
	struct nss_dtlsmgr_ctx *ctx = netdev_priv(dev);
	struct nss_dtlsmgr_stats *encap_stats, *decap_stats;

	encap_stats = &ctx->encap.stats;
	decap_stats = &ctx->decap.stats;

	stats->rx_packets = decap_stats->rx_packets;
	stats->rx_bytes = decap_stats->rx_bytes;
	stats->rx_dropped = decap_stats->rx_dropped;

	stats->tx_bytes = encap_stats->tx_bytes;
	stats->tx_packets = encap_stats->tx_packets;
	stats->tx_dropped = encap_stats->fail_headroom + encap_stats->fail_tailroom;

	return stats;
}

/*
 * nss_dtlsmgr_ctx_dev_change_mtu()
 *	Change MTU size of DTLS context device.
 */
static int32_t nss_dtlsmgr_ctx_dev_change_mtu(struct net_device *dev, int32_t mtu)
{
	dev->mtu = mtu;
	return 0;
}

/*
 * DTLS netdev ops
 */
static const struct net_device_ops nss_dtlsmgr_ctx_dev_ops = {
	.ndo_start_xmit = nss_dtlsmgr_ctx_dev_tx,
	.ndo_open = nss_dtlsmgr_ctx_dev_open,
	.ndo_stop = nss_dtlsmgr_ctx_dev_close,
	.ndo_get_stats64 = nss_dtlsmgr_ctx_dev_stats64,
	.ndo_change_mtu = nss_dtlsmgr_ctx_dev_change_mtu,
};

/*
 * nss_dtlsmgr_ctx_dev_setup()
 *	Setup the DTLS network device.
 */
void nss_dtlsmgr_ctx_dev_setup(struct net_device *dev)
{
	dev->addr_len = ETH_ALEN;
	dev->mtu = ETH_DATA_LEN;
	dev->hard_header_len = 0;
	dev->needed_headroom = 0;
	dev->needed_tailroom = 0;

	dev->type = ARPHRD_TUNNEL;
	dev->ethtool_ops = NULL;
	dev->header_ops = NULL;
	dev->netdev_ops = &nss_dtlsmgr_ctx_dev_ops;
	dev->destructor = nss_dtlsmgr_ctx_dev_free;

	memcpy(dev->dev_addr, "\xaa\xbb\xcc\xdd\xee\xff", dev->addr_len);
	memset(dev->broadcast, 0xff, dev->addr_len);
	memcpy(dev->perm_addr, dev->dev_addr, dev->addr_len);
}
