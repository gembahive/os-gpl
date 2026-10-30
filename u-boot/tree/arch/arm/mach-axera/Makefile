# SPDX-License-Identifier: GPL-2.0+
obj-$(CONFIG_CPU_V7A) += ax620e.o
obj-$(CONFIG_ARM64) += ax620e64.o
obj-$(CONFIG_AX620E_CE_RNG) += ce_rng.o ce_fw.o

CE_FW := $(srctree)/arch/arm/mach-axera/ce-fw.bin
AFLAGS_ce_fw.o := -DCE_FW=\"$(CE_FW)\"
$(obj)/ce_fw.o: $(CE_FW)
