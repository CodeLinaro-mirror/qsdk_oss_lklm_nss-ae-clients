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

struct ppe_vp;
struct ppe_vp_base;

/*
 * ppe_vp_base_stats
 *	PPE VP base Statistics
 */
struct ppe_vp_base_stats {
	atomic64_t vp_allocation_fails;		/* Total VP allocation failures */
	atomic64_t vp_table_full;		/* VP allocation fails due to table full */
	atomic64_t mtu_assign_fails;		/* MTU assign fails */
	atomic64_t mac_assign_fails;		/* MAC assign fails */
};

/*
 * ppe_vp_misc_stats
 *	PPE VP misc Statistics
 */
struct ppe_vp_misc_stats {
	atomic64_t netdev_if_num;         	/* Net device interface number */
	atomic64_t ppe_port_num;          	/* PPE Port number */
};

/*
 * ppe_vp_rx_stats
 *	PPE VP Rx Statistics
 */
struct ppe_vp_rx_stats {
	atomic64_t rx_pkts;			/* Total rx packets */
	atomic64_t rx_bytes;			/* Total rx bytes */
	atomic64_t rx_errors;			/* Total rx errors */
	atomic64_t rx_drops;			/* Total rx drops */
	struct u64_stats_sync syncp;		/* Stats sync status */
};

/*
 * ppe_vp_tx_stats
 *	PPE VP Tx Statistics
 */
struct ppe_vp_tx_stats {
	atomic64_t tx_pkts;			/* Total rx packets */
	atomic64_t tx_bytes;			/* Total rx bytes */
	atomic64_t tx_errors;			/* Total rx errors */
	atomic64_t tx_drops;			/* Total rx drops */
	struct u64_stats_sync syncp;		/* Stats sync status */
};

/*
 * ppe_vp_stats
 *	Structure for VP Per CPU stats
 */
struct ppe_vp_stats {
	struct ppe_vp_hw_stats vp_hw_stats;	/* HW port statistics */
	struct ppe_vp_misc_stats misc_stats;	/* Misc statistics */
	struct ppe_vp_rx_stats __percpu *rx_stats;
						/* VP Rx statistics */
	struct ppe_vp_tx_stats __percpu *tx_stats;
						/* VP Tx statistics */
};
