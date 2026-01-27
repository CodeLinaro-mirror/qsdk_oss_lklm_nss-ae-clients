# Makefile for PPE RULE module

ccflags-y := -I$(obj)/../exports -I$(obj)/..
ccflags-y += -DPPE_DRV_BUILD_ID="$(BUILD_ID)"
ccflags-y += -Wall -Werror

ifeq ($(PPE_RULE_IPQ53XX), y)
ccflags-y += -DPPE_RULE_FLOW_TBL_POLICER
endif

ifeq ($(PPE_RULE_IPQ54XX), y)
ccflags-y += -DPPE_RULE_FLOW_TBL_POLICER
endif

ifeq ($(PPE_LOWMEM_PROFILE_16M), y)
ccflags-y += -DNSS_PPE_LOWMEM_PROFILE_16M
endif

ifeq ($(PPE_LOWMEM_PROFILE_256M), y)
ccflags-y += -DNSS_PPE_LOWMEM_PROFILE_256M
endif

ifeq ($(SoC),$(filter $(SoC),ipq96xx ipq52xx))
ccflags-y += -DNSS_PPE_FEATURE_EXCEPTION_EDIT
ccflags-y += -DNSS_PPE_EXT_VLAN_FEATURE_SUPPORT
endif

ifeq ($(SoC),$(filter $(SoC),ipq53xx ipq52xx))
ccflags-y += -DNSS_PPE_ACL_RULE_RANGE
endif

ifeq ($(SoC),$(filter $(SoC),ipq96xx ipq52xx))
ccflags-y += -DPPE_ACL_DEST_BIND_SUPPORT
endif

obj-m += qca-nss-ppe-rule.o
qca-nss-ppe-rule-objs := ppe_rule.o

ifeq ($(PPE_RFS_ENABLED), y)
qca-nss-ppe-rule-objs += ppe_rfs/ppe_rfs.o
qca-nss-ppe-rule-objs += ppe_rfs/ppe_rfs_stats.o
ccflags-y += -DPPE_RFS_DEBUG_LEVEL=3
endif

ifndef PPE_LOWMEM_PROFILE_16M
ifeq ($(PPE_ACL_ENABLED), y)
qca-nss-ppe-rule-objs += ppe_acl/ppe_acl.o
qca-nss-ppe-rule-objs += ppe_acl/ppe_acl_dump.o
qca-nss-ppe-rule-objs += ppe_acl/ppe_acl_stats.o
ccflags-y += -DPPE_ACL_DEBUG_LEVEL=3
endif

ifeq ($(PPE_POLICER_ENABLED), y)
qca-nss-ppe-rule-objs += ppe_policer/ppe_policer.o
qca-nss-ppe-rule-objs += ppe_policer/ppe_policer_dump.o
qca-nss-ppe-rule-objs += ppe_policer/ppe_policer_stats.o
ccflags-y += -DPPE_POLICER_DEBUG_LEVEL=3
endif

ifeq ($(PPE_QOS_ENABLED), y)
qca-nss-ppe-rule-objs += ppe_qos/ppe_qos.o
ccflags-y += -DPPE_QOS_DEBUG_LEVEL=3
endif

ifeq ($(PPE_DSCP_ENABLED), y)
qca-nss-ppe-rule-objs += ppe_dscp/ppe_dscp.o
qca-nss-ppe-rule-objs += ppe_dscp/ppe_dscp_dump.o
ccflags-y += -DPPE_DSCP_DEBUG_LEVEL=3
ccflags-y += -DNSS_PPE_DSCP_PBIT_FEATURE_SUPPORT
endif

ifeq ($(PPE_PM_ENABLED), y)
qca-nss-ppe-rule-objs += ppe_pm/ppe_pm.o
qca-nss-ppe-rule-objs += ppe_pm/ppe_pm_stats.o
qca-nss-ppe-rule-objs += ppe_pm/ppe_pm_dump.o
ccflags-y += -DPPE_PM_DEBUG_LEVEL=3
ccflags-y += -DNSS_PPE_PM_COUNTER_FEATURE_SUPPORT
endif

ifeq ($(PPE_VLAN_ENABLED), y)
qca-nss-ppe-rule-objs += ppe_vlan/ppe_vlan.o
qca-nss-ppe-rule-objs += ppe_vlan/ppe_vlan_dump.o
qca-nss-ppe-rule-objs += ppe_vlan/ppe_vlan_stats.o
ccflags-y += -DPPE_VLAN_DEBUG_LEVEL=3
endif
endif

ifndef PPE_LOWMEM_PROFILE_16M
ifndef PPE_LOWMEM_PROFILE_256M
ifeq ($(PPE_MIRROR_ENABLED), y)
qca-nss-ppe-rule-objs += ppe_mirror/ppe_mirror.o
qca-nss-ppe-rule-objs += ppe_mirror/ppe_mirror_stats.o
ccflags-y += -DPPE_MIRROR_DEBUG_LEVEL=3
endif

ifeq ($(PPE_PRIORITY_ENABLED), y)
qca-nss-ppe-rule-objs += ppe_priority/ppe_priority.o
qca-nss-ppe-rule-objs += ppe_priority/ppe_priority_stats.o
ccflags-y += -DPPE_PRIORITY_DEBUG_LEVEL=3
endif
endif
endif
