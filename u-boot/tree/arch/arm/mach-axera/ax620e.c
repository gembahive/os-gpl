// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2026, GembaHive, Inc.
 */
/*
 * Axera AX620E in AArch32. U-Boot runs straight after the vendor's first-stage
 * loader, which leaves the CPU in secure SVC with the MMU off, DRAM trained
 * and the system counter running, CNTFRQ set (24 MHz). Secure, so it reads
 * the reset latch and the eFuse shadow itself (mach/ax620e.h).
 * Register addresses are the GPL 4.19 SDK's.
 */

#include <cpu_func.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <mach/ax620e.h>

#define BOOT_FLAGS		0x02390024	/* the loader's, kept over a reset */
#define   BOOT_KERNEL_FAIL	BIT(7)
#define   BOOT_DOWNLOAD		BIT(8)
#define   CHIP_RST_SW		BIT(0)	/* in ABORT_CFG (mach/ax620e.h) */

/* Lowlevel init is skipped (the loader's); turning the caches on is ours. */
void enable_caches(void)
{
	icache_enable();
	dcache_enable();
}

/* A software chip reset: the next boot reads it as one (ABORT_STATUS bit 0). */
void reset_cpu(void)
{
	clrbits_le32(BOOT_FLAGS, BOOT_DOWNLOAD | BOOT_KERNEL_FAIL);
	setbits_le32(ABORT_CFG, CHIP_RST_SW);
	for (;;)
		;
}
