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

#ifndef __NSS_IPSECMGR_SA_H
#define __NSS_IPSECMGR_SA_H

#include "nss_ipsecmgr_flow.h"

#define NSS_IPSECMGR_SA_MAX  64 /* Max SAs */
#if (~(NSS_IPSECMGR_SA_MAX - 1) & (NSS_IPSECMGR_SA_MAX >> 1))
#error "NSS_IPSECMGR_SA_MAX is not a power of 2"
#endif

#define NSS_IPSECMGR_SA_STATS_SZ 512
#define NSS_IPSECMGR_SA_STATS_BUF_SZ 2048
#define NSS_IPSECMGR_SA_FREE_TIMEOUT msecs_to_jiffies(100) /* msecs */

#define NSS_IPSECMGR_ESP_TRAIL_SZ 2 /* esp trailer size */
#define NSS_IPSECMGR_ESP_PAD_SZ 14 /* maximum amount of padding */

/*
 * IPsec manager packets stats per SA
 */
struct nss_ipsecmgr_sa_stats_priv {
	/* Packet counters */
	uint64_t count;				/* Packets processed */
	uint64_t bytes;				/* Bytes processed */

	/* Drop counters */
	uint64_t no_headroom;			/* no headroom */
	uint64_t no_tailroom;			/* no tailroom */
	uint64_t no_buf;			/* no resource in NSS */
	uint64_t fail_queue;			/* Enqueue to nexthop failed */
	uint64_t fail_hash;			/* Hash check failed */
	uint64_t fail_replay;			/* Replay check failed */
	uint64_t fail_hash_cont;		/* Continous fail hash count */

	/* SA state */
	uint64_t seq_num;			/* Current sequence no. */
	uint64_t window_max;			/* Maximum window size supported */
	uint32_t window_size;			/* Current window size */
};

/*
 * IPsec manager SA entry
 */
struct nss_ipsecmgr_sa_entry {
	struct list_head list;			/* List node */
	struct nss_ipsecmgr_ref ref;		/* Reference node */
	struct nss_ipsec_tuple tuple;		/* SA tuple */

	struct delayed_work free_work;		/* Delayed free work */
	unsigned long free_timeout;		/* Delayed free timeout */

	struct crypto_aead *aead;		/* Linux crypto AEAD context */
	struct crypto_ahash *ahash;		/* Linux crypto AHASH context */

	struct nss_ipsecmgr_flow_outer outer;	/* Flow outer representing SA */

	struct nss_ipsec_rule_oip oip;		/* Outer IP information */
	struct nss_ipsec_rule_data data;	/* SA data */

	struct nss_ipsecmgr_priv *priv;		/* Device private */

	struct nss_ipsecmgr_sa_stats_priv stats;/* Per SA  statistics */
	struct dentry *dentry;			/* Debugfs entry per stats dir */

	enum nss_ipsec_type type;		/* ENCAP or DECAP type */
	uint32_t replay_fail_thresh;		/* Replay failure threshold */
	uint16_t if_num;			/* Associated interface number */
};

/*
 * nss_ipsecmgr_init_sa_db()
 * 	initialize the SA database
 */
static inline void nss_ipsecmgr_init_sa_db(struct list_head *db)
{
	struct list_head *head = db;
	int i;

	/*
	 * initialize the SA database
	 */
	for (i = 0; i < NSS_IPSECMGR_SA_MAX; i++, head++)
		INIT_LIST_HEAD(head);
}

/* functions to operate on SA object */
extern struct nss_ipsecmgr_sa_entry *nss_ipsecmgr_sa_lookup(struct list_head *db, struct nss_ipsec_tuple *tp);
void nss_ipsecmgr_sa_update_stats(struct nss_ipsecmgr_sa_entry *sa, struct nss_ipsec_sa_stats *stats,
					struct nss_ipsecmgr_event *ev);

#endif
