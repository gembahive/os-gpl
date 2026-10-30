// SPDX-License-Identifier: GPL-2.0+
/*
 * Logo on the front panel (ST7789, 240x284, on SPI_m2): init sequence, pixels
 * from GRAM (0,0), then the backlight. Pads and the SPI clock are set earlier.
 */

#include <log.h>
#include <spi.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/delay.h>

#include "logo_gram.h"

/* An Axera GPIO line is one word, (n + 1) * 4 into its bank. */
#define GPIO_LINE(bank, n)	((bank) + ((n) + 1) * 4)
#define   GPIO_DR		BIT(0)
#define   GPIO_DDR		BIT(1)	/* output */
#define GPIO0			0x04800000
#define GPIO1			0x04801000

#define PANEL_BL		GPIO_LINE(GPIO0, 30)	/* high: on */
#define PANEL_RST		GPIO_LINE(GPIO0, 31)	/* low: reset */
#define PANEL_DC		GPIO_LINE(GPIO1, 19)	/* low: command */
#define PANEL_CS		GPIO_LINE(GPIO1, 27)	/* low: selected */

#define PANEL_HZ		20000000

static void pin(ulong line, int high)
{
	clrsetbits_le32(line, GPIO_DR | GPIO_DDR, GPIO_DDR | (high ? GPIO_DR : 0));
}

/* SPI_m2's SS has no pad here: the panel's CS is a GPIO, around each transfer. */
void external_cs_manage(struct udevice *dev, bool on)
{
	pin(PANEL_CS, on);
}

/* SPI_m2 runs on cpll_208m; there is no clock driver. */
int dw_spi_get_clk(struct udevice *bus, ulong *rate)
{
	*rate = 208000000;
	return 0;
}

/* Delay after (ms), length (command + parameters), command, parameters. */
static const u8 panel_init[] = {
	120, 1, 0x11,				/* sleep out */
	0, 2, 0x3a, 0x05,			/* RGB565 */
	0, 6, 0xb2, 0x0c, 0x0c, 0x00, 0x33, 0x33,
	0, 1, 0x20,
	0, 2, 0xb7, 0x05,
	0, 2, 0xbb, 0x21,
	0, 2, 0xc0, 0x2c,
	0, 2, 0xc2, 0x01,
	0, 2, 0xc3, 0x15,
	0, 2, 0xc6, 0x0f,
	0, 2, 0xd0, 0xa7,
	0, 3, 0xd0, 0xa4, 0xa1,
	0, 2, 0xd6, 0xa1,
	0, 15, 0xe0, 0xf0, 0x05, 0x0e, 0x08, 0x0a, 0x17, 0x39, 0x54, 0x4e, 0x37,
		0x12, 0x12, 0x31, 0x37,
	0, 15, 0xe1, 0xf0, 0x10, 0x14, 0x0d, 0x0b, 0x05, 0x39, 0x44, 0x4d, 0x38,
		0x14, 0x14, 0x2e, 0x35,
	0, 4, 0xe4, 0x23, 0x00, 0x00,
	0, 1, 0x21,				/* inversion on */
	0, 1, 0x29,				/* display on */
};

static int panel_write(struct spi_slave *spi, u8 cmd, const u8 *data, uint len)
{
	int ret;

	pin(PANEL_DC, 0);
	ret = spi_xfer(spi, 8, &cmd, NULL, SPI_XFER_ONCE);
	if (ret)
		return ret;
	pin(PANEL_DC, 1);
	return len ? spi_xfer(spi, len * 8, data, NULL, SPI_XFER_ONCE) : 0;
}

void logo_show(void)
{
	struct udevice *bus;
	struct spi_slave *spi;
	const u8 *p;
	int ret;

	pin(PANEL_BL, 0);
	pin(PANEL_RST, 1);
	pin(PANEL_CS, 1);
	ret = _spi_get_bus_and_cs(0, 0, PANEL_HZ, SPI_MODE_0, "spi_generic_drv",
				  "panel", &bus, &spi);
	if (!ret)
		ret = spi_claim_bus(spi);
	if (ret) {
		log_err("U-Boot: panel SPI unavailable (%d)\n", ret);
		return;
	}

	mdelay(1);
	pin(PANEL_RST, 0);
	mdelay(1);
	pin(PANEL_RST, 1);
	mdelay(130);
	for (p = panel_init; p < panel_init + sizeof(panel_init); p += 2 + p[1]) {
		ret = panel_write(spi, p[2], p + 3, p[1] - 1);
		if (ret)
			goto out;
		mdelay(p[0]);
	}
	ret = panel_write(spi, 0x2c, logo_gram, sizeof(logo_gram));	/* memory write */
	if (ret)
		goto out;
	mdelay(100);	/* let the panel settle before the backlight */
	pin(PANEL_BL, 1);
out:
	if (ret)
		log_err("U-Boot: panel SPI transfer failed (%d)\n", ret);
	pin(PANEL_CS, 1);
	spi_release_bus(spi);
}
