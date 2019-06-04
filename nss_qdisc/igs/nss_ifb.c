/*
 **************************************************************************
 * Copyright (c) 2019 The Linux Foundation. All rights reserved.
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

#include <nss_api_if.h>
#include <nss_cmn.h>
#include "nss_mirred.h"
#include "nss_igs.h"
#include "nss_ifb.h"

static LIST_HEAD(nss_ifb_list);			/* List of IFB and its mapped interface */
static DEFINE_SPINLOCK(nss_ifb_list_lock);	/* Lock for the ifb list */

/*
 * nss_ifb_list_add()
 *	API to add member in ifb list.
 */
static void nss_ifb_list_add(struct nss_ifb_info *ifb_info)
{
	spin_lock_bh(&nss_ifb_list_lock);
	list_add(&(ifb_info->map_list), &nss_ifb_list);
	spin_unlock_bh(&nss_ifb_list_lock);
}

/*
 * nss_ifb_is_mapped()
 *	Returns the map status of the given ifb bind structure.
 */
bool nss_ifb_is_mapped(struct nss_ifb_info *ifb_info)
{
	bool is_mapped;

	spin_lock_bh(&nss_ifb_list_lock);
	is_mapped = ifb_info->is_mapped;
	spin_unlock_bh(&nss_ifb_list_lock);
	return is_mapped;
}

/*
 * nss_ifb_find_dev()
 *	Find and return the IFB netdev in the ifb list.
 */
struct nss_ifb_info *nss_ifb_find_dev(struct net_device *dev)
{
	struct nss_ifb_info *ifb_info;

	spin_lock_bh(&nss_ifb_list_lock);
	list_for_each_entry(ifb_info, &nss_ifb_list, map_list) {
		if (ifb_info->ifb_dev == dev) {
			spin_unlock_bh(&nss_ifb_list_lock);
			return ifb_info;
		}
	}
	spin_unlock_bh(&nss_ifb_list_lock);
	return NULL;
}

/*
 * nss_ifb_bind()
 *	API to bind an IFB device with its requested mapped interface.
 */
int32_t nss_ifb_bind(struct nss_ifb_info *ifb_info, struct net_device *from_dev,
		struct net_device *to_dev)
{
	if (!ifb_info) {
		/*
		 * IFB not present in local LL.
		 * Add the entry in LL.
		 */
		ifb_info = kmalloc(sizeof(*ifb_info), GFP_KERNEL);
		if (!ifb_info) {
			nss_igs_error("kmalloc failed\n");
			return -ENOMEM;
		}
		ifb_info->ifb_dev = to_dev;
		ifb_info->map_dev = from_dev;
		ifb_info->is_mapped = true;
		nss_ifb_list_add(ifb_info);
	} else {
		/*
		 * IFB present in local LL and its is_mapped is not set.
		 * make the ifb_info's is_mapped to true again.
		 */
		spin_lock_bh(&nss_ifb_list_lock);
		ifb_info->map_dev = from_dev;
		ifb_info->is_mapped = true;
		spin_unlock_bh(&nss_ifb_list_lock);
	}
	return 0;
}

/*
 * nss_ifb_event_cb()
 *	Event Callback for IFB interface to receive events from NSS firmware.
 */
static void nss_ifb_event_cb(void *if_ctx, struct nss_cmn_msg *ncm)
{
	struct net_device *netdev = if_ctx;

	switch (ncm->type) {
	case NSS_IGS_MSG_SYNC_STATS:
		break;

	default:
		nss_igs_error("%p: Unknown Event from NSS\n", netdev);
		break;
	}
}

/*
 * nss_ifb_delete_if()
 *	Delete an IFB interface in NSS Firmware.
 */
void nss_ifb_delete_if(int32_t if_num)
{
	nss_igs_unregister_if(if_num);
	if (nss_dynamic_interface_dealloc_node(if_num, NSS_DYNAMIC_INTERFACE_TYPE_IGS)
			 != NSS_TX_SUCCESS) {
		nss_igs_error("Failed to de-alloc IFB dynamic interface\n");
	}
}

/*
 * nss_ifb_create_if()
 *	Create an IFB interface in NSS Firmware.
 */
int32_t nss_ifb_create_if(struct net_device *dev)
{
	int32_t if_num, ret;
	uint32_t features = 0;
	struct nss_ctx_instance *nss_ctx;

	if_num = nss_dynamic_interface_alloc_node(NSS_DYNAMIC_INTERFACE_TYPE_IGS);
	if (if_num < 0) {
		nss_igs_error("%d interface creation failed\n", if_num);
		return -1;
	}

	nss_ctx = nss_igs_register_if(if_num,
			NSS_DYNAMIC_INTERFACE_TYPE_IGS,
			nss_ifb_event_cb,
			dev,
			features);
	if (!nss_ctx) {
		nss_igs_error("%d interface registration failed\n", if_num);
		goto registration_fail;
	}
	return if_num;

registration_fail:
	ret = nss_dynamic_interface_dealloc_node(if_num, NSS_DYNAMIC_INTERFACE_TYPE_IGS);
	if (ret != NSS_TX_SUCCESS) {
		nss_igs_error("%d interface dealloc failed\n", if_num);
	}
	return -1;
}
