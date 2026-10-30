// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2026, GembaHive, Inc.
 */
/*
 * The Edge (Sipeed NanoKVM Go+ hardware, AX620Q): boots from its TF card.
 * The boot itself is shared by every AX620E board.
 */

#include <init.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/sizes.h>

DECLARE_GLOBAL_DATA_PTR;

/* The loader's misc_info.board_id: its ADC strap and the chip type, decoded. */
#define LOADER_BOARD_ID		0x00000780
#define   BOARD_512M		3	/* AX620QE */
#define   BOARD_256M_AX620Q	16	/* the other 256 MiB one is AX620QF, 1 */

/* Physical RAM, all Linux's: bootm writes it into the DT's memory node (ARCH_FIXUP_FDT_MEMORY). */
int dram_init(void)
{
	gd->ram_size = readl(LOADER_BOARD_ID) == BOARD_512M ? SZ_512M : SZ_256M;
	return 0;
}

#define FLASH_CLK_MUX0_SET	0x10034000
#define FLASH_CLK_MUX0_CLR	0x10038000
#define FLASH_CLK_EB0_SET	0x10034004
#define FLASH_CLK_EB0_CLR	0x10038004
#define FLASH_CLK_DIV0_SET	0x1003400c
#define FLASH_CLK_DIV0_CLR	0x1003800c
#define   SD_CARD_EB		BIT(9)
#define   SD_CARD_SEL_NPLL400	(3 << 16)
#define   SD_CARD_DIV		GENMASK(25, 20)
#define   SD_CARD_DIV_2		(1 << 20)
#define   SD_CARD_DIV_UPDATE	BIT(26)
#define SD_PADS			0x104f1000UL	/* + a pad's offset: an address as wide as a pointer */
#define   SD_PAD_PULL		GENMASK(7, 6)
#define   SD_PAD_PULL_UP	(2 << 6)

/*
 * The card as the vendor U-Boot sets it up: its clock at npll_400m / 2, the
 * 200 MHz base its SDHCI caps report and the divider is computed from; CMD
 * and DAT pulled up. (The PHY DLL relock is in sdhci-cadence.)
 */
static void sd_init(void)
{
	static const u8 pads[] = { 0x0c, 0x18, 0x3c, 0x48, 0x30 };	/* DAT0-3, CMD */
	int i;

	writel(SD_CARD_EB, FLASH_CLK_EB0_CLR);
	udelay(2);
	writel(SD_CARD_SEL_NPLL400, FLASH_CLK_MUX0_CLR);
	writel(SD_CARD_SEL_NPLL400, FLASH_CLK_MUX0_SET);
	writel(SD_CARD_EB, FLASH_CLK_EB0_SET);
	writel(SD_CARD_DIV, FLASH_CLK_DIV0_CLR);
	writel(SD_CARD_DIV_2, FLASH_CLK_DIV0_SET);
	writel(SD_CARD_DIV_UPDATE, FLASH_CLK_DIV0_SET);
	udelay(2);
	writel(SD_CARD_DIV_UPDATE, FLASH_CLK_DIV0_CLR);

	for (i = 0; i < ARRAY_SIZE(pads); i++)
		clrsetbits_le32(SD_PADS + pads[i], SD_PAD_PULL, SD_PAD_PULL_UP);
}

/* The vendor's pin tables for this board, as address/value pairs. */
static const u32 pins_ax620qz[] = {
#include "AX620QZ_DEMO_pinmux.h"
};

static const u32 pins_ax620q[] = {
#include "AX620Q_DEMO_pinmux.h"
};

#define DPHYTX_PADS		0x0230a000
#define DPHYTX_RST_SET		0x046000b8
#define   DPHYTX_RST		BIT(6)
#define DPHYTX_MIPI_EN		0x023f110c
#define PAD_FUNC		GENMASK(18, 16)

/*
 * Every pad as the vendor U-Boot sets it; Linux's pinctrl writes none of
 * them. A MIPI TX pad taken as a GPIO needs the D-PHY held in reset first.
 */
static void pins_init(void)
{
	bool q = readl(LOADER_BOARD_ID) == BOARD_256M_AX620Q;
	const u32 *t = q ? pins_ax620q : pins_ax620qz;
	int i, n = q ? ARRAY_SIZE(pins_ax620q) : ARRAY_SIZE(pins_ax620qz);

	for (i = 0; i < n; i += 2) {
		if (t[i] - DPHYTX_PADS < 0x1000 && t[i + 1] & PAD_FUNC) {
			writel(DPHYTX_RST, DPHYTX_RST_SET);
			writel(0, DPHYTX_MIPI_EN);
		}
		writel(t[i + 1], (ulong)t[i]);
	}
}

void logo_show(void);

/* Before the card is first read (initr_mmc). */
int board_init(void)
{
	pins_init();
	sd_init();
	logo_show();
	return 0;
}

#define PIN_BUTTON_PAD		0x02302024	/* GPIO3_A2 */
#define PIN_BUTTON_DIR		0x0600100c
#define PIN_BUTTON_IN		0x0600108c

/* The pad as a GPIO input, as the vendor U-Boot reads it; low while held. */
bool recovery_held(void)
{
	writel(3, PIN_BUTTON_PAD);
	clrbits_le32(PIN_BUTTON_DIR, BIT(1));
	udelay(1000);
	return !(readl(PIN_BUTTON_IN) & BIT(2));
}
