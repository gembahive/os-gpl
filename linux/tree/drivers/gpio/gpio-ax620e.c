// SPDX-License-Identifier: GPL-2.0
/*
 * Axera AX620E APB GPIO controller. DesignWare register names only: one word
 * per line holds that line's direction, data, interrupt and debounce bits, so
 * gpio-dwapb cannot bind.
 *
 * Copyright (c) 2026 the open-nanokvm-pro contributors.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/spinlock.h>

#define AX620E_GPIO_LINES		32
#define AX620E_GPIO_LINE(n)		(((n) + 1) * 4)

#define AX620E_GPIO_DR			BIT(0)	/* output data */
#define AX620E_GPIO_DDR			BIT(1)	/* 1 = output */
#define AX620E_GPIO_INTEN		BIT(3)
#define AX620E_GPIO_INTMASK		BIT(4)	/* 1 = masked */
#define AX620E_GPIO_INTTYPE_EDGE	BIT(5)	/* 1 = edge, 0 = level */
#define AX620E_GPIO_INT_POLARITY	BIT(6)	/* 1 = high/rising */
#define AX620E_GPIO_DEBOUNCE		BIT(7)
#define AX620E_GPIO_EOI			BIT(8)	/* a pulse, not W1C */
#define AX620E_GPIO_INT_BOTHEDGE	BIT(9)

#define AX620E_GPIO_NSECURE_MODE	0x00
#define AX620E_GPIO_EXT_PORT		0x8c
#define AX620E_GPIO_INTSTATUS		0xa4	/* non-secure view */

struct ax620e_gpio {
	struct gpio_chip gc;
	void __iomem *base;
	/* Line words have no set/clear aliases; irq_ack runs in hardirq. */
	raw_spinlock_t lock;
};

static void ax620e_gpio_update(struct ax620e_gpio *gpio, unsigned int offset,
			       u32 mask, u32 val)
{
	void __iomem *reg = gpio->base + AX620E_GPIO_LINE(offset);
	unsigned long flags;

	raw_spin_lock_irqsave(&gpio->lock, flags);
	writel((readl(reg) & ~mask) | (val & mask), reg);
	raw_spin_unlock_irqrestore(&gpio->lock, flags);
}

static int ax620e_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct ax620e_gpio *gpio = gpiochip_get_data(gc);

	return (readl(gpio->base + AX620E_GPIO_LINE(offset)) & AX620E_GPIO_DDR) ?
		GPIO_LINE_DIRECTION_OUT : GPIO_LINE_DIRECTION_IN;
}

static int ax620e_gpio_direction_input(struct gpio_chip *gc,
				       unsigned int offset)
{
	ax620e_gpio_update(gpiochip_get_data(gc), offset, AX620E_GPIO_DDR, 0);
	return 0;
}

/* Data and direction in one store, so the old latch value never drives. */
static int ax620e_gpio_direction_output(struct gpio_chip *gc,
					unsigned int offset, int value)
{
	ax620e_gpio_update(gpiochip_get_data(gc), offset,
			   AX620E_GPIO_DR | AX620E_GPIO_DDR,
			   (value ? AX620E_GPIO_DR : 0) | AX620E_GPIO_DDR);
	return 0;
}

/* The pad, never the output latch: EXT_PORT follows a driven output too. */
static int ax620e_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct ax620e_gpio *gpio = gpiochip_get_data(gc);

	return !!(readl(gpio->base + AX620E_GPIO_EXT_PORT) & BIT(offset));
}

static int ax620e_gpio_set(struct gpio_chip *gc, unsigned int offset, int value)
{
	ax620e_gpio_update(gpiochip_get_data(gc), offset, AX620E_GPIO_DR,
			   value ? AX620E_GPIO_DR : 0);
	return 0;
}

/* The debounce period is the block clock's, not programmable: on or off. */
static int ax620e_gpio_set_config(struct gpio_chip *gc, unsigned int offset,
				  unsigned long config)
{
	if (pinconf_to_config_param(config) != PIN_CONFIG_INPUT_DEBOUNCE)
		return -ENOTSUPP;

	ax620e_gpio_update(gpiochip_get_data(gc), offset, AX620E_GPIO_DEBOUNCE,
			   pinconf_to_config_argument(config) ?
			   AX620E_GPIO_DEBOUNCE : 0);
	return 0;
}

static void ax620e_gpio_irq_ack(struct irq_data *d)
{
	struct ax620e_gpio *gpio = gpiochip_get_data(irq_data_get_irq_chip_data(d));
	irq_hw_number_t hwirq = irqd_to_hwirq(d);

	/* EOI must be pulsed: left set it would swallow the next interrupt. */
	ax620e_gpio_update(gpio, hwirq, AX620E_GPIO_EOI, AX620E_GPIO_EOI);
	ax620e_gpio_update(gpio, hwirq, AX620E_GPIO_EOI, 0);
}

static void ax620e_gpio_irq_mask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);

	ax620e_gpio_update(gpiochip_get_data(gc), irqd_to_hwirq(d),
			   AX620E_GPIO_INTEN | AX620E_GPIO_INTMASK,
			   AX620E_GPIO_INTMASK);
	gpiochip_disable_irq(gc, irqd_to_hwirq(d));
}

static void ax620e_gpio_irq_unmask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);

	gpiochip_enable_irq(gc, irqd_to_hwirq(d));
	ax620e_gpio_update(gpiochip_get_data(gc), irqd_to_hwirq(d),
			   AX620E_GPIO_INTEN | AX620E_GPIO_INTMASK,
			   AX620E_GPIO_INTEN);
}

static int ax620e_gpio_irq_set_type(struct irq_data *d, unsigned int type)
{
	u32 val;

	switch (type) {
	case IRQ_TYPE_EDGE_BOTH:
		val = AX620E_GPIO_INTTYPE_EDGE | AX620E_GPIO_INT_BOTHEDGE;
		break;
	case IRQ_TYPE_EDGE_RISING:
		val = AX620E_GPIO_INTTYPE_EDGE | AX620E_GPIO_INT_POLARITY;
		break;
	case IRQ_TYPE_EDGE_FALLING:
		val = AX620E_GPIO_INTTYPE_EDGE;
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		val = AX620E_GPIO_INT_POLARITY;
		break;
	case IRQ_TYPE_LEVEL_LOW:
		val = 0;
		break;
	default:
		return -EINVAL;
	}

	ax620e_gpio_update(gpiochip_get_data(irq_data_get_irq_chip_data(d)),
			   irqd_to_hwirq(d),
			   AX620E_GPIO_INTTYPE_EDGE | AX620E_GPIO_INT_POLARITY |
			   AX620E_GPIO_INT_BOTHEDGE, val);

	if (type & IRQ_TYPE_LEVEL_MASK)
		irq_set_handler_locked(d, handle_level_irq);
	else
		irq_set_handler_locked(d, handle_edge_irq);

	return 0;
}

static const struct irq_chip ax620e_gpio_irq_chip = {
	.name = "ax620e-gpio",
	.irq_ack = ax620e_gpio_irq_ack,
	.irq_mask = ax620e_gpio_irq_mask,
	.irq_unmask = ax620e_gpio_irq_unmask,
	.irq_set_type = ax620e_gpio_irq_set_type,
	.flags = IRQCHIP_IMMUTABLE,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static void ax620e_gpio_irq_handler(struct irq_desc *desc)
{
	struct gpio_chip *gc = irq_desc_get_handler_data(desc);
	struct ax620e_gpio *gpio = gpiochip_get_data(gc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	unsigned long status;
	unsigned int hwirq;

	chained_irq_enter(chip, desc);
	status = readl(gpio->base + AX620E_GPIO_INTSTATUS);
	for_each_set_bit(hwirq, &status, AX620E_GPIO_LINES)
		generic_handle_domain_irq(gc->irq.domain, hwirq);
	chained_irq_exit(chip, desc);
}

static int ax620e_gpio_probe(struct platform_device *pdev)
{
	static const char * const resets[] = { "apb", "gpio" };
	struct device *dev = &pdev->dev;
	struct ax620e_gpio *gpio;
	struct clk_bulk_data *clks;
	struct reset_control *rst;
	unsigned int i;
	int irq, ret;

	gpio = devm_kzalloc(dev, sizeof(*gpio), GFP_KERNEL);
	if (!gpio)
		return -ENOMEM;

	raw_spin_lock_init(&gpio->lock);

	gpio->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(gpio->base))
		return PTR_ERR(gpio->base);

	ret = devm_clk_bulk_get_all_enabled(dev, &clks);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to enable clocks\n");

	/* Deassert only: lines already drive things, a reset pulse would glitch them. */
	for (i = 0; i < ARRAY_SIZE(resets); i++) {
		rst = devm_reset_control_get_exclusive_deasserted(dev, resets[i]);
		if (IS_ERR(rst))
			return dev_err_probe(dev, PTR_ERR(rst), "reset %s\n",
					     resets[i]);
	}

	/* Route interrupts to the non-secure status view the handler reads. */
	writel(0, gpio->base + AX620E_GPIO_NSECURE_MODE);

	gpio->gc.label = dev_name(dev);
	gpio->gc.parent = dev;
	gpio->gc.owner = THIS_MODULE;
	gpio->gc.base = -1;
	gpio->gc.ngpio = AX620E_GPIO_LINES;
	gpio->gc.get_direction = ax620e_gpio_get_direction;
	gpio->gc.direction_input = ax620e_gpio_direction_input;
	gpio->gc.direction_output = ax620e_gpio_direction_output;
	gpio->gc.get = ax620e_gpio_get;
	gpio->gc.set = ax620e_gpio_set;
	gpio->gc.set_config = ax620e_gpio_set_config;
	/* Through pinctrl, which muxes the pad to GPIO and then holds it. */
	gpio->gc.request = gpiochip_generic_request;
	gpio->gc.free = gpiochip_generic_free;

	irq = platform_get_irq_optional(pdev, 0);
	if (irq > 0) {
		struct gpio_irq_chip *girq = &gpio->gc.irq;

		gpio_irq_chip_set_chip(girq, &ax620e_gpio_irq_chip);
		girq->parent_handler = ax620e_gpio_irq_handler;
		girq->num_parents = 1;
		girq->parents = devm_kcalloc(dev, 1, sizeof(*girq->parents),
					     GFP_KERNEL);
		if (!girq->parents)
			return -ENOMEM;
		girq->parents[0] = irq;
		girq->default_type = IRQ_TYPE_NONE;
		girq->handler = handle_bad_irq;
	} else if (irq != -ENXIO) {
		return irq;
	}

	return devm_gpiochip_add_data(dev, &gpio->gc, gpio);
}

static const struct of_device_id ax620e_gpio_of_match[] = {
	{ .compatible = "axera,ax620e-gpio" },
	{ }
};
MODULE_DEVICE_TABLE(of, ax620e_gpio_of_match);

static struct platform_driver ax620e_gpio_driver = {
	.probe = ax620e_gpio_probe,
	.driver = {
		.name = "ax620e-gpio",
		.of_match_table = ax620e_gpio_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(ax620e_gpio_driver);
