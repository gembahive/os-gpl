// SPDX-License-Identifier: GPL-2.0
/*
 * Axera AX620E pin controller: 111 pads, one word each (3-bit function select,
 * pull, schmitt, drive), split over two register windows. Pad table from the
 * vendor GPL pinctrl-ax620e.h/.c, one table for AX620Q and AX630C.
 *
 * Copyright (c) 2026 the open-nanokvm-pro contributors.
 */

#include <linux/bitfield.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>

#include "../core.h"
#include "../pinmux.h"

/* Pad VALUE word. There is no direction, input-enable or slew-rate bit. */
#define AX620E_FUNC_MASK	GENMASK(18, 16)
#define AX620E_PULL_MASK	GENMASK(7, 6)
#define AX620E_PULL_UP		BIT(7)
#define AX620E_PULL_DOWN	BIT(6)
#define AX620E_PULL_UP_ENSEL	(BIT(7) | BIT(6))	/* bit 6 enable, bit 7 up */
#define AX620E_SCHMITT_BIT	BIT(4)
#define AX620E_DRIVE_MASK	GENMASK(3, 0)

/*
 * Each pad is a {VALUE, SET, CLR} triple: writing 1s to SET/CLR sets/clears
 * those VALUE bits, so updates touch only their own bits and need no lock.
 */
#define AX620E_PAD_SET	0x4
#define AX620E_PAD_CLR	0x8

/* Functions: enum index (what a pad's mux slot holds) and DT name. */
#define AX620E_FUNCTIONS(F) \
	F(GPIO, gpio) F(DB_GPIO, db_gpio) F(VI, vi) \
	F(SENSOR_SYNC, sensor_sync) F(PWM, pwm) F(DPI, dpi) F(RGMII, rgmii) \
	F(DEBUG_BUS, debug_bus) F(ANALOG_TEST, analog_test) F(SPI_M2, spi_m2) \
	F(DPHY_RX, dphy_rx) F(CLK_AUX, clk_aux) F(EMMC, emmc) \
	F(DPHY_TX, dphy_tx) F(INFRARED, infrared) F(BT656, bt656) \
	F(MCLK, mclk) F(SPI_M1, spi_m1) F(SPI_M0, spi_m0) F(SFC, sfc) \
	F(SPI_S, spi_s) F(SPI2AHB, spi2ahb) F(SDIO, sdio) F(SD, sd) \
	F(I2S1, i2s1) F(I2S0, i2s0) F(RISC_JTAG, risc_jtag) F(EPHY, ephy) \
	F(UART5, uart5) F(UART4, uart4) F(UART3, uart3) F(UART2, uart2) \
	F(UART1, uart1) F(UART0, uart0) F(TIMESTAMP, timestamp) \
	F(THERMAL_AIN, thermal_ain) F(SYSCTL, sysctl) \
	F(RESERVED_ANALOG, reserved_analog) F(EMAC_PPS, emac_pps) \
	F(SD_CTRL, sd_ctrl) F(BOND, bond) F(USB, usb) F(LCD_TE, lcd_te) \
	F(JTAG, jtag) F(I2C_SLV1, i2c_slv1) F(I2C_SLV0, i2c_slv0) \
	F(I2C7, i2c7) F(I2C6, i2c6) F(I2C5, i2c5) F(I2C4, i2c4) F(I2C3, i2c3) \
	F(I2C2, i2c2) F(I2C1, i2c1) F(I2C0, i2c0) F(DMIC, dmic) \
	F(EMMC_PWR_EN, emmc_pwr_en)

#define AX620E_ENUM(uc, lc)	FN_##uc,
#define AX620E_NAME(uc, lc)	#lc,

enum { FN_NONE = -1, AX620E_FUNCTIONS(AX620E_ENUM) AX620E_NUM_FUNCTIONS };

static const char * const ax620e_functions[] = { AX620E_FUNCTIONS(AX620E_NAME) };

/* @ensel: analog-capable pad, pull-up is bit 6 (enable) + bit 7 (direction). */
struct ax620e_pad {
	const char *name;
	u8 window;
	u16 offset;
	bool ensel;
	s8 mux[8];
};

#define PAD(n, w, o, e, m0, m1, m2, m3, m4, m5, m6, m7)			\
	{ n, w, o, e, { FN_##m0, FN_##m1, FN_##m2, FN_##m3,		\
			FN_##m4, FN_##m5, FN_##m6, FN_##m7 } }

/*
 * Offsets are local to the window (0: 0x02300000, 1: 0x104f0000), mux slots
 * 0-7 in order. Pin numbers are the array index. G7 (MIC*) pull values are unknown.
 */
static const struct ax620e_pad ax620e_pads[] = {
	PAD("VI_D0", 0, 0xc, 0, VI, INFRARED, SPI_M1, I2C6, I2S0, DB_GPIO, GPIO, ANALOG_TEST),
	PAD("VI_D1", 0, 0x18, 0, VI, INFRARED, SPI_M1, I2C6, I2S0, DB_GPIO, GPIO, ANALOG_TEST),
	PAD("VI_D2", 0, 0x24, 0, VI, INFRARED, SPI_M1, SYSCTL, I2S0, DB_GPIO, GPIO, ANALOG_TEST),
	PAD("VI_D3", 0, 0x30, 0, VI, INFRARED, SPI_M1, CLK_AUX, I2S0, DB_GPIO, GPIO, ANALOG_TEST),
	PAD("VI_D4", 0, 0x3c, 0, VI, INFRARED, SPI_M1, I2C5, I2S0, DB_GPIO, GPIO, ANALOG_TEST),
	PAD("VI_D5", 0, 0x48, 0, VI, NONE, SPI_S, I2C_SLV0, NONE, DB_GPIO, GPIO, NONE),
	PAD("VI_D6", 0, 0x54, 0, VI, INFRARED, SPI_S, I2C_SLV0, NONE, DB_GPIO, GPIO, NONE),
	PAD("VI_D7", 0, 0x60, 0, VI, INFRARED, SPI_S, SPI_M1, NONE, DB_GPIO, GPIO, NONE),
	PAD("VI_D8", 0, 0x6c, 0, VI, INFRARED, SPI_S, I2C7, NONE, DB_GPIO, GPIO, NONE),
	PAD("VI_D9", 0, 0x78, 0, VI, INFRARED, SPI_S, I2C7, NONE, DB_GPIO, GPIO, NONE),
	PAD("VI_CLK0", 0, 0x84, 0, VI, SPI_M1, SPI_S, I2C5, I2S0, DB_GPIO, GPIO, ANALOG_TEST),
	PAD("I2C0_SCL", 0, 0x400c, 0, I2C0, SPI_M2, RISC_JTAG, PWM,
	    CLK_AUX, UART2, GPIO, ANALOG_TEST),
	PAD("I2C0_SDA", 0, 0x4018, 0, I2C0, SPI_M2, RISC_JTAG, PWM,
	    CLK_AUX, UART2, GPIO, ANALOG_TEST),
	PAD("I2C1_SCL", 0, 0x4024, 0, I2C1, SPI_M2, NONE, PWM, CLK_AUX, UART4, GPIO, NONE),
	PAD("I2C1_SDA", 0, 0x4030, 0, I2C1, SPI_M2, NONE, PWM, CLK_AUX, UART4, GPIO, NONE),
	PAD("UART0_TXD", 0, 0x403c, 0, UART0, NONE, RISC_JTAG, NONE,
	    NONE, DB_GPIO, GPIO, ANALOG_TEST),
	PAD("UART0_RXD", 0, 0x4048, 0, UART0, NONE, RISC_JTAG, NONE,
	    NONE, DB_GPIO, GPIO, ANALOG_TEST),
	PAD("UART1_TXD", 0, 0x4054, 0, UART1, SPI_M2, RISC_JTAG, PWM,
	    SENSOR_SYNC, DB_GPIO, GPIO, ANALOG_TEST),
	PAD("UART1_RXD", 0, 0x4060, 0, UART1, MCLK, CLK_AUX, PWM,
	    SENSOR_SYNC, NONE, GPIO, ANALOG_TEST),
	PAD("UART2_TXD", 0, 0x406c, 0, UART2, NONE, CLK_AUX, TIMESTAMP,
	    SENSOR_SYNC, DB_GPIO, GPIO, ANALOG_TEST),
	PAD("UART2_RXD", 0, 0x4078, 0, UART2, NONE, CLK_AUX, TIMESTAMP,
	    SENSOR_SYNC, DB_GPIO, GPIO, ANALOG_TEST),
	PAD("UART3_TXD", 0, 0x4084, 0, UART3, SPI_M2, CLK_AUX, PWM,
	    SENSOR_SYNC, DB_GPIO, GPIO, NONE),
	PAD("UART3_RXD", 0, 0x4090, 0, UART3, SPI_M2, CLK_AUX, PWM,
	    SENSOR_SYNC, DB_GPIO, GPIO, NONE),
	PAD("EMAC_PTP_PPS0", 1, 0xc, 0, EMAC_PPS, DPI, PWM, UART5,
	    SENSOR_SYNC, DB_GPIO, GPIO, NONE),
	PAD("EMAC_PTP_PPS1", 1, 0x18, 0, EMAC_PPS, DPI, PWM, UART5,
	    SENSOR_SYNC, DB_GPIO, GPIO, NONE),
	PAD("EMAC_PTP_PPS2", 1, 0x24, 0, EMAC_PPS, DPI, SD_CTRL, UART5, NONE, DB_GPIO, GPIO, NONE),
	PAD("EMAC_PTP_PPS3", 1, 0x30, 0, EMAC_PPS, DPI, PWM, UART5, NONE, DB_GPIO, GPIO, NONE),
	PAD("RGMII_MDCK", 1, 0x3c, 0, RGMII, DPI, PWM, SENSOR_SYNC, NONE, DB_GPIO, GPIO, NONE),
	PAD("RGMII_MDIO", 1, 0x48, 0, RGMII, DPI, PWM, SENSOR_SYNC, NONE, DB_GPIO, GPIO, NONE),
	PAD("EPHY_CLK", 1, 0x54, 0, EPHY, DPI, CLK_AUX, SENSOR_SYNC, NONE, DB_GPIO, GPIO, NONE),
	PAD("EPHY_RSTN", 1, 0x60, 0, EPHY, DPI, PWM, SENSOR_SYNC, SPI_M2, DB_GPIO, GPIO, NONE),
	PAD("EPHY_LED0", 1, 0x6c, 0, EPHY, RGMII, LCD_TE, SENSOR_SYNC, SPI_M2, DB_GPIO, GPIO, NONE),
	PAD("EPHY_LED1", 1, 0x78, 0, EPHY, RGMII, LCD_TE, SENSOR_SYNC, SPI_M2, DB_GPIO, GPIO, NONE),
	PAD("RGMII_RXD0", 1, 0x84, 0, RGMII, DPI, DEBUG_BUS, NONE, UART0, DB_GPIO, GPIO, NONE),
	PAD("RGMII_RXD1", 1, 0x90, 0, RGMII, DPI, DEBUG_BUS, NONE, UART0, DB_GPIO, GPIO, NONE),
	PAD("RGMII_RXDV", 1, 0x9c, 0, RGMII, DPI, DEBUG_BUS, NONE, NONE, DB_GPIO, GPIO, NONE),
	PAD("RGMII_RXCLK", 1, 0xa8, 0, RGMII, DPI, DEBUG_BUS, NONE, NONE, DB_GPIO, GPIO, NONE),
	PAD("RGMII_RXD2", 1, 0xb4, 0, RGMII, DPI, NONE, NONE, NONE, DB_GPIO, GPIO, NONE),
	PAD("RGMII_RXD3", 1, 0xc0, 0, RGMII, DPI, NONE, NONE, NONE, DB_GPIO, GPIO, NONE),
	PAD("RGMII_TXD0", 1, 0xcc, 0, RGMII, DPI, DEBUG_BUS, MCLK, SPI_M2, DB_GPIO, GPIO, NONE),
	PAD("RGMII_TXD1", 1, 0xd8, 0, RGMII, DPI, DEBUG_BUS, SENSOR_SYNC,
	    SPI_M2, DB_GPIO, GPIO, NONE),
	PAD("RGMII_TXCLK", 1, 0xe4, 0, RGMII, DPI, DEBUG_BUS, SENSOR_SYNC,
	    SPI_M2, DB_GPIO, GPIO, NONE),
	PAD("RGMII_TXEN", 1, 0xf0, 0, RGMII, DPI, DEBUG_BUS, SENSOR_SYNC,
	    SPI_M2, DB_GPIO, GPIO, ANALOG_TEST),
	PAD("RGMII_TXD2", 1, 0xfc, 0, RGMII, DPI, PWM, SENSOR_SYNC, NONE, DB_GPIO, GPIO, NONE),
	PAD("RGMII_TXD3", 1, 0x108, 0, RGMII, DPI, PWM, SENSOR_SYNC, NONE, DB_GPIO, GPIO, NONE),
	PAD("SD_DAT0", 1, 0x100c, 0, SD, NONE, DEBUG_BUS, NONE, MCLK, DB_GPIO, GPIO, NONE),
	PAD("SD_DAT1", 1, 0x1018, 0, SD, NONE, DEBUG_BUS, NONE, MCLK, DB_GPIO, GPIO, NONE),
	PAD("SD_CLK", 1, 0x1024, 0, SD, NONE, DEBUG_BUS, NONE, NONE, DB_GPIO, GPIO, NONE),
	PAD("SD_CMD", 1, 0x1030, 0, SD, I2C_SLV1, DEBUG_BUS, NONE, NONE, DB_GPIO, GPIO, NONE),
	PAD("SD_DAT2", 1, 0x103c, 0, SD, I2C_SLV1, DEBUG_BUS, NONE, MCLK, DB_GPIO, GPIO, NONE),
	PAD("SD_DAT3", 1, 0x1048, 0, SD, NONE, DEBUG_BUS, NONE, MCLK, DB_GPIO, GPIO, NONE),
	PAD("EMMC_DAT5", 0, 0x900c, 0, EMMC, NONE, NONE, DB_GPIO, NONE, NONE, GPIO, NONE),
	PAD("EMMC_RESET_N", 0, 0x9018, 0, NONE, NONE, NONE, DB_GPIO, NONE, NONE, GPIO, NONE),
	PAD("EMMC_DAT4", 0, 0x9024, 0, EMMC, SFC, NONE, DB_GPIO, NONE, NONE, GPIO, NONE),
	PAD("EMMC_DAT6", 0, 0x9030, 0, EMMC, NONE, NONE, DB_GPIO, NONE, NONE, GPIO, NONE),
	PAD("EMMC_DS", 0, 0x903c, 0, EMMC, NONE, NONE, DB_GPIO, NONE, NONE, GPIO, NONE),
	PAD("EMMC_DAT7", 0, 0x9048, 0, EMMC, NONE, NONE, DB_GPIO, NONE, NONE, GPIO, NONE),
	PAD("EMMC_DAT3", 0, 0x9054, 0, EMMC, SFC, NONE, DB_GPIO, NONE, NONE, GPIO, SPI2AHB),
	PAD("EMMC_DAT2", 0, 0x9060, 0, EMMC, SFC, NONE, DB_GPIO, NONE, NONE, GPIO, SPI2AHB),
	PAD("EMMC_CLK", 0, 0x906c, 0, EMMC, SFC, NONE, DB_GPIO, NONE, NONE, GPIO, SPI2AHB),
	PAD("EMMC_DAT0", 0, 0x9078, 0, EMMC, SFC, NONE, DB_GPIO, NONE, NONE, GPIO, SPI2AHB),
	PAD("EMMC_CMD", 0, 0x9084, 0, EMMC, SFC, NONE, DB_GPIO, NONE, NONE, GPIO, SPI2AHB),
	PAD("EMMC_DAT1", 0, 0x9090, 0, EMMC, SFC, NONE, DB_GPIO, NONE, NONE, GPIO, SPI2AHB),
	PAD("SDIO_DAT0", 1, 0x200c, 0, SDIO, I2C3, NONE, I2S1, PWM, DB_GPIO, GPIO, NONE),
	PAD("SDIO_DAT1", 1, 0x2018, 0, SDIO, I2C3, NONE, I2S1, PWM, DB_GPIO, GPIO, NONE),
	PAD("SDIO_CLK", 1, 0x2024, 0, SDIO, EPHY, I2C2, I2S1, CLK_AUX, DB_GPIO, GPIO, NONE),
	PAD("SDIO_CMD", 1, 0x2030, 0, SDIO, I2C2, NONE, I2S1, NONE, DB_GPIO, GPIO, NONE),
	PAD("SDIO_DAT2", 1, 0x203c, 0, SDIO, I2C4, NONE, I2S1, NONE, DB_GPIO, GPIO, NONE),
	PAD("SDIO_DAT3", 1, 0x2048, 0, SDIO, I2C4, NONE, I2S1, NONE, DB_GPIO, GPIO, NONE),
	PAD("CDTX_L0N", 0, 0xa00c, 0, DPHY_TX, BT656, SPI_M0, DB_GPIO, NONE, NONE, GPIO, NONE),
	PAD("CDTX_L0P", 0, 0xa018, 0, DPHY_TX, BT656, SPI_M0, DB_GPIO, NONE, NONE, GPIO, NONE),
	PAD("CDTX_L1N", 0, 0xa024, 0, DPHY_TX, BT656, SPI_M0, DB_GPIO, NONE, NONE, GPIO, NONE),
	PAD("CDTX_L1P", 0, 0xa030, 0, DPHY_TX, BT656, SPI_M0, DB_GPIO, NONE, NONE, GPIO, NONE),
	PAD("CDTX_L2N", 0, 0xa03c, 0, DPHY_TX, BT656, SPI_M0, DB_GPIO, NONE, NONE, GPIO, NONE),
	PAD("CDTX_L2P", 0, 0xa048, 0, DPHY_TX, BT656, SPI_M0, DB_GPIO, NONE, NONE, GPIO, NONE),
	PAD("CDTX_L3N", 0, 0xa054, 0, DPHY_TX, BT656, SPI_M0, DB_GPIO, NONE, NONE, GPIO, NONE),
	PAD("CDTX_L3P", 0, 0xa060, 0, DPHY_TX, BT656, NONE, DB_GPIO, NONE, NONE, GPIO, NONE),
	PAD("CDTX_L4N", 0, 0xa06c, 0, DPHY_TX, BT656, NONE, DB_GPIO, NONE, NONE, GPIO, NONE),
	PAD("CDTX_L4P", 0, 0xa078, 0, DPHY_TX, NONE, NONE, DB_GPIO, NONE, NONE, GPIO, NONE),
	PAD("THM_AIN3", 0, 0x100c, 1, THERMAL_AIN, NONE, NONE, NONE, NONE, DB_GPIO, GPIO, NONE),
	PAD("THM_AIN2", 0, 0x1018, 1, THERMAL_AIN, NONE, MCLK, NONE, NONE, DB_GPIO, GPIO, NONE),
	PAD("THM_AIN1", 0, 0x1024, 1, THERMAL_AIN, TIMESTAMP, PWM, NONE, NONE, DB_GPIO, GPIO, NONE),
	PAD("THM_AIN0", 0, 0x1030, 1, THERMAL_AIN, TIMESTAMP, PWM, NONE, NONE, DB_GPIO, GPIO, NONE),
	PAD("SD_PWR_SW", 0, 0x200c, 1, SD_CTRL, SYSCTL, NONE, NONE, NONE, DB_GPIO, NONE, NONE),
	PAD("GPIO3_A1", 0, 0x2018, 1, GPIO, NONE, SYSCTL, NONE, NONE, DB_GPIO, NONE, NONE),
	PAD("GPIO3_A2", 0, 0x2024, 1, GPIO, SENSOR_SYNC, NONE, NONE, NONE, DB_GPIO, NONE, NONE),
	PAD("GPIO3_A3", 0, 0x2030, 1, GPIO, NONE, NONE, NONE, NONE, DB_GPIO, NONE, NONE),
	PAD("BOND0", 0, 0x2048, 1, BOND, SENSOR_SYNC, UART3, NONE, NONE, NONE, GPIO, NONE),
	PAD("BOND1", 0, 0x2054, 1, BOND, SENSOR_SYNC, UART3, NONE, NONE, NONE, GPIO, NONE),
	PAD("EMMC_PWR_EN", 0, 0x2060, 1, EMMC_PWR_EN, NONE, NONE, UART1, NONE, DB_GPIO, GPIO, NONE),
	PAD("BOND2", 0, 0x206c, 1, BOND, MCLK, NONE, UART1, NONE, NONE, GPIO, NONE),
	PAD("SYS_RSTN_OUT", 0, 0x2078, 1, SYSCTL, NONE, NONE, NONE, NONE, NONE, NONE, NONE),
	PAD("TMS", 0, 0x2090, 1, JTAG, UART4, NONE, NONE, NONE, DB_GPIO, GPIO, NONE),
	PAD("TCK", 0, 0x209c, 1, JTAG, UART4, NONE, NONE, NONE, DB_GPIO, GPIO, NONE),
	PAD("SD_PWR_EN", 0, 0x20a8, 1, SD_CTRL, NONE, NONE, NONE, NONE, DB_GPIO, GPIO, NONE),
	PAD("MICP_L_D", 0, 0x500c, 1, RESERVED_ANALOG, DMIC, DEBUG_BUS, DB_GPIO,
	    NONE, NONE, GPIO, NONE),
	PAD("MICN_L_D", 0, 0x5018, 1, RESERVED_ANALOG, DMIC, DEBUG_BUS, DB_GPIO,
	    NONE, NONE, GPIO, NONE),
	PAD("MICN_R_D", 0, 0x5024, 1, RESERVED_ANALOG, USB, SENSOR_SYNC, DB_GPIO,
	    NONE, NONE, GPIO, NONE),
	PAD("MICP_R_D", 0, 0x5030, 1, RESERVED_ANALOG, USB, NONE, DB_GPIO, NONE, NONE, GPIO, NONE),
	PAD("CDRX_L0N", 0, 0x300c, 0, DPHY_RX, VI, NONE, NONE, NONE, NONE, NONE, NONE),
	PAD("CDRX_L0P", 0, 0x3018, 0, DPHY_RX, VI, NONE, NONE, NONE, NONE, NONE, NONE),
	PAD("CDRX_L1N", 0, 0x3024, 0, DPHY_RX, VI, NONE, NONE, NONE, NONE, NONE, NONE),
	PAD("CDRX_L1P", 0, 0x3030, 0, DPHY_RX, VI, NONE, NONE, NONE, NONE, NONE, NONE),
	PAD("CDRX_L2N", 0, 0x303c, 0, DPHY_RX, VI, NONE, NONE, NONE, NONE, NONE, NONE),
	PAD("CDRX_L2P", 0, 0x3048, 0, DPHY_RX, VI, NONE, NONE, NONE, NONE, NONE, NONE),
	PAD("CDRX_L3N", 0, 0x3054, 0, DPHY_RX, VI, NONE, NONE, NONE, NONE, NONE, NONE),
	PAD("CDRX_L3P", 0, 0x3060, 0, DPHY_RX, VI, NONE, NONE, NONE, NONE, NONE, NONE),
	PAD("CDRX_L4N", 0, 0x306c, 0, DPHY_RX, VI, NONE, NONE, NONE, NONE, NONE, NONE),
	PAD("CDRX_L4P", 0, 0x3078, 0, DPHY_RX, VI, NONE, NONE, NONE, NONE, NONE, NONE),
	PAD("CDRX_L5N", 0, 0x3084, 0, DPHY_RX, VI, NONE, NONE, NONE, NONE, NONE, NONE),
	PAD("CDRX_L5P", 0, 0x3090, 0, DPHY_RX, VI, NONE, NONE, NONE, NONE, NONE, NONE),
};

#define AX620E_NUM_PADS	ARRAY_SIZE(ax620e_pads)

struct ax620e_pinctrl {
	struct device *dev;
	struct pinctrl_dev *pctl;
	struct pinctrl_desc desc;
	struct pinctrl_pin_desc pins[AX620E_NUM_PADS];
	void __iomem *base[2];
};

static void __iomem *ax620e_pad_reg(struct ax620e_pinctrl *pc, unsigned int pin)
{
	const struct ax620e_pad *pad = &ax620e_pads[pin];

	return pc->base[pad->window] + pad->offset;
}

static void ax620e_pad_update(struct ax620e_pinctrl *pc, unsigned int pin,
			      u32 mask, u32 val)
{
	void __iomem *reg = ax620e_pad_reg(pc, pin);

	if (mask & ~val)
		writel(mask & ~val, reg + AX620E_PAD_CLR);
	if (val & mask)
		writel(val & mask, reg + AX620E_PAD_SET);
}

static u32 ax620e_pull_up(unsigned int pin)
{
	return ax620e_pads[pin].ensel ? AX620E_PULL_UP_ENSEL : AX620E_PULL_UP;
}

static const struct pinctrl_ops ax620e_pinctrl_ops = {
	.get_groups_count = pinctrl_generic_get_group_count,
	.get_group_name = pinctrl_generic_get_group_name,
	.get_group_pins = pinctrl_generic_get_group_pins,
	.dt_node_to_map = pinconf_generic_dt_node_to_map_all,
	.dt_free_map = pinconf_generic_dt_free_map,
};

/* Which mux value on @pin selects function @func, or -EINVAL. */
static int ax620e_mux_value(unsigned int pin, unsigned int func)
{
	unsigned int v;

	for (v = 0; v < ARRAY_SIZE(ax620e_pads[pin].mux); v++)
		if (ax620e_pads[pin].mux[v] == (s8)func)
			return v;

	return -EINVAL;
}

static int ax620e_set_mux(struct pinctrl_dev *pctldev, unsigned int func,
			  unsigned int group)
{
	struct ax620e_pinctrl *pc = pinctrl_dev_get_drvdata(pctldev);
	const unsigned int *pins;
	unsigned int num_pins, i;
	int ret;

	ret = pinctrl_generic_get_group_pins(pctldev, group, &pins, &num_pins);
	if (ret)
		return ret;

	/*
	 * Check every pad first: the vendor silently wrote mux 0 on a miss.
	 * Moving a CDTX pad off function 0 needs the D-PHY TX held in reset
	 * (registers outside this block); nothing here needs that, so refuse.
	 */
	for (i = 0; i < num_pins; i++) {
		ret = ax620e_mux_value(pins[i], func);
		if (ret < 0 || (ret && ax620e_pads[pins[i]].mux[0] == FN_DPHY_TX)) {
			dev_err(pc->dev, "pad %s cannot take function %s\n",
				ax620e_pads[pins[i]].name, ax620e_functions[func]);
			return -EINVAL;
		}
	}

	for (i = 0; i < num_pins; i++)
		ax620e_pad_update(pc, pins[i], AX620E_FUNC_MASK,
				  FIELD_PREP(AX620E_FUNC_MASK,
					     ax620e_mux_value(pins[i], func)));

	return 0;
}

/* GPIO requests mux the pad: the vendor never did, so a re-muxed pad lied. */
static int ax620e_gpio_request_enable(struct pinctrl_dev *pctldev,
				      struct pinctrl_gpio_range *range,
				      unsigned int pin)
{
	struct ax620e_pinctrl *pc = pinctrl_dev_get_drvdata(pctldev);
	int v = ax620e_mux_value(pin, FN_GPIO);

	if (v < 0)
		return v;

	ax620e_pad_update(pc, pin, AX620E_FUNC_MASK,
			  FIELD_PREP(AX620E_FUNC_MASK, v));
	return 0;
}

static const struct pinmux_ops ax620e_pinmux_ops = {
	.get_functions_count = pinmux_generic_get_function_count,
	.get_function_name = pinmux_generic_get_function_name,
	.get_function_groups = pinmux_generic_get_function_groups,
	.set_mux = ax620e_set_mux,
	.gpio_request_enable = ax620e_gpio_request_enable,
	.strict = true,
};

static int ax620e_pinconf_get(struct pinctrl_dev *pctldev, unsigned int pin,
			      unsigned long *config)
{
	struct ax620e_pinctrl *pc = pinctrl_dev_get_drvdata(pctldev);
	enum pin_config_param param = pinconf_to_config_param(*config);
	u32 val = readl(ax620e_pad_reg(pc, pin));
	u32 pull = val & AX620E_PULL_MASK;
	u32 arg = 1;

	switch (param) {
	case PIN_CONFIG_BIAS_DISABLE:
		if (pull)
			return -EINVAL;
		break;
	case PIN_CONFIG_BIAS_PULL_DOWN:
		if (pull != AX620E_PULL_DOWN)
			return -EINVAL;
		break;
	case PIN_CONFIG_BIAS_PULL_UP:
		if (pull != ax620e_pull_up(pin))
			return -EINVAL;
		break;
	case PIN_CONFIG_INPUT_SCHMITT_ENABLE:
		if (!(val & AX620E_SCHMITT_BIT))
			return -EINVAL;
		break;
	case PIN_CONFIG_DRIVE_STRENGTH:
		/* A raw 4-bit code, not milliamps: the mA mapping is unknown. */
		arg = FIELD_GET(AX620E_DRIVE_MASK, val);
		break;
	default:
		return -ENOTSUPP;
	}

	*config = pinconf_to_config_packed(param, arg);
	return 0;
}

static int ax620e_pinconf_set(struct pinctrl_dev *pctldev, unsigned int pin,
			      unsigned long *configs, unsigned int num_configs)
{
	struct ax620e_pinctrl *pc = pinctrl_dev_get_drvdata(pctldev);
	unsigned int i;

	for (i = 0; i < num_configs; i++) {
		enum pin_config_param param = pinconf_to_config_param(configs[i]);
		u32 arg = pinconf_to_config_argument(configs[i]);

		switch (param) {
		case PIN_CONFIG_BIAS_DISABLE:
			ax620e_pad_update(pc, pin, AX620E_PULL_MASK, 0);
			break;
		case PIN_CONFIG_BIAS_PULL_DOWN:
			ax620e_pad_update(pc, pin, AX620E_PULL_MASK,
					  AX620E_PULL_DOWN);
			break;
		case PIN_CONFIG_BIAS_PULL_UP:
			ax620e_pad_update(pc, pin, AX620E_PULL_MASK,
					  ax620e_pull_up(pin));
			break;
		case PIN_CONFIG_INPUT_SCHMITT_ENABLE:
			ax620e_pad_update(pc, pin, AX620E_SCHMITT_BIT,
					  arg ? AX620E_SCHMITT_BIT : 0);
			break;
		case PIN_CONFIG_DRIVE_STRENGTH:
			if (arg > FIELD_MAX(AX620E_DRIVE_MASK))
				return -EINVAL;
			ax620e_pad_update(pc, pin, AX620E_DRIVE_MASK, arg);
			break;
		default:
			return -ENOTSUPP;
		}
	}

	return 0;
}

static const struct pinconf_ops ax620e_pinconf_ops = {
	.is_generic = true,
	.pin_config_get = ax620e_pinconf_get,
	.pin_config_set = ax620e_pinconf_set,
};

/*
 * Group "<function>" holds every pad that can carry it; each pad is also its
 * own group (DT states name pads). Functions go in enum order: the selector
 * set_mux gets is the enum value.
 */
static int ax620e_add_function(struct ax620e_pinctrl *pc, unsigned int func)
{
	const char *name = ax620e_functions[func];
	unsigned int i, n = 0, *pins;
	const char **groups;
	int ret;

	for (i = 0; i < AX620E_NUM_PADS; i++)
		n += ax620e_mux_value(i, func) >= 0;

	pins = devm_kcalloc(pc->dev, n, sizeof(*pins), GFP_KERNEL);
	groups = devm_kcalloc(pc->dev, n + 1, sizeof(*groups), GFP_KERNEL);
	if (!pins || !groups)
		return -ENOMEM;

	groups[0] = name;
	for (i = 0, n = 0; i < AX620E_NUM_PADS; i++) {
		if (ax620e_mux_value(i, func) < 0)
			continue;
		pins[n++] = i;
		groups[n] = ax620e_pads[i].name;
	}

	ret = pinctrl_generic_add_group(pc->pctl, name, pins, n, NULL);
	if (ret < 0)
		return ret;

	ret = pinmux_generic_add_function(pc->pctl, name, groups, n + 1, NULL);
	return ret < 0 ? ret : 0;
}

static int ax620e_pinctrl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ax620e_pinctrl *pc;
	unsigned int i;
	int ret;

	pc = devm_kzalloc(dev, sizeof(*pc), GFP_KERNEL);
	if (!pc)
		return -ENOMEM;

	pc->dev = dev;

	for (i = 0; i < ARRAY_SIZE(pc->base); i++) {
		pc->base[i] = devm_platform_ioremap_resource(pdev, i);
		if (IS_ERR(pc->base[i]))
			return PTR_ERR(pc->base[i]);
	}

	for (i = 0; i < AX620E_NUM_PADS; i++) {
		pc->pins[i].number = i;
		pc->pins[i].name = ax620e_pads[i].name;
	}

	pc->desc = (struct pinctrl_desc) {
		.name = "ax620e-pinctrl",
		.pins = pc->pins,
		.npins = AX620E_NUM_PADS,
		.pctlops = &ax620e_pinctrl_ops,
		.pmxops = &ax620e_pinmux_ops,
		.confops = &ax620e_pinconf_ops,
		.owner = THIS_MODULE,
	};

	/* Groups and functions need a registered pinctrl_dev; enable after. */
	ret = devm_pinctrl_register_and_init(dev, &pc->desc, pc, &pc->pctl);
	if (ret)
		return ret;

	for (i = 0; i < AX620E_NUM_PADS; i++) {
		ret = pinctrl_generic_add_group(pc->pctl, pc->pins[i].name,
						&pc->pins[i].number, 1, NULL);
		if (ret < 0)
			return ret;
	}

	for (i = 0; i < AX620E_NUM_FUNCTIONS; i++) {
		ret = ax620e_add_function(pc, i);
		if (ret)
			return ret;
	}

	return pinctrl_enable(pc->pctl);
}

static const struct of_device_id ax620e_pinctrl_of_match[] = {
	{ .compatible = "axera,ax620e-pinctrl" },
	{ }
};
MODULE_DEVICE_TABLE(of, ax620e_pinctrl_of_match);

static struct platform_driver ax620e_pinctrl_driver = {
	.probe = ax620e_pinctrl_probe,
	.driver = {
		.name = "ax620e-pinctrl",
		.of_match_table = ax620e_pinctrl_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(ax620e_pinctrl_driver);
