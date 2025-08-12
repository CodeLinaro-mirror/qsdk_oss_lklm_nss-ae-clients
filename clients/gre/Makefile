#Makefile for PPE GRE client
ifeq ($(GRE_MGR_FE_PPE_ENABLE),y)
obj-m += qca-nss-ppe-gre.o
qca-nss-ppe-gre-y := gre_mgr.o
qca-nss-ppe-gre-y += frontends/ppe/ppe_gre_mgr.o frontends/ppe/ppe_gre_mgr_stats.o
ccflags-y += -I$(obj)/frontends/ppe
ccflags-y += -I$(obj)/../../exports -I$(obj)/../../drv/exports
ccflags-y += -DGRE_MGR_FE_PPE_ENABLE
endif

ccflags-y += -I$(obj) -I$(obj)/../include
ccflags-y += -DGRE_MGR_DEBUG_LEVEL=0
ccflags-y += -Wall -Werror

obj ?= .

#The below 5 lines will be removed once all the gerrits of name change is merged
obj-m += qca-nss-gre-mgr.o
qca-nss-gre-mgr-y := gre_mgr.o
qca-nss-gre-mgr-y += frontends/ppe/ppe_gre_mgr.o frontends/ppe/ppe_gre_mgr_stats.o
ccflags-y += -I$(obj)/frontends/ppe -I$(obj)/../../exports -I$(obj)/../../drv/exports
ccflags-y += -DGRE_MGR_FE_PPE_ENABLE
