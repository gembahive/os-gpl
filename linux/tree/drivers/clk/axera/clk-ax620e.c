// SPDX-License-Identifier: GPL-2.0
/*
 * Axera AX620E clock and reset controllers. The tree is split over several
 * syscon windows; one driver serves them all, match data picks the table.
 * Only what the DT consumers need is registered: clk_disable_unused never
 * touches a clock that is not, which keeps the rest on.
 * Rates and parents stay as the boot chain set them, so muxes and dividers
 * are read-only, except the I2C source and SDIO card clock assigned by DT.
 *
 * Copyright (c) 2026 the open-nanokvm-pro contributors.
 * Copyright (c) 2026, GembaHive, Inc.
 */

#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/mfd/syscon.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/reset-controller.h>

#include <dt-bindings/clock/axera,ax620e-clk.h>
#include <dt-bindings/reset/axera,ax620e-reset.h>

enum ax620e_clk_type {
	AX620E_FIXED,
	AX620E_MUX,
	AX620E_MUX_ASSIGNED,
	AX620E_DIV,
	AX620E_DIV_ASSIGNED,
	AX620E_GATE,
};

struct ax620e_clk {
	enum ax620e_clk_type type;
	const char *name;
	const char *parent;		/* NULL: source unknown, nothing reads its rate */
	const char * const *parents;	/* muxes */
	u8 num_parents;
	unsigned long rate;		/* fixed */
	unsigned long flags;
	u8 offset, shift, width;
};

/* A reset line is a bit in a value word: 1 holds the block in reset. */
struct ax620e_reset_line {
	u16 reg;
	u8 bit;
	bool gated;		/* release with its APB clock stopped */
	u8 gate_reg, gate_bit;
};

/*
 * Every word has write-1-to-set and write-1-to-clear aliases at
 * word * alias_mul + alias_set/clr. Writing those never touches the bits
 * firmware or another driver owns in the same word.
 */
struct ax620e_clk_desc {
	const struct ax620e_clk *clks;
	unsigned int num_clks;
	const struct ax620e_reset_line *resets;
	unsigned int num_resets;
	u32 alias_mul, alias_set, alias_clr;
};

/* Fixed taps of PLLs the boot chain locks and Linux never reprograms. */
#define FIXED(_name, _rate) \
	{ .type = AX620E_FIXED, .name = (_name), .rate = (_rate) }

#define MUX(_name, _parents, _off, _shift, _width)			\
	{ .type = AX620E_MUX, .name = (_name), .parents = (_parents),	\
	  .num_parents = ARRAY_SIZE(_parents),				\
	  .offset = (_off), .shift = (_shift), .width = (_width) }

#define MUX_ASSIGNED(_name, _parents, _off, _shift, _width)			\
	{ .type = AX620E_MUX_ASSIGNED, .name = (_name), .parents = (_parents),	\
	  .num_parents = ARRAY_SIZE(_parents),				\
	  .offset = (_off), .shift = (_shift), .width = (_width) }

#define DIV(_name, _parent, _off, _shift, _width)			\
	{ .type = AX620E_DIV, .name = (_name), .parent = (_parent),	\
	  .offset = (_off), .shift = (_shift), .width = (_width) }

#define DIV_ASSIGNED(_name, _parent, _off, _shift, _width)		\
	{ .type = AX620E_DIV_ASSIGNED, .name = (_name), .parent = (_parent), \
	  .offset = (_off), .shift = (_shift), .width = (_width) }

#define GATE(_name, _parent, _off, _bit)				\
	{ .type = AX620E_GATE, .name = (_name), .parent = (_parent),	\
	  .offset = (_off), .shift = (_bit), .width = 1 }

#define CRITICAL_GATE(_name, _parent, _off, _bit)			\
	{ .type = AX620E_GATE, .name = (_name), .parent = (_parent),	\
	  .offset = (_off), .shift = (_bit), .width = 1,		\
	  .flags = CLK_IS_CRITICAL }

struct ax620e_hw {
	struct clk_hw hw;
	struct regmap *regmap;
	const struct ax620e_clk_desc *desc;
	const struct ax620e_clk *info;
};

#define to_ax620e_hw(_hw) container_of(_hw, struct ax620e_hw, hw)

static int ax620e_write_bit(struct regmap *regmap, const struct ax620e_clk_desc *desc,
			    u32 offset, u8 bit, bool set)
{
	return regmap_write(regmap, offset * desc->alias_mul +
			    (set ? desc->alias_set : desc->alias_clr), BIT(bit));
}

static u32 ax620e_field(struct ax620e_hw *c)
{
	unsigned int v = 0;

	regmap_read(c->regmap, c->info->offset, &v);
	return (v >> c->info->shift) & (BIT(c->info->width) - 1);
}

static int ax620e_gate_enable(struct clk_hw *hw)
{
	struct ax620e_hw *c = to_ax620e_hw(hw);

	return ax620e_write_bit(c->regmap, c->desc, c->info->offset, c->info->shift, true);
}

static void ax620e_gate_disable(struct clk_hw *hw)
{
	struct ax620e_hw *c = to_ax620e_hw(hw);

	ax620e_write_bit(c->regmap, c->desc, c->info->offset, c->info->shift, false);
}

static int ax620e_gate_is_enabled(struct clk_hw *hw)
{
	return ax620e_field(to_ax620e_hw(hw));
}

static const struct clk_ops ax620e_gate_ops = {
	.enable = ax620e_gate_enable,
	.disable = ax620e_gate_disable,
	.is_enabled = ax620e_gate_is_enabled,
};

static u8 ax620e_mux_get_parent(struct clk_hw *hw)
{
	return ax620e_field(to_ax620e_hw(hw));
}

static const struct clk_ops ax620e_mux_ops = {
	.get_parent = ax620e_mux_get_parent,
};

/* Only the I2C source and SDIO card muxes are assigned by DT. */
static int ax620e_mux_set_parent(struct clk_hw *hw, u8 index)
{
	struct ax620e_hw *c = to_ax620e_hw(hw);
	u32 mask = (BIT(c->info->width) - 1) << c->info->shift;
	u32 val = index << c->info->shift;
	int ret;

	ret = regmap_write(c->regmap, c->info->offset * c->desc->alias_mul +
			   c->desc->alias_set, val);
	return ret ?: regmap_write(c->regmap, c->info->offset * c->desc->alias_mul +
				  c->desc->alias_clr, mask & ~val);
}

static const struct clk_ops ax620e_mux_assigned_ops = {
	.get_parent = ax620e_mux_get_parent,
	.set_parent = ax620e_mux_set_parent,
	.determine_rate = clk_hw_determine_rate_no_reparent,
};

/* Divider field n divides by 2n, and 0 by 1. */
static unsigned long ax620e_div_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	u32 val = ax620e_field(to_ax620e_hw(hw));

	return val ? parent_rate / (val * 2) : parent_rate;
}

static const struct clk_ops ax620e_div_ops = {
	.recalc_rate = ax620e_div_recalc_rate,
};

static u32 ax620e_div_val(struct clk_hw *hw, unsigned long rate, unsigned long parent_rate)
{
	if (!rate || rate >= parent_rate)
		return 0;
	return min_t(unsigned long, DIV_ROUND_UP(parent_rate, 2 * rate),
		     BIT(to_ax620e_hw(hw)->info->width) - 1);
}

static int ax620e_div_determine_rate(struct clk_hw *hw, struct clk_rate_request *req)
{
	u32 val = ax620e_div_val(hw, req->rate, req->best_parent_rate);

	req->rate = val ? req->best_parent_rate / (val * 2) : req->best_parent_rate;
	return 0;
}

/* Program only the divider field, then pulse its update bit (as the vendor driver does). */
static int ax620e_div_set_rate(struct clk_hw *hw, unsigned long rate, unsigned long parent_rate)
{
	struct ax620e_hw *c = to_ax620e_hw(hw);
	u32 shift = c->info->shift, update = shift + c->info->width;
	u32 mask = GENMASK(update - 1, shift);
	u32 val = ax620e_div_val(hw, rate, parent_rate) << shift;
	int ret;

	ret = regmap_write(c->regmap, c->info->offset * c->desc->alias_mul +
			   c->desc->alias_set, val);
	ret = ret ?: regmap_write(c->regmap, c->info->offset * c->desc->alias_mul +
				 c->desc->alias_clr, mask & ~val);
	ret = ret ?: ax620e_write_bit(c->regmap, c->desc, c->info->offset, update, true);
	udelay(2);
	return ret ?: ax620e_write_bit(c->regmap, c->desc, c->info->offset, update, false);
}

static const struct clk_ops ax620e_div_assigned_ops = {
	.recalc_rate = ax620e_div_recalc_rate,
	.determine_rate = ax620e_div_determine_rate,
	.set_rate = ax620e_div_set_rate,
};

struct ax620e_reset {
	struct reset_controller_dev rcdev;
	struct regmap *regmap;
	const struct ax620e_clk_desc *desc;
};

static int ax620e_reset_set(struct reset_controller_dev *rcdev, unsigned long id, bool assert)
{
	struct ax620e_reset *rst = container_of(rcdev, struct ax620e_reset, rcdev);
	const struct ax620e_reset_line *line = &rst->desc->resets[id];

	return ax620e_write_bit(rst->regmap, rst->desc, line->reg, line->bit, assert);
}

static int ax620e_reset_assert(struct reset_controller_dev *rcdev, unsigned long id)
{
	return ax620e_reset_set(rcdev, id, true);
}

static int ax620e_reset_deassert(struct reset_controller_dev *rcdev, unsigned long id)
{
	struct ax620e_reset *rst = container_of(rcdev, struct ax620e_reset, rcdev);
	const struct ax620e_reset_line *line = &rst->desc->resets[id];
	unsigned int v;
	int ret, restore;

	if (!line->gated)
		return ax620e_reset_set(rcdev, id, false);
	ret = regmap_read(rst->regmap, line->gate_reg, &v);
	if (ret)
		return ret;
	if (!(v & BIT(line->gate_bit)))
		return ax620e_reset_set(rcdev, id, false);
	ret = ax620e_write_bit(rst->regmap, rst->desc, line->gate_reg, line->gate_bit, false);
	if (ret)
		return ret;
	ret = ax620e_reset_set(rcdev, id, false);
	restore = ax620e_write_bit(rst->regmap, rst->desc, line->gate_reg, line->gate_bit, true);
	return ret ?: restore;
}

static const struct reset_control_ops ax620e_reset_ops = {
	.assert = ax620e_reset_assert,
	.deassert = ax620e_reset_deassert,
};

/* common, 0x2340000 */
static const char * const pclk_top_sel_parents[] = {
	"cpll_24m", "epll_100m", "cpll_156m", "cpll_208m",
};

static const struct ax620e_clk common_clks[] = {
	[AX620E_CLK_THM_EB] = GATE("clk_thm_eb", NULL, 0x30, 10),
	[AX620E_PCLK_THM_EB] = GATE("pclk_thm_eb", "pclk_top_sel", 0x30, 14),
	[AX620E_EPLL_50M] = FIXED("epll_50m", 50000000),
	[AX620E_RTC_32K] = FIXED("rtc_out_32k", 32768),
	[AX620E_CPLL_24M] = FIXED("cpll_24m", 24000000),
	[AX620E_CPLL_156M] = FIXED("cpll_156m", 156000000),
	[AX620E_CPLL_208M] = FIXED("cpll_208m", 208000000),
	[AX620E_CPLL_312M] = FIXED("cpll_312m", 312000000),
	[AX620E_EPLL_100M] = FIXED("epll_100m", 100000000),
	[AX620E_EPLL_250M] = FIXED("epll_250m", 250000000),
	[AX620E_EPLL_375M] = FIXED("epll_375m", 375000000),
	[AX620E_NPLL_400M] = FIXED("npll_400m", 400000000),
	[AX620E_PCLK_TOP_SEL] = MUX("pclk_top_sel", pclk_top_sel_parents, 0x18, 0, 2),
	/* The capture path's domain gates. */
	[AX620E_CLK_DPHYRX_TLB_EB] = GATE("clk_dphyrx_tlb_eb", "cpll_24m", 0x24, 9),
	[AX620E_CLK_ISP_MM_EB] = GATE("clk_isp_mm_eb", NULL, 0x24, 11),
	[AX620E_CLK_VI_EB] = GATE("clk_vi_eb", NULL, 0x24, 17),
};

static const struct ax620e_reset_line common_resets[] = {
	[AX620E_RST_COMMON_THM0] = { 0x54, 19 },
	[AX620E_RST_COMMON_THM1] = { 0x54, 20 },
	/* Released by the vendor MIPI RX module; no vendor DT binds it. */
	[AX620E_RST_COMMON_DPHYRX_TLB] = { 0x54, 7 },
	/* The D-PHY RX analog power: set = off / not ready, as resets read. */
	[AX620E_RST_COMMON_DPHYRX_PWR_OFF] = { 0x1e8, 0 },
	[AX620E_RST_COMMON_DPHYRX_PWR_READY] = { 0x1f4, 0 },
};

static const struct ax620e_clk_desc common_desc = {
	.clks = common_clks,
	.num_clks = ARRAY_SIZE(common_clks),
	.resets = common_resets,
	.num_resets = ARRAY_SIZE(common_resets),
	.alias_mul = 1, .alias_set = 0x4, .alias_clr = 0x8,
};

/* flash, 0x10030000 */
static const char * const clk_flash_glb_sel_parents[] = {
	"cpll_24m", "epll_100m", "cpll_156m", "cpll_208m", "epll_250m", "cpll_312m",
};

/*
 * Only value 3 (npll_400m, which the vendor driver forces) is cited for the SD
 * card mux; 0-2 are assumed to match the documented eMMC card mux beside it.
 */
static const char * const clk_sd_card_sel_parents[] = {
	"cpll_24m", "cpll_312m", "epll_375m", "npll_400m",
};

static const struct ax620e_clk flash_clks[] = {
	[AX620E_CLK_FLASH_GLB_SEL] = MUX("clk_flash_glb_sel", clk_flash_glb_sel_parents, 0x00, 6, 3),
	[AX620E_CLK_SD_CARD_SEL] = MUX("clk_sd_card_sel", clk_sd_card_sel_parents, 0x00, 16, 2),
	[AX620E_CLK_SD_CARD_DIVN] = DIV("clk_sd_card_divn", "clk_sd_card_sel", 0x0c, 20, 6),
	[AX620E_CLK_SD_CARD_EB] = GATE("clk_sd_card_eb", "clk_sd_card_divn", 0x04, 9),
	/* dwc3 derives REFCLKPER/GFLADJ from this 24 MHz rate. */
	[AX620E_CLK_USB_REF_EB] = GATE("clk_usb_ref_eb", "cpll_24m", 0x04, 12),
	[AX620E_BUS_CLK_USB_EB] = GATE("bus_clk_usb_eb", "clk_flash_glb_sel", 0x08, 5),
	/* Unknown source rate; the USB glue only holds this gate enabled. */
	[AX620E_USB_REF_ALT_CLK_EB] = GATE("usb_ref_alt_clk_eb", NULL, 0x04, 14),
	/* SDIO runs from npll_400m / 2, selected before its host is enabled. */
	[AX620E_CLK_SDIO_CARD_SEL] = MUX_ASSIGNED("clk_sdio_card_sel", clk_sd_card_sel_parents, 0x00, 18, 2),
	[AX620E_CLK_SDIO_CARD_DIVN] = DIV_ASSIGNED("clk_sdio_card_divn", "clk_sdio_card_sel", 0x10, 0, 6),
	[AX620E_CLK_SDIO_CARD_EB] = GATE("clk_sdio_card_eb", "clk_sdio_card_divn", 0x04, 10),
	[AX620E_ACLK_SDIO_EB] = GATE("aclk_sdio_eb", NULL, 0x08, 4),
	[AX620E_PCLK_SDIO_EB] = GATE("pclk_sdio_eb", NULL, 0x08, 18),
};

static const struct ax620e_reset_line flash_resets[] = {
	[AX620E_RST_FLASH_USB2_PHY] = { 0x14, 24 },
	[AX620E_RST_FLASH_USB2_VCC] = { 0x14, 25 },
	[AX620E_RST_FLASH_SDIO_CARD] = { 0x14, 18 },
	[AX620E_RST_FLASH_SDIO_A] = { 0x14, 19 },
	[AX620E_RST_FLASH_SDIO_P] = { 0x14, 20 },
};

static const struct ax620e_clk_desc flash_desc = {
	.clks = flash_clks,
	.num_clks = ARRAY_SIZE(flash_clks),
	.resets = flash_resets,
	.num_resets = ARRAY_SIZE(flash_resets),
	.alias_mul = 1, .alias_set = 0x4000, .alias_clr = 0x8000,
};

/* vpu, 0x4030000. The source mux at 0x00 [2:0] stays as the boot chain left it. */
static const struct ax620e_clk vpu_clks[] = {
	[AX620E_CLK_JENC_EB] = GATE("clk_jenc_eb", NULL, 0x08, 4),
};

/* The JPEG clock stops across the reset release (vendor vpu_reset_async). */
static const struct ax620e_reset_line vpu_resets[] = {
	[AX620E_RST_VPU_JENC] = { 0x0c, 4, .gated = true, .gate_reg = 0x08, .gate_bit = 4 },
};

static const struct ax620e_clk_desc vpu_desc = {
	.clks = vpu_clks,
	.num_clks = ARRAY_SIZE(vpu_clks),
	.resets = vpu_resets,
	.num_resets = ARRAY_SIZE(vpu_resets),
	/* Value word -> set/clear aliases: 0x08 -> 0x114/0x118, 0x0c -> 0x11c/0x120. */
	.alias_mul = 2, .alias_set = 0x104, .alias_clr = 0x108,
};

/* periph, 0x4870000 */
static const char * const rtc_or_24m_parents[] = {
	"rtc_out_32k", "cpll_24m",
};

static const char * const periph_src_parents[] = {
	"cpll_24m", "epll_50m", "cpll_156m", "cpll_208m",
};

static const struct ax620e_clk periph_clks[] = {
	[AX620E_CLK_I2C_SEL] = MUX_ASSIGNED("clk_i2c_sel", periph_src_parents, 0x00, 3, 2),
	[AX620E_CLK_SPI_M2_SEL] = MUX("clk_spi_m2_sel", periph_src_parents, 0x00, 11, 2),
	[AX620E_CLK_TIMER_SEL] = MUX("clk_timer_sel", rtc_or_24m_parents, 0x00, 13, 1),
	[AX620E_CLK_I2C_EB] = GATE("clk_i2c_eb", "clk_i2c_sel", 0x04, 2),
	[AX620E_CLK_SPI_M2_EB] = GATE("clk_spi_m2_eb", "clk_spi_m2_sel", 0x04, 8),
	[AX620E_CLK_TIMER_EB] = GATE("clk_timer_eb", "clk_timer_sel", 0x04, 9),
	[AX620E_CLK_I2C1_EB] = GATE("clk_i2c1_eb", "clk_i2c_eb", 0x08, 9),
	[AX620E_CLK_PWM12_EB] = GATE("clk_pwm12_eb", "clk_timer_eb", 0x08, 25),
	[AX620E_PCLK_I2C1_EB] = GATE("pclk_i2c1_eb", "pclk_top_sel", 0x0c, 18),
	[AX620E_PCLK_SPI_M2_EB] = GATE("pclk_spi_m2_eb", "pclk_top_sel", 0x10, 4),
	[AX620E_PCLK_PWM1_EB] = GATE("pclk_pwm1_eb", "pclk_top_sel", 0x10, 0),
	[AX620E_CLK_I2C0_EB] = GATE("clk_i2c0_eb", "clk_i2c_eb", 0x08, 8),
	[AX620E_PCLK_I2C0_EB] = GATE("pclk_i2c0_eb", "pclk_top_sel", 0x0c, 17),
	[AX620E_CLK_GPIO_SEL] = MUX("clk_gpio_sel", rtc_or_24m_parents, 0x00, 2, 1),
	[AX620E_CLK_WDT0_SEL] = MUX("clk_wdt0_sel", rtc_or_24m_parents, 0x00, 19, 1),
	[AX620E_CLK_GPIO_EB] = GATE("clk_gpio_eb", "clk_gpio_sel", 0x04, 1),
	/* Keep the inherited reset watchdog alive even if its driver never probes
	 * or probe unwinds. Critical leaves also hold their parent dependencies.
	 */
	[AX620E_CLK_WDT0_EB] = CRITICAL_GATE("clk_wdt0_eb", "clk_wdt0_sel", 0x04, 14),
	[AX620E_CLK_GPIO0_EB] = GATE("clk_gpio0_eb", "clk_gpio_eb", 0x08, 4),
	[AX620E_CLK_GPIO1_EB] = GATE("clk_gpio1_eb", "clk_gpio_eb", 0x08, 5),
	[AX620E_CLK_GPIO2_EB] = GATE("clk_gpio2_eb", "clk_gpio_eb", 0x08, 6),
	[AX620E_CLK_GPIO3_EB] = GATE("clk_gpio3_eb", "clk_gpio_eb", 0x08, 7),
	[AX620E_PCLK_GPIO0_EB] = GATE("pclk_gpio0_eb", "pclk_top_sel", 0x0c, 13),
	[AX620E_PCLK_GPIO1_EB] = GATE("pclk_gpio1_eb", "pclk_top_sel", 0x0c, 14),
	[AX620E_PCLK_GPIO2_EB] = GATE("pclk_gpio2_eb", "pclk_top_sel", 0x0c, 15),
	[AX620E_PCLK_GPIO3_EB] = GATE("pclk_gpio3_eb", "pclk_top_sel", 0x0c, 16),
	[AX620E_PCLK_WDT0_EB] = CRITICAL_GATE("pclk_wdt0_eb", "pclk_top_sel", 0x10, 19),
	[AX620E_CLK_CE_CNT_EB] = GATE("clk_ce_cnt_eb", NULL, 0x04, 0),
	[AX620E_ACLK_CE_EB] = GATE("aclk_ce_eb", NULL, 0x08, 1),
	[AX620E_PCLK_CE_EB] = GATE("pclk_ce_eb", "pclk_top_sel", 0x0c, 12),
};

static const struct ax620e_reset_line periph_resets[] = {
	[AX620E_RST_PERIPH_I2C0_PRST] = { 0x18, 18 },
	[AX620E_RST_PERIPH_I2C0] = { 0x18, 19 },
	[AX620E_RST_PERIPH_I2C1_PRST] = { 0x18, 20 },
	[AX620E_RST_PERIPH_I2C1] = { 0x18, 21 },
	[AX620E_RST_PERIPH_PWM1] = { 0x1c, 24 },
	[AX620E_RST_PERIPH_PWM1_CH2] = { 0x1c, 22 },
	[AX620E_RST_PERIPH_GPIO0_PRST] = { 0x18, 10 },
	[AX620E_RST_PERIPH_GPIO0] = { 0x18, 11 },
	[AX620E_RST_PERIPH_GPIO1_PRST] = { 0x18, 12 },
	[AX620E_RST_PERIPH_GPIO1] = { 0x18, 13 },
	[AX620E_RST_PERIPH_GPIO2_PRST] = { 0x18, 14 },
	[AX620E_RST_PERIPH_GPIO2] = { 0x18, 15 },
	[AX620E_RST_PERIPH_GPIO3_PRST] = { 0x18, 16 },
	[AX620E_RST_PERIPH_GPIO3] = { 0x18, 17 },
	[AX620E_RST_PERIPH_WDT0_PRST] = { 0x24, 0 },
	[AX620E_RST_PERIPH_WDT0_ARST] = { 0x24, 1 },
	[AX620E_RST_PERIPH_CE_MAIN] = { 0x18, 4 },
	[AX620E_RST_PERIPH_CE_CNT] = { 0x18, 5 },
	[AX620E_RST_PERIPH_CE_SOFT] = { 0x18, 6 },
	[AX620E_RST_PERIPH_CE_PRST] = { 0x18, 8, .gated = true, .gate_reg = 0x0c, .gate_bit = 12 },
	[AX620E_RST_PERIPH_CE] = { 0x18, 7 },
};

static const struct ax620e_clk_desc periph_desc = {
	.clks = periph_clks,
	.num_clks = ARRAY_SIZE(periph_clks),
	.resets = periph_resets,
	.num_resets = ARRAY_SIZE(periph_resets),
	/* Each word's set/clear pair sits at 0xa8 + 2 * word (MUX0 0xa8/0xac ... SW_RST3 0xf0/0xf4). */
	.alias_mul = 2, .alias_set = 0xa8, .alias_clr = 0xac,
};

/* mm, 0x4430000. The TDP source mux at 0x00 [26:24] stays as the boot chain left it. */
static const struct ax620e_clk mm_clks[] = {
	[AX620E_CLK_TDP_EB] = GATE("clk_tdp_eb", NULL, 0x08, 11),
	[AX620E_PCLK_TDP_EB] = GATE("pclk_tdp_eb", NULL, 0x08, 24),
	[AX620E_CLK_CMD_EB] = GATE("clk_cmd_eb", NULL, 0x08, 3),
	[AX620E_PCLK_CMD_EB] = GATE("pclk_cmd_eb", NULL, 0x08, 19),
};

/* Each line is released with its gate stopped (vendor mm_sys_reset_async). */
static const struct ax620e_reset_line mm_resets[] = {
	[AX620E_RST_MM_TDP_PRST] = { 0x18, 19, .gated = true, .gate_reg = 0x08, .gate_bit = 24 },
	[AX620E_RST_MM_TDP] = { 0x18, 20, .gated = true, .gate_reg = 0x08, .gate_bit = 11 },
};

static const struct ax620e_clk_desc mm_desc = {
	.clks = mm_clks,
	.num_clks = ARRAY_SIZE(mm_clks),
	.resets = mm_resets,
	.num_resets = ARRAY_SIZE(mm_resets),
	/* Value word -> set/clear aliases: 0x08 -> 0xb4/0xb8, 0x18 -> 0xc4/0xc8. */
	.alias_mul = 1, .alias_set = 0xac, .alias_clr = 0xb0,
};

static struct clk_hw *ax620e_register_one(struct device *dev, struct regmap *regmap,
					  const struct ax620e_clk_desc *desc,
					  const struct ax620e_clk *info)
{
	struct clk_init_data init = {
		.name = info->name,
		.flags = info->flags,
		.parent_names = info->parent ? &info->parent : info->parents,
		.num_parents = info->parent ? 1 : info->num_parents,
	};
	struct ax620e_hw *c;
	int ret;

	switch (info->type) {
	case AX620E_FIXED:
		return devm_clk_hw_register_fixed_rate(dev, info->name, NULL, 0, info->rate);
	case AX620E_MUX:
		init.ops = &ax620e_mux_ops;
		break;
	case AX620E_MUX_ASSIGNED:
		init.ops = &ax620e_mux_assigned_ops;
		break;
	case AX620E_DIV:
		init.ops = &ax620e_div_ops;
		break;
	case AX620E_DIV_ASSIGNED:
		init.ops = &ax620e_div_assigned_ops;
		break;
	case AX620E_GATE:
		init.ops = &ax620e_gate_ops;
		break;
	}

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return ERR_PTR(-ENOMEM);
	c->regmap = regmap;
	c->desc = desc;
	c->info = info;
	c->hw.init = &init;

	ret = devm_clk_hw_register(dev, &c->hw);
	return ret ? ERR_PTR(ret) : &c->hw;
}

static int ax620e_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct ax620e_clk_desc *desc = device_get_match_data(dev);
	struct clk_hw_onecell_data *onecell;
	struct ax620e_reset *rst;
	struct regmap *regmap;
	unsigned int i;
	int ret;

	/* The windows are syscons: pinctrl and the USB glue share them. */
	regmap = syscon_node_to_regmap(dev->of_node);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap), "no syscon regmap\n");

	onecell = devm_kzalloc(dev, struct_size(onecell, hws, desc->num_clks), GFP_KERNEL);
	if (!onecell)
		return -ENOMEM;
	onecell->num = desc->num_clks;

	for (i = 0; i < desc->num_clks; i++) {
		onecell->hws[i] = ax620e_register_one(dev, regmap, desc, &desc->clks[i]);
		if (IS_ERR(onecell->hws[i]))
			return dev_err_probe(dev, PTR_ERR(onecell->hws[i]), "%s\n",
					     desc->clks[i].name);
	}

	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, onecell);
	if (ret || !desc->num_resets)
		return ret;

	rst = devm_kzalloc(dev, sizeof(*rst), GFP_KERNEL);
	if (!rst)
		return -ENOMEM;
	rst->regmap = regmap;
	rst->desc = desc;
	rst->rcdev.ops = &ax620e_reset_ops;
	rst->rcdev.of_node = dev->of_node;
	rst->rcdev.of_reset_n_cells = 1;
	rst->rcdev.nr_resets = desc->num_resets;
	return devm_reset_controller_register(dev, &rst->rcdev);
}

static const struct of_device_id ax620e_clk_of_match[] = {
	{ .compatible = "axera,ax620e-common-clk", .data = &common_desc },
	{ .compatible = "axera,ax620e-flash-clk",  .data = &flash_desc },
	{ .compatible = "axera,ax620e-periph-clk", .data = &periph_desc },
	{ .compatible = "axera,ax620e-mm-clk",     .data = &mm_desc },
	{ .compatible = "axera,ax620e-vpu-clk",    .data = &vpu_desc },
	{ }
};

static struct platform_driver ax620e_clk_driver = {
	.probe = ax620e_clk_probe,
	.driver = {
		.name = "ax620e-clk",
		.of_match_table = ax620e_clk_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(ax620e_clk_driver);
