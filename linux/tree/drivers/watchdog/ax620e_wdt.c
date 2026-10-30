// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026, GembaHive, Inc.
 */
/*
 * Axera AX620E watchdog (same block as the AX630C), with the vendor ax_wdt.c's
 * timeout arithmetic.
 *
 * U-Boot arms it, so it is the boot-time dog the core must keep petting, and it
 * reboots (on arm64 PSCI's restart, priority 129, wins: BL31 resets via wdt0).
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/reset.h>
#include <linux/watchdog.h>

#define AX620E_WDT_EN		0x00
#define AX620E_WDT_TORR		0x0c
#define AX620E_WDT_TORR_LOAD	0x18
#define AX620E_WDT_CCVR		0x24
#define AX620E_WDT_CRR		0x30
#define AX620E_WDT_CRR_KICK	0x61696370
/* TORR counts 64Ki-tick units per stage; the reset fires at the 2nd expiry. */
#define AX620E_WDT_TORR_SHIFT	16
#define AX620E_WDT_STAGES	2
#define AX620E_WDT_TORR_MAX	0xffffU

struct ax620e_wdt {
	struct watchdog_device wdd;
	void __iomem *base;
	unsigned long rate;
	unsigned int strobe_us;
	struct notifier_block halt_nb;
};

/* CRR and TORR_LOAD are levels sampled in the counter clock: hold two ticks. */
static void ax620e_wdt_strobe(struct ax620e_wdt *wdt, u32 reg, u32 val)
{
	writel(val, wdt->base + reg);
	udelay(wdt->strobe_us);
	writel(0, wdt->base + reg);
}

static int ax620e_wdt_ping(struct watchdog_device *wdd)
{
	struct ax620e_wdt *wdt = watchdog_get_drvdata(wdd);

	ax620e_wdt_strobe(wdt, AX620E_WDT_CRR, AX620E_WDT_CRR_KICK);
	return 0;
}

static void ax620e_wdt_load(struct ax620e_wdt *wdt, unsigned int timeout)
{
	u64 units = DIV_ROUND_UP_ULL((u64)timeout * wdt->rate,
				     AX620E_WDT_STAGES << AX620E_WDT_TORR_SHIFT);

	writel(min_t(u64, units, AX620E_WDT_TORR_MAX), wdt->base + AX620E_WDT_TORR);
	ax620e_wdt_strobe(wdt, AX620E_WDT_TORR_LOAD, 1);
	ax620e_wdt_ping(&wdt->wdd);
}

static int ax620e_wdt_set_timeout(struct watchdog_device *wdd, unsigned int t)
{
	ax620e_wdt_load(watchdog_get_drvdata(wdd), t);
	wdd->timeout = t;
	return 0;
}

static int ax620e_wdt_start(struct watchdog_device *wdd)
{
	struct ax620e_wdt *wdt = watchdog_get_drvdata(wdd);

	ax620e_wdt_load(wdt, wdd->timeout);
	writel(1, wdt->base + AX620E_WDT_EN);
	return 0;
}

static int ax620e_wdt_stop(struct watchdog_device *wdd)
{
	struct ax620e_wdt *wdt = watchdog_get_drvdata(wdd);

	writel(0, wdt->base + AX620E_WDT_EN);
	clear_bit(WDOG_HW_RUNNING, &wdd->status);
	return 0;
}

static unsigned int ax620e_wdt_get_timeleft(struct watchdog_device *wdd)
{
	struct ax620e_wdt *wdt = watchdog_get_drvdata(wdd);

	return readl(wdt->base + AX620E_WDT_CCVR) / wdt->rate;
}

/* Atomic context: register writes and busy-waits only. */
static int ax620e_wdt_restart(struct watchdog_device *wdd, unsigned long action,
			      void *data)
{
	struct ax620e_wdt *wdt = watchdog_get_drvdata(wdd);

	writel(1, wdt->base + AX620E_WDT_EN);
	ax620e_wdt_load(wdt, 0);
	mdelay(500);
	return 0;
}

/*
 * The board has no power switch, so poweroff is a halt, and a halt must stay
 * halted: nowayout keeps userspace from stopping the dog (systemd's disarm at
 * poweroff included), so the kernel stops it here, after the last of
 * userspace. A reboot (SYS_DOWN) keeps it: it is what resets the SoC.
 */
static int ax620e_wdt_halt(struct notifier_block *nb, unsigned long code,
			   void *data)
{
	struct ax620e_wdt *wdt = container_of(nb, struct ax620e_wdt, halt_nb);

	if (code == SYS_HALT || code == SYS_POWER_OFF)
		ax620e_wdt_stop(&wdt->wdd);
	return NOTIFY_DONE;
}

static const struct watchdog_info ax620e_wdt_info = {
	.identity = "AX620E watchdog",
	.options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE,
};

static const struct watchdog_ops ax620e_wdt_ops = {
	.owner = THIS_MODULE,
	.start = ax620e_wdt_start,
	.stop = ax620e_wdt_stop,
	.ping = ax620e_wdt_ping,
	.set_timeout = ax620e_wdt_set_timeout,
	.get_timeleft = ax620e_wdt_get_timeleft,
	.restart = ax620e_wdt_restart,
};

static int ax620e_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct reset_control *rst;
	struct ax620e_wdt *wdt;
	struct clk *clk;
	int ret;

	wdt = devm_kzalloc(dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;

	wdt->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(wdt->base))
		return PTR_ERR(wdt->base);

	clk = devm_clk_get_enabled(dev, "apb");
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "apb clock\n");

	/* The counter clock: 24 MHz or the 32 kHz RTC, as the DT's mux picks. */
	clk = devm_clk_get_enabled(dev, "wdt");
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "wdt clock\n");
	wdt->rate = clk_get_rate(clk);
	if (!wdt->rate)
		return dev_err_probe(dev, -EINVAL, "wdt clock has no rate\n");
	wdt->strobe_us = DIV_ROUND_UP(2 * USEC_PER_SEC, wdt->rate);

	rst = devm_reset_control_array_get_optional_exclusive(dev);
	if (IS_ERR(rst))
		return dev_err_probe(dev, PTR_ERR(rst), "resets\n");
	ret = reset_control_deassert(rst);
	if (ret)
		return ret;

	wdt->wdd.info = &ax620e_wdt_info;
	wdt->wdd.ops = &ax620e_wdt_ops;
	wdt->wdd.parent = dev;
	wdt->wdd.min_timeout = DIV_ROUND_UP(AX620E_WDT_STAGES << AX620E_WDT_TORR_SHIFT,
					   wdt->rate);
	wdt->wdd.max_timeout = div_u64((u64)AX620E_WDT_STAGES * AX620E_WDT_TORR_MAX <<
				       AX620E_WDT_TORR_SHIFT, wdt->rate);
	/* The DT's timeout-sec, else the longest the counter holds. */
	wdt->wdd.timeout = wdt->wdd.max_timeout;
	watchdog_set_drvdata(&wdt->wdd, wdt);
	watchdog_init_timeout(&wdt->wdd, 0, dev);
	watchdog_set_nowayout(&wdt->wdd, WATCHDOG_NOWAYOUT);
	watchdog_set_restart_priority(&wdt->wdd, 128);

	/* U-Boot's dog: take it over so the core pets it until userspace opens it. */
	if (readl(wdt->base + AX620E_WDT_EN) & 1) {
		ax620e_wdt_start(&wdt->wdd);
		set_bit(WDOG_HW_RUNNING, &wdt->wdd.status);
	}

	wdt->halt_nb.notifier_call = ax620e_wdt_halt;
	ret = devm_register_reboot_notifier(dev, &wdt->halt_nb);
	if (ret)
		return ret;

	return devm_watchdog_register_device(dev, &wdt->wdd);
}

static const struct of_device_id ax620e_wdt_of_match[] = {
	{ .compatible = "axera,ax620e-wdt" },
	{ }
};
MODULE_DEVICE_TABLE(of, ax620e_wdt_of_match);

static struct platform_driver ax620e_wdt_driver = {
	.probe = ax620e_wdt_probe,
	.driver = {
		.name = "ax620e-wdt",
		.of_match_table = ax620e_wdt_of_match,
	},
};
module_platform_driver(ax620e_wdt_driver);

MODULE_DESCRIPTION("Axera AX620E watchdog");
MODULE_LICENSE("GPL");
