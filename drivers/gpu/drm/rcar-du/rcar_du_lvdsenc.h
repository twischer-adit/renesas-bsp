/*
 * rcar_du_lvdsenc.h  --  R-Car Display Unit LVDS Encoder
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

#ifndef __RCAR_DU_LVDSENC_H__
#define __RCAR_DU_LVDSENC_H__

#include <linux/io.h>
#include <linux/module.h>

struct rcar_drm_crtc;
struct rcar_du_lvdsenc;

enum rcar_lvds_input {
	RCAR_LVDS_INPUT_DU0,
	RCAR_LVDS_INPUT_DU1,
	RCAR_LVDS_INPUT_DU2,
};

/* Keep in sync with the LVDCR0.LVMD hardware register values. */
enum rcar_lvds_mode {
	RCAR_LVDS_MODE_JEIDA = 0,
	RCAR_LVDS_MODE_MIRROR = 1,
	RCAR_LVDS_MODE_VESA = 4,
};

enum rcar_lvds_link_mode {
	RCAR_LVDS_SINGLE = 0,
	RCAR_LVDS_DUAL,
};

#if IS_ENABLED(CONFIG_DRM_RCAR_LVDS)
int rcar_du_lvdsenc_init(struct rcar_du_device *rcdu);
void rcar_du_lvdsenc_set_mode(struct rcar_du_lvdsenc *lvds,
			      enum rcar_lvds_mode mode);
int rcar_du_lvdsenc_enable(struct rcar_du_lvdsenc *lvds,
			   struct drm_crtc *crtc, bool enable);
void rcar_du_lvdsenc_atomic_check(struct rcar_du_lvdsenc *lvds,
				  struct drm_display_mode *mode);
void __rcar_du_lvdsenc_stop(struct rcar_du_lvdsenc *lvds);
bool rcar_du_lvdsenc_stop_pll(struct rcar_du_lvdsenc *lvds);
void rcar_du_lvdsenc_pll_pre_start(struct rcar_du_lvdsenc *lvds,
				   struct rcar_du_crtc *rcrtc);
#else
static inline int rcar_du_lvdsenc_init(struct rcar_du_device *rcdu)
{
	return 0;
}
static inline void rcar_du_lvdsenc_set_mode(struct rcar_du_lvdsenc *lvds,
					    enum rcar_lvds_mode mode)
{
}
static inline int rcar_du_lvdsenc_enable(struct rcar_du_lvdsenc *lvds,
					 struct drm_crtc *crtc, bool enable)
{
	return 0;
}
static inline void rcar_du_lvdsenc_atomic_check(struct rcar_du_lvdsenc *lvds,
						struct drm_display_mode *mode)
{
}
static inline void __rcar_du_lvdsenc_stop(struct rcar_du_lvdsenc *lvds)
{
}
static inline bool rcar_du_lvdsenc_stop_pll(struct rcar_du_lvdsenc *lvds)
{
	return 0;
}
static inline void rcar_du_lvdsenc_pll_pre_start(struct rcar_du_lvdsenc *lvds,
						 struct rcar_du_crtc *rcrtc)
{
}
#endif

#endif /* __RCAR_DU_LVDSENC_H__ */
