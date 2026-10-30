// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026, GembaHive, Inc.
 */
/*
 * Confirms the firmware bundle that booted: one call to BL31's FW_CONFIRM
 * (SMC32 fast SiP 0x82000002), which clears BACKUP1's FW_TRIED. A trial
 * bundle that got this far (BL31, both CPUs, Linux) is then kept at the next
 * reset instead of reverted by SPL64; a later rootfs or userspace failure is
 * the slots' business, not the bundle's.
 *
 * Once per boot, from probe, with no userspace interface (and no bind/unbind
 * to run it again).
 * TODO: with OP-TEE, defer the probe until its tee device is up.
 */

#include <linux/arm-smccc.h>
#include <linux/cpumask.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>

#define AX620E_SIP_FW_CONFIRM \
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_32, ARM_SMCCC_OWNER_SIP, 2)

static int ax620e_fw_probe(struct platform_device *pdev)
{
	struct arm_smccc_res res;

	if (num_online_cpus() != num_present_cpus()) {
		dev_err(&pdev->dev, "%u of %u CPUs online: bundle not confirmed\n",
			num_online_cpus(), num_present_cpus());
		return -ENODEV;
	}
	arm_smccc_1_1_invoke(AX620E_SIP_FW_CONFIRM, &res);
	if (res.a0 != SMCCC_RET_SUCCESS) {
		dev_err(&pdev->dev, "FW_CONFIRM failed (%ld): bundle not confirmed\n", (long)res.a0);
		return -EIO;
	}
	dev_info(&pdev->dev, "bundle confirmed\n");
	return 0;
}

static const struct of_device_id ax620e_fw_match[] = {
	{ .compatible = "axera,ax620e-fw" },
	{ }
};

static struct platform_driver ax620e_fw_driver = {
	.probe = ax620e_fw_probe,
	.driver = {
		.name = "ax620e-fw",
		.of_match_table = ax620e_fw_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(ax620e_fw_driver);
