// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026, GembaHive, Inc.
 */
/* Early boot log in the first page of the ramoops console. */
#include <linux/console.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/serial_core.h>
#include <linux/spinlock.h>

#include <asm/barrier.h>

#define RAM_CONSOLE_PHYS	0x45f50000
#define RAM_SIGNATURE	0x43474244
#define RAM_DATA		12
#define RAM_CAPACITY	(4096 - RAM_DATA)

static DEFINE_RAW_SPINLOCK(ram_lock);
static void __iomem *ram_base;
static struct console *ram_console;
static u32 ram_start, ram_size;

static void ram_write(struct console *con, const char *text, unsigned int count)
{
	unsigned long flags;

	/* Panic may bypass console_lock; never spin behind an interrupted writer. */
	if (!raw_spin_trylock_irqsave(&ram_lock, flags))
		return;
	while (count--) {
		writeb(*text++, ram_base + RAM_DATA + ram_start);
		if (++ram_start == RAM_CAPACITY)
			ram_start = 0;
		if (ram_size < RAM_CAPACITY)
			ram_size++;
	}
	/* Publish data first, then size before start so start never exceeds size. */
	dsb(sy);
	writel(ram_size, ram_base + 8);
	writel(ram_start, ram_base + 4);
	dsb(sy);
	raw_spin_unlock_irqrestore(&ram_lock, flags);
}

static int __init ram_setup(struct earlycon_device *dev, const char *options)
{
	if (dev->port.mapbase != RAM_CONSOLE_PHYS || !dev->port.membase)
		return -EINVAL;
	/* A record already here is the last boot's console: ramoops keeps it, so no early log. */
	if (readl(dev->port.membase) == RAM_SIGNATURE)
		return -EBUSY;
	ram_base = dev->port.membase;
	ram_console = dev->con;
	writel(0, ram_base);
	dsb(sy);
	writel(0, ram_base + 4);
	writel(0, ram_base + 8);
	dsb(sy);
	writel(RAM_SIGNATURE, ram_base);
	dsb(sy);
	dev->con->write = ram_write;
	return 0;
}
EARLYCON_DECLARE(early_ram, ram_setup);

static int __init ram_retire(void)
{
	static const char message[] = "early_ram: handoff to ramoops\n";

	if (ram_console) {
		ram_write(ram_console, message, sizeof(message) - 1);
		unregister_console(ram_console);
		/* No accesses through this Device alias after normal ramoops maps it. */
		ram_base = NULL;
	}
	return 0;
}
/* After SMP, before ramoops' postcore initcall reads and clears the old ring. */
core_initcall_sync(ram_retire);
