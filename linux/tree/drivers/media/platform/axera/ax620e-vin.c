// SPDX-License-Identifier: GPL-2.0
/*
 * Axera AX620E capture: the MIPI CSI-2 receiver (controller 0) and its D-PHY,
 * then the SIF front end and the IFE in bypass (no ISP processing), written
 * to memory by one IFE write-DMA channel. One V4L2 capture node, whole YUYV
 * frames. The bridge is not a V4L2 subdev; userspace sets the format to the
 * source's, which the bridge reports.
 *
 * Register write order matters: enable and shadow-load bits go last.
 *
 * Buffers come from the default CMA area (no IOMMU: the WDMA takes the bus
 * address). isp_sys_glb (0x2500000) is a syscon: every write there is a
 * SET/CLR or VALUE/MASK strobe, never a read-modify-write.
 *
 * Copyright (c) 2026 the open-nanokvm-pro contributors.
 * Copyright (c) 2026, GembaHive, Inc.
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/spinlock.h>

#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>

#include "ax620e-vin-regs.h"

/* VIN interrupt banks: ten groups each, 0x10 apart. */
#define INT_GROUPS		10
#define INT_ENABLE(n)		(0x10 * (n) + 0x10)
#define INT_CLEAR(n)		(0x10 * (n) + 0x14)	/* W1C */
#define INT_MASKED(n)		(0x10 * (n) + 0x1c)
#define INT1_ENABLE(n)		(0x10 * (n) + 0xb0)
#define INT1_CLEAR(n)		(0x10 * (n) + 0xb4)
#define INT_GRP_FDONE		4			/* write-DMA frame done */

/* The write-DMA channel for the packed plane; its frame-done is bit chn + 1. */
#define WDMA_CHN		8
#define INT_FDONE		BIT(WDMA_CHN + 1)
#define WDMA_TRIG		(0x14000 + 0x18 * WDMA_CHN + 0x0c)	/* bit 0: shadow load */
#define WDMA_SEQ		(0x14000 + 0x18 * WDMA_CHN + 0x10)	/* low 16: token */
#define WDMA_ADDR		(0x14000 + 0x18 * WDMA_CHN + 0x14)	/* bus address >> 3 */
#define WDMA_ENABLE		(0x14000 + 0x18 * WDMA_CHN + 0x1c)

/* SIF device 0 */
#define SIF_START		0x6404
#define SIF_STOP		0x6408
#define SIF_VBUS_CTRL0		0x640c
#define SIF_CTRL		0x6500
#define SIF_ID			0x6504

/* ISP top: the module gate (status resets to all-ones), the data-source mux. */
#define TOP_GATE_CLR		0x158
#define TOP_GATE_BYPASS		0x8007
#define TOP_MUX_EN		0x170
#define IFE_GO			0x146dc
#define IFE_FLUSH		0x146e0

/* AXI masters quiesced around the reset sweep: IFE, ITP, YUV. */
static const u32 axi_quiesce[][2] = {	/* ctrl, status */
	{ 0x00184, 0x00188 }, { 0x80144, 0x80148 }, { 0xc0148, 0xc014c },
};

/* CSI-2 protocol controller 0 (reg "csi") */
#define CSI_LANE_CONTROL		0x08	/* lane map + combo mode */
#define CSI_ERR_IRQ_MASK_A		0x1c
#define CSI_DPHY_ERR_IRQ_MASK		0x24
#define CSI_ERR_STATUS0			0x28	/* W1C */
#define CSI_ERR_IRQS_MASK		0x2c
#define CSI_N_LANES			0x40
#define CSI_ERR_STATUS1			0x4c	/* W1C: per-lane ErrSotHS */
#define CSI_INFO_IRQS_MASK		0x50
#define CSI_STREAM_CTRL			0x100
#define CSI_STREAM_START		BIT(0)
#define CSI_STREAM_STOP			BIT(1)
#define CSI_STREAM_SRST			BIT(4)
#define CSI_MONITOR_CTRL		0x10c
#define CSI_MONITOR			0x110
#define CSI_MONITOR_LB			0x118

/* Four lanes on one PHY: "combo" 4 in the register (the SDK's AX_LANE_COMBO_MODE_0). */
#define CSI_LANE_CONTROL_MASK		0x77770710
#define CSI_LANE_CONTROL_4LANE		(0x43210010 | (4 << 8))
#define CSI_N_LANES_4			(0xf | 0x10)	/* lane-enable mask | 0x10 */

/*
 * D-PHY global file (reg "dphy"). Config words are write-only SET (0xc0 + 8k)
 * / CLR (+4) pairs; the vendor idiom is CLR(field) then SET(value).
 */
#define DPHY_LANE_CFG_SET		0xc8
#define DPHY_LANE_CFG_CLR		0xcc
#define DPHY_SWAP_FIELD			0x7
#define DPHY_DPDN_SWAP			GENMASK(5, 0)
#define DPHY_DATABUS16_SEL		BIT(24)
#define DPHY_1D2C_EN			BIT(25)
#define DPHY_MODE_SET			0xd0
#define DPHY_MODE_CLR			0xd4
#define DPHY_MODE_MIPI			0x820
#define DPHY_PRE_TIME_SET		0xd8	/* four 8-bit HS-RX pre-times */
#define DPHY_PRE_TIME_CLR		0xdc
#define DPHY_PRE_TIME			8
#define DPHY_DESKEW_DBG_CLR		0xec
#define DPHY_DESKEW_4LANE		0x3c00
#define DPHY_PHY_EN			0x110	/* write 1 to enable a lane */
#define DPHY_PHY_DISABLE		0x114
#define DPHY_ALL_LANES			0x3f
#define DPHY_DATA_LANES			0x3c

#define NUM_DATA_LANES			4
#define NUM_PHY_LANES			6	/* d0-d3, c0, c1 */

/* Swap-field shift of each logical lane: d0..d3, c0, c1. */
static const u8 dphy_swap_shift[NUM_PHY_LANES] = { 21, 18, 15, 12, 9, 6 };

/* isp_sys_glb, through the syscon */
#define ISP_CLK_MUX		0x00	/* [10:2] the VIN mux, [1:0] the CSI-2 link lock */
#define ISP_RST0_STATUS		0x0c
#define ISP_RST1_STATUS		0x10
#define ISP_CLK_MUX_SET		0xc8
#define ISP_CLK_MUX_CLR		0xcc
#define ISP_CLK_EB0_SET		0xd0
#define ISP_CLK_EB0_CLR		0xd4
#define ISP_CFG_PHY_CLK		BIT(0)
#define ISP_DPHY_RX_REF_CLK	BIT(1)
#define ISP_CLK_EB1_SET		0xd8
#define ISP_SYS_PIXEL_CLK	BIT(4)
#define ISP_CSIRX_PCLK		BIT(7)
#define ISP_RST0_ASSERT		0xe0
#define ISP_RST0_DEASSERT	0xe4
#define ISP_RST_PIXEL		BIT(2)
#define ISP_RST_PPI_RX_BYTE	BIT(3)
#define ISP_RST_PRESETN		BIT(4)
#define ISP_RST_SYS		BIT(5)
#define ISP_RST_DESKEW0		BIT(10)
#define ISP_RST_DESKEW1		BIT(11)
#define ISP_RST_DPHYRX		BIT(12)
/* The receiver's own lines; pulsing them would kill a locked link. */
#define ISP_RST0_CSI2RX		0x00001c3c
#define ISP_RST1_ASSERT		0xe8
#define ISP_RST1_DEASSERT	0xec
#define ISP_RST1_MUX_KICK	BIT(20)
#define ISP_CSI_CTRL_SEL_VAL	0x218
#define ISP_CSI_CTRL_SEL_MASK	0x21c

#define MIN_W			64U
#define MAX_W			4096U
#define MIN_H			64U
#define MAX_H			2400U

/* At least three frames at 24p: the one in flight, the one loaded behind it, slack. */
#define STOP_IDLE_MS		150

struct vin_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct vin {
	struct device *dev;
	void __iomem *regs;
	void __iomem *csi;
	void __iomem *dphy;
	struct regmap *isp;
	struct clk *tlb_clk;
	struct reset_control *tlb_rst, *pwr_off, *pwr_ready;

	/* Physical lane slot of each logical lane, from the endpoint. */
	u8 lane_swap[NUM_PHY_LANES];

	struct v4l2_device v4l2_dev;
	struct video_device vdev;

	struct mutex lock;		/* ioctls and the queue */
	struct vb2_queue queue;
	struct v4l2_pix_format fmt;

	spinlock_t irqlock;		/* buf_list, active, sequence, stopping */
	struct list_head buf_list;
	struct vin_buffer *active;	/* what the WDMA writes */
	u32 sequence;
	bool stopping;			/* the next frame-done loads nothing */
	struct completion idle;		/* the stopping frame-done ran */
	int irq;
};

static u32 vin_rd(struct vin *vin, u32 off)
{
	return readl(vin->regs + off);
}

static void vin_wr(struct vin *vin, u32 off, u32 val)
{
	writel(val, vin->regs + off);
}

static void vin_rmw(struct vin *vin, u32 off, u32 keep, u32 set)
{
	vin_wr(vin, off, (vin_rd(vin, off) & keep) | set);
}

static u32 isp_rd(struct vin *vin, u32 off)
{
	unsigned int val = 0;

	regmap_read(vin->isp, off, &val);
	return val;
}

static void isp_wr(struct vin *vin, u32 off, u32 val)
{
	regmap_write(vin->isp, off, val);
}

static void csi_rmw(struct vin *vin, u32 off, u32 mask, u32 val)
{
	writel((readl(vin->csi + off) & ~mask) | (val & mask), vin->csi + off);
}

static void dphy_wr(struct vin *vin, u32 off, u32 val)
{
	writel(val, vin->dphy + off);
}

/* Lane swap, timing and mode; ends by releasing the D-PHY reset. */
static void csi_dphy_init(struct vin *vin)
{
	unsigned int i;

	/* csi_ctrl_sel: four lanes on one PHY for controller 0. */
	isp_wr(vin, ISP_CSI_CTRL_SEL_MASK, 0x3);
	isp_wr(vin, ISP_CSI_CTRL_SEL_VAL, 0x0);
	isp_wr(vin, ISP_CSI_CTRL_SEL_MASK, 0x30);
	isp_wr(vin, ISP_CSI_CTRL_SEL_VAL, 0x30);

	for (i = 0; i <= 0x30; i += 4)
		dphy_wr(vin, i, 0);

	for (i = 0; i < NUM_PHY_LANES; i++) {
		dphy_wr(vin, DPHY_LANE_CFG_CLR, DPHY_SWAP_FIELD << dphy_swap_shift[i]);
		dphy_wr(vin, DPHY_LANE_CFG_SET, (u32)vin->lane_swap[i] << dphy_swap_shift[i]);
	}
	dphy_wr(vin, DPHY_LANE_CFG_CLR, DPHY_1D2C_EN);
	dphy_wr(vin, DPHY_LANE_CFG_CLR, DPHY_DPDN_SWAP);
	dphy_wr(vin, DPHY_LANE_CFG_SET, 0);
	dphy_wr(vin, DPHY_LANE_CFG_CLR, DPHY_DATABUS16_SEL);

	/* Clock group 1, clock group 0, data group 1, data group 0. */
	for (i = 16; i < 32 + 16; i += 8) {
		dphy_wr(vin, DPHY_PRE_TIME_CLR, 0xff << (i % 32));
		dphy_wr(vin, DPHY_PRE_TIME_SET, DPHY_PRE_TIME << (i % 32));
	}

	dphy_wr(vin, DPHY_MODE_CLR, 0xfff);
	dphy_wr(vin, DPHY_MODE_SET, DPHY_MODE_MIPI);

	usleep_range(2000, 2500);
	isp_wr(vin, ISP_RST0_DEASSERT, ISP_RST_DPHYRX);
}

static void csi_ctrl_init(struct vin *vin)
{
	csi_rmw(vin, CSI_LANE_CONTROL, CSI_LANE_CONTROL_MASK, CSI_LANE_CONTROL_4LANE);
	csi_rmw(vin, CSI_N_LANES, 0x1f, CSI_N_LANES_4);

	writel(0x7, vin->csi + CSI_ERR_IRQ_MASK_A);
	writel(0x6f, vin->csi + CSI_DPHY_ERR_IRQ_MASK);
	writel(0x00011ee1, vin->csi + CSI_ERR_IRQS_MASK);
	writel(0x00111100, vin->csi + CSI_INFO_IRQS_MASK);
	csi_rmw(vin, CSI_MONITOR_CTRL, 0x333, 0x301);
	csi_rmw(vin, CSI_MONITOR, BIT(4), BIT(4));
	writel(0x00000d1f, vin->csi + CSI_MONITOR_LB);

	csi_rmw(vin, CSI_STREAM_CTRL, CSI_STREAM_SRST, CSI_STREAM_SRST);
	udelay(10);
	csi_rmw(vin, CSI_STREAM_CTRL, CSI_STREAM_SRST, 0);
	udelay(100);

	csi_rmw(vin, CSI_STREAM_CTRL, CSI_STREAM_STOP, 0);
	csi_rmw(vin, CSI_STREAM_CTRL, CSI_STREAM_START, CSI_STREAM_START);

	writel(~0U, vin->csi + CSI_ERR_STATUS0);
	writel(~0U, vin->csi + CSI_ERR_STATUS1);
}

/* Resets released -> clocks on -> D-PHY powered -> D-PHY on -> controller. */
static int csi_start(struct vin *vin)
{
	int ret;

	/* The D-PHY is programmed in reset on every start, the first included. */
	isp_wr(vin, ISP_RST0_ASSERT, ISP_RST_DPHYRX);
	/* One strobe per bit, in this order, as the vendor does. */
	isp_wr(vin, ISP_RST0_DEASSERT, ISP_RST_PIXEL);
	isp_wr(vin, ISP_RST0_DEASSERT, ISP_RST_PPI_RX_BYTE);
	isp_wr(vin, ISP_RST0_DEASSERT, ISP_RST_PRESETN);
	isp_wr(vin, ISP_RST0_DEASSERT, ISP_RST_SYS);
	isp_wr(vin, ISP_RST0_DEASSERT, ISP_RST_DESKEW0);
	isp_wr(vin, ISP_RST0_DEASSERT, ISP_RST_DESKEW1);

	/* The D-PHY's register file needs its TLB out of reset and clocked. */
	ret = reset_control_deassert(vin->tlb_rst);
	if (ret)
		return ret;
	udelay(10);
	ret = clk_prepare_enable(vin->tlb_clk);
	if (ret)
		return ret;
	dphy_wr(vin, DPHY_DESKEW_DBG_CLR, DPHY_DESKEW_4LANE);

	isp_wr(vin, ISP_CLK_EB1_SET, ISP_CSIRX_PCLK);
	isp_wr(vin, ISP_CLK_EB1_SET, ISP_SYS_PIXEL_CLK);
	/* The CSI-2 config clock select, [1:0]. */
	isp_wr(vin, ISP_CLK_MUX_CLR, 0x3);
	isp_wr(vin, ISP_CLK_MUX_SET, 0x3);
	isp_wr(vin, ISP_CLK_EB0_SET, ISP_CFG_PHY_CLK);
	isp_wr(vin, ISP_CLK_EB0_SET, ISP_DPHY_RX_REF_CLK);

	ret = reset_control_deassert(vin->pwr_off);
	if (ret)
		goto err_clk;
	udelay(100);
	ret = reset_control_deassert(vin->pwr_ready);
	if (ret)
		goto err_clk;
	udelay(100);

	dphy_wr(vin, DPHY_PHY_DISABLE, DPHY_ALL_LANES);
	csi_dphy_init(vin);
	dphy_wr(vin, DPHY_PHY_EN, DPHY_ALL_LANES);
	dphy_wr(vin, DPHY_PHY_DISABLE, DPHY_DATA_LANES);
	dphy_wr(vin, DPHY_PHY_EN, DPHY_DATA_LANES);

	csi_ctrl_init(vin);
	return 0;

err_clk:
	clk_disable_unprepare(vin->tlb_clk);
	return ret;
}

static void csi_stop(struct vin *vin)
{
	isp_wr(vin, ISP_RST0_ASSERT, ISP_RST_DPHYRX);
	isp_wr(vin, ISP_CLK_EB0_CLR, ISP_DPHY_RX_REF_CLK);
	isp_wr(vin, ISP_CLK_EB0_CLR, ISP_CFG_PHY_CLK);
	dphy_wr(vin, DPHY_PHY_DISABLE, DPHY_ALL_LANES);

	reset_control_assert(vin->pwr_ready);
	udelay(100);
	reset_control_assert(vin->pwr_off);
	udelay(100);

	isp_wr(vin, ISP_RST0_ASSERT, ISP_RST_DESKEW0);
	clk_disable_unprepare(vin->tlb_clk);

	csi_rmw(vin, CSI_STREAM_CTRL, CSI_STREAM_START, 0);
	csi_rmw(vin, CSI_STREAM_CTRL, CSI_STREAM_STOP, CSI_STREAM_STOP);
}

static u32 axi_busy(struct vin *vin)
{
	u32 busy = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(axi_quiesce); i++)
		busy |= vin_rd(vin, axi_quiesce[i][1]);
	return busy;
}

/* Every VIN interrupt masked and cleared; frame-done is enabled per stream. */
static void vin_irqs_off(struct vin *vin)
{
	unsigned int n;

	for (n = 0; n < INT_GROUPS; n++) {
		vin_wr(vin, INT_ENABLE(n), 0);
		vin_wr(vin, INT_CLEAR(n), ~0U);
		vin_wr(vin, INT1_ENABLE(n), 0);
		vin_wr(vin, INT1_CLEAR(n), ~0U);
	}
}

/*
 * Pulse every ISP reset line but the receiver's, with the AXI masters
 * quiesced, so the datapath starts from reset, not from where it was left.
 * Ends with the ISP-top gate cleared (until then, datapath writes read back
 * 0) and every interrupt masked.
 *
 * A reset under an outstanding transfer can hang the bus, so a master still
 * busy refuses it: -EBUSY, the masters left quiesced (no new transfers),
 * interrupts masked.
 */
static int vin_datapath_reset(struct vin *vin)
{
	unsigned int i;
	u32 busy;

	for (i = 0; i < ARRAY_SIZE(axi_quiesce); i++)
		vin_wr(vin, axi_quiesce[i][0], ~0U);
	if (read_poll_timeout(axi_busy, busy, !busy, 200, 10200, false, vin)) {
		dev_err(vin->dev, "AXI masters busy (IFE %#x ITP %#x YUV %#x): datapath not reset\n",
			vin_rd(vin, axi_quiesce[0][1]), vin_rd(vin, axi_quiesce[1][1]),
			vin_rd(vin, axi_quiesce[2][1]));
		vin_irqs_off(vin);
		return -EBUSY;
	}

	for (i = 0; i < 32; i++) {
		if (BIT(i) & ISP_RST0_CSI2RX)
			continue;
		isp_wr(vin, ISP_RST0_ASSERT, BIT(i));
		isp_wr(vin, ISP_RST0_DEASSERT, BIT(i));
	}

	for (i = 0; i < ARRAY_SIZE(axi_quiesce); i++)
		vin_wr(vin, axi_quiesce[i][0], 0);

	vin_wr(vin, TOP_GATE_CLR, TOP_GATE_BYPASS);
	vin_irqs_off(vin);
	return 0;
}

/*
 * The ISP domain (isp_sys_glb; nothing outside the ISP): kick the mux domain,
 * select the clock sources, ungate, release every reset line, then reset the
 * datapath. Releasing only the IFE's lines leaves the register file reading
 * 0xdeadbeef.
 */
static void vin_clkrst_init(struct vin *vin)
{
	/* Mux fields [10:8], [7:5], [4:2]: codes 5/5/3. */
	static const u8 mux_shift[] = { 8, 5, 2 }, mux_code[] = { 5, 5, 3 };
	unsigned int i;

	isp_wr(vin, ISP_RST1_ASSERT, ISP_RST1_MUX_KICK);
	isp_wr(vin, ISP_RST1_DEASSERT, ISP_RST1_MUX_KICK);

	for (i = 0; i < ARRAY_SIZE(mux_shift); i++) {
		isp_wr(vin, ISP_CLK_MUX_CLR, 0x7 << mux_shift[i]);
		isp_wr(vin, ISP_CLK_MUX_SET, mux_code[i] << mux_shift[i]);
	}

	isp_wr(vin, ISP_CLK_EB0_SET, 0x3f);
	isp_wr(vin, ISP_CLK_EB1_SET, 0x3fe);

	/* One line per write; all released before any is pulsed, or the bus hangs. */
	for (i = 0; i < 32; i++)
		isp_wr(vin, ISP_RST0_DEASSERT, BIT(i));
	for (i = 0; i < 23; i++)
		if (BIT(i) != ISP_RST1_MUX_KICK)
			isp_wr(vin, ISP_RST1_DEASSERT, BIT(i));

	/* Refused, the probe goes on: each stream start tries again. */
	vin_datapath_reset(vin);

	/* Expect mux 0x5ac/0x5af, resets 0/0, file[0] not 0xdeadbeef. */
	dev_info(vin->dev, "isp up: mux %#x rst %#x/%#x file[0] %#x\n",
		 isp_rd(vin, ISP_CLK_MUX), isp_rd(vin, ISP_RST0_STATUS),
		 isp_rd(vin, ISP_RST1_STATUS), vin_rd(vin, 0));
}

static void vin_load_image(struct vin *vin)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(vin_init_regs); i++) {
		const struct vin_reg *r = &vin_init_regs[i];
		u32 v = r->val;

		if (r->geom == GEOM_WH)
			v = vin->fmt.width | vin->fmt.height << 16;
		else if (r->geom == GEOM_H)
			v = vin->fmt.height;
		else if (r->geom == GEOM_STRIDE8)
			v = vin->fmt.bytesperline / 8;
		vin_wr(vin, r->off, v);
	}
}

/* Pre-clear, stop, start, arm; the vbus enable comes after START. */
static void vin_sif_start(struct vin *vin)
{
	vin_wr(vin, SIF_CTRL, 0);
	vin_wr(vin, SIF_ID, 0);
	vin_rmw(vin, SIF_STOP, ~7U, BIT(0));
	vin_rmw(vin, SIF_START, ~7U, BIT(0));
	vin_rmw(vin, SIF_CTRL, ~7U, BIT(0));
	vin_wr(vin, SIF_ID, 0x00010001);
	vin_rmw(vin, SIF_VBUS_CTRL0, 0xfffc0e0e, BIT(0));
}

/* One shadow load writes exactly one frame, at the next frame boundary. */
static void vin_wdma_load(struct vin *vin, struct vin_buffer *buf)
{
	dma_addr_t addr = vb2_dma_contig_plane_dma_addr(&buf->vb.vb2_buf, 0);

	vin_wr(vin, WDMA_ADDR, lower_32_bits(addr >> 3));
	vin_rmw(vin, WDMA_SEQ, 0xffff0000, vin->sequence & 0xffff);
	vin_rmw(vin, WDMA_TRIG, ~1U, 1);
}

static irqreturn_t vin_irq(int irq, void *arg)
{
	struct vin *vin = arg;
	struct vin_buffer *done = NULL;
	u32 status;

	spin_lock(&vin->irqlock);
	status = vin_rd(vin, INT_MASKED(INT_GRP_FDONE));
	if (status)
		vin_wr(vin, INT_CLEAR(INT_GRP_FDONE), status);
	if (!(status & INT_FDONE)) {
		spin_unlock(&vin->irqlock);
		return status ? IRQ_HANDLED : IRQ_NONE;
	}

	vin->sequence++;
	if (vin->stopping) {
		/*
		 * Nothing loaded and the SIF stopped here, in the blanking: no frame
		 * follows this boundary, however long the rest of the stop takes.
		 */
		done = vin->active;
		vin->active = NULL;
		vin_rmw(vin, WDMA_TRIG, ~1U, 0);
		vin_rmw(vin, SIF_STOP, ~7U, BIT(0));
		complete(&vin->idle);
	} else if (vin->active && !list_empty(&vin->buf_list)) {
		/* Load the next buffer; with none queued, rewrite the active one (frame dropped). */
		done = vin->active;
		vin->active = list_first_entry(&vin->buf_list, struct vin_buffer, list);
		list_del(&vin->active->list);
	}
	if (vin->active)
		vin_wdma_load(vin, vin->active);

	spin_unlock(&vin->irqlock);

	if (done) {
		done->vb.vb2_buf.timestamp = ktime_get_ns();
		done->vb.sequence = vin->sequence - 1;
		done->vb.field = V4L2_FIELD_NONE;
		vb2_set_plane_payload(&done->vb.vb2_buf, 0, vin->fmt.sizeimage);
		vb2_buffer_done(&done->vb.vb2_buf, VB2_BUF_STATE_DONE);
	}
	return IRQ_HANDLED;
}

static int vin_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
			   unsigned int *nplanes, unsigned int sizes[],
			   struct device *alloc_devs[])
{
	struct vin *vin = vb2_get_drv_priv(vq);

	if (*nplanes)
		return sizes[0] < vin->fmt.sizeimage ? -EINVAL : 0;
	*nplanes = 1;
	sizes[0] = vin->fmt.sizeimage;
	return 0;
}

static void vin_buf_queue(struct vb2_buffer *vb)
{
	struct vin *vin = vb2_get_drv_priv(vb->vb2_queue);
	struct vin_buffer *buf = container_of(to_vb2_v4l2_buffer(vb), struct vin_buffer, vb);
	unsigned long flags;

	spin_lock_irqsave(&vin->irqlock, flags);
	list_add_tail(&buf->list, &vin->buf_list);
	spin_unlock_irqrestore(&vin->irqlock, flags);
}

static void vin_return_buffers(struct vin *vin, enum vb2_buffer_state state)
{
	struct vin_buffer *buf, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&vin->irqlock, flags);
	if (vin->active)
		vb2_buffer_done(&vin->active->vb.vb2_buf, state);
	vin->active = NULL;
	list_for_each_entry_safe(buf, tmp, &vin->buf_list, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
	spin_unlock_irqrestore(&vin->irqlock, flags);
}

static int vin_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct vin *vin = vb2_get_drv_priv(vq);
	int ret;

	/* Also releases the IFE master from the last stop's quiesce. */
	ret = vin_datapath_reset(vin);
	if (!ret)
		ret = csi_start(vin);
	if (ret) {
		vin_return_buffers(vin, VB2_BUF_STATE_QUEUED);
		return ret;
	}

	vin_load_image(vin);
	vin_wr(vin, TOP_MUX_EN, 0x30);
	vin_rmw(vin, IFE_GO, 0xf81c, 1);
	vin_wr(vin, IFE_FLUSH, ~0U);
	vin_rmw(vin, WDMA_ENABLE, ~1U, 1);

	vin_irqs_off(vin);
	vin_wr(vin, INT_ENABLE(INT_GRP_FDONE), INT_FDONE);

	vin_sif_start(vin);

	/* The first load after SIF start: one issued before it is lost. */
	spin_lock_irq(&vin->irqlock);
	vin->stopping = false;
	reinit_completion(&vin->idle);
	vin->sequence = 0;
	vin->active = list_first_entry(&vin->buf_list, struct vin_buffer, list);
	list_del(&vin->active->list);
	vin_wdma_load(vin, vin->active);
	spin_unlock_irq(&vin->irqlock);
	return 0;
}

static void vin_stop_streaming(struct vb2_queue *vq)
{
	struct vin *vin = vb2_get_drv_priv(vq);
	u32 busy;

	/*
	 * End on a frame boundary with no load pending, so the WDMA is not disabled
	 * mid-burst. Without a source no frame-done comes; the order is unchanged.
	 */
	spin_lock_irq(&vin->irqlock);
	vin->stopping = true;
	spin_unlock_irq(&vin->irqlock);
	wait_for_completion_timeout(&vin->idle, msecs_to_jiffies(STOP_IDLE_MS));

	vin_irqs_off(vin);
	synchronize_irq(vin->irq);

	vin_rmw(vin, SIF_STOP, ~7U, BIT(0));
	/* The IFE master stays held until the next start. */
	vin_wr(vin, axi_quiesce[0][0], ~0U);
	if (readl_poll_timeout(vin->regs + axi_quiesce[0][1], busy, !busy, 100, 100000))
		dev_warn(vin->dev, "IFE AXI still busy (%#x)\n", busy);
	vin_rmw(vin, IFE_GO, 0xf81c, 0);
	vin_rmw(vin, WDMA_TRIG, ~1U, 0);
	vin_rmw(vin, WDMA_ENABLE, ~1U, 0);

	csi_stop(vin);
	vin_return_buffers(vin, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops vin_vb2_ops = {
	.queue_setup = vin_queue_setup,
	.buf_queue = vin_buf_queue,
	.start_streaming = vin_start_streaming,
	.stop_streaming = vin_stop_streaming,
};

/* The WDMA packs Y0 U Y1 V; its stride register counts 8-byte units. */
static void vin_fill_fmt(struct v4l2_pix_format *pix)
{
	u32 width = clamp(pix->width, MIN_W, MAX_W) & ~1U;
	u32 height = clamp(pix->height, MIN_H, MAX_H);
	u32 bytesperline = ALIGN(width * 2, 8);

	*pix = (struct v4l2_pix_format) {
		.width = width,
		.height = height,
		.pixelformat = V4L2_PIX_FMT_YUYV,
		.field = V4L2_FIELD_NONE,
		.bytesperline = bytesperline,
		.sizeimage = bytesperline * height,
		.colorspace = V4L2_COLORSPACE_SRGB,
	};
}

static int vin_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	strscpy(cap->driver, "ax620e-vin", sizeof(cap->driver));
	strscpy(cap->card, "AX620E VIN", sizeof(cap->card));
	return 0;
}

static int vin_enum_fmt(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;
	f->pixelformat = V4L2_PIX_FMT_YUYV;
	return 0;
}

static int vin_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	f->fmt.pix = ((struct vin *)video_drvdata(file))->fmt;
	return 0;
}

static int vin_try_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	vin_fill_fmt(&f->fmt.pix);
	return 0;
}

static int vin_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct vin *vin = video_drvdata(file);

	if (vb2_is_busy(&vin->queue))
		return -EBUSY;
	vin_fill_fmt(&f->fmt.pix);
	vin->fmt = f->fmt.pix;
	return 0;
}

static const struct v4l2_ioctl_ops vin_ioctl_ops = {
	.vidioc_querycap = vin_querycap,
	.vidioc_enum_fmt_vid_cap = vin_enum_fmt,
	.vidioc_g_fmt_vid_cap = vin_g_fmt,
	.vidioc_s_fmt_vid_cap = vin_s_fmt,
	.vidioc_try_fmt_vid_cap = vin_try_fmt,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
};

static const struct v4l2_file_operations vin_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.unlocked_ioctl = video_ioctl2,
	.mmap = vb2_fop_mmap,
	.poll = vb2_fop_poll,
};

/*
 * The endpoint's data-lanes/clock-lanes name the physical slot (0-5) of each
 * logical lane, which is what the D-PHY swap fields take. c1 belongs to a
 * two-clock mode this receiver does not use; it takes the one slot left over.
 */
static int vin_parse_lanes(struct vin *vin)
{
	struct v4l2_fwnode_endpoint vep = { .bus_type = V4L2_MBUS_CSI2_DPHY };
	struct fwnode_handle *ep;
	unsigned int i, used = 0;
	int ret;

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(vin->dev), 0, 0, 0);
	if (!ep)
		return dev_err_probe(vin->dev, -EINVAL, "no endpoint\n");
	ret = v4l2_fwnode_endpoint_parse(ep, &vep);
	fwnode_handle_put(ep);
	if (ret)
		return dev_err_probe(vin->dev, ret, "endpoint\n");

	if (vep.bus.mipi_csi2.num_data_lanes != NUM_DATA_LANES)
		return dev_err_probe(vin->dev, -EINVAL, "need %u data lanes\n",
				     NUM_DATA_LANES);
	for (i = 0; i < NUM_DATA_LANES; i++)
		vin->lane_swap[i] = vep.bus.mipi_csi2.data_lanes[i];
	vin->lane_swap[4] = vep.bus.mipi_csi2.clock_lane;
	for (i = 0; i < 5; i++) {
		if (vin->lane_swap[i] >= NUM_PHY_LANES || used & BIT(vin->lane_swap[i]))
			return dev_err_probe(vin->dev, -EINVAL, "bad lane map\n");
		used |= BIT(vin->lane_swap[i]);
	}
	vin->lane_swap[5] = ffz(used);
	dev_info(vin->dev, "lanes d[%u %u %u %u] c[%u %u]\n", vin->lane_swap[0],
		 vin->lane_swap[1], vin->lane_swap[2], vin->lane_swap[3],
		 vin->lane_swap[4], vin->lane_swap[5]);
	return 0;
}

static int vin_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk *clk;
	struct vb2_queue *q;
	struct vin *vin;
	int ret;

	vin = devm_kzalloc(dev, sizeof(*vin), GFP_KERNEL);
	if (!vin)
		return -ENOMEM;
	vin->dev = dev;
	mutex_init(&vin->lock);
	spin_lock_init(&vin->irqlock);
	init_completion(&vin->idle);
	INIT_LIST_HEAD(&vin->buf_list);
	vin->fmt.width = 1920;
	vin->fmt.height = 1080;
	vin_fill_fmt(&vin->fmt);

	vin->regs = devm_platform_ioremap_resource_byname(pdev, "vin");
	if (IS_ERR(vin->regs))
		return PTR_ERR(vin->regs);
	vin->csi = devm_platform_ioremap_resource_byname(pdev, "csi");
	if (IS_ERR(vin->csi))
		return PTR_ERR(vin->csi);
	vin->dphy = devm_platform_ioremap_resource_byname(pdev, "dphy");
	if (IS_ERR(vin->dphy))
		return PTR_ERR(vin->dphy);
	vin->isp = syscon_regmap_lookup_by_phandle(dev->of_node, "axera,isp-syscon");
	if (IS_ERR(vin->isp))
		return dev_err_probe(dev, PTR_ERR(vin->isp), "isp syscon\n");

	/* The VI and ISP-MM domain gates: on before the first register access. */
	clk = devm_clk_get_enabled(dev, "vi");
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "vi clock\n");
	clk = devm_clk_get_enabled(dev, "isp_mm");
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "isp_mm clock\n");
	vin->tlb_clk = devm_clk_get(dev, "dphyrx_tlb");
	if (IS_ERR(vin->tlb_clk))
		return dev_err_probe(dev, PTR_ERR(vin->tlb_clk), "dphyrx_tlb clock\n");
	vin->tlb_rst = devm_reset_control_get_exclusive(dev, "dphyrx_tlb");
	if (IS_ERR(vin->tlb_rst))
		return dev_err_probe(dev, PTR_ERR(vin->tlb_rst), "dphyrx_tlb reset\n");
	vin->pwr_off = devm_reset_control_get_exclusive(dev, "dphy_pwr_off");
	if (IS_ERR(vin->pwr_off))
		return dev_err_probe(dev, PTR_ERR(vin->pwr_off), "dphy_pwr_off\n");
	vin->pwr_ready = devm_reset_control_get_exclusive(dev, "dphy_pwr_ready");
	if (IS_ERR(vin->pwr_ready))
		return dev_err_probe(dev, PTR_ERR(vin->pwr_ready), "dphy_pwr_ready\n");

	ret = vin_parse_lanes(vin);
	if (ret)
		return ret;

	vin->irq = platform_get_irq(pdev, 0);
	if (vin->irq < 0)
		return vin->irq;

	ret = v4l2_device_register(dev, &vin->v4l2_dev);
	if (ret)
		return ret;

	q = &vin->queue;
	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP;
	q->drv_priv = vin;
	q->buf_struct_size = sizeof(struct vin_buffer);
	q->ops = &vin_vb2_ops;
	q->mem_ops = &vb2_dma_contig_memops;
	/*
	 * Userspace reads frames with the CPU: let it ask for cacheable buffers
	 * (V4L2_MEMORY_FLAG_NON_COHERENT), synced by vb2 at QBUF and DQBUF.
	 */
	q->allow_cache_hints = 1;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC | V4L2_BUF_FLAG_TSTAMP_SRC_EOF;
	/* One being written, one loaded behind it. */
	q->min_queued_buffers = 2;
	q->dev = dev;
	q->lock = &vin->lock;
	ret = vb2_queue_init(q);
	if (ret)
		goto err_v4l2;

	vin->vdev.fops = &vin_fops;
	vin->vdev.ioctl_ops = &vin_ioctl_ops;
	vin->vdev.release = video_device_release_empty;
	vin->vdev.v4l2_dev = &vin->v4l2_dev;
	vin->vdev.queue = q;
	vin->vdev.lock = &vin->lock;
	vin->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_IO_MC;
	strscpy(vin->vdev.name, dev_name(dev), sizeof(vin->vdev.name));
	video_set_drvdata(&vin->vdev, vin);

	/* The hardware last, just before the node appears; masked before the handler. */
	vin_clkrst_init(vin);
	ret = devm_request_irq(dev, vin->irq, vin_irq, 0, dev_name(dev), vin);
	if (ret)
		goto err_v4l2;

	ret = video_register_device(&vin->vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_v4l2;
	platform_set_drvdata(pdev, vin);
	return 0;

err_v4l2:
	v4l2_device_unregister(&vin->v4l2_dev);
	return ret;
}

/* Unloading needs the node closed (its fops hold the module), so nothing streams. */
static void vin_remove(struct platform_device *pdev)
{
	struct vin *vin = platform_get_drvdata(pdev);

	video_unregister_device(&vin->vdev);
	v4l2_device_unregister(&vin->v4l2_dev);
}

static const struct of_device_id vin_of_match[] = {
	{ .compatible = "axera,ax620e-vin" },
	{ }
};
MODULE_DEVICE_TABLE(of, vin_of_match);

static struct platform_driver vin_driver = {
	.probe = vin_probe,
	.remove = vin_remove,
	.driver = {
		.name = "ax620e-vin",
		.of_match_table = vin_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(vin_driver);

MODULE_DESCRIPTION("Axera AX620E CSI-2 video capture");
MODULE_LICENSE("GPL");
