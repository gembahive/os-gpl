// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026, GembaHive, Inc.
 */
/*
 * Axera AX620E die temperature sensor, after the vendor 4.19 axera_thermal.c
 * (register sequence, conversions and calibration are its own).
 *
 * The block is the SoC's ADC; the sensor is a channel of its monitor. The
 * SPL enables the chip's thermal abort, so TEMP_HIGH is a hardware reset:
 * it takes the zone's hottest trip and is written before the monitor runs.
 * TEMP_LOW and TEMP_MEDIAN (the first two trips) only raise interrupts;
 * nothing requests them, the zone is polled.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/thermal.h>

#define THM_MA_CTRL		0x0c
#define THM_CTRL		0x18
#define THM_CLK_EN		0x1c
#define THM_RSTN		0x24
#define ADC_FILTER_SEL		0x2c
#define ADC_FILTER_EN		0x30
#define MSR_ONE_EN		0x38
#define THM_DATA0		0x78
#define MSR_VREF		0xb0
#define MON_CH			0xc4
#define MON_EN			0xc8
#define MON_INTERVAL		0xcc
#define TEMP_HIGH		0xd0
#define TEMP_MEDIAN		0xd4
#define TEMP_LOW		0xd8
#define INT_MASK		0x104
/* The vendor's stand-ins for an unset calibration word and a zero VREF sample. */
#define THM_VREF_DEFAULT	7
#define VREF_DEFAULT		0x1f1

struct ax620e_thermal {
	void __iomem *base;
	u32 vref[3];	/* the last VREF samples, averaged */
	bool ready;	/* the monitor runs */
};

/* The vendor's step2temp(): whole kelvin, then millidegrees Celsius. */
static int ax620e_thermal_get_temp(struct thermal_zone_device *tz, int *temp)
{
	struct ax620e_thermal *ts = thermal_zone_device_priv(tz);
	u32 step, vref;

	/* Callers hold the zone's lock, which serialises the vref[] update. */
	if (!READ_ONCE(ts->ready))
		return -EAGAIN;
	step = readl(ts->base + THM_DATA0) & 0x3ff;
	ts->vref[0] = ts->vref[1];
	ts->vref[1] = ts->vref[2];
	ts->vref[2] = readl(ts->base + MSR_VREF);
	vref = (ts->vref[0] + ts->vref[1] + ts->vref[2]) / 3 ?: VREF_DEFAULT;
	*temp = 1000 * (int)(90000 * step / (vref * 242)) - 277650;
	return 0;
}

static const struct thermal_zone_device_ops ax620e_thermal_ops = {
	.get_temp = ax620e_thermal_get_temp,
};

/* The vendor's temp2step(), from whole degrees Celsius. */
static u32 ax620e_thermal_step(int millicelsius, u32 vref)
{
	return (millicelsius / 1000 * 100 + 450 + 27315) / 100 * (vref * 242) / 90000;
}

struct ax620e_trips {
	unsigned int n;
	int temp[2];	/* the first two, in DT order */
	int max;
};

static int ax620e_thermal_trip(struct thermal_trip *trip, void *data)
{
	struct ax620e_trips *t = data;

	if (t->n < ARRAY_SIZE(t->temp))
		t->temp[t->n] = trip->temperature;
	t->max = max(t->max, trip->temperature);
	t->n++;
	return 0;
}

static int ax620e_thermal_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ax620e_trips t = { .max = INT_MIN };
	struct thermal_zone_device *tz;
	struct clk_bulk_data *clks;
	struct reset_control *rst;
	struct ax620e_thermal *ts;
	struct resource *res;
	void __iomem *b, *cal;
	u32 vref;
	int ret;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;
	b = ts->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(b))
		return PTR_ERR(b);
	/* Mapped, not claimed: the SPL's boot-info block has other readers. */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	cal = res ? devm_ioremap(dev, res->start, resource_size(res)) : NULL;
	if (!cal)
		return dev_err_probe(dev, -ENODEV, "calibration word\n");
	ret = devm_clk_bulk_get_all_enabled(dev, &clks);
	if (ret < 0)
		return dev_err_probe(dev, ret, "clocks\n");
	rst = devm_reset_control_array_get_exclusive(dev);
	if (IS_ERR(rst))
		return dev_err_probe(dev, PTR_ERR(rst), "resets\n");
	ret = reset_control_deassert(rst);
	if (ret)
		return ret;

	/* Registered first for its trips; reads are -EAGAIN until the monitor runs. */
	tz = devm_thermal_of_zone_register(dev, 0, ts, &ax620e_thermal_ops);
	if (IS_ERR(tz))
		return dev_err_probe(dev, PTR_ERR(tz), "thermal zone\n");
	thermal_zone_for_each_trip(tz, ax620e_thermal_trip, &t);
	if (t.n < 3)
		return dev_err_probe(dev, -EINVAL, "needs 3 trips, has %u\n", t.n);

	writel(0, b + THM_RSTN);
	writel(0, b + MON_EN);
	writel(readl(b + THM_CLK_EN) | BIT(0), b + THM_CLK_EN);
	writel(0, b + ADC_FILTER_SEL);
	writel(0xf, b + ADC_FILTER_EN);
	writel(0x100, b + MON_INTERVAL);
	writel(readl(b + MON_CH) | GENMASK(13, 10), b + MON_CH);
	writel(0, b + INT_MASK);
	writel(1, b + THM_MA_CTRL);
	writel(1, b);
	writel((readl(cal) ?: THM_VREF_DEFAULT) << 5 | 0x18, b + THM_CTRL);
	writel(readl(b + THM_CLK_EN) | BIT(1), b + THM_CLK_EN);
	writel(readl(b + MON_CH) | BIT(0), b + MON_CH);
	writel(GENMASK(18, 16), b + INT_MASK);
	writel(1, b + THM_RSTN);
	writel(BIT(14), b + MSR_ONE_EN);
	udelay(100);
	vref = readl(b + MSR_VREF) ?: VREF_DEFAULT;
	writel(ax620e_thermal_step(t.temp[0], vref), b + TEMP_LOW);
	writel(ax620e_thermal_step(t.temp[1], vref), b + TEMP_MEDIAN);
	writel(ax620e_thermal_step(t.max, vref), b + TEMP_HIGH);
	writel(readl(b + MON_CH) | BIT(14), b + MON_CH);
	writel(1, b + MON_EN);

	ts->vref[0] = ts->vref[1] = ts->vref[2] = vref;
	WRITE_ONCE(ts->ready, true);
	return 0;
}

static const struct of_device_id ax620e_thermal_of_match[] = {
	{ .compatible = "axera,ax620e-tsensor" },
	{ }
};

static struct platform_driver ax620e_thermal_driver = {
	.probe = ax620e_thermal_probe,
	.driver = {
		.name = "ax620e-thermal",
		.of_match_table = ax620e_thermal_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(ax620e_thermal_driver);
