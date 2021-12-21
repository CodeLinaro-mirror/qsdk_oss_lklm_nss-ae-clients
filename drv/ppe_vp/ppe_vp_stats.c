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

#include <linux/debugfs.h>
#include "ppe_vp_base.h"

#define RX_STATS_COUNT		4
#define TX_STATS_COUNT		4

extern struct ppe_vp_base vp_base;

static const char *ppe_vp_stats_base_str[] = {
	"VP Allocation fails",			/* Total VP allocation failures */
	"VP Table full errors",			/* VP allocation fails due to table full */
	"MTU assign fails",			/* MTU assign fails */
	"MAC assign fails"			/* MAC assign fails */
};

static const char *ppe_vp_stats_rx_str[] = {
	"Rx packets",				/* Total rx packets */
	"Rx bytes",				/* Total rx bytes */
	"Rx errors",				/* Total rx errors */
	"Rx drops"				/* Total rx drops */
};

static const char *ppe_vp_stats_tx_str[] = {
	"Tx packets",				/* Total tx packets */
	"Tx bytes",				/* Total tx bytes */
	"Tx errors",				/* Total tx errors */
	"Tx drops"				/* Total tx drops */
};

/*
 * ppe_vp_stats_reset_per_cpu_stats()
 *	Reset VP's per CPU stats.
 */
static void ppe_vp_stats_reset_per_cpu_stats(struct ppe_vp_stats *vp_stats)
{
	struct ppe_vp_rx_stats *rx_pcpu_stats;
	struct ppe_vp_tx_stats *tx_pcpu_stats;
	int i;

	for_each_possible_cpu(i) {
		unsigned int start;

		rx_pcpu_stats = per_cpu_ptr(vp_stats->rx_stats, i);
		do {
			start = u64_stats_fetch_begin_irq(&rx_pcpu_stats->syncp);
			memset(rx_pcpu_stats, 0, sizeof(*rx_pcpu_stats));
		} while (u64_stats_fetch_retry_irq(&rx_pcpu_stats->syncp, start));

		tx_pcpu_stats = per_cpu_ptr(vp_stats->tx_stats, i);
		do {
			start = u64_stats_fetch_begin_irq(&tx_pcpu_stats->syncp);
			memset(tx_pcpu_stats, 0, sizeof(*tx_pcpu_stats));
		} while (u64_stats_fetch_retry_irq(&tx_pcpu_stats->syncp, start));
	}
}

static int ppe_vp_stats_show(struct seq_file *m, void __attribute__((unused))*p)
{
	struct ppe_vp_base *pvb = &vp_base;
	struct ppe_vp *vp;
	struct ppe_vp_base_stats *pvb_stats;
	uint64_t *stats_shadow;
	struct ppe_vp_stats *vp_stats;
	struct ppe_vp_rx_stats *rx_pcpu_stats, rx_stats;
	struct ppe_vp_tx_stats *tx_pcpu_stats, tx_stats;
	uint64_t rx_aggr[RX_STATS_COUNT], tx_aggr[TX_STATS_COUNT];
	uint32_t active_vp_counter = 0;
	int16_t idx;
	int i;

	/*
	 * Read the statistics from the main structure for
	 * 1. PPE-VP base, 2. PPE-VP
	 */
	pvb_stats = kmalloc(sizeof(struct ppe_vp_base), GFP_KERNEL);
	if (!pvb) {
		ppe_vp_warn("Failed to allocate memory for pvb\n");
		return -ENOMEM;
	}

	memcpy(pvb_stats, &pvb->base_stats, sizeof(struct ppe_vp_base_stats));

	/*
	 * Start displaying the Base VP stats
	 */
	seq_printf(m, "\n################ Virtual Port Statistics Start ################\n");
	seq_printf(m, "\nBase VP Statistics:\n");
	stats_shadow = (uint64_t *)pvb_stats;
	seq_printf(m, "\tActive VPs: %u\n", pvb->vp_table.active_vp);
	for (i = 0; i < sizeof(struct ppe_vp_base_stats) / sizeof(uint64_t); i++) {
		seq_printf(m, "\t[%s]:  %llu\n", ppe_vp_stats_base_str[i], stats_shadow[i]);
	}

	/*
	 * Start displaying all the active VP stats.
	 */
	active_vp_counter = ppe_vp_base_get_active_vp_count(pvb);
	seq_printf(m, "\nVP Statistics (Active Count = %u)\n", active_vp_counter);

	for (idx = 0; idx < active_vp_counter; idx++) {

		memset(rx_aggr, 0, sizeof(rx_aggr));
		memset(tx_aggr, 0, sizeof(tx_aggr));

		rcu_read_lock();
		vp = ppe_vp_base_get_vp_by_idx(idx);
		if (vp) {
			vp_stats = &vp->vp_stats;

			seq_printf(m, "\tVP Port: %u\n", vp_stats->misc_info.ppe_port_num);
			seq_printf(m, "\t\tNetdev if num: %u\n", vp_stats->misc_info.netdev_if_num);

			/*
			 * Active VP: Accumulate stats from all CPUs.
			 */
			for_each_possible_cpu(i) {

				unsigned int start;
				rx_pcpu_stats = per_cpu_ptr(vp_stats->rx_stats, i);

				do {
					start = u64_stats_fetch_begin_irq(&rx_pcpu_stats->syncp);
					memcpy(&rx_stats, rx_pcpu_stats, sizeof(*rx_pcpu_stats));
				} while (u64_stats_fetch_retry_irq(&rx_pcpu_stats->syncp, start));

				rx_aggr[0] += atomic64_read(&rx_stats.rx_pkts);
				rx_aggr[1] += atomic64_read(&rx_stats.rx_bytes);
				rx_aggr[2] += atomic64_read(&rx_stats.rx_errors);
				rx_aggr[3] += atomic64_read(&rx_stats.rx_drops);

				tx_pcpu_stats = per_cpu_ptr(vp_stats->tx_stats, i);

				do {
					start = u64_stats_fetch_begin_irq(&tx_pcpu_stats->syncp);
					memcpy(&tx_stats, tx_pcpu_stats, sizeof(*tx_pcpu_stats));
				} while (u64_stats_fetch_retry_irq(&tx_pcpu_stats->syncp, start));

				tx_aggr[0] += atomic64_read(&tx_stats.tx_pkts);
				tx_aggr[1] += atomic64_read(&tx_stats.tx_bytes);
				tx_aggr[2] += atomic64_read(&tx_stats.tx_errors);
				tx_aggr[3] += atomic64_read(&tx_stats.tx_drops);
			}

			seq_printf(m, "\n\t\tVP Rx Stats:\n");
			stats_shadow = (uint64_t *)rx_aggr;
			for (i = 0; i < RX_STATS_COUNT; i++) {
				seq_printf(m, "\t\t\t[%s]:  %llu\n", ppe_vp_stats_rx_str[i], stats_shadow[i]);
			}

			seq_printf(m, "\n\t\tVP Tx Stats:\n");
			stats_shadow = (uint64_t *)tx_aggr;
			for (i = 0; i < TX_STATS_COUNT; i++) {
				seq_printf(m, "\t\t\t[%s]:  %llu\n", ppe_vp_stats_tx_str[i], stats_shadow[i]);
			}

			seq_printf(m, "\n\t\tHW port counters\n");
			seq_printf(m, "\t\t\trx_pkts: %llu\n", atomic64_read(&vp_stats->vp_hw_stats.rx_pkts));
			seq_printf(m, "\t\t\trx_bytes: %llu\n", atomic64_read(&vp_stats->vp_hw_stats.rx_bytes));
			seq_printf(m, "\t\t\ttx_pkts: %llu\n", atomic64_read(&vp_stats->vp_hw_stats.tx_pkts));
			seq_printf(m, "\t\t\ttx_bytes: %llu\n\n", atomic64_read(&vp_stats->vp_hw_stats.tx_bytes));
		}

		rcu_read_unlock();
	}

	seq_printf(m, "\n################ Virtual Port Statistics End ################\n\n");

	kfree(pvb_stats);
	return 0;
}

/*
 * ppe_vp_stats_open()
 *	Read IPV4 stats
 */
static int ppe_vp_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, ppe_vp_stats_show, inode->i_private);
}

/*
 * ppe_vp_stats_ops
 *	File operations for PPE-VP (also base) stats
 */
static const struct file_operations ppe_vp_stats_ops = {
		.open = ppe_vp_stats_open,
		.read = seq_read,
		.llseek = seq_lseek,
		.release = seq_release,
};

/*
 * ppe_vp_stats_reset_vp_stats()
 *	Reset VP statistics.
 */
void ppe_vp_stats_reset_vp_stats(struct ppe_vp_stats *vp_stats)
{
	memset(vp_stats, 0, (sizeof(struct ppe_vp_hw_stats) + sizeof(struct ppe_vp_misc_info)));
	ppe_vp_stats_reset_per_cpu_stats(vp_stats);
}

/*
 * ppe_vp_stats_deinit()
 *	Free VP statistics.
 */
ppe_vp_status_t ppe_vp_stats_deinit(struct ppe_vp *vp)
{
	struct ppe_vp_stats *vp_stats = &vp->vp_stats;

	if (!vp_stats->rx_stats) {
		ppe_vp_warn("%px: Percpu Rx stats not allocated\n", vp);
		return PPE_VP_STATUS_FAILURE;
	}

	free_percpu(vp_stats->rx_stats);
	vp_stats->rx_stats = NULL;

	if (!vp_stats->tx_stats) {
		ppe_vp_warn("%px: Percpu Rx stats not allocated\n", vp);
		return PPE_VP_STATUS_FAILURE;
	}

	free_percpu(vp_stats->tx_stats);
	vp_stats->tx_stats = NULL;

	return PPE_VP_STATUS_SUCCESS;
}

/*
 * ppe_vp_stats_init()
 *	Initialize VP statistics.
 */
ppe_vp_status_t ppe_vp_stats_init(struct ppe_vp *vp)
{
	struct ppe_vp_stats *vp_stats = &vp->vp_stats;

	/*
	 * Allocate per-cpu stats memory
	 */
	vp_stats->rx_stats = netdev_alloc_pcpu_stats(struct ppe_vp_rx_stats);
	if (!vp_stats->rx_stats) {
		ppe_vp_warn("Percpu Rx stats alloc failed for VP %px\n", vp);
		return PPE_VP_STATUS_FAILURE;
	}

	vp_stats->tx_stats = netdev_alloc_pcpu_stats(struct ppe_vp_tx_stats);
	if (!vp_stats->tx_stats) {
		ppe_vp_warn("Percpu Tx stats alloc failed for VP %px\n", vp);
		free_percpu(vp_stats->rx_stats);
		vp_stats->rx_stats = NULL;
		return PPE_VP_STATUS_FAILURE;
	}

	return PPE_VP_STATUS_SUCCESS;
}

/*
 * ppe_vp_base_stats_init()
 *	Initialize VP base statistics.
 *
 * Note: No need to remove the file. The upper level directory removes this file
 * recursively.
 */
ppe_vp_status_t ppe_vp_base_stats_init(struct ppe_vp_base *pvb)
{
	struct dentry *vp_dentry;
	vp_dentry = debugfs_create_dir("ppe_vp", pvb->dentry);
	if (!debugfs_create_file("vp_stats", S_IRUGO, vp_dentry, pvb, &ppe_vp_stats_ops)) {
		ppe_vp_warn("%px: Failed to create debug entry for all VP stats\n", pvb);
		return PPE_VP_STATUS_FAILURE;
	}

	return PPE_VP_STATUS_SUCCESS;
}