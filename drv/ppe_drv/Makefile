# Makefile for PPE driver

ccflags-y := -I$(obj)/../exports -I$(obj)/..
ccflags-y += -DNSS_PPE_BUILD_ID="$(BUILD_ID)"

obj-m += qca-nss-ppe.o
qca-nss-ppe-objs := ppe_drv.o

ccflags-y += -Wall -Werror
