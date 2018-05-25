/*
 * rcar_du_lvdsenc.c  --  R-Car Display Unit LVDS Encoder
 *
 * Copyright (C) 2013-2017 Renesas Electronics Corporation
 *
 * Contact: Laurent Pinchart (laurent.pinchart@ideasonboard.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/slab.h>

#include "rcar_du_drv.h"
#include "rcar_du_encoder.h"
#include "rcar_du_lvdsenc.h"
#include "rcar_lvds_regs.h"

struct rcar_du_lvdsenc {
	struct rcar_du_device *dev;
	struct reset_control *rstc;

	unsigned int index;
	void __iomem *mmio;
	struct clk *clock;
	bool enabled;

	enum rcar_lvds_input input;
	enum rcar_lvds_mode mode;
};

struct pll_info {
	unsigned int pllclk;
	unsigned int diff;
	unsigned int clk_n;
	unsigned int clk_m;
	unsigned int clk_e;
	unsigned int div;
};

static void rcar_lvds_write(struct rcar_du_lvdsenc *lvds, u32 reg, u32 data)
{
	iowrite32(data, lvds->mmio + reg);
}

static void rcar_du_lvdsenc_start_gen2(struct rcar_du_lvdsenc *lvds,
				       struct rcar_du_crtc *rcrtc)
{
	const struct drm_display_mode *mode = &rcrtc->crtc.mode;
	unsigned int freq = mode->clock;
	u32 lvdcr0;
	u32 pllcr;

	/* PLL clock configuration */
	if (freq < 39000)
		pllcr = LVDPLLCR_CEEN | LVDPLLCR_COSEL | LVDPLLCR_PLLDLYCNT_38M;
	else if (freq < 61000)
		pllcr = LVDPLLCR_CEEN | LVDPLLCR_COSEL | LVDPLLCR_PLLDLYCNT_60M;
	else if (freq < 121000)
		pllcr = LVDPLLCR_CEEN | LVDPLLCR_COSEL | LVDPLLCR_PLLDLYCNT_121M;
	else
		pllcr = LVDPLLCR_PLLDLYCNT_150M;

	rcar_lvds_write(lvds, LVDPLLCR, pllcr);

	/*
	 * Select the input, hardcode mode 0, enable LVDS operation and turn
	 * bias circuitry on.
	 */
	lvdcr0 = (lvds->mode << LVDCR0_LVMD_SHIFT) | LVDCR0_BEN | LVDCR0_LVEN;
	if (rcrtc->index == 2)
		lvdcr0 |= LVDCR0_DUSEL;
	rcar_lvds_write(lvds, LVDCR0, lvdcr0);

	/* Turn all the channels on. */
	rcar_lvds_write(lvds, LVDCR1,
			LVDCR1_CHSTBY_GEN2(3) | LVDCR1_CHSTBY_GEN2(2) |
			LVDCR1_CHSTBY_GEN2(1) | LVDCR1_CHSTBY_GEN2(0) |
			LVDCR1_CLKSTBY_GEN2);

	/*
	 * Turn the PLL on, wait for the startup delay, and turn the output
	 * on.
	 */
	lvdcr0 |= LVDCR0_PLLON;
	rcar_lvds_write(lvds, LVDCR0, lvdcr0);

	usleep_range(100, 150);

	lvdcr0 |= LVDCR0_LVRES;
	rcar_lvds_write(lvds, LVDCR0, lvdcr0);
}

static void rcar_du_lvdsenc_start_gen3(struct rcar_du_lvdsenc *lvds,
				       struct rcar_du_crtc *rcrtc)
{
	const struct drm_display_mode *mode = &rcrtc->crtc.mode;
	unsigned int freq = mode->clock;
	u32 lvdcr0;
	u32 pllcr;

	/* PLL clock configuration */
	if (freq < 42000)
		pllcr = LVDPLLCR_PLLDIVCNT_42M;
	else if (freq < 85000)
		pllcr = LVDPLLCR_PLLDIVCNT_85M;
	else if (freq < 128000)
		pllcr = LVDPLLCR_PLLDIVCNT_128M;
	else
		pllcr = LVDPLLCR_PLLDIVCNT_148M;

	rcar_lvds_write(lvds, LVDPLLCR, pllcr);

	/* Turn all the channels on. */
	rcar_lvds_write(lvds, LVDCR1,
			LVDCR1_CHSTBY_GEN3(3) | LVDCR1_CHSTBY_GEN3(2) |
			LVDCR1_CHSTBY_GEN3(1) | LVDCR1_CHSTBY_GEN3(0) |
			LVDCR1_CLKSTBY_GEN3);

	/*
	 * Turn the PLL on, set it to LVDS normal mode, wait for the startup
	 * delay and turn the output on.
	 */
	lvdcr0 = (lvds->mode << LVDCR0_LVMD_SHIFT) | LVDCR0_PLLON;
	rcar_lvds_write(lvds, LVDCR0, lvdcr0);

	lvdcr0 |= LVDCR0_PWD;
	rcar_lvds_write(lvds, LVDCR0, lvdcr0);

	usleep_range(100, 150);

	lvdcr0 |= LVDCR0_LVRES;
	rcar_lvds_write(lvds, LVDCR0, lvdcr0);
}

static int rcar_du_lvdsenc_start(struct rcar_du_lvdsenc *lvds,
				 struct rcar_du_crtc *rcrtc)
{
	u32 lvdhcr;
	int ret;

	if (lvds->enabled)
		return 0;

	reset_control_deassert(lvds->rstc);

	ret = clk_prepare_enable(lvds->clock);
	if (ret < 0)
		return ret;

	/*
	 * Hardcode the channels and control signals routing for now.
	 *
	 * HSYNC -> CTRL0
	 * VSYNC -> CTRL1
	 * DISP  -> CTRL2
	 * 0     -> CTRL3
	 */
	rcar_lvds_write(lvds, LVDCTRCR, LVDCTRCR_CTR3SEL_ZERO |
			LVDCTRCR_CTR2SEL_DISP | LVDCTRCR_CTR1SEL_VSYNC |
			LVDCTRCR_CTR0SEL_HSYNC);

	if (rcar_du_needs(lvds->dev, RCAR_DU_QUIRK_LVDS_LANES))
		lvdhcr = LVDCHCR_CHSEL_CH(0, 0) | LVDCHCR_CHSEL_CH(1, 3)
		       | LVDCHCR_CHSEL_CH(2, 2) | LVDCHCR_CHSEL_CH(3, 1);
	else
		lvdhcr = LVDCHCR_CHSEL_CH(0, 0) | LVDCHCR_CHSEL_CH(1, 1)
		       | LVDCHCR_CHSEL_CH(2, 2) | LVDCHCR_CHSEL_CH(3, 3);

	rcar_lvds_write(lvds, LVDCHCR, lvdhcr);

	/* Perform generation-specific initialization. */
	if (lvds->dev->info->gen < 3)
		rcar_du_lvdsenc_start_gen2(lvds, rcrtc);
	else
		rcar_du_lvdsenc_start_gen3(lvds, rcrtc);

	lvds->enabled = true;

	return 0;
}

static void rcar_du_lvdsenc_pll_calc(struct rcar_du_crtc *rcrtc,
				     struct pll_info *pll, unsigned int in_fre,
				     unsigned int mode_freq, bool edivider)
{
	unsigned long diff = (unsigned long)-1;
	unsigned long fout, fpfd, fvco, n, m, e, div;
	bool clk_diff_set = true;

	if (in_fre < 12000000 || in_fre > 192000000)
		return;

	for (n = 0; n < 127; n++) {
		if (((n + 1) < 60) || ((n + 1) > 120))
			continue;

		for (m = 0; m < 7; m++) {
			for (e = 0; e < 1; e++) {
				if (edivider)
					fout = (((in_fre / 1000) * (n + 1)) /
						((m + 1) * (e + 1) * 2)) *
						1000;
				else
					fout = (((in_fre / 1000) * (n + 1)) /
						(m + 1)) * 1000;

				if (fout > 1039500000)
					continue;

				fpfd  = (in_fre / (m + 1));
				if (fpfd < 12000000 || fpfd > 24000000)
					continue;

				fvco  = (((in_fre / 1000) * (n + 1)) / (m + 1))
					 * 1000;
				if (fvco < 900000000 || fvco > 1800000000)
					continue;

				fout = fout / 7; /* 7 divider */

				for (div = 0; div < 64; div++) {
					diff = abs((long)(fout / (div + 1)) -
					       (long)mode_freq);

					if (clk_diff_set ||
					    (diff == 0 ||
					    pll->diff > diff)) {
						pll->diff = diff;
						pll->clk_n = n;
						pll->clk_m = m;
						pll->clk_e = e;
						pll->pllclk = fout;
						pll->div = div;

						clk_diff_set = false;

						if (diff == 0)
							goto done;
					}
				}
			}
		}
	}
done:
	return;
}

void rcar_du_lvdsenc_pll_pre_start(struct rcar_du_lvdsenc *lvds,
				   struct rcar_du_crtc *rcrtc)
{
	const struct drm_display_mode *mode =
				&rcrtc->crtc.state->adjusted_mode;
	unsigned int mode_freq = mode->clock * 1000;
	unsigned int extal_freq = 48000000; /* EXTAL 48MHz */
	struct pll_info *lvds_pll[2];
	int i, ret;
	u32 clksel;

	if (lvds->enabled)
		return;

	for (i = 0; i < RCAR_DU_MAX_LVDS; i++) {
		lvds_pll[i] = kzalloc(sizeof(*lvds_pll), GFP_KERNEL);
		if (!lvds_pll[i])
			return;
	}

	/* software reset release */
	reset_control_deassert(lvds->rstc);

	ret = clk_prepare_enable(lvds->clock);
	if (ret < 0)
		goto end;

	for (i = 0; i < 2; i++) {
		bool edivider;

		if (i == 0)
			edivider = true;
		else
			edivider = false;

		rcar_du_lvdsenc_pll_calc(rcrtc, lvds_pll[i], extal_freq,
					 mode_freq, edivider);
	}

	dev_dbg(rcrtc->group->dev->dev, "mode_frequency %d Hz\n", mode_freq);

	if (lvds_pll[1]->diff >= lvds_pll[0]->diff) {
		/* use E-edivider */
		i = 0;
		clksel = LVDPLLCR_OUTCLKSEL_AFTER |
			 LVDPLLCR_STP_CLKOUTE1_EN;
	} else {
		/* not use E-divider */
		i = 1;
		clksel = LVDPLLCR_OUTCLKSEL_BEFORE |
			 LVDPLLCR_STP_CLKOUTE1_DIS;
	}
	dev_dbg(rcrtc->group->dev->dev,
		"E-divider %s\n", (i == 0 ? "is used" : "is not used"));

	dev_dbg(rcrtc->group->dev->dev,
		"pllclk:%u, n:%u, m:%u, e:%u, diff:%u, div:%u\n",
		 lvds_pll[i]->pllclk, lvds_pll[i]->clk_n, lvds_pll[i]->clk_m,
		 lvds_pll[i]->clk_e, lvds_pll[i]->diff, lvds_pll[i]->div);

	rcar_lvds_write(lvds, LVDPLLCR, LVDPLLCR_PLLON |
			LVDPLLCR_OCKSEL_7 | clksel | LVDPLLCR_CLKOUT_ENABLE |
			LVDPLLCR_CKSEL_EXTAL | (lvds_pll[i]->clk_e << 10) |
			(lvds_pll[i]->clk_n << 3) | lvds_pll[i]->clk_m);

	if (lvds_pll[i]->div > 0)
		rcar_lvds_write(lvds, LVDDIV, LVDDIV_DIVSEL |
				LVDDIV_DIVRESET | lvds_pll[i]->div);
	else
		rcar_lvds_write(lvds, LVDDIV, 0);

	dev_dbg(rcrtc->group->dev->dev, "LVDPLLCR: 0x%x\n",
		ioread32(lvds->mmio + LVDPLLCR));
	dev_dbg(rcrtc->group->dev->dev, "LVDDIV: 0x%x\n",
		ioread32(lvds->mmio + LVDDIV));

end:
	for (i = 0; i < RCAR_DU_MAX_LVDS; i++)
		kfree(lvds_pll[i]);
}

static int rcar_du_lvdsenc_pll_start(struct rcar_du_lvdsenc *lvds,
				     struct rcar_du_crtc *rcrtc)
{
	u32 lvdhcr, lvdcr0;

	rcar_lvds_write(lvds, LVDCTRCR, LVDCTRCR_CTR3SEL_ZERO |
			LVDCTRCR_CTR2SEL_DISP | LVDCTRCR_CTR1SEL_VSYNC |
			LVDCTRCR_CTR0SEL_HSYNC);

	lvdhcr = LVDCHCR_CHSEL_CH(0, 0) | LVDCHCR_CHSEL_CH(1, 1) |
		 LVDCHCR_CHSEL_CH(2, 2) | LVDCHCR_CHSEL_CH(3, 3);
	rcar_lvds_write(lvds, LVDCHCR, lvdhcr);

	rcar_lvds_write(lvds, LVDSTRIPE, 0);
	/* Turn all the channels on. */
	rcar_lvds_write(lvds, LVDCR1,
			LVDCR1_CHSTBY_GEN3(3) | LVDCR1_CHSTBY_GEN3(2) |
			LVDCR1_CHSTBY_GEN3(1) | LVDCR1_CHSTBY_GEN3(0) |
			LVDCR1_CLKSTBY_GEN3);
	/*
	 * Turn the PLL on, set it to LVDS normal mode, wait for the startup
	 * delay and turn the output on.
	 */
	lvdcr0 = (lvds->mode << LVDCR0_LVMD_SHIFT) | LVDCR0_PWD;
	rcar_lvds_write(lvds, LVDCR0, lvdcr0);

	lvdcr0 |= LVDCR0_LVEN;
	rcar_lvds_write(lvds, LVDCR0, lvdcr0);

	lvdcr0 |= LVDCR0_LVRES;
	rcar_lvds_write(lvds, LVDCR0, lvdcr0);

	lvds->enabled = true;

	return 0;
}

void __rcar_du_lvdsenc_stop(struct rcar_du_lvdsenc *lvds)
{
	rcar_lvds_write(lvds, LVDCR0, 0);
	rcar_lvds_write(lvds, LVDCR1, 0);
	rcar_lvds_write(lvds, LVDPLLCR, 0);

	clk_disable_unprepare(lvds->clock);

	reset_control_assert(lvds->rstc);

	lvds->enabled = false;
}

static void rcar_du_lvdsenc_stop(struct rcar_du_lvdsenc *lvds)
{
	struct rcar_du_device *rcdu = lvds->dev;

	if (!lvds->enabled || rcar_du_has(rcdu, RCAR_DU_FEATURE_LVDS_PLL))
		return;

	__rcar_du_lvdsenc_stop(lvds);
}

int rcar_du_lvdsenc_enable(struct rcar_du_lvdsenc *lvds, struct drm_crtc *crtc,
			   bool enable)
{
	struct rcar_du_device *rcdu = lvds->dev;

	if (!enable) {
		rcar_du_lvdsenc_stop(lvds);
		return 0;
	} else if (crtc) {
		struct rcar_du_crtc *rcrtc = to_rcar_crtc(crtc);
		if (rcar_du_has(rcdu, RCAR_DU_FEATURE_LVDS_PLL))
			return rcar_du_lvdsenc_pll_start(lvds, rcrtc);
		else
			return rcar_du_lvdsenc_start(lvds, rcrtc);
	} else
		return -EINVAL;
}

void rcar_du_lvdsenc_atomic_check(struct rcar_du_lvdsenc *lvds,
				  struct drm_display_mode *mode)
{
	struct rcar_du_device *rcdu = lvds->dev;

	/*
	 * The internal LVDS encoder has a restricted clock frequency operating
	 * range (30MHz to 150MHz on Gen2, 25.175MHz to 148.5MHz on Gen3). Clamp
	 * the clock accordingly.
	 */
	if (rcdu->info->gen < 3)
		mode->clock = clamp(mode->clock, 30000, 150000);
	else
		mode->clock = clamp(mode->clock, 25175, 148500);
}

void rcar_du_lvdsenc_set_mode(struct rcar_du_lvdsenc *lvds,
			      enum rcar_lvds_mode mode)
{
	lvds->mode = mode;
}

static int rcar_du_lvdsenc_get_resources(struct rcar_du_lvdsenc *lvds,
					 struct platform_device *pdev)
{
	struct resource *mem;
	char name[7];

	sprintf(name, "lvds.%u", lvds->index);

	mem = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	lvds->mmio = devm_ioremap_resource(&pdev->dev, mem);
	if (IS_ERR(lvds->mmio))
		return PTR_ERR(lvds->mmio);

	lvds->clock = devm_clk_get(&pdev->dev, name);
	if (IS_ERR(lvds->clock)) {
		dev_err(&pdev->dev, "failed to get clock for %s\n", name);
		return PTR_ERR(lvds->clock);
	}

	lvds->rstc = devm_reset_control_get(&pdev->dev, name);
	if (IS_ERR(lvds->rstc)) {
		dev_err(&pdev->dev, "failed to get cpg reset %s\n", name);
		return PTR_ERR(lvds->rstc);
	}

	return 0;
}

int rcar_du_lvdsenc_init(struct rcar_du_device *rcdu)
{
	struct platform_device *pdev = to_platform_device(rcdu->dev);
	struct rcar_du_lvdsenc *lvds;
	unsigned int i;
	int ret;

	for (i = 0; i < rcdu->info->num_lvds; ++i) {
		lvds = devm_kzalloc(&pdev->dev, sizeof(*lvds), GFP_KERNEL);
		if (lvds == NULL)
			return -ENOMEM;

		lvds->dev = rcdu;
		lvds->index = i;
		lvds->input = i ? RCAR_LVDS_INPUT_DU1 : RCAR_LVDS_INPUT_DU0;
		lvds->enabled = false;

		ret = rcar_du_lvdsenc_get_resources(lvds, pdev);
		if (ret < 0)
			return ret;

		rcdu->lvds[i] = lvds;
	}

	return 0;
}
