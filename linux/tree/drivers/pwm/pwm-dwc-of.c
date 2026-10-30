// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026, GembaHive, Inc.
 */
/*
 * DesignWare APB timers in PWM mode, memory-mapped and described in DT
 * (snps,dw-apb-timers-pwm2). pwm-dwc-core drives the registers; the only
 * other front end is PCI.
 */

#include <linux/clk.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/reset.h>

#include "pwm-dwc.h"

static int dwc_pwm_of_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct reset_control *rstc;
	struct pwm_chip *chip;
	struct dwc_pwm *dwc;
	struct clk *timer;
	unsigned long rate;
	int ret;

	chip = dwc_pwm_alloc(dev);
	if (IS_ERR(chip))
		return PTR_ERR(chip);
	dwc = to_dwc_pwm(chip);

	dwc->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dwc->base))
		return PTR_ERR(dwc->base);

	ret = PTR_ERR_OR_ZERO(devm_clk_get_enabled(dev, "bus"));
	if (ret)
		return dev_err_probe(dev, ret, "bus clock\n");

	timer = devm_clk_get_enabled(dev, "timer");
	if (IS_ERR(timer))
		return dev_err_probe(dev, PTR_ERR(timer), "timer clock\n");

	/* Released, not pulsed: a pulse would blink a backlight at every boot. */
	rstc = devm_reset_control_array_get_optional_exclusive(dev);
	if (IS_ERR(rstc))
		return PTR_ERR(rstc);
	ret = reset_control_deassert(rstc);
	if (ret)
		return ret;

	/* The core counts in whole nanoseconds per tick (24 MHz: 42, 0.8 % long). */
	rate = clk_get_rate(timer);
	if (!rate)
		return -EINVAL;
	dwc->clk_ns = DIV_ROUND_CLOSEST(NSEC_PER_SEC, rate);

	return devm_pwmchip_add(dev, chip);
}

static const struct of_device_id dwc_pwm_of_match[] = {
	{ .compatible = "snps,dw-apb-timers-pwm2" },
	{ }
};
MODULE_DEVICE_TABLE(of, dwc_pwm_of_match);

static struct platform_driver dwc_pwm_of_driver = {
	.probe = dwc_pwm_of_probe,
	.driver = {
		.name = "dwc-pwm-of",
		.of_match_table = dwc_pwm_of_match,
	},
};
module_platform_driver(dwc_pwm_of_driver);

MODULE_DESCRIPTION("DesignWare PWM Controller (DT)");
MODULE_LICENSE("GPL");
