/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: ISC
 */

/**
 * @file ppe_port_mgmt.h
 *      NSS PPE VLAN definitions.
 */

#include <linux/if.h>
#include <linux/if_ether.h>

#ifndef _PPE_PORT_MGMT_H_
#define _PPE_PORT_MGMT_H_

#define PPE_PORT_MGMT_OMCI_ID_MAX	0x40
#define PPE_PORT_MGMT_ACT_ARR_SIZE	16
#define PPE_PORT_MGMT_PORT_MAX		256

/*
 * ppe_port_mgmt_ret
 *	Return type
 */
typedef enum ppe_port_mgmt_ret {
	PPE_PORT_MGMT_RET_SUCCESS = 0,				/**< API call successful */
	PPE_PORT_MGMT_RET_FAILURE,				/**< API call failed */
	PPE_PORT_MGMT_ISOL_RET_FAILURE,				/**< Port isolation get/set failed */
} ppe_port_mgmt_ret_t;

/**
 * ppe_port_mgmt_isol
 *	Port isolation configuration
 */
struct ppe_port_mgmt_isol {
	uint32_t port_id;					/**< Port id */
	uint32_t omci_id;					/**< OMCI id */
	char port_name[IFNAMSIZ];				/**< Port Name */
	bool mc_isol_en;					/**< Multicast Isolation Enable */
	bool bc_isol_en;					/**< Broadcast Isolation Enable */
	ppe_port_mgmt_ret_t ret;				/**< PORT_MGMT return type. */
};

/**
 * ppe_port_mgmt_isol_config()
 *	Set port isolation configuration
 *
 * @param[in] isol_info		Port isolation configuration
 *
 * @return
 * Status of the operation, indicating success or failure.
 */
ppe_port_mgmt_ret_t ppe_port_mgmt_isol_config(struct ppe_port_mgmt_isol *isol_info);

/**
 * ppe_port_mgmt_act_ctrl_set()
 *	Set Action Control configuration
 *
 * @param[in] isol_info		Port isolation configuration
 *
 * @return
 * Status of the operation, indicating success or failure.
 */
ppe_port_mgmt_ret_t ppe_port_mgmt_act_ctrl_set(struct ppe_port_mgmt_isol *isol_info);

/**
 * ppe_port_mgmt_default_isol_set()
 *	Set Default isolation configuration
 *
 * @return
 * Status of the operation, indicating success or failure.
 */
ppe_port_mgmt_ret_t ppe_port_mgmt_default_isol_set(void);
#endif // _PPE_PORT_MGMT_H_
