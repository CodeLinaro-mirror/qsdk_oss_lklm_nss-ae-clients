ccflags-y := -Wall -Werror
ccflags-y += -I$(obj)/include -I$(obj)/../exports -I$(obj)/..
ccflags-y += -DNSS_PPENL_DEBUG_LEVEL=4
ccflags-y += -DNSS_CLIENT_BUILD_ID="$(BUILD_ID)"
ccflags-y += -DCONFIG_NSS_PPENL_ACL=1
ccflags-y += -DCONFIG_NSS_PPENL_POLICER=1
ccflags-y += -DCONFIG_NSS_PPENL_QOS=1
ccflags-y += -DCONFIG_NSS_PPENL_EXCEPTION=1
ccflags-y += -DCONFIG_NSS_PPENL_VLAN=1

ifeq ($(SoC),$(filter $(SoC),ipq96xx ipq52xx))
ccflags-y += -DNSS_PPE_FEATURE_EXCEPTION_EDIT
ccflags-y += -DCONFIG_NSS_PPENL_DSCP=1
ccflags-y += -DCONFIG_NSS_PPENL_PM=1
ccflags-y += -DNSS_PPE_DSCP_PBIT_FEATURE_SUPPORT
ccflags-y += -DNSS_PPE_PM_COUNTER_FEATURE_SUPPORT
ccflags-y += -DNSS_PPE_EXT_VLAN_FEATURE_SUPPORT
ccflags-y += -DCONFIG_NSS_PPENL_PORT_MGMT=1
ccflags-y += -DNSS_PPE_PPENL_PORT_MGMT_SUPPORT
endif

qca-nss-ppe-netlink-objs := nss_ppenl.o
qca-nss-ppe-netlink-objs += nss_ppenl_acl.o
qca-nss-ppe-netlink-objs += nss_ppenl_policer.o
qca-nss-ppe-netlink-objs += nss_ppenl_qos.o
qca-nss-ppe-netlink-objs += nss_ppenl_exception.o

ifeq ($(SoC),$(filter $(SoC),ipq96xx ipq52xx))
qca-nss-ppe-netlink-objs += nss_ppenl_dscp.o
qca-nss-ppe-netlink-objs += nss_ppenl_pm.o
qca-nss-ppe-netlink-objs += nss_ppenl_port_mgmt.o
qca-nss-ppe-netlink-objs += nss_ppenl_vlan.o
endif

obj-m += qca-nss-ppe-netlink.o
