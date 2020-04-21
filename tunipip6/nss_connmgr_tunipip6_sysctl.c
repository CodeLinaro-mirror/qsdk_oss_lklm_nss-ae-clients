 /*
 **************************************************************************
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.

 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.

 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 **************************************************************************
 */

#include <linux/version.h>
#include <linux/types.h>
#include <linux/ip.h>
#include <linux/of.h>
#include <linux/tcp.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <net/ipv6.h>
#include <net/ip_tunnels.h>
#include <net/ip6_tunnel.h>
#include <linux/if_arp.h>
#include <nss_api_if.h>
#include <linux/sysctl.h>
#include <linux/printk.h>
#include <linux/inet.h>
#include "nss_connmgr_tunipip6.h"

#define MAX_PROC_SIZE 1024
#define MAX_DATA_LEN 500
#define MAX_IPV4_PREFIX_LEN 32
#define MAX_IPV6_PREFIX_LEN 128
#define MAX_PSID_OFFSET_LEN 15
#define NETDEV_STR_LEN 30
#define PREFIX_STR_LEN 100

unsigned char nss_tunipip6_data[MAX_DATA_LEN] __read_mostly;
enum nss_tunipip6_sysctl_mode {
	NSS_TUNIPIP6_SYSCTL_ADD_FMR,
	NSS_TUNIPIP6_SYSCTL_DEL_FMR,
	NSS_TUNIPIP6_SYSCTL_FLUSH_FMR,
};


static int nss_tunipip6_data_parser(struct ctl_table *ctl, int write, void __user *buffer, size_t *lenp, loff_t *ppos, enum nss_tunipip6_sysctl_mode mode)
{
	char dev_name[NETDEV_STR_LEN] = {0}, ipv6_prefix_str[PREFIX_STR_LEN] = {0}, ipv6_suffix_str[PREFIX_STR_LEN] = {0}, ipv4_prefix_str[PREFIX_STR_LEN] = {0};
	uint32_t ipv6_prefix[4], ipv6_prefix_len, ipv6_suffix[4], ipv6_suffix_len, ipv4_prefix, ipv4_prefix_len, ea_len, psid_offset;
	bool ipv6_prefix_valid = false, ipv6_prefix_len_valid = false, ipv6_suffix_valid = false;
	bool ipv4_prefix_valid = false, ipv4_prefix_len_valid = false, ipv6_suffix_len_valid = false;
	bool ea_len_valid = false, psid_offset_valid = false, netdev_valid = false;
	struct nss_connmgr_tunipip6_fmr_cfg fmrcfg = {0};
	char *buf = kzalloc(MAX_PROC_SIZE, GFP_KERNEL);
	enum nss_connmgr_tunipip6_err_codes status;
	struct net_device *dev = NULL;
	char *pfree;
	char *token;
	int ret;
	int count;


	if (!buf) {
		return -ENOMEM;
	}
	pfree = buf;
	count = *lenp;
	if (count > MAX_PROC_SIZE) {
		count = MAX_PROC_SIZE;
	}

	if (copy_from_user(buf, buffer, count)) {
		kfree(pfree);
		return -EFAULT;
	}

	while (buf) {
		char *param, *value;
		token = strsep(&buf, " \n");
		if (token[0] == 0) {
			continue;
		}

		param = strsep(&token, "=");
		value = token;

		/*
		 * Parse netdev and FMR parameters.
		 */

		if (!strcmp(param, "netdev")) {
			strlcpy(dev_name, value, 30);
			dev = dev_get_by_name(&init_net, dev_name);
			if (!dev) {
				kfree(pfree);
				goto fail;
			}
			netdev_valid = true;
			continue;
		}

		if (!strcmp(param, "ipv4_prefix")) {
			strlcpy(ipv4_prefix_str, value, 30);
			ret = in4_pton(ipv4_prefix_str, -1, (uint8_t *)&ipv4_prefix, -1, NULL);
			if (ret != 1) {
				kfree(pfree);
				goto fail;
			}
			ipv4_prefix_valid = true;
			continue;
		}

		if (!strcmp(param, "ipv4_prefix_len")) {
			if (!sscanf(value, "%u", &ipv4_prefix_len)) {
				kfree(pfree);
				goto fail;
			}

			if (ipv4_prefix_len > MAX_IPV4_PREFIX_LEN) {
				kfree(pfree);
				goto fail;
			}

			ipv4_prefix_len_valid = true;
			continue;
		}

		if (!strcmp(param, "ipv6_prefix")) {
			strlcpy(ipv6_prefix_str, value, 100);
			ret = in6_pton(ipv6_prefix_str, -1, (uint8_t *)&ipv6_prefix, -1, NULL);
			if (ret != 1) {
				kfree(pfree);
				goto fail;
			}
			ipv6_prefix_valid = true;
			continue;
		}

		if (!strcmp(param, "ipv6_prefix_len")) {
			if (!sscanf(value, "%u", &ipv6_prefix_len)) {
				kfree(pfree);
				goto fail;
			}

			if (ipv6_prefix_len > MAX_IPV6_PREFIX_LEN) {
				kfree(pfree);
				goto fail;
			}

			ipv6_prefix_len_valid = true;
			continue;
		}

		if (!strcmp(param, "ipv6_suffix")) {
			strlcpy(ipv6_suffix_str, value, 100);
			ret = in6_pton(ipv6_suffix_str, -1, (uint8_t *)&ipv6_suffix, -1, NULL);
			if (ret != 1) {
				kfree(pfree);
				goto fail;
			}
			ipv6_suffix_valid = true;
			continue;
		}

		if (!strcmp(param, "ipv6_suffix_len")) {
			if (!sscanf(value, "%u", &ipv6_suffix_len)) {
				kfree(pfree);
				goto fail;
			}

			if (ipv6_suffix_len > MAX_IPV6_PREFIX_LEN) {
				kfree(pfree);
				goto fail;
			}

			ipv6_suffix_len_valid = true;
			continue;
		}

		if (!strcmp(param, "ea_len")) {
			if (!sscanf(value, "%u", &ea_len)) {
				kfree(pfree);
				goto fail;
			}

			ea_len_valid = true;
			continue;
		}

		if (!strcmp(param, "psid_offset")) {
			if (!sscanf(value, "%u", &psid_offset)) {
				kfree(pfree);
				goto fail;
			}

			if (psid_offset> MAX_PSID_OFFSET_LEN) {
				kfree(pfree);
				goto fail;
			}

			psid_offset_valid = true;
			continue;
		}
	}

	kfree(pfree);

	if (!netdev_valid) {
		goto fail;
	}

	switch(mode) {
	case NSS_TUNIPIP6_SYSCTL_ADD_FMR:
	case NSS_TUNIPIP6_SYSCTL_DEL_FMR:
		if (!(ipv6_prefix_valid && ipv6_prefix_len_valid && ipv6_suffix_valid &&
			ipv4_prefix_valid && ipv4_prefix_len_valid && ea_len_valid &&
			psid_offset_valid)) {
			goto fail;
		}

		fmrcfg.ipv6_prefix[0] = ntohl(ipv6_prefix[0]);
		fmrcfg.ipv6_prefix[1] = ntohl(ipv6_prefix[1]);
		fmrcfg.ipv6_prefix[2] = ntohl(ipv6_prefix[2]);
		fmrcfg.ipv6_prefix[3] = ntohl(ipv6_prefix[3]);
		fmrcfg.ipv6_prefix_len = ipv6_prefix_len;

		fmrcfg.ipv4_prefix = ntohl(ipv4_prefix);
		fmrcfg.ipv4_prefix_len = ipv4_prefix_len;

		fmrcfg.ipv6_suffix[0] = ntohl(ipv6_suffix[0]);
		fmrcfg.ipv6_suffix[1] = ntohl(ipv6_suffix[1]);
		fmrcfg.ipv6_suffix[2] = ntohl(ipv6_suffix[2]);
		fmrcfg.ipv6_suffix[3] = ntohl(ipv6_suffix[3]);
		fmrcfg.ipv6_suffix_len = ipv6_suffix_len;

		fmrcfg.ea_len = ea_len;
		fmrcfg.psid_offset = psid_offset;

		if (mode == NSS_TUNIPIP6_SYSCTL_ADD_FMR) {
			status = nss_connmgr_tunipip6_add_fmr(dev, &fmrcfg);
			if (status == NSS_CONNMGR_TUNIPIP6_SUCCESS) {
				pr_info("FMR create success for netdev: %s\n", dev->name);
			} else {
				pr_info("FMR create failure for netdev: %s\n", dev->name);
			}
		} else {
			status = nss_connmgr_tunipip6_del_fmr(dev, &fmrcfg);
			if (status == NSS_CONNMGR_TUNIPIP6_SUCCESS) {
				pr_info("FMR delete success for netdev: %s\n", dev->name);
			} else {
				pr_info("FMR delete failure for netdev: %s\n", dev->name);
			}
		}
		break;

	case NSS_TUNIPIP6_SYSCTL_FLUSH_FMR:
		status = nss_connmgr_tunipip6_flush_fmr(dev);
		if (status == NSS_CONNMGR_TUNIPIP6_SUCCESS) {
			pr_info("FMR flush success for netdev: %s\n", dev->name);
		} else {
			pr_info("FMR flush failed for netdev: %s\n", dev->name);
		}
		break;
	}

	dev_put(dev);
	return 0;

fail:
	if (dev) {
		dev_put(dev);
	}

	pr_info("Wrong input, check help. (cat /proc/sys/dev/nss/ipip6/help)\n");
	return 0;
}

static int nss_tunipip6_cmd_procfs_add_fmr(struct ctl_table *ctl, int write, void __user *buffer, size_t *lenp, loff_t *ppos)
{
	return nss_tunipip6_data_parser(ctl, write, buffer, lenp, ppos, NSS_TUNIPIP6_SYSCTL_ADD_FMR);
}

static int nss_tunipip6_cmd_procfs_del_fmr(struct ctl_table *ctl, int write, void __user *buffer, size_t *lenp, loff_t *ppos)
{
	return nss_tunipip6_data_parser(ctl, write, buffer, lenp, ppos, NSS_TUNIPIP6_SYSCTL_DEL_FMR);
}

static int nss_tunipip6_cmd_procfs_flush_fmr(struct ctl_table *ctl, int write, void __user *buffer, size_t *lenp, loff_t *ppos)
{
	return nss_tunipip6_data_parser(ctl, write, buffer, lenp, ppos, NSS_TUNIPIP6_SYSCTL_FLUSH_FMR);
}

static int nss_tunipip6_cmd_procfs_read_help(struct ctl_table *ctl, int write, void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int ret = proc_dointvec(ctl, write, buffer, lenp, ppos);

	pr_info("\nHelp: (/proc/sys/dev/nss/ipip6/help) \n\
			1. To add FMR:\n\
			echo dev=<map-mape/MAP-E netdevice> ipv6_prefix=<XXXX::XXXX> ipv6_prefix_len=<XX> ipv4_prefix=<X.X.X.X> ipv4_prefix_len=<XX> \n\
			ipv6_suffix=<XXXX::XXXX> ipv6_sufffix_len=<XX> ea_len=<XX> psid_offset=<XX> > add_fmr \n\
			2. To delete FMR:\n\
			echo dev=<map-mape/MAP-E netdevice> ipv6_prefix=<XXXX::XXXX> ipv6_prefix_len=<XX> ipv4_prefix=<X.X.X.X> ipv4_prefix_len=<XX> \n\
			ipv6_suffix=<XXXX::XXXX> ipv6_sufffix_len=<XX> ea_len=<XX> psid_offset=<XX> > delete_fmr \n\
			3. To flush `FMR:\n\
			echo dev=<map-mape/MAP-E netdevice> > flush_fmr\n");

	*lenp = 0;
	return ret;
}

static struct ctl_table nss_tunipip6_table[] = {
	{
		.procname		= "add_fmr",
		.data			= &nss_tunipip6_data,
		.maxlen			= sizeof(nss_tunipip6_data),
		.mode			= 0644,
		.proc_handler		= &nss_tunipip6_cmd_procfs_add_fmr,
	},
	{
		.procname		= "del_fmr",
		.data			= &nss_tunipip6_data,
		.maxlen			= sizeof(nss_tunipip6_data),
		.mode			= 0644,
		.proc_handler		= &nss_tunipip6_cmd_procfs_del_fmr,
	},
	{
		.procname		= "flush_fmr",
		.data			= &nss_tunipip6_data,
		.maxlen			= sizeof(nss_tunipip6_data),
		.mode			= 0644,
		.proc_handler		= &nss_tunipip6_cmd_procfs_flush_fmr,
	},
	{
		.procname		= "help",
		.data			= &nss_tunipip6_data,
		.maxlen			= sizeof(nss_tunipip6_data),
		.mode			= 0400,
		.proc_handler		= &nss_tunipip6_cmd_procfs_read_help,
	},
	{ }
};

static struct ctl_table nss_tunipip6_root_dir[] = {
	{
		.procname		= "ipip6",
		.mode			= 0555,
		.child			= nss_tunipip6_table,
	},
	{ }
};

static struct ctl_table nss_tunipip6_nss_root_dir[] = {
	{
		.procname		= "nss",
		.mode			= 0555,
		.child			= nss_tunipip6_root_dir,
	},
	{ }
};

static struct ctl_table nss_tunipip6_root[] = {
	{
		.procname		= "dev",
		.mode			= 0555,
		.child			= nss_tunipip6_nss_root_dir,
	},
	{ }
};

static struct ctl_table_header *nss_tunipip6_ctl_header;

/*
 * nss_tunipip6_sysctl_register()
 * 	Register command line interface for tunipip6.
 */
bool nss_tunipip6_sysctl_register(void) {
	nss_tunipip6_ctl_header = register_sysctl_table(nss_tunipip6_root);
	if (!nss_tunipip6_ctl_header) {
		return false;
	}

	return true;
}

/*
 * nss_tunipip6_sysctl_unregister()
 * 	Unregister command line interface for tunipip6.
 */
void nss_tunipip6_sysctl_unregister(void) {
	if (nss_tunipip6_ctl_header) {
		unregister_sysctl_table(nss_tunipip6_ctl_header);
	}
}
