/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: ISC
 */

/**
 * @file ppe_drv_port_mgmt.h
 *	NSS PPE DRV Port Management definitions.
 */

#include <linux/if_ether.h>
#include <ppe_drv.h>

#ifndef _PPE_DRV_PORT_MGMT_H_
#define _PPE_DRV_PORT_MGMT_H_

/*
 * PORT_MGMT ISOL table rule flags.
 */
#define PPE_DRV_OMCI_INDEX_MAX						255
#define PPE_DRV_PORT_MGMT_ACT_ARR_SIZE					4
#define PPE_DRV_VP_PROFILE_ID_MAX					64
#define PPE_DRV_PORT_MGMT_ISOL_RULE_FLAG_PORT_ID			0x1

/**
 * ppe_drv_vp_profile_id_info
 *	VP (Virtual Port) profile‑ID mapping information.
 */
struct ppe_drv_vp_profile_id_info {
	uint8_t profile_id;	     /**< VP profile ID */
	uint32_t omci_id;	     /**< OMCI entity ID associated with the VP */
	uint32_t port_id;	     /**< Physical port ID mapped to this VP profile */
	bool in_use;		     /**< Indicates whether this profile entry is active */
};

/**
 * ppe_drv_omci_index_info
 *	OMCI index mapping information for port‑management operations.
 */
struct ppe_drv_omci_index_info {
	uint16_t omci_index;	     /**< Internal OMCI index */
	uint32_t omci_id;	     /**< OMCI entity ID */
	uint32_t port_cnt;	     /**< Number of ports associated with this OMCI entity */
	bool in_use;		     /**< Indicates whether this OMCI index entry is active */
};

/*
 * ppe_drv_port_mgmt_ret
 *	Return type for port‑management APIs.
 */
typedef enum ppe_drv_port_mgmt_ret {
	PPE_DRV_PORT_MGMT_RET_SUCCESS = 0,   /**< API call successful */
	PPE_DRV_PORT_MGMT_RET_FAILURE,	     /**< API call failed */
	PPE_DRV_PORT_MGMT_ISOL_RET_FAILURE,  /**< Port isolation get/set failed */
} ppe_drv_port_mgmt_ret_t;

/**
 * ppe_drv_port_mgmt_isol
 *	Port isolation configuration.
 */
struct ppe_drv_port_mgmt_isol {
	uint32_t port_id;		  /**< Port ID */
	uint32_t omci_id;		  /**< OMCI ID */
	bool mc_isol_en;		  /**< Multicast isolation enable */
	bool bc_isol_en;		  /**< Broadcast isolation enable */
	ppe_drv_port_mgmt_ret_t ret;	  /**< Port‑management return type */
};

/*
 * ppe_drv_port_mgmt
 *	Structure for global port‑management context.
 */
struct ppe_drv_port_mgmt {
	spinlock_t lock;							   	/**< Spinlock for port‑management operations */
	struct list_head active_rules;						  	/**< List of active port‑management rules */

	uint64_t port_phy_isol_bitmap[PPE_DRV_OMCI_INDEX_MAX];			  	/**< PHY‑port isolation bitmap for each OMCI index */
	uint64_t port_vp_isol_bitmap[PPE_DRV_OMCI_INDEX_MAX];			  	/**< VP‑port isolation bitmap for each OMCI index */

	uint32_t port_phy_isol_action[PPE_DRV_OMCI_INDEX_MAX][PPE_DRV_PORT_MGMT_ACT_ARR_SIZE];
											/**< PHY isolation action table */
	uint32_t port_vp_isol_action[PPE_DRV_OMCI_INDEX_MAX][PPE_DRV_PORT_MGMT_ACT_ARR_SIZE];
											/**< VP isolation action table */
	uint8_t port_omci_index_map[PPE_DRV_PORTS_MAX];				  	/**< Mapping from port ID to OMCI index */
	struct ppe_drv_vp_profile_id_info vp_profile_id_info[PPE_DRV_VP_PROFILE_ID_MAX];
											/**< VP profile‑ID information table */
	struct ppe_drv_omci_index_info omci_index_info[PPE_DRV_OMCI_INDEX_MAX];   	/**< OMCI index mapping information table */
	bool mc_isol_en;							   	/**< Global multicast isolation enable */
	bool bc_isol_en;							   	/**< Global broadcast isolation enable */
	uint8_t us_port_id;							  	/**< Upstream port ID */
};

/**
 * ppe_drv_port_mgmt_act_ctrl_set()
 *	Configures the action control parameters for port‑management‑based
 *	isolation or forwarding behavior.
 *
 * @datatypes
 * ppe_drv_port_isolation_info_t
 *
 * @param[in] isol_info       Pointer to port‑management isolation/action
 *			      control configuration structure.
 *
 * @return
 * ppe_drv_ret_t	      Status of the action control configuration
 *			      (success or failure).
 */
ppe_drv_ret_t ppe_drv_port_mgmt_act_ctrl_set(struct ppe_drv_port_mgmt_isol *isol_info);

/**
 * ppe_drv_port_mgmt_isol_configure()
 *	Applies isolation configuration for a given port or port group.
 *	This function sets up ingress/egress isolation rules based on
 *	the supplied isolation information.
 *
 * @datatypes
 * ppe_drv_port_isolation_info_t
 *
 * @param[in] isol_info       Pointer to isolation configuration parameters
 *			      for port‑management operations.
 *
 * @return
 * ppe_drv_ret_t	      Status of isolation configuration update.
 */
ppe_drv_ret_t ppe_drv_port_mgmt_isol_configure(struct ppe_drv_port_mgmt_isol *isol_info);

/**
 * ppe_drv_port_mgmt_default_isol_set()
 *	Sets the default port isolation behavior across the system.
 *	Ensures that baseline isolation rules are in place before
 *	applying specific port‑level isolation policies.
 *
 * @return
 * ppe_drv_ret_t	      Status of default isolation configuration.
 */
ppe_drv_ret_t ppe_drv_port_mgmt_default_isol_set(void);
#endif // _PPE_DRV_PORT_MGMT_H_
