# SPDX-License-Identifier: GPL-2.0
# The vendor Makefile's choices for this board (SDIO FullMAC, no Bluetooth, no
# debugfs), with request_firmware instead of opening files by path.
obj-y += aic8800_fdrv.o
aic8800_fdrv-y := \
	rwnx_msg_tx.o \
	rwnx_msg_rx.o \
	rwnx_utils.o \
	rwnx_cmds.o \
	rwnx_irqs.o \
	rwnx_cfgfile.o \
	rwnx_strs.o \
	rwnx_rx.o \
	rwnx_tx.o \
	rwnx_txq.o \
	rwnx_main.o \
	rwnx_mod_params.o \
	rwnx_mesh.o \
	rwnx_platform.o \
	rwnx_pci.o \
	rwnx_dini.o \
	rwnx_v7.o \
	ipc_host.o \
	rwnx_tdls.o \
	aic_vendor.o \
	md5.o \
	aicwf_compat_8800dc.o \
	aicwf_compat_8800d80.o \
	rwnx_wakelock.o \
	regdb.o \
	aic_priv_cmd.o \
	aicwf_rx_prealloc.o \
	rwnx_radar.o \
	sdio_host.o \
	aicwf_txrxif.o \
	aicwf_sdio.o \
	aicwf_tcp_ack.o

# Built in, both vendor modules share one namespace: fdrv's own copies of names
# the bsp defines too get a prefix, as each module's copy bound first before.
ccflags-y += -include $(src)/fdrv_names.h
ccflags-y += -I$(src)/../aic8800_bsp
# request_firmware finds the files built into the kernel; the path only names them in logs.
ccflags-y += -DCONFIG_AIC_FW_PATH=\"\"
ccflags-y += -DAICWF_SDIO_SUPPORT
ccflags-y += -DCONFIG_USE_FW_REQUEST
ccflags-y += -DCONFIG_RWNX_FULLMAC
ccflags-y += -DCONFIG_START_FROM_BOOTROM
ccflags-y += -DCONFIG_PMIC_SETTING
ccflags-y += -DCONFIG_VRF_DCDC_MODE
ccflags-y += -DCONFIG_ROM_PATCH_EN
ccflags-y += -DCONFIG_COEX
ccflags-y += -DCONFIG_RWNX_RADAR
ccflags-y += -DCONFIG_RWNX_DBG
ccflags-y += -DCONFIG_RFTEST
ccflags-y += -DDEFAULT_COUNTRY_CODE=\"US\"
ccflags-y += -DUSE_5G
ccflags-y += -DCONFIG_TXRX_THREAD_PRIO
ccflags-y += -DCONFIG_SUPPORT_REALTIME_CHANGE_MAC
ccflags-y += -DCONFIG_PREALLOC_RX_SKB
ccflags-y += -DCONFIG_PREALLOC_TXQ
ccflags-y += -DCONFIG_DPD -DCONFIG_FORCE_DPD_CALIB
ccflags-y += -DCONFIG_FILTER_TCP_ACK
ccflags-y += -DCONFIG_RESV_MEM_SUPPORT
ccflags-y += -DCONFIG_MCC
ccflags-y += -DCONFIG_SHUTDOWN_CALLBACK
ccflags-y += -DCONFIG_RX_NETIF_RECV_SKB
ccflags-y += -DAICWF_RX_REORDER
ccflags-y += -DAICWF_ARP_OFFLOAD
ccflags-y += -DCONFIG_USER_MAX=1
ccflags-y += -DNX_TXQ_CNT=5
ccflags-y += -DNX_VIRT_DEV_MAX=4
ccflags-y += -DNX_REMOTE_STA_MAX_FOR_OLD_IC=10
ccflags-y += -DNX_REMOTE_STA_MAX=32
ccflags-y += -DNX_MU_GROUP_MAX=62
ccflags-y += -DNX_TXDESC_CNT=64
ccflags-y += -DNX_TX_MAX_RATES=4
ccflags-y += -DNX_CHAN_CTXT_CNT=3
