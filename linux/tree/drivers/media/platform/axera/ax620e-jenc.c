// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026, GembaHive, Inc.
 */
/*
 * Axera AX620E JPEG encoder: a VeriSilicon VC9000-family core behind a VCMD
 * command engine, as a V4L2 mem2mem device. Raw frames in, baseline JPEG out.
 *
 * The kernel writes the JPEG headers and builds every command buffer; user
 * space only queues buffers, so it never names a DMA address or a register.
 */

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-jpeg.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "ax620e-jenc-regs.h"

#define JENC_NAME	"ax620e-jenc"
#define JENC_MIN	96	/* the core's shortest row */
#define JENC_MAX	8192
/* log2 alignments: width 16 (MB columns), height 2 (NV12); rows reach 16 by overfill. */
#define JENC_W_ALIGN	4
#define JENC_H_ALIGN	1

/* The header is padded with a COM segment so the core writes the scan from an aligned address. */
#define JENC_HDR_SIZE	640
#define JENC_CMD_SIZE	SZ_4K
#define JENC_TIMEOUT_MS	1000

struct jenc_dev {
	struct v4l2_device v4l2;
	struct video_device vfd;
	struct v4l2_m2m_dev *m2m;
	struct mutex lock;
	struct device *dev;
	void __iomem *vcmd;
	struct reset_control *rst;
	u32 *cmd;
	dma_addr_t cmd_dma;
	/* Core registers the command buffer reads back after the frame. */
	u32 *status;
	dma_addr_t status_dma;
	struct delayed_work watchdog;
};

struct jenc_ctx {
	struct v4l2_fh fh;
	struct v4l2_ctrl_handler ctrls;
	struct jenc_dev *jenc;
	struct v4l2_pix_format src;
	struct v4l2_pix_format dst;
	u32 quality;
};

static struct jenc_ctx *fh_to_ctx(struct file *file)
{
	return container_of(file_to_v4l2_fh(file), struct jenc_ctx, fh);
}

struct jenc_cmd {
	u32 *p;
	unsigned int n;
};

static void cmd_put(struct jenc_cmd *c, u32 v)
{
	c->p[c->n++] = v;
}

/* Every opcode is 64-bit aligned. */
static void cmd_align(struct jenc_cmd *c)
{
	if (c->n & 1)
		cmd_put(c, 0);
}

static void cmd_wreg(struct jenc_cmd *c, u32 addr, unsigned int count, const u32 *v)
{
	cmd_put(c, VCMD_OP_WREG | count << 16 | addr);
	memcpy(c->p + c->n, v, count * 4);
	c->n += count;
	cmd_align(c);
}

static void cmd_rreg(struct jenc_cmd *c, u32 addr, unsigned int count, dma_addr_t dst)
{
	cmd_put(c, VCMD_OP_RREG | count << 16 | addr);
	cmd_put(c, lower_32_bits(dst));
	cmd_put(c, upper_32_bits(dst));
	cmd_put(c, 0);
}

static void cmd_end(struct jenc_cmd *c)
{
	cmd_put(c, VCMD_OP_END);
	cmd_put(c, 0);
}

/* VCMD starts from idle on every buffer: END leaves it idle again (ENDCMD). */
static void jenc_vcmd_run(struct jenc_dev *jenc, unsigned int words)
{
	void __iomem *v = jenc->vcmd;

	writel(VCMD_CTRL_MASTER_CLK_ON, v + VCMD_CTRL);
	writel(readl(v + VCMD_IRQ), v + VCMD_IRQ);
	writel(VCMD_IRQ_ALL, v + VCMD_IRQ_EN);
	writel(lower_32_bits(jenc->cmd_dma), v + VCMD_CMD_ADDR);
	writel(upper_32_bits(jenc->cmd_dma), v + VCMD_CMD_ADDR_MSB);
	writel(DIV_ROUND_UP(words, 2), v + VCMD_CMD_LEN);
	writel(VCMD_AXI_BURST(16), v + VCMD_AXI);
	writel(1, v + VCMD_RDY_COUNT);
	writel(~0U, v + VCMD_EXT_INT_GATE);
	writel(VCMD_CTRL_MASTER_CLK_ON | VCMD_CTRL_START, v + VCMD_CTRL);
}

static u8 *jpeg_marker(u8 *p, u8 marker, u16 len)
{
	*p++ = 0xff;
	*p++ = marker;
	*p++ = len >> 8;
	*p++ = len;
	return p;
}

static u8 *jpeg_table(u8 *p, u8 id, const u8 *t, size_t len)
{
	*p++ = id;
	memcpy(p, t, len);
	return p + len;
}

/*
 * libax_venc's scaler: IJG scaling of the Annex K tables, then large steps
 * rounded down to multiples of 2/4/8. */
static void jenc_scale(u8 *q, const u8 *ref, u32 quality)
{
	u32 scale = max(quality < 50 ? 5000 / quality : 200 - 2 * quality, 2U);
	int i;

	for (i = 0; i < V4L2_JPEG_PIXELS_IN_BLOCK; i++) {
		u32 v = DIV_ROUND_CLOSEST(ref[i] * scale, 100);

		if (!v)
			v = 1;
		else if (v > 255)
			v = 248;
		else if (v > 128)
			v &= ~7U;
		else if (v > 64)
			v &= ~3U;
		else if (v > 32)
			v &= ~1U;
		q[i] = v;
	}
}

/* Natural-order tables in; the header carries them in zigzag order. */
static void jenc_header(u8 *buf, const struct jenc_ctx *ctx, const u8 *lq, const u8 *cq)
{
	u8 *p = buf, zz[2][V4L2_JPEG_PIXELS_IN_BLOCK];
	int i;

	for (i = 0; i < V4L2_JPEG_PIXELS_IN_BLOCK; i++) {
		zz[0][i] = lq[v4l2_jpeg_zigzag_scan_index[i]];
		zz[1][i] = cq[v4l2_jpeg_zigzag_scan_index[i]];
	}

	*p++ = 0xff;
	*p++ = 0xd8;	/* SOI */

	p = jpeg_marker(p, 0xdb /* DQT */, 2 + 2 * 65);
	p = jpeg_table(p, 0, zz[0], 64);
	p = jpeg_table(p, 1, zz[1], 64);

	p = jpeg_marker(p, 0xc0 /* SOF0 */, 17);
	*p++ = 8;
	*p++ = ctx->dst.height >> 8;
	*p++ = ctx->dst.height;
	*p++ = ctx->dst.width >> 8;
	*p++ = ctx->dst.width;
	*p++ = 3;
	*p++ = 1;
	*p++ = 0x22;	/* 4:2:0 */
	*p++ = 0;
	*p++ = 2;
	*p++ = 0x11;
	*p++ = 1;
	*p++ = 3;
	*p++ = 0x11;
	*p++ = 1;

	p = jpeg_marker(p, 0xc4 /* DHT */, 2 + 4 + 2 * V4L2_JPEG_REF_HT_DC_LEN +
			2 * V4L2_JPEG_REF_HT_AC_LEN);
	p = jpeg_table(p, 0x00, v4l2_jpeg_ref_table_luma_dc_ht, V4L2_JPEG_REF_HT_DC_LEN);
	p = jpeg_table(p, 0x10, v4l2_jpeg_ref_table_luma_ac_ht, V4L2_JPEG_REF_HT_AC_LEN);
	p = jpeg_table(p, 0x01, v4l2_jpeg_ref_table_chroma_dc_ht, V4L2_JPEG_REF_HT_DC_LEN);
	p = jpeg_table(p, 0x11, v4l2_jpeg_ref_table_chroma_ac_ht, V4L2_JPEG_REF_HT_AC_LEN);

	/* COM pads to JENC_HDR_SIZE; SOS (14 bytes) closes the header. */
	i = JENC_HDR_SIZE - 14 - (p - buf);
	p = jpeg_marker(p, 0xfe /* COM */, i - 2);
	memset(p, 0, i - 4);
	p += i - 4;

	p = jpeg_marker(p, 0xda /* SOS */, 12);
	*p++ = 3;
	*p++ = 1;
	*p++ = 0x00;
	*p++ = 2;
	*p++ = 0x11;
	*p++ = 3;
	*p++ = 0x11;
	*p++ = 0;
	*p++ = 63;
	*p++ = 0;
	WARN_ON(p - buf != JENC_HDR_SIZE);
}

/* The core's quantiser order (VeriSilicon's, as hantro's hw_reorder): column pairs, not zigzag. */
static const u8 jenc_qorder[V4L2_JPEG_PIXELS_IN_BLOCK] = {
	 0,  8, 16, 24,  1,  9, 17, 25, 32, 40, 48, 56, 33, 41, 49, 57,
	 2, 10, 18, 26,  3, 11, 19, 27, 34, 42, 50, 58, 35, 43, 51, 59,
	 4, 12, 20, 28,  5, 13, 21, 29, 36, 44, 52, 60, 37, 45, 53, 61,
	 6, 14, 22, 30,  7, 15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63,
};

/*
 * libax_venc's frame: swreg1..511 in one burst, the AX register at 0x2800,
 * then swreg5 again with the enable bit, and a STALL until the core's
 * interrupt. Registers it leaves at zero stay zero here.
 */
static void jenc_program(struct jenc_cmd *c, const struct jenc_ctx *ctx, dma_addr_t src,
			 dma_addr_t strm, u32 strm_size, const u8 *lq, const u8 *cq)
{
	const struct v4l2_pix_format *f = &ctx->src;
	u32 h = ALIGN(f->height, 16), fill = h - f->height;
	static const u32 ax_2800 = 0x44;
	u32 kick, *r;
	int i;

	cmd_put(c, VCMD_OP_WREG | (JENC_REGS - 1) << 16 | JENC_SWREG(1));
	r = c->p + c->n - 1;	/* r[n] is swreg n */
	memset(r + 1, 0, (JENC_REGS - 1) * 4);
	c->n += JENC_REGS - 1;
	cmd_align(c);

	/* Status bits stay 0: on older cores a 1 sets them rather than clearing. */
	r[JENC_REG_IRQ] = JENC_IRQ_TIMEOUT_EN;
	/*
	 * swreg2: the JPEG path forces the ROI-map swap to 0xf, stream/picture
	 * swaps 0. swreg3/81: clock gating and burst as the vendor's VENC frames.
	 */
	r[JENC_REG_AXI_SWAP] = 0xf0;
	r[JENC_REG_AXI_CLK] = 0xff000000;
	r[JENC_REG_MAX_BURST] = 16 << 24;
	r[JENC_REG_AXI_WR_OUT] = 64 << 6;
	r[JENC_REG_AXI_RD_OUT] = 64 << 4;

	r[JENC_REG_MODE] = JENC_MODE_JPEG;
	r[JENC_REG_PIC] = JENC_PIC_WIDTH8(f->width) | JENC_PIC_HEIGHT8(h);
	r[JENC_REG_STRM_BASE] = lower_32_bits(strm);
	r[JENC_REG_STRM_LIMIT] = strm_size;
	r[JENC_REG_ROWLEN] = JENC_ROWLEN(f->width);

	/* Rows past the frame are overfill: the core pads them instead of reading. */
	r[JENC_REG_IN_FMT] = JENC_IN_YFILL(fill);
	r[JENC_REG_FILL_MSB] = JENC_FILL_YMSB(fill);
	r[JENC_REG_IN_Y] = lower_32_bits(src);
	/* In pixels: the vendor's YUYV VENC frames at 1920 wide carry 1920 here, chroma 0. */
	r[JENC_REG_LU_STRIDE] = JENC_STRIDE(f->width);
	if (f->pixelformat == V4L2_PIX_FMT_NV12) {
		r[JENC_REG_IN_FMT] |= JENC_IN_FMT_NV12;
		r[JENC_REG_IN_CB] = lower_32_bits(src + f->bytesperline * f->height);
		r[JENC_REG_CH_STRIDE] = JENC_STRIDE(f->width);
	} else {
		r[JENC_REG_IN_FMT] |= JENC_IN_FMT_YUYV;
		r[JENC_REG_IN_CB] = lower_32_bits(src);
	}
	r[JENC_REG_IN_CR] = r[JENC_REG_IN_CB];

	for (i = 0; i < V4L2_JPEG_PIXELS_IN_BLOCK; i++) {
		r[JENC_REG_QUANT_LUMA + i / 4] |= (u32)lq[jenc_qorder[i]] << (24 - 8 * (i % 4));
		r[JENC_REG_QUANT_LUMA + 16 + i / 4] |= (u32)cq[jenc_qorder[i]] << (24 - 8 * (i % 4));
	}
	memcpy(r + JENC_REG_QUANT_NONROI, r + JENC_REG_QUANT_LUMA, 32 * 4);

	cmd_wreg(c, JENC_AX_2800, 1, &ax_2800);
	kick = r[JENC_REG_PIC] | JENC_PIC_E;
	cmd_wreg(c, JENC_SWREG(JENC_REG_PIC), 1, &kick);
	cmd_put(c, VCMD_OP_STALL | 1);
	cmd_put(c, 0);
}

/* After the read-back: acknowledge the core as libax_venc does (swreg5, then swreg1), and stop. */
static void jenc_program_end(struct jenc_cmd *c)
{
	cmd_put(c, VCMD_OP_CLRINT | JENC_SWREG(JENC_REG_PIC));
	cmd_put(c, 0xfffffffe);
	cmd_put(c, VCMD_OP_CLRINT | JENC_SWREG(JENC_REG_IRQ));
	cmd_put(c, 0xffffffff);
	cmd_end(c);
}

static void jenc_device_run(void *priv)
{
	struct jenc_ctx *ctx = priv;
	struct jenc_dev *jenc = ctx->jenc;
	struct vb2_v4l2_buffer *src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	struct vb2_v4l2_buffer *dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	u8 lq[V4L2_JPEG_PIXELS_IN_BLOCK], cq[V4L2_JPEG_PIXELS_IN_BLOCK];
	struct jenc_cmd c = { .p = jenc->cmd };

	jenc_scale(lq, v4l2_jpeg_ref_table_luma_qt, ctx->quality);
	jenc_scale(cq, v4l2_jpeg_ref_table_chroma_qt, ctx->quality);
	jenc_header(vb2_plane_vaddr(&dst->vb2_buf, 0), ctx, lq, cq);

	jenc_program(&c, ctx, vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0),
		     vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 0) + JENC_HDR_SIZE,
		     vb2_plane_size(&dst->vb2_buf, 0) - JENC_HDR_SIZE, lq, cq);
	cmd_rreg(&c, JENC_SWREG(0), JENC_STATUS_REGS, jenc->status_dma);
	jenc_program_end(&c);

	v4l2_m2m_buf_copy_metadata(src, dst, true);
	schedule_delayed_work(&jenc->watchdog, msecs_to_jiffies(JENC_TIMEOUT_MS));
	jenc_vcmd_run(jenc, c.n);
}

/* vcmd_irq: what VCMD raised for the job, 0 when it raised nothing. */
static void jenc_finish(struct jenc_dev *jenc, u32 vcmd_irq)
{
	struct jenc_ctx *ctx = v4l2_m2m_get_curr_priv(jenc->m2m);
	u32 core_irq = jenc->status[JENC_REG_IRQ];
	struct vb2_v4l2_buffer *src, *dst;
	bool ok = vcmd_irq == VCMD_IRQ_ENDCMD && (core_irq & JENC_IRQ_FRAME_RDY) &&
		  !(core_irq & JENC_IRQ_ERRORS);
	u32 bytes = 0;

	src = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
	if (ok) {
		bytes = min_t(u32, jenc->status[JENC_REG_STRM_LIMIT],
			      vb2_plane_size(&dst->vb2_buf, 0) - JENC_HDR_SIZE);
	} else {
		dev_err(jenc->dev, "frame failed: vcmd irq %#x, core irq %#x\n", vcmd_irq, core_irq);
		/* As the vendor driver does on a command error: the block, VCMD included, back to reset state. */
		reset_control_assert(jenc->rst);
		reset_control_deassert(jenc->rst);
	}

	vb2_set_plane_payload(&dst->vb2_buf, 0, JENC_HDR_SIZE + bytes);
	if (v4l2_m2m_is_last_draining_src_buf(ctx->fh.m2m_ctx, src)) {
		dst->flags |= V4L2_BUF_FLAG_LAST;
		v4l2_m2m_mark_stopped(ctx->fh.m2m_ctx);
	}
	v4l2_m2m_buf_done(src, ok ? VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR);
	v4l2_m2m_buf_done(dst, ok ? VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR);
	v4l2_m2m_job_finish(jenc->m2m, ctx->fh.m2m_ctx);
}

static irqreturn_t jenc_irq(int irq, void *data)
{
	struct jenc_dev *jenc = data;
	u32 st = readl(jenc->vcmd + VCMD_IRQ);

	if (!st)
		return IRQ_NONE;
	writel(st, jenc->vcmd + VCMD_IRQ);
	/* The watchdog already finished this job. */
	if (!cancel_delayed_work(&jenc->watchdog))
		return IRQ_HANDLED;
	jenc_finish(jenc, st);
	return IRQ_HANDLED;
}

static void jenc_watchdog(struct work_struct *work)
{
	struct jenc_dev *jenc = container_of(work, struct jenc_dev, watchdog.work);

	dev_err(jenc->dev, "no interrupt\n");
	jenc_finish(jenc, 0);
}

static const struct v4l2_m2m_ops jenc_m2m_ops = {
	.device_run = jenc_device_run,
};

static bool jenc_src_fourcc(u32 fourcc)
{
	return fourcc == V4L2_PIX_FMT_YUYV || fourcc == V4L2_PIX_FMT_NV12;
}

static void jenc_fill_src(struct v4l2_pix_format *f)
{
	if (!jenc_src_fourcc(f->pixelformat))
		f->pixelformat = V4L2_PIX_FMT_YUYV;
	v4l_bound_align_image(&f->width, JENC_MIN, JENC_MAX, JENC_W_ALIGN,
			      &f->height, JENC_MIN, JENC_MAX, JENC_H_ALIGN, 0);
	f->field = V4L2_FIELD_NONE;
	f->colorspace = V4L2_COLORSPACE_SRGB;
	f->xfer_func = V4L2_XFER_FUNC_DEFAULT;
	f->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	f->quantization = V4L2_QUANTIZATION_DEFAULT;
	if (f->pixelformat == V4L2_PIX_FMT_YUYV) {
		f->bytesperline = f->width * 2;
		f->sizeimage = f->bytesperline * f->height;
	} else {
		f->bytesperline = f->width;
		f->sizeimage = f->width * f->height * 3 / 2;
	}
}

/* The capture side follows the source size, with a byte a pixel of room. */
static void jenc_fill_dst(struct jenc_ctx *ctx)
{
	struct v4l2_pix_format *f = &ctx->dst;

	f->pixelformat = V4L2_PIX_FMT_JPEG;
	f->width = ctx->src.width;
	f->height = ctx->src.height;
	f->field = V4L2_FIELD_NONE;
	f->colorspace = V4L2_COLORSPACE_JPEG;
	f->bytesperline = 0;
	f->sizeimage = JENC_HDR_SIZE + f->width * f->height;
}

static int jenc_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	strscpy(cap->driver, JENC_NAME, sizeof(cap->driver));
	strscpy(cap->card, "AX620E JPEG encoder", sizeof(cap->card));
	return 0;
}

static int jenc_enum_fmt_src(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	static const u32 fourcc[] = { V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_NV12 };

	if (f->index >= ARRAY_SIZE(fourcc))
		return -EINVAL;
	f->pixelformat = fourcc[f->index];
	return 0;
}

static int jenc_enum_fmt_dst(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;
	f->pixelformat = V4L2_PIX_FMT_JPEG;
	return 0;
}

static int jenc_g_fmt_src(struct file *file, void *priv, struct v4l2_format *f)
{
	f->fmt.pix = fh_to_ctx(file)->src;
	return 0;
}

static int jenc_g_fmt_dst(struct file *file, void *priv, struct v4l2_format *f)
{
	f->fmt.pix = fh_to_ctx(file)->dst;
	return 0;
}

static int jenc_try_fmt_src(struct file *file, void *priv, struct v4l2_format *f)
{
	jenc_fill_src(&f->fmt.pix);
	return 0;
}

static int jenc_s_fmt_src(struct file *file, void *priv, struct v4l2_format *f)
{
	struct jenc_ctx *ctx = fh_to_ctx(file);

	if (vb2_is_busy(v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type)) ||
	    vb2_is_busy(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx)))
		return -EBUSY;
	jenc_fill_src(&f->fmt.pix);
	ctx->src = f->fmt.pix;
	jenc_fill_dst(ctx);
	return 0;
}

static int jenc_encoder_cmd(struct file *file, void *priv, struct v4l2_encoder_cmd *ec)
{
	struct jenc_ctx *ctx = fh_to_ctx(file);
	struct v4l2_m2m_ctx *m2m_ctx = ctx->fh.m2m_ctx;
	int ret;

	ret = v4l2_m2m_ioctl_try_encoder_cmd(file, priv, ec);
	if (ret)
		return ret;
	/* Draining needs both queues streaming; otherwise there is nothing to flush. */
	if (!vb2_is_streaming(v4l2_m2m_get_src_vq(m2m_ctx)) ||
	    !vb2_is_streaming(v4l2_m2m_get_dst_vq(m2m_ctx)))
		return 0;
	/* The frame in flight completes first, so it cannot miss the drain state. */
	v4l2_m2m_suspend(ctx->jenc->m2m);
	ret = v4l2_m2m_encoder_cmd(file, m2m_ctx, ec);
	v4l2_m2m_resume(ctx->jenc->m2m);
	if (!ret && ec->cmd == V4L2_ENC_CMD_START)
		vb2_clear_last_buffer_dequeued(v4l2_m2m_get_dst_vq(m2m_ctx));
	return ret;
}

static const struct v4l2_ioctl_ops jenc_ioctl_ops = {
	.vidioc_querycap = jenc_querycap,
	.vidioc_enum_fmt_vid_out = jenc_enum_fmt_src,
	.vidioc_enum_fmt_vid_cap = jenc_enum_fmt_dst,
	.vidioc_g_fmt_vid_out = jenc_g_fmt_src,
	.vidioc_g_fmt_vid_cap = jenc_g_fmt_dst,
	.vidioc_try_fmt_vid_out = jenc_try_fmt_src,
	.vidioc_try_fmt_vid_cap = jenc_g_fmt_dst,
	.vidioc_s_fmt_vid_out = jenc_s_fmt_src,
	.vidioc_s_fmt_vid_cap = jenc_g_fmt_dst,
	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,
	.vidioc_try_encoder_cmd = v4l2_m2m_ioctl_try_encoder_cmd,
	.vidioc_encoder_cmd = jenc_encoder_cmd,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static int jenc_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers, unsigned int *nplanes,
			    unsigned int sizes[], struct device *alloc_devs[])
{
	struct jenc_ctx *ctx = vb2_get_drv_priv(vq);
	u32 size = V4L2_TYPE_IS_OUTPUT(vq->type) ? ctx->src.sizeimage : ctx->dst.sizeimage;

	if (*nplanes)
		return sizes[0] < size ? -EINVAL : 0;
	*nplanes = 1;
	sizes[0] = size;
	return 0;
}

static int jenc_buf_prepare(struct vb2_buffer *vb)
{
	struct jenc_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	if (V4L2_TYPE_IS_OUTPUT(vb->type)) {
		if (vb2_plane_size(vb, 0) < ctx->src.sizeimage)
			return -EINVAL;
		vb2_set_plane_payload(vb, 0, ctx->src.sizeimage);
	} else {
		if (vb2_plane_size(vb, 0) < ctx->dst.sizeimage)
			return -EINVAL;
		/* Also for a buffer the drain returns empty. */
		to_vb2_v4l2_buffer(vb)->field = V4L2_FIELD_NONE;
	}
	return 0;
}

static void jenc_buf_queue(struct vb2_buffer *vb)
{
	struct jenc_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	/* A drain that found no frame left ends on the next capture buffer, empty. */
	if (V4L2_TYPE_IS_CAPTURE(vb->type) && vb2_is_streaming(vb->vb2_queue) &&
	    v4l2_m2m_dst_buf_is_last(ctx->fh.m2m_ctx)) {
		vb2_set_plane_payload(vb, 0, 0);
		v4l2_m2m_last_buffer_done(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(vb));
		return;
	}
	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(vb));
}

static void jenc_stop_streaming(struct vb2_queue *vq)
{
	struct jenc_ctx *ctx = vb2_get_drv_priv(vq);
	struct vb2_v4l2_buffer *vb;

	while ((vb = V4L2_TYPE_IS_OUTPUT(vq->type) ? v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx) :
						      v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx)))
		v4l2_m2m_buf_done(vb, VB2_BUF_STATE_ERROR);
	v4l2_m2m_update_stop_streaming_state(ctx->fh.m2m_ctx, vq);
}

static const struct vb2_ops jenc_qops = {
	.queue_setup = jenc_queue_setup,
	.buf_prepare = jenc_buf_prepare,
	.buf_queue = jenc_buf_queue,
	.stop_streaming = jenc_stop_streaming,
};

static int jenc_queue_init(void *priv, struct vb2_queue *src, struct vb2_queue *dst)
{
	struct jenc_ctx *ctx = priv;
	struct vb2_queue *q[] = { src, dst };
	int i, ret;

	/* Frames come as capture dma-bufs; the CPU writes each JPEG header, so ours to map. */
	src->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	src->io_modes = VB2_MMAP | VB2_DMABUF;
	dst->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dst->io_modes = VB2_MMAP;
	for (i = 0; i < ARRAY_SIZE(q); i++) {
		q[i]->drv_priv = ctx;
		q[i]->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
		q[i]->ops = &jenc_qops;
		q[i]->mem_ops = &vb2_dma_contig_memops;
		q[i]->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
		q[i]->lock = &ctx->jenc->lock;
		q[i]->dev = ctx->jenc->dev;
		ret = vb2_queue_init(q[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static int jenc_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct jenc_ctx *ctx = container_of(ctrl->handler, struct jenc_ctx, ctrls);

	ctx->quality = ctrl->val;
	return 0;
}

static const struct v4l2_ctrl_ops jenc_ctrl_ops = {
	.s_ctrl = jenc_s_ctrl,
};

static int jenc_open(struct file *file)
{
	struct jenc_dev *jenc = video_drvdata(file);
	struct jenc_ctx *ctx;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->jenc = jenc;
	v4l2_fh_init(&ctx->fh, video_devdata(file));

	v4l2_ctrl_handler_init(&ctx->ctrls, 1);
	v4l2_ctrl_new_std(&ctx->ctrls, &jenc_ctrl_ops, V4L2_CID_JPEG_COMPRESSION_QUALITY,
			  1, 100, 1, 80);
	ret = ctx->ctrls.error;
	if (ret)
		goto err;
	ctx->fh.ctrl_handler = &ctx->ctrls;
	v4l2_ctrl_handler_setup(&ctx->ctrls);

	ctx->src.width = 1920;
	ctx->src.height = 1080;
	jenc_fill_src(&ctx->src);
	jenc_fill_dst(ctx);

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(jenc->m2m, ctx, jenc_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err;
	}
	v4l2_fh_add(&ctx->fh, file);
	return 0;

err:
	v4l2_ctrl_handler_free(&ctx->ctrls);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return ret;
}

static int jenc_release(struct file *file)
{
	struct jenc_ctx *ctx = fh_to_ctx(file);

	mutex_lock(&ctx->jenc->lock);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	mutex_unlock(&ctx->jenc->lock);
	v4l2_ctrl_handler_free(&ctx->ctrls);
	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return 0;
}

static const struct v4l2_file_operations jenc_fops = {
	.owner = THIS_MODULE,
	.open = jenc_open,
	.release = jenc_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

/* One read-back through VCMD before the interrupt is wired: the core answers or probe fails. */
static int jenc_identify(struct jenc_dev *jenc, u32 vcmd_id)
{
	struct jenc_cmd c = { .p = jenc->cmd };
	u32 st;
	int ret;

	cmd_rreg(&c, JENC_SWREG(0), JENC_STATUS_REGS, jenc->status_dma);
	cmd_end(&c);
	jenc_vcmd_run(jenc, c.n);
	ret = readl_poll_timeout(jenc->vcmd + VCMD_IRQ, st, st, 10, 100000);
	/* Masked until device_run: devm_request_irq must not find it enabled. */
	writel(0, jenc->vcmd + VCMD_IRQ_EN);
	writel(st, jenc->vcmd + VCMD_IRQ);
	if (ret || st != VCMD_IRQ_ENDCMD) {
		/* VCMD may still be writing status_dma, which devm frees next: stop it. */
		reset_control_assert(jenc->rst);
		return dev_err_probe(jenc->dev, -EIO, "VCMD read-back: irq %#x\n", st);
	}
	dev_info(jenc->dev, "VCMD %#x, core id %#010x, config %#010x\n", vcmd_id,
		 jenc->status[0], jenc->status[JENC_REG_HW_CFG]);
	if (!(jenc->status[JENC_REG_HW_CFG] & JENC_HW_CFG_JPEG))
		return dev_err_probe(jenc->dev, -ENODEV, "core without JPEG\n");
	return 0;
}

static int jenc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct jenc_dev *jenc;
	struct clk *clk;
	u32 id;
	int irq, ret;

	jenc = devm_kzalloc(dev, sizeof(*jenc), GFP_KERNEL);
	if (!jenc)
		return -ENOMEM;
	jenc->dev = dev;
	mutex_init(&jenc->lock);
	INIT_DELAYED_WORK(&jenc->watchdog, jenc_watchdog);

	jenc->vcmd = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(jenc->vcmd))
		return PTR_ERR(jenc->vcmd);
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	jenc->cmd = dmam_alloc_coherent(dev, JENC_CMD_SIZE, &jenc->cmd_dma, GFP_KERNEL);
	jenc->status = dmam_alloc_coherent(dev, JENC_STATUS_REGS * 4, &jenc->status_dma,
					   GFP_KERNEL);
	if (!jenc->cmd || !jenc->status)
		return -ENOMEM;

	/* The vendor's order: reset pulse with the clock still off, then the clock for good. */
	jenc->rst = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(jenc->rst))
		return PTR_ERR(jenc->rst);
	ret = reset_control_assert(jenc->rst) ?: reset_control_deassert(jenc->rst);
	if (ret)
		return ret;
	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	id = readl(jenc->vcmd + VCMD_HWID);
	if (id >> 16 != VCMD_HWID_MAGIC)
		return dev_err_probe(dev, -ENODEV, "no VCMD: id %#x\n", id);
	ret = jenc_identify(jenc, id);
	if (ret)
		return ret;

	ret = devm_request_irq(dev, irq, jenc_irq, 0, JENC_NAME, jenc);
	if (ret)
		return ret;

	ret = v4l2_device_register(dev, &jenc->v4l2);
	if (ret)
		return ret;
	jenc->m2m = v4l2_m2m_init(&jenc_m2m_ops);
	if (IS_ERR(jenc->m2m)) {
		ret = PTR_ERR(jenc->m2m);
		goto err_v4l2;
	}
	jenc->vfd = (struct video_device) {
		.name = JENC_NAME,
		.fops = &jenc_fops,
		.ioctl_ops = &jenc_ioctl_ops,
		.release = video_device_release_empty,
		.lock = &jenc->lock,
		.v4l2_dev = &jenc->v4l2,
		.vfl_dir = VFL_DIR_M2M,
		.device_caps = V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING,
	};
	video_set_drvdata(&jenc->vfd, jenc);
	ret = video_register_device(&jenc->vfd, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_m2m;
	platform_set_drvdata(pdev, jenc);
	return 0;

err_m2m:
	v4l2_m2m_release(jenc->m2m);
err_v4l2:
	v4l2_device_unregister(&jenc->v4l2);
	return ret;
}

/* Unloading needs the node closed (its fops hold the module), so no job runs. */
static void jenc_remove(struct platform_device *pdev)
{
	struct jenc_dev *jenc = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&jenc->watchdog);
	video_unregister_device(&jenc->vfd);
	v4l2_m2m_release(jenc->m2m);
	v4l2_device_unregister(&jenc->v4l2);
}

static const struct of_device_id jenc_of_match[] = {
	{ .compatible = "axera,ax620e-jenc" },
	{ }
};
MODULE_DEVICE_TABLE(of, jenc_of_match);

static struct platform_driver jenc_driver = {
	.probe = jenc_probe,
	.remove = jenc_remove,
	.driver = {
		.name = JENC_NAME,
		.of_match_table = jenc_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(jenc_driver);

MODULE_DESCRIPTION("Axera AX620E JPEG encoder");
MODULE_LICENSE("GPL");
