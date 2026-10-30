// SPDX-License-Identifier: GPL-2.0
/*
 * chipkey: keys bound to the AX620E's EIP-130 security engine.
 *
 * /dev/chipkey: one write of a LABEL_LEN-byte label derives, then reads return
 * HMAC-SHA256(K, CHIPKEY_MSG), K = KDF(KDK, label), KDK = KDF(HUK, KDK_LABEL),
 * both SP800-108 counter mode. The hardware unique key (HUK, static asset 0)
 * never leaves the engine; KDK and K exist only for the duration of a write.
 *
 * sysfs, /sys/class/misc/chipkey/:
 *   huk        "provisioned", "empty" or "error..."
 *   sysinfo    System Info result words W0..W5 in hex
 *   fuses      eFuse blk12, blk13, blk15..22, blk31 in hex, as OP-TEE's
 *              chipkey.pta reads them (CMD_FUSES); unreadable without OP-TEE
 *   provision  any write creates the HUK (burns OTP, once); refused unless
 *              huk is "empty"
 *
 * hwrng "chipkey": the engine's random numbers; unregistered from provisioning
 * the HUK until reboot.
 *
 * The engine filters assets by the AXI secure bit (sysinfo W3[19]); derived
 * outputs depend on it.
 *
 * Mailbox 2, polled; one mutex serializes all tokens.
 */
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/hw_random.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/miscdevice.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/tee_drv.h>
#include <linux/uaccess.h>
#include <linux/uuid.h>

/* Signed engine image, built in via CONFIG_EXTRA_FIRMWARE. */
#define FW_NAME			"ax620e-ce.bin"
#define FW_WORDS		19632

/* Engine registers. */
#define MBOX(n)			(((n) - 1) * 0x400)
#define MBOX_CTRL		0x3f00	/* write: per-mailbox nibble commands */
#define MBOX_STAT		0x3f00	/* read: per-mailbox nibble state */
#define MBOX_LOCKOUT		0x3f10
#define MODULE_STATUS		0x3fe0
#define FW_RAM			0x4000
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

/* clk_ce_bus_sel, periph CLK_MUX0 [1:0]: set-alias write of bit 1 selects cpll_312m. */
#define PERI_MUX0_SET		0xa8
#define CE_BUS_SEL		BIT(1)

#define IDENTITY		0x4f5a3647	/* W1 of every token */
#define RESULT_ERROR		BIT(31)
#define RESULT_CODE(w0)		(((w0) >> 24) & 0x7f)
#define INVALID_ASSET		13
#define LABEL_LEN		64	/* callers pad to it; the engine allows up to 216 */
#define TOKEN_US		5000000
#define PROVISION_US		30000000	/* writes OTP: the slowest token */
#define RNG_LEN			64	/* the hwrng core's reads */

/*
 * The policy, flag, label and message constants below feed the derivation;
 * changing any of them changes every derived key.
 *
 * The HUK derives only a trusted KDK: deriving a MAC key directly from it
 * returns ACCESS_ERROR (-7).
 */
#define KDK_POLICY	0x0000000400000000ULL	/* CE_POLICY_TRUSTED_KEY_DERIVE */
#define KEY_POLICY	0x0000000004000004ULL	/* CE_POLICY_SHA256 | CE_POLICY_MAC_GENERATE */
#define KEY_LEN		32		/* both KDK and K */
#define KEY_CREATE	BIT(28)		/* AssetCreate W4: NoLdLifetime */
#define KDF_FLAGS	(BIT(24) | BIT(14))	/* AssetLoad W3: Derive | SP800-108 counter; RFC5869 (15) off */
#define MAC_HMAC_SHA256	3
static const u8 KDK_LABEL[LABEL_LEN] = "kdk v1";	/* zero-padded */
static const char CHIPKEY_MSG[16] = "device chipkey 1";

struct chipkey {
	struct device *dev;
	void __iomem *base;
	struct mutex lock;
	u16 token_id;
	bool provisioned;	/* no more tokens until reboot */
	void *msg;		/* CHIPKEY_MSG, DMA-read by the engine */
	dma_addr_t msg_dma;
	void *rng_buf;		/* RNG_LEN bytes, DMA-written by the engine */
	dma_addr_t rng_dma;
	struct hwrng rng;
	struct miscdevice misc;
};

/* Send one command token; res gets its result. Results with another token ID (late) are skipped. */
static int ck_exchange_us(struct chipkey *ck, u32 *cmd, u32 *res, s64 us)
{
	ktime_t end = ktime_add_us(ktime_get(), us);
	u32 st;
	int ret;

	res[0] = 0;
	if (ck->provisioned)
		return -ENODEV;
	cmd[0] |= ++ck->token_id;
	cmd[1] = IDENTITY;
	if (readl(ck->base + MBOX_STAT) & NIB(MB, MB_IN_FULL))
		return -EBUSY;
	__iowrite32_copy(ck->base + MBOX(MB), cmd, 64);
	writel(NIB(MB, MB_IN_FULL), ck->base + MBOX_CTRL);
	do {
		us = ktime_us_delta(end, ktime_get());
		if (us <= 0)
			return -ETIMEDOUT;
		ret = readl_poll_timeout(ck->base + MBOX_STAT, st, st & NIB(MB, MB_OUT_FULL), 20, us);
		if (ret)
			return ret;
		__ioread32_copy(res, ck->base + MBOX(MB), 64);
		writel(NIB(MB, MB_OUT_FULL), ck->base + MBOX_CTRL);
	} while ((res[0] & 0xffff) != (cmd[0] & 0xffff));
	return 0;
}

static int ck_exchange(struct chipkey *ck, u32 *cmd, u32 *res)
{
	return ck_exchange_us(ck, cmd, res, TOKEN_US);
}

/* As ck_exchange_us, but an error result is -EIO. */
static int ck_token_us(struct chipkey *ck, u32 *cmd, u32 *res, s64 us)
{
	u32 op = cmd[0];
	int ret = ck_exchange_us(ck, cmd, res, us);

	if (!ret && (res[0] & RESULT_ERROR))
		ret = -EIO;
	if (ret)
		dev_err(ck->dev, "token %08x: %d, result %08x\n", op, ret, res[0]);
	return ret;
}

static int ck_token(struct chipkey *ck, u32 *cmd, u32 *res)
{
	return ck_token_us(ck, cmd, res, TOKEN_US);
}

static int ck_link(struct chipkey *ck, int n)
{
	writel(NIB(n, MB_LINK), ck->base + MBOX_CTRL);
	return readl(ck->base + MBOX_STAT) & NIB(n, MB_LINK) ? 0 : -EIO;
}

/* The first 256 bytes of the image go through mailbox 1, the rest into FW_RAM. */
static int ck_load(struct chipkey *ck)
{
	const struct firmware *fw;
	int ret;

	ret = request_firmware(&fw, FW_NAME, ck->dev);
	if (ret)
		return ret;
	ret = fw->size == FW_WORDS * 4 ? ck_link(ck, 1) : -EINVAL;
	if (!ret) {
		__iowrite32_copy(ck->base + MBOX(1), fw->data, 64);
		writel(NIB(1, MB_IN_FULL), ck->base + MBOX_CTRL);
		writel(NIB(1, MB_UNLINK), ck->base + MBOX_CTRL);
		__iowrite32_copy(ck->base + FW_RAM, fw->data + 256, FW_WORDS - 64);
		writel(ST_FW_WRITTEN, ck->base + MODULE_STATUS);
	}
	release_firmware(fw);
	return ret;
}

/* Load the image unless the engine already accepted one or is checking it. */
static int ck_boot(struct chipkey *ck)
{
	void __iomem *b = ck->base;
	u32 st;
	int ret;

	ret = readl_poll_timeout(b + MODULE_STATUS, st, !(st & ST_CRC24_BUSY), 10, 100000);
	if (ret || !(st & ST_CRC24_OK) || (st & ST_FATAL))
		goto bad;
	if ((!(st & ST_FW_WRITTEN) || ((st & ST_FW_CHECKS_DONE) && !(st & ST_FW_ACCEPTED))) &&
	    ck_load(ck))
		goto bad;
	ret = readl_poll_timeout(b + MODULE_STATUS, st, st & ST_FW_CHECKS_DONE, 10, 1000000);
	if (ret || !(st & ST_FW_ACCEPTED) || ck_link(ck, MB))
		goto bad;
	writel(0, b + MBOX_LOCKOUT);
	return 0;
bad:
	dev_err(ck->dev, "engine not ready, status %08x\n", st);
	return -EIO;
}

enum { HUK_ERROR, HUK_EMPTY, HUK_PROVISIONED };

/*
 * The HUK's state, and its asset ID when provisioned. Only a blank OTP with
 * no asset 0 is "empty"; any other state without asset 0, or a failed token,
 * is HUK_ERROR.
 * System Info: W0 opcode 15 | subcode 0; W5[15:12] = OTP state (0 fine,
 * 1 empty, others anomalies).
 * AssetSearch, static asset 0: W4[21:16] = asset number; result W1 = asset
 * ID; an absent asset is error INVALID_ASSET.
 */
static int ck_huk(struct chipkey *ck, u32 *id)
{
	u32 cmd[64] = { 0x0f000000 }, res[64], otp;

	*id = 0;
	if (ck_token(ck, cmd, res))
		return HUK_ERROR;
	otp = (res[5] >> 12) & 0xf;
	memset(cmd, 0, sizeof(cmd));
	cmd[0] = 0x07000000;
	if (ck_exchange(ck, cmd, res))
		goto bad;
	if (otp == 0 && !(res[0] & RESULT_ERROR) && res[1]) {
		*id = res[1];
		return HUK_PROVISIONED;
	}
	if (otp == 1 && (res[0] & RESULT_ERROR) && RESULT_CODE(res[0]) == INVALID_ASSET)
		return HUK_EMPTY;
bad:
	dev_err(ck->dev, "huk: otp %u, AssetSearch %08x %08x\n", otp, res[0], res[1]);
	return HUK_ERROR;
}

/* AssetCreate; *id = 0 on failure. */
static int ck_create(struct chipkey *ck, u64 policy, u32 *id)
{
	u32 cmd[64] = { 0x17000000, 0, lower_32_bits(policy), upper_32_bits(policy),
			KEY_LEN | KEY_CREATE }, res[64];
	int ret = ck_token(ck, cmd, res);

	*id = ret ? 0 : res[1];
	return ret;
}

/*
 * AssetLoad Derive: W2 = the asset to fill, W3 = KDF_FLAGS | AAD length
 * << 16; W4..W8 = 0: no salt input, no IV output, monotonic counter 0;
 * W9 = parent; W10..W25 = the LABEL_LEN-byte label, LSB first.
 */
static int ck_load_derive(struct chipkey *ck, u32 id, u32 parent, const u8 *label)
{
	u32 cmd[64] = { 0x27000000, 0, id, KDF_FLAGS | LABEL_LEN << 16 }, res[64];

	cmd[9] = parent;
	memcpy(&cmd[10], label, LABEL_LEN);
	return ck_token(ck, cmd, res);
}

/*
 * MAC, one shot: W2 length, W3/W4 DMA address, W5 length rounded up to 4, W6 = algorithm |
 * init+final (bits 4,5 clear) | key from asset (8), W24/W25 total length,
 * W28 key asset. Result W2.. = the MAC.
 */
static int ck_mac(struct chipkey *ck, u32 key, u8 *out)
{
	u32 cmd[64] = { 0x03000000, 0, sizeof(CHIPKEY_MSG), lower_32_bits(ck->msg_dma),
			upper_32_bits(ck->msg_dma), ALIGN(sizeof(CHIPKEY_MSG), 4),
			MAC_HMAC_SHA256 | BIT(8) }, res[64];
	int ret;

	cmd[24] = sizeof(CHIPKEY_MSG);
	cmd[28] = key;
	ret = ck_token(ck, cmd, res);
	if (!ret)
		memcpy(out, &res[2], KEY_LEN);
	memzero_explicit(res, sizeof(res));
	return ret;
}

/* AssetDelete: W2 = asset. Keeps the first error. */
static void ck_delete(struct chipkey *ck, u32 id, int *ret)
{
	u32 cmd[64] = { 0x37000000, 0, id }, res[64];
	int err;

	if (!id)
		return;
	err = ck_token(ck, cmd, res);
	*ret = *ret ?: err;
}

/* KDK = KDF(HUK, KDK_LABEL), K = KDF(KDK, label), out = HMAC-SHA256(K, CHIPKEY_MSG). */
static int ck_derive(struct chipkey *ck, const u8 *label, u8 *out)
{
	u32 huk, kdk = 0, key = 0;
	int ret;

	switch (ck_huk(ck, &huk)) {
	case HUK_EMPTY:
		return -ENOKEY;
	case HUK_ERROR:
		return -EIO;
	}
	ret = ck_create(ck, KDK_POLICY, &kdk) ?:
	      ck_load_derive(ck, kdk, huk, KDK_LABEL) ?:
	      ck_create(ck, KEY_POLICY, &key) ?:
	      ck_load_derive(ck, key, kdk, label) ?:
	      ck_mac(ck, key, out);
	ck_delete(ck, key, &ret);
	ck_delete(ck, kdk, &ret);
	return ret;
}

/*
 * One-time HUK provisioning.
 *
 * ProvisionRandomHUK: W0 opcode 7 | subcode 9 << 28 (| token ID). W1 =
 * IDENTITY, kept by the engine as the crypto officer ID. W2 = 0x40020000:
 * asset number 0, AutoSeed 0, 256-bit (BIT 17), CRC (BIT 30); KeyBlob
 * (BIT 31) clear, so no key blob is output. W3 = 0x00030108: TRNG for the
 * key, SampleCycles 3 << 16 | SampleDiv 1 << 8 | NoiseBlocks 8. Then System
 * Reset (W0 opcode 15 | subcode 2 << 28).
 *
 * Afterwards no more tokens are sent and huk reads "error: reboot after
 * provisioning" until the engine is restarted.
 */
static int ck_provision_tokens(struct chipkey *ck)
{
	u32 cmd[64] = { 0x97000000, IDENTITY, 0x40020000, 0x00030108 }, res[64];
	int ret = ck_token_us(ck, cmd, res, PROVISION_US);

	if (!ret) {
		memset(cmd, 0, sizeof(cmd));
		cmd[0] = 0x2f000000;
		ret = ck_token(ck, cmd, res);
	}
	ck->provisioned = true;
	return ret;
}

/*
 * RandomNumber: W0 opcode 4 | subcode 0 << 28, W2 = length in bytes,
 * W3/W4 = output DMA address of a buffer of whole words. A nonzero result
 * code without the error bit is a TRNG statistics warning: the bytes are
 * discarded.
 */
static int ck_rng_read(struct hwrng *rng, void *buf, size_t max, bool wait)
{
	struct chipkey *ck = container_of(rng, struct chipkey, rng);
	u32 len = min_t(size_t, max, RNG_LEN);
	u32 cmd[64] = { 0x04000000, 0, len, lower_32_bits(ck->rng_dma),
			upper_32_bits(ck->rng_dma) }, res[64];
	int ret;

	mutex_lock(&ck->lock);
	ret = ck_token(ck, cmd, res);
	if (!ret && RESULT_CODE(res[0]))
		ret = -EIO;
	if (!ret)
		memcpy(buf, ck->rng_buf, len);
	memzero_explicit(ck->rng_buf, RNG_LEN);
	mutex_unlock(&ck->lock);
	return ret ?: len;
}

/*
 * TRNG Configure: W0 opcode 4 | subcode 1 << 28, W2 = AutoSeed << 8 |
 * BIT 1 (reseed the DRBG) | BIT 0 (load this configuration and start),
 * W3 = SampleCycles << 16 | SampleDiv << 8 | NoiseBlocks. W4 = 0 (newer
 * firmware reads health-test cutoffs there).
 *
 * Quality 1024: the output is the engine's SP800-90A DRBG, seeded from its
 * own TRNG.
 */
static int ck_rng_register(struct chipkey *ck)
{
	u32 cmd[64] = { 0x14000000, 0, BIT(1) | BIT(0), 1 << 16 | 0 << 8 | 8 }, res[64];
	int ret;

	ck->rng_buf = dmam_alloc_coherent(ck->dev, RNG_LEN, &ck->rng_dma, GFP_KERNEL);
	if (!ck->rng_buf)
		return -ENOMEM;
	mutex_lock(&ck->lock);
	ret = ck_token(ck, cmd, res);
	mutex_unlock(&ck->lock);
	if (ret)
		return ret;
	ck->rng = (struct hwrng) {
		.name = "chipkey",
		.read = ck_rng_read,
		.quality = 1024,
	};
	return devm_hwrng_register(ck->dev, &ck->rng);
}

static struct chipkey *ck_of(struct device *dev)
{
	return container_of((struct miscdevice *)dev_get_drvdata(dev), struct chipkey, misc);
}

static ssize_t huk_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	static const char *const names[] = { "error", "empty", "provisioned" };
	struct chipkey *ck = ck_of(dev);
	u32 id;
	int state;

	mutex_lock(&ck->lock);
	state = ck->provisioned ? -1 : ck_huk(ck, &id);
	mutex_unlock(&ck->lock);
	if (state < 0)
		return sysfs_emit(buf, "error: reboot after provisioning\n");
	return sysfs_emit(buf, "%s\n", names[state]);
}
static struct device_attribute dev_attr_huk = __ATTR(huk, 0400, huk_show, NULL);

static ssize_t sysinfo_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct chipkey *ck = ck_of(dev);
	u32 cmd[64] = { 0x0f000000 }, res[64];
	int ret;

	mutex_lock(&ck->lock);
	ret = ck_token(ck, cmd, res);
	mutex_unlock(&ck->lock);
	if (ret)
		return ret;
	return sysfs_emit(buf, "%08x %08x %08x %08x %08x %08x\n",
			  res[0], res[1], res[2], res[3], res[4], res[5]);
}
static struct device_attribute dev_attr_sysinfo = __ATTR(sysinfo, 0400, sysinfo_show, NULL);

/* chipkey.pta's CMD_FUSES (read-only): [out] memref of FUSE_WORDS words. Kernel sessions only. */
static const uuid_t pta_chipkey = UUID_INIT(0x81fa3ed4, 0x750b, 0x432a,
					    0x92, 0xb2, 0x6f, 0x4d, 0xab, 0x34, 0x64, 0x01);
#define PTA_CMD_FUSES		2
#define FUSE_WORDS		11

static int ck_optee_match(struct tee_ioctl_version_data *ver, const void *data)
{
	return ver->impl_id == TEE_IMPL_ID_OPTEE;
}

/* One session per read: the TEE may probe after this driver. */
static int ck_fuses(u32 *w)
{
	struct tee_ioctl_open_session_arg sess = { .clnt_login = TEE_IOCTL_LOGIN_REE_KERNEL };
	struct tee_ioctl_invoke_arg inv = { .func = PTA_CMD_FUSES, .num_params = 4 };
	struct tee_param p[4] = { };
	struct tee_context *ctx;
	struct tee_shm *shm;
	void *va;
	int ret;

	ctx = tee_client_open_context(NULL, ck_optee_match, NULL, NULL);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);
	export_uuid(sess.uuid, &pta_chipkey);
	ret = tee_client_open_session(ctx, &sess, NULL);
	if (ret || sess.ret) {
		ret = ret ?: -EIO;
		goto ctx;
	}
	shm = tee_shm_alloc_kernel_buf(ctx, FUSE_WORDS * sizeof(u32));
	if (IS_ERR(shm)) {
		ret = PTR_ERR(shm);
		goto session;
	}
	inv.session = sess.session;
	p[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_OUTPUT;
	p[0].u.memref.shm = shm;
	p[0].u.memref.size = FUSE_WORDS * sizeof(u32);
	ret = tee_client_invoke_func(ctx, &inv, p);
	if (!ret && inv.ret)
		ret = -EIO;
	va = tee_shm_get_va(shm, 0);
	if (!ret && IS_ERR(va))
		ret = PTR_ERR(va);
	if (!ret)
		memcpy(w, va, FUSE_WORDS * sizeof(u32));
	tee_shm_free(shm);
session:
	tee_client_close_session(ctx, sess.session);
ctx:
	tee_client_close_context(ctx);
	return ret;
}

static ssize_t fuses_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	u32 w[FUSE_WORDS];
	int i, n = 0, ret = ck_fuses(w);

	if (ret)
		return ret;
	for (i = 0; i < FUSE_WORDS; i++)
		n += sysfs_emit_at(buf, n, "%08x%c", w[i], i + 1 < FUSE_WORDS ? ' ' : '\n');
	return n;
}
static struct device_attribute dev_attr_fuses = __ATTR(fuses, 0400, fuses_show, NULL);

static ssize_t provision_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct chipkey *ck = ck_of(dev);
	bool sent = false;
	u32 id;
	int ret;

	mutex_lock(&ck->lock);
	switch (ck_huk(ck, &id)) {
	case HUK_EMPTY:
		dev_warn(dev, "provisioning the HUK (one-time OTP write)\n");
		ret = ck_provision_tokens(ck);
		sent = true;
		break;
	case HUK_PROVISIONED:
		ret = -EEXIST;
		break;
	default:
		ret = -EIO;
	}
	mutex_unlock(&ck->lock);
	/* No more random numbers until reboot. Unlocked: unregistering waits for a read that may wait on ck->lock. */
	if (sent)
		devm_hwrng_unregister(ck->dev, &ck->rng);
	return ret ?: count;
}
static DEVICE_ATTR_WO(provision);

static struct attribute *ck_attrs[] = {
	&dev_attr_huk.attr, &dev_attr_sysinfo.attr, &dev_attr_fuses.attr, &dev_attr_provision.attr,
	NULL,
};
ATTRIBUTE_GROUPS(ck);

/* Per open file: the last write's result. */
struct ck_file {
	struct chipkey *ck;
	bool ready;
	u8 out[KEY_LEN];
};

static int ck_open(struct inode *inode, struct file *f)
{
	struct ck_file *cf = kzalloc(sizeof(*cf), GFP_KERNEL);

	if (!cf)
		return -ENOMEM;
	cf->ck = container_of(f->private_data, struct chipkey, misc);
	f->private_data = cf;
	return 0;
}

static int ck_release(struct inode *inode, struct file *f)
{
	kfree_sensitive(f->private_data);
	return 0;
}

static ssize_t ck_write(struct file *f, const char __user *ubuf, size_t len, loff_t *off)
{
	struct ck_file *cf = f->private_data;
	u8 label[LABEL_LEN];
	int ret;

	if (len != LABEL_LEN)
		return -EINVAL;
	if (copy_from_user(label, ubuf, len))
		return -EFAULT;
	mutex_lock(&cf->ck->lock);
	ret = ck_derive(cf->ck, label, cf->out);
	cf->ready = !ret;
	mutex_unlock(&cf->ck->lock);
	memzero_explicit(label, sizeof(label));
	return ret ?: len;
}

static ssize_t ck_read(struct file *f, char __user *ubuf, size_t len, loff_t *off)
{
	struct ck_file *cf = f->private_data;

	if (!cf->ready || len < KEY_LEN)
		return -EINVAL;
	return copy_to_user(ubuf, cf->out, KEY_LEN) ? -EFAULT : KEY_LEN;
}

static const struct file_operations ck_fops = {
	.owner = THIS_MODULE,
	.open = ck_open,
	.release = ck_release,
	.read = ck_read,
	.write = ck_write,
};

/*
 * The secure world's note in BACKUP0 of what its engine bring-up found, one
 * line every boot: a healthy boot's line is the baseline for a failing one's.
 * Bits: 12 noted; 20..22 cnt, aclk, pclk gates on; 23 a CE reset asserted;
 * 24..27 status FATAL, CRC24_OK, FW_WRITTEN, FW_ACCEPTED, read only with all
 * gates on and no reset; 28 the first self-test failed; 29 fatally.
 */
#define NOTE_GATES		GENMASK(22, 20)
#define MAP_BIT(v, bit, to)	(((v) & (bit)) ? (to) : 0)

static void ck_report_note(struct device *dev, u32 v)
{
	u32 st = MAP_BIT(v, BIT(24), ST_FATAL) | MAP_BIT(v, BIT(25), ST_CRC24_OK) |
		 MAP_BIT(v, BIT(26), ST_FW_WRITTEN) | MAP_BIT(v, BIT(27), ST_FW_ACCEPTED);
	char found[40];

	if (!(v & BIT(12)))
		return;
	if (v & BIT(23))
		strscpy(found, "a CE reset asserted", sizeof(found));
	else if ((v & NOTE_GATES) != NOTE_GATES)
		strscpy(found, "resets released, status unread", sizeof(found));
	else
		snprintf(found, sizeof(found), "resets released, status 0x%08x", st);
	dev_info(dev, "secure bring-up found gates cnt %d aclk %d pclk %d, %s; first self-test %s\n",
		 !!(v & BIT(20)), !!(v & BIT(21)), !!(v & BIT(22)), found,
		 !(v & BIT(28)) ? "passed" : (v & BIT(29)) ? "failed fatally" : "failed with no verdict");
}

static int ck_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk_bulk_data *clks;
	struct reset_control *rst;
	struct regmap *periph;
	struct chipkey *ck;
	void __iomem *note;
	int ret;

	ck = devm_kzalloc(dev, sizeof(*ck), GFP_KERNEL);
	if (!ck)
		return -ENOMEM;
	ck->dev = dev;
	mutex_init(&ck->lock);
	ck->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ck->base))
		return PTR_ERR(ck->base);
	ck->msg = dmam_alloc_coherent(dev, sizeof(CHIPKEY_MSG), &ck->msg_dma, GFP_KERNEL);
	if (!ck->msg)
		return -ENOMEM;
	memcpy(ck->msg, CHIPKEY_MSG, sizeof(CHIPKEY_MSG));

	/* Clocks and bus source first, then release the resets. */
	ret = devm_clk_bulk_get_all_enabled(dev, &clks);
	if (ret < 0)
		return dev_err_probe(dev, ret, "clocks\n");
	periph = syscon_regmap_lookup_by_phandle(dev->of_node, "axera,periph-syscon");
	if (IS_ERR(periph))
		return dev_err_probe(dev, PTR_ERR(periph), "periph syscon\n");
	ret = regmap_write(periph, PERI_MUX0_SET, CE_BUS_SEL);
	if (ret)
		return ret;
	rst = devm_reset_control_array_get_exclusive(dev);
	if (IS_ERR(rst))
		return dev_err_probe(dev, PTR_ERR(rst), "resets\n");
	ret = reset_control_deassert(rst);
	if (ret)
		return ret;

	note = devm_platform_ioremap_resource(pdev, 1);
	if (!IS_ERR(note))
		ck_report_note(dev, readl(note));
	ret = ck_boot(ck);
	if (ret)
		return ret;
	/* Key derivation does not need the hwrng; probe continues without it. */
	ret = ck_rng_register(ck);
	if (ret)
		dev_err(dev, "no hwrng: %d\n", ret);
	ck->misc = (struct miscdevice) {
		.minor = MISC_DYNAMIC_MINOR,
		.name = "chipkey",
		.fops = &ck_fops,
		.groups = ck_groups,
		.parent = dev,
		.mode = 0600,
	};
	return misc_register(&ck->misc);
}

static const struct of_device_id ck_of_match[] = {
	{ .compatible = "axera,ax620e-ce" },
	{ }
};

/* Initialized after the hwrng core. */
static struct platform_driver ck_driver = {
	.probe = ck_probe,
	.driver = {
		.name = "chipkey",
		.of_match_table = ck_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(ck_driver);
