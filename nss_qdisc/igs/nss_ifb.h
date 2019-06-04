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

/*
 * nss_ifb_info
 *	IFB and its mapped interface's bind structure.
 */
struct nss_ifb_info {
	struct net_device *ifb_dev;	/* IFB device */
	struct net_device *map_dev;	/* Device mapped to an IFB device */
	struct list_head map_list;	/* Internal list */
	bool is_mapped;			/* Flag to indicate whether mapping is valid or not */
};

/*
 * nss_ifb_is_mapped()
 *	Returns the map status of the given ifb bind structure.
 */
extern bool nss_ifb_is_mapped(struct nss_ifb_info *ifb_info);

/*
 * nss_ifb_find_dev()
 *	Find and return the IFB netdev in the ifb list.
 */
extern struct nss_ifb_info *nss_ifb_find_dev(struct net_device *dev);

/*
 * nss_ifb_bind()
 *	API to bind an IFB device with its requested mapped interface.
 */
extern int32_t nss_ifb_bind(struct nss_ifb_info *ifb_info, struct net_device *from_dev,
		struct net_device *to_dev);

/*
 * nss_ifb_delete_if()
 *	Delete an IFB interface in NSS Firmware.
 */
extern void nss_ifb_delete_if(int32_t if_num);

/*
 * nss_ifb_create_if()
 *	Create an IFB interface in NSS Firmware.
 */
extern int32_t nss_ifb_create_if(struct net_device *dev);
