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

#ifndef __NSS_IPSECMGR_FLOW_H
#define __NSS_IPSECMGR_FLOW_H

#include <nss_ipsec.h>
#include <nss_ipsecmgr.h>
#include "nss_ipsecmgr_ref.h"

#define NSS_IPSECMGR_FLOW_MAX 256 /* Max flows */
#if (~(NSS_IPSECMGR_FLOW_MAX - 1) & (NSS_IPSECMGR_FLOW_MAX >> 1))
#error "NSS_IPSECMGR_FLOW_MAX is not a power of 2"
#endif

#define NSS_IPSECMGR_FLOW_RETRY_TIMEOUT msecs_to_jiffies(500) /* msecs */

struct nss_ipsecmgr_sa_entry;
struct nss_ipsecmgr_priv;

/*
 * nss_ipsecmgr_flow_state
 */
enum nss_ipsecmgr_flow_state {
	NSS_IPSECMGR_FLOW_STATE_INIT = 0,	/* Flow is initialized */
	NSS_IPSECMGR_FLOW_STATE_PENDING,	/* Flow is pending registration */
	NSS_IPSECMGR_FLOW_STATE_ACTIVE,		/* Flow is registered in NSS */
	NSS_IPSECMGR_FLOW_STATE_MAX
};

/*
 * IPsec manager flow entry
 */
struct nss_ipsecmgr_flow_entry {
	struct list_head list;			/* List object. */
	struct nss_ipsecmgr_ref ref;		/* Reference object. */
	struct nss_ipsec_tuple tuple;		/* Associated inner flow tuple. */
	struct delayed_work retry_work;		/* Retry work */

	struct nss_ipsecmgr_flow_outer outer;	/* Associate outer flow. */

	int tunnel_id;				/* Associate IPsec tunnel */
	struct nss_ipsec_msg nim;		/* IPsec message. */
	atomic_t state;				/* Flow state */
};

/*
 * nss_ipsecmgr_init_flow_db()
 *	Initialize the flow databases
 */
static inline void nss_ipsecmgr_init_flow_db(struct list_head *db)
{
	struct list_head *head = db;
	int i;

	/*
	 * initialize the flow database
	 */
	for (i = 0; i < NSS_IPSECMGR_FLOW_MAX; i++, head++)
		INIT_LIST_HEAD(head);
}

/* functions to operate on flow object */
extern void nss_ipsecmgr_flow_inner2tuple(struct nss_ipsecmgr_flow_inner *inner, struct nss_ipsec_tuple *tuple);
extern void nss_ipsecmgr_flow_outer2tuple(struct nss_ipsecmgr_flow_outer *outer, struct nss_ipsec_tuple *tuple);
extern struct nss_ipsecmgr_flow_entry *nss_ipsecmgr_flow_lookup(struct list_head *db, struct nss_ipsec_tuple *tp);
extern nss_ipsecmgr_status_t nss_ipsecmgr_flow_alloc(struct nss_ipsecmgr_priv *priv, struct nss_ipsec_tuple *tp,
							struct nss_ipsecmgr_sa_entry *sa);
#endif
