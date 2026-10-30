// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2026, GembaHive, Inc.
 */
/*
 * The boot every AX620E board shares: the A/B slots with their power-on
 * counter, recovery, the reset reason, wdt0 and the FDT fixups. What differs
 * per board is its own file and defconfig.
 */

#include <blk.h>
#include <bootm.h>
#include <command.h>
#include <cpu_func.h>
#include <dm.h>
#include <env.h>
#include <hexdump.h>
#include <image.h>
#include <init.h>
#include <membuf.h>
#include <mmc.h>
#include <part.h>
#include <rng.h>
#include <fdt_support.h>
#include <boot-configs.h>
#include <bootchain.h>
#include <loader-build-id.h>
#include <u-boot/sha256.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/sizes.h>
#include <linux/stringify.h>
#include <mach/ax620e.h>

DECLARE_GLOBAL_DATA_PTR;

#define WDT0			0x04840000	/* CR at +0; Linux's ax620e_wdt takes it over */
#define   WDT0_TORR		(WDT0 + 0x0c)
#define   WDT0_TORR_LOAD	(WDT0 + 0x18)
#define   WDT0_CRR		(WDT0 + 0x30)
#define   WDT0_KICK		0x61696370
#define WDT0_CLK_MUX_SET	0x048700a8
#define   WDT0_CLK_24M		BIT(19)

/*
 * wdt0 resets the chip 30 s from now unless Linux's driver takes it over:
 * what ends a boot that hangs anywhere before. U-Boot restarts it only around
 * boot-area writes and just before bootm, never while it waits. The
 * clock first, the load strobe held 50 us (strobed for no time, the 30 s
 * never loads), then a kick that restarts the count.
 */
static void wdt0_arm(void)
{
	writel(WDT0_CLK_24M, WDT0_CLK_MUX_SET);
	writel((30 * 24000000) >> 16, WDT0_TORR);
	writel(1, WDT0_TORR_LOAD);
	udelay(50);
	writel(0, WDT0_TORR_LOAD);
	writel(1, WDT0);
	writel(WDT0_KICK, WDT0_CRR);
	udelay(50);
	writel(0, WDT0_CRR);
}

/* Why the chip last reset (ax620e_reset_reason). */
static u32 reset_reason;
/* The pin button held: recovery. */
static bool pin_held;
/* Booting the stash or recovery: the card stays writable (ft_board_setup). */
static bool unlocked;
/* The slot booting holds TRIES_CONFIRMED: Linux has kept it. */
static bool confirmed;

bool recovery_held(void);	/* the board's file: its recovery button */

int board_late_init(void)
{
	wdt0_arm();
	if (IS_ENABLED(CONFIG_ARM64))
		ax620e_bl33_up();
	ax620e_reset_reason(&reset_reason);
	pin_held = recovery_held();
	printf("U-Boot: reset 0x%x, recovery pin %s\n", reset_reason, pin_held ? "held" : "up");
	ax620e_verbose("U-Boot: %lu MiB DRAM\n", (ulong)(gd->ram_size >> 20));
	return 0;
}

/*
 * The card, in 512-byte sectors (U-Boot caps an MMC's block at 512:
 * MMC_MAX_BLOCK_LEN). Each root slot's last 65 MiB lie outside its rootfs:
 * its FIT at SLOT_FIT_AT, then the recovery FIT it stages, at STAGED_AT.
 * RECOVERY_LBA holds the installed recovery FIT, raw. FITs, signed and
 * verified against the key in our DTB, of at most SLOT_FIT_MAX and
 * RECOVERY_FIT_MAX bytes.
 */

/* The boot disk (env, the root slots, recovery): U-Boot and Linux number it alike, by their mmcN aliases. */
#define DISK			__stringify(CONFIG_ENV_MMC_DEVICE_INDEX)
/* ttynull: /dev/console exists, and nothing reaches it. */
#define NO_CONSOLE		"console=ttynull "
#ifdef CONFIG_AX620E_SERIAL_CONSOLE
#define CONSOLE			"console=" CONFIG_AX620E_LINUX_CONSOLE " "
#define EARLYCON		"earlycon=" CONFIG_AX620E_LINUX_EARLYCON " "
#else
#define CONSOLE			NO_CONSOLE
#define EARLYCON		""
#endif
#if BOOT_SYSTEM_CONSOLE
#define SLOT_CONSOLE		CONSOLE
#else
#define SLOT_CONSOLE		NO_CONSOLE
#endif

#ifdef CONFIG_CONSOLE_RECORD
/* [image] debug_logs only: the recorded console's tail, up to here, in /chosen. */
static int attach_bootlog(void *blob, int chosen)
{
	struct membuf console = gd->console_out;
	char text[1024];
	char *discard;
	int skip = membuf_avail(&console) - (int)sizeof(text) + 1;
	bool truncated = (gd->flags & GD_FLG_RECORD_OVF) || skip > 0;
	int len, ret;

	/* A wrapped membuf has at most two contiguous regions. */
	if (skip > 0)
		skip -= membuf_getraw(&console, skip, true, &discard);
	if (skip > 0)
		membuf_getraw(&console, skip, true, &discard);
	len = membuf_get(&console, text, sizeof(text) - 1);
	text[len] = '\0';
	ret = fdt_setprop(blob, chosen, "bootlog", text, len + 1);
	if (!ret)
		ret = fdt_setprop_u32(blob, chosen, "bootlog-truncated", truncated);
	return ret;
}
#else
/* A release loader records no console output, so it hands Linux none. */
static int attach_bootlog(void *blob, int chosen)
{
	return 0;
}
#endif

/*
 * gemba,unlocked (empty) when booting the stash or recovery, never a slot: the
 * kernel's boot-area write lock is off on those boots alone. Recovery is
 * entered only with the pin. The loader's build id, in every build. Then the
 * arch half's (gemba,spl).
 */
int ft_board_setup(void *blob, struct bd_info *bd)
{
	int chosen = fdt_path_offset(blob, "/chosen");
	int ret = chosen < 0 ? chosen : unlocked ? fdt_setprop_empty(blob, chosen, "gemba,unlocked") : 0;

	if (!ret)
		ret = attach_bootlog(blob, chosen);
	if (!ret)
		ret = fdt_setprop_string(blob, chosen, "loader-build-id", LOADER_BUILD_ID);
	if (!ret)
		ret = ax620e_fixup_chosen(blob, chosen);
	if (ret)
		printf("U-Boot: /chosen fixup failed: %s\n", fdt_strerror(ret));
	/* A failed fixup must not prevent a verified kernel from booting. */
	return 0;
}

#ifdef CONFIG_BOARD_RNG_SEED
/*
 * /chosen/rng-seed: Linux credits it as entropy, so its RNG is ready from the
 * start. Without an RNG there is none, and the boot goes on.
 */
int board_rng_seed(struct abuf *buf)
{
	struct udevice *dev;

	if (uclass_get_device(UCLASS_RNG, 0, &dev))
		return -ENODEV;
	if (!abuf_init_size(buf, 64))
		return -ENOMEM;
	if (dm_rng_read(dev, abuf_data(buf), abuf_size(buf))) {
		abuf_uninit(buf);
		return -EIO;
	}
	return 0;
}
#endif

/* The boot card, initialized, or NULL. */
static struct blk_desc *card(void)
{
	struct mmc *mmc = find_mmc_device(CONFIG_ENV_MMC_DEVICE_INDEX);

	return mmc && !mmc_init(mmc) ? mmc_get_blk_desc(mmc) : NULL;
}

/*
 * The FIT at sector `lba`, raw, read to `to`: its header, then all of its
 * totalsize in whole sectors, if that is at most `max` (a multiple of 512).
 * Its size, or 0 for none.
 */
static ulong fit_read(struct blk_desc *desc, lbaint_t lba, void *to, ulong max)
{
	ulong size, n;

	if (!desc || blk_dread(desc, lba, 1, to) != 1 || fdt_magic(to) != FDT_MAGIC)
		return 0;
	size = fdt_totalsize(to);
	n = DIV_ROUND_UP(size, 512);
	if (size < sizeof(struct fdt_header) || size > max || blk_dread(desc, lba, n, to) != n)
		return 0;
	return size;
}

/* Configuration `name` (NULL: the default) of the FIT of `size` at `fit`, if its signature verifies: its node, else -1. */
static int fit_verified(const void *fit, ulong size, const char *name)
{
	int conf = size && !fit_check_format(fit, size) ? fit_conf_get_node(fit, name) : -1;

	return conf >= 0 && !fit_config_verify(fit, conf) ? conf : -1;
}

/* Configuration `conf`'s u32 property `name`, signed with it; 0 when absent. */
static u32 conf_u32(const void *fit, int conf, const char *name)
{
	const fdt32_t *v;
	int len;

	v = fdt_getprop(fit, conf, name, &len);
	return v && len == sizeof(*v) ? fdt32_to_cpu(*v) : 0;
}

/*
 * Anti-rollback, for the slots: the configuration `conf`'s signature (NULL:
 * the default), then its signed rollback-index against the eFuse floor
 * (ax620e_rollback_floor).
 * No index reads as 0: it is signed, so only a FIT signed without one lacks it.
 *
 * Fails open until a floor is burnt on a unit with ROM secure boot fused
 * (before that anyone with the card replaces U-Boot); a failed or gated eFuse
 * read also reads 0, so a fused design needs a proven read path or a
 * fail-closed sanity read.
 *
 * Recovery is exempt: a misfire on an unnamed eFuse block must not lock out
 * the way back. Known gap: an old signed recovery FIT plus a pin press boots
 * on a fused unit. Recovery's index must be held to a floor too, raised only
 * after recovery is updated.
 */
static bool fit_current(const char *what, loff_t size, const char *name)
{
	const void *fit = (const void *)CONFIG_SYS_LOAD_ADDR;
	int conf = fit_verified(fit, size, name);
	u32 value, floor;

	if (conf < 0) {
		printf("U-Boot: %s: not verified\n", what);
		return false;
	}
	value = conf_u32(fit, conf, "rollback-index");
	floor = ax620e_rollback_floor();
	printf("U-Boot: %s: rollback-index %u, eFuse floor %u\n", what, value, floor);
	return value >= floor;
}

/*
 * The configuration to boot: in the AArch64 build, "a64" when the FIT has
 * one, else the default (NULL). The 32-bit build always boots the default.
 */
static const char *fit_conf_name(const void *fit, loff_t size)
{
	if (IS_ENABLED(CONFIG_ARM64) && !fit_check_format(fit, size) && fit_conf_get_node(fit, "a64") >= 0)
		return "a64";
	return NULL;
}

/*
 * Whether configuration `conf`'s kernel is for this build's CPU state
 * (fit_image_check_arch: arm64 takes no arm kernel, ARM64_SUPPORT_AARCH32
 * being off). bootm checks no arch outside sandbox, so without this U-Boot64
 * would load a single-config FIT's AArch32 stub and its loadables (BL31 into
 * the secure window it leaves unmapped: an abort) and jump to the stub.
 */
static bool fit_for_this_cpu(loff_t size, const char *conf)
{
	const void *fit = (const void *)CONFIG_SYS_LOAD_ADDR;
	int node = fit_check_format(fit, size) ? -1 : fit_conf_get_node(fit, conf);

	if (node >= 0)
		node = fit_conf_get_prop_node(fit, node, FIT_KERNEL_PROP, IH_PHASE_NONE);
	return node >= 0 && fit_image_check_target_arch(fit, node);
}

/* RAM past the slot FIT, which stays loaded for bootm: what a write sends, then what the card holds. */
#define STAGE_AT		(CONFIG_SYS_LOAD_ADDR + SLOT_FIT_MAX)
#define CHECK_AT		(STAGE_AT + RECOVERY_FIT_MAX)

/* The firmware bundle's copies, FW-A and FW-B, raw, FW_MAX bytes each. */
#define FW_LBA(copy)		((copy) ? FW_B_LBA : FW_A_LBA)

/*
 * BOOT.BIN in the stock p1 (FAT32, cluster 13792, contiguous), where the ROM
 * finds it by name: two 128 KiB SPL copies, each a 1 KiB Axera header, the
 * SPL and the CE firmware.
 */
#define SPL_COPY		SZ_128K

/*
 * `n` sectors from `buf` to sector `lba`, read back into CHECK_AT and
 * compared; wdt0 restarted around each step.
 */
static bool write_checked(struct blk_desc *desc, lbaint_t lba, lbaint_t n, const void *buf)
{
	void *check = (void *)CHECK_AT;
	bool ok;

	wdt0_arm();
	ok = blk_dwrite(desc, lba, n, buf) == n;
	wdt0_arm();
	ok = ok && blk_dread(desc, lba, n, check) == n && !memcmp(check, buf, n * 512);
	wdt0_arm();
	return ok;
}

/* The image configuration `conf` names in `prop`, its hashes checked: its data, or NULL. */
static const void *conf_image(const void *fit, int conf, const char *prop, size_t *size)
{
	int node = fit_conf_get_prop_node(fit, conf, prop, IH_PHASE_NONE);
	const void *data;

	if (node < 0 || !fit_image_verify(fit, node) || fit_image_get_data(fit, node, &data, size))
		return NULL;
	return data;
}

/* The wrapping sum of the little-endian words in `len` bytes (a trailing partial word ignored). */
static u32 word_sum(const u8 *p, u32 len)
{
	const u32 *w = (const u32 *)p;
	u32 sum = 0;

	for (len /= 4; len; len--)
		sum += le32_to_cpu(*w++);
	return sum;
}

/* A bundle header's key fields (key_n_header, n, key_e_header, e), then the signature. */
#define FW_SIG_AT		(FW_KEY_AT + FW_KEY_LEN)

/*
 * Whether SPL64 takes `hdr` as a bundle header (its fw_header): magic and
 * sum, RSA-3072 in the clear, e 65537, the key of `ours` (a header it booted)
 * and a size that fits a copy.
 */
static bool fw_header_ok(const u8 *hdr, const u8 *ours)
{
	const u32 *h = (const u32 *)hdr;
	u32 cap = le32_to_cpu(h[2]), size = le32_to_cpu(h[3]);

	return le32_to_cpu(h[1]) == AXERA_MAGIC && word_sum(hdr + 8, AXERA_HDR - 8) == le32_to_cpu(h[0]) &&
	       (cap & (AXERA_RSA_3072 | AXERA_CIPHER)) == AXERA_RSA_3072 &&
	       le32_to_cpu(h[FW_SIG_AT / 4 - 1]) == 65537 &&
	       !memcmp(hdr + FW_KEY_AT, ours + FW_KEY_AT, FW_KEY_LEN) &&
	       size >= FW_TABLE_LEN && size <= FW_MAX - AXERA_HDR;
}

/*
 * fw.bin, zero-padded to whole sectors, unless FW-A or FW-B already holds it:
 * under SPL64 into the copy it did not boot, its seq, floor and trial rules
 * deciding whether it boots; without SPL64, which boots neither, into both.
 * Held by the copy SPL64 booted, it also goes into the other copy if SPL64
 * would refuse that copy's header (a stub, a torn write); a copy it would take,
 * an older bundle, stays as the fallback. Whether a copy holds it now.
 */
static bool update_fw(struct blk_desc *desc, const void *data, size_t size)
{
	u8 *stage = (u8 *)STAGE_AT, *check = (u8 *)CHECK_AT;
	lbaint_t n = DIV_ROUND_UP(size, 512);
	int booted = ax620e_fw_copy(), i;
	bool held = false, ok, holds[2], usable[2];

	if (size > FW_MAX) {
		printf("U-Boot: fw.bin: over 3 MiB; refused\n");
		return false;
	}
	memcpy(stage, data, size);
	memset(stage + size, 0, n * 512 - size);
	for (i = 0; i < 2; i++) {
		ok = blk_dread(desc, FW_LBA(i), n, check) == n;
		holds[i] = ok && !memcmp(check, stage, n * 512);
		usable[i] = !ok || fw_header_ok(check, stage);
	}
	if (booted >= 0 && holds[booted] && !usable[!booted]) {
		ok = write_checked(desc, FW_LBA(!booted), n, stage);
		printf("U-Boot: fw.bin to FW-%c: %s\n", 'A' + !booted, ok ? "written" : "write failed");
	}
	if (holds[0] || holds[1])
		return true;
	/* Under SPL64 the booted copy is unknown without its handoff: write neither. */
	if (booted < 0 && IS_ENABLED(CONFIG_ARM64))
		return false;
	for (i = 0; i < 2; i++) {
		if (i == booted)
			continue;
		ok = write_checked(desc, FW_LBA(i), n, stage);
		printf("U-Boot: fw.bin to FW-%c: %s\n", 'A' + i, ok ? "written" : "write failed");
		held |= ok;
	}
	return held;
}

/*
 * One SPL copy as the ROM checks it: header magic and sum, the SPL's sum, the
 * backup copy's place, and the CE firmware, word-aligned after the SPL, with
 * its sum.
 */
static bool spl_valid(const u8 *c)
{
	const u32 *h = (const u32 *)c;
	u32 img = le32_to_cpu(h[3]), fw_size = le32_to_cpu(h[4]), fw = le32_to_cpu(h[9]);

	return le32_to_cpu(h[1]) == AXERA_MAGIC && word_sum(c + 8, AXERA_HDR - 8) == le32_to_cpu(h[0]) &&
	       img <= SPL_COPY - AXERA_HDR && word_sum(c + AXERA_HDR, img) == le32_to_cpu(h[5]) &&
	       le32_to_cpu(h[8]) == SPL_COPY + AXERA_HDR && fw % 4 == 0 &&
	       fw >= AXERA_HDR + img && fw <= SPL_COPY && fw_size <= SPL_COPY - fw &&
	       word_sum(c + fw, fw_size) == le32_to_cpu(h[6]);
}

/*
 * Whether card copy `c` may be overwritten: it is no valid copy (a torn
 * write), or `list` (gemba,spl-update, `len` bytes) holds "*" or the sha256
 * of the package `c c`, in lowercase hex.
 */
static bool spl_replaceable(const u8 *c, const char *list, int len)
{
	sha256_context ctx;
	u8 sum[SHA256_SUM_LEN];
	char hex[2 * SHA256_SUM_LEN + 1];

	if (!spl_valid(c))
		return true;
	if (!list)
		return false;
	sha256_starts(&ctx);
	sha256_update(&ctx, c, SPL_COPY);
	sha256_update(&ctx, c, SPL_COPY);
	sha256_finish(&ctx, sum);
	*bin2hex(hex, sum, sizeof(sum)) = '\0';
	return fdt_stringlist_contains(list, len, "*") || fdt_stringlist_contains(list, len, hex);
}

/*
 * BOOT.BIN copy 1, while copy 2 is a valid fallback and spl_replaceable allows
 * copy 1. Copy 2, the ROM's fallback, is never written. Where BOOT_BIN_LBA
 * holds no BOOT.BIN, copy 2 is no valid copy and nothing is written.
 */
static void update_spl(struct blk_desc *desc, const u8 *want, size_t size, const char *list, int len)
{
	u8 *have = (u8 *)CHECK_AT, *stage = (u8 *)STAGE_AT;

	if (size != 2 * SPL_COPY || memcmp(want, want + SPL_COPY, SPL_COPY) || !spl_valid(want)) {
		printf("U-Boot: BOOT.BIN: not an SPL package; refused\n");
		return;
	}
	if (blk_dread(desc, BOOT_BIN_LBA, 2 * SPL_COPY / 512, have) != 2 * SPL_COPY / 512 ||
	    !memcmp(have, want, SPL_COPY))
		return;
	if (!spl_valid(have + SPL_COPY)) {
		printf("U-Boot: BOOT.BIN: copy 2 not valid; copy 1 held\n");
		return;
	}
	if (!spl_replaceable(have, list, len)) {
		printf("U-Boot: BOOT.BIN copy 1: not in gemba,spl-update; held\n");
		return;
	}
	memcpy(stage, want, SPL_COPY);
	printf("U-Boot: BOOT.BIN copy 1: %s\n",
	       write_checked(desc, BOOT_BIN_LBA, SPL_COPY / 512, stage) ? "written" : "write failed");
}

/*
 * The recovery FIT slot `slot` stages, into the recovery area if it verifies
 * and the installed one does not, or the slot is confirmed and its signed
 * gemba,seq is above the installed one's: a slot on trial never replaces a
 * recovery that verifies, yet a card is never left without one.
 */
static void update_recovery(struct blk_desc *desc, lbaint_t slot)
{
	void *staged = (void *)STAGE_AT, *installed = (void *)CHECK_AT;
	ulong size = fit_read(desc, slot + STAGED_AT, staged, RECOVERY_FIT_MAX);
	ulong have = fit_read(desc, RECOVERY_LBA, installed, RECOVERY_FIT_MAX);
	int conf = fit_verified(staged, size, fit_conf_name(staged, size));
	int old = fit_verified(installed, have, fit_conf_name(installed, have));
	u32 seq;

	if (conf < 0)
		return;
	seq = conf_u32(staged, conf, "gemba,seq");
	if (old >= 0 && (!confirmed || seq <= conf_u32(installed, old, "gemba,seq")))
		return;
	printf("U-Boot: recovery seq %u: %s\n", seq,
	       write_checked(desc, RECOVERY_LBA, DIV_ROUND_UP(size, 512), staged) ? "written" : "write failed");
}

/*
 * Before a slot's verified FIT boots: the recovery FIT the slot stages, then,
 * if the slot is confirmed, what its signed "update" configuration carries,
 * fw.bin and BOOT.BIN, each written only where due. BOOT.BIN only once a
 * firmware copy holds fw.bin: SPL64 boots nothing without it.
 */
static void apply_updates(lbaint_t slot, ulong size)
{
	const void *fit = (const void *)CONFIG_SYS_LOAD_ADDR;
	struct blk_desc *desc = card();
	const void *fw = NULL, *boot = NULL;
	size_t fw_size = 0, boot_size = 0;
	const char *list;
	int conf, len;

	if (!desc)
		return;
	update_recovery(desc, slot);
	if (!confirmed)
		return;
	conf = fit_verified(fit, size, "update");
	if (conf >= 0) {
		fw = conf_image(fit, conf, "fw-bin", &fw_size);
		boot = conf_image(fit, conf, "boot-bin", &boot_size);
	}
	if (!fw || !boot) {
		printf("U-Boot: update: not verified\n");
		return;
	}
	list = fdt_getprop(fit, conf, "gemba,spl-update", &len);
	if (update_fw(desc, fw, fw_size))
		update_spl(desc, boot, boot_size, list, len);
}

/*
 * Boots the FIT of `size` at CONFIG_SYS_LOAD_ADDR with `args`, held to the
 * rollback floor if `slot`, after the updates of slot `update` (its first
 * sector; 0 for none): returns only if it did not.
 * wdt0 restarts its 30 s just before: Linux's watchdog driver takes it over.
 * bootm boots the configuration fit_current checked: named as <addr>#<conf>.
 * 32-bit: conf is NULL; the CPU check compiles out.
 */
static void boot_loaded(const char *what, loff_t size, const char *args, bool slot, lbaint_t update)
{
	const char *conf = fit_conf_name((const void *)CONFIG_SYS_LOAD_ADDR, size);
	struct bootm_info bmi;
	char addr[IS_ENABLED(CONFIG_ARM64) ? sizeof("46000000#a64") : 11];

	if (slot && !fit_current(what, size, conf))
		return;
	if (IS_ENABLED(CONFIG_ARM64) && !fit_for_this_cpu(size, conf)) {
		printf("U-Boot: %s: not for this CPU\n", what);
		return;
	}
	if (update)
		apply_updates(update, size);
	if (env_set("bootargs", args)) {
		printf("U-Boot: %s: boot arguments failed; not booting\n", what);
		return;
	}
	wdt0_arm();
	bootm_init(&bmi);
	if (conf)
		snprintf(addr, sizeof(addr), "%x#%s", CONFIG_SYS_LOAD_ADDR, conf);
	else
		snprintf(addr, sizeof(addr), "%x", CONFIG_SYS_LOAD_ADDR);
	bmi.addr_img = addr;
	bootm_run(&bmi);
}

/* The FIT at sector `lba`, of at most `max`, loaded and booted as boot_loaded() does. */
static void boot_raw(const char *what, struct blk_desc *desc, lbaint_t lba, ulong max, const char *args, bool slot,
		     lbaint_t update)
{
	ulong size = fit_read(desc, lba, (void *)CONFIG_SYS_LOAD_ADDR, max);

	if (!size) {
		printf("U-Boot: %s: load failed\n", what);
		return;
	}
	ax620e_verbose("U-Boot: loaded %lu KiB\n", size >> 10);
	boot_loaded(what, size, args, slot, update);
}

/* Only the A/B layout puts p2 at slot A; on a stock card the env offsets lie inside its rootfs. */
static bool ab_layout(struct blk_desc *desc)
{
	struct disk_partition p2;

	return desc && desc->blksz == 512 &&
		!part_get_info(desc, 2, &p2) && p2.start == SLOT_A_LBA;
}

/* The stock layout: the stock p1 and p2. */
static bool stock_layout(struct blk_desc *desc)
{
	struct disk_partition p1, p2;

	return desc && desc->blksz == 512 &&
		!part_get_info(desc, 1, &p1) && disk_partition_sys_ind(&p1) == STOCK_P1_TYPE &&
		p1.start == STOCK_P1_LBA && p1.size == STOCK_P1_SECTORS &&
		!part_get_info(desc, 2, &p2) && p2.start == STOCK_P2_LBA;
}

/* env/mmc.c calls this for both reads and writes, after initializing MMC. */
int mmc_get_env_addr(struct mmc *mmc, int copy, u32 *env_addr)
{
	if (!ab_layout(mmc_get_blk_desc(mmc)))
		return -ENODEV;
	*env_addr = copy ? CONFIG_ENV_OFFSET_REDUND : CONFIG_ENV_OFFSET;
	return 0;
}

/*
 * A/B with RAUC's variables: the first slot in BOOT_ORDER with tries left
 * spends one, saved before anything loads so a slot that never comes up
 * runs out, and boots the FIT in its tail with its root, p2 (A) or p3 (B),
 * named; Linux resets the count once the slot is up. A slot that does not
 * boot costs its try and the next is taken.
 *
 * A reset the chip made (a hang, a panic, every Linux reboot: all go through
 * a watchdog) spends a try, a power-on only every POWER_ONS_PER_TRY-th
 * (BOOT_POWER_ONS counts them, for whichever slot boots): power cuts during
 * boot say nothing about the slot, yet one whose failures look like
 * power-ons (it cuts its own power, or a chained U-Boot that finds the
 * reason already cleared) still runs out. A spend env_save cannot write is
 * no boot of that slot: uncounted, a slot that never comes up would boot
 * forever, so a card that takes no env write boots none (do_abboot).
 */
static void boot_slots(void)
{
	struct blk_desc *desc = card();
	bool reset = reset_reason & 0x17;
	ulong power_ons = env_get_ulong("BOOT_POWER_ONS", 10, 0);
	char order[8], *s;

	if (!ax620e_slots_allowed()) {
		printf("U-Boot: slots refused\n");
		return;
	}
	/* No rauc.slot= on an old layout: Linux must not confirm into rootfs. */
	if (!ab_layout(desc)) {
		printf("U-Boot: not an A/B card\n");
		return;
	}
	strlcpy(order, env_get("BOOT_ORDER") ?: "", sizeof(order));
	for (s = order; *s; s++) {
		char left[] = "BOOT_?_LEFT", what[] = "slot ?", args[256];
		lbaint_t lba = *s == 'A' ? SLOT_A_LBA : SLOT_B_LBA;
		ulong n, p;

		if (*s != 'A' && *s != 'B')
			continue;
		left[5] = what[5] = *s;
		n = env_get_ulong(left, 10, 0);
		/* Linux gives a slot it keeps TRIES_CONFIRMED again, a trial fewer: see apply_updates. */
		confirmed = n == TRIES_CONFIRMED;
		printf("U-Boot: slot %c: %lu tries left\n", *s, n);
		/*
		 * Chained after a loader that spent this boot's try: no count,
		 * each slot in order, as only one carries a FIT for this CPU.
		 */
		if (!ax620e_try_spent()) {
			if (!n)
				continue;
			p = reset ? power_ons : (power_ons + 1) % POWER_ONS_PER_TRY;
			env_set_ulong(left, reset || !p ? n - 1 : n);
			env_set_ulong("BOOT_POWER_ONS", p);
			if (env_save()) {
				printf("U-Boot: slot %c: environment save failed; not booting\n", *s);
				continue;
			}
		}
		/*
		 * handle_boot_enabled=0: the watchdog core leaves wdt0 to systemd, so a slot that never gets there resets.
		 * boot.panic_on_fail: a NixOS stage 1 that fails panics (a warm reset) instead of idling in emergency mode.
		 */
		snprintf(args, sizeof(args), SLOT_CONSOLE EARLYCON "init=/sbin/init root=/dev/mmcblk" DISK "p%c rauc.slot=%c "
			 "watchdog.handle_boot_enabled=0 boot.panic_on_fail boot.reset=%x rootdelay=3 rootfstype=ext4"
			 BOOT_KERNEL_LOGLEVEL,
			 *s == 'A' ? '2' : '3', *s, reset_reason);
		boot_raw(what, desc, lba + SLOT_FIT_AT, SLOT_FIT_MAX, args, true, lba);
	}
}

/*
 * The stash: slot A's signed FIT, which `flash` writes raw into a stock
 * card's p2 where slot A's tail will be (SLOT_A_LBA + SLOT_FIT_AT), outside
 * the rootfs and any filesystem. Read only while the card is not A/B: the
 * stock U-Boot then runs us from the stub's /boot/kernel.img.
 */
#define STASH_LBA		(SLOT_A_LBA + SLOT_FIT_AT)

/*
 * A stock card's first boots: the stash, under the slot rule (signature and
 * rollback floor). No rauc.slot=, gemba.stash instead, and the stub (the stock
 * p2) as the root: its initramfs installs our boot chain from this same stash,
 * then lays the card out. Like recovery, no
 * watchdog.handle_boot_enabled=0: the kernel keeps wdt0 fed. There is no try
 * counter (no env off the A/B layout), so a deterministic panic in the
 * initramfs loops through wdt0 resets: card wear, not a brick.
 */
static void boot_stash(void)
{
	struct blk_desc *desc = card();

	if (!desc || ab_layout(desc))
		return;
	/* Not A/B and the stash refused: nothing here may boot, so no pin cue and no wait. */
	if (!ax620e_stash_allowed(stock_layout(desc))) {
		printf("U-Boot: stash refused; resetting\n");
		reset_cpu();
	}
	unlocked = true;
	boot_raw("stash", desc, STASH_LBA, SLOT_FIT_MAX,
		 SLOT_CONSOLE EARLYCON "init=/sbin/init root=/dev/mmcblk" DISK "p2 gemba.stash rootdelay=3 rootfstype=ext4"
		 BOOT_KERNEL_LOGLEVEL, true, 0);
	unlocked = false;
}

/*
 * Recovery, from its raw area on an A/B card: held to its signature alone.
 * It keeps the default boot configs; boot_key=1 tells its initramfs it was asked for.
 */
static void boot_recovery(void)
{
	struct blk_desc *desc = card();

	if (!ab_layout(desc)) {
		printf("U-Boot: recovery: not an A/B card\n");
		return;
	}
	unlocked = true;
	boot_raw("recovery", desc, RECOVERY_LBA, RECOVERY_FIT_MAX, CONSOLE EARLYCON
		 "init=/sbin/init root=/dev/mmcblk" DISK "p2 rw rootdelay=3 rootfstype=ext4 boot_key=1", false, 0);
	unlocked = false;
}

/* Held, and still held 50 ms later: a glitch alone opens no USB. */
static bool pin_pressed(void)
{
	if (!recovery_held())
		return false;
	mdelay(50);
	return recovery_held();
}

/*
 * A slot (on a stock card, the stash), else recovery, which serves USB, so only
 * when the pin asks for it:
 * held at power-on, or while no slot boots. That wait ends in wdt0's reset
 * (armed above; nothing in this build pets it: no CONFIG_WDT or CONFIG_CYCLIC,
 * which would make the wait endless) and the slots are tried again. No prompt.
 */
static int do_abboot(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	if (!pin_held) {
		boot_slots();
		boot_stash();
		printf("U-Boot: no slot booted; waiting for the pin\n");
		while (!pin_pressed())
			;
	}
	boot_recovery();
	reset_cpu();
	return CMD_RET_FAILURE;
}

U_BOOT_CMD(abboot, 1, 0, do_abboot, "boot slot A or B, else recovery on the pin", "");
