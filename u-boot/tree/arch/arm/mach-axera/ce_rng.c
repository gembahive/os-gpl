// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2026, GembaHive, Inc.
 */
/*
 * Random numbers from the EIP-130 security engine, through mailbox 2, polled.
 * The engine's firmware is loaded unless an earlier stage already did.
 */

#include <dm.h>
#include <dm/device_compat.h>
#include <rng.h>
#include <time.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/string.h>

#define MBOX(n)			(((n) - 1) * 0x400)
#define MBOX_CTRL		0x3f00	/* write: per-mailbox nibble commands */
#define MBOX_STAT		0x3f00	/* read: per-mailbox nibble state */
#define MBOX_LOCKOUT		0x3f10
#define MODULE_STATUS		0x3fe0
#define FW_RAM			0x4000
#define FW_WORDS		19632
#define NIB(n, v)		((v) << (((n) - 1) * 4))
#define MB_IN_FULL		1	/* ctrl: submit;      stat: command pending */
#define MB_OUT_FULL		2	/* ctrl: result read; stat: result ready */
#define MB_LINK			4
#define MB_UNLINK		8
#define ST_CRC24_BUSY		BIT(8)
#define ST_CRC24_OK		BIT(9)
#define ST_FW_WRITTEN		BIT(20)
#define ST_FW_CHECKS_DONE	BIT(22)
#define ST_FW_ACCEPTED		BIT(23)
#define ST_FATAL		BIT(31)
#define MB			2
#define TOKEN_WORDS		64

/*
 * The periph block's write-1-to-set/clear aliases: CE bus clock from
 * cpll_312m (MUX0 bit 1); the cnt, aclk and pclk gates (EB0 bit 0, EB1 bit 1,
 * EB2 bit 12); CE resets released (RST0 bit 4, then 5-8) with pclk stopped, as
 * CE_PRST (bit 8) must not leave reset on a running APB clock. Never asserted:
 * only the engine's first owner each boot (OP-TEE) resets it.
 */
#define PERIPH			0x04870000UL
#define PERI_MUX0_SET		(PERIPH + 0xa8)
#define PERI_EB0_SET		(PERIPH + 0xb0)
#define PERI_EB1_SET		(PERIPH + 0xb8)
#define PERI_EB2_SET		(PERIPH + 0xc0)
#define PERI_EB2_CLR		(PERIPH + 0xc4)
#define PERI_RST0_CLR		(PERIPH + 0xdc)

#define IDENTITY		0x4f5a3647	/* W1 of every token */
#define RESULT_ERROR		BIT(31)
#define RESULT_CODE(w0)		(((w0) >> 24) & 0x7f)
#define TOKEN_US		5000000
#define RNG_LEN			64

struct ce_rng_priv {
	void __iomem *base;
	u16 token_id;
};

/*
 * Where the engine's DMA writes the random numbers. A token that timed out may
 * still write it later, once only: after any failure the engine is not used.
 */
static u8 rng_buf[ALIGN(RNG_LEN, ARCH_DMA_MINALIGN)] __aligned(ARCH_DMA_MINALIGN);

/* Set by any failure: probe and reads then fail at once, so a dead engine's timeouts cost once. */
static bool dead;

/*
 * One command token; res gets its result. A result with another token ID is
 * a late one, from a token that timed out: acknowledged and skipped. Any
 * error result is a failure.
 */
static int token(struct ce_rng_priv *priv, u32 *cmd, u32 *res)
{
	void __iomem *b = priv->base;
	ulong end = timer_get_us() + TOKEN_US;
	long left;
	u32 st;
	int i, ret;

	cmd[0] |= ++priv->token_id;
	cmd[1] = IDENTITY;
	if (readl(b + MBOX_STAT) & NIB(MB, MB_IN_FULL))
		return -EBUSY;
	for (i = 0; i < TOKEN_WORDS; i++)
		writel(cmd[i], b + MBOX(MB) + i * 4);
	writel(NIB(MB, MB_IN_FULL), b + MBOX_CTRL);
	do {
		left = (long)(end - timer_get_us());
		if (left <= 0)
			return -ETIMEDOUT;
		ret = readl_poll_timeout(b + MBOX_STAT, st, st & NIB(MB, MB_OUT_FULL), left);
		if (ret)
			return ret;
		for (i = 0; i < TOKEN_WORDS; i++)
			res[i] = readl(b + MBOX(MB) + i * 4);
		writel(NIB(MB, MB_OUT_FULL), b + MBOX_CTRL);
	} while ((res[0] & 0xffff) != (cmd[0] & 0xffff));
	return res[0] & RESULT_ERROR ? -EIO : 0;
}

/*
 * RandomNumber: W2 = length, W3/W4 = the buffer's address. A result code
 * without the error bit is a warning that the TRNG's statistics are off: no
 * bytes then either.
 */
static int random_token(struct ce_rng_priv *priv, u8 *out, size_t len)
{
	dma_addr_t dma = dma_map_single(rng_buf, sizeof(rng_buf), DMA_FROM_DEVICE);
	u32 cmd[TOKEN_WORDS] = { 0x04000000, 0, len, lower_32_bits(dma), upper_32_bits(dma) };
	u32 res[TOKEN_WORDS] = { };
	int ret = token(priv, cmd, res);

	dma_unmap_single(dma, sizeof(rng_buf), DMA_FROM_DEVICE);
	if (!ret && RESULT_CODE(res[0]))
		ret = -EIO;
	if (!ret)
		memcpy(out, rng_buf, len);
	memset(rng_buf, 0, sizeof(rng_buf));
	return ret;
}

static int ce_rng_read(struct udevice *dev, void *data, size_t len)
{
	struct ce_rng_priv *priv = dev_get_priv(dev);
	u8 *out = data;
	int ret;

	while (len) {
		size_t n = min_t(size_t, len, RNG_LEN);

		ret = dead ? -EIO : random_token(priv, out, n);
		if (ret) {
			dead = true;
			return ret;
		}
		out += n;
		len -= n;
	}
	return 0;
}

extern const u32 ce_fw[], ce_fw_end[];

/* The first 256 bytes of the image go through mailbox 1, the rest into FW_RAM. */
static int ce_load(void __iomem *b)
{
	int i;

	if (ce_fw_end - ce_fw != FW_WORDS)
		return -EINVAL;
	writel(NIB(1, MB_LINK), b + MBOX_CTRL);
	if (!(readl(b + MBOX_STAT) & NIB(1, MB_LINK)))
		return -EIO;
	for (i = 0; i < TOKEN_WORDS; i++)
		writel(ce_fw[i], b + MBOX(1) + i * 4);
	writel(NIB(1, MB_IN_FULL), b + MBOX_CTRL);
	writel(NIB(1, MB_UNLINK), b + MBOX_CTRL);
	for (i = TOKEN_WORDS; i < FW_WORDS; i++)
		writel(ce_fw[i], b + FW_RAM + (i - TOKEN_WORDS) * 4);
	writel(ST_FW_WRITTEN, b + MODULE_STATUS);
	return 0;
}

static int ce_rng_of_to_plat(struct udevice *dev)
{
	struct ce_rng_priv *priv = dev_get_priv(dev);

	priv->base = dev_read_addr_ptr(dev);
	return priv->base ? 0 : -EINVAL;
}

/*
 * Clocks and resets, the firmware loaded (unless already written and not
 * rejected) and accepted, mailbox MB linked and open to
 * every master (lockout 0), then TRNG Configure: W2 = reseed (BIT 1) | load
 * and start (BIT 0); W3 = SampleCycles 1 << 16 | SampleDiv 0 << 8 |
 * NoiseBlocks 8.
 */
static int ce_rng_probe(struct udevice *dev)
{
	struct ce_rng_priv *priv = dev_get_priv(dev);
	void __iomem *b = priv->base;
	u32 cmd[TOKEN_WORDS] = { 0x14000000, 0, BIT(1) | BIT(0), 1 << 16 | 0 << 8 | 8 };
	u32 res[TOKEN_WORDS] = { };
	u32 st;
	int ret;

	if (dead)
		return -EIO;
	writel(BIT(1), PERI_MUX0_SET);
	writel(BIT(0), PERI_EB0_SET);
	writel(BIT(1), PERI_EB1_SET);
	writel(BIT(12), PERI_EB2_CLR);
	writel(BIT(4), PERI_RST0_CLR);
	writel(GENMASK(8, 5), PERI_RST0_CLR);
	writel(BIT(12), PERI_EB2_SET);

	ret = readl_poll_timeout(b + MODULE_STATUS, st, !(st & ST_CRC24_BUSY), 100000);
	if (ret || !(st & ST_CRC24_OK) || (st & ST_FATAL))
		goto bad;
	if ((!(st & ST_FW_WRITTEN) || ((st & ST_FW_CHECKS_DONE) && !(st & ST_FW_ACCEPTED))) &&
	    ce_load(b))
		goto bad;
	ret = readl_poll_timeout(b + MODULE_STATUS, st, st & ST_FW_CHECKS_DONE, 1000000);
	if (ret || !(st & ST_FW_ACCEPTED))
		goto bad;
	writel(NIB(MB, MB_LINK), b + MBOX_CTRL);
	if (!(readl(b + MBOX_STAT) & NIB(MB, MB_LINK)))
		goto bad;
	writel(0, b + MBOX_LOCKOUT);

	ret = token(priv, cmd, res);
	if (ret) {
		writel(NIB(MB, MB_UNLINK), b + MBOX_CTRL);
		dev_err(dev, "TRNG Configure: %d, result %08x\n", ret, res[0]);
		dead = true;
	}
	return ret;
bad:
	dev_err(dev, "engine not ready, status %08x\n", st);
	dead = true;
	return -EIO;
}

/* Before the OS: mailbox MB unlinked, as Linux expects to find it. */
static int ce_rng_remove(struct udevice *dev)
{
	struct ce_rng_priv *priv = dev_get_priv(dev);

	writel(NIB(MB, MB_UNLINK), priv->base + MBOX_CTRL);
	return 0;
}

static const struct dm_rng_ops ce_rng_ops = {
	.read = ce_rng_read,
};

static const struct udevice_id ce_rng_ids[] = {
	{ .compatible = "axera,ax620e-ce" },
	{ }
};

U_BOOT_DRIVER(ax620e_ce_rng) = {
	.name = "ax620e_ce_rng",
	.id = UCLASS_RNG,
	.of_match = ce_rng_ids,
	.of_to_plat = ce_rng_of_to_plat,
	.probe = ce_rng_probe,
	.remove = ce_rng_remove,
	.ops = &ce_rng_ops,
	.priv_auto = sizeof(struct ce_rng_priv),
	.flags = DM_FLAG_OS_PREPARE,
};
