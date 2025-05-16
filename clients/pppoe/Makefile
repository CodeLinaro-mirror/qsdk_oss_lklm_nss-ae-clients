# Makefile for pppoe client
obj-m += qca-nss-pppoe-mgr.o
qca-nss-pppoe-mgr-y := pppoe_mgr.o
ccflags-y += -Wall -Werror
ccflags-y += -DPPPOE_MGR_DEBUG_LEVEL=0
ccflags-y += -I$(obj)/../include

# Makefile changes for PPE frontend
qca-nss-pppoe-mgr-$(PPPOE_MGR_FE_PPE_ENABLE) += frontends/ppe/ppe_pppoe_mgr.o
ccflags-$(PPPOE_MGR_FE_PPE_ENABLE) += -DPPPOE_MGR_FE_PPE_ENABLE
ccflags-$(PPPOE_MGR_FE_PPE_ENABLE) += -I$(obj)/../exports -I$(obj)/.. -I$(obj)/../../drv/exports
ccflags-$(PPPOE_MGR_FE_PPE_ENABLE) += -I$(obj)/frontends/ppe -I$(obj)

ifneq (,$(filter $(CONFIG_BONDING),y m))
ccflags-y += -DBONDING_SUPPORT
endif
