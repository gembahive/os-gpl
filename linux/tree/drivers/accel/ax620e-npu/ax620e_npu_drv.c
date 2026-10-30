// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026, GembaHive, Inc.
 */
/*
 * The AX620E NPU (Neutron v4) as a DRM accel device.
 *
 * The NPU's DMA is not confined (no IOMMU), so a command stream decides what
 * it reads and writes. Userspace never supplies one: the kernel loads the
 * model from /lib/firmware/npu/, parses it (ax620e_npu_model.c) and owns every
 * buffer the NPU needs besides the caller's tensors:
 *
 *   <name>.neu     command stream, queues, OCM params, runtime-variable slots
 *   <name>.params  the weights, read straight into their DMA buffer
 *
 * One DMA buffer per model holds the command stream, the weights, the DDR
 * ring buffer ("ddr_swap") and the per-tensor offset words. SUBMIT names a
 * model and one GEM buffer per tensor, each at least the model's size for it;
 * the kernel writes the runtime-variable words into OCM, copies the OCM
 * params, programs the dispatcher and waits for the done interrupt, holding
 * every buffer's reference until the NPU has stopped. Buffers are CMA
 * (drm_gem_dma, write-combined) and cross drivers as dma-bufs.
 *
 * The driver runs a single NPU with all 2.5 MiB of OCM at offset 0 ("virtual
 * NPU disabled" mode), using only virtual NPU 0's registers and interrupts.
 *
 * Probe touches no register; the first submit brings the NPU up, with its
 * interrupts held off until then. Every poll is bounded; a job that errors or
 * overruns timeout_ms is halted and the NPU soft-reset before SUBMIT returns.
 * If it still does not stop, the device is dead: every later SUBMIT is
 * refused and the buffers it was given are never freed, since DMA into
 * reused memory is worse than a leak.
 *
 * The driver uses all of OCM, so the node must stay disabled wherever
 * something else (e.g. a parked CPU) occupies OCM.
 */

#include <drm/drm_accel.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/xarray.h>

#include "ax620e_npu.h"

/* Execution units: CONV0, CONV1, TENG, SDMA, 0x10000 apart. */
#define EU(i)			((i) * 0x10000)
#define EU_CFG			0x000	/* 0x3f at config */
#define EU_CRC_DATA		0x008	/* CRC off: 0 */
#define EU_CRC_CMD		0x00c
#define EU_CFG2			0x0cc	/* 0 at config */
#define EU_ERR_CODE		0x0d0
#define EU_ERR_CLR		0x0d4	/* write 1 */
#define NPU_EUS			4

/* Dispatcher, 0x3840000. */
#define DISP			0x40000
#define DISP_OCM_BASE		(DISP + 0x000)	/* OCM offset >> 5: 0 */
#define DISP_CMDQ_BASE		(DISP + 0x004)	/* command stream address >> 5 */
#define DISP_Q_START(t)		(DISP + 0x100 + 0x10 * (t))	/* IO 0x100, COMPUTE 0x110 */
#define DISP_Q_SIZE(t)		(DISP + 0x104 + 0x10 * (t))	/* len / 32 - 1 */
#define DISP_Q_BIT(t)		BIT(2 * (t))			/* IO 1, COMPUTE 4 */
#define DISP_MODE		(DISP + 0x120)	/* 0x2000 */
#define DISP_TRIGGER		(DISP + 0x168)	/* queue mask */
#define DISP_HALT		(DISP + 0x178)
#define DISP_HALT_CLR		(DISP + 0x188)
#define DISP_IDLE		(DISP + 0x190)	/* (val & 5) == 5: idle */
#define DISP_IDLE_MASK		0x5
#define DISP_ERR_CODE		(DISP + 0x20c)	/* 0xf0xx */
#define DISP_ERR_CLR		(DISP + 0x210)	/* write 1 */

/* Sync manager, 0x3850000. */
#define SM			0x50000
#define SM_DONE_CLR		(SM + 0x5c)	/* write the status back */
#define SM_IRQ_EN0		(SM + 0x64)	/* 0xfffff000 */
#define SM_IRQ_EN1		(SM + 0x68)
#define SM_IRQ_EN_ALL		0xfffff000
#define SM_DONE_STA		(SM + 0x74)
#define SM_ERR_CLR		(SM + 0x7c)
#define SM_ERR_STA		(SM + 0x84)
#define SM_ERR_DISP		BIT(4)		/* bits 0-3: an EU */
#define SM_EU_STA_CLR0		(SM + 0x8c)	/* 0xffff */
#define SM_EU_STA_CLR1		(SM + 0x90)
#define SM_WAIT_BYPASS		(SM + 0x1ac)	/* |= 0x101 while halting */
#define SM_WAIT_BYPASS0		0x101

/*
 * The NPU's own clock and reset block, 0x3890000. +0x64..+0x78 are named by
 * offset; their set/clear meaning is inferred.
 */
#define CR			0x90000
#define CR_MUX			(CR + 0x00)	/* [2:0] EU, [4:3] glb, [7:5] timer */
#define CR_MUX_FIELDS		0xff
#define CR_MUX_594		0xbb		/* EU 3, glb 3, timer 5 */
#define CR_04			(CR + 0x04)	/* saved at init, bit 0 restored via +0x64 */
#define CR_GATE			(CR + 0x10)	/* auto-gate: clear bits 1:0 */
#define CR_64			(CR + 0x64)
#define CR_68			(CR + 0x68)
#define CR_6C			(CR + 0x6c)
#define CR_70			(CR + 0x70)
#define CR_74			(CR + 0x74)
#define CR_78			(CR + 0x78)	/* 0xfff */
/* Soft reset writes {+0x70 = gate, +0x74 = rst, +0x78 = rst, +0x6c = gate}. */
#define CR_EUS_GATE		(0x4 | 0x8 | 0x80 | 0x40)
#define CR_EUS_RST		(0x4 | 0x8 | 0x400 | 0x100)
#define CR_SM_RST		0x200
#define CR_DISP_RST		0x10

/* Default unit configuration written at bring-up. */
#define EU_IR_DROP		0x0dc		/* EU0 and EU1 */
#define EU_IR_DROP_DEF		0xc80
#define SM_CONV_DELAY		(SM + 0x1a4)
#define SM_CONV_DELAY_DEF	0x50
#define SM_ISP0			(SM + 0x14)	/* {~0, 8, 16, 16} x2 */
#define SM_ISP0_CFG		(SM + 0x18)
#define SM_ISP1			(SM + 0x34)
#define SM_ISP1_CFG		(SM + 0x38)
#define SM_ISP_ALL		0xffffffff
#define SM_ISP_CFG_DEF		0x2108		/* 8 | 16 << 4 | 16 << 9 */
#define DISP_THROTTLE		(DISP + 0x124)	/* all-zero conf */
#define DISP_THROTTLE_HOLD	0xf00

/*
 * The DDR limiter, 0x3880000. Not written at first bring-up; the soft reset
 * writes it: +0x2c = a << 25 | b << 24 | c << 16 | d, then +0x28 = 0, 1.
 */
#define FAB			0x80000
#define FAB_LIMIT_LATCH		(FAB + 0x28)
#define FAB_LIMIT		(FAB + 0x2c)
#define FAB_LIMIT_DEF		(6 << 16 | 1900)	/* 0x0006076c */

/* A job's time limit in ms, clamped to 1..30000; read once when a job is armed. */
static unsigned int timeout_ms = 2000;

static int npu_timeout_set(const char *val, const struct kernel_param *kp)
{
	return param_set_uint_minmax(val, kp, 1, 30000);
}

static const struct kernel_param_ops npu_timeout_ops = {
	.set = npu_timeout_set,
	.get = param_get_uint,
};
module_param_cb(timeout_ms, &npu_timeout_ops, &timeout_ms, 0644);

#define NPU_IDLE_TIMEOUT_US	(100 * USEC_PER_MSEC)
/* Offset-word slots are 128 bytes apart. */
#define NPU_SLOT_STRIDE		128

struct npu_dev {
	struct drm_device drm;
	void __iomem *regs;
	void __iomem *ocm;
	u32 ocm_size;
	int irq_done, irq_err;
	struct mutex lock;		/* one job at a time, the bring-up and `dead` */
	bool up;			/* clocks on, interrupts enabled */
	/* The NPU did not stop after a reset: its DMA may still run, so nothing it was given is freed and no job runs. */
	bool dead;
	/*
	 * `done`, `err` and `err_code` belong to the job from arming to
	 * disarming, under irq_lock: a handler completes only an armed job and
	 * disarms it, so a stale edge completes nothing. Reset runs with both
	 * IRQ lines disabled.
	 */
	spinlock_t irq_lock;
	bool armed;
	struct completion done;
	u32 err, err_code;
};

/* A loaded model: what the NPU needs besides the caller's tensors. */
struct npu_model {
	struct npu_layout l;		/* file positions are stale after load */
	void *vaddr;			/* command stream | params | swap | offset words */
	dma_addr_t dma;
	size_t size;
	u32 params, swap, slots;	/* their offsets in the buffer */
	u8 *blobs;			/* the OCM params, back to back, 32-bit aligned */
};

struct npu_file {
	struct xarray models;
};

static struct npu_dev *to_npu(struct drm_device *drm)
{
	return container_of(drm, struct npu_dev, drm);
}

/* A runtime-variable word: a 32-byte-aligned address >> 5. */
static u32 npu_addr(dma_addr_t a)
{
	return lower_32_bits((u64)a >> 5);
}

static void npu_soft_reset(struct npu_dev *npu, u32 gate, u32 rst)
{
	writel(gate, npu->regs + CR_70);
	writel(rst, npu->regs + CR_74);
	writel(rst, npu->regs + CR_78);
	writel(gate, npu->regs + CR_6C);
}

/* Interrupts and units, after the clocks are on. Interrupts are disabled and stale status cleared first. */
static void npu_setup(struct npu_dev *npu)
{
	void __iomem *r = npu->regs;

	writel(0, r + SM_IRQ_EN0);
	writel(0, r + SM_IRQ_EN1);
	writel(readl(r + SM_DONE_STA), r + SM_DONE_CLR);
	writel(readl(r + SM_ERR_STA), r + SM_ERR_CLR);
	writel(SM_IRQ_EN_ALL, r + SM_IRQ_EN0);
	writel(SM_IRQ_EN_ALL, r + SM_IRQ_EN1);
	for (int i = 0; i < NPU_EUS; i++) {
		writel(0, r + EU(i) + EU_CFG2);
		writel(0, r + EU(i) + EU_CRC_DATA);
		writel(0, r + EU(i) + EU_CRC_CMD);
		writel(0x3f, r + EU(i) + EU_CFG);
	}
	writel(0x2000, r + DISP_MODE);
	writel(EU_IR_DROP_DEF, r + EU(0) + EU_IR_DROP);
	writel(EU_IR_DROP_DEF, r + EU(1) + EU_IR_DROP);
	writel(SM_CONV_DELAY_DEF, r + SM_CONV_DELAY);
	writel(DISP_THROTTLE_HOLD, r + DISP_THROTTLE);
	writel(0, r + DISP + 0x218);
	for (u32 off = 0x128; off <= 0x154; off += 4)
		writel(0, r + DISP + off);
	writel(0, r + DISP_THROTTLE);
	writel(SM_ISP_ALL, r + SM_ISP0);
	writel(SM_ISP_CFG_DEF, r + SM_ISP0_CFG);
	writel(SM_ISP_ALL, r + SM_ISP1);
	writel(SM_ISP_CFG_DEF, r + SM_ISP1_CFG);
}

/*
 * First register access: set the clock muxes (saving +0x00 and +0x04), release
 * the resets, restore the muxes, enable clocks, restore +0x04 bit 0 and enable
 * auto-gate. The clock block must read back the mux written before anything
 * behind it is touched.
 */
static int npu_power_up(struct npu_dev *npu)
{
	void __iomem *r = npu->regs;
	u32 mux, eb;

	/* Progress messages: if this first access hangs the bus, the log shows how far it got. */
	dev_info(npu->drm.dev, "powering up\n");
	writel((readl(r + CR_MUX) & ~0x18u) | 3 << 3, r + CR_MUX);	/* glb */
	writel((readl(r + CR_MUX) & ~0x07u) | 3, r + CR_MUX);		/* EU */
	writel(0x2, r + CR_68);						/* timer */
	writel((readl(r + CR_MUX) & ~0xe0u) | 5 << 5, r + CR_MUX);
	writel(0x2, r + CR_64);
	mux = readl(r + CR_MUX);
	eb = readl(r + CR_04);
	dev_info(npu->drm.dev, "clock mux reads 0x%x\n", mux);
	if ((mux & CR_MUX_FIELDS) != CR_MUX_594) {
		dev_err(npu->drm.dev, "clock block reads 0x%x: not powered\n", mux);
		return -ENODEV;
	}

	writel(0xfff, r + CR_78);
	writel(mux, r + CR_MUX);
	writel(0x4, r + CR_68);						/* clocks on */
	writel(~0x4u, r + CR_64);
	writel(0x311, r + CR_70);
	writel(~0x311u, r + CR_6C);
	writel(eb & 1, r + CR_64);					/* restore saved +0x04 bit 0 */
	writel(readl(r + CR_GATE) & ~0x3u, r + CR_GATE);
	npu_setup(npu);
	enable_irq(npu->irq_done);
	enable_irq(npu->irq_err);
	npu->up = true;
	return 0;
}

static int npu_wait_idle(struct npu_dev *npu)
{
	u32 v;

	return readl_poll_timeout(npu->regs + DISP_IDLE, v,
				  (v & DISP_IDLE_MASK) == DISP_IDLE_MASK, 10, NPU_IDLE_TIMEOUT_US);
}

/*
 * Halt, then soft-reset every unit and set them up again, with both interrupt
 * lines off. Still not idle after that: the device is dead.
 */
static int npu_reset(struct npu_dev *npu)
{
	void __iomem *r = npu->regs;

	disable_irq(npu->irq_done);
	disable_irq(npu->irq_err);
	writel(1, r + DISP_HALT);
	writel(readl(r + SM_WAIT_BYPASS) | SM_WAIT_BYPASS0, r + SM_WAIT_BYPASS);
	if (npu_wait_idle(npu))
		dev_warn(npu->drm.dev, "not idle after halt: 0x%x\n", readl(r + DISP_IDLE));
	writel(1, r + DISP_HALT_CLR);
	writel(0, r + DISP_HALT);
	writel(readl(r + SM_WAIT_BYPASS) & ~SM_WAIT_BYPASS0, r + SM_WAIT_BYPASS);
	writel(0xffff, r + SM_EU_STA_CLR0);
	writel(0xffff, r + SM_EU_STA_CLR1);
	npu_soft_reset(npu, CR_EUS_GATE, CR_EUS_RST);
	npu_soft_reset(npu, 0, CR_SM_RST);
	npu_soft_reset(npu, 0, CR_DISP_RST);
	npu_setup(npu);
	writel(FAB_LIMIT_DEF, r + FAB_LIMIT);
	writel(0, r + FAB_LIMIT_LATCH);
	writel(1, r + FAB_LIMIT_LATCH);
	enable_irq(npu->irq_done);
	enable_irq(npu->irq_err);
	if (!npu_wait_idle(npu))
		return 0;
	dev_err(npu->drm.dev, "not idle after reset (0x%x): stopped, its buffers kept\n",
		readl(r + DISP_IDLE));
	WRITE_ONCE(npu->dead, true);
	return -EIO;
}

/* Give the armed job its end, once. */
static void npu_job_end(struct npu_dev *npu, u32 err, u32 code)
{
	spin_lock(&npu->irq_lock);
	if (npu->armed) {
		npu->armed = false;
		npu->err = err;
		npu->err_code = code;
		complete(&npu->done);
	}
	spin_unlock(&npu->irq_lock);
}

/* A done interrupt ends the job. */
static irqreturn_t npu_irq_done(int irq, void *data)
{
	struct npu_dev *npu = data;
	u32 sta = readl(npu->regs + SM_DONE_STA);

	if (!sta)
		return IRQ_NONE;
	writel(sta, npu->regs + SM_DONE_CLR);
	npu_job_end(npu, 0, 0);
	return IRQ_HANDLED;
}

static irqreturn_t npu_irq_err(int irq, void *data)
{
	struct npu_dev *npu = data;
	u32 sta = readl(npu->regs + SM_ERR_STA), code = 0;

	if (!sta)
		return IRQ_NONE;
	writel(sta, npu->regs + SM_ERR_CLR);
	for (int i = 0; i < NPU_EUS; i++) {
		if (sta & BIT(i)) {
			code = readl(npu->regs + EU(i) + EU_ERR_CODE);
			writel(1, npu->regs + EU(i) + EU_ERR_CLR);
		}
	}
	if (sta & SM_ERR_DISP) {
		code = readl(npu->regs + DISP_ERR_CODE);
		writel(1, npu->regs + DISP_ERR_CLR);
	}
	npu_job_end(npu, sta, code);
	return IRQ_HANDLED;
}

/* One run of `m` on the tensors at `addr`. Under npu->lock. */
static int npu_run(struct npu_dev *npu, const struct npu_model *m, const dma_addr_t *addr)
{
	const struct npu_layout *l = &m->l;
	void __iomem *ocm = npu->ocm;
	u32 mask = 0, blob = 0;
	unsigned long limit;
	int ret;

	if (!npu->up) {
		ret = npu_power_up(npu);
		if (ret)
			return ret;
	}
	if (npu_wait_idle(npu)) {
		ret = npu_reset(npu);
		if (ret)
			return ret;
	}
	writel(0xffff, npu->regs + SM_EU_STA_CLR0);

	/* The offset words stay 0: SUBMIT takes only 32-byte-aligned buffers. */
	for (u32 i = 0; i < l->n_tensors; i++)
		writel(npu_addr(addr[i]), ocm + 4 * l->tensor[i].rtv);
	for (u32 i = 0; i < l->n_slots; i++)
		writel(npu_addr(m->dma + m->slots + NPU_SLOT_STRIDE * i), ocm + 4 * l->slot_rtv[i]);
	if (l->params_size)
		writel(npu_addr(m->dma + m->params), ocm + 4 * l->params_rtv);
	if (l->swap_size)
		writel(npu_addr(m->dma + m->swap), ocm + 4 * l->swap_rtv);
	if (l->ocm_base_rtv != NPU_NO_RTV)
		writel(ALIGN(l->ocm_reserved, 32) >> 5, ocm + 4 * l->ocm_base_rtv);
	/* After the RTV words; the parser refuses overlaps. */
	for (u32 i = 0; i < l->n_blobs; i++) {
		__iowrite32_copy(ocm + l->blob[i].ocm, m->blobs + blob, l->blob[i].len / 4);
		blob += l->blob[i].len;
	}

	writel(0, npu->regs + DISP_OCM_BASE);
	writel(npu_addr(m->dma), npu->regs + DISP_CMDQ_BASE);
	for (u32 i = 0; i < l->n_queues; i++) {
		const struct npu_queue *q = &l->queue[i];

		writel(q->start >> 5, npu->regs + DISP_Q_START(q->type));
		writel(q->len / 32 - 1, npu->regs + DISP_Q_SIZE(q->type));
		mask |= DISP_Q_BIT(q->type);
	}
	spin_lock_irq(&npu->irq_lock);
	reinit_completion(&npu->done);
	npu->err = 0;
	npu->err_code = 0;
	npu->armed = true;
	spin_unlock_irq(&npu->irq_lock);
	limit = msecs_to_jiffies(READ_ONCE(timeout_ms));
	writel(mask, npu->regs + DISP_TRIGGER);	/* writel orders the buffer writes above */

	wait_for_completion_timeout(&npu->done, limit);
	spin_lock_irq(&npu->irq_lock);
	ret = npu->armed ? -ETIMEDOUT : npu->err ? -EIO : 0;
	npu->armed = false;
	spin_unlock_irq(&npu->irq_lock);
	/* Its buffers are released only once the NPU has stopped. */
	if (!ret && npu_wait_idle(npu)) {
		dev_err(npu->drm.dev, "job done but not idle (0x%x)\n",
			readl(npu->regs + DISP_IDLE));
		ret = -EIO;
	}
	if (ret) {
		dev_err(npu->drm.dev, "job %s (status 0x%x, code 0x%x): resetting\n",
			ret == -EIO ? "error" : "timeout", npu->err, npu->err_code);
		if (npu_reset(npu))
			return -EIO;
	}
	return ret;
}

/* Never called on the model of the job a dead NPU did not stop (SUBMIT drops it). */
static void npu_model_free(struct npu_dev *npu, struct npu_model *m)
{
	if (m->vaddr)
		dma_free_wc(npu->drm.dev, m->size, m->vaddr, m->dma);
	kfree(m->blobs);
	kfree(m);
}

/* Lay out the model's DMA buffer in 128-byte aligned regions. */
static int npu_model_alloc(struct npu_dev *npu, struct npu_model *m)
{
	const struct npu_layout *l = &m->l;
	u64 params = ALIGN((u64)l->cmdq_len, 128);
	u64 swap = ALIGN(params + l->params_size, 128);
	u64 slots = ALIGN(swap + l->swap_size, 128);
	u64 size = slots + NPU_SLOT_STRIDE * l->n_slots;

	if (size > U32_MAX)
		return -E2BIG;
	m->params = params;
	m->swap = swap;
	m->slots = slots;
	m->size = size;
	m->vaddr = dma_alloc_wc(npu->drm.dev, m->size, &m->dma, GFP_KERNEL);
	return m->vaddr ? 0 : -ENOMEM;
}

/*
 * Load npu/<name>.neu and npu/<name>.params, and report the caller's tensors.
 * request_firmware_direct and request_firmware_into_buf never fall back to
 * the sysfs upload interface.
 */
static struct npu_model *npu_model_load(struct npu_dev *npu, struct drm_ax620e_npu_load_model *args)
{
	struct device *dev = npu->drm.dev;
	const struct firmware *fw, *pfw;
	struct npu_model *m;
	char path[16 + DRM_AX620E_NPU_NAME_LEN];
	u32 blobs = 0;
	int ret;

	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return ERR_PTR(-ENOMEM);
	snprintf(path, sizeof(path), "npu/%s.neu", args->name);
	ret = request_firmware_direct(&fw, path, dev);
	if (ret)
		goto err;
	ret = npu_parse(fw->data, fw->size, npu->ocm_size, &m->l);
	if (ret) {
		dev_err(dev, "%s: not a model this driver runs\n", path);
		goto err_fw;
	}
	for (u32 i = 0; i < m->l.n_blobs; i++)
		blobs += m->l.blob[i].len;	/* each inside the file */
	m->blobs = kmalloc(blobs, GFP_KERNEL);
	ret = m->blobs ? npu_model_alloc(npu, m) : -ENOMEM;
	if (ret)
		goto err_fw;
	memcpy(m->vaddr, fw->data + m->l.cmdq, m->l.cmdq_len);
	blobs = 0;
	for (u32 i = 0; i < m->l.n_blobs; i++) {
		memcpy(m->blobs + blobs, fw->data + m->l.blob[i].data, m->l.blob[i].len);
		blobs += m->l.blob[i].len;
	}
	args->n_inputs = m->l.n_inputs;
	args->n_outputs = m->l.n_tensors - m->l.n_inputs;
	for (u32 i = 0; i < m->l.n_tensors; i++) {
		args->sizes[i] = m->l.tensor[i].size;
		memcpy(args->names[i], fw->data + m->l.tensor[i].name, m->l.tensor[i].name_len);
	}
	release_firmware(fw);

	if (m->l.params_size) {
		snprintf(path, sizeof(path), "npu/%s.params", args->name);
		ret = request_firmware_into_buf(&pfw, path, dev, m->vaddr + m->params,
						m->l.params_size);
		if (ret)
			goto err;
		if (pfw->size != m->l.params_size) {
			dev_err(dev, "%s: %zu bytes, the model wants %u\n", path, pfw->size,
				m->l.params_size);
			ret = -EINVAL;
		}
		release_firmware(pfw);
		if (ret)
			goto err;
	}
	return m;

err_fw:
	release_firmware(fw);
err:
	npu_model_free(npu, m);
	return ERR_PTR(ret);
}

static int npu_ioctl_load_model(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_ax620e_npu_load_model *args = data;
	struct npu_file *f = file->driver_priv;
	size_t len = strnlen(args->name, sizeof(args->name));
	struct npu_model *m;
	int ret;

	if (READ_ONCE(to_npu(drm)->dead))
		return -ENODEV;
	if (!len || len == sizeof(args->name) || args->pad)
		return -EINVAL;
	/* A path under npu/: ASCII [a-z0-9_] only (the kernel's ctype is Latin-1). */
	for (size_t i = 0; i < len; i++) {
		char c = args->name[i];

		if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '_')
			return -EINVAL;
	}
	memset(args->sizes, 0, sizeof(args->sizes));
	memset(args->names, 0, sizeof(args->names));

	m = npu_model_load(to_npu(drm), args);
	if (IS_ERR(m))
		return PTR_ERR(m);
	ret = xa_alloc(&f->models, &args->handle, m, xa_limit_31b, GFP_KERNEL);
	if (ret)
		npu_model_free(to_npu(drm), m);
	return ret;
}

static int npu_ioctl_create_bo(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_ax620e_npu_create_bo *args = data;
	struct drm_gem_dma_object *bo;
	int ret;

	if (!args->size)
		return -EINVAL;
	bo = drm_gem_dma_create(drm, args->size);
	if (IS_ERR(bo))
		return PTR_ERR(bo);
	ret = drm_gem_handle_create(file, &bo->base, &args->handle);
	drm_gem_object_put(&bo->base);
	if (ret)
		return ret;
	args->offset = drm_vma_node_offset_addr(&bo->base.vma_node);
	return 0;
}

static int npu_ioctl_submit(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_ax620e_npu_submit *args = data;
	struct npu_file *f = file->driver_priv;
	struct drm_gem_object *bo[DRM_AX620E_NPU_MAX_TENSORS] = {};
	dma_addr_t addr[DRM_AX620E_NPU_MAX_TENSORS] = {};
	struct npu_dev *npu = to_npu(drm);
	struct npu_model *m = xa_load(&f->models, args->model);
	bool leak = false;
	int ret = 0;

	if (!m)
		return -ENOENT;
	if (args->n_bos != m->l.n_tensors)
		return -EINVAL;
	/* RTV words carry address >> 5, so every buffer starts 32-byte aligned. */
	for (u32 i = 0; i < args->n_bos && !ret; i++) {
		bo[i] = drm_gem_object_lookup(file, args->bos[i]);
		if (!bo[i])
			ret = -ENOENT;
		else if (bo[i]->size < m->l.tensor[i].size)
			ret = -EINVAL;
		else if ((addr[i] = to_drm_gem_dma_obj(bo[i])->dma_addr) & 0x1f)
			ret = -EINVAL;
	}
	if (!ret)
		ret = mutex_lock_interruptible(&npu->lock);
	if (!ret) {
		ret = npu->dead ? -ENODEV : npu_run(npu, m, addr);
		leak = npu->dead && ret != -ENODEV;
		mutex_unlock(&npu->lock);
	}
	/* The NPU did not stop: the job's buffers keep their references and its model leaves the file, never freed. */
	if (leak) {
		xa_erase(&f->models, args->model);
		return ret;
	}
	for (u32 i = 0; i < args->n_bos; i++)
		if (bo[i])
			drm_gem_object_put(bo[i]);
	return ret;
}

static int npu_open(struct drm_device *drm, struct drm_file *file)
{
	struct npu_file *f = kzalloc(sizeof(*f), GFP_KERNEL);

	if (!f)
		return -ENOMEM;
	xa_init_flags(&f->models, XA_FLAGS_ALLOC1);
	file->driver_priv = f;
	return 0;
}

/* No ioctl runs on a closing file, so no job holds these models. */
static void npu_postclose(struct drm_device *drm, struct drm_file *file)
{
	struct npu_file *f = file->driver_priv;
	struct npu_model *m;
	unsigned long id;

	xa_for_each(&f->models, id, m)
		npu_model_free(to_npu(drm), m);
	xa_destroy(&f->models);
	kfree(f);
}

static const struct drm_ioctl_desc npu_ioctls[] = {
	DRM_IOCTL_DEF_DRV(AX620E_NPU_LOAD_MODEL, npu_ioctl_load_model, 0),
	DRM_IOCTL_DEF_DRV(AX620E_NPU_CREATE_BO, npu_ioctl_create_bo, 0),
	DRM_IOCTL_DEF_DRV(AX620E_NPU_SUBMIT, npu_ioctl_submit, 0),
};

DEFINE_DRM_ACCEL_FOPS(npu_fops);

static const struct drm_driver npu_driver = {
	.driver_features = DRIVER_COMPUTE_ACCEL | DRIVER_GEM,
	.open = npu_open,
	.postclose = npu_postclose,
	.gem_prime_import_sg_table = drm_gem_dma_prime_import_sg_table,
	.ioctls = npu_ioctls,
	.num_ioctls = ARRAY_SIZE(npu_ioctls),
	.fops = &npu_fops,
	.name = "ax620e-npu",
	.desc = "AX620E NPU",
	.major = 1,
};

static int npu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct npu_dev *npu;
	int ret;

	npu = devm_drm_dev_alloc(dev, &npu_driver, struct npu_dev, drm);
	if (IS_ERR(npu))
		return PTR_ERR(npu);
	npu->regs = devm_platform_ioremap_resource_byname(pdev, "regs");
	if (IS_ERR(npu->regs))
		return PTR_ERR(npu->regs);
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ocm");
	npu->ocm = devm_ioremap_resource(dev, res);
	if (IS_ERR(npu->ocm))
		return PTR_ERR(npu->ocm);
	npu->ocm_size = resource_size(res);
	init_completion(&npu->done);
	spin_lock_init(&npu->irq_lock);
	ret = drmm_mutex_init(&npu->drm, &npu->lock);
	if (ret)
		return ret;
	/* Its address words carry 32 bits of address >> 5; DRAM is below 4 GiB. */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	npu->irq_done = platform_get_irq_byname(pdev, "done");
	if (npu->irq_done < 0)
		return npu->irq_done;
	npu->irq_err = platform_get_irq_byname(pdev, "error");
	if (npu->irq_err < 0)
		return npu->irq_err;
	ret = devm_request_irq(dev, npu->irq_done, npu_irq_done, IRQF_NO_AUTOEN,
			       "npu-done", npu);
	if (ret)
		return ret;
	ret = devm_request_irq(dev, npu->irq_err, npu_irq_err, IRQF_NO_AUTOEN,
			       "npu-error", npu);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, npu);
	return drm_dev_register(&npu->drm, 0);
}

/* Unloading needs every file closed (the fops hold the module), so no job runs. */
static void npu_remove(struct platform_device *pdev)
{
	struct npu_dev *npu = platform_get_drvdata(pdev);

	drm_dev_unplug(&npu->drm);
}

static const struct of_device_id npu_of_match[] = {
	{ .compatible = "axera,ax620e-npu" },
	{ }
};
MODULE_DEVICE_TABLE(of, npu_of_match);

static struct platform_driver npu_platform_driver = {
	.probe = npu_probe,
	.remove = npu_remove,
	.driver = {
		.name = "ax620e-npu",
		.of_match_table = npu_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(npu_platform_driver);

MODULE_DESCRIPTION("Axera AX620E NPU");
MODULE_LICENSE("GPL");
