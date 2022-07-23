/*
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
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

#include <net/vxlan.h>
#include <fal_tunnel.h>
#include <fal_mapt.h>
#include <fal_port_ctrl.h>
#include <fal_vport.h>
#include <ppe_drv/ppe_drv.h>
#include <fal_vxlan.h>
#include <ppe_drv_tun_public.h>
#include "ppe_drv_tun.h"
#include "ppe_drv_tun_v4.h"
#include "ppe_drv_tun_v6.h"

/*
 * ppe_drv_tun_check_support()
 *     check if protocol is tunnel
 */
bool ppe_drv_tun_check_support(uint8_t protocol)
{
	switch (protocol) {
	case IPPROTO_IPIP:
	case IPPROTO_GRE:
		return true;
	default:
		return false;
	}
}

/*
 * ppe_drv_tun_ref
 *	Take reference on tunnel context
 */
static struct ppe_drv_tun *ppe_drv_tun_ref(struct ppe_drv_tun *ptun)
{

	kref_get(&ptun->ref);

	ppe_drv_assert(kref_read(&ptun->ref), "%p: ref count rollover for tun_idx:%u", ptun, ptun->tun_idx);
	ppe_drv_trace("%p: tun_idx: %u ref inc:%u", ptun, ptun->tun_idx, kref_read(&ptun->ref));
	return ptun;
}

/*
 * ppe_drv_tun_free
 *	Free tunnel instance
 */
static void ppe_drv_tun_free(struct kref *kref)
{
	struct ppe_drv_tun *ptun = container_of(kref, struct ppe_drv_tun, ref);

	ppe_drv_port_tun_set(ptun->pp, NULL);

	/*
	 * Release all the tables reserved for this tunnel context
	 */
	if (ptun->ptec) {
		if (ppe_drv_tun_encap_deref(ptun->ptec)) {
			ptun->ptec = NULL;
		}
	}

	if (ptun->ptdc) {
		if (ppe_drv_tun_decap_deref(ptun->ptdc)) {
			ptun->ptdc = NULL;
		}
	}

	if (ptun->pt_l3_if) {
		if (ppe_drv_tun_l3_if_deref(ptun->pt_l3_if)) {
			ptun->pt_l3_if = NULL;
		}
	}

	kfree(ptun);
}

/*
 * ppe_drv_tun_deref
 *	Release reference to tunnel context
 */
static bool ppe_drv_tun_deref(struct ppe_drv_tun *ptun)
{
	ppe_drv_assert(kref_read(&ptun->ref), "%p: ref count under run for tun", ptun);

	if (kref_put(&ptun->ref, ppe_drv_tun_free)) {
		ppe_drv_trace("reference count is 0 for tun: %p at index: %u", ptun, ptun->tun_idx);
		return true;
	}

	ppe_drv_trace("%p: tun_idx: %u ref dec:%u", ptun, ptun->tun_idx, kref_read(&ptun->ref));
	return false;
}

/*
 * ppe_drv_tun_port_encap_disable
 * 	Disable encapsulation on tunnel virtual port
 */
bool ppe_drv_tun_port_encap_disable(struct ppe_drv_port *pp)
{
	sw_error_t err;
	fal_vport_state_t vp_state = {0};
	uint32_t v_port = FAL_PORT_ID(FAL_PORT_TYPE_VPORT, pp->port);

	err = fal_vport_state_check_get(PPE_DRV_SWITCH_ID, v_port, &vp_state);
	if (err != SW_OK) {
		ppe_drv_warn("%p: failed to get vp port state for port vp %d", pp, pp->port);
		return false;
	}

	/*
	 * Ensure that VP Check enable is true
	 */
	ppe_drv_assert(vp_state.check_en == true, "%p: VP state check is not enabled on port %d",
						pp, pp->port);

	vp_state.eg_data_valid = false;
	vp_state.vp_active = false;
	err = fal_vport_state_check_set(PPE_DRV_SWITCH_ID, v_port, &vp_state);
	if (err != SW_OK) {
		ppe_drv_warn("%p: failed to reset vp state port vp %d", pp, pp->port);
		return false;
	}

	return true;
}

/*
 * ppe_drv_tun_port_deconfigure
 * 	Deconfigure tunnel specific configuration in port
 */
bool ppe_drv_tun_port_reset_physical_port(struct ppe_drv_port *pp)
{
	sw_error_t err;
	uint32_t v_port = FAL_PORT_ID(FAL_PORT_TYPE_VPORT, pp->port);

	err = fal_vport_physical_port_id_set(PPE_DRV_SWITCH_ID, v_port, 0);
	if (err != SW_OK) {
		ppe_drv_warn("%p: failed to reset physical port for vp %d", pp, pp->port);
		return false;
	}

	return true;
}

/*
 * ppe_drv_tun_port_configure
 * 	Configure L2 VP port table.
 */
bool ppe_drv_tun_port_configure(struct ppe_drv_tun *ptun, uint16_t xmit_port)
{
	sw_error_t err;
	fal_vport_state_t vp_state = {0};
	struct ppe_drv_port *pp = ptun->pp;
	uint32_t v_port = FAL_PORT_ID(FAL_PORT_TYPE_VPORT, pp->port);
	uint16_t extra_hdr_len = 0;
	uint8_t dp_queue_id;
	struct ppe_drv_port *dp = NULL; /* Destination port */

	/*
	 * TODO: Update extra header length setting in port MTU config
	 * based on PPPOE header
	 */
	extra_hdr_len = 0;
	if (!ppe_drv_port_mtu_cfg_update(pp, extra_hdr_len)) {
		ppe_drv_warn("%p: failed to set mtu extra header len for port %d", ptun, pp->port);
		return false;
	}

	/*
	 * Set phyiscal port or CPU port based on xmit_port value
	 */
	xmit_port = (xmit_port < PPE_DRV_PHYSICAL_MAX) ? xmit_port : 0;

	/*
	 * Get destination port
	 */
	dp = ppe_drv_port_from_port_num(xmit_port);
	if (!dp) {
		ppe_drv_warn("%p: Couldn't get destination port for iface index %u", ptun, xmit_port);
		return false;
	}

	/*
	 * Map destination port queue to tunnel port
	 */
	dp_queue_id = ppe_drv_port_ucast_queue_get(dp);
	if (!ppe_drv_port_ucast_queue_set(ptun->pp, dp_queue_id)) {
		ppe_drv_warn("%p: Failed to set queue %d for port", ptun, dp_queue_id);
		return false;
	}
	ppe_drv_trace("%p: Destination port: %p:%d, queue_id:%d", ptun, dp, xmit_port, dp_queue_id);

	err = fal_vport_physical_port_id_set(PPE_DRV_SWITCH_ID, v_port, xmit_port);
	if (err != SW_OK) {
		ppe_drv_warn("%p: failed to set physical port:%d for vp port:%d", pp,
					xmit_port, pp->port);
		return false;
	}

	err = fal_vport_state_check_get(PPE_DRV_SWITCH_ID, v_port, &vp_state);
	if (err != SW_OK) {
		ppe_drv_warn("%p: failed to get vp port state for port vp %d", pp, pp->port);
		return false;
	}

	/*
	 * Ensure that VP Check enable is true
	 */
	ppe_drv_assert(vp_state.check_en == true, "%p: VP state check is not enabled on port %d",
						pp, pp->port);

	/*
	 * Set eg_data_valid and context enable
	 */
	vp_state.eg_data_valid = true;
	vp_state.vp_active = true;
	err = fal_vport_state_check_set(PPE_DRV_SWITCH_ID, v_port, &vp_state);
	if (err != SW_OK) {
		ppe_drv_warn("%p: failed to set L2 vp port state for port %d", pp, pp->port);
		return false;
	}

	return true;
}

/*
 * ppe_drv_tun_port_tl_l3_if_get
 * Get the tl_l3_if associated with port
 */
struct ppe_drv_tun_l3_if *ppe_drv_tun_port_tl_l3_if_get(struct ppe_drv_tun *ptun, uint16_t xmit_port)
{
	struct ppe_drv_port *pp = NULL;
	struct ppe_drv *p = &ppe_drv_gbl;
	struct ppe_drv_tun_l3_if *ptun_l3_if = NULL;

	/*
	 * Get destination port
	 */
	pp = ppe_drv_port_from_port_num(xmit_port);
	if (!pp) {
		ppe_drv_warn("%p: Couldn't get destination port for iface index %u", ptun, xmit_port);
		return NULL;
	}

	/*
	 * check if tl_l3_if attached to port if so take a reference
	 * reference count will be decremented during tun deactivate
	 */
	ptun_l3_if = ppe_drv_port_tl_l3_if_get_n_ref(pp);
	if (ptun_l3_if) {
		return ptun_l3_if;
	}

	/*
	 * Attach tl_l3_if to destination port
	 */
	ptun_l3_if = ppe_drv_tun_l3_if_alloc(p);
	if (!ptun_l3_if) {
		ppe_drv_warn("%p: Failed to attach tl_l3_if to port %u", ptun, xmit_port);
		return NULL;
	}

	ppe_drv_tun_l3_if_configure(ptun_l3_if);

	/*
	 * Increase the reference count to tl_l3_if this would be used for
	 * reference count decrement when port is getting destroyed
	 */
	ppe_drv_port_tl_l3_if_attach(pp, ptun_l3_if);

	/*
	 * Take additional reference count to decrement during tun deactivate
	 */
	return ppe_drv_port_tl_l3_if_get_n_ref(pp);
}

/*
 * ppe_drv_tun_decap_xmitport_cfg_set
 *	Port DECAP Configuration setup
 */
bool ppe_drv_tun_decap_xmitport_cfg_set(struct ppe_drv_tun *ptun, uint16_t xmit_port,
				       struct ppe_drv_tun_cmn_ctx_l2 *l2_hdr, uint16_t tl_l3_if_idx)
{

	fal_tunnel_port_intf_t port_tnl_cfg = {0};
	struct ppe_drv_port *dp = NULL;
	sw_error_t err;

	/*
	 * Set phyiscal port or CPU port based on xmit_port value
	 */
	xmit_port = (xmit_port < PPE_DRV_PHYSICAL_MAX) ? xmit_port : 0;

	/*
	 * Get destination port
	 */
	dp = ppe_drv_port_from_port_num(xmit_port);
	if (!dp) {
		ppe_drv_warn("%p: Couldn't get destination port for iface index %u", ptun, xmit_port);
		return false;
	}

	/*
	 * Get PPPoE profile and set xmit port configuration
	 */
	if (l2_hdr->flags & PPE_DRV_TUN_CMN_CTX_L2_PPPOE_VALID) {
		port_tnl_cfg.pppoe_en = PPE_DRV_TUN_FIELD_VALID;
		/*
		 * TODO: Check if any specific handling needed for PPPOE
		 */
	}

	port_tnl_cfg.l3_if.l3_if_valid = true;
	port_tnl_cfg.l3_if.l3_if_index = tl_l3_if_idx;

	/*
	 * Set MAC address of port on which tunnel is established.
	 */
	memcpy(port_tnl_cfg.mac_addr.uc, &l2_hdr->smac[0], ETH_ALEN);

	err = fal_tunnel_port_intf_set(PPE_DRV_SWITCH_ID, xmit_port, &port_tnl_cfg);
	if (err != SW_OK) {
		ppe_drv_warn("%p: unable to set xmit port %d", ptun, xmit_port);
		return false;
	}

	return true;
}

/*
 * ppe_drv_tun_deactivate
 *	Deactivate PPE tunnel
 */
bool ppe_drv_tun_deactivate(uint16_t port_num, void *vdestroy_rule)
{
	ppe_drv_ret_t ret = PPE_DRV_RET_SUCCESS;
	struct ppe_drv_tun_cmn_ctx *pth;
	struct ppe_drv *p = &ppe_drv_gbl;
	struct ppe_drv_port *pp;
	struct ppe_drv_tun *ptun;
	struct ppe_drv_v4_conn_sync *cns_v4 = NULL;
	struct ppe_drv_v6_conn_sync *cns_v6 = NULL;
	bool is_ipv6;

	spin_lock_bh(&p->lock);

	pp = ppe_drv_port_from_port_num(port_num);
	if (!pp) {
		spin_unlock_bh(&p->lock);
		ppe_drv_warn("%p: Invalid port number %d", p, port_num);
		return false;
	}

	ptun = ppe_drv_port_tun_get(pp);
	if (!ptun) {
		spin_unlock_bh(&p->lock);
		ppe_drv_warn("%p: Tunnel not found for port %d", p, port_num);
		return false;
	}

	pth = ptun->th;
	is_ipv6 = ppe_drv_tun_cmn_ctx_tun_is_ipv6(pth);

	if (vdestroy_rule && is_ipv6) {
		ret = ppe_drv_v6_tun_del_ce_validate(vdestroy_rule, &cns_v6);
		if (ret != PPE_DRV_RET_SUCCESS) {
			spin_unlock_bh(&p->lock);
			return false;
		}
	} else if (vdestroy_rule) {
		ret = ppe_drv_v4_tun_del_ce_validate(vdestroy_rule, &cns_v4);
		if (ret != PPE_DRV_RET_SUCCESS) {
			spin_unlock_bh(&p->lock);
			return false;
		}
	}

	ppe_drv_trace("%p: Deactivating Tunnel %d at index %u", ptun, pth->type, ptun->tun_idx);

	/*
	 * Disable decapsulation
	 */
	if (!ppe_drv_tun_decap_disable(ptun->ptdc)) {
		ppe_drv_assert(false, "%p: Decap reset failed for deactivating Tunnel %d at index %u", ptun,
				pth->type, ptun->tun_idx);
		goto error;
	}

	/*
	 * Disable encapsulation
	 */
	if (!ppe_drv_tun_port_encap_disable(pp)) {
		ppe_drv_assert(false, "%p: Encap reset failed for deactivating Tunnel %d at index %u", ptun,
						pth->type, ptun->tun_idx);
		goto error;
	}

	/*
	 * Reset phyisical port associated with tunnel port
	 */
	if (!ppe_drv_tun_port_reset_physical_port(pp)) {
		ppe_drv_assert(false, "%p: Physical port reset failed for  deactivating Tunnel %d at index %u",
				ptun, pth->type, ptun->tun_idx);
		goto error;
	}

	/*
	 * Detach reference for tl_l3_if index; reference is taken during activate
	 */
	ppe_drv_tun_l3_if_deref(ptun->pt_l3_if);
	ptun->pt_l3_if = NULL;

	/*
	 * Reset encap entry association with virtual port
	 */
	if (!ppe_drv_tun_encap_tun_idx_configure(ptun->ptec, ptun->vp_num, false)) {
		ppe_drv_assert(false, "%p: Failed resetting encap index for Tunnel %d at index %u", ptun,
						pth->type, ptun->tun_idx);
		goto error;
	}

	/*
	 * Release reference
	 */
	ppe_drv_tun_deref(ptun);
	spin_unlock_bh(&p->lock);

	/*
	 *  Invoke callback and free the cns structure
	 */
	if (cns_v4) {
		ppe_drv_v4_conn_stats_sync_invoke_cb(cns_v4);
		ppe_drv_v4_conn_stats_free(cns_v4);
	} else if (cns_v6) {
		ppe_drv_v6_conn_stats_sync_invoke_cb(cns_v6);
		ppe_drv_v6_conn_stats_free(cns_v6);
	}

	return true;

error:
	spin_unlock_bh(&p->lock);
	ppe_drv_v4_conn_stats_free(cns_v4);
	ppe_drv_v6_conn_stats_free(cns_v6);

	return false;
}
EXPORT_SYMBOL(ppe_drv_tun_deactivate);

/*
 * ppe_drv_tun_vxlan_deconfigure
 *	Disable VxLAN PPE tunnel
 */
void ppe_drv_tun_vxlan_deconfigure(struct ppe_drv *p)
{
	fal_tunnel_udp_entry_t ftue = {0};
	fal_vxlan_type_t type = FAL_VXLAN;
	sw_error_t err;

	ftue.ip_ver = FAL_TUNNEL_IP_VER_V4;
	ftue.udp_type = FAL_TUNNEL_L4_TYPE_UDP;
	ftue.l4_port_type = FAL_TUNNEL_L4_PORT_TYPE_DST;
	ftue.l4_port = IANA_VXLAN_UDP_PORT;
	err = fal_vxlan_entry_del(FUNC_VXLAN_ENTRY_ADD, type, &ftue);
	if (err != SW_OK) {
		ppe_drv_warn("%p VXLAN: failed to delete UDP entry for IPV4 %d", p, err);
	}

	ftue.ip_ver = FAL_TUNNEL_IP_VER_V6;
	err = fal_vxlan_entry_del(FUNC_VXLAN_ENTRY_ADD, type, &ftue);
	if (err != SW_OK) {
		ppe_drv_warn("%p VXLAN: failed to delete UDP entry for IPV6 %d", p, err);
	}
}

/*
 * ppe_drv_tun_deconfigure
 *	Disable PPE tunnel
 */
bool ppe_drv_tun_deconfigure(uint16_t port_num)
{
	struct ppe_drv_tun_cmn_ctx *pth = NULL;
	struct ppe_drv *p = &ppe_drv_gbl;
	struct ppe_drv_tun *ptun;
	struct ppe_drv_port *pp;

	spin_lock_bh(&p->lock);

	pp = ppe_drv_port_from_port_num(port_num);
	if (!pp) {
		spin_unlock_bh(&p->lock);
		ppe_drv_warn("Invalid port number %d", port_num);
		return false;
	}

	ptun = ppe_drv_port_tun_get(pp);
	if (!ptun) {
		spin_unlock_bh(&p->lock);
		ppe_drv_warn("Tunnel not found for port %d", port_num);
		return false;
	}

	pth = ptun->th;

	ppe_drv_trace("%p: Destroying Tunnel %d at index %u", ptun, pth->type, ptun->tun_idx);

	/*
	 * Release reference
	 */
	ppe_drv_tun_deref(ptun);
	spin_unlock_bh(&p->lock);
	return true;
}
EXPORT_SYMBOL(ppe_drv_tun_deconfigure);

/*
 * ppe_drv_tun_activate
 *	Activate PPE tunnel
 */
bool ppe_drv_tun_activate(uint16_t port_num, void *vcreate_rule)
{
	struct ppe_drv *p = &ppe_drv_gbl;
	fal_port_t port_id;
	uint16_t xmit_port;
	uint16_t tl_l3_if_idx;
	bool dc_cfg_status = false;
	struct ppe_drv_port *pp;
	struct ppe_drv_tun *ptun;
	struct ppe_drv_v4_conn *cn_v4 = NULL;
	struct ppe_drv_v6_conn *cn_v6 = NULL;
	struct ppe_drv_comm_stats *comm_stats;
	struct ppe_drv_tun_cmn_ctx *pth;
	struct ppe_drv_tun_cmn_ctx_l2 *l2_hdr;
	bool is_ipv6;

	comm_stats = &p->stats.comm_stats[PPE_DRV_CONN_TYPE_TUNNEL];

	spin_lock_bh(&p->lock);

	pp = ppe_drv_port_from_port_num(port_num);
	if (!pp) {
		ppe_drv_warn("%p: invalid port number %d", p, port_num);
		goto err_fail;
	}

	ptun = ppe_drv_port_tun_get(pp);
	if (!ptun) {
		ppe_drv_warn("%p: tunnel not found for port %d", p, port_num);
		goto err_fail;
	}

	if (!ptun->ptec) {
		ppe_drv_warn("%p: tun encap is not initialized properly", ptun);
		goto err_fail;
	}

	if (!ptun->ptdc) {
		ppe_drv_warn("%p: tun decap is not initialized properly", ptun);
		goto err_fail;
	}

	pth = ptun->th;
	l2_hdr = &pth->l2;
	is_ipv6 = ppe_drv_tun_cmn_ctx_tun_is_ipv6(pth);

	/*
	 * there are two ways tunnel can be configured.
	 * a. when ECM is front end, vcreate_rule is filled during outer rule push and we need
	 *    to extract the L2 parameter from it.
	 * b. the L2 parameter can be configured from user when ECM is not present.
	 */
	if (vcreate_rule && is_ipv6) {
		cn_v6 = ppe_drv_v6_conn_alloc();
		if (!cn_v6) {
			ppe_drv_stats_inc(&comm_stats->v6_create_fail_mem);
			ppe_drv_warn("%p: failed to allocate connection memory: %p", p, vcreate_rule);
			goto err_fail;
		}

		if (ppe_drv_v6_tun_add_ce_validate(vcreate_rule, cn_v6)) {
			goto err_fail;
		}

		/*
		 * Extract the L2 HDR from ECM rule
		 */
		ppe_drv_tun_v6_parse_l2_hdr(vcreate_rule, cn_v6, l2_hdr);
	} else if (vcreate_rule) {
		cn_v4 = ppe_drv_v4_conn_alloc();
		if (!cn_v4) {
			ppe_drv_stats_inc(&comm_stats->v4_create_fail_mem);
			ppe_drv_warn("%p: failed to allocate connection memory: %p", p, vcreate_rule);
			goto err_fail;
		}

		if (ppe_drv_v4_tun_add_ce_validate(vcreate_rule, cn_v4)) {
			goto err_fail;
		}

		/*
		 * Extract the L2 HDR from ECM Connection entry
		 */
		ppe_drv_tun_v4_parse_l2_hdr(vcreate_rule, cn_v4, l2_hdr);
	}

	port_id = ptun->vp_num;

	/*
	 * 1. Program EG Header Data table
	 * 2. Program EG tunnel control table
	 * 3. Program EG VP table
	 */
	if (!ppe_drv_tun_encap_configure(ptun->ptec, pth, l2_hdr)) {
		ppe_drv_warn("%p: Failed to do encap configure for tun %d of type %d", ptun, ptun->tun_idx,
								pth->type);
		goto err_fail;
	}

	if (!ppe_drv_tun_encap_tun_idx_configure(ptun->ptec, ptun->vp_num, true)) {
		ppe_drv_warn("%p: Failed to do encap tun idx for tun %d of type %d", ptun, ptun->tun_idx,
								pth->type);
		goto err_fail;
	}

	/*
	 * tunnel Counter configuration is already set during port alloc time
	 */

	/*
	 * TODO: Need API to enable VSI_TAG Mode on EG_VP_TBL
	 * Check if EG_L3_IF table needs to be programmed
	 */

	/*
	 * TODO: Need to add PPPOE specific handling
	 */
	xmit_port = l2_hdr->xmit_port;

	/*
	 * Get the tl_l3_if_index;
	 */
	ptun->pt_l3_if = ppe_drv_tun_port_tl_l3_if_get(ptun, xmit_port);
	if (ptun->pt_l3_if == NULL) {
		ppe_drv_warn("%p: Failed to get active tl l3 index for tun %d of type %d",
					ptun, ptun->tun_idx, pth->type);
		goto err_fail;
	}

	tl_l3_if_idx = ppe_drv_tun_l3_if_get_index(ptun->pt_l3_if);

	/*
	 * Set TL_L3_IDX and transmit mac address in TL_PORT_VP_TBL
	 */
	ppe_drv_tun_decap_xmitport_cfg_set(ptun, xmit_port, l2_hdr, tl_l3_if_idx);
	ppe_drv_tun_decap_set_tl_l3_idx(ptun->ptdc, tl_l3_if_idx);


	if (pth->type != PPE_DRV_TUN_CMN_CTX_TYPE_MAPT) {
		dc_cfg_status = ppe_drv_tun_decap_activate(ptun->ptdc, l2_hdr);
	} else {
		dc_cfg_status = false; /*TODO : MAP-T activate */
	}

	if (!dc_cfg_status) {
		ppe_drv_warn("%p: Failed to activate decap entry for tun %d of type %d", ptun, ptun->tun_idx,
								pth->type);
		goto err_fail;
	}

	/*
	 * Activate tunnel in L2_VP_TBL
	 */
	if (!ppe_drv_tun_port_configure(ptun, xmit_port)) {
		ppe_drv_warn("%p: Failed to configure VP tunnel port for tun %d of type %d", ptun,
						ptun->tun_idx, pth->type);
		goto err_fail;
	}

	ptun->xmit_port = xmit_port;

	/*
	 * Take reference
	 */
	ppe_drv_tun_ref(ptun);

	if (cn_v6) {
		list_add(&cn_v6->list, &p->conn_tun_v6);
	} else if (cn_v4) {
		list_add(&cn_v4->list, &p->conn_tun_v4);
	}

	spin_unlock_bh(&p->lock);
	return true;

err_fail:
	spin_unlock_bh(&p->lock);
	kfree(cn_v4);
	kfree(cn_v6);
	return false;
}
EXPORT_SYMBOL(ppe_drv_tun_activate);

/*
 * ppe_drv_tun_configure
 *	Allocate PPE tunnel instance and initialize objects
 */
bool ppe_drv_tun_configure(uint16_t port_num, struct ppe_drv_tun_cmn_ctx *pth, void *add_cb, void *del_cb)
{
	struct ppe_drv *p = &ppe_drv_gbl;
	struct ppe_drv_tun *ptun = NULL;
	int16_t decap_hwidx = PPE_DRV_TUN_DECAP_INVALID_IDX;

	struct ppe_drv_port *pp = ppe_drv_port_from_port_num(port_num);
	if (!pp) {
		ppe_drv_warn("%p: Invalid port number %d", p, port_num);
		return false;
	}

	ppe_drv_assert(pth->type < PPE_DRV_TUN_CMN_CTX_TYPE_MAX, "%p: unknown tunnel type %u", p, pth->type);

	/*
	 * Check if the tunnel type is MAP-T, it requires separate tables
	 */
	if (pth->type == PPE_DRV_TUN_CMN_CTX_TYPE_MAPT) {
		/*
		 * TODO: MAP-T alloc
		 */
	}

	/*
	 * Check and try allocating tunnel instance corresponding to tun_idx
	 * on allocation one reference would be taken
	 */
	ptun = kzalloc(sizeof(struct ppe_drv_tun), GFP_ATOMIC);
	if (!ptun) {
		ppe_drv_warn("%p: Couldn't allocate tun memory", p);
		return false;
	}

	kref_init(&ptun->ref);

	ptun->th = pth;
	ptun->pp = pp;
	ptun->add_cb = add_cb;
	ptun->del_cb = del_cb;
	ptun->vp_num = ppe_drv_port_num_get(pp);

	/*
	 * Inbound tunnel packets are processed through decap TL_TBL, get an instance.
	 */
	spin_lock_bh(&p->lock);
	ptun->ptdc = ppe_drv_tun_decap_alloc(p);
	if (!ptun->ptdc) {
		ppe_drv_warn("%p: Failed to get decap instance", ptun);
		goto err_exit;
	}

	/*
	 * Need to get the Hw index first for decap operation before allocating any other entries
	 */
	decap_hwidx = ppe_drv_tun_decap_configure(ptun->ptdc, pp, pth);
	if (decap_hwidx == PPE_DRV_TUN_DECAP_INVALID_IDX) {
		ppe_drv_warn("%p: Failed to allocate decap hw entry idx", ptun);
		goto err_exit;
	}

	/*
	 * Set the TL table index in HW table
	 */
	ppe_drv_tun_decap_set_tl_index(ptun->ptdc, decap_hwidx);

	/*
	 * Request instances of encap tables
	 */
	ptun->ptec = ppe_drv_tun_encap_alloc(p);
	if (!ptun->ptec) {
		ppe_drv_warn("%p: couldn't get encap index", ptun);
		goto err_exit;
	}

	ptun->ptec->port = pp;

	ppe_drv_port_tun_set(pp, ptun);

	spin_unlock_bh(&p->lock);
	ppe_drv_trace("%p: tun context with tun_idx %u of type %u created", ptun, ptun->tun_idx, pth->type);

	return true;

err_exit:
	ppe_drv_tun_deref(ptun);
	spin_unlock_bh(&p->lock);
	return false;
}
EXPORT_SYMBOL(ppe_drv_tun_configure);

/*
 * ppe_drv_tun_global_init
 *	Initialize tables with values that does not require changes
 */
bool ppe_drv_tun_global_init(struct ppe_drv *p)
{
	sw_error_t err;
	fal_vxlan_type_t type = FAL_VXLAN;
	fal_tunnel_decap_key_t ptdkcfg =  {0};
	fal_tunnel_global_cfg_t ptglcfg =  {0};
	fal_mapt_decap_ctrl_t ptmapglcfg = {0};
	fal_tunnel_udp_entry_t ftue = {0};
	fal_tunnel_type_t tunnel_type;

	spin_lock_bh(&p->lock);

	/*
	 * GRETAP IPv4/6 key configuration
	 */
	ptdkcfg.key_bmp |= PPE_DRV_TUN_BIT(FAL_TUNNEL_KEY_SIP_EN);
	ptdkcfg.key_bmp |= PPE_DRV_TUN_BIT(FAL_TUNNEL_KEY_DIP_EN);
	ptdkcfg.key_bmp |= PPE_DRV_TUN_BIT(FAL_TUNNEL_KEY_L4PROTO_EN);
	ptdkcfg.key_bmp |= PPE_DRV_TUN_BIT(FAL_TUNNEL_KEY_TLINFO_EN);
	ptdkcfg.tunnel_info_mask = FAL_TUNNEL_DECAP_TUNNEL_INFO_MASK;

	tunnel_type = FAL_TUNNEL_TYPE_GRE_TAP_OVER_IPV4;
	err = fal_tunnel_decap_key_set(PPE_DRV_SWITCH_ID, tunnel_type, &ptdkcfg);
	if (err != SW_OK) {
		spin_unlock_bh(&p->lock);
		ppe_drv_warn("%p: Tunnel Decap key set failure for GREIPV4", p);
		return false;
	}

	tunnel_type = FAL_TUNNEL_TYPE_GRE_TAP_OVER_IPV6;
	err = fal_tunnel_decap_key_set(PPE_DRV_SWITCH_ID, tunnel_type, &ptdkcfg);
	if (err != SW_OK) {
		spin_unlock_bh(&p->lock);
		ppe_drv_warn("%p: Tunnel Decap key set failure for GREIPV6", p);
		return false;
	}

	/*
	 * VxLAN IPv4/6 key configuration
	 */
	ptdkcfg.key_bmp |= PPE_DRV_TUN_BIT(FAL_TUNNEL_KEY_DPORT_EN);
	ptdkcfg.tunnel_info_mask = FAL_TUNNEL_DECAP_TUNNEL_INFO_MASK;
	tunnel_type = FAL_TUNNEL_TYPE_VXLAN_OVER_IPV4;
	err = fal_tunnel_decap_key_set(PPE_DRV_SWITCH_ID, tunnel_type, &ptdkcfg);
	if (err != SW_OK) {
		spin_unlock_bh(&p->lock);
		ppe_drv_warn("%p: Tunnel Decap key set failure for VxlanIPV4", p);
		return false;
	}

	tunnel_type = FAL_TUNNEL_TYPE_VXLAN_OVER_IPV6;
	err = fal_tunnel_decap_key_set(PPE_DRV_SWITCH_ID, tunnel_type, &ptdkcfg);
	if (err != SW_OK) {
		spin_unlock_bh(&p->lock);
		ppe_drv_warn("%p: Tunnel Decap key set failure for VxlanIPV6", p);
		return false;
	}

	/*
	 * VxLAN GPE IPv4/6 key configuration
	 * TODO: Check if additional flags are needed for GPE
	 */
	tunnel_type = FAL_TUNNEL_TYPE_VXLAN_GPE_OVER_IPV4;
	err = fal_tunnel_decap_key_set(PPE_DRV_SWITCH_ID, tunnel_type, &ptdkcfg);
	if (err != SW_OK) {
		spin_unlock_bh(&p->lock);
		ppe_drv_warn("%p: Tunnel Decap key set failure for VxlanGPEIPV4", p);
		return false;
	}

	tunnel_type = FAL_TUNNEL_TYPE_VXLAN_GPE_OVER_IPV6;
	err = fal_tunnel_decap_key_set(PPE_DRV_SWITCH_ID, tunnel_type, &ptdkcfg);
	if (err != SW_OK) {
		spin_unlock_bh(&p->lock);
		ppe_drv_warn("%p: Tunnel Decap key set failure for VxlanGPEIPV6", p);
		return false;
	}

	/*
	 * VxLAN Decap port number match for IPV4 tunnel.
	 */
	ftue.ip_ver = FAL_TUNNEL_IP_VER_V4;
	ftue.udp_type = FAL_TUNNEL_L4_TYPE_UDP;
	ftue.l4_port_type = FAL_TUNNEL_L4_PORT_TYPE_DST;
	ftue.l4_port = IANA_VXLAN_UDP_PORT;
	err = fal_vxlan_entry_add(0, type, &ftue);
	if (err != SW_OK) {
		spin_unlock_bh(&p->lock);
		ppe_drv_warn("%p failed to add UDP entry for IPV4. err: %d", p, err);
		return false;
	}

	/*
	 * VxLAN Decap port number match for IPV6 tunnel.
	 */
	ftue.ip_ver = FAL_TUNNEL_IP_VER_V6;
	err = fal_vxlan_entry_add(0, type, &ftue);
	if (err != SW_OK) {
		spin_unlock_bh(&p->lock);
		ppe_drv_warn("%p failed to add UDP entry for IPV6. err: %d", p, err);
		return false;
	}

	/*
	 * IPv4 over IPv6
	 */
	ptdkcfg.key_bmp = 0;
	ptdkcfg.key_bmp |= PPE_DRV_TUN_BIT(FAL_TUNNEL_KEY_SIP_EN);
	ptdkcfg.key_bmp |= PPE_DRV_TUN_BIT(FAL_TUNNEL_KEY_DIP_EN);
	ptdkcfg.key_bmp |= PPE_DRV_TUN_BIT(FAL_TUNNEL_KEY_L4PROTO_EN);

	tunnel_type = FAL_TUNNEL_TYPE_IPV4_OVER_IPV6;
	err = fal_tunnel_decap_key_set(PPE_DRV_SWITCH_ID, tunnel_type, &ptdkcfg);
	if (err != SW_OK) {
		spin_unlock_bh(&p->lock);
		ppe_drv_warn("%p: Tunnel Decap key set failure for IPIP6", p);
		return false;
	}

	/*
	 * Initialize Tunnel global configuration
	 */
	ptglcfg.deacce_action = FAL_MAC_RDT_TO_CPU;
	ptglcfg.src_if_check_action = FAL_MAC_RDT_TO_CPU;
	ptglcfg.src_if_check_deacce_en = true;
	ptglcfg.vlan_check_action = FAL_MAC_RDT_TO_CPU;
	ptglcfg.vlan_check_deacce_en = true;
	ptglcfg.udp_csum_zero_action = FAL_MAC_RDT_TO_CPU;
	ptglcfg.udp_csum_zero_deacce_en = false;
	ptglcfg.pppoe_multicast_action = FAL_MAC_RDT_TO_CPU;
	ptglcfg.pppoe_multicast_deacce_en = true;
	ptglcfg.hash_mode[0] = PPE_DRV_TUN_TL_HASH_MODE_CRC10;
	ptglcfg.hash_mode[1] = PPE_DRV_TUN_TL_HASH_MODE_XOR;
	err = fal_tunnel_global_cfg_set(PPE_DRV_SWITCH_ID, &ptglcfg);
	if (err != SW_OK) {
		spin_unlock_bh(&p->lock);
		ppe_drv_warn("%p: Tunnel Global configuration failed", p);
		return false;
	}

	/*
	 * Initialize map-t specific tunnel configuration
	 */
	ptmapglcfg.src_check_action = FAL_MAC_RDT_TO_CPU;
	ptmapglcfg.dst_check_action = FAL_MAC_RDT_TO_CPU;
	ptmapglcfg.no_tcp_udp_action = FAL_MAC_RDT_TO_CPU;
	ptmapglcfg.udp_csum_zero_action = FAL_MAC_FRWRD;
	ptmapglcfg.ipv4_df_set = PPE_DRV_TUN_TL_IPV4_DF_MODE_0;
	err = fal_mapt_decap_ctrl_set(PPE_DRV_SWITCH_ID, &ptmapglcfg);
	if (err != SW_OK) {
		spin_unlock_bh(&p->lock);
		ppe_drv_warn("%p: Tunnel Map-t common configuration failed", p);
		return false;
	}

	spin_unlock_bh(&p->lock);
	return true;
}
