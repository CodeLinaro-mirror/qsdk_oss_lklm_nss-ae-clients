# Makefile for the PPE drivers

ccflags-y := -I$(obj) -I$(obj)/.. -I$(obj)/../..

ifneq ($(ppe-vp),)
obj-m += ppe_vp/
else
obj-m += ppe_drv/
endif
obj ?= .
