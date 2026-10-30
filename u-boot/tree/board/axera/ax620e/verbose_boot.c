// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2026, GembaHive, Inc.
 */
/*
 * The front panel as a video device, for U-Boot's console: the console draws
 * on a frame buffer of its own, and each sync sends the changed rectangle with
 * the background replaced by the boot logo and the text outlined in black, so
 * the text sits over the logo. The panel is already set up and showing the
 * logo; this only writes pixels (ST7789 CASET, RASET, RAMWR).
 */

#include <dm.h>
#include <malloc.h>
#include <spi.h>
#include <video.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/kernel.h>

#include "logo_gram.h"

#define PANEL_W			240
#define PANEL_H			284
#define PANEL_HZ		20000000

static_assert(sizeof(logo_gram) == PANEL_W * PANEL_H * 2);

/* GPIO1 line 19, one word at (19 + 1) * 4 into its bank: low for a command. */
#define PANEL_DC		(0x04801000UL + 20 * 4)
#define   GPIO_DR		BIT(0)
#define   GPIO_DDR		BIT(1)	/* output */

struct panel_priv {
	struct spi_slave *spi;
	u8 *tx;		/* the pixels sent, big-endian RGB565 as the panel takes them */
};

static int panel_send(struct spi_slave *spi, u8 cmd, const void *data, uint len)
{
	int ret;

	clrsetbits_le32(PANEL_DC, GPIO_DR | GPIO_DDR, GPIO_DDR);
	ret = spi_xfer(spi, 8, &cmd, NULL, SPI_XFER_ONCE);
	if (ret || !len)
		return ret;
	setbits_le32(PANEL_DC, GPIO_DR);
	return spi_xfer(spi, len * 8, data, NULL, SPI_XFER_ONCE);
}

/* [start, end) as CASET's and RASET's parameters: first and last, big-endian. */
static void span(u8 *p, uint start, uint end)
{
	p[0] = start >> 8;
	p[1] = start;
	p[2] = (end - 1) >> 8;
	p[3] = end - 1;
}

static bool ink(const u16 *fb, u16 bg, int x, int y)
{
	return x >= 0 && x < PANEL_W && y >= 0 && y < PANEL_H && fb[y * PANEL_W + x] != bg;
}

/* A text pixel as drawn; a background one black next to text, else the logo's. */
static void compose(u8 *out, const u16 *fb, u16 bg, int x, int y)
{
	int i = y * PANEL_W + x;
	int dx, dy;

	if (fb[i] != bg) {
		out[0] = fb[i] >> 8;
		out[1] = fb[i];
		return;
	}
	out[0] = logo_gram[2 * i];
	out[1] = logo_gram[2 * i + 1];
	for (dy = -1; dy <= 1; dy++)
		for (dx = -1; dx <= 1; dx++)
			if (ink(fb, bg, x + dx, y + dy))
				out[0] = out[1] = 0;
}

static int panel_sync(struct udevice *dev)
{
	struct video_priv *uc = dev_get_uclass_priv(dev);
	struct panel_priv *priv = dev_get_priv(dev);
	/* The damage, and the outline one pixel around it. */
	int x0 = max(uc->damage.xstart - 1, 0);
	int y0 = max(uc->damage.ystart - 1, 0);
	int x1 = min(uc->damage.xend + 1, PANEL_W);
	int y1 = min(uc->damage.yend + 1, PANEL_H);
	u8 cols[4], rows[4], *p = priv->tx;
	int x, y, ret;

	if (uc->damage.xend <= uc->damage.xstart || uc->damage.yend <= uc->damage.ystart)
		return 0;
	for (y = y0; y < y1; y++)
		for (x = x0; x < x1; x++, p += 2)
			compose(p, uc->fb, uc->colour_bg, x, y);
	span(cols, x0, x1);
	span(rows, y0, y1);

	ret = spi_claim_bus(priv->spi);
	if (ret)
		return ret;
	ret = panel_send(priv->spi, 0x2a, cols, sizeof(cols));
	if (!ret)
		ret = panel_send(priv->spi, 0x2b, rows, sizeof(rows));
	if (!ret)
		ret = panel_send(priv->spi, 0x2c, priv->tx, p - priv->tx);
	spi_release_bus(priv->spi);
	return ret;
}

/* Bound after relocation only: the frame buffer is the driver's, from malloc. */
static int panel_bind(struct udevice *dev)
{
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);

	plat->size = PANEL_W * PANEL_H * 2;
	plat->base = (ulong)malloc(plat->size);
	return plat->base ? 0 : -ENOMEM;
}

static int panel_probe(struct udevice *dev)
{
	struct video_priv *uc = dev_get_uclass_priv(dev);
	struct panel_priv *priv = dev_get_priv(dev);
	struct udevice *bus;
	int ret;

	uc->xsize = PANEL_W;
	uc->ysize = PANEL_H;
	uc->bpix = VIDEO_BPP16;
	ret = _spi_get_bus_and_cs(0, 0, PANEL_HZ, SPI_MODE_0, "spi_generic_drv",
				  "panel", &bus, &priv->spi);
	if (ret)
		return ret;
	priv->tx = malloc(PANEL_W * PANEL_H * 2);
	return priv->tx ? 0 : -ENOMEM;
}

static const struct video_ops panel_ops = {
	.video_sync = panel_sync,
};

U_BOOT_DRIVER(ax620e_panel) = {
	.name = "ax620e_panel",
	.id = UCLASS_VIDEO,
	.bind = panel_bind,
	.probe = panel_probe,
	.ops = &panel_ops,
	.priv_auto = sizeof(struct panel_priv),
};

/* Not in the control DT, which the 32-bit U-Boot shares: the panel has no driver there. */
U_BOOT_DRVINFO(ax620e_panel) = {
	.name = "ax620e_panel",
};
