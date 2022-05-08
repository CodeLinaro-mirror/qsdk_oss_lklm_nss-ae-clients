/*
 **************************************************************************
 * Copyright (c) 2014-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
 * OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 **************************************************************************
 */

#ifndef __NSS_CAPWAPMGR_H
#define __NSS_CAPWAPMGR_H

#if (NSS_CAPWAPMGR_DEBUG_LEVEL < 1)
#define nss_capwapmgr_assert(fmt, args...)
#else
#define nss_capwapmgr_assert(c) if (!(c)) { BUG_ON(!(c)); }
#endif /* NSS_CAPWAPMGR_DEBUG_LEVEL */

/*
 * Compile messages for dynamic enable/disable
 */
#if defined(CONFIG_DYNAMIC_DEBUG)
#define nss_capwapmgr_warn(s, ...)	pr_debug("%s[%d]:" s, __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define nss_capwapmgr_info(s, ...)	pr_debug("%s[%d]:" s, __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define nss_capwapmgr_trace(s, ...)	pr_debug("%s[%d]:" s, __FUNCTION__, __LINE__, ##__VA_ARGS__)
#else /* CONFIG_DYNAMIC_DEBUG */
/*
 * Statically compile messages at different levels
 */
#if (NSS_CAPWAPMGR_DEBUG_LEVEL < 2)
#define nss_capwapmgr_warn(s, ...)
#else
#define nss_capwapmgr_warn(s, ...)	pr_warn("%s[%d]:" s, __FUNCTION__, __LINE__, ##__VA_ARGS__)
#endif

#if (NSS_CAPWAPMGR_DEBUG_LEVEL < 3)
#define nss_capwapmgr_info(s, ...)
#else
#define nss_capwapmgr_info(s, ...)	pr_notice("%s[%d]:" s, __FUNCTION__, __LINE__, ##__VA_ARGS__)
#endif

#if (NSS_CAPWAPMGR_DEBUG_LEVEL < 4)
#define nss_capwapmgr_trace(s, ...)
#else
#define nss_capwapmgr_trace(s, ...)	pr_info("%s[%d]:" s, __FUNCTION__, __LINE__, ##__VA_ARGS__)
#endif
#endif /* CONFIG_DYNAMIC_DEBUG */

/*
 * CAPWAP net device Name.
 */
#define NSS_CAPWAPMGR_NETDEV_NAME	"nsscapwap"

/*
 * NSS capwap mgr macros
 */
#define NSS_CAPWAPMGR_NORMAL_FRAME_MTU	1500

/*
 * Default CAPWAP re-assembly timeout 10 milli-seconds.
 */
#define NSS_CAPWAPMGR_REASSEMBLY_TIMEOUT 10
/*
 * Ethernet types.
 */
#define NSS_CAPWAPMGR_ETH_TYPE_MASK	0xFFFF
#define NSS_CAPWAPMGR_ETH_TYPE_TRUSTSEC	0x8909
#define NSS_CAPWAPMGR_ETH_TYPE_IPV4	ETH_P_IP
#define NSS_CAPWAPMGR_ETH_TYPE_IPV6	ETH_P_IPV6
#define NSS_CAPWAPMGR_DSCP_MAX	64

/*
 * Vlan tag not configured
 */
#define NSS_CAPWAPMGR_VLAN_TAG_NOT_CONFIGURED	0xFFF

/*
 * nss_capwapmgr_global
 *	Global structure for capwapmgr.
 */
struct nss_capwapmgr_global {
	uint32_t count;				/* Counter for driver queue selection. */
	struct nss_capwap_tunnel_stats tunneld_stats;	/* Stats from deleted capwap tunnels. */
};

#endif /* __NSS_CAPWAPMGR_H */
