/*
 * ********************************************************************************
 * Copyright (c) 2016-2018, The Linux Foundation. All rights reserved.
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 **********************************************************************************
 */

#ifndef __NSS_IPSECMGR_TUNNEL_H
#define __NSS_IPSECMGR_TUNNEL_H

#include "nss_ipsecmgr_sa.h"

#define NSS_IPSECMGR_CHK_POW2(x) (__builtin_constant_p(x) && !(~(x - 1) & (x >> 1)))

#define NSS_IPSECMGR_MAX_NAME (NSS_IPSECMGR_MAX_KEY_NAME + 64)
#define NSS_IPSECMGR_NODE_STATS_SZ 512
#define NSS_IPSECMGR_CONFIGURE_NODE_RETRY_TIMEOUT msecs_to_jiffies(500) /* msecs */

#define NSS_IPSECMGR_DEFAULT_TUN_NAME "ipsecdummy"

/*
 * IPsec manager private context
 */
struct nss_ipsecmgr_priv {
	struct list_head list;			/* List node */
	struct net_device *dev;			/* back pointer to tunnel device */
	struct nss_ipsecmgr_ref ref;		/* SA objects under the tunnel */
	struct nss_ipsecmgr_callback cb;	/* Callback entry */
	struct rtnl_link_stats64 stats;		/* stats of IPsec tunnel */
};

/*
 * nss_ipsecmgr_init_tun_db()
 *	Initialize the tunnel databases
 */
static inline void nss_ipsecmgr_init_tun_db(struct list_head *db)
{
	struct list_head *head = db;

	/*
	 * initialize the tunnel database
	 */
	INIT_LIST_HEAD(head);
}

/* function to operate on exception data */
extern void nss_ipsecmgr_tunnel_rx_notify(void *app_data, struct nss_ipsec_msg *nim);
extern void nss_ipsecmgr_tunnel_rx(struct net_device *dev, struct sk_buff *skb, struct napi_struct *napi);
extern ssize_t nss_ipsecmgr_tunnel_stats_read(struct file *fp, char __user *ubuf, size_t sz, loff_t *ppos);
#endif
