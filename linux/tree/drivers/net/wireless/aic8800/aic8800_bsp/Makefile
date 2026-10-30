# SPDX-License-Identifier: GPL-2.0
# The vendor Makefile's choices for this board (SDIO, no Bluetooth), with
# request_firmware instead of opening files by path.
obj-y += aic8800_bsp.o
aic8800_bsp-y := \
	aic8800dc_compat.o \
	aic8800d80_compat.o \
	aic8800d80n_compat.o \
	aic8800d80x2_compat.o \
	aic_bsp_main.o \
	aic_bsp_driver.o \
	aicsdio.o \
	aicsdio_txrxif.o \
	aicwf_txq_prealloc.o \
	md5.o

# request_firmware finds the files built into the kernel; the path only names them in logs.
ccflags-y += -DCONFIG_AIC_FW_PATH=\"\"
ccflags-y += -DAICWF_SDIO_SUPPORT
ccflags-y += -DCONFIG_USE_FW_REQUEST
ccflags-y += -DCONFIG_LINK_DET_5G
ccflags-y += -DCONFIG_VRF_DCDC_MODE
ccflags-y += -DCONFIG_PREALLOC_TXQ
ccflags-y += -DCONFIG_DPD -DCONFIG_FORCE_DPD_CALIB
ccflags-y += -DCONFIG_RESV_MEM_SUPPORT
ccflags-y += -DCONFIG_AMSDU_RX
