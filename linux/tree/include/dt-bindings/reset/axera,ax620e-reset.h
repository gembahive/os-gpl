/* SPDX-License-Identifier: (GPL-2.0 OR MIT) */
/*
 * Reset IDs for the Axera AX620E family. The lines are bits in the clock
 * controllers' windows, so the clock nodes are the reset providers too.
 *
 * Copyright (c) 2026 the open-nanokvm-pro contributors.
 * Copyright (c) 2026, GembaHive, Inc.
 */

#ifndef _DT_BINDINGS_RESET_AXERA_AX620E_H
#define _DT_BINDINGS_RESET_AXERA_AX620E_H

/* axera,ax620e-periph-clk */
#define AX620E_RST_PERIPH_GPIO0_PRST	0
#define AX620E_RST_PERIPH_GPIO0		1
#define AX620E_RST_PERIPH_GPIO1_PRST	2
#define AX620E_RST_PERIPH_GPIO1		3
#define AX620E_RST_PERIPH_GPIO2_PRST	4
#define AX620E_RST_PERIPH_GPIO2		5
#define AX620E_RST_PERIPH_GPIO3_PRST	6
#define AX620E_RST_PERIPH_GPIO3		7
#define AX620E_RST_PERIPH_WDT0_PRST	8
#define AX620E_RST_PERIPH_WDT0_ARST	9
#define AX620E_RST_PERIPH_CE_MAIN	10
#define AX620E_RST_PERIPH_CE_CNT	11
#define AX620E_RST_PERIPH_CE_SOFT	12
#define AX620E_RST_PERIPH_CE_PRST	13
#define AX620E_RST_PERIPH_CE		14

#define AX620E_RST_PERIPH_I2C1_PRST	15
#define AX620E_RST_PERIPH_I2C1	16
#define AX620E_RST_PERIPH_PWM1	17
#define AX620E_RST_PERIPH_PWM1_CH2	18
#define AX620E_RST_PERIPH_I2C0_PRST	19
#define AX620E_RST_PERIPH_I2C0		20

/* axera,ax620e-flash-clk */
#define AX620E_RST_FLASH_USB2_PHY	0
#define AX620E_RST_FLASH_USB2_VCC	1
#define AX620E_RST_FLASH_SDIO_CARD	2
#define AX620E_RST_FLASH_SDIO_A		3
#define AX620E_RST_FLASH_SDIO_P		4

/* axera,ax620e-common-clk */
#define AX620E_RST_COMMON_THM0		0
#define AX620E_RST_COMMON_THM1		1
#define AX620E_RST_COMMON_DPHYRX_TLB	2
#define AX620E_RST_COMMON_DPHYRX_PWR_OFF	3
#define AX620E_RST_COMMON_DPHYRX_PWR_READY	4

/* axera,ax620e-mm-clk */
#define AX620E_RST_MM_TDP_PRST		0
#define AX620E_RST_MM_TDP		1

/* axera,ax620e-vpu-clk */
#define AX620E_RST_VPU_JENC		0

#endif /* _DT_BINDINGS_RESET_AXERA_AX620E_H */
