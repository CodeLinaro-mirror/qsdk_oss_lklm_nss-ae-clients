/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/version.h>
#include <net/addrconf.h>
#include <net/dst.h>
#include <net/flow.h>
#include <net/ipv6.h>
#include <net/route.h>
#include <net/vxlan.h>

#include "nss_ppe_vxlanmgr_priv.h"
#include "nss_ppe_vxlanmgr_tun_stats.h"
#include "nss_ppe_tun_drv.h"
#include "ppe_drv_tun_cmn_ctx.h"

/*
 * FIB update event list.
 */
static LIST_HEAD(fib_event_list);
static DEFINE_SPINLOCK(fib_event_list_lock);

static struct work_struct fib_event_work;			/* Work queue */
static struct workqueue_struct *nss_ppe_vxlanmgr_fib_event_wq;

/*
 * nss_ppe_vxlanmgr_fib_add_event_handler()
 *	Handler for FIB add event from the kernel.
 */
static void nss_ppe_vxlanmgr_fib_add_event_handler(struct nss_ppe_vxlanmgr_fib_event_data *fib_newneigh_info)
{
	/*
	 * TODO: Add support for vxlan-gpe
	 */
	nss_ppe_vxlanmgr_trace("FIB add event handler \n");
	return;
}

/*
 * nss_ppe_vxlanmgr_fib_del_event_handler()
 *	Handler for FIB delete event from the kernel.
 */
static void nss_ppe_vxlanmgr_fib_del_event_handler(struct nss_ppe_vxlanmgr_fib_event_data *fib_delneigh_info)
{
	/*
	 * TODO: Add support for vxlan-gpe
	 */
	nss_ppe_vxlanmgr_trace("FIB delete event handler \n");
	return;
}

/*
 * nss_ppe_vxlanmgr_fib_update_event_handler()
 *	Handler for FIB update event received from kernel.
 */
static void nss_ppe_vxlanmgr_fib_update_event_handler(struct work_struct *neigh_event_work)
{
	struct nss_ppe_vxlanmgr_fib_event_data *fib_event_info;
	uint8_t event;

	/*
	 * Dequeue the work from the head of the list and process it
	 */
	spin_lock_bh(&fib_event_list_lock);

	if (list_empty(&fib_event_list)) {
		spin_unlock_bh(&fib_event_list_lock);
		return;
	}

	fib_event_info = list_first_entry(&fib_event_list, struct nss_ppe_vxlanmgr_fib_event_data, fib_event_list);

	list_del(&fib_event_info->fib_event_list);

	spin_unlock_bh(&fib_event_list_lock);

	event = fib_event_info->event;
	if (event == FIB_EVENT_ENTRY_REPLACE) {
		nss_ppe_vxlanmgr_trace("FIB add/append/replace event received: %X\n", event);
		nss_ppe_vxlanmgr_fib_add_event_handler(fib_event_info);
	} else {
		nss_ppe_vxlanmgr_trace("FIB delete event received: %X\n", event);
		nss_ppe_vxlanmgr_fib_del_event_handler(fib_event_info);
	}

	kfree(fib_event_info);
	queue_work(nss_ppe_vxlanmgr_fib_event_wq, &fib_event_work);
}

/*
 * nss_ppe_vxlanmgr_fib_update_event()
 *	This is a call back for FIB table update event.
 */
static int nss_ppe_vxlanmgr_fib_update_event(struct notifier_block *nb, unsigned long event, void *ptr)
{
	struct nss_ppe_vxlanmgr_fib_event_data *fib_event_data;
	struct fib_notifier_info *info = ptr;
	struct lwtunnel_state *lwtstate;
	struct ip_tunnel_info *tun_info;
	bool restart_work = false;
	struct net_device *dev;
	struct vxlan_dev *priv;
	__be32 vni = 0;

	/*
	 * FIB update event from kernel is same for route add/replace/append which is
	 * of type 'FIB_EVENT_ENTRY_REPLACE'.
	 */
	if (event != FIB_EVENT_ENTRY_REPLACE && event != FIB_EVENT_ENTRY_DEL) {
		nss_ppe_vxlanmgr_warn("%px: Unsupported event type: [%lu] received \n", info, event);
		return NOTIFY_DONE;
	}

	if (info->family == AF_INET) {
		struct fib_entry_notifier_info *fen_info;
		struct fib_nh *nh;

		fen_info = container_of(info, struct fib_entry_notifier_info, info);
		nh = &fen_info->fi->fib_nh[0];
		if (!nh) {
			nss_ppe_vxlanmgr_warn("%px: Next hop entry for IPv4 is NULL \n", info);
			return NOTIFY_DONE;
		}

		nss_ppe_vxlanmgr_trace("IPv4: event for prefix: %pI4, prefix_len: %d \n", &fen_info->dst, fen_info->dst_len);

		dev = nh->fib_nh_dev;
		lwtstate = nh->fib_nh_lws;
		if (!lwtstate) {
			nss_ppe_vxlanmgr_trace("lwt state is NULL for IPv4 \n");
			return NOTIFY_DONE;
		}
	} else if (info->family == AF_INET6) {
		/*
		 * TODO: add support for IPv6
		 */
		nss_ppe_vxlanmgr_warn("IPv6 not supported  \n");
		return NOTIFY_DONE;
	} else {
		nss_ppe_vxlanmgr_warn("Unsupported address family: %X ! \n", info->family);
		return NOTIFY_DONE;
	}

	/*
	 * Check if LW tunnel type is ENCAP IP or IP6, used for VxLAN-GPE encapsulation
	 */
	if (lwtstate->type != LWTUNNEL_ENCAP_IP && lwtstate->type != LWTUNNEL_ENCAP_IP6) {
		nss_ppe_vxlanmgr_trace("%px: Unsupported tunnel encap type: %u", info, lwtstate->type);
		return NOTIFY_DONE;
	}

	/*
	 * Check if VXLAN-GPE net device
	 */
	if (!netif_is_vxlan(dev)) {
		nss_ppe_vxlanmgr_trace("%px: It is not VXLAN netdevice dev:%s", dev, dev->name);
		return NOTIFY_DONE;
	}

	priv = netdev_priv(dev);
	if (!(priv->cfg.flags & VXLAN_F_GPE)) {
		nss_ppe_vxlanmgr_warn("%px: It is not VXLAN-GPE netdevice dev:%s", dev, dev->name);
		return NOTIFY_DONE;
	}

	if (dstport_gpe != ntohs(priv->cfg.dst_port)) {
		nss_ppe_vxlanmgr_trace("%px: VXLAN: configured PPE dport: %u is not-equal to user given dport:%dn", dev, dstport_gpe, ntohs(priv->cfg.dst_port));
		return NOTIFY_DONE;
	}

	fib_event_data = kzalloc(sizeof(struct nss_ppe_vxlanmgr_fib_event_data), GFP_ATOMIC);
	if (!fib_event_data) {
		nss_ppe_vxlanmgr_warn("%px: Alloc failed for fib_event_data", dev);
		return NOTIFY_DONE;
	}

	/*
	 * Fill the vxlan-gpe related event data
	 */
	tun_info = lwt_tun_info(lwtstate);
	vni = tunnel_id_to_key32(tun_info->key.tun_id);
	fib_event_data->vni = vxlan_vni_field(vni);
	fib_event_data->tos = tun_info->key.tos;
	fib_event_data->ttl = tun_info->key.ttl;
	fib_event_data->event = event;
	fib_event_data->tun_flags = tun_info->key.tun_flags;
	fib_event_data->parent_netdev = dev;

	fib_event_data->rip.sa.sa_family = ip_tunnel_info_af(tun_info);
	if (ip_tunnel_info_af(tun_info) == AF_INET) {
		fib_event_data->rip.sin.sin_addr.s_addr = tun_info->key.u.ipv4.dst;
		fib_event_data->sip.sin.sin_addr.s_addr = tun_info->key.u.ipv4.src;
		nss_ppe_vxlanmgr_trace("%px: Local ip: %pI4, Remote_ip: %pI4", tun_info, &tun_info->key.u.ipv4.src, &tun_info->key.u.ipv4.dst);
	} else {
		/*
		 * TODO: add support for IPv6
		 */
		nss_ppe_vxlanmgr_warn("IPv6 remote IP not supported  \n");
		return NOTIFY_DONE;
	}

	nss_ppe_vxlanmgr_trace("%px: VxLAN-GPE - dev_name: %s, vni: %X, tos: %X, ttl: %X, tun_flags:%X \n",
			tun_info, dev->name, fib_event_data->vni, fib_event_data->tos, fib_event_data->ttl, fib_event_data->tun_flags);

	/*
	 * We use workqueue to process the event asynchronously as we will be configuring/deconfiguring
	 * PPE hardware for VxLAN-GPE. It may take some time during this process and we don't want the
	 * kernel process to be blocked here. Also, we may sleep during the event processing.
	 *
	 * The list is empty, so we need to restart the work queue.
	 */
	spin_lock_bh(&fib_event_list_lock);
	if (list_empty(&fib_event_list)) {
		restart_work = true;
	}

	/*
	 * Add the work at the tail of the list
	 */
	list_add_tail(&fib_event_data->fib_event_list, &fib_event_list);
	spin_unlock_bh(&fib_event_list_lock);

	if (restart_work) {
		queue_work(nss_ppe_vxlanmgr_fib_event_wq, &fib_event_work);
		nss_ppe_vxlanmgr_trace("%px: FIB_EVENT_WORK: started the work-queue", dev);
	}

	return NOTIFY_DONE;
}

struct notifier_block nss_ppe_vxlanmgr_fib_update_nb = {
	.notifier_call = nss_ppe_vxlanmgr_fib_update_event,
};

/*
 * nss_ppe_vxlanmgr_gpe_wq_exit()
 *	Destroy the work_queue for GPE
 */
int nss_ppe_vxlanmgr_gpe_wq_exit(void)
{
	destroy_workqueue(nss_ppe_vxlanmgr_fib_event_wq);
	return 0;
}

/*
 * nss_ppe_vxlanmgr_wq_init()
 *	Initialize the work_queue for GPE
 */
int nss_ppe_vxlanmgr_gpe_wq_init(void)
{
	nss_ppe_vxlanmgr_fib_event_wq = create_singlethread_workqueue("wq_fib_update");

	if (!nss_ppe_vxlanmgr_fib_event_wq){
		nss_ppe_vxlanmgr_warn("work queue allocation failed for VXLAN GPE fib update events");
		return -1;
	}

	INIT_WORK(&fib_event_work, nss_ppe_vxlanmgr_fib_update_event_handler);

	return 0;
}
