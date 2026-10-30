/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (c) 2026, GembaHive, Inc.
 */
#ifndef __AX620E_H
#define __AX620E_H

#include <bootchain.h>

#define CFG_SYS_SDRAM_BASE	0x40000000

/* A/B boot state: the only variables read back from storage (ENV_WRITEABLE_LIST). */
#define CFG_ENV_FLAGS_LIST_STATIC \
	"BOOT_ORDER:sw,BOOT_A_LEFT:dw,BOOT_B_LEFT:dw,BOOT_POWER_ONS:dw"
/* Both slots on trial until Linux confirms one. */
#define CFG_EXTRA_ENV_SETTINGS	BOOTCHAIN_DEFAULT_ENV

#endif
