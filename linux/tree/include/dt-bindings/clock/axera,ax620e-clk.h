/* SPDX-License-Identifier: (GPL-2.0 OR MIT) */
/*
 * Clock IDs for the Axera AX620E family, one namespace per controller node.
 *
 * Copyright (c) 2026 the open-nanokvm-pro contributors.
 * Copyright (c) 2026, GembaHive, Inc.
 */

#ifndef _DT_BINDINGS_CLOCK_AXERA_AX620E_H
#define _DT_BINDINGS_CLOCK_AXERA_AX620E_H

/* axera,ax620e-common-clk */
#define AX620E_RTC_32K			0
#define AX620E_CPLL_24M			1
#define AX620E_CPLL_156M		2
#define AX620E_CPLL_208M		3
#define AX620E_CPLL_312M		4
#define AX620E_EPLL_100M		5
#define AX620E_EPLL_250M		6
#define AX620E_EPLL_375M		7
#define AX620E_NPLL_400M		8
#define AX620E_PCLK_TOP_SEL		9

#define AX620E_EPLL_50M			10

#define AX620E_CLK_THM_EB		11
#define AX620E_PCLK_THM_EB		12

#define AX620E_CLK_DPHYRX_TLB_EB	13
#define AX620E_CLK_ISP_MM_EB		14
#define AX620E_CLK_VI_EB		15

/* axera,ax620e-flash-clk */
#define AX620E_CLK_FLASH_GLB_SEL	0
#define AX620E_CLK_SD_CARD_SEL		1
#define AX620E_CLK_SD_CARD_DIVN		2
#define AX620E_CLK_SD_CARD_EB		3
#define AX620E_CLK_USB_REF_EB		4
#define AX620E_BUS_CLK_USB_EB		5
#define AX620E_USB_REF_ALT_CLK_EB	6
#define AX620E_CLK_SDIO_CARD_SEL	7
#define AX620E_CLK_SDIO_CARD_DIVN	8
#define AX620E_CLK_SDIO_CARD_EB		9
#define AX620E_ACLK_SDIO_EB		10
#define AX620E_PCLK_SDIO_EB		11

/* axera,ax620e-periph-clk */
#define AX620E_CLK_GPIO_SEL		0
#define AX620E_CLK_WDT0_SEL		1
#define AX620E_CLK_GPIO_EB		2
#define AX620E_CLK_WDT0_EB		3
#define AX620E_CLK_GPIO0_EB		4
#define AX620E_CLK_GPIO1_EB		5
#define AX620E_CLK_GPIO2_EB		6
#define AX620E_CLK_GPIO3_EB		7
#define AX620E_PCLK_GPIO0_EB		8
#define AX620E_PCLK_GPIO1_EB		9
#define AX620E_PCLK_GPIO2_EB		10
#define AX620E_PCLK_GPIO3_EB		11
#define AX620E_PCLK_WDT0_EB		12
#define AX620E_CLK_CE_CNT_EB		13
#define AX620E_ACLK_CE_EB		14
#define AX620E_PCLK_CE_EB		15

#define AX620E_CLK_I2C_SEL		16
#define AX620E_CLK_SPI_M2_SEL		17
#define AX620E_CLK_TIMER_SEL		18
#define AX620E_CLK_I2C_EB		19
#define AX620E_CLK_SPI_M2_EB		20
#define AX620E_CLK_TIMER_EB		21
#define AX620E_CLK_I2C1_EB		22
#define AX620E_CLK_PWM12_EB		23
#define AX620E_PCLK_I2C1_EB		24
#define AX620E_PCLK_SPI_M2_EB		25
#define AX620E_PCLK_PWM1_EB		26
#define AX620E_CLK_I2C0_EB		27
#define AX620E_PCLK_I2C0_EB		28

/* axera,ax620e-mm-clk */
#define AX620E_CLK_TDP_EB		0
#define AX620E_PCLK_TDP_EB		1
#define AX620E_CLK_CMD_EB		2
#define AX620E_PCLK_CMD_EB		3

/* axera,ax620e-vpu-clk */
#define AX620E_CLK_JENC_EB		0

#endif /* _DT_BINDINGS_CLOCK_AXERA_AX620E_H */
