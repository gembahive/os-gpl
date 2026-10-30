// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026, GembaHive, Inc.
 */
/*
 * Axera AX620E TDP: crop, scale and colour-convert one frame between two
 * buffers, as a V4L2 mem2mem device. NV12 or YUYV in, NV12 or RGB565 out;
 * the crop is the OUTPUT queue's selection, the scale the CAPTURE size.
 *
 * The vendor driver hands its register list to a command DMA; the list is plain
 * register writes, so the CPU writes it here, the kick last. User space
 * only queues buffers: every register value is computed in this file.
 */

#include <linux/clk.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/fcntl.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/v4l2-rect.h>
#include <media/videobuf2-dma-contig.h>

#define TDP_NAME	"ax620e-tdp"
#define TDP_VERSION	0x00
#define TDP_VERSION_ID	0x20230511	/* what the vendor's TDP reads back */
#define TDP_IRQ_STATUS	0x04
#define TDP_IRQ_MASK	0x0c
#define TDP_IRQ_CLEAR	0x10	/* W1C */
#define TDP_GO		0x1c
#define TDP_AXI_EN	0x2f0
#define TDP_AXI_BUSY	0x2f4
#define TDP_IRQ_DONE	BIT(0)
#define TDP_IRQ_START	BIT(1)	/* masked */
#define TDP_IRQ_AXI_ERR	(BIT(2) | BIT(3))	/* write, read */

/*
 * Limits: even width and height, 16-byte stride, width 8..8192, height
 * 2..8192; scale ratios (in << 10) / out of 32..32768, i.e. 1/32..32x.
 */
#define TDP_MIN_W	8
#define TDP_MIN_H	2
#define TDP_MAX		8192
#define TDP_RATIO_MIN	32
#define TDP_RATIO_MAX	32768
#define TDP_TIMEOUT_MS	1000

struct tdp_fmt {
	u32 fourcc;
	u8 hw;		/* the TDP's own format code */
	u8 cpp;		/* bytes a pixel in the first plane */
	bool nv12;
};

static const struct tdp_fmt tdp_src_fmts[] = {
	{ V4L2_PIX_FMT_NV12, 0x0a, 1, true },
	{ V4L2_PIX_FMT_YUYV, 0x13, 2, false },
};

static const struct tdp_fmt tdp_dst_fmts[] = {
	{ V4L2_PIX_FMT_NV12, 0x0a, 1, true },
	{ V4L2_PIX_FMT_RGB565, 0x28, 2, false },
};

/*
 * Initial {value, offset} tables: two RGB->YUV CSCs and one YUV->RGB CSC, then
 * the two words it clears. The CSCs are BT.601 full range, Q8, 11-bit signed.
 */
static const u16 tdp_init_regs[][2] = {
	/* RGB->YUV input CSC: off, Y 29,150,77; U 128,-85,-43; V -21,-107,128 */
	{ 0x058, 0 }, { 0x05c, 0x1d }, { 0x060, 0x96 }, { 0x064, 0x4d }, { 0x068, 0x80 },
	{ 0x06c, 0x7ab }, { 0x070, 0x7d5 }, { 0x074, 0x7eb }, { 0x078, 0x795 }, { 0x07c, 0x80 },
	{ 0x080, 0 }, { 0x084, 0 }, { 0x088, 0 }, { 0x08c, 0 }, { 0x090, 0 }, { 0x094, 0 },
	{ 0x098, 0 }, { 0x09c, 0 }, { 0x0a0, 0 }, { 0x0a4, 0x20 }, { 0x0a8, 0 }, { 0x0ac, 0 },
	{ 0x0b0, 0 },
	/* the second RGB->YUV CSC, same values */
	{ 0x1a4, 0 }, { 0x1a8, 0x1d }, { 0x1ac, 0x96 }, { 0x1b0, 0x4d }, { 0x1b4, 0x80 },
	{ 0x1b8, 0x7ab }, { 0x1bc, 0x7d5 }, { 0x1c0, 0x7eb }, { 0x1c4, 0x795 }, { 0x1c8, 0x80 },
	{ 0x1cc, 0 }, { 0x1d0, 0 }, { 0x1d4, 0 }, { 0x1d8, 0 }, { 0x1dc, 0 }, { 0x1e0, 0 },
	{ 0x1f0, 0x20 }, { 0x1f4, 0 },
	/* YUV->RGB output CSC: off, 1, 1.772, 0; 1, -0.344, -0.714; 1, 0, 1.402 */
	{ 0x250, 0 }, { 0x254, 0x100 }, { 0x258, 0x1c6 }, { 0x25c, 0 }, { 0x260, 0x100 },
	{ 0x264, 0x7a8 }, { 0x268, 0x749 }, { 0x26c, 0x100 }, { 0x270, 0 }, { 0x274, 0x167 },
	{ 0x278, 0 }, { 0x27c, 0 }, { 0x280, 0 }, { 0x284, 0 }, { 0x288, 0 }, { 0x28c, 0 },
	{ 0x29c, 1 }, { 0x2a0, 1 },
	{ 0x2cc, 0 }, { 0x014, 0 },
};

struct tdp_dev {
	struct v4l2_device v4l2;
	struct video_device vfd;
	struct v4l2_m2m_dev *m2m;
	struct mutex lock;
	struct device *dev;
	void __iomem *regs;
	struct reset_control *rst;
	int irq;
	/*
	 * Armed once per job before the kick. The job belongs to the IRQ
	 * thread only if it cancels the still-pending watchdog; once the
	 * watchdog has started, the job is the watchdog's. The watchdog
	 * holds the IRQ off (waiting out a running thread) until it has
	 * reset and finished, so no IRQ of that job reaches the next one.
	 */
	struct delayed_work watchdog;
	/*
	 * AXI would not drain: every later job fails. Set before the job is
	 * finished; m2m's job lock orders that before the next device_run.
	 */
	bool dead;
};

struct tdp_ctx {
	struct v4l2_fh fh;
	struct tdp_dev *tdp;
	struct v4l2_pix_format src, dst;
	const struct tdp_fmt *sfmt, *dfmt;
	struct v4l2_rect crop;
	u32 src_seq, dst_seq;	/* per queue, from 0 at each streamon */
};

static struct tdp_ctx *fh_to_ctx(struct file *file)
{
	return container_of(file_to_v4l2_fh(file), struct tdp_ctx, fh);
}

static void tdp_wr(struct tdp_dev *tdp, u32 off, u32 val)
{
	writel(val, tdp->regs + off);
}

/* With AXI on: interrupts, then the tables. */
static void tdp_hw_init(struct tdp_dev *tdp)
{
	unsigned int i;

	tdp_wr(tdp, TDP_AXI_EN, 1);
	tdp_wr(tdp, TDP_IRQ_CLEAR, TDP_IRQ_DONE | TDP_IRQ_START);
	tdp_wr(tdp, TDP_IRQ_MASK, TDP_IRQ_START);
	for (i = 0; i < ARRAY_SIZE(tdp_init_regs); i++)
		tdp_wr(tdp, tdp_init_regs[i][0], tdp_init_regs[i][1]);
}

/*
 * Stop new AXI traffic, let the bus drain (10000 x 10 us), pulse the resets.
 * No reset while transfers are outstanding; AXI then stays off: false means the
 * engine may still be writing and is never used again.
 */
static bool tdp_hw_reset(struct tdp_dev *tdp)
{
	u32 busy;

	tdp_wr(tdp, TDP_AXI_EN, 0);
	if (readl_poll_timeout(tdp->regs + TDP_AXI_BUSY, busy, !busy, 10, 100000)) {
		dev_err(tdp->dev, "AXI never drained (%#x): TDP off until reboot\n", busy);
		return false;
	}
	reset_control_assert(tdp->rst);
	reset_control_deassert(tdp->rst);
	tdp_hw_init(tdp);
	return true;
}

/*
 * One job for an uncompressed frame with no rotation, mirror, overlay, mask or
 * letterbox. The crop moves the input addresses; ratios are Q10 in/out, each
 * phase starts half a step in. 0x040/0x04c/0x2a8/0x2b4 are taken to be the
 * high halves of the 64-bit addresses.
 */
static void tdp_program(struct tdp_ctx *ctx, dma_addr_t src, dma_addr_t dst)
{
	const struct v4l2_rect *c = &ctx->crop;
	u32 sbpl = ctx->src.bytesperline, dbpl = ctx->dst.bytesperline;
	u32 ow = ctx->dst.width, oh = ctx->dst.height;
	u32 rw = (c->width << 10) / ow, rh = (c->height << 10) / oh;
	/*
	 * Second-plane addresses: out is Y + pixel stride * height, so for
	 * RGB565 (no second plane) it stays inside the frame; in is plane 1,
	 * which for packed YUYV is taken as Y + w*h, plus the crop offset
	 * (top/2 * stride + left) * bytes a luma pixel.
	 */
	u32 c_base = ctx->sfmt->nv12 ? sbpl * ctx->src.height : ctx->src.width * ctx->src.height;
	dma_addr_t in_y = src + c->top * sbpl + c->left * ctx->sfmt->cpp;
	dma_addr_t in_c = src + c_base + c->top / 2 * sbpl + c->left * ctx->sfmt->cpp;
	dma_addr_t out_c = dst + dbpl / ctx->dfmt->cpp * oh;
	bool rgb = !ctx->dfmt->nv12;
	const u32 prog[][2] = {
		{ 0x314, 0 },
		{ 0x0e0, 0 }, { 0x0e4, 0 }, { 0x0dc, 0 }, { 0x0d8, 0 },	/* mirror, flip, rotate */
		{ 0x2c0, 0 },						/* compressed output */
		{ 0x2b0, dbpl }, { 0x2bc, dbpl },
		{ 0x0b8, rw != 1024 || rh != 1024 }, { 0x0c0, rw | rh << 16 }, { 0x0bc, ow | oh << 16 },
		{ 0x0d4, 0 },
		{ 0x2a8, upper_32_bits(dst) }, { 0x2ac, lower_32_bits(dst) },
		{ 0x2b4, upper_32_bits(out_c) }, { 0x2b8, lower_32_bits(out_c) },
		{ 0x020, 1 },
		{ 0x034, 0 }, { 0x310, 0 },				/* compressed input */
		{ 0x03c, 0 }, { 0x024, c->width | c->height << 16 },
		/* Only used for compressed input. */
		{ 0x038, 0 },
		{ 0x0c4, (rh >> 1) - 512 }, { 0x0cc, (rh >> 1) - 512 },	/* luma, chroma: vertical */
		{ 0x0c8, (rw >> 1) - 512 }, { 0x0d0, (rw >> 1) - 512 },	/* horizontal */
		{ 0x048, sbpl }, { 0x054, sbpl },
		{ 0x040, upper_32_bits(in_y) }, { 0x044, lower_32_bits(in_y) },
		{ 0x04c, upper_32_bits(in_c) }, { 0x050, lower_32_bits(in_c) },
		{ 0x15c, 0 },						/* overlay */
		{ 0x0e8, 0 }, { 0x0f8, 0 }, { 0x0ec, 0 }, { 0x0f0, 0 }, { 0x0f4, 0 },	/* mask */
		{ 0x028, ctx->sfmt->hw }, { 0x2a4, ctx->dfmt->hw },
		/* RGB-input CSC off; YUV->RGB on for RGB out, else the chroma plane on. */
		{ 0x0b4, 0 }, { 0x058, 0 }, { 0x250, rgb }, { 0x290, !rgb },
		{ TDP_IRQ_CLEAR, TDP_IRQ_DONE | TDP_IRQ_START }, { TDP_IRQ_MASK, TDP_IRQ_START },
		{ 0x2fc, 4 }, { TDP_GO, 1 },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(prog); i++)
		tdp_wr(ctx->tdp, prog[i][0], prog[i][1]);
}

static bool tdp_ratio_ok(u32 in, u32 out)
{
	u32 r = (in << 10) / out;

	return r >= TDP_RATIO_MIN && r <= TDP_RATIO_MAX;
}

/*
 * @v even, in [@lo, @hi], and within 1/32..32x of @other: either side of
 * the ratio, other/32 rounded up .. other*32 keeps (in << 10) / out in range.
 */
static u32 tdp_clamp_scale(u32 v, u32 other, u32 lo, u32 hi)
{
	lo = ALIGN(max(lo, DIV_ROUND_UP(other, 32)), 2);
	hi = min(hi, other * 32);
	return min(max(v, lo), hi) & ~1;
}

static void tdp_finish(struct tdp_dev *tdp, enum vb2_buffer_state state)
{
	struct tdp_ctx *ctx = v4l2_m2m_get_curr_priv(tdp->m2m);
	struct vb2_v4l2_buffer *src = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	struct vb2_v4l2_buffer *dst = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);

	v4l2_m2m_buf_copy_metadata(src, dst, true);
	src->sequence = ctx->src_seq++;
	dst->sequence = ctx->dst_seq++;
	vb2_set_plane_payload(&dst->vb2_buf, 0, ctx->dst.sizeimage);
	v4l2_m2m_buf_done(src, state);
	v4l2_m2m_buf_done(dst, state);
	v4l2_m2m_job_finish(tdp->m2m, ctx->fh.m2m_ctx);
}

/*
 * Keep a buffer's memory allocated forever: an exported dma-buf that is
 * never put holds an MMAP buffer, an extra reference an imported one.
 * get_dmabuf runs here without the queue lock, so it can race VIDIOC_EXPBUF
 * filling the buffer's sgt_base; the worst case is a leaked sg_table.
 */
static bool tdp_pin(struct vb2_v4l2_buffer *vbuf)
{
	struct vb2_buffer *vb = &vbuf->vb2_buf;

	if (vb->memory == VB2_MEMORY_DMABUF) {
		get_dma_buf(vb->planes[0].dbuf);
		return true;
	}
	return vb->vb2_queue->mem_ops->get_dmabuf(vb, vb->planes[0].mem_priv, O_RDWR);
}

/*
 * End the job in flight with an error. If the engine would not stop, it may
 * still write the destination whenever it likes: its memory is leaked so
 * freeing the queue cannot hand it to anyone else, and if even that fails
 * the job is never finished (closing the device hangs, nothing is overwritten).
 * The source is only read; a late read of reused memory lands in the pinned
 * destination.
 */
static void tdp_fail(struct tdp_dev *tdp)
{
	struct tdp_ctx *ctx = v4l2_m2m_get_curr_priv(tdp->m2m);

	if (!tdp_hw_reset(tdp)) {
		WRITE_ONCE(tdp->dead, true);
		/* A set bit masks its source: no interrupt storm. */
		tdp_wr(tdp, TDP_IRQ_MASK, 0xf);
		if (!tdp_pin(v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx)))
			return;
	}
	tdp_finish(tdp, VB2_BUF_STATE_ERROR);
}

static void tdp_device_run(void *priv)
{
	struct tdp_ctx *ctx = priv;
	struct tdp_dev *tdp = ctx->tdp;
	struct vb2_v4l2_buffer *src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	struct vb2_v4l2_buffer *dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);

	if (!tdp_ratio_ok(ctx->crop.width, ctx->dst.width) ||
	    !tdp_ratio_ok(ctx->crop.height, ctx->dst.height)) {
		dev_warn_ratelimited(tdp->dev, "scale beyond 1/32..32x\n");
		tdp_finish(tdp, VB2_BUF_STATE_ERROR);
		return;
	}
	if (READ_ONCE(tdp->dead)) {
		tdp_finish(tdp, VB2_BUF_STATE_ERROR);
		return;
	}
	schedule_delayed_work(&tdp->watchdog, msecs_to_jiffies(TDP_TIMEOUT_MS));
	tdp_program(ctx, vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0),
		    vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 0));
}

static irqreturn_t tdp_irq(int irq, void *data)
{
	struct tdp_dev *tdp = data;
	u32 st = readl(tdp->regs + TDP_IRQ_STATUS);

	if (!st)
		return IRQ_NONE;
	tdp_wr(tdp, TDP_IRQ_CLEAR, st);
	if (!(st & (TDP_IRQ_DONE | TDP_IRQ_AXI_ERR)) || !cancel_delayed_work(&tdp->watchdog))
		return IRQ_HANDLED;
	if (st & TDP_IRQ_AXI_ERR) {
		dev_err(tdp->dev, "AXI error, irq %#x\n", st);
		tdp_fail(tdp);
	} else {
		tdp_finish(tdp, VB2_BUF_STATE_DONE);
	}
	return IRQ_HANDLED;
}

static void tdp_watchdog(struct work_struct *work)
{
	struct tdp_dev *tdp = container_of(work, struct tdp_dev, watchdog.work);

	/* Waits out a running IRQ thread and holds off the next one. */
	disable_irq(tdp->irq);
	dev_err(tdp->dev, "no interrupt, irq status %#x\n", readl(tdp->regs + TDP_IRQ_STATUS));
	tdp_fail(tdp);
	enable_irq(tdp->irq);
}

static const struct v4l2_m2m_ops tdp_m2m_ops = {
	.device_run = tdp_device_run,
};

static const struct tdp_fmt *tdp_find(const struct tdp_fmt *fmts, unsigned int n, u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		if (fmts[i].fourcc == fourcc)
			return &fmts[i];
	return &fmts[0];
}

static const struct tdp_fmt *tdp_fill(struct v4l2_pix_format *f, bool src)
{
	const struct tdp_fmt *fmt = src ? tdp_find(tdp_src_fmts, ARRAY_SIZE(tdp_src_fmts), f->pixelformat)
					: tdp_find(tdp_dst_fmts, ARRAY_SIZE(tdp_dst_fmts), f->pixelformat);

	f->pixelformat = fmt->fourcc;
	v4l_bound_align_image(&f->width, TDP_MIN_W, TDP_MAX, 1, &f->height, TDP_MIN_H, TDP_MAX, 1, 0);
	f->field = V4L2_FIELD_NONE;
	/* An undescribed source gets the colour table's own BT.601 full range. */
	if (src && f->colorspace == V4L2_COLORSPACE_DEFAULT) {
		f->colorspace = V4L2_COLORSPACE_SRGB;
		f->ycbcr_enc = V4L2_YCBCR_ENC_601;
		f->quantization = V4L2_QUANTIZATION_FULL_RANGE;
		f->xfer_func = V4L2_XFER_FUNC_SRGB;
	}
	f->bytesperline = ALIGN(f->width * fmt->cpp, 16);
	f->sizeimage = f->bytesperline * f->height * (fmt->nv12 ? 3 : 2) / 2;
	return fmt;
}

static int tdp_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	strscpy(cap->driver, TDP_NAME, sizeof(cap->driver));
	strscpy(cap->card, "AX620E TDP", sizeof(cap->card));
	return 0;
}

static int tdp_enum_fmt(const struct tdp_fmt *fmts, unsigned int n, struct v4l2_fmtdesc *f)
{
	if (f->index >= n)
		return -EINVAL;
	f->pixelformat = fmts[f->index].fourcc;
	return 0;
}

static int tdp_enum_fmt_src(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	return tdp_enum_fmt(tdp_src_fmts, ARRAY_SIZE(tdp_src_fmts), f);
}

static int tdp_enum_fmt_dst(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	return tdp_enum_fmt(tdp_dst_fmts, ARRAY_SIZE(tdp_dst_fmts), f);
}

static int tdp_g_fmt_src(struct file *file, void *priv, struct v4l2_format *f)
{
	f->fmt.pix = fh_to_ctx(file)->src;
	return 0;
}

static int tdp_g_fmt_dst(struct file *file, void *priv, struct v4l2_format *f)
{
	f->fmt.pix = fh_to_ctx(file)->dst;
	return 0;
}

static int tdp_try_fmt_src(struct file *file, void *priv, struct v4l2_format *f)
{
	tdp_fill(&f->fmt.pix, true);
	return 0;
}

/* The capture side carries the source's colour description. */
static void tdp_copy_colour(struct v4l2_pix_format *dst, const struct v4l2_pix_format *src)
{
	dst->colorspace = src->colorspace;
	dst->ycbcr_enc = src->ycbcr_enc;
	dst->quantization = src->quantization;
	dst->xfer_func = src->xfer_func;
}

/* The output size, kept within the scaler's reach of the crop. */
static const struct tdp_fmt *tdp_fill_dst(struct tdp_ctx *ctx, struct v4l2_pix_format *f)
{
	tdp_copy_colour(f, &ctx->src);
	f->width = tdp_clamp_scale(f->width, ctx->crop.width, TDP_MIN_W, TDP_MAX);
	f->height = tdp_clamp_scale(f->height, ctx->crop.height, TDP_MIN_H, TDP_MAX);
	return tdp_fill(f, false);
}

static int tdp_try_fmt_dst(struct file *file, void *priv, struct v4l2_format *f)
{
	tdp_fill_dst(fh_to_ctx(file), &f->fmt.pix);
	return 0;
}

static int tdp_s_fmt_src(struct file *file, void *priv, struct v4l2_format *f)
{
	struct tdp_ctx *ctx = fh_to_ctx(file);

	if (vb2_is_busy(v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type)))
		return -EBUSY;
	ctx->sfmt = tdp_fill(&f->fmt.pix, true);
	ctx->src = f->fmt.pix;
	ctx->crop = (struct v4l2_rect) { .width = ctx->src.width, .height = ctx->src.height };
	tdp_copy_colour(&ctx->dst, &ctx->src);
	return 0;
}

static int tdp_s_fmt_dst(struct file *file, void *priv, struct v4l2_format *f)
{
	struct tdp_ctx *ctx = fh_to_ctx(file);

	if (vb2_is_busy(v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type)))
		return -EBUSY;
	ctx->dfmt = tdp_fill_dst(ctx, &f->fmt.pix);
	ctx->dst = f->fmt.pix;
	return 0;
}

static int tdp_g_selection(struct file *file, void *priv, struct v4l2_selection *s)
{
	struct tdp_ctx *ctx = fh_to_ctx(file);

	if (s->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
		return -EINVAL;
	switch (s->target) {
	case V4L2_SEL_TGT_CROP:
		s->r = ctx->crop;
		return 0;
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		s->r = (struct v4l2_rect) { .width = ctx->src.width, .height = ctx->src.height };
		return 0;
	}
	return -EINVAL;
}

/*
 * Even origin and size (NV12 chroma), inside the frame. The origin goes into
 * the addresses; 0x03c stays 0.
 */
static int tdp_s_selection(struct file *file, void *priv, struct v4l2_selection *s)
{
	struct tdp_ctx *ctx = fh_to_ctx(file);
	struct v4l2_rect bounds = { .width = ctx->src.width, .height = ctx->src.height };
	struct v4l2_rect *r = &s->r;

	if (s->type != V4L2_BUF_TYPE_VIDEO_OUTPUT || s->target != V4L2_SEL_TGT_CROP)
		return -EINVAL;
	/* device_run reads the crop unlocked: it is fixed while OUTPUT buffers exist. */
	if (vb2_is_busy(v4l2_m2m_get_vq(ctx->fh.m2m_ctx, s->type)))
		return -EBUSY;
	r->left = clamp_t(s32, r->left, 0, bounds.width - TDP_MIN_W) & ~1;
	r->top = clamp_t(s32, r->top, 0, bounds.height - TDP_MIN_H) & ~1;
	r->width = tdp_clamp_scale(r->width, ctx->dst.width, TDP_MIN_W, bounds.width);
	r->height = tdp_clamp_scale(r->height, ctx->dst.height, TDP_MIN_H, bounds.height);
	v4l2_rect_map_inside(r, &bounds);
	ctx->crop = *r;
	return 0;
}

static const struct v4l2_ioctl_ops tdp_ioctl_ops = {
	.vidioc_querycap = tdp_querycap,
	.vidioc_enum_fmt_vid_out = tdp_enum_fmt_src,
	.vidioc_enum_fmt_vid_cap = tdp_enum_fmt_dst,
	.vidioc_g_fmt_vid_out = tdp_g_fmt_src,
	.vidioc_g_fmt_vid_cap = tdp_g_fmt_dst,
	.vidioc_try_fmt_vid_out = tdp_try_fmt_src,
	.vidioc_try_fmt_vid_cap = tdp_try_fmt_dst,
	.vidioc_s_fmt_vid_out = tdp_s_fmt_src,
	.vidioc_s_fmt_vid_cap = tdp_s_fmt_dst,
	.vidioc_g_selection = tdp_g_selection,
	.vidioc_s_selection = tdp_s_selection,
	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,
};

static u32 tdp_size(struct vb2_queue *vq)
{
	struct tdp_ctx *ctx = vb2_get_drv_priv(vq);

	return V4L2_TYPE_IS_OUTPUT(vq->type) ? ctx->src.sizeimage : ctx->dst.sizeimage;
}

static int tdp_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers, unsigned int *nplanes,
			   unsigned int sizes[], struct device *alloc_devs[])
{
	if (*nplanes)
		return sizes[0] < tdp_size(vq) ? -EINVAL : 0;
	*nplanes = 1;
	sizes[0] = tdp_size(vq);
	return 0;
}

/* The hardware reads and writes whole frames: a short buffer never reaches it. */
/* Progressive frames only, as vim2m: ANY means NONE, anything else is refused. */
static int tdp_buf_out_validate(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	if (vbuf->field == V4L2_FIELD_ANY)
		vbuf->field = V4L2_FIELD_NONE;
	return vbuf->field == V4L2_FIELD_NONE ? 0 : -EINVAL;
}

static int tdp_buf_prepare(struct vb2_buffer *vb)
{
	u32 size = tdp_size(vb->vb2_queue);

	if (vb2_plane_size(vb, 0) < size)
		return -EINVAL;
	if (V4L2_TYPE_IS_OUTPUT(vb->type))
		vb2_set_plane_payload(vb, 0, size);
	return 0;
}

static void tdp_buf_queue(struct vb2_buffer *vb)
{
	struct tdp_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(vb));
}

static int tdp_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct tdp_ctx *ctx = vb2_get_drv_priv(vq);

	if (V4L2_TYPE_IS_OUTPUT(vq->type))
		ctx->src_seq = 0;
	else
		ctx->dst_seq = 0;
	return 0;
}

static void tdp_stop_streaming(struct vb2_queue *vq)
{
	struct tdp_ctx *ctx = vb2_get_drv_priv(vq);
	struct vb2_v4l2_buffer *vb;

	while ((vb = V4L2_TYPE_IS_OUTPUT(vq->type) ? v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx) :
						      v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx)))
		v4l2_m2m_buf_done(vb, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops tdp_qops = {
	.queue_setup = tdp_queue_setup,
	.buf_out_validate = tdp_buf_out_validate,
	.buf_prepare = tdp_buf_prepare,
	.buf_queue = tdp_buf_queue,
	.start_streaming = tdp_start_streaming,
	.stop_streaming = tdp_stop_streaming,
};

static int tdp_queue_init(void *priv, struct vb2_queue *src, struct vb2_queue *dst)
{
	struct tdp_ctx *ctx = priv;
	struct vb2_queue *q[] = { src, dst };
	int i, ret;

	src->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	dst->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	for (i = 0; i < ARRAY_SIZE(q); i++) {
		q[i]->io_modes = VB2_MMAP | VB2_DMABUF;
		q[i]->drv_priv = ctx;
		q[i]->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
		q[i]->ops = &tdp_qops;
		q[i]->mem_ops = &vb2_dma_contig_memops;
		q[i]->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
		q[i]->lock = &ctx->tdp->lock;
		q[i]->dev = ctx->tdp->dev;
		ret = vb2_queue_init(q[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static int tdp_open(struct file *file)
{
	struct tdp_dev *tdp = video_drvdata(file);
	struct tdp_ctx *ctx;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->tdp = tdp;
	v4l2_fh_init(&ctx->fh, video_devdata(file));

	ctx->src = (struct v4l2_pix_format) { .width = 1920, .height = 1080 };
	ctx->sfmt = tdp_fill(&ctx->src, true);
	ctx->dst = ctx->src;
	ctx->dfmt = tdp_fill(&ctx->dst, false);
	ctx->crop = (struct v4l2_rect) { .width = ctx->src.width, .height = ctx->src.height };

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(tdp->m2m, ctx, tdp_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		v4l2_fh_exit(&ctx->fh);
		kfree(ctx);
		return ret;
	}
	v4l2_fh_add(&ctx->fh, file);
	return 0;
}

static int tdp_release(struct file *file)
{
	struct tdp_ctx *ctx = fh_to_ctx(file);

	mutex_lock(&ctx->tdp->lock);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	mutex_unlock(&ctx->tdp->lock);
	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return 0;
}

static const struct v4l2_file_operations tdp_fops = {
	.owner = THIS_MODULE,
	.open = tdp_open,
	.release = tdp_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

static int tdp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk_bulk_data *clks;
	struct tdp_dev *tdp;
	int ret;
	u32 id;

	tdp = devm_kzalloc(dev, sizeof(*tdp), GFP_KERNEL);
	if (!tdp)
		return -ENOMEM;
	tdp->dev = dev;
	mutex_init(&tdp->lock);
	INIT_DELAYED_WORK(&tdp->watchdog, tdp_watchdog);

	tdp->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(tdp->regs))
		return PTR_ERR(tdp->regs);
	tdp->irq = platform_get_irq(pdev, 0);
	if (tdp->irq < 0)
		return tdp->irq;
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	/* The vendor's order: resets released, then the clocks. */
	tdp->rst = devm_reset_control_array_get_exclusive(dev);
	if (IS_ERR(tdp->rst))
		return PTR_ERR(tdp->rst);
	ret = reset_control_assert(tdp->rst) ?: reset_control_deassert(tdp->rst);
	if (ret)
		return ret;
	/* No clocks would mean an unclocked register read, which hangs the bus. */
	ret = devm_clk_bulk_get_all_enabled(dev, &clks);
	if (ret <= 0)
		return dev_err_probe(dev, ret ?: -ENODEV, "no clocks\n");

	id = readl(tdp->regs + TDP_VERSION);
	dev_dbg(dev, "version %#x\n", id);
	if (id != TDP_VERSION_ID)
		return dev_err_probe(dev, -ENODEV, "unexpected version %#x\n", id);
	tdp_hw_init(tdp);
	/* All four status bits, so nothing left from before fires the handler. */
	tdp_wr(tdp, TDP_IRQ_CLEAR, 0xf);

	/* Threaded: an AXI error waits for the bus to drain before the reset. */
	ret = devm_request_threaded_irq(dev, tdp->irq, NULL, tdp_irq, IRQF_ONESHOT, TDP_NAME, tdp);
	if (ret)
		return ret;

	ret = v4l2_device_register(dev, &tdp->v4l2);
	if (ret)
		return ret;
	tdp->m2m = v4l2_m2m_init(&tdp_m2m_ops);
	if (IS_ERR(tdp->m2m)) {
		ret = PTR_ERR(tdp->m2m);
		goto err_v4l2;
	}
	tdp->vfd = (struct video_device) {
		.name = TDP_NAME,
		.fops = &tdp_fops,
		.ioctl_ops = &tdp_ioctl_ops,
		.release = video_device_release_empty,
		.lock = &tdp->lock,
		.v4l2_dev = &tdp->v4l2,
		.vfl_dir = VFL_DIR_M2M,
		.device_caps = V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING,
	};
	video_set_drvdata(&tdp->vfd, tdp);
	ret = video_register_device(&tdp->vfd, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_m2m;
	platform_set_drvdata(pdev, tdp);
	return 0;

err_m2m:
	v4l2_m2m_release(tdp->m2m);
err_v4l2:
	v4l2_device_unregister(&tdp->v4l2);
	return ret;
}

/* Unloading needs the node closed (its fops hold the module), so no job runs. */
static void tdp_remove(struct platform_device *pdev)
{
	struct tdp_dev *tdp = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&tdp->watchdog);
	video_unregister_device(&tdp->vfd);
	v4l2_m2m_release(tdp->m2m);
	v4l2_device_unregister(&tdp->v4l2);
}

static const struct of_device_id tdp_of_match[] = {
	{ .compatible = "axera,ax620e-tdp" },
	{ }
};
MODULE_DEVICE_TABLE(of, tdp_of_match);

static struct platform_driver tdp_driver = {
	.probe = tdp_probe,
	.remove = tdp_remove,
	.driver = {
		.name = TDP_NAME,
		.of_match_table = tdp_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(tdp_driver);

MODULE_DESCRIPTION("Axera AX620E TDP scaler");
MODULE_LICENSE("GPL");
