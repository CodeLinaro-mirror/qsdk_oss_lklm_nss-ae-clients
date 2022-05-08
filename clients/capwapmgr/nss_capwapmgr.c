/*
 **************************************************************************
 * Copyright (c) 2014-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all copies.
 *
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
 * nss_capwapmgr.c
 *	NSS to HLOS CAPWAP manager
 */
#include <linux/types.h>
#include <linux/ip.h>
#include <linux/of.h>
#include <linux/tcp.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <net/ipv6.h>
#include <linux/version.h>
#include <net/ip_tunnels.h>
#include <linux/if_arp.h>
#include <linux/etherdevice.h>
#include <linux/if_pppox.h>
#include <nss_api_if.h>
#include <linux/in.h>
#include <fal/fal_qos.h>
#include <ppe_drv.h>
#include <ppe_drv_v4.h>
#include <ppe_drv_v6.h>
#include <nss_dp_api_if.h>
#include <ppe_vp_public.h>
#include <ppe_drv_iface.h>
#include <nss_capwapmgr_public.h>
#include <nss_capwapmgr_user.h>
#include "nss_capwapmgr.h"

/*
 * This file is responsible for interacting with qca-nss-drv's
 * CAPWAP API to manage CAPWAP tunnels.
 *
 * This driver also exposes few APIs which can be used by
 * another module to perform operations on CAPWAP tunnels. However, we create
 * one netdevice for all the CAPWAP tunnels which is done at the module's
 * init time if NSS_CAPWAPMGR_ONE_NETDEV is set in the Makefile.
 *
 * If your requirement is to create one netdevice per-CAPWAP tunnel, then
 * netdevice needs to be created before CAPWAP tunnel create. Netdevice are
 * created using nss_capwapmgr_netdev_create() API.
 *
 */

/*
 * Global Structure to hold tunnel statistics and driver queue selection.
 */
static struct nss_capwapmgr_global global;

#if defined(NSS_CAPWAPMGR_ONE_NETDEV)
/*
 * If you want only one netdev for all the tunnels. If you don't want
 * to create one netdev for all the tunnels, then netdev must be
 * created using nss_capwapmgr_netdev_create() before every tunnel create
 * operation.
 */
static struct net_device *nss_capwapmgr_ndev = NULL;
#endif

/*
 * nss_capwapmgr_open()
 *	Netdev's open call.
 */
static int nss_capwapmgr_open(struct net_device *dev)
{
	netif_start_queue(dev);
	return 0;
}

/*
 * nss_capwapmgr_close()
 *	Netdev's close call.
 */
static int nss_capwapmgr_close(struct net_device *dev)
{
	netif_stop_queue(dev);
	return 0;
}

/*
 * nss_capwapmgr_decongestion_callback()
 *	Wakeup netif queue if we were stopped by start_xmit
 */
static void nss_capwapmgr_decongestion_callback(void *arg)
{
	struct net_device *dev = arg;

	if (netif_queue_stopped(dev)) {
		netif_wake_queue(dev);
	}
}

/*
 * nss_capwapmgr_start_xmit()
 *	Transmit's skb to NSS FW over CAPWAP if_num_inner.
 *
 * Please make sure to leave headroom of NSS_CAPWAP_HEADROOM with every
 * packet so that NSS can encap eth,vlan,ip,udp,capwap headers.
 * Also, skb->len must include size of metaheader. Essentially skb->len is
 * size of CAPWAP Payload (including wireless info sections) and metaheader.
 */
static netdev_tx_t nss_capwapmgr_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct net_device_stats *stats = &dev->stats;
	struct nss_capwapmgr_priv *priv;
	struct nss_capwap_metaheader *pre;
	uint32_t if_num_inner;
	nss_tx_status_t status;

	priv = netdev_priv(dev);
	pre = (struct nss_capwap_metaheader *)skb->data;

	if (unlikely(pre->tunnel_id > NSS_CAPWAPMGR_MAX_TUNNELS)) {
		nss_capwapmgr_warn("%px: (CAPWAP packet) tunnel-id invalid: %d\n", dev, pre->tunnel_id);
		kfree_skb(skb);
		stats->tx_dropped++;
		return NETDEV_TX_OK;
	}

	if_num_inner = priv->tunnel[pre->tunnel_id].if_num_inner;
	if (unlikely(if_num_inner == -1)) {
		nss_capwapmgr_warn("%px: (CAPWAP packet) if_num_inner in the tunnel not set pre->tunnel_id %d\n", dev,
				pre->tunnel_id);
		kfree_skb(skb);
		stats->tx_dropped++;
		return NETDEV_TX_OK;
	}

	/*
	 * We use the lowest bit in the inner flow_id to determine which Tx ring
	 * to use (drv uses queue mapping to select Tx ring).
	 *
	 * This ring distribution will in turn get used in NSS firmware
	 * for better thread distribution of encap operation.
	 */
	skb_set_queue_mapping(skb, pre->flow_id & 0x1);

	status = nss_capwap_tx_buf(priv->nss_ctx, skb, if_num_inner);
	if (unlikely(status != NSS_TX_SUCCESS)) {
		if (status == NSS_TX_FAILURE_QUEUE) {
			nss_capwapmgr_warn("%px: netdev :%px queue is full", dev, dev);
			if (!netif_queue_stopped(dev)) {
				netif_stop_queue(dev);
			}
		}

		return NETDEV_TX_BUSY;
	}

	return NETDEV_TX_OK;
}

/*
 * nss_capwapmgr_fill_up_stats()
 *	Fills up stats in netdev's stats.
 */
static void nss_capwapmgr_fill_up_stats(struct rtnl_link_stats64 *stats, struct nss_capwap_tunnel_stats *tstats)
{
	stats->rx_packets += tstats->pnode_stats.rx_packets;
	stats->rx_dropped += tstats->pnode_stats.rx_dropped;

	/* rx_fifo_errors will appear as rx overruns in ifconfig */
	stats->rx_fifo_errors += (tstats->rx_n2h_drops + tstats->rx_n2h_queue_full_drops);
	stats->rx_errors += (tstats->rx_mem_failure_drops + tstats->rx_oversize_drops + tstats->rx_frag_timeout_drops);
	stats->rx_bytes += tstats->pnode_stats.rx_bytes;

	/* tx_fifo_errors  will appear as tx overruns in ifconfig */
	stats->tx_fifo_errors += tstats->tx_queue_full_drops;
	stats->tx_errors += tstats->tx_mem_failure_drops;
	stats->tx_bytes += tstats->pnode_stats.tx_bytes;

	stats->tx_dropped += (tstats->tx_dropped_sg_ref + tstats->tx_dropped_ver_mis + tstats->tx_dropped_hroom
			 + tstats->tx_dropped_dtls + tstats->tx_dropped_nwireless);
	stats->tx_packets += tstats->pnode_stats.tx_packets;
}

/*
 * nss_capwapmgr_get_tunnel_stats()
 *	Netdev get stats function to get tunnel stats
 */
static struct rtnl_link_stats64 *nss_capwapmgr_get_tunnel_stats(struct net_device *dev, struct rtnl_link_stats64 *stats)
{
	struct nss_capwap_tunnel_stats tstats;
	int i;

	if (!stats) {
		nss_capwapmgr_warn("%px: invalid rtnl structure\n", dev);
		return stats;
	}

	/*
	 * Netdev seems to be incrementing rx_dropped because we don't give IP header.
	 * So reset it as it's of no use for us.
	 */
	atomic_long_set(&dev->rx_dropped, 0);

	memset(stats, 0, sizeof (struct rtnl_link_stats64));
	nss_capwapmgr_fill_up_stats(stats, &global.tunneld_stats);

	for (i = NSS_DYNAMIC_IF_START; i <= (NSS_DYNAMIC_IF_START + NSS_MAX_DYNAMIC_INTERFACES); i++) {
		if (nss_capwap_get_stats(i, &tstats) == false) {
			continue;
		}

		nss_capwapmgr_fill_up_stats(stats, &tstats);
	}

	return stats;
}

/*
 * nss_capwapmgr_dev_tunnel_stats()
 *	Netdev ops function to retrieve stats for kernel version > 4.6
 */
static void nss_capwapmgr_dev_tunnel_stats(struct net_device *dev, struct rtnl_link_stats64 *stats)
{
	nss_capwapmgr_get_tunnel_stats(dev, stats);
}

/*
 * nss_capwapmgr_netdev_ops
 *	Netdev operations.
 */
static const struct net_device_ops nss_capwapmgr_netdev_ops = {
	.ndo_open		= nss_capwapmgr_open,
	.ndo_stop		= nss_capwapmgr_close,
	.ndo_start_xmit		= nss_capwapmgr_start_xmit,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_change_mtu		= eth_change_mtu,
	.ndo_get_stats64	= nss_capwapmgr_dev_tunnel_stats,
};

/*
 * nss_capwapmgr_dummy_netdev_setup()
 *	Netdev setup function.
 */
static void nss_capwapmgr_dummy_netdev_setup(struct net_device *dev)
{
	dev->addr_len = ETH_ALEN;
	dev->mtu = ETH_DATA_LEN;
	dev->needed_headroom = NSS_CAPWAP_HEADROOM;
	dev->needed_tailroom = 4;
	dev->type = ARPHRD_VOID;
	dev->ethtool_ops = NULL;
	dev->header_ops = NULL;
	dev->netdev_ops = &nss_capwapmgr_netdev_ops;
	dev->priv_destructor = NULL;

	memcpy(dev->dev_addr, "\x00\x00\x00\x00\x00\x00", dev->addr_len);
	memset(dev->broadcast, 0xff, dev->addr_len);
	memcpy(dev->perm_addr, dev->dev_addr, dev->addr_len);
}

/*
 * nss_capwapmgr_msg_event_receive()
 *	CAPWAP message callback for responses to commands sent to NSS FW
 *
 * This is command hanlder for all the messages since all we do is wake-up
 * the caller who is sending message to NSS FW.
 */
static void nss_capwapmgr_msg_event_receive(void *app_data, struct nss_capwap_msg *nim)
{
	struct net_device *dev = app_data;
	struct nss_cmn_msg *ncm = (struct nss_cmn_msg *)nim;
	struct nss_capwapmgr_response *r;
	struct nss_capwapmgr_priv *priv;
	uint32_t if_num;

	if (ncm->response == NSS_CMN_RESPONSE_NOTIFY) {
		return;
	}

	/*
	 * Since all CAPWAP messages are sync in nature we need to wake-up caller.
	 */
	if_num = ncm->interface - NSS_DYNAMIC_IF_START;
	dev_hold(dev);
	priv = netdev_priv(dev);
	r = &priv->resp[if_num];

	/*
	 * If somebody is waiting...
	 */
	if (atomic_read(&r->seq) != 0) {
		if (ncm->response != NSS_CMN_RESPONSE_ACK) {
			r->error = ncm->error;
		}

		r->response = ncm->response;
		atomic_dec(&r->seq);
		wake_up(&r->wq);
	}

	dev_put(dev);
}

/*
 * nss_capwap_remap_error()
 *	Remaps NSS FW response error to nss_capwapmgr_status_t
 */
static nss_capwapmgr_status_t nss_capwap_remap_error(nss_capwap_msg_response_t error)
{
	nss_capwapmgr_status_t status;

	switch (error) {
	case NSS_CAPWAP_ERROR_MSG_INVALID_REASSEMBLY_TIMEOUT:
		status = NSS_CAPWAPMGR_FAILURE_INVALID_REASSEMBLY_TIMEOUT;
		break;
	case NSS_CAPWAP_ERROR_MSG_INVALID_PATH_MTU:
		status = NSS_CAPWAPMGR_FAILURE_INVALID_PATH_MTU;
		break;
	case NSS_CAPWAP_ERROR_MSG_INVALID_MAX_FRAGMENT:
		status = NSS_CAPWAPMGR_FAILURE_INVALID_MAX_FRAGMENT;
		break;
	case NSS_CAPWAP_ERROR_MSG_INVALID_BUFFER_SIZE:
		status = NSS_CAPWAPMGR_FAILURE_INVALID_BUFFER_SIZE;
		break;
	case NSS_CAPWAP_ERROR_MSG_INVALID_L3_PROTO:
		status = NSS_CAPWAPMGR_FAILURE_INVALID_L3_PROTO;
		break;
	case NSS_CAPWAP_ERROR_MSG_INVALID_UDP_PROTO:
		status = NSS_CAPWAPMGR_FAILURE_INVALID_UDP_PROTO;
		break;
	case NSS_CAPWAP_ERROR_MSG_INVALID_VERSION:
		status = NSS_CAPWAPMGR_FAILURE_INVALID_VERSION;
		break;
	case NSS_CAPWAP_ERROR_MSG_TUNNEL_DISABLED:
		status = NSS_CAPWAPMGR_FAILURE_TUNNEL_DISABLED;
		break;
	case NSS_CAPWAP_ERROR_MSG_TUNNEL_ENABLED:
		status = NSS_CAPWAPMGR_FAILURE_TUNNEL_ENABLED;
		break;
	case NSS_CAPWAP_ERROR_MSG_TUNNEL_NOT_CFG:
		status = NSS_CAPWAPMGR_FAILURE_TUNNEL_NOT_CFG;
		break;
	default:
		status = NSS_CAPWAPMGR_FAILURE;
	}

	return status;
}

/*
 * nss_capwapmgr_get_tunnel()
 *	Common function to verify tunnel.
 *
 * The caller of the function should hold reference to the net device before calling.
 */
static nss_capwapmgr_status_t nss_capwapmgr_get_tunnel(struct net_device *dev, uint8_t tunnel_id, struct nss_capwapmgr_tunnel **t)
{
	struct nss_capwapmgr_priv *priv;

	if (!dev) {
		nss_capwapmgr_warn("Invalid net_device\n");
		return NSS_CAPWAPMGR_INVALID_NETDEVICE;
	}

	if (tunnel_id > NSS_CAPWAPMGR_MAX_TUNNELS) {
		nss_capwapmgr_warn("%px: tunnel_id: %d out of range (%d)\n", dev, tunnel_id, NSS_CAPWAPMGR_MAX_TUNNELS);
		return NSS_CAPWAPMGR_MAX_TUNNEL_COUNT_EXCEEDED;
	}

	priv = netdev_priv(dev);
	*t = &priv->tunnel[tunnel_id];
	if ( ((*t)->if_num_inner == -1) || ((*t)->if_num_outer == -1) ) {
		return NSS_CAPWAPMGR_FAILURE_TUNNEL_DOES_NOT_EXIST;
	}

	return NSS_CAPWAPMGR_SUCCESS;
}

/*
 * nss_capwapmgr_receive_pkt()
 *	Receives a pkt from NSS
 */
static void nss_capwapmgr_receive_pkt(struct net_device *dev, struct sk_buff *skb, struct napi_struct *napi)
{
	struct nss_capwapmgr_priv *priv;
	struct nss_capwap_metaheader *pre = (struct nss_capwap_metaheader *)skb->data;
	int32_t if_num;

	if (unlikely(skb->len < sizeof(struct nss_capwap_metaheader))) {
		nss_capwapmgr_warn("%px: skb len is short :%d", dev, skb->len);
		dev_kfree_skb_any(skb);
		return;
	}

	/*
	 * SKB NETIF START
	 */
	dev_hold(dev);
	priv = netdev_priv(dev);

	/*
	 * NSS FW sends interface number
	 */
	if_num = pre->tunnel_id;
	if (unlikely(if_num > NSS_MAX_DYNAMIC_INTERFACES)) {
		nss_capwapmgr_warn("%px: if_num %d is wrong for skb\n", dev, if_num);
		pre->tunnel_id = 0xFF;
	} else {
		/*
		 * Remap interface number to tunnel_id.
		 */
		pre->tunnel_id = priv->if_num_to_tunnel_id[if_num];
	}

	skb->dev = dev;
	skb->pkt_type = PACKET_HOST;
	skb->skb_iif = dev->ifindex;
	skb_reset_mac_header(skb);
	skb_reset_transport_header(skb);
	(void)netif_receive_skb(skb);

	/*
	 * SKB NETIF END
	 */
	dev_put(dev);
}

/*
 * nss_capwapmgr_register_with_nss()
 *	Internal function to register with NSS FW.
 */
static nss_capwapmgr_status_t nss_capwapmgr_register_with_nss(uint32_t interface_num, struct net_device *dev)
{
	/* features denote the skb_types supported */
	uint32_t features = 0;

	struct nss_ctx_instance *ctx = nss_capwap_data_register(interface_num, nss_capwapmgr_receive_pkt, dev, features);
	if (!ctx) {
		nss_capwapmgr_warn("%px: %d: nss_capwapmgr_data_register failed\n", dev, interface_num);
		return NSS_CAPWAPMGR_FAILURE;
	}

	return NSS_CAPWAPMGR_SUCCESS;
}

/*
 * nss_capwapmgr_unregister_with_nss()
 *	Internal function to unregister with NSS FW
 */
static void nss_capwapmgr_unregister_with_nss(uint32_t if_num)
{
	nss_capwapmgr_trace("%d: unregister with NSS FW\n", if_num);
	nss_capwap_data_unregister(if_num);
}

/*
 * nss_capwapmgr_tx_msg_sync()
 *	Waits for message to return.
 */
static nss_capwapmgr_status_t nss_capwapmgr_tx_msg_sync(struct nss_ctx_instance *ctx, struct net_device *dev, struct nss_capwap_msg *msg)
{
	struct nss_capwapmgr_priv *priv;
	struct nss_capwapmgr_response *r;
	uint32_t if_num;
	nss_capwapmgr_status_t status;

	if_num = msg->cm.interface - NSS_DYNAMIC_IF_START;
	dev_hold(dev);
	priv = netdev_priv(dev);
	r = &priv->resp[if_num];
	down(&r->sem);
	r->response = NSS_CMN_RESPONSE_ACK;

	/*
	 * Indicate that we are waiting
	 */
	atomic_set(&r->seq, 1);

	/*
	 * Call NSS driver
	 */
	status = nss_capwap_tx_msg(ctx, msg);
	if (status != NSS_CAPWAPMGR_SUCCESS) {
		up(&r->sem);
		dev_put(dev);
		return status;
	}

	if (!wait_event_timeout(r->wq, atomic_read(&r->seq) == 0, 5 * HZ)) {

		/*
		 * Indicate that we are no longer waiting
		 */
		atomic_set(&r->seq, 0);
		up(&r->sem);
		nss_capwapmgr_warn("%px: CAPWAP command msg response timeout\n", ctx);
		dev_put(dev);
		return NSS_CAPWAPMGR_FAILURE_CMD_TIMEOUT;
	}

	/*
	 * If NSS FW responded back with an error.
	 */
	if (r->response != NSS_CMN_RESPONSE_ACK) {
		up(&r->sem);
		nss_capwapmgr_warn("%px: CAPWAP command msg response : %d, error:%d\n", ctx,
				r->response, r->error);
		dev_put(dev);
		return nss_capwap_remap_error(r->error);
	}

	up(&r->sem);
	dev_put(dev);
	return status;
}

/*
 * nss_capwapmgr_create_capwap_rule()
 *	Internal function to create a CAPWAP rule
 */
static nss_capwapmgr_status_t nss_capwapmgr_create_capwap_rule(struct net_device *dev,
	uint32_t if_num, struct nss_capwap_rule_msg *msg, uint16_t type_flags)
{
	struct nss_ctx_instance *ctx = nss_capwap_get_ctx();
	struct nss_capwap_msg capwapmsg;
	struct nss_capwap_rule_msg *capwapcfg;
	nss_tx_status_t status;

	nss_capwapmgr_info("%px: ctx: CAPWAP Rule src_port: 0x%d dest_port:0x%d\n", ctx,
	    ntohl(msg->encap.src_port), ntohl(msg->encap.dest_port));

	/*
	 * Verify CAPWAP rule parameters.
	 */
	if (ntohl(msg->decap.reassembly_timeout) > NSS_CAPWAP_MAX_REASSEMBLY_TIMEOUT) {
		nss_capwapmgr_warn("%px: invalid reassem timeout: %d, max: %d\n",
			ctx, ntohl(msg->decap.reassembly_timeout), NSS_CAPWAP_MAX_REASSEMBLY_TIMEOUT);
		return NSS_CAPWAPMGR_FAILURE_INVALID_REASSEMBLY_TIMEOUT;
	}

	if (msg->decap.reassembly_timeout == 0) {

		/*
		 * 10 milli-seconds
		 */
		msg->decap.reassembly_timeout = htonl(NSS_CAPWAPMGR_REASSEMBLY_TIMEOUT);
	}

	if (ntohl(msg->decap.max_fragments) > NSS_CAPWAP_MAX_FRAGMENTS) {
		nss_capwapmgr_warn("%px: invalid fragment setting: %d, max: %d\n",
			ctx, ntohl(msg->decap.max_fragments), NSS_CAPWAP_MAX_FRAGMENTS);
		return NSS_CAPWAPMGR_FAILURE_INVALID_MAX_FRAGMENT;
	}

	if (msg->decap.max_fragments == 0) {
		msg->decap.max_fragments = htonl(NSS_CAPWAP_MAX_FRAGMENTS);
	}

	if (ntohl(msg->decap.max_buffer_size) > NSS_CAPWAP_MAX_BUFFER_SIZE) {
		nss_capwapmgr_warn("%px: invalid buffer size: %d, max: %d\n",
			ctx, ntohl(msg->decap.max_buffer_size), NSS_CAPWAP_MAX_BUFFER_SIZE);
		return NSS_CAPWAPMGR_FAILURE_INVALID_BUFFER_SIZE;
	}

	if (msg->decap.max_buffer_size == 0) {
		msg->decap.max_buffer_size = htonl(nss_capwap_get_max_buf_size(ctx));
	}

	if (ntohl(msg->encap.path_mtu) > NSS_CAPWAP_MAX_MTU) {
		nss_capwapmgr_warn("%px: invalid path_mtu: %d, max: %d\n",
			ctx, ntohl(msg->encap.path_mtu), NSS_CAPWAP_MAX_MTU);
		return NSS_CAPWAPMGR_FAILURE_INVALID_PATH_MTU;
	}

	if (msg->encap.path_mtu == 0) {
		msg->encap.path_mtu = htonl(NSS_CAPWAPMGR_NORMAL_FRAME_MTU);
	}

	msg->type_flags = type_flags;

	/*
	 * Prepare the tunnel configuration parameter to send to NSS FW
	 */
	memset(&capwapmsg, 0, sizeof(struct nss_capwap_msg));
	capwapcfg = &capwapmsg.msg.rule;
	memcpy(capwapcfg, msg, sizeof(struct nss_capwap_rule_msg));

	/*
	 * Send CAPWAP tunnel create command to NSS
	 */
	nss_capwap_msg_init(&capwapmsg, if_num, NSS_CAPWAP_MSG_TYPE_CFG_RULE,
			sizeof(struct nss_capwap_rule_msg),
			nss_capwapmgr_msg_event_receive, dev);

	status = nss_capwapmgr_tx_msg_sync(ctx, dev, &capwapmsg);
	if (status != NSS_TX_SUCCESS) {
		nss_capwapmgr_warn("%px: ctx: create encap data tunnel error %d \n", ctx, status);
		return status;
	}

	return NSS_CAPWAPMGR_SUCCESS;
}

/*
 * nss_capwapmgr_update_pppoe_rule()
 *	Updates PPPoE rule from PPPoE netdevice.
 */
static bool nss_capwapmgr_update_pppoe_rule(struct net_device *dev, struct ppe_drv_pppoe_session *pppoe_rule)
{
	struct pppoe_opt addressing;
	struct ppp_channel *channel[1] = {NULL};
	int px_proto;
	int ppp_ch_count;
	bool status = true;

	if (ppp_is_multilink(dev)) {
		nss_capwapmgr_warn("%px: channel is multilink PPP\n", dev);
		goto fail;
	}

	ppp_ch_count = ppp_hold_channels(dev, channel, 1);
	nss_capwapmgr_info("%px: PPP hold channel ret %d\n", dev, ppp_ch_count);
	if (ppp_ch_count != 1) {
		nss_capwapmgr_warn("%px: hold channel for netdevice failed\n", dev);
		goto fail;
	}

	px_proto = ppp_channel_get_protocol(channel[0]);
	if (px_proto != PX_PROTO_OE) {
		nss_capwapmgr_warn("%px: session socket is not of type PX_PROTO_OE\n", dev);
		goto fail;
	}

	if (pppoe_channel_addressing_get(channel[0], &addressing)) {
		nss_capwapmgr_warn("%px: failed to get addressing information\n", dev);
		goto fail;
	}

	/*
	 * Update the PPPoE rule information and set PPPoE valid flag.
	 */
	pppoe_rule->session_id = (uint16_t)ntohs((uint16_t)addressing.pa.sid);
	memcpy(pppoe_rule->server_mac, addressing.pa.remote, ETH_ALEN);

	nss_capwapmgr_info("%px: Update PPE PPPoE flow rule with session_id = %u, remote_mac = %pM\n",
			dev, pppoe_rule->session_id, pppoe_rule->server_mac);

	/*
	 * pppoe_channel_addressing_get returns held device.
	 */
	dev_put(addressing.dev);
	goto done;

fail:
	ppp_release_channels(channel, 1);
	status = false;
done:
	return status;
}

/*
 * nss_capwapmgr_ppe_destroy_ipv4_rule
 *	Internal Function to destroy IPv4 connection.
 */
static nss_capwapmgr_status_t nss_capwapmgr_ppe_destroy_ipv4_rule(struct nss_capwapmgr_tunnel *t)
{
	ppe_drv_ret_t ppe_status;
	struct ppe_drv_v4_rule_destroy pd4rd;
	memset(&pd4rd, 0, sizeof (struct ppe_drv_v4_rule_destroy));
	pd4rd.tuple.protocol = IPPROTO_UDP;
	pd4rd.tuple.flow_ip = t->ip_rule.v4.src_ip;
	pd4rd.tuple.flow_ident = (uint32_t)t->ip_rule.v4.src_port;
	pd4rd.tuple.return_ip = t->ip_rule.v4.dest_ip;
	pd4rd.tuple.return_ident = (uint32_t)t->ip_rule.v4.dest_port;

	ppe_status = ppe_drv_v4_destroy(&pd4rd);
	if (ppe_status != PPE_DRV_RET_SUCCESS) {
		nss_capwapmgr_warn("%px: Unconfigure ipv4 rule failed : %d\n", t, ppe_status);
		return NSS_CAPWAPMGR_FAILURE_IP_DESTROY_RULE;
	}

	t->tunnel_state &= ~NSS_CAPWAPMGR_TUNNEL_STATE_IPRULE_CONFIGURED;
	return NSS_CAPWAPMGR_SUCCESS;
}

/*
 * nss_capwapmgr_ppe_create_ipv4_rule
 *	Internal function to configure IPv4 connection.
 */
static nss_capwapmgr_status_t nss_capwapmgr_ppe_create_ipv4_rule(struct nss_capwapmgr_tunnel *t, struct nss_ipv4_create *v4)
{
	struct ppe_drv_v4_rule_create *pd4rc;
	ppe_drv_ret_t ppe_status;
	nss_capwapmgr_status_t status = NSS_CAPWAPMGR_SUCCESS;
	bool use_top_interface = false;

	pd4rc = (struct ppe_drv_v4_rule_create *)kzalloc(sizeof(struct ppe_drv_v4_rule_create), GFP_KERNEL);
	if (!pd4rc) {
		nss_capwapmgr_warn("%px: No memory to allocate ppe ipv4 rule create structure\n", t);
		goto fail;
	}

	pd4rc->valid_flags = 0;
	pd4rc->rule_flags = 0;

	/*
	 * Copy over the 5 tuple details.
	 */
	pd4rc->tuple.protocol = (uint8_t)v4->protocol;
	pd4rc->tuple.flow_ip = v4->src_ip;
	pd4rc->tuple.flow_ident = (uint32_t)v4->src_port;
	pd4rc->tuple.return_ip = v4->dest_ip;
	pd4rc->tuple.return_ident = (uint32_t)v4->dest_port;

	/*
	 * Copy over the connection rules and set the CONN_VALID flag
	 */
	pd4rc->conn_rule.rx_if = v4->src_interface_num;
	pd4rc->conn_rule.flow_mtu = v4->from_mtu;
	pd4rc->conn_rule.flow_ip_xlate = v4->src_ip_xlate;
	pd4rc->conn_rule.flow_ident_xlate = (uint32_t)v4->src_port_xlate;
	memcpy(pd4rc->conn_rule.flow_mac, v4->src_mac, 6);

	pd4rc->conn_rule.tx_if = v4->dest_interface_num;
	pd4rc->conn_rule.return_mtu = v4->to_mtu;
	pd4rc->conn_rule.return_ip_xlate = v4->dest_ip_xlate;
	pd4rc->conn_rule.return_ident_xlate = (uint32_t)v4->dest_port_xlate;
	if (pd4rc->tuple.return_ip != pd4rc->conn_rule.return_ip_xlate ||
		pd4rc->tuple.return_ident != pd4rc->conn_rule.return_ident_xlate)  {
		memcpy(pd4rc->conn_rule.return_mac, v4->dest_mac_xlate, 6);
	} else {
		memcpy(pd4rc->conn_rule.return_mac, v4->dest_mac, 6);
	}

	pd4rc->rule_flags |= PPE_DRV_V4_RULE_FLAG_RETURN_VALID | PPE_DRV_V4_RULE_FLAG_FLOW_VALID;


	if ((v4->in_vlan_tag[0] & 0xFFF) != 0xFFF) {
		/*
	 	 * Copy over the VLAN tag and set the VLAN_VALID flag.
		 * IP rule direction is AC->AP, so we only update the ingress VLAN tag.
	 	 */
		pd4rc->vlan_rule.primary_vlan.ingress_vlan_tag = v4->in_vlan_tag[0];
		pd4rc->vlan_rule.primary_vlan.egress_vlan_tag = NSS_CAPWAPMGR_VLAN_TAG_NOT_CONFIGURED;
		pd4rc->vlan_rule.secondary_vlan.ingress_vlan_tag = NSS_CAPWAPMGR_VLAN_TAG_NOT_CONFIGURED;
		pd4rc->vlan_rule.secondary_vlan.egress_vlan_tag = NSS_CAPWAPMGR_VLAN_TAG_NOT_CONFIGURED;
		pd4rc->valid_flags |= PPE_DRV_V4_VALID_FLAG_VLAN;
		use_top_interface = true;
	}

	if (v4->flow_pppoe_if_exist) {
		/*
	 	 * Copy over the PPPOE rules and set PPPOE_VALID flag.
	 	 */
		if (!nss_capwapmgr_update_pppoe_rule(v4->top_ndev, &pd4rc->pppoe_rule.flow_session)) {
			nss_capwapmgr_warn("%px:PPPoE rule update failed\n",t);
			goto fail;
		}

		pd4rc->valid_flags |= PPE_DRV_V4_VALID_FLAG_FLOW_PPPOE;
		use_top_interface = true;
	}

	/*
	 * If Ingress VLAN tag or pppoe rule is present, set the top interface.
	 */
	if (use_top_interface) {
		pd4rc->top_rule.rx_if = ppe_drv_iface_idx_get_by_dev(v4->top_ndev);
	} else {
		pd4rc->top_rule.rx_if = v4->src_interface_num;
	}

	pd4rc->top_rule.tx_if = v4->dest_interface_num;
	ppe_status = ppe_drv_v4_create(pd4rc);
	if (ppe_status != PPE_DRV_RET_SUCCESS) {
		nss_capwapmgr_warn("%px:PPE rule create failed\n",t);
		goto fail;
	}

	/*
	 * Update the tunnel state.
	 */
	t->tunnel_state |= NSS_CAPWAPMGR_TUNNEL_STATE_IPRULE_CONFIGURED;
	goto done;

fail:
	status = NSS_CAPWAPMGR_FAILURE_IP_RULE;
done:
	kfree(pd4rc);
	return status;
}

/*
 * nss_capwapmgr_ppe_destroy_ipv6_rule
 *	Internal Function to destroy IPv6 connection.
 */
static nss_capwapmgr_status_t nss_capwapmgr_ppe_destroy_ipv6_rule(struct nss_capwapmgr_tunnel *t)
{
	ppe_drv_ret_t ppe_status;
	struct ppe_drv_v6_rule_destroy pd6rd;
	memset(&pd6rd, 0, sizeof (struct nss_ipv6_destroy));

	if (t->capwap_rule.which_udp == NSS_CAPWAP_TUNNEL_UDP) {
		pd6rd.tuple.protocol = IPPROTO_UDP;
	} else {
		pd6rd.tuple.protocol = IPPROTO_UDPLITE;
	}

	pd6rd.tuple.flow_ip[0] = t->ip_rule.v6.src_ip[0];
	pd6rd.tuple.flow_ip[1] = t->ip_rule.v6.src_ip[1];
	pd6rd.tuple.flow_ip[2] = t->ip_rule.v6.src_ip[2];
	pd6rd.tuple.flow_ip[3] = t->ip_rule.v6.src_ip[3];

	pd6rd.tuple.return_ip[0] = t->ip_rule.v6.dest_ip[0];
	pd6rd.tuple.return_ip[1] = t->ip_rule.v6.dest_ip[1];
	pd6rd.tuple.return_ip[2] = t->ip_rule.v6.dest_ip[2];
	pd6rd.tuple.return_ip[3] = t->ip_rule.v6.dest_ip[3];

	pd6rd.tuple.flow_ident = t->ip_rule.v6.src_port;
	pd6rd.tuple.return_ident = t->ip_rule.v6.dest_port;
	ppe_status = ppe_drv_v6_destroy(&pd6rd);
	if (ppe_status != PPE_DRV_RET_SUCCESS) {
		nss_capwapmgr_warn("%px: unconfigure ipv6 rule failed : %d\n", t, ppe_status);
		return NSS_CAPWAPMGR_FAILURE_IP_DESTROY_RULE;
	}

	t->tunnel_state &= ~NSS_CAPWAPMGR_TUNNEL_STATE_IPRULE_CONFIGURED;
	return NSS_CAPWAPMGR_SUCCESS;
}

/*
 * nss_capwapmgr_ppe_create_ipv6_rule
 *	Internal function to configure IPv6 connection.
 */
static nss_capwapmgr_status_t nss_capwapmgr_ppe_create_ipv6_rule(struct nss_capwapmgr_tunnel *t, struct nss_ipv6_create *v6)
{
	struct ppe_drv_v6_rule_create *pd6rc;
	ppe_drv_ret_t ppe_status;
	nss_capwapmgr_status_t status = NSS_CAPWAPMGR_SUCCESS;
	bool use_top_interface = false;

	pd6rc = (struct ppe_drv_v6_rule_create *)kzalloc(sizeof(struct ppe_drv_v6_rule_create), GFP_KERNEL);
	if (!pd6rc) {
		nss_capwapmgr_warn("%px:No memory to allocate ppe ipv6 rule create structure\n", v6);
		goto fail;
	}

	pd6rc->valid_flags = 0;
	pd6rc->rule_flags = 0;

	/*
	 * Copy over the 5 tuple information.
	 */
	pd6rc->tuple.protocol = (uint8_t)v6->protocol;
	pd6rc->tuple.flow_ip[0] = v6->src_ip[0];
	pd6rc->tuple.flow_ip[1] = v6->src_ip[1];
	pd6rc->tuple.flow_ip[2] = v6->src_ip[2];
	pd6rc->tuple.flow_ip[3] = v6->src_ip[3];
	pd6rc->tuple.flow_ident = (uint32_t)v6->src_port;

	pd6rc->tuple.return_ip[0] = v6->dest_ip[0];
	pd6rc->tuple.return_ip[1] = v6->dest_ip[1];
	pd6rc->tuple.return_ip[2] = v6->dest_ip[2];
	pd6rc->tuple.return_ip[3] = v6->dest_ip[3];
	pd6rc->tuple.return_ident = (uint32_t)v6->dest_port;

	/*
	 * Copy over the connection rules and set the CONN_VALID flag
	 */
	pd6rc->conn_rule.rx_if = v6->src_interface_num;
	pd6rc->conn_rule.flow_mtu = v6->from_mtu;
	memcpy(pd6rc->conn_rule.flow_mac, v6->src_mac, 6);

	pd6rc->conn_rule.tx_if = v6->dest_interface_num;
	pd6rc->conn_rule.return_mtu = v6->to_mtu;
	memcpy(pd6rc->conn_rule.return_mac, v6->dest_mac, 6);

	pd6rc->rule_flags |= PPE_DRV_V6_RULE_FLAG_RETURN_VALID | PPE_DRV_V6_RULE_FLAG_FLOW_VALID;

	if ((v6->in_vlan_tag[0] & 0xFFF) != 0xFFF) {
		/*
	 	 * Copy over the VLAN tag and set the VLAN_VALID flag.
		 * IP rule direction is AC->AP, so we only update the ingress VLAN tag.
	 	 */
		pd6rc->vlan_rule.primary_vlan.ingress_vlan_tag = v6->in_vlan_tag[0];
		pd6rc->vlan_rule.primary_vlan.egress_vlan_tag = NSS_CAPWAPMGR_VLAN_TAG_NOT_CONFIGURED;
		pd6rc->vlan_rule.secondary_vlan.ingress_vlan_tag = NSS_CAPWAPMGR_VLAN_TAG_NOT_CONFIGURED;
		pd6rc->vlan_rule.secondary_vlan.egress_vlan_tag = NSS_CAPWAPMGR_VLAN_TAG_NOT_CONFIGURED;
		pd6rc->valid_flags |= PPE_DRV_V6_VALID_FLAG_VLAN;
		use_top_interface = true;
	}

	if (v6->flow_pppoe_if_exist) {
		/*
		 * Copy over the PPPOE rules and set PPPOE_VALID flag.
		 */
		if (!nss_capwapmgr_update_pppoe_rule(v6->top_ndev, &pd6rc->pppoe_rule.flow_session)) {
			nss_capwapmgr_warn("%px:PPPoE rule update failed\n",t);
			goto fail;
		}

		pd6rc->valid_flags |= PPE_DRV_V6_VALID_FLAG_PPPOE_FLOW;
		use_top_interface = true;
	}

	/*
	 * If Ingress VLAN tag or pppoe rule is present, set the top interface.
	 */
	if (use_top_interface) {
		pd6rc->top_rule.rx_if = ppe_drv_iface_idx_get_by_dev(v6->top_ndev);
	} else {
		pd6rc->top_rule.rx_if = v6->src_interface_num;
	}

	pd6rc->top_rule.tx_if = v6->dest_interface_num;
	ppe_status = ppe_drv_v6_create(pd6rc);
	if (ppe_status != PPE_DRV_RET_SUCCESS) {
		nss_capwapmgr_warn("%px:PPE rule create failed\n", v6);
		goto fail;
	}

	/*
	 * Update the tunnel state.
	 */
	t->tunnel_state |= NSS_CAPWAPMGR_TUNNEL_STATE_IPRULE_CONFIGURED;
	goto done;

fail:
	status = NSS_CAPWAPMGR_FAILURE_IP_RULE;
done:
	kfree(pd6rc);
	return status;
}

/*
 * nss_capwapmgr_tx_msg_enable_tunnel()
 *	Common function to send CAPWAP tunnel enable msg
 */
static nss_tx_status_t nss_capwapmgr_tx_msg_enable_tunnel(struct nss_ctx_instance *ctx, struct net_device *dev, uint32_t if_num, uint32_t sibling_if_num)
{
	struct nss_capwap_msg capwapmsg;
	nss_tx_status_t status;

	/*
	 * Prepare the tunnel configuration parameter to send to NSS FW
	 */
	memset(&capwapmsg, 0, sizeof(struct nss_capwap_msg));
	capwapmsg.msg.enable_tunnel.sibling_if_num = sibling_if_num;

	/*
	 * Send CAPWAP data tunnel command to NSS
	 */
	nss_capwap_msg_init(&capwapmsg, if_num, NSS_CAPWAP_MSG_TYPE_ENABLE_TUNNEL, sizeof(struct nss_capwap_enable_tunnel_msg), nss_capwapmgr_msg_event_receive, dev);

	status = nss_capwapmgr_tx_msg_sync(ctx, dev, &capwapmsg);
	if (status != NSS_TX_SUCCESS) {
		nss_capwapmgr_warn("%px: ctx: CMD: %d Tunnel error : %d \n", ctx, NSS_CAPWAP_MSG_TYPE_ENABLE_TUNNEL, status);
	}

	return status;
}

/*
 * nss_capwapmgr_tunnel_action()
 *	Common function for CAPWAP tunnel operation messages without
 *	any message data structures.
 */
static nss_tx_status_t nss_capwapmgr_tunnel_action(struct nss_ctx_instance *ctx, struct net_device *dev, uint32_t if_num, nss_capwap_msg_type_t cmd)
{
	struct nss_capwap_msg capwapmsg;
	nss_tx_status_t status;

	/*
	 * Prepare the tunnel configuration parameter to send to NSS FW
	 */
	memset(&capwapmsg, 0, sizeof(struct nss_capwap_msg));

	/*
	 * Send CAPWAP data tunnel command to NSS
	 */
	nss_capwap_msg_init(&capwapmsg, if_num, cmd, 0, nss_capwapmgr_msg_event_receive, dev);

	status = nss_capwapmgr_tx_msg_sync(ctx, dev, &capwapmsg);
	if (status != NSS_TX_SUCCESS) {
		nss_capwapmgr_warn("%px: ctx: CMD: %d Tunnel error : %d \n", ctx, cmd, status);
	}

	return status;
}

/*
 * nss_capwapmgr_tx_msg_update_vp_num()
 *	Function to send update vp message.
 */
static nss_tx_status_t nss_capwapmgr_tx_msg_update_vp_num(struct net_device *dev, uint32_t if_num, int16_t vp_num)
{
	struct nss_capwap_msg capwapmsg;
	nss_tx_status_t status;
	struct nss_capwapmgr_priv *priv = netdev_priv(dev);
	struct nss_ctx_instance *ctx = priv->nss_ctx;

	/*
	 * Prepare the tunnel configuration parameter to send to NSS FW
	 */
	memset(&capwapmsg, 0, sizeof(struct nss_capwap_msg));
	capwapmsg.msg.update_vp_num.vp_num = vp_num;

	/*
	 * Send CAPWAP data tunnel command to NSS
	 */
	nss_capwap_msg_init(&capwapmsg, if_num, NSS_CAPWAP_MSG_TYPE_UPDATE_VP_NUM, sizeof(struct nss_capwap_update_vp_num_msg), nss_capwapmgr_msg_event_receive, dev);

	status = nss_capwapmgr_tx_msg_sync(ctx, dev, &capwapmsg);
	if (status != NSS_TX_SUCCESS) {
		nss_capwapmgr_warn("%px: ctx: CMD: %d Tunnel error : %d \n", ctx, NSS_CAPWAP_MSG_TYPE_UPDATE_VP_NUM, status);
	}

	return status;
}

/*
 * nss_capwapmgr_tunnel_create_common()
 *	Common handling for creating IPv4 or IPv6 tunnel
 */
static nss_capwapmgr_status_t nss_capwapmgr_tunnel_create_common(struct net_device *dev, uint8_t tunnel_id,
	struct nss_ipv4_create *v4, struct nss_ipv6_create *v6, struct nss_capwap_rule_msg *capwap_rule)
{
	struct nss_capwapmgr_priv *priv;
	struct nss_capwapmgr_tunnel *t = NULL;
	nss_capwapmgr_status_t status = NSS_CAPWAPMGR_SUCCESS;
	int32_t capwap_if_num_inner, capwap_if_num_outer;
	uint16_t type_flags = 0;
	struct ppe_vp_ai vpai;
	struct net_device *internal_dev = NULL;
	ppe_vp_num_t vp_num;

	if (!v4 && !v6) {
		nss_capwapmgr_warn("%px: invalid ip create rule for tunnel: %d\n", dev, tunnel_id);
		return NSS_CAPWAPMGR_INVALID_IP_RULE;
	}

	if (!(capwap_rule->l3_proto == NSS_CAPWAP_TUNNEL_IPV4 ||
		capwap_rule->l3_proto == NSS_CAPWAP_TUNNEL_IPV6)) {
		nss_capwapmgr_warn("%px: tunnel %d: wrong argument for l3_proto\n", dev, tunnel_id);
		return NSS_CAPWAPMGR_FAILURE_INVALID_L3_PROTO;
	}

	if (!(capwap_rule->which_udp == NSS_CAPWAP_TUNNEL_UDP ||
		capwap_rule->which_udp == NSS_CAPWAP_TUNNEL_UDPLite)) {
		nss_capwapmgr_warn("%px: tunnel %d: wrong argument for which_udp\n", dev, tunnel_id);
		return NSS_CAPWAPMGR_FAILURE_INVALID_UDP_PROTO;
	}

	/*
	 * Allocate a internal net_device for every tunnel.
	 */
	internal_dev = alloc_netdev(0,"capwapint%d",
				NET_NAME_ENUM, nss_capwapmgr_dummy_netdev_setup);
	if (!internal_dev) {
		nss_capwapmgr_warn("Error allocating internal netdev\n");
		return NSS_CAPWAPMGR_FAILRUE_INTERNAL_NETDEV_ALLOC_FAILED;
	}

	/*
	 * Update the MTU of internal dev as PPE VP sets the PPE iface MTU
	 * based on netdevs MTU.
	 */
	memset(&vpai, 0, sizeof(struct ppe_vp_ai));
	if (v4) {
		internal_dev->mtu = v4->to_mtu;
	} else {
		internal_dev->mtu = v6->to_mtu;
	}
	vpai.type = PPE_VP_TYPE_SW_L3;
	vpai.queue_num = edma_cfg_rx_point_offload_ring_queue_get();

	/*
	 * Allocate a PPE VP
	 */
	vp_num = ppe_vp_alloc(internal_dev, &vpai);
	if (vp_num == -1) {
		nss_capwapmgr_warn("%px: VP alloc failed", dev);
		free_netdev(internal_dev);
		return NSS_CAPWAPMGR_FAILURE_VP_ALLOC;
	}

	dev_hold(dev);

	/*
	 * We create tunnel only when the tunnel does not exist.
	 */
	status = nss_capwapmgr_get_tunnel(dev, tunnel_id, &t);
	if (status != NSS_CAPWAPMGR_FAILURE_TUNNEL_DOES_NOT_EXIST) {
		nss_capwapmgr_warn("%px: tunnel: %d get error: %d\n", dev, tunnel_id, status);
		goto fail;
	}

	capwap_if_num_inner = nss_dynamic_interface_alloc_node(NSS_DYNAMIC_INTERFACE_TYPE_CAPWAP_HOST_INNER);
	if (capwap_if_num_inner < 0) {
		nss_capwapmgr_warn("%px: di returned error : %d\n", dev, capwap_if_num_inner);
		status = NSS_CAPWAPMGR_FAILURE_DI_ALLOC_FAILED;
		goto fail;
	}
	t->tunnel_state |= NSS_CAPWAPMGR_TUNNEL_STATE_INNER_ALLOCATED;

	if (nss_capwapmgr_register_with_nss(capwap_if_num_inner, dev) != NSS_CAPWAPMGR_SUCCESS) {
		nss_capwapmgr_warn("%d: NSS CAPWAP register with NSS failed", capwap_if_num_inner);
		status = NSS_CAPWAPMGR_FAILURE_REGISTER_NSS;
		goto fail1;
	}

	capwap_if_num_outer = nss_dynamic_interface_alloc_node(NSS_DYNAMIC_INTERFACE_TYPE_CAPWAP_OUTER);
	if (capwap_if_num_outer < 0) {
		nss_capwapmgr_warn("%px: di returned error : %d\n", dev, capwap_if_num_outer);
		status = NSS_CAPWAPMGR_FAILURE_DI_ALLOC_FAILED;
		goto fail2;
	}
	t->tunnel_state |= NSS_CAPWAPMGR_TUNNEL_STATE_OUTER_ALLOCATED;

	if (nss_capwapmgr_register_with_nss(capwap_if_num_outer, dev) != NSS_CAPWAPMGR_SUCCESS) {
		nss_capwapmgr_warn("%d: NSS CAPWAP register with NSS failed", capwap_if_num_outer);
		status = NSS_CAPWAPMGR_FAILURE_REGISTER_NSS;
		goto fail3;
	}

	if (nss_capwapmgr_tx_msg_update_vp_num(dev, capwap_if_num_outer, vp_num) != NSS_TX_SUCCESS) {
		nss_capwapmgr_warn("%px: %d VP number update failed %d", dev, vp_num, status);
		status = NSS_CAPWAPMGR_FAILURE_UPDATE_VP_NUM;
		goto fail4;
	}

	if (nss_capwapmgr_tx_msg_update_vp_num(dev, capwap_if_num_inner, vp_num) != NSS_TX_SUCCESS) {
		nss_capwapmgr_warn("%px: %d VP number update failed %d", dev, vp_num, status);
		status = NSS_CAPWAPMGR_FAILURE_UPDATE_VP_NUM;
		goto fail4;
	}

	/*
	 * We use type_flags to determine the correct header sizes
	 * for a frame when encaping. CAPWAP processing node in the
	 * NSS FW does not know anything about IP rule information.
	 */
	if (v4) {
		if ((v4->out_vlan_tag[0] & 0xFFF) != 0xFFF) {
			type_flags |= NSS_CAPWAP_RULE_CREATE_VLAN_CONFIGURED;
		}

		if (v4->flow_pppoe_if_exist) {
			type_flags |= NSS_CAPWAP_RULE_CREATE_PPPOE_CONFIGURED;
		}
	} else {
		if ((v6->out_vlan_tag[0] & 0xFFF) != 0xFFF) {
			type_flags |= NSS_CAPWAP_RULE_CREATE_VLAN_CONFIGURED;
		}

		if (v6->flow_pppoe_if_exist) {
			type_flags |= NSS_CAPWAP_RULE_CREATE_PPPOE_CONFIGURED;
		}
	}

	if (capwap_rule->l3_proto == NSS_CAPWAP_TUNNEL_IPV6 && capwap_rule->which_udp == NSS_CAPWAP_TUNNEL_UDPLite) {
		type_flags |= NSS_CAPWAP_ENCAP_UDPLITE_HDR_CSUM;
	}

	/*
	 * Copy over the IP rule information to capwap rule for encap side.
	 * This will avoid any confusions because IP rule direction is AC->AP and
	 * capwap encap rule direction is AP->AC.
	 */
	if (v4) {
		capwap_rule->encap.src_port = htonl(v4->dest_port);
		capwap_rule->encap.src_ip.ip.ipv4 = htonl(v4->dest_ip);

		capwap_rule->encap.dest_port = htonl(v4->src_port);
		capwap_rule->encap.dest_ip.ip.ipv4 = htonl(v4->src_ip);
	} else {
		capwap_rule->encap.src_port = htonl(v6->dest_port);
		capwap_rule->encap.src_ip.ip.ipv6[0] = htonl(v6->dest_ip[0]);
		capwap_rule->encap.src_ip.ip.ipv6[1] = htonl(v6->dest_ip[1]);
		capwap_rule->encap.src_ip.ip.ipv6[2] = htonl(v6->dest_ip[2]);
		capwap_rule->encap.src_ip.ip.ipv6[3] = htonl(v6->dest_ip[3]);

		capwap_rule->encap.dest_port = htonl(v6->src_port);
		capwap_rule->encap.dest_ip.ip.ipv6[0] = htonl(v6->src_ip[0]);
		capwap_rule->encap.dest_ip.ip.ipv6[1] = htonl(v6->src_ip[1]);
		capwap_rule->encap.dest_ip.ip.ipv6[2] = htonl(v6->src_ip[2]);
		capwap_rule->encap.dest_ip.ip.ipv6[3] = htonl(v6->src_ip[3]);
	}

	status = nss_capwapmgr_create_capwap_rule(dev, capwap_if_num_inner, capwap_rule, type_flags);
	nss_capwapmgr_info("%px: dynamic interface if_num is :%d and capwap tunnel status:%d\n", dev, capwap_if_num_inner, status);
	if (status != NSS_CAPWAPMGR_SUCCESS) {
		nss_capwapmgr_warn("%px: %d: CAPWAP rule create failed with status: %d", dev, capwap_if_num_inner, status);
		status = NSS_CAPWAPMGR_FAILURE_CAPWAP_RULE;
		goto fail4;
	}

	status = nss_capwapmgr_create_capwap_rule(dev, capwap_if_num_outer, capwap_rule, type_flags);
	nss_capwapmgr_info("%px: dynamic interface if_num is :%d and capwap tunnel status:%d\n", dev, capwap_if_num_outer, status);
	if (status != NSS_CAPWAPMGR_SUCCESS) {
		nss_capwapmgr_warn("%px: %d: CAPWAP rule create failed with status: %d", dev, capwap_if_num_outer, status);
		status = NSS_CAPWAPMGR_FAILURE_CAPWAP_RULE;
		goto fail5;
	}

	priv = netdev_priv(dev);
	t = &priv->tunnel[tunnel_id];
	if (v4) {
		v4->dest_interface_num = ppe_drv_iface_idx_get_by_dev(internal_dev);
		status = nss_capwapmgr_ppe_create_ipv4_rule(t, v4);
	} else {
		v6->dest_interface_num = ppe_drv_iface_idx_get_by_dev(internal_dev);
		status = nss_capwapmgr_ppe_create_ipv6_rule(t, v6);
	}

	if (status != NSS_CAPWAPMGR_SUCCESS) {
		nss_capwapmgr_warn("%px: IPv4/IPv6 rule create failed with status: %d", dev, status);
		goto fail6;
	}

	nss_capwapmgr_info("%px: %d: %d: CAPWAP TUNNEL CREATE DONE tunnel_id:%d (%px)\n", dev, capwap_if_num_inner, capwap_if_num_outer, tunnel_id, t);

	/*
	 * Keep a copy of rule information.
	 */
	if (v4) {
		memcpy(&t->ip_rule.v4, v4, sizeof (struct nss_ipv4_create));
	} else {
		memcpy(&t->ip_rule.v6, v6, sizeof (struct nss_ipv6_create));
	}

	memcpy(&t->capwap_rule, capwap_rule, sizeof (struct nss_capwap_rule_msg));

	/*
	 * Make it globally visible inside the netdev.
	 */
	t->internal_dev = internal_dev;
	t->if_num_inner = capwap_if_num_inner;
	t->if_num_outer = capwap_if_num_outer;
	priv->if_num_to_tunnel_id[capwap_if_num_inner] = tunnel_id;
	priv->if_num_to_tunnel_id[capwap_if_num_outer] = tunnel_id;
	t->tunnel_state |= NSS_CAPWAPMGR_TUNNEL_STATE_CONFIGURED;
	t->type_flags = type_flags;
	t->vp_num = vp_num;

	goto done;

fail6:
	nss_capwapmgr_tunnel_action(priv->nss_ctx, dev, capwap_if_num_outer, NSS_CAPWAP_MSG_TYPE_UNCFG_RULE);
fail5:
	nss_capwapmgr_tunnel_action(priv->nss_ctx, dev, capwap_if_num_inner, NSS_CAPWAP_MSG_TYPE_UNCFG_RULE);
fail4:
	nss_capwapmgr_unregister_with_nss(capwap_if_num_outer);
fail3:
	nss_dynamic_interface_dealloc_node(capwap_if_num_outer, NSS_DYNAMIC_INTERFACE_TYPE_CAPWAP_OUTER);
fail2:
	nss_capwapmgr_unregister_with_nss(capwap_if_num_inner);
fail1:
	nss_dynamic_interface_dealloc_node(capwap_if_num_inner, NSS_DYNAMIC_INTERFACE_TYPE_CAPWAP_HOST_INNER);
fail:
	ppe_vp_free(vp_num);
	free_netdev(internal_dev);

done:
	dev_put(dev);
	return status;
}

/*
 * nss_capwapmgr_tunnel_save_stats()
 *	Internal function to save tunnel stats when a tunnel is being
 *	destroyed.
 */
static void nss_capwapmgr_tunnel_save_stats(struct nss_capwap_tunnel_stats *save, struct nss_capwap_tunnel_stats *fstats)
{
	save->dtls_pkts += fstats->dtls_pkts;

	save->rx_segments += fstats->rx_segments;
	save->rx_dup_frag += fstats->rx_dup_frag;
	save->rx_oversize_drops += fstats->rx_oversize_drops;
	save->rx_frag_timeout_drops += fstats->rx_frag_timeout_drops;
	save->rx_n2h_drops += fstats->rx_n2h_drops;
	save->rx_n2h_queue_full_drops += fstats->rx_n2h_queue_full_drops;
	save->rx_mem_failure_drops += fstats->rx_mem_failure_drops;
	save->rx_csum_drops += fstats->rx_csum_drops;
	save->rx_malformed += fstats->rx_malformed;
	save->rx_frag_gap_drops += fstats->rx_frag_gap_drops;

	save->tx_segments += fstats->tx_segments;
	save->tx_queue_full_drops += fstats->tx_queue_full_drops;
	save->tx_mem_failure_drops += fstats->tx_mem_failure_drops;
	save->tx_dropped_sg_ref += fstats->tx_dropped_sg_ref;
	save->tx_dropped_ver_mis += fstats->tx_dropped_ver_mis;
	save->tx_dropped_hroom += fstats->tx_dropped_hroom;
	save->tx_dropped_dtls += fstats->tx_dropped_dtls;
	save->tx_dropped_nwireless += fstats->tx_dropped_nwireless;

	/*
	 * add pnode stats now.
	 */
	save->pnode_stats.rx_packets += fstats->pnode_stats.rx_packets;
	save->pnode_stats.rx_bytes += fstats->pnode_stats.rx_bytes;
	save->pnode_stats.rx_dropped += fstats->pnode_stats.rx_dropped;
	save->pnode_stats.tx_packets += fstats->pnode_stats.tx_packets;
	save->pnode_stats.tx_bytes += fstats->pnode_stats.tx_bytes;
}

/*
 * nss_capwapmgr_flow_rule_action()
 */
static inline nss_capwapmgr_status_t nss_capwapmgr_flow_rule_action(struct net_device *dev, uint8_t tunnel_id,
						nss_capwap_msg_type_t cmd, uint16_t ip_version,
						uint16_t protocol, uint32_t *src_ip, uint32_t *dst_ip,
						uint16_t src_port, uint16_t dst_port, uint32_t flow_id)
{
	struct nss_capwapmgr_priv *priv;
	struct nss_capwap_msg capwapmsg;
	struct nss_capwap_flow_rule_msg *ncfrm;
	struct nss_capwapmgr_tunnel *t = NULL;
	nss_capwapmgr_status_t status;

	dev_hold(dev);
	status = nss_capwapmgr_get_tunnel(dev, tunnel_id, &t);
	if (status != NSS_CAPWAPMGR_SUCCESS) {
		nss_capwapmgr_warn("%px: can't find tunnel: %d\n", dev, tunnel_id);
		goto done;
	}

	priv = netdev_priv(dev);

	memset(&capwapmsg, 0, sizeof(struct nss_capwap_msg));
	nss_capwap_msg_init(&capwapmsg, t->if_num_outer, cmd,
		sizeof(struct nss_capwap_flow_rule_msg), nss_capwapmgr_msg_event_receive, dev);

	/*
	 * Set flow rule message
	 */
	if (cmd == NSS_CAPWAP_MSG_TYPE_FLOW_RULE_ADD) {
		ncfrm = &capwapmsg.msg.flow_rule_add;
	} else {
		ncfrm = &capwapmsg.msg.flow_rule_del;
	}
	ncfrm->protocol = protocol;
	ncfrm->src_port = src_port;
	ncfrm->dst_port = dst_port;
	ncfrm->ip_version = ip_version;
	memcpy(ncfrm->src_ip, src_ip, sizeof(struct in6_addr));
	memcpy(ncfrm->dst_ip, dst_ip, sizeof(struct in6_addr));
	ncfrm->flow_id = flow_id;

	/*
	 * Send flow rule message to NSS core
	 */
	status = nss_capwapmgr_tx_msg_sync(priv->nss_ctx, dev, &capwapmsg);
	if (status != NSS_CAPWAPMGR_SUCCESS) {
		nss_capwapmgr_warn("%px: send flow rule message failed with error: %d\n", dev, status);
	}

done:
	dev_put(dev);
	return status;
}

/*
 * nss_capwapmgr_netdev_up()
 *	NSS CAPWAP Tunnel device i/f up handler
 */
static int nss_capwapmgr_netdev_up(struct net_device *netdev)
{
	uint8_t i;
	for (i = 0; i < NSS_CAPWAPMGR_MAX_TUNNELS; i++) {
		(void)nss_capwapmgr_enable_tunnel(nss_capwapmgr_ndev, i);
	}

	return NOTIFY_DONE;
}

/*
 * nss_capwapmgr_netdev_down()
 *	NSS CAPWAP Tunnel device i/f up handler
 */
static int nss_capwapmgr_netdev_down(struct net_device *netdev)
{
	uint8_t i;
	for (i = 0; i < NSS_CAPWAPMGR_MAX_TUNNELS; i++) {
		(void)nss_capwapmgr_disable_tunnel(nss_capwapmgr_ndev, i);
	}

	return NOTIFY_DONE;
}

/*
 * nss_capwapmgr_netdev_event()
 *	Net device notifier for NSS CAPWAP manager module
 */
static int nss_capwapmgr_netdev_event(struct notifier_block *nb, unsigned long event, void *dev)
{
	struct net_device *netdev = (struct net_device *)dev;

	if (strstr(netdev->name, NSS_CAPWAPMGR_NETDEV_NAME) == NULL) {
		return NOTIFY_DONE;
	}

	switch (event) {
	case NETDEV_UP:
		nss_capwapmgr_trace("%px: NETDEV_UP: event %lu name %s\n", netdev, event, netdev->name);
		return nss_capwapmgr_netdev_up(netdev);

	case NETDEV_DOWN:
		nss_capwapmgr_trace("%px: NETDEV_DOWN: event %lu name %s\n", netdev, event, netdev->name);
		return nss_capwapmgr_netdev_down(netdev);

	default:
		nss_capwapmgr_trace("%px: Unhandled notifier event %lu name %s\n", netdev, event, netdev->name);
		break;
	}

	return NOTIFY_DONE;
}

/*
 * Linux Net device Notifier
 */
struct notifier_block nss_capwapmgr_netdev_notifier = {
	.notifier_call = nss_capwapmgr_netdev_event,
};

/*
 * nss_capwapmgr_netdev_create()
 *	API to create a CAPWAP netdev
 */
struct net_device *nss_capwapmgr_netdev_create()
{
	struct nss_capwapmgr_priv *priv;
	struct nss_capwapmgr_response *r;
	struct net_device *ndev;
	int i;
	int err;

	ndev = alloc_netdev(sizeof(struct nss_capwapmgr_priv),
					"nsscapwap%d", NET_NAME_ENUM, nss_capwapmgr_dummy_netdev_setup);
	if (!ndev) {
		nss_capwapmgr_warn("Error allocating netdev\n");
		return NULL;
	}

	err = register_netdev(ndev);
	if (err) {
		nss_capwapmgr_warn("register_netdev() fail with error :%d\n", err);
		free_netdev(ndev);
		return NULL;
	}

	priv = netdev_priv(ndev);
	priv->nss_ctx = nss_capwap_get_ctx();
	priv->tunnel = kmalloc(sizeof(struct nss_capwapmgr_tunnel) * NSS_CAPWAPMGR_MAX_TUNNELS, GFP_ATOMIC);
	if (!priv->tunnel) {
		nss_capwapmgr_warn("%px: failed to allocate tunnel memory\n", ndev);
		goto fail1;
	}
	memset(priv->tunnel, 0, sizeof(struct nss_capwapmgr_tunnel) * NSS_CAPWAPMGR_MAX_TUNNELS);
	for (i = 0; i < NSS_CAPWAPMGR_MAX_TUNNELS; i++) {
		priv->tunnel[i].if_num_inner = -1;
		priv->tunnel[i].if_num_outer = -1;
	}

	priv->resp = kmalloc(sizeof(struct nss_capwapmgr_response) * NSS_MAX_DYNAMIC_INTERFACES, GFP_ATOMIC);
	if (!priv->resp) {
		nss_capwapmgr_warn("%px: failed to allocate tunnel response memory\n", ndev);
		goto fail2;
	}
	for (i = 0; i < NSS_MAX_DYNAMIC_INTERFACES; i++) {
		r = &priv->resp[i];
		init_waitqueue_head(&r->wq);

		/*
		 * CAPWAP interface is limited to one command per-interface.
		 */
		sema_init(&r->sem, 1);
	}

	priv->if_num_to_tunnel_id = kmalloc(sizeof(uint8_t) * NSS_MAX_DYNAMIC_INTERFACES, GFP_ATOMIC);
	if (!priv->if_num_to_tunnel_id) {
		nss_capwapmgr_warn("%px: failed to allocate if_num to tunnel_id memory\n", ndev);
		goto fail3;
	}
	memset(priv->if_num_to_tunnel_id, 0, sizeof(uint8_t) * NSS_MAX_DYNAMIC_INTERFACES);

	if (nss_cmn_register_queue_decongestion(priv->nss_ctx, nss_capwapmgr_decongestion_callback, ndev) != NSS_CB_REGISTER_SUCCESS) {
		nss_capwapmgr_warn("%px: failed to register decongestion callback\n", ndev);
		goto fail4;
	}

	return ndev;

fail4:
	kfree(priv->if_num_to_tunnel_id);
fail3:
	kfree(priv->resp);
fail2:
	kfree(priv->tunnel);
fail1:
	unregister_netdev(ndev);
	free_netdev(ndev);
	return NULL;
}
EXPORT_SYMBOL(nss_capwapmgr_netdev_create);

/*
 * nss_capwapmgr_netdev_destroy()
 *	API for destroying a netdevice.
 *
 * All the CAPWAP tunnels must be destroyed first before netdevice.
 */
nss_capwapmgr_status_t nss_capwapmgr_netdev_destroy(struct net_device *dev)
{
	rtnl_is_locked() ? unregister_netdevice(dev) : unregister_netdev(dev);
	return NSS_CAPWAPMGR_SUCCESS;
}
EXPORT_SYMBOL(nss_capwapmgr_netdev_destroy);

/*
 * nss_capwapmgr_update_path_mtu()
 *	API for updating Path MTU
 */
nss_capwapmgr_status_t nss_capwapmgr_update_path_mtu(struct net_device *dev, uint8_t tunnel_id, uint32_t mtu)
{
	struct nss_capwapmgr_priv *priv;
	struct nss_capwap_msg capwapmsg;
	struct nss_capwapmgr_tunnel *t = NULL;
	nss_capwapmgr_status_t status;
	ppe_vp_status_t ppe_vp_status;
	struct nss_ipv4_create *v4;
	struct nss_ipv6_create *v6;

	if (mtu > NSS_CAPWAP_MAX_MTU) {
		nss_capwapmgr_warn("%px: invalid path_mtu: %d, max: %d\n", dev, mtu, NSS_CAPWAP_MAX_MTU);
		return NSS_CAPWAPMGR_FAILURE_INVALID_PATH_MTU;
	}

	dev_hold(dev);
	status = nss_capwapmgr_get_tunnel(dev, tunnel_id, &t);
	if (status != NSS_CAPWAPMGR_SUCCESS) {
		nss_capwapmgr_warn("%px: can't find tunnel: %d\n", dev, tunnel_id);
		goto done;
	}

	priv = netdev_priv(dev);
	nss_capwapmgr_info("%px: %d: tunnel update MTU is being called\n", dev, t->if_num_inner);

	/*
	 * Prepare the tunnel configuration parameter to send to NSS FW
	 */
	memset(&capwapmsg, 0, sizeof(struct nss_capwap_msg));

	/*
	 * Send CAPWAP data tunnel command to NSS
	 */
	nss_capwap_msg_init(&capwapmsg, t->if_num_inner, NSS_CAPWAP_MSG_TYPE_UPDATE_PATH_MTU,
		sizeof(struct nss_capwap_path_mtu_msg), nss_capwapmgr_msg_event_receive, dev);
	capwapmsg.msg.mtu.path_mtu = htonl(mtu);
	status = nss_capwapmgr_tx_msg_sync(priv->nss_ctx, dev, &capwapmsg);
	if (status != NSS_CAPWAPMGR_SUCCESS) {
		nss_capwapmgr_warn("%px: Update Path MTU CAPWAP tunnel error : %d \n", dev, status);
		status = NSS_CAPWAPMGR_FAILURE_CAPWAP_RULE;
		goto done;
	}

	/*
	 * Update the MTU for the CAPWAP VP.
	 */
	ppe_vp_status = ppe_vp_mtu_set(t->vp_num, mtu);
	if (ppe_vp_status != PPE_VP_STATUS_SUCCESS) {
		nss_capwapmgr_warn("%px: PPE_VP mtu set failed %d\n", dev, ppe_vp_status);
		status = NSS_CAPWAPMGR_FAILURE_VP_MTU_SET;
		goto fail;
	}

	/*
	 * Delete and re-create the IPv4/IPv6 rule with the new MTU for flow and return
	 */
	if (t->tunnel_state & NSS_CAPWAPMGR_TUNNEL_STATE_IPRULE_CONFIGURED) {
		if (t->capwap_rule.l3_proto == NSS_CAPWAP_TUNNEL_IPV4) {
			status = nss_capwapmgr_ppe_destroy_ipv4_rule(t);
			if (status != NSS_CAPWAPMGR_SUCCESS) {
				goto fail1;
			}
		} else {
			status = nss_capwapmgr_ppe_destroy_ipv6_rule(t);
			if (status != NSS_CAPWAPMGR_SUCCESS) {
				goto fail1;
			}
		}
	}

	if (t->capwap_rule.l3_proto == NSS_CAPWAP_TUNNEL_IPV4) {
		v4 = &t->ip_rule.v4;
		v4->from_mtu = v4->to_mtu = mtu;
		status = nss_capwapmgr_ppe_create_ipv4_rule(t, v4);
		if (status != NSS_CAPWAPMGR_SUCCESS) {
			v4->from_mtu = v4->to_mtu = ntohl(t->capwap_rule.encap.path_mtu);
			nss_capwapmgr_warn("%px: Configure ipv4 rule failed : %d\n", dev, status);
			goto fail1;
		}
	} else {
		v6 = &t->ip_rule.v6;
		v6->from_mtu = v6->to_mtu = mtu;
		status = nss_capwapmgr_ppe_create_ipv6_rule(t, v6);
		if (status != NSS_CAPWAPMGR_SUCCESS) {
			v6->from_mtu = v6->to_mtu = ntohl(t->capwap_rule.encap.path_mtu);
			nss_capwapmgr_warn("%px: Configure ipv6 rule failed : %d\n", dev, status);
			goto fail1;
		}
	}

	t->capwap_rule.encap.path_mtu = htonl(mtu);
	goto done;

fail1:
	ppe_vp_status = ppe_vp_mtu_set(t->vp_num, t->capwap_rule.encap.path_mtu);
	if (ppe_vp_status != PPE_VP_STATUS_SUCCESS) {
		nss_capwapmgr_warn("%px: Restore PPE_VP mtu failed %d\n", dev, ppe_vp_status);
	}

fail:
	nss_capwapmgr_warn("%px: Update Path MTU IP RULE tunnel error : %d \n", dev, status);
	capwapmsg.msg.mtu.path_mtu = t->capwap_rule.encap.path_mtu;
	if (nss_capwapmgr_tx_msg_sync(priv->nss_ctx, dev, &capwapmsg) != NSS_CAPWAPMGR_SUCCESS) {
		nss_capwapmgr_warn("%px: Restore Path MTU CAPWAP tunnel error : %d \n", dev, status);
	}

done:
	dev_put(dev);
	return status;
}
EXPORT_SYMBOL(nss_capwapmgr_update_path_mtu);

/*
 * nss_capwapmgr_update_dest_mac_addr()
 *	API for updating Destination Mac Addr
 */
nss_capwapmgr_status_t nss_capwapmgr_update_dest_mac_addr(struct net_device *dev, uint8_t tunnel_id, uint8_t *mac_addr)
{
	struct nss_capwapmgr_priv *priv;
	struct nss_capwapmgr_tunnel *t = NULL;
	nss_capwapmgr_status_t status = NSS_CAPWAPMGR_SUCCESS;
	struct nss_ipv4_create *v4;
	struct nss_ipv6_create *v6;
	uint8_t mac_addr_old[ETH_ALEN];

	dev_hold(dev);
	status = nss_capwapmgr_get_tunnel(dev, tunnel_id, &t);
	if (status != NSS_CAPWAPMGR_SUCCESS) {
		nss_capwapmgr_warn("%px: can't find tunnel: %d\n", dev, tunnel_id);
		goto done;
	}

	priv = netdev_priv(dev);
	nss_capwapmgr_info("%px: %d: tunnel update mac Addr is being called\n", dev, tunnel_id);

	/*
	 * Delete and re-create the IPv4/IPv6 rule with the new destination mac address for flow and return.
	 * Since the encap direction is handled by the return rule, we are updating the src_mac.
	 */
	if (t->tunnel_state & NSS_CAPWAPMGR_TUNNEL_STATE_IPRULE_CONFIGURED) {
		if (t->capwap_rule.l3_proto == NSS_CAPWAP_TUNNEL_IPV4) {
			status = nss_capwapmgr_ppe_destroy_ipv4_rule(t);
			if (status != NSS_CAPWAPMGR_SUCCESS) {
				goto done;
			}
		} else {
			status = nss_capwapmgr_ppe_destroy_ipv6_rule(t);
			if (status != NSS_CAPWAPMGR_SUCCESS) {
				goto done;
			}
		}
	}

	if (t->capwap_rule.l3_proto == NSS_CAPWAP_TUNNEL_IPV4) {
		v4 = &t->ip_rule.v4;
		memcpy(mac_addr_old, v4->src_mac, ETH_ALEN);
		memcpy(v4->src_mac, mac_addr, ETH_ALEN);
		status = nss_capwapmgr_ppe_create_ipv4_rule(t, v4);
		if (status != NSS_CAPWAPMGR_SUCCESS) {
			nss_capwapmgr_warn("%px: Update Destination Mac for tunnel error : %d \n", dev, status);
			memcpy(t->ip_rule.v4.src_mac, mac_addr_old, ETH_ALEN);
			goto done;
		}
	} else {
		v6 = &t->ip_rule.v6;
		memcpy(mac_addr_old, v6->src_mac, ETH_ALEN);
		memcpy(v6->src_mac, mac_addr, ETH_ALEN);
		status = nss_capwapmgr_ppe_create_ipv6_rule(t, &t->ip_rule.v6);
		if (status != NSS_CAPWAPMGR_SUCCESS) {
			nss_capwapmgr_warn("%px: Update Destination Mac for tunnel error : %d \n", dev, status);
			memcpy(t->ip_rule.v6.src_mac, mac_addr_old, ETH_ALEN);
		}
	}

done:
	dev_put(dev);
	return status;
}
EXPORT_SYMBOL(nss_capwapmgr_update_dest_mac_addr);

/*
 * nss_capwapmgr_update_src_interface()
 *	API for updating Source Interface
 */
nss_capwapmgr_status_t nss_capwapmgr_update_src_interface(struct net_device *dev, uint8_t tunnel_id, int32_t src_interface_num)
{
	struct nss_capwapmgr_priv *priv;
	struct nss_capwapmgr_tunnel *t = NULL;
	nss_capwapmgr_status_t status;
	int32_t src_interface_num_temp;

	dev_hold(dev);
	status = nss_capwapmgr_get_tunnel(dev, tunnel_id, &t);
	if (status != NSS_CAPWAPMGR_SUCCESS) {
		nss_capwapmgr_warn("%px: can't find tunnel: %d\n", dev, tunnel_id);
		goto done;
	}

	priv = netdev_priv(dev);
	nss_capwapmgr_info("%px: %d: tunnel update source interface is being called\n", dev, tunnel_id);

	/*
	 * Destroy/Re-Create the IPv4/IPv6 rule with the new Interface number for flow and return
	 */
	if (t->tunnel_state & NSS_CAPWAPMGR_TUNNEL_STATE_IPRULE_CONFIGURED) {
		if (t->capwap_rule.l3_proto == NSS_CAPWAP_TUNNEL_IPV4) {
			status = nss_capwapmgr_ppe_destroy_ipv4_rule(t);
			if (status != NSS_CAPWAPMGR_SUCCESS) {
				goto done;
			}
		} else {
			status = nss_capwapmgr_ppe_destroy_ipv6_rule(t);
			if (status != NSS_CAPWAPMGR_SUCCESS) {
				goto done;
			}
		}
	}

	if (t->capwap_rule.l3_proto == NSS_CAPWAP_TUNNEL_IPV4) {
		src_interface_num_temp = t->ip_rule.v4.src_interface_num;
		t->ip_rule.v4.src_interface_num = src_interface_num;
		status = nss_capwapmgr_ppe_create_ipv4_rule(t, &t->ip_rule.v4);
		if (status != NSS_CAPWAPMGR_SUCCESS) {
			nss_capwapmgr_warn("%px: unconfigure ipv4 rule failed : %d\n", dev, status);
			t->ip_rule.v4.src_interface_num = src_interface_num_temp;
			goto done;
		}
	} else {
		src_interface_num_temp = t->ip_rule.v6.src_interface_num;
		t->ip_rule.v6.src_interface_num = src_interface_num;
		status = nss_capwapmgr_ppe_create_ipv6_rule(t, &t->ip_rule.v6);
		if (status != NSS_CAPWAPMGR_SUCCESS) {
			nss_capwapmgr_warn("%px: configure ipv6 rule failed : %d\n", dev, status);
			t->ip_rule.v6.src_interface_num = src_interface_num_temp;
		}
	}

done:
	dev_put(dev);
	return status;
}
EXPORT_SYMBOL(nss_capwapmgr_update_src_interface);

/*
 * nss_capwapmgr_change_version()
 *	Change CAPWAP version
 */
nss_capwapmgr_status_t nss_capwapmgr_change_version(struct net_device *dev, uint8_t tunnel_id, uint8_t ver)
{
	struct nss_capwapmgr_priv *priv;
	struct nss_capwap_msg capwapmsg;
	struct nss_capwapmgr_tunnel *t = NULL;
	nss_capwapmgr_status_t status = NSS_CAPWAPMGR_SUCCESS;

	dev_hold(dev);
	status = nss_capwapmgr_get_tunnel(dev, tunnel_id, &t);
	if (status != NSS_CAPWAPMGR_SUCCESS) {
		nss_capwapmgr_warn("%px: can't find tunnel: %d\n", dev, tunnel_id);
		goto done;
	}

	if (ver > NSS_CAPWAP_VERSION_V2) {
		nss_capwapmgr_warn("%px: un-supported Version: %d\n", dev, ver);
		status = NSS_CAPWAPMGR_FAILURE_INVALID_VERSION;
		goto done;
	}

	priv = netdev_priv(dev);

	/*
	 * Prepare the tunnel configuration parameter to send to NSS FW
	 */
	memset(&capwapmsg, 0, sizeof(struct nss_capwap_msg));

	/*
	 * Send CAPWAP data tunnel command to NSS
	 */
	nss_capwap_msg_init(&capwapmsg, t->if_num_inner, NSS_CAPWAP_MSG_TYPE_VERSION,
		sizeof(struct nss_capwap_version_msg), nss_capwapmgr_msg_event_receive, dev);
	capwapmsg.msg.version.version = ver;
	status = nss_capwapmgr_tx_msg_sync(priv->nss_ctx, dev, &capwapmsg);
	if (status != NSS_CAPWAPMGR_SUCCESS) {
		nss_capwapmgr_warn("%px: Update Path MTU Tunnel error : %d \n", dev, status);
	}

done:
	dev_put(dev);
	return status;
}
EXPORT_SYMBOL(nss_capwapmgr_change_version);

/*
 * nss_capwapmgr_enable_tunnel()
 *	API for enabling a data tunnel
 */
nss_capwapmgr_status_t nss_capwapmgr_enable_tunnel(struct net_device *dev, uint8_t tunnel_id)
{
	struct nss_capwapmgr_priv *priv;
	struct nss_capwapmgr_tunnel *t = NULL;
	nss_capwapmgr_status_t status = NSS_CAPWAPMGR_SUCCESS;

	dev_hold(dev);
	status = nss_capwapmgr_get_tunnel(dev, tunnel_id, &t);
	if (status != NSS_CAPWAPMGR_SUCCESS) {
		nss_capwapmgr_warn("%px: can't find tunnel: %d\n", dev, tunnel_id);
		goto done;
	}

	if (t->tunnel_state & NSS_CAPWAPMGR_TUNNEL_STATE_ENABLED) {
		nss_capwapmgr_warn("%px: tunnel %d is already enabled\n", dev, tunnel_id);
		status = NSS_CAPWAPMGR_FAILURE_TUNNEL_ENABLED;
		goto done;
	}

	priv = netdev_priv(dev);
	nss_capwapmgr_info("%px: Inner:%d Outer:%d. Tunnel enable is being called\n", dev, t->if_num_inner, t->if_num_outer);

	status = nss_capwapmgr_tx_msg_enable_tunnel(priv->nss_ctx, dev, t->if_num_inner,t->if_num_outer);
	if (status != NSS_CAPWAPMGR_SUCCESS) {
		goto done;
	}

	status = nss_capwapmgr_tx_msg_enable_tunnel(priv->nss_ctx, dev, t->if_num_outer,t->if_num_inner);
	if(status != NSS_CAPWAPMGR_SUCCESS) {
		nss_capwapmgr_tunnel_action(priv->nss_ctx, dev, t->if_num_inner,NSS_CAPWAP_MSG_TYPE_DISABLE_TUNNEL);
		goto done;
	}

	t->tunnel_state |= NSS_CAPWAPMGR_TUNNEL_STATE_ENABLED;

done:
	dev_put(dev);
	return status;
}
EXPORT_SYMBOL(nss_capwapmgr_enable_tunnel);

/*
 * nss_capwapmgr_disable_tunnel()
 *	API for disabling a data tunnel
 */
nss_capwapmgr_status_t nss_capwapmgr_disable_tunnel(struct net_device *dev, uint8_t tunnel_id)
{
	struct nss_capwapmgr_priv *priv;
	struct nss_capwapmgr_tunnel *t = NULL;
	nss_capwapmgr_status_t status = NSS_CAPWAPMGR_SUCCESS;

	dev_hold(dev);
	status = nss_capwapmgr_get_tunnel(dev, tunnel_id, &t);
	if (status != NSS_CAPWAPMGR_SUCCESS) {
		nss_capwapmgr_warn("%px: can't find tunnel: %d\n", dev, tunnel_id);
		goto done;
	}

	if (!(t->tunnel_state & NSS_CAPWAPMGR_TUNNEL_STATE_ENABLED)) {
		nss_capwapmgr_warn("%px: tunnel %d is already disabled\n", dev, tunnel_id);
		status = NSS_CAPWAPMGR_FAILURE_TUNNEL_DISABLED;
		goto done;
	}

	priv = netdev_priv(dev);
	nss_capwapmgr_info("%px: Inner:%d Outer:%d. Tunnel disable is being called\n", dev, t->if_num_inner, t->if_num_outer);

	status = nss_capwapmgr_tunnel_action(priv->nss_ctx, dev, t->if_num_inner,NSS_CAPWAP_MSG_TYPE_DISABLE_TUNNEL);
	if (status != NSS_CAPWAPMGR_SUCCESS) {
		status = NSS_CAPWAPMGR_FAILURE_TUNNEL_DISABLED;
		nss_capwapmgr_warn("%px: tunnel %d disable failed\n", dev, tunnel_id);
		goto done;
	}

	status = nss_capwapmgr_tunnel_action(priv->nss_ctx, dev, t->if_num_outer,NSS_CAPWAP_MSG_TYPE_DISABLE_TUNNEL);
	if (status != NSS_CAPWAPMGR_SUCCESS) {
		nss_capwapmgr_warn("%px: tunnel %d disable failed\n", dev, tunnel_id);
		nss_capwapmgr_tx_msg_enable_tunnel(priv->nss_ctx, dev, t->if_num_inner, t->if_num_outer);
		goto done;
	}

	t->tunnel_state &= ~NSS_CAPWAPMGR_TUNNEL_STATE_ENABLED;

done:
	dev_put(dev);
	return status;
}
EXPORT_SYMBOL(nss_capwapmgr_disable_tunnel);

/*
 * nss_capwapmgr_ipv4_tunnel_create()
 *	API for creating IPv4 and CAPWAP rule.
 */
nss_capwapmgr_status_t nss_capwapmgr_ipv4_tunnel_create(struct net_device *dev, uint8_t tunnel_id,
			struct nss_ipv4_create *ip_rule, struct nss_capwap_rule_msg *capwap_rule)
{
	return nss_capwapmgr_tunnel_create_common(dev, tunnel_id, ip_rule, NULL, capwap_rule);
}
EXPORT_SYMBOL(nss_capwapmgr_ipv4_tunnel_create);

/*
 * nss_capwapmgr_ipv6_tunnel_create()
 *	API for creating IPv6 and CAPWAP rule.
 */
nss_capwapmgr_status_t nss_capwapmgr_ipv6_tunnel_create(struct net_device *dev, uint8_t tunnel_id,
			struct nss_ipv6_create *ip_rule, struct nss_capwap_rule_msg *capwap_rule)
{
	return nss_capwapmgr_tunnel_create_common(dev, tunnel_id, NULL, ip_rule, capwap_rule);
}
EXPORT_SYMBOL(nss_capwapmgr_ipv6_tunnel_create);

/*
 * nss_capwapmgr_tunnel_destroy()
 *	API for destroying a tunnel. CAPWAP tunnel must be first disabled.
 */
nss_capwapmgr_status_t nss_capwapmgr_tunnel_destroy(struct net_device *dev, uint8_t tunnel_id)
{
	struct nss_capwap_tunnel_stats stats;
	struct nss_capwapmgr_priv *priv;
	struct nss_capwapmgr_tunnel *t = NULL;
	nss_tx_status_t nss_status = NSS_TX_SUCCESS;
	uint32_t if_num_inner, if_num_outer;
	nss_capwapmgr_status_t status = NSS_CAPWAPMGR_SUCCESS;
	ppe_vp_status_t vp_status;

	dev_hold(dev);
	status = nss_capwapmgr_get_tunnel(dev, tunnel_id, &t);
	if (status != NSS_CAPWAPMGR_SUCCESS) {
		nss_capwapmgr_warn("%px: can't find tunnel: %d\n", dev, tunnel_id);
		goto done;
	}

	if (!(t->tunnel_state & NSS_CAPWAPMGR_TUNNEL_STATE_CONFIGURED)) {
		nss_capwapmgr_warn("%px: tunnel %d is not configured yet\n", dev, tunnel_id);
		status = NSS_CAPWAPMGR_FAILURE_TUNNEL_NOT_CFG;
		goto done;
	}

	/*
	 * We don't allow destroy operation on tunnel if it's still enabled.
	 */
	if (t->tunnel_state & NSS_CAPWAPMGR_TUNNEL_STATE_ENABLED) {
		nss_capwapmgr_warn("%px: no destroy allowed for an enabled tunnel: %d\n", dev, tunnel_id);
		status = NSS_CAPWAPMGR_FAILURE_TUNNEL_ENABLED;
		goto done;
	}

	if (!(t->capwap_rule.l3_proto == NSS_CAPWAP_TUNNEL_IPV4 ||
			t->capwap_rule.l3_proto == NSS_CAPWAP_TUNNEL_IPV6)) {
		nss_capwapmgr_warn("%px: tunnel %d: wrong argument for l3_proto\n", dev, tunnel_id);
		status = NSS_CAPWAPMGR_FAILURE_INVALID_L3_PROTO;
		goto done;
	}

	if (!(t->capwap_rule.which_udp == NSS_CAPWAP_TUNNEL_UDP ||
			t->capwap_rule.which_udp == NSS_CAPWAP_TUNNEL_UDPLite)) {
		nss_capwapmgr_warn("%px: tunnel %d: wrong argument for which_udp(%d)\n", dev, tunnel_id, t->capwap_rule.which_udp);
		status = NSS_CAPWAPMGR_FAILURE_INVALID_UDP_PROTO;
		goto done;
	}

	priv = netdev_priv(dev);
	nss_capwapmgr_info("%px: %d: tunnel destroy is being called\n", dev, tunnel_id);

	if_num_inner = t->if_num_inner;
	if_num_outer = t->if_num_outer;

	if (priv->if_num_to_tunnel_id[if_num_inner] != tunnel_id) {
		nss_capwapmgr_warn("%px: %d: tunnel_id %d didn't match with tunnel_id :%d\n",
			dev, if_num_inner, tunnel_id, priv->if_num_to_tunnel_id[if_num_inner]);
		status = NSS_CAPWAPMGR_FAILURE_BAD_PARAM;
		goto done;
	}

	if (priv->if_num_to_tunnel_id[if_num_outer] != tunnel_id) {
		nss_capwapmgr_warn("%px: %d: tunnel_id %d didn't match with tunnel_id :%d\n",
			dev, if_num_outer, tunnel_id, priv->if_num_to_tunnel_id[if_num_outer]);
		status = NSS_CAPWAPMGR_FAILURE_BAD_PARAM;
		goto done;
	}

	if (nss_capwap_get_stats(if_num_inner, &stats)) {
		nss_capwapmgr_tunnel_save_stats(&global.tunneld_stats, &stats);
	}

	if (nss_capwap_get_stats(if_num_outer, &stats)) {
		nss_capwapmgr_tunnel_save_stats(&global.tunneld_stats, &stats);
	}

	/*
	 * Destroy IP rule first.
	 */
	if (t->tunnel_state & NSS_CAPWAPMGR_TUNNEL_STATE_IPRULE_CONFIGURED) {
		if (t->capwap_rule.l3_proto == NSS_CAPWAP_TUNNEL_IPV4) {
			status = nss_capwapmgr_ppe_destroy_ipv4_rule(t);
			if (status != NSS_CAPWAPMGR_SUCCESS) {
				goto done;
			}
		} else {
			status = nss_capwapmgr_ppe_destroy_ipv6_rule(t);
			if (status != NSS_CAPWAPMGR_SUCCESS) {
				goto done;
			}
		}
	}

	/*
	 * Destroy CAPWAP rule now.
	 */
	status = nss_capwapmgr_tunnel_action(priv->nss_ctx, dev, if_num_outer, NSS_CAPWAP_MSG_TYPE_UNCFG_RULE);
	if (status != NSS_CAPWAPMGR_SUCCESS) {
		nss_capwapmgr_warn("%px: %d: Unconfigure Inner CAPWAP rule failed for tunnel : %d\n",
			dev, if_num_outer, tunnel_id);
	}

	status = nss_capwapmgr_tunnel_action(priv->nss_ctx, dev, if_num_inner, NSS_CAPWAP_MSG_TYPE_UNCFG_RULE);
	if (status != NSS_CAPWAPMGR_SUCCESS) {
		nss_capwapmgr_warn("%px: %d: Unconfigure Outer CAPWAP rule failed for tunnel : %d\n",
			dev, if_num_inner, tunnel_id);
	}

	nss_capwapmgr_unregister_with_nss(if_num_outer);
	nss_capwapmgr_unregister_with_nss(if_num_inner);

	/*
	 * Deallocate dynamic interface
	 */
	if ((t->tunnel_state & NSS_CAPWAPMGR_TUNNEL_STATE_OUTER_ALLOCATED)) {
		nss_status = nss_dynamic_interface_dealloc_node(if_num_outer, NSS_DYNAMIC_INTERFACE_TYPE_CAPWAP_OUTER);
		if (nss_status != NSS_TX_SUCCESS) {
			nss_capwapmgr_warn("%px: %d: Dealloc of dynamic interface failed for tunnel : %d\n",
				dev, if_num_outer, tunnel_id);
		}

		t->tunnel_state &= ~NSS_CAPWAPMGR_TUNNEL_STATE_OUTER_ALLOCATED;
	}

	if ((t->tunnel_state & NSS_CAPWAPMGR_TUNNEL_STATE_INNER_ALLOCATED)) {
		nss_status = nss_dynamic_interface_dealloc_node(if_num_inner, NSS_DYNAMIC_INTERFACE_TYPE_CAPWAP_HOST_INNER);
		if (nss_status != NSS_TX_SUCCESS) {
			nss_capwapmgr_warn("%px: %d: Dealloc of dynamic interface failed for tunnel : %d\n",
				dev, if_num_inner, tunnel_id);
		}

		t->tunnel_state &= ~NSS_CAPWAPMGR_TUNNEL_STATE_INNER_ALLOCATED;
	}

	/*
	 * Free the VP interface associated with the tunnel.
	 */
	vp_status = ppe_vp_free(t->vp_num);
	if (vp_status!= PPE_VP_STATUS_SUCCESS) {
		nss_capwapmgr_warn("%px: VP Number %d: Failed to free the associated VP for tunnel : %d\n",
			dev, t->vp_num, tunnel_id);
		status = NSS_CAPWAPMGR_FAILURE_VP_FREE;
		goto done;
	}

	/*
	 * Update the state flag to say the tunnel is unconfigured.
	 */
	t->tunnel_state &= ~NSS_CAPWAPMGR_TUNNEL_STATE_CONFIGURED;

	/*
	 * Free the internal net device associated with the tunnel.
	 */
	free_netdev(t->internal_dev);

	priv->if_num_to_tunnel_id[if_num_inner] = -1;
	priv->if_num_to_tunnel_id[if_num_outer] = -1;

	memset(t, 0, sizeof(struct nss_capwapmgr_tunnel));

	t->if_num_inner = -1;
	t->if_num_outer = -1;

	nss_capwapmgr_info("%px: Tunnel %d is destroyed\n", dev , tunnel_id);
	status = NSS_CAPWAPMGR_SUCCESS;

done:
	dev_put(dev);
	return status;
}
EXPORT_SYMBOL(nss_capwapmgr_tunnel_destroy);

/*
 * nss_capwapmgr_add_flow_rule()
 *	Send a capwap flow rule add message to NSS core.
 */
nss_capwapmgr_status_t nss_capwapmgr_add_flow_rule(struct net_device *dev, uint8_t tunnel_id, uint16_t ip_version,
						uint16_t protocol, uint32_t *src_ip, uint32_t *dst_ip,
						uint16_t src_port, uint16_t dst_port, uint32_t flow_id)
{
	return nss_capwapmgr_flow_rule_action(dev, tunnel_id, NSS_CAPWAP_MSG_TYPE_FLOW_RULE_ADD, ip_version,
											protocol, src_ip, dst_ip, src_port, dst_port, flow_id);
}
EXPORT_SYMBOL(nss_capwapmgr_add_flow_rule);

/*
 * nss_capwapmgr_del_flow_rule()
 *	Send a capwap flow rule del message to NSS core.
 */
nss_capwapmgr_status_t nss_capwapmgr_del_flow_rule(struct net_device *dev, uint8_t tunnel_id, uint16_t ip_version,
						uint16_t protocol, uint32_t *src_ip, uint32_t *dst_ip,
						uint16_t src_port, uint16_t dst_port)
{
	return nss_capwapmgr_flow_rule_action(dev, tunnel_id, NSS_CAPWAP_MSG_TYPE_FLOW_RULE_DEL, ip_version,
											protocol, src_ip, dst_ip, src_port, dst_port, 0);
}
EXPORT_SYMBOL(nss_capwapmgr_del_flow_rule);

/*
 * nss_capwapmgr_tunnel_stats()
 *	Gets tunnel stats from netdev
 */
nss_capwapmgr_status_t nss_capwapmgr_tunnel_stats(struct net_device *dev,
		uint8_t tunnel_id, struct nss_capwap_tunnel_stats *stats)
{
	struct nss_capwapmgr_tunnel *t = NULL;
	struct nss_capwap_tunnel_stats stats_temp;
	nss_capwapmgr_status_t status = NSS_CAPWAPMGR_SUCCESS;

	if (!stats) {
		nss_capwapmgr_warn("%px: invalid rtnl structure\n", dev);
		return NSS_CAPWAPMGR_FAILURE_BAD_PARAM;
	}

	dev_hold(dev);
	status = nss_capwapmgr_get_tunnel(dev, tunnel_id, &t);
	if (status != NSS_CAPWAPMGR_SUCCESS) {
		nss_capwapmgr_warn("%px: can't find tunnel: %d\n", dev, tunnel_id);
		goto done;
	}

	if (!(t->tunnel_state & NSS_CAPWAPMGR_TUNNEL_STATE_CONFIGURED)) {
		nss_capwapmgr_trace("%px: tunnel: %d not configured yet\n", dev, tunnel_id);
		status = NSS_CAPWAPMGR_FAILURE_TUNNEL_NOT_CFG;
		goto done;
	}

	/*
	 * Copy the inner interface stats.
	 */
	if (nss_capwap_get_stats(t->if_num_inner, &stats_temp) == false) {
		nss_capwapmgr_warn("%px: tunnel %d not ready yet\n", dev, tunnel_id);
		status = NSS_CAPWAPMGR_FAILURE_NOT_READY;
		goto done;
	}

	stats->dtls_pkts += stats_temp.dtls_pkts;
	stats->tx_segments += stats_temp.tx_segments;
	stats->tx_queue_full_drops += stats_temp.tx_queue_full_drops;
	stats->tx_mem_failure_drops += stats_temp.tx_mem_failure_drops;
	stats->tx_dropped_sg_ref += stats_temp.tx_dropped_sg_ref;
	stats->tx_dropped_ver_mis += stats_temp.tx_dropped_ver_mis;
	stats->tx_dropped_hroom += stats_temp.tx_dropped_hroom;
	stats->tx_dropped_dtls += stats_temp.tx_dropped_dtls;
	stats->tx_dropped_nwireless += stats_temp.tx_dropped_nwireless;

	/*
	 * Pnode tx stats for Inner node.
	 */
	stats->pnode_stats.tx_packets += stats_temp.pnode_stats.tx_packets;
	stats->pnode_stats.tx_bytes += stats_temp.pnode_stats.tx_bytes;
	stats->tx_dropped_inner += stats_temp.tx_dropped_inner;

	/*
	 * Copy the outer interface stats.
	 */
	if (nss_capwap_get_stats(t->if_num_outer, &stats_temp) == false) {
		nss_capwapmgr_warn("%px: tunnel %d not ready yet\n", dev, tunnel_id);
		status = NSS_CAPWAPMGR_FAILURE_NOT_READY;
		goto done;
	}

	stats->rx_segments += stats_temp.rx_segments;
	stats->dtls_pkts += stats_temp.dtls_pkts;
	stats->rx_dup_frag += stats_temp.rx_dup_frag;
	stats->rx_oversize_drops += stats_temp.rx_oversize_drops;
	stats->rx_frag_timeout_drops += stats_temp.rx_frag_timeout_drops;
	stats->rx_n2h_drops += stats_temp.rx_n2h_drops;
	stats->rx_n2h_queue_full_drops += stats_temp.rx_n2h_queue_full_drops;
	stats->rx_mem_failure_drops += stats_temp.rx_mem_failure_drops;
	stats->rx_csum_drops += stats_temp.rx_csum_drops;
	stats->rx_malformed += stats_temp.rx_malformed;
	stats->rx_frag_gap_drops += stats_temp.rx_frag_gap_drops;

	/*
	 * Pnode rx stats for outer node.
	 */
	stats->pnode_stats.rx_packets += stats_temp.pnode_stats.rx_packets;
	stats->pnode_stats.rx_bytes += stats_temp.pnode_stats.rx_bytes;
	stats->pnode_stats.rx_dropped += stats_temp.pnode_stats.rx_dropped;

done:
	dev_put(dev);
	return status;
}
EXPORT_SYMBOL(nss_capwapmgr_tunnel_stats);

#if defined(NSS_CAPWAPMGR_ONE_NETDEV)
/*
 * nss_capwapmgr_get_netdev()
 *	Returns net device used.
 */
struct net_device *nss_capwapmgr_get_netdev(void)
{
	return nss_capwapmgr_ndev;
}
EXPORT_SYMBOL(nss_capwapmgr_get_netdev);
#endif

/*
 * nss_capwapmgr_init_module()
 *	Tunnel CAPWAP module init function
 */
int __init nss_capwapmgr_init_module(void)
{
#ifdef CONFIG_OF
	/*
	 * If the node is not compatible, don't do anything.
	 */
	if (!of_find_node_by_name(NULL, "nss-common")) {
		return 0;
	}
#endif
	nss_capwapmgr_info("module (platform - IPQ9574, %s) loaded\n",
			   NSS_PPE_BUILD_ID);

#if defined(NSS_CAPWAPMGR_ONE_NETDEV)
	/*
	 * In this code, we create a single netdev for all the CAPWAP
	 * tunnels.
	 */
	nss_capwapmgr_ndev = nss_capwapmgr_netdev_create();
	if (!nss_capwapmgr_ndev) {
		nss_capwapmgr_warn("Couldn't create capwap interface\n");
		return -1;
	}
#endif

	register_netdevice_notifier(&nss_capwapmgr_netdev_notifier);
	memset(&global.tunneld_stats, 0, sizeof(struct nss_capwap_tunnel_stats));

	return 0;
}

/*
 * nss_capwapmgr_exit_module()
 *	Tunnel CAPWAP module exit function
 */
void __exit nss_capwapmgr_exit_module(void)
{
#if defined(NSS_CAPWAPMGR_ONE_NETDEV)
	struct nss_capwapmgr_priv *priv;
	uint8_t i;
#endif

#ifdef CONFIG_OF
	/*
	 * If the node is not compatible, don't do anything.
	 */
	if (!of_find_node_by_name(NULL, "nss-common")) {
		return;
	}
#endif

#if defined(NSS_CAPWAPMGR_ONE_NETDEV)
	priv = netdev_priv(nss_capwapmgr_ndev);
	for (i = 0; i < NSS_CAPWAPMGR_MAX_TUNNELS; i++) {
		(void) nss_capwapmgr_disable_tunnel(nss_capwapmgr_ndev, i);
		(void) nss_capwapmgr_tunnel_destroy(nss_capwapmgr_ndev, i);
	}
	kfree(priv->if_num_to_tunnel_id);
	kfree(priv->resp);
	kfree(priv->tunnel);
	unregister_netdev(nss_capwapmgr_ndev);
	free_netdev(nss_capwapmgr_ndev);

	nss_capwapmgr_ndev = NULL;
#endif
	unregister_netdevice_notifier(&nss_capwapmgr_netdev_notifier);

	nss_capwapmgr_info("module unloaded\n");
}

module_init(nss_capwapmgr_init_module);
module_exit(nss_capwapmgr_exit_module);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("NSS CAPWAP manager");
