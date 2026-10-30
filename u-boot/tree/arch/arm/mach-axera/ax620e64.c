// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2026, GembaHive, Inc.
 */
/*
 * Axera AX620E in AArch64: U-Boot as TF-A's BL33 at Non-secure EL2, entered
 * by BL31 with the MMU off and CNTFRQ set; PSCI is BL31's, behind SMC. One
 * position-independent binary, entered either at TEXT_BASE after SPL64 (its
 * handoff block in IRAM) or at 0x40200000 from a FIT (no block). It never
 * touches ABORT_* or the eFuses: SPL64 passes what it read of them in the block.
 */

#include <config.h>
#include <cpu_func.h>
#include <image.h>
#include <init.h>
#include <asm/armv8/mmu.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <asm/ptrace.h>
#include <asm/system.h>
#include <linux/build_bug.h>
#include <linux/libfdt.h>
#include <linux/sizes.h>
#include <mach/ax620e.h>

DECLARE_GLOBAL_DATA_PTR;

/*
 * DRAM's secure window (BL31, OP-TEE) at its base: left unmapped, as Linux's
 * no-map reserved-memory keeps it out of Linux, so no Non-secure access, not
 * even a speculative one, reaches it. The DRAM bank and Linux's memory node
 * still start at the DRAM base.
 * TODO: from board.toml [secure] (2 or 4 MiB; at 4 MiB the FIT entry at
 * 0x40200000 moves too).
 */
#define SECURE_WINDOW		SZ_2M

static struct mm_region ax620e_mem_map[] = {
	{
		/* IRAM and MMIO */
		.virt = 0x0UL,
		.phys = 0x0UL,
		.size = 0x40000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/*
		 * DRAM above the secure window, sized by enable_caches. The
		 * largest part's size until then: arm_reserve_mmu sizes the
		 * page tables from this map before dram_init's size is known here.
		 */
		.virt = CFG_SYS_SDRAM_BASE + SECURE_WINDOW,
		.phys = CFG_SYS_SDRAM_BASE + SECURE_WINDOW,
		.size = SZ_512M - SECURE_WINDOW,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_INNER_SHARE
	}, {
		/* List terminator */
		0,
	}
};

struct mm_region *mem_map = ax620e_mem_map;

/* Only the DRAM the part has is cacheable: the board's dram_init (256 or 512 MiB) ran before relocation. */
void enable_caches(void)
{
	ax620e_mem_map[1].size = gd->ram_size - SECURE_WINDOW;
	icache_enable();
	dcache_enable();
}

/* BL31's SYSTEM_RESET resets the chip through wdt0, so the next boot reads a chip reset. */
void reset_cpu(void)
{
	psci_system_reset();
}

void board_cleanup_before_linux(void);

/*
 * The last step before the jump, caches and panel still up: names the
 * architecture Linux is entered in (arm64, or arm for an AArch32 kernel).
 * Linux is entered with PSTATE.DAIF all masked (Documentation/arch/arm64/booting.rst);
 * U-Boot's start.S unmasks SError and disable_interrupts() masks nothing.
 */
void board_cleanup_before_linux(void)
{
	ax620e_verbose("U-Boot: kernel go, %s\n", genimg_get_arch_short_name(images.os.arch));
	asm volatile("msr daifset, #0xf" : : : "memory");
}

/*
 * SPL64's handoff block (its struct fw_handoff), rewritten by SPL64 on every
 * boot before anything Non-secure runs.
 */
#define HANDOFF_AT		0x900	/* IRAM0 page 0, Non-secure readable */
#define HANDOFF_MAGIC		0x4c505347	/* "GSPL" */
#define HANDOFF_VERSION		1
struct fw_handoff {
	u32 magic, version;
	u32 reset_reason;	/* ABORT_STATUS as found */
	u32 floor;		/* fls(eFuse blk12) */
	u32 sec_boot;
	u32 fw_copy;		/* 0 FW-A, 1 FW-B */
	u32 fw_seq, fw_rollback_index;
	u32 fw_trial, fw_factory;
	u32 sw0_at_entry;
	u32 prev_backup0, prev_backup1;
	u32 rom_sctlr;	/* SCTLR_EL3 as SPL64 was entered with */
	u64 cntpct[5];
};

static_assert(sizeof(struct fw_handoff) == 96);

static struct fw_handoff block;
static int block_state;	/* 0 unread, 1 valid, -1 absent */

/*
 * The block, or NULL when SPL64 did not start this boot (its magic or version
 * do not match). Read once, after relocation; word by word, as IRAM is mapped
 * Device.
 */
static const struct fw_handoff *handoff(void)
{
	u32 *w = (u32 *)&block;
	ulong i;

	if (!block_state) {
		for (i = 0; i < sizeof(block) / 4; i++)
			w[i] = readl(HANDOFF_AT + i * 4);
		block_state = block.magic == HANDOFF_MAGIC && block.version == HANDOFF_VERSION ? 1 : -1;
		if (block_state < 0)
			printf("U-Boot: no SPL64 handoff block\n");
	}
	return block_state > 0 ? &block : NULL;
}

/* BL31's SiP call (SMC32). A BL31 without it answers SMC_UNK. */
#define SIP_BL33_UP		0x82000001
#define SMC_UNK			-1

static void sip(u32 fid)
{
	struct pt_regs regs = { .regs[0] = fid };
	s32 ret;

	smc_call(&regs);
	ret = regs.regs[0];
	if (ret && ret != SMC_UNK)
		printf("BL31: SiP 0x%x returned %d\n", fid, ret);
}

void ax620e_bl33_up(void)
{
	sip(SIP_BL33_UP);
}

void ax620e_reset_reason(u32 *reason)
{
	const struct fw_handoff *h = handoff();

	*reason = h ? h->reset_reason : 0;
}

u32 ax620e_rollback_floor(void)
{
	const struct fw_handoff *h = handoff();

	return h ? h->floor : 0;
}

/*
 * With no block the rollback floor is unknown, so slots booted would skip
 * rollback protection: refused, unless AX620E_SLOTS_WITHOUT_HANDOFF allows them.
 */
bool ax620e_slots_allowed(void)
{
	return handoff() || IS_ENABLED(CONFIG_AX620E_SLOTS_WITHOUT_HANDOFF);
}

/*
 * Started from a slot FIT with no SPL64 handoff block (allowed in a dev loader
 * only), the loader that loaded this one spent the try.
 */
bool ax620e_try_spent(void)
{
	return IS_ENABLED(CONFIG_AX620E_SLOTS_WITHOUT_HANDOFF) && !handoff();
}

/*
 * Under a release bundle the stash boots only on the stock layout, where it
 * finishes installing the boot chain. Without SPL64, or under a factory
 * bundle, it boots on any card that is not A/B.
 */
bool ax620e_stash_allowed(bool stock)
{
	const struct fw_handoff *h = handoff();

	return !h || h->fw_factory || stock;
}

/* The raw block as /chosen's gemba,spl, for Linux. */
int ax620e_fixup_chosen(void *blob, int chosen)
{
	const struct fw_handoff *h = handoff();

	return h ? fdt_setprop(blob, chosen, "gemba,spl", h, sizeof(*h)) : 0;
}

int ax620e_fw_copy(void)
{
	const struct fw_handoff *h = handoff();

	return h && h->fw_copy <= 1 ? h->fw_copy : -1;
}
