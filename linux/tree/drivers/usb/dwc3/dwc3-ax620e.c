// SPDX-License-Identifier: GPL-2.0
/*
 * Axera AX620E DWC3 glue. The core owns its clocks and resets; the glue
 * holds the alternate reference gate and sets flash-domain VBUSVALID.
 * Register layout from the vendor dwc3-axera.c.
 */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/mfd/syscon.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include "debug.h"
#include "glue.h"
#include "io.h"

#define AX620E_FLASH_USB2_CTRL	0x40
#define AX620E_FLASH_USB2_CTRL_SET	0x4040
#define AX620E_USB2_VBUSVALID	BIT(6)

static int dwc3_ax620e_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dwc3_probe_data data = {};
	struct regmap *syscon;
	struct dwc3 *dwc;
	struct clk *ref_alt;
	unsigned int value;
	int ret;

	dwc = devm_kzalloc(dev, sizeof(*dwc), GFP_KERNEL);
	if (!dwc)
		return -ENOMEM;

	data.res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!data.res)
		return -ENODEV;

	/* Its rate is unknown: enable the gate without overriding core timing. */
	ref_alt = devm_clk_get_enabled(dev, "ref_alt");
	if (IS_ERR(ref_alt))
		return dev_err_probe(dev, PTR_ERR(ref_alt), "alternate reference clock\n");

	syscon = syscon_regmap_lookup_by_phandle(dev->of_node, "axera,flash-syscon");
	if (IS_ERR(syscon))
		return dev_err_probe(dev, PTR_ERR(syscon), "flash syscon\n");

	/* No VBUS comparator reaches the controller: a gadget only pulls up D+ with this set. */
	ret = regmap_write(syscon, AX620E_FLASH_USB2_CTRL_SET, AX620E_USB2_VBUSVALID);
	if (ret)
		return ret;
	ret = regmap_read(syscon, AX620E_FLASH_USB2_CTRL, &value);
	if (ret)
		return ret;
	if (!(value & AX620E_USB2_VBUSVALID))
		return dev_err_probe(dev, -EIO, "VBUSVALID did not latch\n");

	dwc->dev = dev;
	data.dwc = dwc;
	return dwc3_core_probe(&data);
}

static void dwc3_ax620e_remove(struct platform_device *pdev)
{
	dwc3_core_remove(platform_get_drvdata(pdev));
}

/* As dwc3-generic-plat: the core parks gadget events while runtime-suspended. */
static int dwc3_ax620e_runtime_suspend(struct device *dev)
{
	return dwc3_runtime_suspend(dev_get_drvdata(dev));
}

static int dwc3_ax620e_runtime_resume(struct device *dev)
{
	return dwc3_runtime_resume(dev_get_drvdata(dev));
}

static int dwc3_ax620e_runtime_idle(struct device *dev)
{
	return dwc3_runtime_idle(dev_get_drvdata(dev));
}

static const struct dev_pm_ops dwc3_ax620e_pm_ops = {
	RUNTIME_PM_OPS(dwc3_ax620e_runtime_suspend, dwc3_ax620e_runtime_resume,
		       dwc3_ax620e_runtime_idle)
};

/*
 * The device port's link state, as debugfs's link_state reads it, available
 * without DEBUG_FS: with VBUS forced valid the UDC state stays "configured"
 * across an unplug, so userspace needs the link state to detect a cable.
 */
static ssize_t link_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct dwc3 *dwc = dev_get_drvdata(dev);
	enum dwc3_link_state state;
	unsigned long flags;
	const char *name;
	u32 reg;
	int ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return ret;
	spin_lock_irqsave(&dwc->lock, flags);
	if (DWC3_GSTS_CURMOD(dwc3_readl(dwc, DWC3_GSTS)) != DWC3_GSTS_CURMOD_DEVICE) {
		name = "Not available";
	} else {
		reg = dwc3_readl(dwc, DWC3_DSTS);
		state = DWC3_DSTS_USBLNKST(reg);
		name = (reg & DWC3_DSTS_CONNECTSPD) >= DWC3_DSTS_SUPERSPEED ?
		       dwc3_gadget_link_string(state) : dwc3_gadget_hs_link_string(state);
	}
	spin_unlock_irqrestore(&dwc->lock, flags);
	pm_runtime_put_sync(dev);
	return sysfs_emit(buf, "%s\n", name);
}
static DEVICE_ATTR_RO(link_state);

static struct attribute *dwc3_ax620e_attrs[] = {
	&dev_attr_link_state.attr,
	NULL
};
ATTRIBUTE_GROUPS(dwc3_ax620e);

static const struct of_device_id dwc3_ax620e_of_match[] = {
	{ .compatible = "axera,ax620e-dwc3" },
	{ }
};
MODULE_DEVICE_TABLE(of, dwc3_ax620e_of_match);

static struct platform_driver dwc3_ax620e_driver = {
	.probe = dwc3_ax620e_probe,
	.remove = dwc3_ax620e_remove,
	.driver = {
		.name = "dwc3-ax620e",
		.of_match_table = dwc3_ax620e_of_match,
		.dev_groups = dwc3_ax620e_groups,
		.pm = pm_ptr(&dwc3_ax620e_pm_ops),
	},
};
module_platform_driver(dwc3_ax620e_driver);

MODULE_DESCRIPTION("Axera AX620E DWC3 glue");
MODULE_LICENSE("GPL");
