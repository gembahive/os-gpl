// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025-2026 Sipeed Technology Co., Ltd.
 * Copyright (c) 2026, GembaHive, Inc.
 */
/*
 * Lontium LT7911EXC (USB-C DP Alt Mode to MIPI CSI-2) bridge manager: the
 * source's geometry and the bridge's EDID and firmware flash under
 * /proc/lt7911_info.
 *
 * The bridge's 8051 also runs USB-C CC/PD for both ports. Halting it
 * (E0:EE = 1, required for any flash access) restarts its firmware on
 * release, losing PD and Alt Mode state; holding it in reset drops Rd. The
 * reset line is therefore pulsed only once, after a firmware program.
 */

#include <linux/capability.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>

/* The MCU's info page: version, INT type, video info, halt and the flash controller. */
#define E0(reg)			(0xe000 | (reg))
#define REG_HALT		E0(0xee)
#define REG_FW_VERSION		E0(0x81)	/* 3 bytes */
#define REG_INT_TYPE		E0(0x84)
#define REG_VIDEO_INFO		E0(0x85)	/* pclk kHz 3, htotal 2, vtotal 2, width 2, height 2 */
#define REG_AUDIO_RATE		E0(0xad)
#define REG_HOST_MARKER		E0(0xa8)
#define REG_RXTX			E0(0xae)
#define REG_HW_CRC		0x0022		/* page 00, 4 bytes big-endian */

#define INT_VIDEO_OFF		0x00
#define INT_VIDEO_READY		0x01

#define FW_NAME			"Lontium/lt7911exc_fw.bin"
#define FW_SIZE			0x10000
#define FW_DATA_SIZE		(FW_SIZE - 4)	/* the CRC word sits at 0xfffc */
#define FLASH_PAGE		32
#define EDID_ADDR		0x60000
#define EDID_SIZE		256

struct lt7911 {
	struct device *dev;
	struct regmap *regmap;
	struct gpio_desc *reset;
	int irq;

	struct mutex io;	/* bridge access: IRQ thread, EDID, fw update */
	/* MCU left halted by a failed update; it must not be run until an update succeeds. */
	bool fw_halted;

	struct mutex state;	/* everything below; present also changes only under io */
	bool present;
	u16 width, height, fps;
	const char *fw_state;	/* idle, busy, done, error, error-halted */
	int fw_result;
	size_t fw_size;
	u32 fw_crc_expected, fw_crc_actual;
};

/* Flash access, including EDID reads, halts the bridge's CC/PD firmware; off by default. */
static bool allow_fw_update;
module_param(allow_fw_update, bool, 0644);
MODULE_PARM_DESC(allow_fw_update, "Allow firmware and EDID flash access (requires independent power)");

/* Geometry reported while a source is up, instead of the bridge's. */
static int force_width = -1;
static int force_height = -1;
static int force_fps = -1;
static struct lt7911 *active;	/* under kernel_param_lock */

/*
 * The registers use 256-byte pages behind selector FF. Leave the range
 * unnamed: reading all of page C7 would pop the PD RX FIFO.
 */
static const struct regmap_range_cfg lt7911_range = {
	.range_max = 0xffff,
	.selector_reg = 0xff,
	.selector_mask = 0xff,
	.window_len = 0x100,
};

static const struct regmap_config lt7911_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xffff,
	.ranges = &lt7911_range,
	.num_ranges = 1,
};

/* 64 KiB block erase at @addr. */
static int lt7911_erase(struct lt7911 *lt, u32 addr)
{
	const struct reg_sequence seq[] = {
		{ REG_HALT, 0x01 },
		{ E0(0x54), 0x01 }, { E0(0x55), 0x06 },
		{ E0(0x51), 0x01 }, { E0(0x51), 0x00 },
		{ E0(0x54), 0x05 }, { E0(0x55), 0xd8 },
		{ E0(0x5a), (addr >> 16) & 0xff },
		{ E0(0x5b), (addr >> 8) & 0xff },
		{ E0(0x5c), addr & 0xff },
		{ E0(0x51), 0x01 }, { E0(0x51), 0x00 },
	};
	int ret;

	ret = regmap_multi_reg_write(lt->regmap,seq, ARRAY_SIZE(seq));
	if (ret)
		return ret;
	msleep(200);
	return 0;
}

/* Program in 32-byte pages through the FIFO at E0:5D; a short last page is flushed. */
static int lt7911_program(struct lt7911 *lt, u32 addr, const u8 *data, size_t len)
{
	size_t off;
	int ret;

	for (off = 0; off < len; off += FLASH_PAGE, addr += FLASH_PAGE) {
		size_t n = min_t(size_t, len - off, FLASH_PAGE);
		const struct reg_sequence init[] = {
			{ REG_HALT, 0x01 },
			{ E0(0x5f), 0x01 },
			{ E0(0x5a), (addr >> 16) & 0xff },
			{ E0(0x5b), (addr >> 8) & 0xff },
			{ E0(0x5c), addr & 0xff },
		};

		ret = regmap_multi_reg_write(lt->regmap,init, ARRAY_SIZE(init));
		if (ret)
			return ret;
		ret = regmap_raw_write(lt->regmap, E0(0x5d), data + off, n);
		if (ret) {
			dev_err(lt->dev, "flash write error at page %zu\n", off / FLASH_PAGE);
			return ret;
		}
		if (n < FLASH_PAGE) {
			ret = regmap_write(lt->regmap,E0(0x5f), 0x05);
			if (!ret)
				ret = regmap_write(lt->regmap,E0(0x5f), 0x01);
			if (ret)
				return ret;
			usleep_range(1000, 2000);
		}
		ret = regmap_write(lt->regmap,E0(0x5f), 0x00);
		if (ret)
			return ret;
	}
	return 0;
}

static int lt7911_read_flash(struct lt7911 *lt, u32 addr, u8 *data, size_t len)
{
	size_t off;
	int ret;

	/* FIFO reset. */
	ret = regmap_write(lt->regmap,E0(0x5f), 0x08);
	if (!ret)
		ret = regmap_write(lt->regmap,E0(0x5f), 0x00);
	if (ret)
		return ret;
	msleep(1000);

	for (off = 0; off < len; off += FLASH_PAGE) {
		size_t n = min_t(size_t, len - off, FLASH_PAGE);
		u32 a = addr + off;
		const struct reg_sequence seq[] = {
			{ E0(0x54), 0x45 }, { E0(0x55), 0x03 },
			{ E0(0x56), n }, { E0(0x57), 0x00 }, { E0(0x58), 0x00 },
			{ E0(0x5a), (a >> 16) & 0xff },
			{ E0(0x5b), (a >> 8) & 0xff },
			{ E0(0x5c), a & 0xff },
			{ E0(0x51), 0x01 }, { E0(0x51), 0x00 },
		};

		ret = regmap_multi_reg_write(lt->regmap,seq, ARRAY_SIZE(seq));
		if (!ret)
			ret = regmap_bulk_read(lt->regmap, E0(0x5e), data + off, n);
		if (ret)
			return ret;
	}
	return 0;
}

/* The CRC word at 0xfffc, little-endian. */
static int lt7911_write_crc(struct lt7911 *lt, u32 crc)
{
	const struct reg_sequence seq[] = {
		{ E0(0x5f), 0x01 },
		{ E0(0x5a), (FW_DATA_SIZE >> 16) & 0xff },
		{ E0(0x5b), (FW_DATA_SIZE >> 8) & 0xff },
		{ E0(0x5c), FW_DATA_SIZE & 0xff },
	};
	u8 le[4] = { crc, crc >> 8, crc >> 16, crc >> 24 };
	int ret;

	ret = regmap_multi_reg_write(lt->regmap,seq, ARRAY_SIZE(seq));
	if (!ret)
		ret = regmap_raw_write(lt->regmap, E0(0x5d), le, sizeof(le));
	if (!ret)
		ret = regmap_write(lt->regmap,E0(0x5f), 0x05);
	if (!ret)
		ret = regmap_write(lt->regmap,E0(0x5f), 0x01);
	if (ret)
		return ret;
	usleep_range(1000, 2000);
	return regmap_write(lt->regmap,E0(0x5f), 0x00);
}

/* The hardware CRC of flash 0x0000-0xfffb. Halts the MCU; the caller runs it. */
static int lt7911_read_hw_crc(struct lt7911 *lt, u32 *crc)
{
	static const struct reg_sequence seq[] = {
		{ REG_HALT, 0x01 }, { E0(0x7b), 0x60 }, { E0(0x7b), 0x40 },
	};
	u8 be[4];
	int ret;

	ret = regmap_multi_reg_write(lt->regmap,seq, ARRAY_SIZE(seq));
	if (ret)
		return ret;
	msleep(150);
	ret = regmap_bulk_read(lt->regmap, REG_HW_CRC, be, sizeof(be));
	if (!ret)
		*crc = get_unaligned_be32(be);
	return ret;
}

/*
 * The updater's CRC: CRC-32/MPEG-2 over the file padded with 0xff to 0xfffc
 * bytes, each 32-bit word fed last byte first.
 */
static u32 lt7911_fw_crc(const struct firmware *fw)
{
	u32 crc = 0xffffffff;
	size_t i;
	int j, b;

	for (i = 0; i < FW_DATA_SIZE; i += 4) {
		for (j = 3; j >= 0; j--) {
			crc ^= (u32)(i + j < fw->size ? fw->data[i + j] : 0xff) << 24;
			for (b = 0; b < 8; b++)
				crc = crc & 0x80000000 ? (crc << 1) ^ 0x04c11db7 : crc << 1;
		}
	}
	return crc;
}

/* Reset pulse: 20 ms in reset, then 400 ms for the firmware to boot. */
static void lt7911_reset(struct lt7911 *lt)
{
	gpiod_set_value_cansleep(lt->reset, 1);
	msleep(20);
	gpiod_set_value_cansleep(lt->reset, 0);
	msleep(400);
}

/*
 * Erase, program and check the firmware block. No reset before the halt: the
 * halt keeps the CC state latched, a reset drops it. A failure before the
 * post-program reset leaves the MCU halted, since running it would boot a
 * blank or partial image.
 */
static int lt7911_flash_fw(struct lt7911 *lt, const struct firmware *fw, u32 expected,
			   u32 *actual)
{
	ktime_t t0 = ktime_get();
	u32 pre_crc = 0;
	u8 ver[3];
	int ret, run;

	dev_info(lt->dev, "fw_update: halt MCU; %zu bytes, crc 0x%08x\n", fw->size, expected);
	ret = regmap_write(lt->regmap,REG_HALT, 0x01);
	if (ret)
		goto halted;
	ret = lt7911_erase(lt, 0);
	if (ret)
		goto halted;
	dev_info(lt->dev, "fw_update: erased (+%lld ms)\n", ktime_ms_delta(ktime_get(), t0));
	ret = lt7911_program(lt, 0, fw->data, fw->size);
	if (ret)
		goto halted;
	dev_info(lt->dev, "fw_update: programmed %zu pages (+%lld ms)\n",
		 DIV_ROUND_UP(fw->size, FLASH_PAGE), ktime_ms_delta(ktime_get(), t0));
	ret = lt7911_write_crc(lt, expected);
	if (ret)
		goto halted;

	/* Informational only; the check that decides is after the reset. */
	if (!lt7911_read_hw_crc(lt, &pre_crc))
		dev_info(lt->dev, "fw_update: pre-reset hw crc 0x%08x (%s) (+%lld ms)\n",
			 pre_crc, pre_crc == expected ? "match" : "differs",
			 ktime_ms_delta(ktime_get(), t0));

	dev_info(lt->dev, "fw_update: reset bridge\n");
	lt7911_reset(lt);

	ret = lt7911_read_hw_crc(lt, actual);
	if (!ret && *actual != expected)
		ret = -EIO;
	dev_info(lt->dev, "fw_update: hw crc 0x%08x, expected 0x%08x: %s (+%lld ms)\n",
		 *actual, expected, ret ? "FAIL" : "ok", ktime_ms_delta(ktime_get(), t0));

	run = regmap_write(lt->regmap,REG_HALT, 0x00);
	if (!ret && !run) {
		/* Let the new image publish its version. */
		msleep(200);
		if (!regmap_bulk_read(lt->regmap, REG_FW_VERSION, ver, sizeof(ver)))
			dev_info(lt->dev, "fw_update: done, fw version %u.%u.%u\n",
				 ver[0], ver[1], ver[2]);
	}
	return ret ?: run;

halted:
	lt->fw_halted = true;
	dev_err(lt->dev, "fw_update: failed before reset (%d) at +%lld ms; MCU left halted, write start to retry\n",
		ret, ktime_ms_delta(ktime_get(), t0));
	return ret;
}

static void lt7911_set_fw_state(struct lt7911 *lt, const char *state, int result, size_t size,
				u32 expected, u32 actual)
{
	mutex_lock(&lt->state);
	lt->fw_state = state;
	lt->fw_result = result;
	lt->fw_size = size;
	lt->fw_crc_expected = expected;
	lt->fw_crc_actual = actual;
	mutex_unlock(&lt->state);
}

static int lt7911_fw_update(struct lt7911 *lt)
{
	const struct firmware *fw;
	u32 expected = 0, actual = 0;
	size_t size = 0;
	bool halted = false;
	int ret;

	ret = request_firmware(&fw, FW_NAME, lt->dev);
	if (ret) {
		dev_err(lt->dev, "failed to request firmware %s: %d\n", FW_NAME, ret);
		goto out;
	}
	size = fw->size;
	if (!size || size > FW_DATA_SIZE) {
		dev_err(lt->dev, "invalid firmware size: %zu\n", size);
		ret = -EINVAL;
		goto release;
	}
	expected = lt7911_fw_crc(fw);
	lt7911_set_fw_state(lt, "busy", 0, size, expected, 0);

	disable_irq(lt->irq);
	mutex_lock(&lt->io);
	lt->fw_halted = false;
	ret = lt7911_flash_fw(lt, fw, expected, &actual);
	halted = lt->fw_halted;
	mutex_unlock(&lt->io);
	enable_irq(lt->irq);
release:
	release_firmware(fw);
out:
	lt7911_set_fw_state(lt, !ret ? "done" : halted ? "error-halted" : "error",
			    ret, size, expected, actual);
	return ret;
}

/* Under io. */
static int lt7911_read_video(struct lt7911 *lt, u16 *width, u16 *height, u16 *fps)
{
	u8 b[11];
	u32 pclk, htotal, vtotal;
	int ret;

	ret = regmap_bulk_read(lt->regmap, REG_VIDEO_INFO, b, sizeof(b));
	if (ret)
		return ret;
	pclk = b[0] << 16 | b[1] << 8 | b[2];
	htotal = b[3] << 8 | b[4];
	vtotal = b[5] << 8 | b[6];
	*width = b[7] << 8 | b[8];
	*height = b[9] << 8 | b[10];
	*fps = pclk && htotal && vtotal ? div_u64((u64)pclk * 1000, htotal * vtotal) : 0;
	dev_info(lt->dev, "LT7911EXC HTotal: %u, VTotal: %u\n", htotal, vtotal);
	return 0;
}

static void lt7911_set_video(struct lt7911 *lt, bool present, u16 width, u16 height, u16 fps)
{
	mutex_lock(&lt->state);
	lt->present = present;
	lt->width = width;
	lt->height = height;
	lt->fps = fps;
	mutex_unlock(&lt->state);
	dev_info(lt->dev, "LT7911EXC video %s: %u x %u, fps %u\n",
		 present ? "ready" : "off", width, height, fps);
}

static bool lt7911_forced(void)
{
	return force_width > 0 && force_height > 0;
}

static void lt7911_set_forced(struct lt7911 *lt)
{
	lt7911_set_video(lt, true, force_width, force_height, force_fps > 0 ? force_fps : 30);
}

static irqreturn_t lt7911_irq(int irq, void *data)
{
	struct lt7911 *lt = data;
	unsigned int type;
	u16 w, h, fps;

	mutex_lock(&lt->io);
	if (regmap_read(lt->regmap, REG_INT_TYPE, &type)) {
		dev_err(lt->dev, "Failed to get LT7911EXC INT type\n");
		goto out;
	}
	dev_info(lt->dev, "LT7911EXC INT type: 0x%02x\n", type);
	/* 2 and 3 are audio off/ready: nothing here reports audio. */
	if (type == INT_VIDEO_OFF)
		lt7911_set_video(lt, false, 0, 0, 0);
	else if (type == INT_VIDEO_READY && lt7911_forced())
		lt7911_set_forced(lt);
	else if (type == INT_VIDEO_READY && !lt7911_read_video(lt, &w, &h, &fps))
		lt7911_set_video(lt, true, w, h, fps);
out:
	mutex_unlock(&lt->io);
	return IRQ_HANDLED;
}

/*
 * A source already up at probe raised its IRQ before the handler was
 * installed. It counts only with geometry, rate and audio all non-zero.
 */
static void lt7911_refresh(struct lt7911 *lt)
{
	unsigned int rate;
	u16 w, h, fps;

	mutex_lock(&lt->io);
	if (!lt7911_read_video(lt, &w, &h, &fps) &&
	    !regmap_read(lt->regmap, REG_AUDIO_RATE, &rate) && w && h && fps && rate) {
		if (lt7911_forced())
			lt7911_set_forced(lt);
		else
			lt7911_set_video(lt, true, w, h, fps);
	}
	mutex_unlock(&lt->io);
}

static int force_param_set(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_int(val, kp);

	/*
	 * Claims a source only when one is present. No reset when cleared: the
	 * next video IRQ reports the source.
	 */
	if (!ret && active && lt7911_forced()) {
		mutex_lock(&active->io);
		if (active->present)
			lt7911_set_forced(active);
		mutex_unlock(&active->io);
	}
	return ret;
}

static const struct kernel_param_ops force_param_ops = {
	.set = force_param_set,
	.get = param_get_int,
};

module_param_cb(force_width, &force_param_ops, &force_width, 0644);
MODULE_PARM_DESC(force_width, "Force HDMI width");
module_param_cb(force_height, &force_param_ops, &force_height, 0644);
MODULE_PARM_DESC(force_height, "Force HDMI height");
module_param_cb(force_fps, &force_param_ops, &force_fps, 0644);
MODULE_PARM_DESC(force_fps, "Force HDMI fps");

/* "disappear" means no source. */
static int status_show(struct seq_file *m, void *v)
{
	struct lt7911 *lt = m->private;

	mutex_lock(&lt->state);
	seq_puts(m, lt->present ? "new res\n" : "disappear\n");
	mutex_unlock(&lt->state);
	return 0;
}

static int width_show(struct seq_file *m, void *v)
{
	struct lt7911 *lt = m->private;

	mutex_lock(&lt->state);
	seq_printf(m, "%u\n", lt->width);
	mutex_unlock(&lt->state);
	return 0;
}

static int height_show(struct seq_file *m, void *v)
{
	struct lt7911 *lt = m->private;

	mutex_lock(&lt->state);
	seq_printf(m, "%u\n", lt->height);
	mutex_unlock(&lt->state);
	return 0;
}

static int fps_show(struct seq_file *m, void *v)
{
	struct lt7911 *lt = m->private;

	mutex_lock(&lt->state);
	seq_printf(m, "%u\n", lt->fps);
	mutex_unlock(&lt->state);
	return 0;
}

static bool lt7911_edid_valid(const u8 *edid, size_t len)
{
	size_t i;
	u8 sum = 0;

	if (len != 128 && len != EDID_SIZE)
		return false;
	for (i = 0; i < len; i++) {
		sum += edid[i];
		if (i % 128 == 127 && sum)
			return false;
	}
	return true;
}

/* Halts the MCU; video stays off afterwards until the link is replugged. */
static ssize_t edid_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct lt7911 *lt = pde_data(file_inode(file));
	u8 edid[EDID_SIZE];
	int ret, run;

	if (!capable(CAP_SYS_ADMIN) || !READ_ONCE(allow_fw_update))
		return -EPERM;
	if (*ppos >= EDID_SIZE)
		return 0;

	disable_irq(lt->irq);
	mutex_lock(&lt->io);
	if (lt->fw_halted) {
		ret = -EBUSY;
	} else {
		ret = regmap_write(lt->regmap,REG_HALT, 0x01);
		if (!ret) {
			ret = lt7911_read_flash(lt, EDID_ADDR, edid, EDID_SIZE);
			run = regmap_write(lt->regmap,REG_HALT, 0x00);
			ret = ret ?: run;
		}
	}
	mutex_unlock(&lt->io);
	enable_irq(lt->irq);
	if (ret)
		return ret;
	return simple_read_from_buffer(buf, count, ppos, edid, EDID_SIZE);
}

/*
 * Rewrites the EDID block (the firmware below is untouched), halted, with no
 * reset: the firmware loads it at the bridge's next power-up.
 */
static ssize_t edid_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	struct lt7911 *lt = pde_data(file_inode(file));
	u8 edid[EDID_SIZE], back[EDID_SIZE];
	int ret, run;

	if (!capable(CAP_SYS_ADMIN) || !READ_ONCE(allow_fw_update))
		return -EPERM;
	if (count != 128 && count != EDID_SIZE)
		return -EINVAL;
	if (copy_from_user(edid, buf, count))
		return -EFAULT;
	if (!lt7911_edid_valid(edid, count))
		return -EINVAL;

	disable_irq(lt->irq);
	mutex_lock(&lt->io);
	if (lt->fw_halted) {
		ret = -EBUSY;
		goto out;
	}
	ret = regmap_write(lt->regmap,REG_HALT, 0x01);
	if (ret)
		goto out;
	ret = lt7911_erase(lt, EDID_ADDR);
	if (!ret)
		ret = lt7911_program(lt, EDID_ADDR, edid, count);
	if (!ret)
		ret = lt7911_read_flash(lt, EDID_ADDR, back, count);
	if (!ret && memcmp(back, edid, count))
		ret = -EIO;
	run = regmap_write(lt->regmap,REG_HALT, 0x00);
	ret = ret ?: run;
out:
	mutex_unlock(&lt->io);
	enable_irq(lt->irq);
	return ret ?: count;
}

static const struct proc_ops edid_ops = {
	.proc_read = edid_read,
	.proc_write = edid_write,
	.proc_lseek = default_llseek,
};

/* Reports "state=done result=0 ..." and the two CRCs. */
static ssize_t fw_update_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct lt7911 *lt = pde_data(file_inode(file));
	char s[160];
	int len;

	mutex_lock(&lt->state);
	len = scnprintf(s, sizeof(s),
			"state=%s result=%d size=%zu crc_expected=0x%08x crc_actual=0x%08x firmware=%s\n",
			lt->fw_state, lt->fw_result, lt->fw_size, lt->fw_crc_expected,
			lt->fw_crc_actual, FW_NAME);
	mutex_unlock(&lt->state);
	return simple_read_from_buffer(buf, count, ppos, s, len);
}

/* "start": flash request_firmware(FW_NAME); returns once done (4-8 s). */
static ssize_t fw_update_write(struct file *file, const char __user *buf, size_t count,
			       loff_t *ppos)
{
	struct lt7911 *lt = pde_data(file_inode(file));
	char cmd[16];
	int ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (!count || count >= sizeof(cmd))
		return -EINVAL;
	if (copy_from_user(cmd, buf, count))
		return -EFAULT;
	cmd[count] = '\0';
	if (strcmp(strim(cmd), "start"))
		return -EINVAL;
	if (!READ_ONCE(allow_fw_update)) {
		dev_warn(lt->dev, "fw_update refused: allow_fw_update=0 (needs bridge-independent power)\n");
		return -EPERM;
	}

	mutex_lock(&lt->state);
	if (!strcmp(lt->fw_state, "busy")) {
		mutex_unlock(&lt->state);
		return -EBUSY;
	}
	lt->fw_state = "busy";
	lt->fw_result = 0;
	lt->fw_size = 0;
	lt->fw_crc_expected = 0;
	lt->fw_crc_actual = 0;
	mutex_unlock(&lt->state);

	ret = lt7911_fw_update(lt);
	return ret ?: count;
}

static const struct proc_ops fw_update_ops = {
	.proc_read = fw_update_read,
	.proc_write = fw_update_write,
	.proc_lseek = default_llseek,
};

static void lt7911_proc_remove(void *dir)
{
	proc_remove(dir);
}

static int lt7911_passive_read(struct lt7911 *lt, unsigned int reg, u8 *buf, size_t len)
{
	int ret;

	mutex_lock(&lt->io);
	ret = lt->fw_halted ? -EBUSY : regmap_bulk_read(lt->regmap, reg, buf, len);
	mutex_unlock(&lt->io);
	return ret;
}

static int host_marker_show(struct seq_file *m, void *unused)
{
	u8 value;
	int ret = lt7911_passive_read(m->private, REG_HOST_MARKER, &value, 1);

	if (!ret)
		seq_printf(m, "%02x\n", value);
	return ret;
}

static int fw_version_show(struct seq_file *m, void *unused)
{
	u8 version[3];
	int ret = lt7911_passive_read(m->private, REG_FW_VERSION, version, sizeof(version));

	if (!ret)
		seq_printf(m, "%u.%u.%u\n", version[0], version[1], version[2]);
	return ret;
}

static int rxtx_show(struct seq_file *m, void *unused)
{
	u8 state[2];
	int ret = lt7911_passive_read(m->private, REG_RXTX, state, sizeof(state));

	if (!ret)
		seq_printf(m, "%02x %02x\n", state[0], state[1]);
	return ret;
}

static int lt7911_proc_init(struct lt7911 *lt)
{
	struct proc_dir_entry *dir;
	int ret;

	dir = proc_mkdir("lt7911_info", NULL);
	if (!dir)
		return -ENOMEM;
	ret = devm_add_action_or_reset(lt->dev, lt7911_proc_remove, dir);
	if (ret)
		return ret;
	if (!proc_create_single_data("status", 0444, dir, status_show, lt) ||
	    !proc_create_single_data("width", 0444, dir, width_show, lt) ||
	    !proc_create_single_data("height", 0444, dir, height_show, lt) ||
	    !proc_create_single_data("fps", 0444, dir, fps_show, lt) ||
	    !proc_create_single_data("host_marker", 0444, dir, host_marker_show, lt) ||
	    !proc_create_single_data("fw_version", 0444, dir, fw_version_show, lt) ||
	    !proc_create_single_data("rxtx", 0444, dir, rxtx_show, lt) ||
	    !proc_create_data("edid", 0600, dir, &edid_ops, lt) ||
	    !proc_create_data("fw_update", 0600, dir, &fw_update_ops, lt))
		return -ENOMEM;
	return 0;
}

static int lt7911_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct gpio_desc *irq_gpio;
	struct lt7911 *lt;
	u8 ver[3];
	int ret;

	lt = devm_kzalloc(dev, sizeof(*lt), GFP_KERNEL);
	if (!lt)
		return -ENOMEM;
	lt->dev = dev;
	lt->fw_state = "idle";
	ret = devm_mutex_init(dev, &lt->io);
	if (!ret)
		ret = devm_mutex_init(dev, &lt->state);
	if (ret)
		return ret;

	lt->regmap = devm_regmap_init_i2c(client, &lt7911_regmap);
	if (IS_ERR(lt->regmap))
		return PTR_ERR(lt->regmap);

	irq_gpio = devm_gpiod_get(dev, "irq", GPIOD_IN);
	if (IS_ERR(irq_gpio))
		return dev_err_probe(dev, PTR_ERR(irq_gpio), "irq GPIO\n");
	/* Deasserted, i.e. running, as the bridge is since power-up: no pulse here. */
	lt->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(lt->reset))
		return dev_err_probe(dev, PTR_ERR(lt->reset), "reset GPIO\n");
	lt->irq = gpiod_to_irq(irq_gpio);
	if (lt->irq < 0)
		return dev_err_probe(dev, lt->irq, "irq GPIO to IRQ\n");

	/* Chip check; the bridge is already running. */
	ret = regmap_bulk_read(lt->regmap, REG_FW_VERSION, ver, sizeof(ver));
	if (ret)
		return dev_err_probe(dev, ret, "reading the fw version\n");
	dev_info(dev, "Chip: LT7911EXC fw version %u.%u.%u\n", ver[0], ver[1], ver[2]);

	msleep(50);
	ret = devm_request_threaded_irq(dev, lt->irq, NULL, lt7911_irq,
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT, "lt7911_manage", lt);
	if (ret)
		return ret;

	lt7911_refresh(lt);
	/* Publish last; devres removes callbacks before releasing their IRQ. */
	ret = lt7911_proc_init(lt);
	if (ret)
		return ret;

	kernel_param_lock(THIS_MODULE);
	active = lt;
	kernel_param_unlock(THIS_MODULE);
	return 0;
}

static const struct of_device_id lt7911_of_match[] = {
	{ .compatible = "lontium,lt7911exc" },
	{ }
};
MODULE_DEVICE_TABLE(of, lt7911_of_match);

static struct i2c_driver lt7911_driver = {
	.driver = {
		.name = "lt7911_manage",
		.of_match_table = lt7911_of_match,
		/* active and the proc files assume one bridge that stays bound. */
		.suppress_bind_attrs = true,
	},
	.probe = lt7911_probe,
};
module_i2c_driver(lt7911_driver);

MODULE_DESCRIPTION("Lontium LT7911EXC bridge manager");
MODULE_AUTHOR("Z2Z-BuGu");
MODULE_AUTHOR("916BGAI");
MODULE_LICENSE("GPL");
