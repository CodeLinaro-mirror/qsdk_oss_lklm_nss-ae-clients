/*
 **************************************************************************
 * Copyright (c) 2017-2019, The Linux Foundation. All rights reserved.
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

/* nss_ipsecmgr.c
 *	NSS to HLOS IPSec Manager
 */
#include <linux/version.h>
#include <linux/types.h>
#include <linux/ip.h>
#include <linux/of.h>
#include <linux/ipv6.h>
#include <linux/skbuff.h>
#include <linux/module.h>
#include <linux/bitops.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/etherdevice.h>
#include <linux/vmalloc.h>
#include <linux/debugfs.h>
#include <linux/atomic.h>
#include <net/protocol.h>
#include <net/route.h>
#include <net/ip6_route.h>
#include <net/esp.h>
#include <net/xfrm.h>
#include <net/icmp.h>

#include <crypto/aead.h>
#include <crypto/skcipher.h>
#include <crypto/internal/hash.h>

#include <nss_api_if.h>
#include <nss_ipsec_cmn.h>
#include <nss_ipsecmgr.h>
#include <nss_cryptoapi.h>

#include "nss_ipsecmgr_ref.h"
#include "nss_ipsecmgr_flow.h"
#include "nss_ipsecmgr_sa.h"
#include "nss_ipsecmgr_ctx.h"
#include "nss_ipsecmgr_tunnel.h"
#include "nss_ipsecmgr_priv.h"

/*
 * nss_ipsecmgr_tunnel_open()
 *	open the tunnel for usage
 */
static int nss_ipsecmgr_tunnel_open(struct net_device *dev)
{
	struct nss_ipsecmgr_tunnel *tun __attribute__((unused)) = netdev_priv(dev);

	netif_start_queue(dev);
	return 0;
}

/*
 * nss_ipsecmgr_tunnel_stop()
 *	stop the IPsec tunnel
 */
static int nss_ipsecmgr_tunnel_stop(struct net_device *dev)
{
	struct nss_ipsecmgr_tunnel *tun __attribute__((unused)) = netdev_priv(dev);

	netif_stop_queue(dev);
	return 0;
}

/*
 * nss_ipsecmgr_tunnel_tx()
 *	tunnel transmit function
 */
static netdev_tx_t nss_ipsecmgr_tunnel_tx(struct sk_buff *skb, struct net_device *dev)
{
	struct nss_ipsecmgr_tunnel *tun = netdev_priv(dev);
	struct nss_ctx_instance *nss_ctx;
	struct nss_ipsecmgr_ctx *ctx;
	bool expand_skb = false;
	struct iphdr *iph;
	int nhead, ntail;
	uint32_t ifnum;

	nhead = dev->needed_headroom;
	ntail = dev->needed_tailroom;

	iph = (struct iphdr *)skb->data;

	/*
	 * IPsec does not encapsulate non-IP frames
	 */
	if ((iph->version != IPVERSION) && (iph->version != 6))
		goto free;

	read_lock_bh(&ipsecmgr_drv->lock);

	ctx = nss_ipsecmgr_ctx_find(tun, NSS_IPSEC_CMN_CTX_TYPE_INNER);
	if (!ctx) {
		read_unlock_bh(&ipsecmgr_drv->lock);
		nss_ipsecmgr_warn("%p: failed to find inner context for TX\n", tun);
		goto free;
	}

	nss_ctx = ctx->nss_ctx;
	ifnum = ctx->ifnum;
	read_unlock_bh(&ipsecmgr_drv->lock);

	/*
	 * Check if skb is shared
	 */
	if (unlikely(skb_shared(skb))) {
		skb = skb_unshare(skb, in_atomic() ? GFP_ATOMIC : GFP_KERNEL);
		if (!skb)
			return NETDEV_TX_OK;
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

	if (expand_skb && pskb_expand_head(skb, nhead, ntail, GFP_KERNEL)) {
		nss_ipsecmgr_trace("%s: unable to expand buffer\n", dev->name);
		goto free;
	}

	/*
	 * Send the packet down;
	 * TODO: Use stop queue and start queue to restart in case of
	 * queue full condition
	 */
	if (nss_ipsec_cmn_tx_buf(nss_ctx, skb, ifnum) != 0) {
		goto free;
	}

	return NETDEV_TX_OK;

free:
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

/*
 * nss_ipsecmgr_tunnel_stats64()
 *	Get device statistics
 */
static struct rtnl_link_stats64 *nss_ipsecmgr_tunnel_stats64(struct net_device *dev, struct rtnl_link_stats64 *stats)
{
	struct nss_ipsecmgr_tunnel *tun = netdev_priv(dev);
	struct list_head *head = &tun->ctx_db;
	struct nss_ipsecmgr_ctx *ctx;

	memset(stats, 0, sizeof(*stats));

	read_lock_bh(&ipsecmgr_drv->lock);
	list_for_each_entry(ctx, head, list) {
		nss_ipsecmgr_ctx_stats_read(ctx, stats);
	}

	read_unlock_bh(&ipsecmgr_drv->lock);
	return stats;
}

/*
 * nss_ipsecmgr_tunnel_mtu_update()
 *	Update tunnel max MTU
 */
static void nss_ipsecmgr_tunnel_mtu_update(struct list_head *head)
{
	struct nss_ipsecmgr_tunnel *tun;
	uint16_t max_mtu = 0;
	bool update_mtu = false;

	write_lock(&ipsecmgr_drv->lock);
	list_for_each_entry(tun, head, list) {
		if (tun->dev->mtu > max_mtu)
			max_mtu = tun->dev->mtu;
	}

	if (ipsecmgr_drv->max_mtu != max_mtu) {
		ipsecmgr_drv->max_mtu = max_mtu;
		update_mtu = true;
	}

	write_unlock(&ipsecmgr_drv->lock);

#ifdef NSS_IPSECMGR_PPE_SUPPORT
	/*
	 * Set PPE inline port's MTU.
	 * TODO: this needs to move to Virtual Port
	 */
	if (ipsecmgr_drv->ipsec_inline && update_mtu) {
		struct nss_ipsecmgr_ctx *ctx;
		uint32_t redir_ifnum;

		read_lock_bh(&ipsecmgr_drv->lock);
		ctx = nss_ipsecmgr_ctx_find(netdev_priv(ipsecmgr_drv->dev), NSS_IPSEC_CMN_CTX_TYPE_REDIR);
		if (!ctx) {
			read_unlock_bh(&ipsecmgr_drv->lock);
			nss_ipsecmgr_warn("Unable to find REDIR interface for %s", ipsecmgr_drv->dev->name);
			return;
		}
		redir_ifnum = ctx->ifnum;
		read_unlock_bh(&ipsecmgr_drv->lock);

		nss_ipsecmgr_info("Updating mtu for %s as %d", ipsecmgr_drv->dev->name, max_mtu);
		nss_ipsec_cmn_ppe_mtu_update(ipsecmgr_drv->nss_ctx, redir_ifnum, max_mtu, max_mtu);
	}
#endif
}

/*
 * nss_ipsecmgr_tunnel_mtu()
 *	Change device MTU
 */
static int nss_ipsecmgr_tunnel_mtu(struct net_device *dev, int mtu)
{
	dev->mtu = mtu;
	nss_ipsecmgr_tunnel_mtu_update(&ipsecmgr_drv->tun_db);
	return 0;
}

/* NSS IPsec tunnel operation */
static const struct net_device_ops ipsecmgr_dev_ops = {
	.ndo_open = nss_ipsecmgr_tunnel_open,
	.ndo_stop = nss_ipsecmgr_tunnel_stop,
	.ndo_start_xmit = nss_ipsecmgr_tunnel_tx,
	.ndo_get_stats64 = nss_ipsecmgr_tunnel_stats64,
	.ndo_change_mtu = nss_ipsecmgr_tunnel_mtu,
};

/*
 * nss_ipsecmgr_tunnel_free()
 *	free an existing IPsec tunnel interface
 */
static void nss_ipsecmgr_tunnel_free(struct net_device *dev)
{
	nss_ipsecmgr_info("IPsec tunnel device(%s) freed\n", dev->name);
	free_netdev(dev);
}

/*
 * nss_ipsecmr_dev_setup()
 *	setup the IPsec tunnel
 */
static void nss_ipsecmgr_tunnel_setup(struct net_device *dev)
{
	dev->addr_len = ETH_ALEN;
	dev->mtu = NSS_IPSECMGR_TUN_MTU(ETH_DATA_LEN);

	dev->hard_header_len = NSS_IPSECMGR_TUN_MAX_HDR_LEN;
	dev->needed_headroom = NSS_IPSECMGR_TUN_HEADROOM;
	dev->needed_tailroom = NSS_IPSECMGR_TUN_TAILROOM;

	dev->type = NSS_IPSEC_CMN_ARPHRD_IPSEC;

	dev->ethtool_ops = NULL;
	dev->header_ops = NULL;
	dev->netdev_ops = &ipsecmgr_dev_ops;

	dev->destructor = nss_ipsecmgr_tunnel_free;

	/*
	 * Get the MAC address from the ethernet device
	 */
	random_ether_addr(dev->dev_addr);

	memset(dev->broadcast, 0xff, dev->addr_len);
	memcpy(dev->perm_addr, dev->dev_addr, dev->addr_len);
}

/*
 * nss_ipsecmgr_tunnel_del()
 *	delete an existing IPsec tunnel
 */
void nss_ipsecmgr_tunnel_del(struct net_device *dev)
{
	struct nss_ipsecmgr_tunnel *tun = netdev_priv(dev);

	debugfs_remove_recursive(tun->dentry);

	/*
	 * Flush all associated SA(s) and flow(s) with the tunnel
	 */
	write_lock_bh(&ipsecmgr_drv->lock);
	nss_ipsecmgr_ref_free(&tun->ref);

	list_del(&tun->list);
	write_unlock_bh(&ipsecmgr_drv->lock);

	nss_ipsecmgr_tunnel_mtu_update(&ipsecmgr_drv->tun_db);

	/*
	 * The unregister should start here but the expectation is that the free would
	 * happen when the reference count goes down to '0'
	 */
	rtnl_is_locked() ? unregister_netdevice(dev) : unregister_netdev(dev);
}
EXPORT_SYMBOL(nss_ipsecmgr_tunnel_del);

/*
 * nss_ipsecmgr_tunnel_add()
 *	add a IPsec pseudo tunnel device
 */
struct net_device *nss_ipsecmgr_tunnel_add(struct nss_ipsecmgr_callback *cb)
{
	struct nss_ipsecmgr_ctx *inner, *outer;
	struct nss_ipsecmgr_tunnel *tun;
	struct net_device *skb_dev;
	struct net_device *dev;
	int status;

	dev = alloc_netdev(sizeof(*tun), NSS_IPSECMGR_TUN_NAME, NET_NAME_ENUM, nss_ipsecmgr_tunnel_setup);
	if (!dev) {
		nss_ipsecmgr_error("unable to allocate a tunnel device\n");
		return NULL;
	}

	skb_dev = cb->skb_dev;
	tun = netdev_priv(dev);

	tun->dev = dev;
	nss_ipsecmgr_ref_init(&tun->ref, NULL);

	INIT_LIST_HEAD(&tun->list);
	nss_ipsecmgr_db_init(&tun->ctx_db);

	memcpy(&tun->cb, cb, sizeof(tun->cb));

	/*
	 * Use HLOS netdev if it is loaded in the callback context;
	 * else use the NSS netdev
	 */
	if (!skb_dev)
		tun->cb.skb_dev = dev;

	/*
	 * Inner context allocation
	 */
	inner = nss_ipsecmgr_ctx_alloc_inner(tun);
	if (!inner) {
		nss_ipsecmgr_warn("%p: failed to allocate context inner\n", tun);
		goto free_dev;
	}

	/*
	 * Outer context allocation
	 */
	outer = nss_ipsecmgr_ctx_alloc_outer(tun);
	if (!outer) {
		nss_ipsecmgr_warn("%p: failed to allocate context outer\n", tun);
		goto free_inner;
	}

	nss_ipsecmgr_ctx_attach(&tun->ctx_db, inner);
	nss_ipsecmgr_ctx_attach(&tun->ctx_db, outer);

	/*
	 * We need to setup the exception interface number for inner & outer;
	 * The exception interface is used by the NSS to send the packet back
	 * to host when there is a rule miss after the processing (inner or outer).
	 * In case of inner any exception after its processing must come with the
	 * outer interface number. Similarly, for outer we have to do the opposite
	 */
	nss_ipsecmgr_ctx_set_except(inner, outer->ifnum);
	nss_ipsecmgr_ctx_set_except(outer, inner->ifnum);

	if (!nss_ipsecmgr_ctx_config(inner)) {
		nss_ipsecmgr_warn("%p: failed to configure inner context\n", tun);
		goto free_outer;
	}

	if (!nss_ipsecmgr_ctx_config(outer)) {
		nss_ipsecmgr_warn("%p: failed to configure inner context\n", tun);
		goto free_outer;
	}

	status = rtnl_is_locked() ? register_netdevice(dev) : register_netdev(dev);
	if (status < 0) {
		nss_ipsecmgr_warn("%p: register net dev failed :%s\n", tun, dev->name);
		goto free_outer;
	}

	write_lock(&ipsecmgr_drv->lock);
	list_add(&tun->list, &ipsecmgr_drv->tun_db);
	write_unlock(&ipsecmgr_drv->lock);

	nss_ipsecmgr_tunnel_mtu(dev, skb_dev ? skb_dev->mtu : dev->mtu);

	/*
	 * Create debugfs entry for tunnel and its child context(s)
	 */
	tun->dentry = debugfs_create_dir(dev->name, ipsecmgr_drv->dentry);
	if (tun->dentry) {
		debugfs_create_file("inner", S_IRUGO, tun->dentry, inner, &ipsecmgr_ctx_file_ops);
		debugfs_create_file("outer", S_IRUGO, tun->dentry, outer, &ipsecmgr_ctx_file_ops);
	}

	return dev;

free_outer:
	nss_ipsecmgr_ctx_free(outer);
free_inner:
	nss_ipsecmgr_ctx_free(inner);
free_dev:
	free_netdev(dev);
	return NULL;
}
EXPORT_SYMBOL(nss_ipsecmgr_tunnel_add);
