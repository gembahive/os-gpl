/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * AX620E JPEG encoder registers. VCMD from VeriSilicon's dual MIT/GPL
 * vcmdregistertable.h; core fields by their VC8000E registertable.h names,
 * the ones libax_venc's JPEG path programs.
 */

#ifndef AX620E_JENC_REGS_H
#define AX620E_JENC_REGS_H

#include <linux/bits.h>

/* VCMD */
#define VCMD_HWID			0x00
#define VCMD_HWID_MAGIC			0x4342
#define VCMD_CTRL			0x40
#define VCMD_CTRL_START			BIT(0)
#define VCMD_CTRL_MASTER_CLK_ON		BIT(5)
#define VCMD_IRQ			0x44
#define VCMD_IRQ_EN			0x48
#define VCMD_IRQ_ENDCMD			BIT(0)
/* ENDCMD, BUSERR, TIMEOUT, CMDERR, ABORT, JMPD, JMPP: what the vendor driver enables. */
#define VCMD_IRQ_ALL			0xdf
#define VCMD_CMD_ADDR			0x50
#define VCMD_CMD_ADDR_MSB		0x54
#define VCMD_CMD_LEN			0x58	/* 64-bit words */
#define VCMD_AXI			0x5c
#define VCMD_AXI_BURST(n)		((n) << 16)
#define VCMD_RDY_COUNT			0x60
#define VCMD_EXT_INT_GATE		0x64

#define VCMD_OP_WREG			(0x01U << 27)
#define VCMD_OP_END			(0x02U << 27)
#define VCMD_OP_STALL			(0x09U << 27)
#define VCMD_OP_RREG			(0x16U << 27)
#define VCMD_OP_CLRINT			(0x1aU << 27)

/* VCMD bus address of core register n. */
#define JENC_CORE			0x1000
#define JENC_SWREG(n)			(JENC_CORE + 4 * (n))
#define JENC_STATUS_REGS		512

#define JENC_REG_IRQ			1
#define JENC_IRQ_FRAME_RDY		BIT(2)
#define JENC_IRQ_ERRORS			(BIT(3) | BIT(4) | BIT(5) | BIT(6) | BIT(9))
#define JENC_IRQ_TIMEOUT_EN		BIT(11)
#define JENC_REG_AXI_SWAP		2
#define JENC_REG_AXI_CLK		3
#define JENC_REG_MODE			4
#define JENC_MODE_JPEG			(4U << 29)
#define JENC_REG_PIC			5
#define JENC_PIC_WIDTH8(w)		((w) / 8 << 20)
#define JENC_PIC_HEIGHT8(h)		((h) / 8 << 8)
#define JENC_PIC_E			BIT(0)
#define JENC_REG_STRM_BASE		8
#define JENC_REG_STRM_LIMIT		9	/* bytes written, after the frame */
#define JENC_REG_IN_Y			12
#define JENC_REG_IN_CB			13
#define JENC_REG_IN_CR			14
#define JENC_REG_ROWLEN			20
#define JENC_ROWLEN(w)			((w) << 17)
#define JENC_REG_IN_FMT			38
#define JENC_IN_FMT_NV12		(1U << 28)
#define JENC_IN_FMT_YUYV		(2U << 28)
#define JENC_IN_YFILL(n)		(((n) & 7) << 1)
#define JENC_REG_HW_CFG			80
#define JENC_HW_CFG_JPEG		BIT(15)
#define JENC_REG_MAX_BURST		81
#define JENC_REG_QUANT_LUMA		83	/* 16 regs; chroma follows */
#define JENC_REG_FILL_MSB		193
#define JENC_FILL_YMSB(n)		(((n) >> 3) << 4)
#define JENC_REG_LU_STRIDE		210
#define JENC_REG_CH_STRIDE		211
#define JENC_STRIDE(b)			((b) << 12)
#define JENC_REG_AXI_WR_OUT		246
#define JENC_REG_AXI_RD_OUT		261
#define JENC_REG_QUANT_NONROI		398	/* the same 32 regs again */
#define JENC_REGS			512

/*
 * An AX register beside the core (VCMD bus 0x2800). libax_venc picks its value
 * from a config field (+0x1d70): 2 -> 0x44, 0 -> 0x42, anything else -> 0x46.
 * The driver writes 0x44.
 */
#define JENC_AX_2800			0x2800

#endif
