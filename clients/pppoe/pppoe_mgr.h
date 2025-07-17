/*
 * Copyright (c) 2017-2018 The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: ISC
 */

/*
 * pppoe_mgr.h
 *	PPPoE client definitions
 */

#ifndef _PPPOE_MGR_H_
#define _PPPOE_MGR_H_

#include <nss_client_mgr.h>

/*
 * Debug macros
 */
#if (PPPOE_MGR_DEBUG_LEVEL < 1)
#define pppoe_mgr_assert(fmt, args...)
#else
#define pppoe_mgr_assert(c) BUG_ON(!(c))
#endif

#if defined(CONFIG_DYNAMIC_DEBUG)
/*
 * Compile messages for dynamic enable/disable
 */
#define pppoe_mgr_warn(s, ...) pr_debug("%s[%d]:" s, __func__, \
					  __LINE__, ##__VA_ARGS__)
#define pppoe_mgr_info(s, ...) pr_debug("%s[%d]:" s, __func__, \
					  __LINE__, ##__VA_ARGS__)
#define pppoe_mgr_trace(s, ...) pr_debug("%s[%d]:" s, __func__, \
					   __LINE__, ##__VA_ARGS__)
#else

/*
 * Statically compile messages at different levels
 */
#if (PPPOE_MGR_DEBUG_LEVEL < 2)
#define pppoe_mgr_warn(s, ...)
#else
#define pppoe_mgr_warn(s, ...) pr_warn("%s[%d]:" s, __func__, \
					 __LINE__, ##__VA_ARGS__)
#endif

#if (PPPOE_MGR_DEBUG_LEVEL < 3)
#define pppoe_mgr_info(s, ...)
#else
#define pppoe_mgr_info(s, ...) pr_notice("%s[%d]:" s, __func__, \
					   __LINE__, ##__VA_ARGS__)
#endif

#if (PPPOE_MGR_DEBUG_LEVEL < 4)
#define pppoe_mgr_trace(s, ...)
#else
#define pppoe_mgr_trace(s, ...)  pr_info("%s[%d]:" s, __func__, \
					   __LINE__, ##__VA_ARGS__)
#endif
#endif

/*
 * PPPoE Client Frontend methods
 *	PPPoE client frontend mothods for the frontends PPE/IPA.
 */
typedef int (*pppoe_mgr_connect_event_t)(struct net_device *netdev);
typedef int (*pppoe_mgr_disconnect_event_t)(struct net_device *netdev);
typedef int (*pppoe_mgr_changemtu_event_t)(struct net_device *netdev);

/*
 * struct pppoe_mgr_cmn_ctx
 *	Structure for PPPoE client frontend methods for PPE/IPA.
 */
struct pppoe_mgr_cmn_ctx {
	pppoe_mgr_connect_event_t pppoe_connect;	/* PPPoE connect function */
	pppoe_mgr_disconnect_event_t pppoe_disconnect;	/* PPPoE disconnect function */
	pppoe_mgr_changemtu_event_t pppoe_changemtu;	/* PPPoE change mtu function */
};

/*
 * struct pppoe_mgr_session_info
 *	Structure for PPPoE client driver session info
 */
struct pppoe_mgr_session_info {
	uint32_t session_id;		/* PPPoE Session ID */
	uint8_t server_mac[ETH_ALEN];	/* PPPoE server MAC address */
	uint8_t local_mac[ETH_ALEN];	/* PPPoE local MAC address */
};

/*
 * struct pppoe_mgr_session_entry
 *	Structure for PPPoE session entry into HASH table
 */
struct pppoe_mgr_session_entry {
	struct pppoe_mgr_session_info info;	/* Session information */
	struct net_device *dev;			/* Net device */
	uint16_t mtu;				/* MTU */
};

bool pppoe_mgr_get_session(struct net_device *dev, struct pppoe_opt *opt);
void pppoe_mgr_init_session(struct net_device *dev, struct pppoe_opt *opt, struct pppoe_mgr_session_entry *entry);
#endif
