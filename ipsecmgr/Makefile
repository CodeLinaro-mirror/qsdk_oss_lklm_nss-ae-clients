# Makefile for the ipsecmgr client

ccflags-y := -I$(obj) -I$(obj)/..
ccflags-y += -DNSS_CLIENT_BUILD_ID="$(BUILD_ID)"
ccflags-y += -Werror
ifeq ($(SoC), fsm9010)
	ccflags-y += -DNSS_IPSECMGR_PMTU_SUPPORT
endif

obj-m += qca-nss-ipsecmgr.o
qca-nss-ipsecmgr-objs := nss_ipsecmgr.o
qca-nss-ipsecmgr-objs += nss_ipsecmgr_flow.o
qca-nss-ipsecmgr-objs += nss_ipsecmgr_sa.o
qca-nss-ipsecmgr-objs += nss_ipsecmgr_subnet.o
ccflags-y += -DNSS_IPSECMGR_DEBUG_LEVEL=3

ifeq (ipq807x, $(findstring ipq807x, $(SoC)))
ccflags-y += -DNSS_IPSECMGR_IPQ807X_SUPPORT
endif

obj ?= .
