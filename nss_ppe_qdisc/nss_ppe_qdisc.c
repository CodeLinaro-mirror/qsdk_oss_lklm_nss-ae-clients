/*
 **************************************************************************
 * Copyright (c) 2017, The Linux Foundation. All rights reserved.
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

#include "nss_ppe_qdisc.h"
#include "nss_ppe_fifo.h"
#include "nss_ppe_htb.h"
#include "nss_ppe_red.h"

/*
 * HW scaling factor
 *
 * TODO: Change the value on actual HW
 * Frequency of RUMI is 100 times slower.
 */
#define NSS_PPE_QDISC_HW_FREQ_SCALING	100

/*
 * Max Resources per port
 *
 * TODO: These macros need to be removed once
 * configuration is read from device tree.
 */
#define NSS_PPE_QDISC_L0_SP_MAX		4
#define NSS_PPE_QDISC_L0_CDRR_MAX	16
#define NSS_PPE_QDISC_L0_EDRR_MAX	16
#define NSS_PPE_QDISC_L1_SP_MAX		1
#define NSS_PPE_QDISC_L1_CDRR_MAX	4
#define NSS_PPE_QDISC_L1_EDRR_MAX	4
#define NSS_PPE_QDISC_UCAST_QUEUE_MAX	16
#define NSS_PPE_QDISC_MCAST_QUEUE_MAX	4


#define NSS_PPE_QDISC_CPU0_L0_SP_MAX	36
#define NSS_PPE_QDISC_CPU0_L0_CDRR_MAX	48
#define NSS_PPE_QDISC_CPU0_L0_EDRR_MAX	48
#define NSS_PPE_QDISC_CPU0_L1_SP_MAX	1
#define NSS_PPE_QDISC_CPU0_L1_CDRR_MAX	8
#define NSS_PPE_QDISC_CPU0_L1_EDRR_MAX	8
#define NSS_PPE_QDISC_CPU0_QUEUE_MAX	144
#define NSS_PPE_QDISC_CPU0_MCAST_QUEUE_MAX	272

#define NSS_PPE_QDISC_LOOPBACK_L0_SP_MAX	1
#define NSS_PPE_QDISC_LOOPBACK_L0_CDRR_MAX	16
#define NSS_PPE_QDISC_LOOPBACK_L0_EDRR_MAX	16
#define NSS_PPE_QDISC_LOOPBACK_QUEUE_MAX	16
#define NSS_PPE_QDISC_LOOPBACK_MCAST_QUEUE_MAX	1

#define NSS_PPE_QDISC_LOOPBACK_L0_SP_BASE	35
#define NSS_PPE_QDISC_LOOPBACK_L0_CDRR_BASE	32
#define NSS_PPE_QDISC_LOOPBACK_L0_EDRR_BASE	32
#define NSS_PPE_QDISC_LOOPBACK_QUEUE_BASE	128
#define NSS_PPE_QDISC_LOOPBACK_MCAST_QUEUE_BASE	256

#define NSS_PPE_QDISC_PRIORITY_MAX	7

#define NSS_PPE_QDISC_PORT_MAX		8

#define NSS_PPE_QDISC_DRR_WT_MAX	1024

static struct nss_ppe_qdisc_port ppe_qdisc_port[NSS_PPE_QDISC_PORT_MAX];

/*
 * nss_ppe_qdisc_loobback_l1_conf_set()
 *	Sets default L1 queue scheduler in SSDK for port 0.
 */
static int nss_ppe_qdisc_loobback_l1_conf_set(void)
{
	fal_qos_scheduler_cfg_t l1cfg;

	/*
	 * Set Level 1 Configuration
	 */
	memset(&l1cfg, 0, sizeof(l1cfg));
	l1cfg.sp_id = 0;
	l1cfg.c_pri = 0;
	l1cfg.e_pri = 0;
	l1cfg.c_drr_id = 0;
	l1cfg.e_drr_id = 0;
	l1cfg.c_drr_wt = 1;
	l1cfg.e_drr_wt = 1;

	nss_ppe_qdisc_trace("SSDK level1 configuration: Port:0, l0spid:%d, c_drrid:%d, c_pri:%d, c_drr_wt:%d, e_drrid:%d, e_pri:%d, e_drr_wt:%d, l1spid:%d\n",
			NSS_PPE_QDISC_LOOPBACK_L0_SP_BASE, l1cfg.c_drr_id, l1cfg.c_pri, l1cfg.c_drr_wt, l1cfg.e_drr_id, l1cfg.e_pri, l1cfg.e_drr_wt, l1cfg.sp_id);
	if (fal_queue_scheduler_set(0, NSS_PPE_QDISC_LOOPBACK_L0_SP_BASE, NSS_PPE_QDISC_FLOW_LEVEL - 1, 0, &l1cfg) != 0) {
		nss_ppe_qdisc_error("SSDK level1 queue scheduler configuration failed\n");
		return -EINVAL;
	}

	nss_ppe_qdisc_info("SSDK L1 configuration successful\n");
	return 0;
}

/*
 * nss_ppe_qdisc_res_entries_free()
 *	Free all the resource for a given port.
 */
static void nss_ppe_qdisc_res_entries_free(uint32_t port, nss_ppe_qdisc_res_type_t type)
{
	struct nss_ppe_qdisc_res *res;
	struct nss_ppe_qdisc_res *head = NULL;
	struct nss_ppe_qdisc_port *ppe_port = &ppe_qdisc_port[port];

	spin_lock_bh(&ppe_port->lock);
	head = ppe_port->res_free[type];
	while (head) {
		res = head;
		head = head->next;
		kfree(res);
	}

	ppe_port->res_free[type] = NULL;
	spin_unlock_bh(&ppe_port->lock);

	nss_ppe_qdisc_info("port:%d, type:%d\n", port, type);
}

/*
 * nss_ppe_qdisc_res_entries_alloc()
 *	Allocates all the resources for a given port.
 */
static struct nss_ppe_qdisc_res *nss_ppe_qdisc_res_entries_alloc(uint32_t port, nss_ppe_qdisc_res_type_t type)
{
	struct nss_ppe_qdisc_res *res = NULL;
	struct nss_ppe_qdisc_res *next = NULL;
	uint32_t i;
	struct nss_ppe_qdisc_port *ppe_port = &ppe_qdisc_port[port];
	uint32_t max = ppe_port->max[type];

	spin_lock_bh(&ppe_port->lock);
	for (i = max; i > 0; i--) {
		res = kzalloc(sizeof(struct nss_ppe_qdisc_res), GFP_KERNEL);
		if (!res) {
			nss_ppe_qdisc_error("Free queue list allocation failed for port %u\n", port);
			goto fail;
		}

		res->offset = i - 1;
		res->type = type;
		res->next = next;
		next = res;

	}
	spin_unlock_bh(&ppe_port->lock);

	nss_ppe_qdisc_trace("port:%d, type:%d\n", port, type);
	return res;

fail:
	while (next) {
		res = next;
		next = next->next;
		kfree(res);
	}
	spin_unlock_bh(&ppe_port->lock);
	return NULL;
}

/*
 * nss_ppe_qdisc_port_res_free()
 *	Free resources allocated to PPE ports
 */
static int nss_ppe_qdisc_port_res_free(void)
{
	uint32_t i, j;
	nss_ppe_qdisc_info("nss_ppe_qdisc_port_res_free");

	for (i = 0; i < NSS_PPE_QDISC_PORT_MAX - 1; i++) {
		for (j = 0; j < NSS_PPE_QDISC_MAX_RES_TYPE; j++) {
			nss_ppe_qdisc_res_entries_free(i, j);
		}
	}

	return 0;
}

/*
 * nss_ppe_qdisc_port_res_alloc()
 *	Allocates per port resources
 *
 *	TODO: Replace the MAX with the configuration from device tree.
 */
static int nss_ppe_qdisc_port_res_alloc(void)
{
	int j, type;
	int i = 0;
	nss_ppe_qdisc_info("nss_ppe_qdisc_port_res_alloc");

	memset(&ppe_qdisc_port, 0, sizeof(struct nss_ppe_qdisc_port) * NSS_PPE_QDISC_PORT_MAX);

	nss_ppe_qdisc_info("Resource allocation for loopback port %u\n", i);

	/*
	 * Initialize lock
	 */
	spin_lock_init(&ppe_qdisc_port[i].lock);

	/*
	 * By default, queue configuration is enabled
	 */
	ppe_qdisc_port[i].def_conf_enable = true;

	/*
	 * Loopback port configuration
	 * Loopback port requires only L0 resources.
	 */
	ppe_qdisc_port[i].max[NSS_PPE_QDISC_UCAST_QUEUE] = NSS_PPE_QDISC_LOOPBACK_QUEUE_MAX;
	ppe_qdisc_port[i].base[NSS_PPE_QDISC_UCAST_QUEUE] = NSS_PPE_QDISC_LOOPBACK_QUEUE_BASE;

	ppe_qdisc_port[i].max[NSS_PPE_QDISC_MCAST_QUEUE] = NSS_PPE_QDISC_LOOPBACK_MCAST_QUEUE_MAX;
	ppe_qdisc_port[i].base[NSS_PPE_QDISC_MCAST_QUEUE] = NSS_PPE_QDISC_LOOPBACK_MCAST_QUEUE_BASE;

	ppe_qdisc_port[i].max[NSS_PPE_QDISC_L0_CDRR] = NSS_PPE_QDISC_LOOPBACK_L0_CDRR_MAX;
	ppe_qdisc_port[i].base[NSS_PPE_QDISC_L0_CDRR] = NSS_PPE_QDISC_LOOPBACK_L0_CDRR_BASE;

	ppe_qdisc_port[i].max[NSS_PPE_QDISC_L0_EDRR] = NSS_PPE_QDISC_LOOPBACK_L0_EDRR_MAX;
	ppe_qdisc_port[i].base[NSS_PPE_QDISC_L0_EDRR] = NSS_PPE_QDISC_LOOPBACK_L0_EDRR_BASE;

	for (type = 0; type < NSS_PPE_QDISC_L0_SP; type++) {
		ppe_qdisc_port[i].res_free[type] = nss_ppe_qdisc_res_entries_alloc(i, type);
		if (!ppe_qdisc_port[i].res_free[type]) {
			nss_ppe_qdisc_error("Resource list allocation failed for port:%u type:%u\n", i, type);
			goto failure;
		}
	}

	/*
	 * Allocate resources for GMAC switch ports.
	 */
	for (i = 1; i < NSS_PPE_QDISC_PORT_MAX - 1; i++) {
		nss_ppe_qdisc_info("Resource allocation for port %u\n", i);

		/*
		 * Initialize locks
		 */
		spin_lock_init(&ppe_qdisc_port[i].lock);

		/*
		 * By default, queue configuration is enabled
		 */
		ppe_qdisc_port[i].def_conf_enable = true;

		/*
		 * Resource configuration
		 */
		ppe_qdisc_port[i].max[NSS_PPE_QDISC_UCAST_QUEUE] = NSS_PPE_QDISC_UCAST_QUEUE_MAX;
		ppe_qdisc_port[i].base[NSS_PPE_QDISC_UCAST_QUEUE] = NSS_PPE_QDISC_CPU0_QUEUE_MAX + (i - 1) * NSS_PPE_QDISC_UCAST_QUEUE_MAX;

		ppe_qdisc_port[i].max[NSS_PPE_QDISC_MCAST_QUEUE] = NSS_PPE_QDISC_MCAST_QUEUE_MAX;
		ppe_qdisc_port[i].base[NSS_PPE_QDISC_MCAST_QUEUE] = NSS_PPE_QDISC_CPU0_MCAST_QUEUE_MAX + (i - 1) * NSS_PPE_QDISC_MCAST_QUEUE_MAX;

		ppe_qdisc_port[i].max[NSS_PPE_QDISC_L0_CDRR] = NSS_PPE_QDISC_L0_CDRR_MAX;
		ppe_qdisc_port[i].base[NSS_PPE_QDISC_L0_CDRR] = NSS_PPE_QDISC_CPU0_L0_CDRR_MAX + (i - 1) * NSS_PPE_QDISC_L0_CDRR_MAX;

		ppe_qdisc_port[i].max[NSS_PPE_QDISC_L0_EDRR] = NSS_PPE_QDISC_L0_EDRR_MAX;
		ppe_qdisc_port[i].base[NSS_PPE_QDISC_L0_EDRR] = NSS_PPE_QDISC_CPU0_L0_EDRR_MAX + (i - 1) * NSS_PPE_QDISC_L0_EDRR_MAX;

		ppe_qdisc_port[i].max[NSS_PPE_QDISC_L0_SP] = NSS_PPE_QDISC_L0_SP_MAX;
		ppe_qdisc_port[i].base[NSS_PPE_QDISC_L0_SP] = NSS_PPE_QDISC_CPU0_L0_SP_MAX + (i - 1) * NSS_PPE_QDISC_L0_SP_MAX;

		ppe_qdisc_port[i].max[NSS_PPE_QDISC_L1_CDRR] = NSS_PPE_QDISC_L1_CDRR_MAX;
		ppe_qdisc_port[i].base[NSS_PPE_QDISC_L1_CDRR] = NSS_PPE_QDISC_CPU0_L1_CDRR_MAX + (i - 1) * NSS_PPE_QDISC_L1_CDRR_MAX;

		ppe_qdisc_port[i].max[NSS_PPE_QDISC_L1_EDRR] = NSS_PPE_QDISC_L1_EDRR_MAX;
		ppe_qdisc_port[i].base[NSS_PPE_QDISC_L1_EDRR] = NSS_PPE_QDISC_CPU0_L1_EDRR_MAX + (i - 1) * NSS_PPE_QDISC_L1_EDRR_MAX;

		for (type = 0; type < NSS_PPE_QDISC_MAX_RES_TYPE; type++) {
			ppe_qdisc_port[i].res_free[type] = nss_ppe_qdisc_res_entries_alloc(i, type);
			if (!ppe_qdisc_port[i].res_free[type]) {
				nss_ppe_qdisc_error("Resource list allocation failed for port:%u type:%u\n", i, type);
				goto failure;
			}
		}
	}

	/*
	 * Set default level 1 configuration for loopback port.
	 */
	if (nss_ppe_qdisc_loobback_l1_conf_set() != 0) {
		nss_ppe_qdisc_error("Failed default level 1 configuration for brige port\n");
		goto failure;
	}
	return 0;

failure:
	/*
	 * In case we have successfully allocated resources for all the ports
	 * and failure occurs while setting default level 1 configuration
	 * for loobpack port, then decrement the counter to deallocate resources.
	 */
	if (i == (NSS_PPE_QDISC_PORT_MAX - 1)) {
		i--;
	}

	while (i >= 0) {
		for (j = 0; j < NSS_PPE_QDISC_MAX_RES_TYPE; j++) {
			nss_ppe_qdisc_res_entries_free(i, j);
		}
		i--;
	}
	return -EINVAL;
}

/*
 * nss_ppe_qdisc_def_conf_disable()
 *	Disables the default queue if default queue configuration status is enabled.
 */
static void nss_ppe_qdisc_def_conf_disable(uint32_t port)
{
	bool status;
	struct nss_ppe_qdisc_port *ppe_port = &ppe_qdisc_port[port];
	uint32_t ucast_qid = nss_ppe_qdisc_base_get(port, NSS_PPE_QDISC_UCAST_QUEUE);
	uint32_t mcast_qid = nss_ppe_qdisc_base_get(port, NSS_PPE_QDISC_MCAST_QUEUE);

	/*
	 * Get the default queue configuration status.
	 */
	spin_lock_bh(&ppe_port->lock);
	status = ppe_port->def_conf_enable;
	spin_unlock_bh(&ppe_port->lock);

	/*
	 * If default configuration is enabled,
	 * disable queue enqueue, dequeue and flush the queue.
	 */
 	if (status) {
		fal_qm_enqueue_ctrl_set(0, ucast_qid, false);
		fal_scheduler_dequeue_ctrl_set(0, ucast_qid, false);
		fal_queue_flush(0, port, ucast_qid);

		fal_qm_enqueue_ctrl_set(0, mcast_qid, false);
		fal_scheduler_dequeue_ctrl_set(0, mcast_qid, false);
		fal_queue_flush(0, port, mcast_qid);

		/*
		 * Reset the default queue configuration status.
		 */
		spin_lock_bh(&ppe_port->lock);
		ppe_port->def_conf_enable = false;
		spin_unlock_bh(&ppe_port->lock);
	}

	nss_ppe_qdisc_info("SSDK default queue status disabled\n");
}

/*
 * nss_ppe_qdisc_queue_scheduler_disable()
 *	Disables level 0 queue scheduler in SSDK.
 */
static void nss_ppe_qdisc_queue_scheduler_disable(struct nss_ppe_qdisc *npq)
{
	uint32_t port_num = nss_ppe_qdisc_port_num_get(npq);

	/*
	 * Disable queue enqueue, dequeue and flush the queue.
	 */
	fal_qm_enqueue_ctrl_set(0, npq->q.ucast_qid, false);
	fal_scheduler_dequeue_ctrl_set(0, npq->q.ucast_qid, false);
	fal_queue_flush(0, port_num, npq->q.ucast_qid);

	nss_ppe_qdisc_info("Disable SSDK level0 queue scheduler successful\n");
}

/*
 * nss_ppe_qdisc_queue_scheduler_enable()
 *	Enables level 0 queue scheduler in SSDK.
 */
static void nss_ppe_qdisc_queue_scheduler_enable(struct nss_ppe_qdisc *npq)
{
	/*
	 * Enable queue enqueue and dequeue.
	 */
	fal_qm_enqueue_ctrl_set(0, npq->q.ucast_qid, true);
	fal_scheduler_dequeue_ctrl_set(0, npq->q.ucast_qid, true);

	nss_ppe_qdisc_info("Enable SSDK level0 queue scheduler successful\n");
}

/*
 * nss_ppe_qdisc_l1_res_free()
 *	Frees Level 1 scheduler resources.
 */
static int nss_ppe_qdisc_l1_res_free(struct nss_ppe_qdisc *npq)
{
	uint32_t offset;
	uint32_t port_num = nss_ppe_qdisc_port_num_get(npq);

	/*
	 * Bridge interface will have one level less than the max shaper levels.
	 * L1 scheduler was configured at init time, so resources were allocated.
	 */
	if (npq->nq.is_bridge) {
		npq->l0spid = 0;
		return 0;
	}

	/*
	 * If l1_valid is not set, then no resources to free.
	 */
	if (!npq->l1_valid) {
		return 0;
	}

	/*
	 * Free Level 1 DRR resource
	 */
	offset = npq->l1c_drrid - nss_ppe_qdisc_base_get(port_num, NSS_PPE_QDISC_L1_CDRR);
	if (nss_ppe_qdisc_res_free(port_num, offset, NSS_PPE_QDISC_L1_CDRR) != 0) {
		nss_ppe_qdisc_error("Used res:%d not found for port:%d, type:%d \n", npq->l1c_drrid, port_num, NSS_PPE_QDISC_L1_CDRR);
		return -EINVAL;
	}

	offset = npq->l1e_drrid - nss_ppe_qdisc_base_get(port_num, NSS_PPE_QDISC_L1_EDRR);
	if (nss_ppe_qdisc_res_free(port_num, offset, NSS_PPE_QDISC_L1_EDRR) != 0) {
		nss_ppe_qdisc_error("Used res:%d not found for port:%d, type:%d \n", npq->l1e_drrid, port_num, NSS_PPE_QDISC_L1_EDRR);
		return -EINVAL;
	}

	/*
	 * Free Level 0 SP resource
	 */
	offset = npq->l0spid - nss_ppe_qdisc_base_get(port_num, NSS_PPE_QDISC_L0_SP);
	if (nss_ppe_qdisc_res_free(port_num, offset, NSS_PPE_QDISC_L0_SP) != 0) {
		nss_ppe_qdisc_error("Used res:%d not found for port:%d, type:%d \n", npq->l0spid, port_num, NSS_PPE_QDISC_L0_SP);
		return -EINVAL;
	}

	/*
	 * Reset Res id values in qdisc
	 */
	npq->l0spid = 0;
	npq->l1c_drrid = 0;
	npq->l1e_drrid = 0;
	npq->l1_valid = false;

	nss_ppe_qdisc_info("SSDK level1 queue scheduler configuration successful\n");
	return 0;
}

/*
 * nss_ppe_qdisc_l1_res_alloc()
 *	Allocates Level 1 scheduler resources
 */
static int nss_ppe_qdisc_l1_res_alloc(struct nss_ppe_qdisc *npq)
{
	struct nss_ppe_qdisc_res *l1c_drr = NULL;
	struct nss_ppe_qdisc_res *l1e_drr = NULL;
	struct nss_ppe_qdisc_res *l0sp = NULL;
	uint32_t port_num = nss_ppe_qdisc_port_num_get(npq);

	/*
	 * Get Level 1 DRR resource
	 */
	l1c_drr = nss_ppe_qdisc_res_alloc(port_num, NSS_PPE_QDISC_L1_CDRR);
	if (!l1c_drr) {
		nss_ppe_qdisc_warning("Free res not found for port:%d, type:%d \n", port_num, NSS_PPE_QDISC_L1_CDRR);
		goto fail;
	}

	l1e_drr = nss_ppe_qdisc_res_alloc(port_num, NSS_PPE_QDISC_L1_EDRR);
	if (!l1e_drr) {
		nss_ppe_qdisc_warning("Free res not found for port:%d, type:%d \n", port_num, NSS_PPE_QDISC_L1_EDRR);
		goto fail;
	}

	/*
	 * Get Level 0 SP resource
	 */
	l0sp = nss_ppe_qdisc_res_alloc(port_num, NSS_PPE_QDISC_L0_SP);
	if (!l0sp) {
		nss_ppe_qdisc_warning("Free res not found for port:%d, type:%d \n", port_num, NSS_PPE_QDISC_L0_SP);
		goto fail;
	}

	/*
	 * Set Res id values in qdisc
	 */
	npq->q.ucast_qid = 0;
	npq->q.mcast_qid = 0;
	npq->q.ucast_valid = false;
	npq->l1_valid = true;
	npq->l0c_drrid = 0;
	npq->l0e_drrid = 0;
	npq->l0spid = nss_ppe_qdisc_base_get(port_num, NSS_PPE_QDISC_L0_SP) + l0sp->offset;
	npq->l1c_drrid = nss_ppe_qdisc_base_get(port_num, NSS_PPE_QDISC_L1_CDRR) + l1c_drr->offset;
	npq->l1e_drrid = nss_ppe_qdisc_base_get(port_num, NSS_PPE_QDISC_L1_EDRR) + l1e_drr->offset;

	nss_ppe_qdisc_info("Level1 scheduler resource allocation successful\n");
	return 0;

fail:
	if (l0sp) {
		nss_ppe_qdisc_res_free(port_num, l0sp->offset, NSS_PPE_QDISC_L0_SP);
	}

	if (l1e_drr) {
		nss_ppe_qdisc_res_free(port_num, l1e_drr->offset, NSS_PPE_QDISC_L1_EDRR);
	}

	if (l1c_drr) {
		nss_ppe_qdisc_res_free(port_num, l1c_drr->offset, NSS_PPE_QDISC_L1_CDRR);
	}
	return -EINVAL;
}

/*
 * nss_ppe_qdisc_l1_queue_scheduler_configure()
 *	Configures Level 1 queue scheduler in SSDK.
 */
static int nss_ppe_qdisc_l1_queue_scheduler_configure(struct nss_ppe_qdisc *npq)
{
	fal_qos_scheduler_cfg_t l1cfg;
	uint32_t port_num = nss_ppe_qdisc_port_num_get(npq);

	/*
	 * Set Level 1 configuration
	 */
	memset(&l1cfg, 0, sizeof(l1cfg));
	l1cfg.sp_id = port_num;

	l1cfg.c_drr_wt = (npq->scheduler.quantum < NSS_PPE_QDISC_DRR_WT_MAX) ? 1 : (npq->scheduler.quantum / NSS_PPE_QDISC_DRR_WT_MAX);
	l1cfg.e_drr_wt = (npq->scheduler.quantum < NSS_PPE_QDISC_DRR_WT_MAX) ? 1 : (npq->scheduler.quantum / NSS_PPE_QDISC_DRR_WT_MAX);
	l1cfg.c_pri = NSS_PPE_QDISC_PRIORITY_MAX - npq->scheduler.priority;
	l1cfg.e_pri = NSS_PPE_QDISC_PRIORITY_MAX - npq->scheduler.priority;
	l1cfg.c_drr_id = npq->l1c_drrid;
	l1cfg.e_drr_id = npq->l1e_drrid;

	nss_ppe_qdisc_trace("SSDK level1 configuration: Port:%d, l0spid:%d, c_drrid:%d, c_pri:%d, c_drr_wt:%d, e_drrid:%d, e_pri:%d, e_drr_wt:%d, l1spid:%d\n",
			port_num, npq->l0spid, l1cfg.c_drr_id, l1cfg.c_pri, l1cfg.c_drr_wt, l1cfg.e_drr_id, l1cfg.e_pri, l1cfg.e_drr_wt, l1cfg.sp_id);
	if (fal_queue_scheduler_set(0, npq->l0spid, NSS_PPE_QDISC_FLOW_LEVEL - 1, port_num, &l1cfg) != 0) {
		nss_ppe_qdisc_error("SSDK level1 queue scheduler configuration failed\n");
		return -EINVAL;
	}

	nss_ppe_qdisc_info("SSDK level1 queue scheduler configuration successful\n");
	return 0;
}

/*
 * nss_ppe_qdisc_l1_queue_scheduler_set()
 *	Configures Level 1 queue scheduler in SSDK.
 */
static int nss_ppe_qdisc_l1_queue_scheduler_set(struct nss_ppe_qdisc *npq)
{
	/*
	 * Bridge interface will have one level less than the max shaper levels.
	 * L1 scheduler was configured at init time, so no need to allocate resources.
	 */
	if (npq->nq.is_bridge) {
		/*
		 * Set Res id values in qdisc
		 */
		npq->l0spid = NSS_PPE_QDISC_LOOPBACK_L0_SP_BASE;

		nss_ppe_qdisc_info("SSDK level1 queue scheduler configuration successful\n");
		return 0;
	}

	/*
	 * Allocate new resources only if scheduler is not valid.
	 */
	if (!npq->l1_valid) {
		if (nss_ppe_qdisc_l1_res_alloc(npq) != 0) {
			nss_ppe_qdisc_warning("SSDK level0 queue scheduler configuration failed\n");
			return -EINVAL;
		}
	}

	/*
	 * Set Level 1 configuration
	 */
	if (nss_ppe_qdisc_l1_queue_scheduler_configure(npq) != 0) {
		nss_ppe_qdisc_error("SSDK level1 queue scheduler configuration failed\n");
		nss_ppe_qdisc_l1_res_free(npq);
		return -EINVAL;
	}

	nss_ppe_qdisc_info("SSDK level1 queue scheduler configuration successful\n");
	return 0;
}

/*
 * nss_ppe_qdisc_l0_res_free()
 *	Frees level0 scheduler resources.
 */
static int nss_ppe_qdisc_l0_res_free(struct nss_ppe_qdisc *npq)
{
	uint32_t offset;
	uint32_t port_num = nss_ppe_qdisc_port_num_get(npq);

	/*
	 * If l0_valid is not set, then no resources to free.
	 */
	if (!npq->l0_valid) {
		return 0;
	}

	/*
	 * Free Level 0 DRR resource
	 */
	offset = npq->l0c_drrid - nss_ppe_qdisc_base_get(port_num, NSS_PPE_QDISC_L0_CDRR);
	if (nss_ppe_qdisc_res_free(port_num, offset, NSS_PPE_QDISC_L0_CDRR) != 0) {
		nss_ppe_qdisc_error("Used res:%d not found for port:%d, type:%d \n", npq->l0c_drrid, port_num, NSS_PPE_QDISC_L0_CDRR);
		return -EINVAL;
	}

	offset = npq->l0e_drrid - nss_ppe_qdisc_base_get(port_num, NSS_PPE_QDISC_L0_EDRR);
	if (nss_ppe_qdisc_res_free(port_num, offset, NSS_PPE_QDISC_L0_EDRR) != 0) {
		nss_ppe_qdisc_error("Used res:%d not found for port:%d, type:%d \n", npq->l0e_drrid, port_num, NSS_PPE_QDISC_L0_EDRR);
		return -EINVAL;
	}

	/*
	 * Free Level 0 queue resource
	 */
	offset = npq->q.ucast_qid - nss_ppe_qdisc_base_get(port_num, NSS_PPE_QDISC_UCAST_QUEUE);
	if (nss_ppe_qdisc_res_free(port_num, offset, NSS_PPE_QDISC_UCAST_QUEUE) != 0) {
		nss_ppe_qdisc_error("Used res:%d not found for port:%d, type:%d \n", npq->q.ucast_qid, port_num, NSS_PPE_QDISC_UCAST_QUEUE);
		return -EINVAL;
	}


	/*
	 * Reset Res id values in qdisc
	 */
	npq->q.ucast_qid = 0;
	npq->q.mcast_qid = 0;
	npq->q.mcast_valid = false;
	npq->q.ucast_valid = false;
	npq->l0c_drrid = 0;
	npq->l0e_drrid = 0;
	npq->l0_valid = false;

	nss_ppe_qdisc_info("Level0 scheduler resource de-allocation successful\n");
	return 0;
}

/*
 * nss_ppe_qdisc_l0_queue_scheduler_deconfigure()
 *	De-configures Level 0 queue scheduler configuration in SSDK.
 */
static int nss_ppe_qdisc_l0_queue_scheduler_deconfigure(struct nss_ppe_qdisc *npq)
{
	fal_qos_scheduler_cfg_t l0cfg;
	uint32_t port_num = nss_ppe_qdisc_port_num_get(npq);

	nss_ppe_qdisc_queue_scheduler_disable(npq);

	/*
	 * Reset Level 0 configuration
	 */
	memset(&l0cfg, 0, sizeof(l0cfg));
	l0cfg.sp_id = npq->l0spid;
	l0cfg.c_drr_wt = 0;
	l0cfg.e_drr_wt = 0;
	l0cfg.c_drr_id = npq->l0c_drrid;
	l0cfg.e_drr_id = npq->l0e_drrid;

	nss_ppe_qdisc_trace("SSDK level0 configuration: Port:%d, ucast_qid:%d, c_drrid:%d, c_pri:%d, c_drr_wt:%d, e_drrid:%d, e_pri:%d, e_drr_wt:%d, l0spid:%d\n",
			port_num, npq->q.ucast_qid, l0cfg.c_drr_id, l0cfg.c_pri, l0cfg.c_drr_wt, l0cfg.e_drr_id, l0cfg.e_pri, l0cfg.e_drr_wt, l0cfg.sp_id);
	if (fal_queue_scheduler_set(0, npq->q.ucast_qid, NSS_PPE_QDISC_QUEUE_LEVEL - 1, port_num, &l0cfg) != 0) {
		nss_ppe_qdisc_error("SSDK level0 queue scheduler configuration failed\n");
		return -EINVAL;
	}

	nss_ppe_qdisc_info("SSDK level0 queue scheduler configuration successful\n");
	return 0;
}

/*
 * nss_ppe_qdisc_l0_queue_scheduler_reset()
 *	Resets Level 0 queue scheduler configuration in SSDK.
 */
static int nss_ppe_qdisc_l0_queue_scheduler_reset(struct nss_ppe_qdisc *npq)
{
	if (nss_ppe_qdisc_l0_queue_scheduler_deconfigure(npq) != 0) {
		nss_ppe_qdisc_error("SSDK level0 queue scheduler configuration failed\n");
		return -EINVAL;
	}

	if (nss_ppe_qdisc_l0_res_free(npq) != 0) {
		nss_ppe_qdisc_error("Level0 scheduler resources de-allocation failed\n");
		return -EINVAL;
	}

	nss_ppe_qdisc_info("SSDK level0 queue scheduler configuration successful\n");
	return 0;
}

/*
 * nss_ppe_qdisc_l0_res_alloc()
 *	Allocates level 0 resources.
 */
static int nss_ppe_qdisc_l0_res_alloc(struct nss_ppe_qdisc *npq)
{
	struct nss_ppe_qdisc_res *l0c_drr = NULL;
	struct nss_ppe_qdisc_res *l0e_drr = NULL;
	struct nss_ppe_qdisc_res *q = NULL;
	uint32_t port_num = nss_ppe_qdisc_port_num_get(npq);

	/*
	 * Get Level 0 DRR Resource
	 */
	l0c_drr = nss_ppe_qdisc_res_alloc(port_num, NSS_PPE_QDISC_L0_CDRR);
	if (!l0c_drr) {
		nss_ppe_qdisc_warning("Free res not found for port:%d, type:%d \n", port_num, NSS_PPE_QDISC_L0_CDRR);
		goto fail;
	}

	l0e_drr = nss_ppe_qdisc_res_alloc(port_num, NSS_PPE_QDISC_L0_EDRR);
	if (!l0e_drr) {
		nss_ppe_qdisc_warning("Free res not found for port:%d, type:%d \n", port_num, NSS_PPE_QDISC_L0_EDRR);
		goto fail;
	}

	/*
	 * Get Level 0 queue Resource
	 */
	q = nss_ppe_qdisc_res_alloc(port_num, NSS_PPE_QDISC_UCAST_QUEUE);
	if (!q) {
		nss_ppe_qdisc_warning("Free res not found for port:%d, type:%d \n", port_num, NSS_PPE_QDISC_UCAST_QUEUE);
		goto fail;
	}

	/*
	 * Set res id values in qdisc
	 */
	npq->q.ucast_qid = nss_ppe_qdisc_base_get(port_num, NSS_PPE_QDISC_UCAST_QUEUE) + q->offset;
	npq->q.ucast_valid = true;
	npq->l0_valid = true;
	npq->l0c_drrid = nss_ppe_qdisc_base_get(port_num, NSS_PPE_QDISC_L0_CDRR) + l0c_drr->offset;
	npq->l0e_drrid = nss_ppe_qdisc_base_get(port_num, NSS_PPE_QDISC_L0_EDRR) + l0e_drr->offset;
	npq->q.mcast_qid = 0;

	if (npq->parent) {
		npq->l0spid = npq->parent->l0spid;
	}

	nss_ppe_qdisc_info("Level0 scheduler resource allocation successful\n");
	return 0;

fail:
	if (q) {
		nss_ppe_qdisc_res_free(port_num, q->offset, NSS_PPE_QDISC_UCAST_QUEUE);
	}

	if (l0c_drr) {
		nss_ppe_qdisc_res_free(port_num, l0c_drr->offset, NSS_PPE_QDISC_L0_CDRR);
	}

	if (l0e_drr) {
		nss_ppe_qdisc_res_free(port_num, l0e_drr->offset, NSS_PPE_QDISC_L0_EDRR);
	}
	return -EINVAL;
}

/*
 * nss_ppe_qdisc_l0_queue_scheduler_configure()
 *	Configures a level 0 queue scheduler in SSDK.
 */
static int nss_ppe_qdisc_l0_queue_scheduler_configure(struct nss_ppe_qdisc *npq)
{
	fal_qos_scheduler_cfg_t l0cfg;
	uint32_t port_num = nss_ppe_qdisc_port_num_get(npq);

	/*
	 * Set Level 0 configuration
	 */
	memset(&l0cfg, 0, sizeof(l0cfg));
	l0cfg.sp_id = npq->l0spid;
	l0cfg.c_drr_wt = (npq->scheduler.quantum < NSS_PPE_QDISC_DRR_WT_MAX) ? 1 : (npq->scheduler.quantum / NSS_PPE_QDISC_DRR_WT_MAX);
	l0cfg.e_drr_wt = (npq->scheduler.quantum < NSS_PPE_QDISC_DRR_WT_MAX) ? 1 : (npq->scheduler.quantum / NSS_PPE_QDISC_DRR_WT_MAX);
	l0cfg.c_pri = NSS_PPE_QDISC_PRIORITY_MAX - npq->scheduler.priority;
	l0cfg.e_pri = NSS_PPE_QDISC_PRIORITY_MAX - npq->scheduler.priority;
	l0cfg.c_drr_id = npq->l0c_drrid;
	l0cfg.e_drr_id = npq->l0e_drrid;

	nss_ppe_qdisc_trace("SSDK level0 configuration: Port:%d, ucast_qid:%d, c_drrid:%d, c_pri:%d, c_drr_wt:%d, e_drrid:%d, e_pri:%d, e_drr_wt:%d, l0spid:%d\n",
			port_num, npq->q.ucast_qid, l0cfg.c_drr_id, l0cfg.c_pri, l0cfg.c_drr_wt, l0cfg.e_drr_id, l0cfg.e_pri, l0cfg.e_drr_wt, l0cfg.sp_id);
	if (fal_queue_scheduler_set(0, npq->q.ucast_qid, NSS_PPE_QDISC_QUEUE_LEVEL - 1, port_num, &l0cfg) != 0) {
		nss_ppe_qdisc_error("SSDK level0 queue scheduler configuration failed\n");
		return -EINVAL;
	}

	/*
	 * This function is called at the time of queue qdisc allocation at all
	 * levels and class allocation at NSS_PPE_QDISC_QUEUE_LEVEL.
	 * Also invoked at time of change class at NSS_PPE_QDISC_QUEUE_LEVEL and
	 * NSS_PPE_QDISC_SUB_QUEUE_LEVEL levels. But, mcast will be valid only in
	 * case queue is attached at NSS_PPE_QDISC_SUB_QUEUE_LEVEL and change class
	 * is invoked at NSS_PPE_QDISC_QUEUE_LEVEL level.
	 * Other scenarios catering to mcast queue are handled in separate mcast APIs.
	 */
	if (npq->q.mcast_valid) {
		nss_ppe_qdisc_trace("SSDK level0 configuration: Port:%d, mcast_qid:%d, c_drrid:%d, c_pri:%d, c_drr_wt:%d, e_drrid:%d, e_pri:%d, e_drr_wt:%d, l0spid:%d\n",
				port_num, npq->q.mcast_qid, l0cfg.c_drr_id, l0cfg.c_pri, l0cfg.c_drr_wt, l0cfg.e_drr_id, l0cfg.e_pri, l0cfg.e_drr_wt, l0cfg.sp_id);
		if (fal_queue_scheduler_set(0, npq->q.mcast_qid, NSS_PPE_QDISC_QUEUE_LEVEL - 1, port_num, &l0cfg) != 0) {
			nss_ppe_qdisc_error("SSDK level0 multicast queue scheduler configuration failed\n");
			return -EINVAL;
		}
	}

	nss_ppe_qdisc_queue_scheduler_enable(npq);

	nss_ppe_qdisc_info("SSDK level0 queue scheduler configuration successful\n");
	return 0;
}

/*
 * nss_ppe_qdisc_l0_queue_scheduler_set()
 *	Configures a level 0 queue scheduler in SSDK.
 */
static int nss_ppe_qdisc_l0_queue_scheduler_set(struct nss_ppe_qdisc *npq)
{
	uint32_t port_num = nss_ppe_qdisc_port_num_get(npq);

	/*
	 * Allocate resources only if scheduler is not valid
	 */
	if (!npq->l0_valid) {
		/*
		 * Disable default configuration if present
		 * and allocate Level 0 resources
		 */
		nss_ppe_qdisc_def_conf_disable(port_num);

		if (nss_ppe_qdisc_l0_res_alloc(npq) != 0) {
			nss_ppe_qdisc_warning("SSDK level0 queue scheduler configuration failed\n");
			return -EINVAL;
		}
	}

	/*
	 * Set Level 0 configuration
	 */
	if (nss_ppe_qdisc_l0_queue_scheduler_configure(npq) != 0) {
		nss_ppe_qdisc_error("SSDK level0 queue scheduler configuration failed\n");
		nss_ppe_qdisc_l0_res_free(npq);
		return -EINVAL;
	}

	nss_ppe_qdisc_info("SSDK level0 queue scheduler configuration successful\n");
	return 0;
}

/*
 * nss_ppe_qdisc_port_shaper_reset()
 *	Resets a port shaper in SSDK.
 */
static int nss_ppe_qdisc_port_shaper_reset(struct nss_ppe_qdisc *npq)
{
	fal_shaper_config_t cfg;
	uint32_t port_num = nss_ppe_qdisc_port_num_get(npq);

	memset(&cfg, 0, sizeof(cfg));
	nss_ppe_qdisc_trace("SSDK port shaper reset : Port:%d\n", port_num);
	if (fal_port_shaper_set(0, port_num, &cfg) != 0) {
		nss_ppe_qdisc_error("SSDK port shaper configuration failed for port:%d\n", port_num);
		return -EINVAL;
	}

	nss_ppe_qdisc_info("SSDK port shaper configuration successful for port:%d\n", port_num);
	return 0;
}

/*
 * nss_ppe_qdisc_port_shaper_set()
 *	Configures a port shaper in SSDK.
 */
static int nss_ppe_qdisc_port_shaper_set(struct nss_ppe_qdisc *npq)
{
	fal_shaper_token_number_t token;
	fal_shaper_config_t cfg;
	uint32_t port_num = nss_ppe_qdisc_port_num_get(npq);

	/*
	 * Set port shaper token number
	 */
	memset(&token, 0, sizeof(token));
	token.c_token_number = NSS_PPE_QDISC_TOKEN_MAX;
	token.e_token_number = NSS_PPE_QDISC_TOKEN_MAX;

	nss_ppe_qdisc_trace("SSDK port token set : Port:%d, c_token_number:%x, e_token_number:%x\n",
		port_num, token.c_token_number, token.e_token_number);
	if (fal_port_shaper_token_number_set(0, port_num, &token) != 0) {
		nss_ppe_qdisc_error("SSDK port shaper token configuration failed for port:%d\n", port_num);
		return -EINVAL;
	}

	/*
	 * Set port shaper configuration
	 * Note: SSDK API requires burst in bytes/sec
	 * while rate in kbits/sec.
	 */
	memset(&cfg, 0, sizeof(cfg));
	cfg.couple_en = 0;
	cfg.meter_unit = 0;
	cfg.c_shaper_en = 1;
	cfg.cbs = npq->shaper.burst;
	cfg.cir = (npq->shaper.rate * 8) / 1000;
	cfg.shaper_frame_mode = 0;

	/*
	 * Take HW scaling into consideration
	 */
	cfg.cir = cfg.cir * NSS_PPE_QDISC_HW_FREQ_SCALING;

	nss_ppe_qdisc_trace("SSDK port shaper configuration: Port:%d, couple_en:%d, meter_unit:%d, c_shaper_en:%d, cbs:%d, cir:%d, ebs:%d, eir:%d, shaper_frame_mode:%d\n",
			port_num, cfg.couple_en, cfg.meter_unit, cfg.c_shaper_en, cfg.cbs, cfg.cir, cfg.ebs, cfg.eir, cfg.shaper_frame_mode);
	if (fal_port_shaper_set(0, port_num, &cfg) != 0) {
		nss_ppe_qdisc_error("SSDK port shaper configuration failed for port:%d\n", port_num);
		return -EINVAL;
	}

	nss_ppe_qdisc_info("SSDK port shaper configuration successful for port:%d\n", port_num);
	return 0;
}

/*
 * nss_ppe_qdisc_flow_shaper_reset()
 *	Resets a flow shaper in SSDK.
 */
static int nss_ppe_qdisc_flow_shaper_reset(struct nss_ppe_qdisc *npq)
{
	fal_shaper_config_t cfg;
	uint32_t port_num = nss_ppe_qdisc_port_num_get(npq);

	/*
	 * Reset flow shaper configuration
	 */
	memset(&cfg, 0, sizeof(cfg));
	nss_ppe_qdisc_trace("SSDK flow shaper reset : l0spid:%d\n", npq->l0spid);
	if (fal_flow_shaper_set(0, npq->l0spid, &cfg) != 0) {
		nss_ppe_qdisc_error("SSDK flow shaper configuration failed for port:%d, l0spid:%d\n",
			port_num, npq->l0spid);
		return -EINVAL;
	}

	nss_ppe_qdisc_info("SSDK flow shaper configuration successful for port:%d\n", port_num);
	return 0;
}

/*
 * nss_ppe_qdisc_flow_shaper_set()
 *	Configures a flow shaper in SSDK.
 */
static int nss_ppe_qdisc_flow_shaper_set(struct nss_ppe_qdisc *npq)
{
	fal_shaper_token_number_t token;
	fal_shaper_config_t cfg;
	uint32_t port_num = nss_ppe_qdisc_port_num_get(npq);

	/*
	 * Set flow shaper token number
	 */
	memset(&token, 0, sizeof(token));
	token.c_token_number = NSS_PPE_QDISC_TOKEN_MAX;
	token.e_token_number = NSS_PPE_QDISC_TOKEN_MAX;

	nss_ppe_qdisc_trace("SSDK flow token set : l0spid:%d, c_token_number:%x, e_token_number:%x\n",
		npq->l0spid, token.c_token_number, token.e_token_number);
	if (fal_flow_shaper_token_number_set(0, npq->l0spid, &token) != 0) {
		nss_ppe_qdisc_error("SSDK flow shaper token configuration failed for port:%d, l0spid:%d\n",
			port_num, npq->l0spid);
		return -EINVAL;
	}

	/*
	 * Set flow shaper token configuration
	 * Note: SSDK API requires burst in bytes/sec
	 * while rate in kbits/sec.
	 */
	cfg.couple_en = 0;
	cfg.meter_unit = 0;
	cfg.c_shaper_en = 1;
	cfg.cbs = npq->shaper.burst;
	cfg.cir = (npq->shaper.rate * 8) / 1000;
	cfg.e_shaper_en = 1;
	cfg.ebs = npq->shaper.cburst;
	cfg.eir = (npq->shaper.crate * 8 / 1000) - cfg.cir;
	cfg.shaper_frame_mode = 0;

	/*
	 * Take HW scaling into consideration
	 */
	cfg.cir = cfg.cir * NSS_PPE_QDISC_HW_FREQ_SCALING;
	cfg.eir = cfg.eir * NSS_PPE_QDISC_HW_FREQ_SCALING;

	nss_ppe_qdisc_trace("SSDK flow shaper configuration: l0spid:%d, couple_en:%d, meter_unit:%d, c_shaper_en:%d, cbs:%d, cir:%d, ebs:%d, eir:%d, shaper_frame_mode:%d\n",
		npq->l0spid, cfg.couple_en, cfg.meter_unit, cfg.c_shaper_en, cfg.cbs, cfg.cir, cfg.ebs, cfg.eir, cfg.shaper_frame_mode);
	if (fal_flow_shaper_set(0, npq->l0spid, &cfg) != 0) {
		nss_ppe_qdisc_error("SSDK flow shaper configuration failed for port:%d, l0spid:%d\n",
			port_num, npq->l0spid);
		return -EINVAL;
	}

	nss_ppe_qdisc_info("SSDK flow shaper configuration successful for port:%d\n", port_num);
	return 0;
}

/*
 * nss_ppe_qdisc_queue_shaper_reset()
 *	Resets a queue shaper in SSDK.
 */
static int nss_ppe_qdisc_queue_shaper_reset(struct nss_ppe_qdisc *npq)
{
	fal_shaper_config_t cfg;
	uint32_t port_num = nss_ppe_qdisc_port_num_get(npq);

	/*
	 * Reset queue shaper configuration
	 */
	memset(&cfg, 0, sizeof(cfg));
	nss_ppe_qdisc_trace("SSDK queue shaper reset : ucast_qid:%d\n", npq->q.ucast_qid);
	if (fal_queue_shaper_set(0, npq->q.ucast_qid, &cfg) != 0) {
		nss_ppe_qdisc_error("SSDK queue shaper configuration failed for port:%d, ucast_qid:%d\n",
			port_num, npq->q.ucast_qid);
		return -EINVAL;
	}

	nss_ppe_qdisc_info("SSDK queue shaper configuration successful for port:%d\n",
		port_num);
	return 0;
}

/*
 * nss_ppe_qdisc_queue_shaper_set()
 *	Configures a queue shaper in SSDK.
 */
static int nss_ppe_qdisc_queue_shaper_set(struct nss_ppe_qdisc *npq)
{
	fal_shaper_token_number_t token;
	fal_shaper_config_t cfg;
	uint32_t port_num = nss_ppe_qdisc_port_num_get(npq);

	/*
	 * Set queue shaper token number
	 */
	memset(&token, 0, sizeof(token));
	token.c_token_number = NSS_PPE_QDISC_TOKEN_MAX;
	token.e_token_number = NSS_PPE_QDISC_TOKEN_MAX;

	nss_ppe_qdisc_trace("SSDK queue token set : ucast_qid:%d, c_token_number:%x, e_token_number:%x\n",
		npq->q.ucast_qid, token.c_token_number, token.e_token_number);
	if (fal_queue_shaper_token_number_set(0, npq->q.ucast_qid, &token) != 0) {
		nss_ppe_qdisc_error("SSDK queue shaper token configuration failed\n");
		return -EINVAL;
	}

	/*
	 * Set queue shaper confguration
	 * Note: SSDK API requires burst in bytes/sec
	 * while rate in kbits/sec.
	 */
	cfg.couple_en = 0;
	cfg.meter_unit = 0;
	cfg.c_shaper_en = 1;
	cfg.cbs = npq->shaper.burst;
	cfg.cir = (npq->shaper.rate * 8) / 1000;
	cfg.e_shaper_en = 1;
	cfg.ebs = npq->shaper.cburst;
	cfg.eir = (npq->shaper.crate * 8 / 1000) - cfg.cir;
	cfg.shaper_frame_mode = 0;

	/*
	 * Take HW scaling into consideration
	 */
	cfg.cir = cfg.cir * NSS_PPE_QDISC_HW_FREQ_SCALING;
	cfg.eir = cfg.eir * NSS_PPE_QDISC_HW_FREQ_SCALING;

	nss_ppe_qdisc_trace("SSDK queue shaper configuration: ucast_qid:%d, couple_en:%d, meter_unit:%d, c_shaper_en:%d, cbs:%d, cir:%d, ebs:%d, eir:%d, shaper_frame_mode:%d\n",
		npq->q.ucast_qid, cfg.couple_en, cfg.meter_unit, cfg.c_shaper_en, cfg.cbs, cfg.cir, cfg.ebs, cfg.eir, cfg.shaper_frame_mode);
	if (fal_queue_shaper_set(0, npq->q.ucast_qid, &cfg) != 0) {
		nss_ppe_qdisc_error("SSDK queue shaper configuration failed\n");
		return -EINVAL;
	}

	/*
	 * This function is called at the time of class allocation and change at
	 * NSS_PPE_QDISC_QUEUE_LEVEL. But, mcast will be valid only in case queue
	 * is attached at NSS_PPE_QDISC_SUB_QUEUE_LEVEL and change class is invoked
	 * at NSS_PPE_QDISC_QUEUE_LEVEL level.
	 * Other scenarios catering to mcast queue are handled in separate mcast APIs.
	 */
	if (npq->q.mcast_valid) {
		nss_ppe_qdisc_trace("SSDK multicast queue shaper configuration: mcast_qid:%d, couple_en:%d, meter_unit:%d, c_shaper_en:%d, cbs:%d, cir:%d, ebs:%d, eir:%d, shaper_frame_mode:%d\n",
			npq->q.mcast_qid, cfg.couple_en, cfg.meter_unit, cfg.c_shaper_en, cfg.cbs, cfg.cir, cfg.ebs, cfg.eir, cfg.shaper_frame_mode);
		if (fal_queue_shaper_set(0, npq->q.mcast_qid, &cfg) != 0) {
			nss_ppe_qdisc_error("SSDK multicast queue shaper configuration failed\n");
			return -EINVAL;
		}
	}

	nss_ppe_qdisc_info("SSDK queue shaper configuration successful for port:%d\n", port_num);
	return 0;
}

/*
 * nss_ppe_qdisc_max_level_get()
 *	Returns the max configuration level for the given prot.
 */
static int nss_ppe_qdisc_max_level_get(struct nss_ppe_qdisc *npq)
{
	int level = NSS_PPE_MAX_LEVEL;

	/*
	 * For bridge ports, one level is being used by loopback.
	 */
	if (npq->nq.is_bridge) {
		level = level - 1;
	}

	nss_ppe_qdisc_info("level:%d\n", level);
	return level;
}

/*
 * nss_ppe_qdisc_base_get()
 *	Returns base of the particular resource for a given port.
 */
uint32_t nss_ppe_qdisc_base_get(uint32_t port, nss_ppe_qdisc_res_type_t type)
{
	uint32_t base = 0;
	struct nss_ppe_qdisc_port *ppe_port = &ppe_qdisc_port[port];

	spin_lock_bh(&ppe_port->lock);
	base = ppe_port->base[type];
	spin_unlock_bh(&ppe_port->lock);

	nss_ppe_qdisc_info("port:%d, type:%d, base:%d\n", port, type, base);
	return base;
}

/*
 * nss_ppe_qdisc_attach_free()
 *	Attaches a resource to free list.
 */
void nss_ppe_qdisc_attach_free(uint32_t port, struct nss_ppe_qdisc_res *res)
{
	struct nss_ppe_qdisc_port *ppe_port = &ppe_qdisc_port[port];

	spin_lock_bh(&ppe_port->lock);
	res->next = ppe_port->res_free[res->type];
	ppe_port->res_free[res->type] = res;
	spin_unlock_bh(&ppe_port->lock);

	nss_ppe_qdisc_info("port:%d, type:%d, res:%p\n", port, res->type, res);
	return;
}

/*
 * nss_ppe_qdisc_res_free()
 *	Frees the allocated resource and attach it to free list.
 */
int nss_ppe_qdisc_res_free(uint32_t port, uint32_t offset, nss_ppe_qdisc_res_type_t type)
{
	struct nss_ppe_qdisc_res *temp = NULL;
	struct nss_ppe_qdisc_res *res = NULL;
	struct nss_ppe_qdisc_port *ppe_port = &ppe_qdisc_port[port];

	if (type >= NSS_PPE_QDISC_MAX_RES_TYPE) {
		nss_ppe_qdisc_assert(false, "Resource type:%d not valid for port:%d", type, port);
		return -1;
	}

	spin_lock_bh(&ppe_port->lock);
	res = ppe_port->res_used[type];
	if (res->offset == offset) {
		ppe_port->res_used[type] = res->next;
		res->next = NULL;
		spin_unlock_bh(&ppe_port->lock);
		goto success;
	}

	temp = res;
	res = res->next;

	while (res) {
		if (res->offset == offset) {
			temp->next = res->next;
			res->next = NULL;
			break;
		} else {
			temp = res;
			res = res->next;
		}
	}
	spin_unlock_bh(&ppe_port->lock);

	if (!res) {
		nss_ppe_qdisc_assert(false, "Resource:%d type:%d not found for port:%d", offset, type, port);
		return -1;
	}

success:
	nss_ppe_qdisc_attach_free(port, res);
	nss_ppe_qdisc_info("port:%d, type:%d, res:%p\n", port, type, res);
	return 0;
}

/*
 * nss_ppe_qdisc_res_alloc()
 *	Allocates free resource for a given port.
 */
struct nss_ppe_qdisc_res *nss_ppe_qdisc_res_alloc(uint32_t port, nss_ppe_qdisc_res_type_t type)
{
	struct nss_ppe_qdisc_res *res = NULL;
	struct nss_ppe_qdisc_port *ppe_port = &ppe_qdisc_port[port];

	/*
	 * Detach the resource from free list
	 * and attach to used list.
	 */
	spin_lock_bh(&ppe_port->lock);
	res = ppe_port->res_free[type];
	if (res) {
		ppe_port->res_free[type] = res->next;
		res->next = ppe_port->res_used[type];
		ppe_port->res_used[type] = res;
	}
	spin_unlock_bh(&ppe_port->lock);

	nss_ppe_qdisc_info("port:%d, type:%d, res:%p\n", port, type, res);
	return res;
}

/*
 * nss_ppe_qdisc_default_conf_set()
 *	Sets default queue scheduler in SSDK.
 */
int nss_ppe_qdisc_default_conf_set(uint32_t port_num)
{
	fal_qos_scheduler_cfg_t l1cfg;
	fal_qos_scheduler_cfg_t l0cfg;
	fal_ac_obj_t obj;
	uint32_t l0spid;
	uint32_t ucast_qid;
	uint32_t mcast_qid;
	struct nss_ppe_qdisc_port *ppe_port = &ppe_qdisc_port[port_num];

	/*
	 * No resources were allocated for Port 0 (bridge interface).
	 * L1 scheduler was configured at init time.
	 */
	if (port_num == 0) {
		l0spid = NSS_PPE_QDISC_LOOPBACK_L0_SP_BASE;
		goto conf;
	}

	/*
	 * Reset Level 1 Configuration
	 */
	memset(&l1cfg, 0, sizeof(l1cfg));
	l1cfg.sp_id = port_num;
	l1cfg.c_pri = NSS_PPE_QDISC_PRIORITY_MAX;
	l1cfg.e_pri = NSS_PPE_QDISC_PRIORITY_MAX;
	l1cfg.c_drr_wt = 1;
	l1cfg.e_drr_wt = 1;
	l1cfg.c_drr_id = nss_ppe_qdisc_base_get(port_num, NSS_PPE_QDISC_L1_CDRR);
	l1cfg.e_drr_id = nss_ppe_qdisc_base_get(port_num, NSS_PPE_QDISC_L1_EDRR);
	l0spid = nss_ppe_qdisc_base_get(port_num, NSS_PPE_QDISC_L0_SP);

	nss_ppe_qdisc_trace("SSDK level1 configuration: Port:%d, l0spid:%d, c_drrid:%d, c_pri:%d, c_drr_wt:%d, e_drrid:%d, e_pri:%d, e_drr_wt:%d, l1spid:%d\n",
			port_num, l0spid, l1cfg.c_drr_id, l1cfg.c_pri, l1cfg.c_drr_wt, l1cfg.e_drr_id, l1cfg.e_pri, l1cfg.e_drr_wt, l1cfg.sp_id);
	if (fal_queue_scheduler_set(0, l0spid, NSS_PPE_QDISC_FLOW_LEVEL - 1, port_num, &l1cfg) != 0) {
		nss_ppe_qdisc_error("SSDK level1 queue scheduler configuration failed\n");
		return -EINVAL;
	}

conf:
	/*
	 * Reset Level 0 Configuration
	 */
	memset(&l0cfg, 0, sizeof(l0cfg));
	l0cfg.sp_id = l0spid;
	l0cfg.c_pri = NSS_PPE_QDISC_PRIORITY_MAX;
	l0cfg.e_pri = NSS_PPE_QDISC_PRIORITY_MAX;
	l0cfg.c_drr_wt = 1;
	l0cfg.e_drr_wt = 1;
	l0cfg.c_drr_id = nss_ppe_qdisc_base_get(port_num, NSS_PPE_QDISC_L0_CDRR);
	l0cfg.e_drr_id = nss_ppe_qdisc_base_get(port_num, NSS_PPE_QDISC_L0_EDRR);
	ucast_qid = nss_ppe_qdisc_base_get(port_num, NSS_PPE_QDISC_UCAST_QUEUE);
	mcast_qid = nss_ppe_qdisc_base_get(port_num, NSS_PPE_QDISC_MCAST_QUEUE);

	nss_ppe_qdisc_trace("SSDK level0 configuration: Port:%d, ucast_qid:%d, c_drrid:%d, c_pri:%d, c_drr_wt:%d, e_drrid:%d, e_pri:%d, e_drr_wt:%d, l0spid:%d\n",
			port_num, ucast_qid, l0cfg.c_drr_id, l0cfg.c_pri, l0cfg.c_drr_wt, l0cfg.e_drr_id, l0cfg.e_pri, l0cfg.e_drr_wt, l0cfg.sp_id);
	if (fal_queue_scheduler_set(0, ucast_qid, NSS_PPE_QDISC_QUEUE_LEVEL - 1, port_num, &l0cfg) != 0) {
		nss_ppe_qdisc_error("SSDK level0 queue scheduler configuration failed\n");
		return -EINVAL;
	}

	nss_ppe_qdisc_trace("SSDK level0 configuration: Port:%d, mcast_qid:%d, c_drrid:%d, c_pri:%d, c_drr_wt:%d, e_drrid:%d, e_pri:%d, e_drr_wt:%d, l0spid:%d\n",
			port_num, mcast_qid, l0cfg.c_drr_id, l0cfg.c_pri, l0cfg.c_drr_wt, l0cfg.e_drr_id, l0cfg.e_pri, l0cfg.e_drr_wt, l0cfg.sp_id);
	if (fal_queue_scheduler_set(0, mcast_qid, NSS_PPE_QDISC_QUEUE_LEVEL - 1, port_num, &l0cfg) != 0) {
		nss_ppe_qdisc_error("SSDK level0 queue scheduler configuration failed\n");
		return -EINVAL;
	}

	memset(&obj, 0, sizeof(obj));
	obj.obj_id = ucast_qid;
	fal_ac_prealloc_buffer_set(0, &obj, 0);
	obj.obj_id = mcast_qid;
	fal_ac_prealloc_buffer_set(0, &obj, 0);

	/*
	 * Enable queue enqueue and dequeue.
	 */
	fal_qm_enqueue_ctrl_set(0, ucast_qid, true);
	fal_qm_enqueue_ctrl_set(0, mcast_qid, true);
	fal_scheduler_dequeue_ctrl_set(0, ucast_qid, true);
	fal_scheduler_dequeue_ctrl_set(0, mcast_qid, true);

	/*
	 * Set the default queue configuration status.
	 */
	spin_lock_bh(&ppe_port->lock);
	ppe_port->def_conf_enable = true;
	spin_unlock_bh(&ppe_port->lock);

	nss_ppe_qdisc_info("SSDK queue configuration successful\n");
	return 0;
}

/*
 * nss_ppe_qdisc_queue_limit_set()
 *	Sets queue size in SSDK.
 */
int nss_ppe_qdisc_queue_limit_set(struct nss_ppe_qdisc *npq)
{
	fal_ac_obj_t obj;
	fal_ac_static_threshold_t cfg;

	/*
	 * Unicast queue configuration.
	 */
	memset(&obj, 0, sizeof(obj));
	obj.obj_id = npq->q.ucast_qid;

	nss_ppe_qdisc_trace("SSDK queue buffer set: ucast_qid:%d, qlimit:%d\n", npq->q.ucast_qid, npq->q.qlimit);
	if (fal_ac_prealloc_buffer_set(0, &obj, npq->q.qlimit) != 0) {
		nss_ppe_qdisc_error("SSDK queue configuration failed\n");
		return -EINVAL;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.wred_enable = npq->q.red_en;
	cfg.green_max = npq->q.max_th[NSS_PPE_COLOR_GREEN];
	cfg.green_min_off = npq->q.min_th[NSS_PPE_COLOR_GREEN];
	nss_ppe_qdisc_trace("SSDK queue ac threshold set: ucast_qid:%d, wred_enable:%d, green_max:%d, green_min_off:%d\n",
		npq->q.ucast_qid, cfg.wred_enable, cfg.green_max, cfg.green_min_off);
	if (fal_ac_static_threshold_set(0, &obj, &cfg) != 0) {
		nss_ppe_qdisc_error("SSDK queue configuration failed\n");
		return -EINVAL;
	}

	/*
	 * Multicast queue configuration.
	 */
	if (npq->q.mcast_valid) {
		obj.obj_id = npq->q.mcast_qid;

		nss_ppe_qdisc_trace("SSDK multicast queue buffer set: mcast_qid:%d, qlimit:%d\n", npq->q.mcast_qid, npq->q.qlimit);
		if (fal_ac_prealloc_buffer_set(0, &obj, npq->q.qlimit) != 0) {
			nss_ppe_qdisc_error("SSDK multicast queue configuration failed\n");
			return -EINVAL;
		}

		nss_ppe_qdisc_trace("SSDK multicast queue ac threshold set: mcast_qid:%d, wred_enable:%d, green_max:%d, green_min_off:%d\n",
			npq->q.mcast_qid, cfg.wred_enable, cfg.green_max, cfg.green_min_off);
		if (fal_ac_static_threshold_set(0, &obj, &cfg) != 0) {
			nss_ppe_qdisc_error("SSDK multicast queue configuration failed\n");
			return -EINVAL;
		}
	}

	nss_ppe_qdisc_info("SSDK queue configuration successful\n");
	return 0;
}

/*
 * nss_ppe_qdisc_scheduler_reset()
 *	Resets a configured scheduler in SSDK.
 */
int nss_ppe_qdisc_scheduler_reset(struct nss_ppe_qdisc *npq)
{
	if (npq->level == NSS_PPE_QDISC_PORT_LEVEL) {
		if (npq->nq.is_root) {
			if (nss_ppe_qdisc_l0_queue_scheduler_reset(npq) != 0) {
				nss_ppe_qdisc_warning("SSDK Level0 queue scheduler reset failed\n");
				return -EINVAL;
			}

			if (nss_ppe_qdisc_l1_res_free(npq) != 0) {
				nss_ppe_qdisc_warning("SSDK Level1 queue scheduler reset failed\n");
				return -EINVAL;
			}
		}
		nss_ppe_qdisc_info("SSDK reset scheduler successful\n");
		return 0;
	}

	if (npq->level == NSS_PPE_QDISC_FLOW_LEVEL) {
		if (!npq->nq.is_class) {
			if (nss_ppe_qdisc_l0_queue_scheduler_reset(npq) < 0) {
				nss_ppe_qdisc_warning("SSDK Level0 configuration for attach of new qdisc failed\n");
				return -EINVAL;
			}
		}
		if (nss_ppe_qdisc_l1_res_free(npq) != 0) {
			nss_ppe_qdisc_warning("SSDK Level1 queue scheduler reset failed\n");
			return -EINVAL;
		}
	} else if (npq->level == NSS_PPE_QDISC_QUEUE_LEVEL) {
		if (nss_ppe_qdisc_l0_queue_scheduler_reset(npq) != 0) {
			nss_ppe_qdisc_warning("SSDK Level0 queue scheduler reset failed\n");
			return -EINVAL;
		}
	} else {

		/*
		 * When a classful qdisc say HTB is configured with max levels of hierarchy,
		 * and then if a qdisc say FIFO is attached at the last level, we will have all
		 * the resources allocated and we just need to enable/disable the queue.
		 */
		nss_ppe_qdisc_queue_scheduler_disable(npq);
	}

	nss_ppe_qdisc_info("SSDK reset scheduler successful\n");
	return 0;
}

/*
 * nss_ppe_qdisc_scheduler_set()
 *	Configures a scheduler in SSDK.
 */
int nss_ppe_qdisc_scheduler_set(struct nss_ppe_qdisc *npq)
{
	if (npq->level == NSS_PPE_QDISC_PORT_LEVEL) {

		/*
		 * In case of HTB class 1:1, the level is PORT_LEVEL but its not root.
		 * So, set queue schedulers only when qdisc is root.
		 */
		if (npq->nq.is_root) {
			if (nss_ppe_qdisc_l1_queue_scheduler_set(npq) < 0) {
				nss_ppe_qdisc_warning("SSDK Level1 configuration failed\n");
				return -EINVAL;
			}

			if (nss_ppe_qdisc_l0_queue_scheduler_set(npq) < 0) {
				nss_ppe_qdisc_l1_res_free(npq);
				nss_ppe_qdisc_warning("SSDK Level0 configuration failed\n");
				return -EINVAL;
			}
		}
		nss_ppe_qdisc_info("SSDK scheduler configuration successful\n");
		return 0;
	}

	if (npq->level == NSS_PPE_QDISC_FLOW_LEVEL) {
		if (nss_ppe_qdisc_l1_queue_scheduler_set(npq) < 0) {
			nss_ppe_qdisc_warning("SSDK Level1 configuration failed\n");
			return -EINVAL;
		}

		if (!npq->nq.is_class) {
			if (nss_ppe_qdisc_l0_queue_scheduler_set(npq) < 0) {
				nss_ppe_qdisc_warning("SSDK Level0 configuration failed\n");
				return -EINVAL;
			}
		}
	} else if (npq->level == NSS_PPE_QDISC_QUEUE_LEVEL) {
		if (nss_ppe_qdisc_l0_queue_scheduler_set(npq) < 0) {
			nss_ppe_qdisc_warning("SSDK Level0 configuration failed\n");
			return -EINVAL;
		}

		/*
		 * Disable the scheduler unitl a child qdisc is added
		 */
		if (npq->nq.is_class) {
			nss_ppe_qdisc_queue_scheduler_disable(npq);
		}
	} else {

		/*
		 * When a classful qdisc say HTB is configured with max levels of hierarchy,
		 * and then if a qdisc say FIFO is attached at the last level, we will have all
		 * the resources allocated and we just need to enable/disable the queue.
		 */
		nss_ppe_qdisc_queue_scheduler_enable(npq);
	}

	nss_ppe_qdisc_info("SSDK scheduler configuration successful\n");
	return 0;
}

/*
 * nss_ppe_qdisc_shaper_reset()
 *	Resets a configured shaper in SSDK.
 */
int nss_ppe_qdisc_shaper_reset(struct nss_ppe_qdisc *npq)
{
	if (npq->level == NSS_PPE_QDISC_PORT_LEVEL) {
		if (nss_ppe_qdisc_port_shaper_reset(npq) != 0) {
				nss_ppe_qdisc_warning("Reset Port shaper failed\n");
			return -EINVAL;
		}
		nss_ppe_qdisc_info("SSDK reset shaper successful\n");
		return 0;
	}

	if (npq->level == NSS_PPE_QDISC_FLOW_LEVEL) {
		if (nss_ppe_qdisc_flow_shaper_reset(npq) != 0) {
			nss_ppe_qdisc_warning("Reset Flow shaper failed\n");
			return -EINVAL;
		}
		nss_ppe_qdisc_info("SSDK reset shaper successful\n");
		return 0;
	}

	if (nss_ppe_qdisc_queue_shaper_reset(npq) != 0) {
		nss_ppe_qdisc_warning("Reset Queue shaper failed\n");
			return -EINVAL;
	}

	nss_ppe_qdisc_info("SSDK reset shaper successful\n");
	return 0;
}

/*
 * nss_ppe_qdisc_shaper_set()
 *	Configures a shaper in SSDK.
 */
int nss_ppe_qdisc_shaper_set(struct nss_ppe_qdisc *npq)
{
	if (npq->level == NSS_PPE_QDISC_PORT_LEVEL) {
		if (nss_ppe_qdisc_port_shaper_set(npq) != 0) {
			nss_ppe_qdisc_warning("Port shaper configuration failed\n");
			return -EINVAL;
		}
		nss_ppe_qdisc_info("SSDK set shaper successful\n");
		return 0;
	}

	if (npq->level == NSS_PPE_QDISC_FLOW_LEVEL) {
		if (nss_ppe_qdisc_flow_shaper_set(npq) != 0) {
			nss_ppe_qdisc_warning("Port shaper configuration failed\n");
			return -EINVAL;
		}
		nss_ppe_qdisc_info("SSDK set shaper successful\n");
		return 0;
	}

	if (nss_ppe_qdisc_queue_shaper_set(npq) != 0) {
		nss_ppe_qdisc_warning("Queue shaper configuration failed\n");
		return -EINVAL;
	}

	nss_ppe_qdisc_info("SSDK set shaper successful\n");
	return 0;
}

/*
 * nss_ppe_qdisc_port_num_get()
 *	Returns the port number.
 */
int nss_ppe_qdisc_port_num_get(struct nss_ppe_qdisc *npq)
{
	int port_num = 0;

	/*
	 * Fetch port number based on interface type.
	 * TODO: Change this when API from DP is available
	 */
	if (!(npq->nq.is_bridge)) {
		port_num = npq->nq.nss_interface_number;
	}

	nss_ppe_qdisc_info("port:%d\n", port_num);
	return port_num;
}

/*
 * nss_ppe_qdisc_is_depth_valid()
 *	Checks the depth of Qdisc tree.
 */
int nss_ppe_qdisc_is_depth_valid(struct nss_ppe_qdisc *npq)
{
	nss_ppe_qdisc_trace("level:%d\n", npq->level);
	if (npq->level == NSS_PPE_QDISC_QUEUE_LEVEL) {
		return false;
	}

	return true;
}

/*
 * nss_ppe_qdisc_node_detach()
 *	Configuration function that helps detach a child shaper node from a parent.
 */
int nss_ppe_qdisc_node_detach(struct nss_ppe_qdisc *npq, struct Qdisc *old)
{
	struct nss_if_msg nim_detach;
	struct nss_qdisc *nq_old = qdisc_priv(old);
	struct nss_ppe_qdisc *npq_old = qdisc_priv(old);

	if (nq_old->mode != NSS_QDISC_MODE_PPE) {
		if (nss_qdisc_set_hybrid_mode(nq_old, NSS_QDISC_HYBRID_MODE_DISABLE, 0) < 0) {
			nss_ppe_qdisc_warning("detach of old qdisc %x failed\n", old->handle);
			return -EINVAL;
		}
	}

	nim_detach.msg.shaper_configure.config.msg.shaper_node_config.qos_tag = npq->nq.qos_tag;
	nim_detach.msg.shaper_configure.config.msg.shaper_node_config.snc.ppe_sn_detach.child_qos_tag = nq_old->qos_tag;
	if (nss_qdisc_node_detach(&npq->nq, nq_old, &nim_detach,
			NSS_SHAPER_CONFIG_TYPE_SHAPER_NODE_DETACH) < 0) {
		nss_ppe_qdisc_warning("detach of old qdisc %x failed\n", old->handle);
		return -EINVAL;
	}

	if (nss_ppe_qdisc_mcast_queue_reset(npq_old) < 0) {
		nss_ppe_qdisc_warning("SSDK scheduler reset for detach of old qdisc %x failed\n", old->handle);
		return -EINVAL;
	}

	if (nss_ppe_qdisc_scheduler_reset(npq_old) < 0) {
		nss_ppe_qdisc_warning("SSDK scheduler reset for detach of old qdisc %x failed\n", old->handle);
		return -EINVAL;
	}

	nss_ppe_qdisc_info("Qdisc:%p, node:%p\n", npq, old);
	return 0;
}

/*
 * nss_ppe_qdisc_node_attach()
 *	Configuration function that helps attach a child shaper node to a parent.
 */
int nss_ppe_qdisc_node_attach(struct nss_ppe_qdisc *npq, struct Qdisc *new)
{
	struct nss_if_msg nim_attach;
	struct nss_ppe_qdisc *npq_new = qdisc_priv(new);
	struct nss_qdisc *nq_new = qdisc_priv(new);
	int ucast_qbase = nss_ppe_qdisc_base_get(nss_ppe_qdisc_port_num_get(npq), NSS_PPE_QDISC_UCAST_QUEUE);
	bool mcast_enable = npq_new->q.mcast_enable;

	nim_attach.msg.shaper_configure.config.msg.shaper_node_config.qos_tag = npq->nq.qos_tag;
	nim_attach.msg.shaper_configure.config.msg.shaper_node_config.snc.ppe_sn_attach.child_qos_tag = nq_new->qos_tag;
	if (nss_qdisc_node_attach(&npq->nq, nq_new, &nim_attach,
			NSS_SHAPER_CONFIG_TYPE_SHAPER_NODE_ATTACH) < 0) {
		nss_ppe_qdisc_warning("attach of new qdisc %x failed\n", new->handle);
		return -EINVAL;
	}

	/*
	 * Set SSDK configuration
	 * TODO: New qdisc can be of mode NSS_QDISC_MODE_NSS.
	 * We cannot typecast it to nss_ppe_qdisc type.
	 * This typecasting will go once we move PPE Qdisc to NSS Qdisc.
	 */
	npq_new->level = npq->level - 1;
	npq_new->l0spid = npq->l0spid;
	npq_new->q = npq->q;
	npq_new->q.mcast_enable = mcast_enable;

	if (nss_ppe_qdisc_scheduler_set(npq_new) < 0) {
		nss_ppe_qdisc_warning("SSDK scheduler configuration for attach of new qdisc %x failed\n", new->handle);
		return -EINVAL;
	}

	/*
	 * Configure unicast and multicast queue, offset and limit in NSS
	 * as this information was not available at queue init time.
	 */
	if (nss_ppe_qdisc_configure(npq_new) < 0) {
		nss_ppe_qdisc_mcast_queue_reset(npq_new);
		nss_ppe_qdisc_scheduler_reset(npq_new);
		nss_ppe_qdisc_warning("SSDK scheduler configuration for attach of new qdisc %x failed\n", new->handle);
		return -EINVAL;
	}

	/*
	 * If new qdisc is not of type PPE, then configure a simple fifo
	 * queue in HW. This is where packets coming from the Adv QoS
	 * portion of the tree will get enqueued to.
	 */
	if (nq_new->mode != NSS_QDISC_MODE_PPE) {
		if (nss_qdisc_set_hybrid_mode(nq_new, NSS_QDISC_HYBRID_MODE_ENABLE, npq_new->q.ucast_qid - ucast_qbase) < 0) {
			nss_qdisc_warning("nss qdisc configuration failed\n");
				nss_ppe_qdisc_mcast_queue_reset(npq_new);
				nss_ppe_qdisc_scheduler_reset(npq_new);
			return -EINVAL;
		}
	}

	nss_ppe_qdisc_info("Qdisc:%p, node:%p\n", npq, new);
	return 0;
}

/*
 * nss_ppe_qdisc_configure()
 *	Configures the SSDK schedulers and NSS shapers.
 */
int nss_ppe_qdisc_configure(struct nss_ppe_qdisc *npq)
{
	struct nss_if_msg nim;
	int ucast_qbase = nss_ppe_qdisc_base_get(nss_ppe_qdisc_port_num_get(npq), NSS_PPE_QDISC_UCAST_QUEUE);
	int mcast_qbase = nss_ppe_qdisc_base_get(nss_ppe_qdisc_port_num_get(npq), NSS_PPE_QDISC_MCAST_QUEUE);

	/*
	 * We have nothing to do when qid is not known
	 */
	if (!npq->q.ucast_valid) {
		return 0;
	}

	if (nss_ppe_qdisc_mcast_queue_set(npq) < 0) {
		nss_ppe_qdisc_warning("SSDK multicast queueue configuration failed\n");
		return -EINVAL;
	}

	/*
	 * Program PPE queue parameters
	 */
	if (nss_ppe_qdisc_queue_limit_set(npq) < 0) {
		nss_ppe_qdisc_warning("SSDK multicast queueue configuration failed\n");
		return -EINVAL;
	}

	/*
	 * Send configuration to NSS only if it is PPE qdisc.
	 */
	if (npq->nq.mode != NSS_QDISC_MODE_PPE) {
		return 0;
	}

	memset(&nim, 0, sizeof(struct nss_if_msg));
	nim.msg.shaper_configure.config.msg.shaper_node_config.qos_tag = npq->nq.qos_tag;
	nim.msg.shaper_configure.config.msg.shaper_node_config.snc.ppe_sn_param.type = npq->sub_type;
	nim.msg.shaper_configure.config.msg.shaper_node_config.snc.ppe_sn_param.ucast_base = ucast_qbase;
	nim.msg.shaper_configure.config.msg.shaper_node_config.snc.ppe_sn_param.ucast_offset = npq->q.ucast_qid - ucast_qbase;
	nim.msg.shaper_configure.config.msg.shaper_node_config.snc.ppe_sn_param.limit = npq->q.qlimit;

	if (npq->q.mcast_valid) {
		nim.msg.shaper_configure.config.msg.shaper_node_config.snc.ppe_sn_param.mcast_base = mcast_qbase;
		nim.msg.shaper_configure.config.msg.shaper_node_config.snc.ppe_sn_param.mcast_offset = npq->q.mcast_qid - mcast_qbase;
	}

	nim.msg.shaper_configure.config.msg.shaper_node_config.snc.ppe_sn_param.port = nss_ppe_qdisc_port_num_get(npq);

	if (nss_qdisc_configure(&npq->nq, &nim, NSS_SHAPER_CONFIG_TYPE_SHAPER_NODE_CHANGE_PARAM) < 0) {
		nss_ppe_qdisc_warning("Qdisc configuration failed\n");
		return -EINVAL;
	}

	nss_ppe_qdisc_info("Qdisc configured successfully\n");
	return 0;
}

/*
 * nss_ppe_qdisc_init()
 *	Initializes NSS Qdisc and sets the level of this qdisc.
 */
int nss_ppe_qdisc_init(struct Qdisc *sch, struct nss_ppe_qdisc *npq, nss_shaper_node_type_t type,
			enum nss_shaper_config_ppe_sn_type sub_type, uint32_t classid)
{
	if (nss_qdisc_init(sch, &npq->nq, NSS_QDISC_MODE_PPE, type, classid) < 0) {
		nss_ppe_qdisc_warning("%p init failed\n", sch);
		return -EINVAL;
	}

	npq->parent = NULL;
	npq->sub_type = sub_type;
	npq->q.qlimit = qdisc_dev(sch)->tx_queue_len ? : 1;
	npq->q.ucast_valid = false;
	npq->q.mcast_valid = false;
	npq->q.mcast_enable = false;
	npq->l1_valid = false;
	npq->l0_valid = false;
	npq->level = nss_ppe_qdisc_max_level_get(npq) - 1;
	memset(&npq->shaper, 0, sizeof(struct nss_ppe_shaper));
	memset(&npq->scheduler, 0, sizeof(struct nss_ppe_scheduler));

	nss_ppe_qdisc_info("Qdisc initialization successful\n");
	return 0;
}

/*
 * nss_ppe_qdisc_module_init()
 *	PPE Qdisc module init function.
 */
static int __init nss_ppe_qdisc_module_init(void)
{
	int ret;
#ifdef CONFIG_OF
	/*
	 * If the node is not compatible, don't do anything.
	 */
	if (!of_find_node_by_name(NULL, "nss-common")) {
		return 0;
	}
#endif

	/*
	 * Bail out on not supported platform
	 * TODO: Handle this properly with SoC ops
	 */
	if (!of_machine_is_compatible("qcom,ipq807x")) {
		return 0;
	}

	nss_ppe_qdisc_info("Module initializing\n");

	ret = register_qdisc(&nss_ppe_pfifo_qdisc_ops);
	if (ret != 0)
		return ret;
	nss_ppe_qdisc_info_always("ppepfifo registered\n");

	ret = register_qdisc(&nss_ppe_bfifo_qdisc_ops);
	if (ret != 0)
		return ret;
	nss_ppe_qdisc_info_always("ppebfifo registered\n");

	ret = register_qdisc(&nss_ppe_htb_qdisc_ops);
	if (ret != 0)
		return ret;
	nss_ppe_qdisc_info_always("ppehtb registered\n");

	ret = register_qdisc(&nss_ppe_red_qdisc_ops);
	if (ret != 0)
		return ret;
	nss_ppe_qdisc_info_always("ppered registered\n");

	nss_ppe_qdisc_port_res_alloc();
	nss_ppe_qdisc_info_always("nss ppe qdsic configured");

	nss_qdisc_ppe_mod_owner_set(THIS_MODULE);
	return 0;
}

/*
 * nss_ppe_qdisc_module_exit()
 *	PPE Qdisc module exit function.
 */
static void __exit nss_ppe_qdisc_module_exit(void)
{
#ifdef CONFIG_OF
	/*
	 * If the node is not compatible, don't do anything.
	 */
	if (!of_find_node_by_name(NULL, "nss-common")) {
		return;
	}
#endif

	/*
	 * Bail out on not supported platform
	 * TODO: Handle this properly with SoC ops
	 */
	if (!of_machine_is_compatible("qcom,ipq807x")) {
		return;
	}

	nss_ppe_qdisc_port_res_free();
	nss_ppe_qdisc_info_always("nss_ppe_qdisc_port_res_free\n");

	unregister_qdisc(&nss_ppe_pfifo_qdisc_ops);
	nss_ppe_qdisc_info_always("ppepfifo unregistered\n");

	unregister_qdisc(&nss_ppe_bfifo_qdisc_ops);
	nss_ppe_qdisc_info_always("ppebfifo unregistered\n");

	unregister_qdisc(&nss_ppe_htb_qdisc_ops);
	nss_ppe_qdisc_info_always("ppehtb unregistered\n");

	unregister_qdisc(&nss_ppe_red_qdisc_ops);
	nss_ppe_qdisc_info_always("ppered unregistered\n");

	nss_qdisc_ppe_mod_owner_set(NULL);
}

module_init(nss_ppe_qdisc_module_init)
module_exit(nss_ppe_qdisc_module_exit)

MODULE_LICENSE("Dual BSD/GPL");
