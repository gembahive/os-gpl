// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026, GembaHive, Inc.
 */
/* Public UID words cached by the SPL; no eFuse controller access. */

#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

struct ax620e_uid {
	u32 low, high;
};

static int ax620e_uid_show(struct seq_file *m, void *unused)
{
	const struct ax620e_uid *uid = m->private;

	seq_printf(m, "ax_uid: 0x%08x%08x\n", uid->high, uid->low);
	return 0;
}

static void ax620e_uid_remove(void *entry)
{
	proc_remove(entry);
}

static int ax620e_uid_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct proc_dir_entry *dir, *entry;
	struct ax620e_uid *uid;
	struct resource *res;
	void __iomem *base;
	int ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || res->start != 0x788 || resource_size(res) != 8)
		return -EINVAL;
	base = devm_ioremap(dev, res->start, resource_size(res));
	if (!base)
		return -ENOMEM;
	uid = devm_kzalloc(dev, sizeof(*uid), GFP_KERNEL);
	if (!uid)
		return -ENOMEM;
	uid->low = readl(base);
	uid->high = readl(base + 4);

	dir = proc_mkdir("ax_proc", NULL);
	if (!dir)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, ax620e_uid_remove, dir);
	if (ret)
		return ret;
	entry = proc_create_single_data("uid", 0444, dir, ax620e_uid_show, uid);
	if (!entry)
		return -ENOMEM;
	return 0;
}

static const struct of_device_id ax620e_uid_match[] = {
	{ .compatible = "axera,ax620e-uid" },
	{ }
};

static struct platform_driver ax620e_uid_driver = {
	.probe = ax620e_uid_probe,
	.driver = {
		.name = "ax620e-uid",
		.of_match_table = ax620e_uid_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(ax620e_uid_driver);
