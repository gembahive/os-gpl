/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (c) 2026, GembaHive, Inc.
 */
/*
 * What the board code asks of the arch half. AArch32 reads the chip itself,
 * inline; AArch64, Non-secure under TF-A, reads it from SPL64's handoff block.
 */
#ifndef __MACH_AX620E_H
#define __MACH_AX620E_H

#include <linux/types.h>

/*
 * AArch64 only: tells BL31 that BL33 came up (SiP BL33_UP, BACKUP1 bit 31,
 * which SPL64 reads at the next boot). Once, from board_late_init. Declared
 * for both: the AArch32 build compiles its call out (IS_ENABLED).
 */
void ax620e_bl33_up(void);

/*
 * A boot step on the console, so on the panel: printed in a verbose_boot
 * loader only, and nothing at all otherwise. Lines fit the panel's 30 columns.
 */
#if IS_ENABLED(CONFIG_AX620E_VERBOSE_BOOT)
#define ax620e_verbose(fmt, ...)	printf(fmt, ##__VA_ARGS__)
#else
#define ax620e_verbose(fmt, ...)
#endif

#if IS_ENABLED(CONFIG_ARM64)
/*
 * Why the chip last reset, raw ABORT_STATUS: bit 4 wdt2, 2 wdt0, 1 thermal,
 * 0 software; 0 after a power-on, into *reason. Once, from board_late_init:
 * AArch32 clears the latch as it reads it, so it is stored first.
 */
void ax620e_reset_reason(u32 *reason);

/* The rollback floor the slots are held to: fls(eFuse blk12). */
u32 ax620e_rollback_floor(void);

/* Whether the A/B slots may boot at all. */
bool ax620e_slots_allowed(void);

/* Whether the loader that started this one already spent this boot's try. */
bool ax620e_try_spent(void);
/* Whether the stash may boot on a card that is not A/B; `stock`: the card is the stock layout. */
bool ax620e_stash_allowed(bool stock);

/* The firmware bundle copy SPL64 booted: 0 FW-A, 1 FW-B; -1 without SPL64. */
int ax620e_fw_copy(void);

/* Adds to /chosen (offset `chosen`) what the arch half hands Linux; 0 or an FDT error. */
int ax620e_fixup_chosen(void *blob, int chosen);
#else
#include <asm/io.h>
#include <linux/bitops.h>

#define ABORT_CFG		0x023400a8
#define   ABORT_CLR		(BIT(5) | BIT(3) | BIT(2) | BIT(1))	/* wdt2 wdt0 thermal sw */
#define   ABORT_EN		(BIT(9) | BIT(7) | BIT(6))		/* wdt2 wdt0 thermal */
#define ABORT_STATUS		0x023400b4

/* Read, then cleared and the abort sources re-armed, so the next boot sees only its own cause. */
static inline void ax620e_reset_reason(u32 *reason)
{
	*reason = readl(ABORT_STATUS);
	writel(ABORT_CLR, ABORT_CFG);
	writel(ABORT_EN, ABORT_CFG);
}

#define EFSC			0x02200000	/* the eFuse controller, as the SPL reads it */
#define   EFSC_SHADOW(blk)	(EFSC + 0x58 + (blk) * 4)
/*
 * The rollback floor's eFuse block: zero and unlocked (blk31 locks 16..28) as
 * measured on the AX620QE, and named by nothing on the AX620E map (0-1 lot id, 2 bond,
 * 3 thermal, 4 unnamed but set, 13 flags, 15-22 key hash, 23-30 AES key, 31 lock;
 * 32-63 need EFUSE_SPACE).
 */
#define ROLLBACK_BLK		12

/*
 * A thermometer read as the number of its highest burnt bit (fls): one burn
 * raises it to any level, and a bit below that failed to burn cannot lower it.
 * A failed or gated eFuse read gives 0 (fails open): a fused design needs a
 * proven read path or a fail-closed sanity read first.
 */
static inline u32 ax620e_rollback_floor(void)
{
	return fls(readl(EFSC_SHADOW(ROLLBACK_BLK)));
}

static inline bool ax620e_slots_allowed(void)
{
	return true;
}

static inline bool ax620e_try_spent(void)
{
	return false;
}

static inline bool ax620e_stash_allowed(bool stock)
{
	return true;
}

static inline int ax620e_fixup_chosen(void *blob, int chosen)
{
	return 0;
}

static inline int ax620e_fw_copy(void)
{
	return -1;
}
#endif

#endif
