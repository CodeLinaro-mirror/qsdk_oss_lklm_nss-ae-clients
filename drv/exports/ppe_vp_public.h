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

/**
 * @file ppe_vp_public.h
 *	NSS PPE VP Public definitions.
 */

#ifndef _PPE_VP_PUBLIC_H_
#define _PPE_VP_PUBLIC_H_

/**
 * @addtogroup ppe_vp_public_subsystem
 * @{
 */

/**
 * ppe_vp_status
 *	Types of PPE VP statuses
 */
typedef enum ppe_vp_status {
	PPE_VP_STATUS_SUCCESS = 0,	/* VP Success */
	PPE_VP_STATUS_FAILURE,		/* VP Failure */
	PPE_VP_STATUS_MAX,		/* Maximum VP statuses */
} ppe_vp_status_t;

typedef int16_t ppe_vp_num_t;

/**
 * Callback function for dynamic interface messages.
 *
 * @datatypes
 * net_device
 * sk_buff
 *
 * @param[in] net_device  Pointer to the net device.
 * @param[in] sk_buff     Pointer to the skb.
 * @param[in] cb_data     Pointer to the callback data.
 */
typedef bool(*ppe_vp_callback_t)(struct net_device *, struct sk_buff *, void *cb_data);

/**
 * ppe_vp_type
 *	Types of VPs
 */
typedef enum ppe_vp_type {
	PPE_VP_TYPE_SW_L2,		/**< VP type for L2 SW interfaces */
	PPE_VP_TYPE_SW_L3,		/**< VP type for L3 SW interfaces */
	PPE_VP_TYPE_HW_L2TUN,		/**< VP type for L2 HW tunnels */
	PPE_VP_TYPE_HW_L3TUN,		/**< VP type for L3 HW tunnels */
	PPE_VP_TYPE_MAX,		/**< Maximum VP types */
} ppe_vp_type_t;

/**
 * ppe_vp_hw_stats
 *	PPE VP HW port Statistics
 */
struct ppe_vp_hw_stats {
	atomic64_t rx_pkts;			/**< Total rx packets on VP port */
	atomic64_t rx_bytes;			/**< Total rx bytes on VP port */
	atomic64_t tx_pkts;			/**< Total tx packets on VP port */
	atomic64_t tx_bytes;			/**< Total tx bytes on VP port */
};

/**
 * Callback function for synchronizing statistics.
 *
 * @datatypes
 * net_device
 * ppe_vp_hw_stats
 *
 * @param[in] net_device  	Pointer to the net device.
 * @param[in] ppe_vp_hw_stats	Pointer to the skb.
 */
typedef bool(*ppe_vp_stats_callback_t)(struct net_device *, struct ppe_vp_hw_stats *);

/** @} */ /* end_addtogroup ppe_vp_public_subsystem */

#endif /* _PPE_VP_PUBLIC_H_ */
