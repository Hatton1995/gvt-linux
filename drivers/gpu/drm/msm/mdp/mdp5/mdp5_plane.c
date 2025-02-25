/*
 * Copyright (C) 2014-2015 The Linux Foundation. All rights reserved.
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <drm/drm_print.h>
#include "mdp5_kms.h"

struct mdp5_plane {
	struct drm_plane base;

	uint32_t nformats;
	uint32_t formats[32];
};
#define to_mdp5_plane(x) container_of(x, struct mdp5_plane, base)

static int mdp5_plane_mode_set(struct drm_plane *plane,
		struct drm_crtc *crtc, struct drm_framebuffer *fb,
		struct drm_rect *src, struct drm_rect *dest);

static int mdp5_update_cursor_plane_legacy(struct drm_plane *plane,
		struct drm_crtc *crtc,
		struct drm_framebuffer *fb,
		int crtc_x, int crtc_y,
		unsigned int crtc_w, unsigned int crtc_h,
		uint32_t src_x, uint32_t src_y,
		uint32_t src_w, uint32_t src_h,
		struct drm_modeset_acquire_ctx *ctx);

static void set_scanout_locked(struct drm_plane *plane,
		struct drm_framebuffer *fb);

static struct mdp5_kms *get_kms(struct drm_plane *plane)
{
	struct msm_drm_private *priv = plane->dev->dev_private;
	return to_mdp5_kms(to_mdp_kms(priv->kms));
}

static bool plane_enabled(struct drm_plane_state *state)
{
	return state->visible;
}

static void mdp5_plane_destroy(struct drm_plane *plane)
{
	struct mdp5_plane *mdp5_plane = to_mdp5_plane(plane);

	drm_plane_helper_disable(plane);
	drm_plane_cleanup(plane);

	kfree(mdp5_plane);
}

static void mdp5_plane_install_rotation_property(struct drm_device *dev,
		struct drm_plane *plane)
{
	drm_plane_create_rotation_property(plane,
					   DRM_ROTATE_0,
					   DRM_ROTATE_0 |
					   DRM_ROTATE_180 |
					   DRM_REFLECT_X |
					   DRM_REFLECT_Y);
}

/* helper to install properties which are common to planes and crtcs */
static void mdp5_plane_install_properties(struct drm_plane *plane,
		struct drm_mode_object *obj)
{
	struct drm_device *dev = plane->dev;
	struct msm_drm_private *dev_priv = dev->dev_private;
	struct drm_property *prop;

#define INSTALL_PROPERTY(name, NAME, init_val, fnc, ...) do { \
		prop = dev_priv->plane_property[PLANE_PROP_##NAME]; \
		if (!prop) { \
			prop = drm_property_##fnc(dev, 0, #name, \
				##__VA_ARGS__); \
			if (!prop) { \
				dev_warn(dev->dev, \
					"Create property %s failed\n", \
					#name); \
				return; \
			} \
			dev_priv->plane_property[PLANE_PROP_##NAME] = prop; \
		} \
		drm_object_attach_property(&plane->base, prop, init_val); \
	} while (0)

#define INSTALL_RANGE_PROPERTY(name, NAME, min, max, init_val) \
		INSTALL_PROPERTY(name, NAME, init_val, \
				create_range, min, max)

#define INSTALL_ENUM_PROPERTY(name, NAME, init_val) \
		INSTALL_PROPERTY(name, NAME, init_val, \
				create_enum, name##_prop_enum_list, \
				ARRAY_SIZE(name##_prop_enum_list))

	INSTALL_RANGE_PROPERTY(zpos, ZPOS, 1, 255, 1);

	mdp5_plane_install_rotation_property(dev, plane);

#undef INSTALL_RANGE_PROPERTY
#undef INSTALL_ENUM_PROPERTY
#undef INSTALL_PROPERTY
}

static int mdp5_plane_atomic_set_property(struct drm_plane *plane,
		struct drm_plane_state *state, struct drm_property *property,
		uint64_t val)
{
	struct drm_device *dev = plane->dev;
	struct mdp5_plane_state *pstate;
	struct msm_drm_private *dev_priv = dev->dev_private;
	int ret = 0;

	pstate = to_mdp5_plane_state(state);

#define SET_PROPERTY(name, NAME, type) do { \
		if (dev_priv->plane_property[PLANE_PROP_##NAME] == property) { \
			pstate->name = (type)val; \
			DBG("Set property %s %d", #name, (type)val); \
			goto done; \
		} \
	} while (0)

	SET_PROPERTY(zpos, ZPOS, uint8_t);

	dev_err(dev->dev, "Invalid property\n");
	ret = -EINVAL;
done:
	return ret;
#undef SET_PROPERTY
}

static int mdp5_plane_atomic_get_property(struct drm_plane *plane,
		const struct drm_plane_state *state,
		struct drm_property *property, uint64_t *val)
{
	struct drm_device *dev = plane->dev;
	struct mdp5_plane_state *pstate;
	struct msm_drm_private *dev_priv = dev->dev_private;
	int ret = 0;

	pstate = to_mdp5_plane_state(state);

#define GET_PROPERTY(name, NAME, type) do { \
		if (dev_priv->plane_property[PLANE_PROP_##NAME] == property) { \
			*val = pstate->name; \
			DBG("Get property %s %lld", #name, *val); \
			goto done; \
		} \
	} while (0)

	GET_PROPERTY(zpos, ZPOS, uint8_t);

	dev_err(dev->dev, "Invalid property\n");
	ret = -EINVAL;
done:
	return ret;
#undef SET_PROPERTY
}

static void
mdp5_plane_atomic_print_state(struct drm_printer *p,
		const struct drm_plane_state *state)
{
	struct mdp5_plane_state *pstate = to_mdp5_plane_state(state);

	drm_printf(p, "\thwpipe=%s\n", pstate->hwpipe ?
			pstate->hwpipe->name : "(null)");
	drm_printf(p, "\tpremultiplied=%u\n", pstate->premultiplied);
	drm_printf(p, "\tzpos=%u\n", pstate->zpos);
	drm_printf(p, "\talpha=%u\n", pstate->alpha);
	drm_printf(p, "\tstage=%s\n", stage2name(pstate->stage));
}

static void mdp5_plane_reset(struct drm_plane *plane)
{
	struct mdp5_plane_state *mdp5_state;

	if (plane->state && plane->state->fb)
		drm_framebuffer_unreference(plane->state->fb);

	kfree(to_mdp5_plane_state(plane->state));
	mdp5_state = kzalloc(sizeof(*mdp5_state), GFP_KERNEL);

	/* assign default blend parameters */
	mdp5_state->alpha = 255;
	mdp5_state->premultiplied = 0;

	if (plane->type == DRM_PLANE_TYPE_PRIMARY)
		mdp5_state->zpos = STAGE_BASE;
	else
		mdp5_state->zpos = STAGE0 + drm_plane_index(plane);

	mdp5_state->base.plane = plane;

	plane->state = &mdp5_state->base;
}

static struct drm_plane_state *
mdp5_plane_duplicate_state(struct drm_plane *plane)
{
	struct mdp5_plane_state *mdp5_state;

	if (WARN_ON(!plane->state))
		return NULL;

	mdp5_state = kmemdup(to_mdp5_plane_state(plane->state),
			sizeof(*mdp5_state), GFP_KERNEL);

	if (mdp5_state && mdp5_state->base.fb)
		drm_framebuffer_reference(mdp5_state->base.fb);

	return &mdp5_state->base;
}

static void mdp5_plane_destroy_state(struct drm_plane *plane,
		struct drm_plane_state *state)
{
	struct mdp5_plane_state *pstate = to_mdp5_plane_state(state);

	if (state->fb)
		drm_framebuffer_unreference(state->fb);

	kfree(pstate);
}

static const struct drm_plane_funcs mdp5_plane_funcs = {
		.update_plane = drm_atomic_helper_update_plane,
		.disable_plane = drm_atomic_helper_disable_plane,
		.destroy = mdp5_plane_destroy,
		.set_property = drm_atomic_helper_plane_set_property,
		.atomic_set_property = mdp5_plane_atomic_set_property,
		.atomic_get_property = mdp5_plane_atomic_get_property,
		.reset = mdp5_plane_reset,
		.atomic_duplicate_state = mdp5_plane_duplicate_state,
		.atomic_destroy_state = mdp5_plane_destroy_state,
		.atomic_print_state = mdp5_plane_atomic_print_state,
};

static const struct drm_plane_funcs mdp5_cursor_plane_funcs = {
		.update_plane = mdp5_update_cursor_plane_legacy,
		.disable_plane = drm_atomic_helper_disable_plane,
		.destroy = mdp5_plane_destroy,
		.set_property = drm_atomic_helper_plane_set_property,
		.atomic_set_property = mdp5_plane_atomic_set_property,
		.atomic_get_property = mdp5_plane_atomic_get_property,
		.reset = mdp5_plane_reset,
		.atomic_duplicate_state = mdp5_plane_duplicate_state,
		.atomic_destroy_state = mdp5_plane_destroy_state,
		.atomic_print_state = mdp5_plane_atomic_print_state,
};

static int mdp5_plane_prepare_fb(struct drm_plane *plane,
				 struct drm_plane_state *new_state)
{
	struct mdp5_kms *mdp5_kms = get_kms(plane);
	struct drm_framebuffer *fb = new_state->fb;

	if (!new_state->fb)
		return 0;

	DBG("%s: prepare: FB[%u]", plane->name, fb->base.id);
	return msm_framebuffer_prepare(fb, mdp5_kms->id);
}

static void mdp5_plane_cleanup_fb(struct drm_plane *plane,
				  struct drm_plane_state *old_state)
{
	struct mdp5_kms *mdp5_kms = get_kms(plane);
	struct drm_framebuffer *fb = old_state->fb;

	if (!fb)
		return;

	DBG("%s: cleanup: FB[%u]", plane->name, fb->base.id);
	msm_framebuffer_cleanup(fb, mdp5_kms->id);
}

#define FRAC_16_16(mult, div)    (((mult) << 16) / (div))
static int mdp5_plane_atomic_check_with_state(struct drm_crtc_state *crtc_state,
					      struct drm_plane_state *state)
{
	struct mdp5_plane_state *mdp5_state = to_mdp5_plane_state(state);
	struct drm_plane *plane = state->plane;
	struct drm_plane_state *old_state = plane->state;
	struct mdp5_cfg *config = mdp5_cfg_get_config(get_kms(plane)->cfg);
	bool new_hwpipe = false;
	uint32_t max_width, max_height;
	uint32_t caps = 0;
	struct drm_rect clip;
	int min_scale, max_scale;
	int ret;

	DBG("%s: check (%d -> %d)", plane->name,
			plane_enabled(old_state), plane_enabled(state));

	max_width = config->hw->lm.max_width << 16;
	max_height = config->hw->lm.max_height << 16;

	/* Make sure source dimensions are within bounds. */
	if ((state->src_w > max_width) || (state->src_h > max_height)) {
		struct drm_rect src = drm_plane_state_src(state);
		DBG("Invalid source size "DRM_RECT_FP_FMT,
				DRM_RECT_FP_ARG(&src));
		return -ERANGE;
	}

	clip.x1 = 0;
	clip.y1 = 0;
	clip.x2 = crtc_state->adjusted_mode.hdisplay;
	clip.y2 = crtc_state->adjusted_mode.vdisplay;
	min_scale = FRAC_16_16(1, 8);
	max_scale = FRAC_16_16(8, 1);

	ret = drm_plane_helper_check_state(state, &clip, min_scale,
					   max_scale, true, true);
	if (ret)
		return ret;

	if (plane_enabled(state)) {
		unsigned int rotation;
		const struct mdp_format *format;
		struct mdp5_kms *mdp5_kms = get_kms(plane);
		uint32_t blkcfg = 0;

		format = to_mdp_format(msm_framebuffer_format(state->fb));
		if (MDP_FORMAT_IS_YUV(format))
			caps |= MDP_PIPE_CAP_SCALE | MDP_PIPE_CAP_CSC;

		if (((state->src_w >> 16) != state->crtc_w) ||
				((state->src_h >> 16) != state->crtc_h))
			caps |= MDP_PIPE_CAP_SCALE;

		rotation = drm_rotation_simplify(state->rotation,
						 DRM_ROTATE_0 |
						 DRM_REFLECT_X |
						 DRM_REFLECT_Y);

		if (rotation & DRM_REFLECT_X)
			caps |= MDP_PIPE_CAP_HFLIP;

		if (rotation & DRM_REFLECT_Y)
			caps |= MDP_PIPE_CAP_VFLIP;

		if (plane->type == DRM_PLANE_TYPE_CURSOR)
			caps |= MDP_PIPE_CAP_CURSOR;

		/* (re)allocate hw pipe if we don't have one or caps-mismatch: */
		if (!mdp5_state->hwpipe || (caps & ~mdp5_state->hwpipe->caps))
			new_hwpipe = true;

		if (mdp5_kms->smp) {
			const struct mdp_format *format =
				to_mdp_format(msm_framebuffer_format(state->fb));

			blkcfg = mdp5_smp_calculate(mdp5_kms->smp, format,
					state->src_w >> 16, false);

			if (mdp5_state->hwpipe && (mdp5_state->hwpipe->blkcfg != blkcfg))
				new_hwpipe = true;
		}

		/* (re)assign hwpipe if needed, otherwise keep old one: */
		if (new_hwpipe) {
			/* TODO maybe we want to re-assign hwpipe sometimes
			 * in cases when we no-longer need some caps to make
			 * it available for other planes?
			 */
			struct mdp5_hw_pipe *old_hwpipe = mdp5_state->hwpipe;
			mdp5_state->hwpipe = mdp5_pipe_assign(state->state,
					plane, caps, blkcfg);
			if (IS_ERR(mdp5_state->hwpipe)) {
				DBG("%s: failed to assign hwpipe!", plane->name);
				return PTR_ERR(mdp5_state->hwpipe);
			}
			mdp5_pipe_release(state->state, old_hwpipe);
		}
	}

	return 0;
}

static int mdp5_plane_atomic_check(struct drm_plane *plane,
				   struct drm_plane_state *state)
{
	struct drm_crtc *crtc;
	struct drm_crtc_state *crtc_state;

	crtc = state->crtc ? state->crtc : plane->state->crtc;
	if (!crtc)
		return 0;

	crtc_state = drm_atomic_get_existing_crtc_state(state->state, crtc);
	if (WARN_ON(!crtc_state))
		return -EINVAL;

	return mdp5_plane_atomic_check_with_state(crtc_state, state);
}

static void mdp5_plane_atomic_update(struct drm_plane *plane,
				     struct drm_plane_state *old_state)
{
	struct drm_plane_state *state = plane->state;

	DBG("%s: update", plane->name);

	if (plane_enabled(state)) {
		int ret;

		ret = mdp5_plane_mode_set(plane,
				state->crtc, state->fb,
				&state->src, &state->dst);
		/* atomic_check should have ensured that this doesn't fail */
		WARN_ON(ret < 0);
	}
}

static const struct drm_plane_helper_funcs mdp5_plane_helper_funcs = {
		.prepare_fb = mdp5_plane_prepare_fb,
		.cleanup_fb = mdp5_plane_cleanup_fb,
		.atomic_check = mdp5_plane_atomic_check,
		.atomic_update = mdp5_plane_atomic_update,
};

static void set_scanout_locked(struct drm_plane *plane,
		struct drm_framebuffer *fb)
{
	struct mdp5_kms *mdp5_kms = get_kms(plane);
	struct mdp5_hw_pipe *hwpipe = to_mdp5_plane_state(plane->state)->hwpipe;
	enum mdp5_pipe pipe = hwpipe->pipe;

	mdp5_write(mdp5_kms, REG_MDP5_PIPE_SRC_STRIDE_A(pipe),
			MDP5_PIPE_SRC_STRIDE_A_P0(fb->pitches[0]) |
			MDP5_PIPE_SRC_STRIDE_A_P1(fb->pitches[1]));

	mdp5_write(mdp5_kms, REG_MDP5_PIPE_SRC_STRIDE_B(pipe),
			MDP5_PIPE_SRC_STRIDE_B_P2(fb->pitches[2]) |
			MDP5_PIPE_SRC_STRIDE_B_P3(fb->pitches[3]));

	mdp5_write(mdp5_kms, REG_MDP5_PIPE_SRC0_ADDR(pipe),
			msm_framebuffer_iova(fb, mdp5_kms->id, 0));
	mdp5_write(mdp5_kms, REG_MDP5_PIPE_SRC1_ADDR(pipe),
			msm_framebuffer_iova(fb, mdp5_kms->id, 1));
	mdp5_write(mdp5_kms, REG_MDP5_PIPE_SRC2_ADDR(pipe),
			msm_framebuffer_iova(fb, mdp5_kms->id, 2));
	mdp5_write(mdp5_kms, REG_MDP5_PIPE_SRC3_ADDR(pipe),
			msm_framebuffer_iova(fb, mdp5_kms->id, 3));

	plane->fb = fb;
}

/* Note: mdp5_plane->pipe_lock must be locked */
static void csc_disable(struct mdp5_kms *mdp5_kms, enum mdp5_pipe pipe)
{
	uint32_t value = mdp5_read(mdp5_kms, REG_MDP5_PIPE_OP_MODE(pipe)) &
			 ~MDP5_PIPE_OP_MODE_CSC_1_EN;

	mdp5_write(mdp5_kms, REG_MDP5_PIPE_OP_MODE(pipe), value);
}

/* Note: mdp5_plane->pipe_lock must be locked */
static void csc_enable(struct mdp5_kms *mdp5_kms, enum mdp5_pipe pipe,
		struct csc_cfg *csc)
{
	uint32_t  i, mode = 0; /* RGB, no CSC */
	uint32_t *matrix;

	if (unlikely(!csc))
		return;

	if ((csc->type == CSC_YUV2RGB) || (CSC_YUV2YUV == csc->type))
		mode |= MDP5_PIPE_OP_MODE_CSC_SRC_DATA_FORMAT(DATA_FORMAT_YUV);
	if ((csc->type == CSC_RGB2YUV) || (CSC_YUV2YUV == csc->type))
		mode |= MDP5_PIPE_OP_MODE_CSC_DST_DATA_FORMAT(DATA_FORMAT_YUV);
	mode |= MDP5_PIPE_OP_MODE_CSC_1_EN;
	mdp5_write(mdp5_kms, REG_MDP5_PIPE_OP_MODE(pipe), mode);

	matrix = csc->matrix;
	mdp5_write(mdp5_kms, REG_MDP5_PIPE_CSC_1_MATRIX_COEFF_0(pipe),
			MDP5_PIPE_CSC_1_MATRIX_COEFF_0_COEFF_11(matrix[0]) |
			MDP5_PIPE_CSC_1_MATRIX_COEFF_0_COEFF_12(matrix[1]));
	mdp5_write(mdp5_kms, REG_MDP5_PIPE_CSC_1_MATRIX_COEFF_1(pipe),
			MDP5_PIPE_CSC_1_MATRIX_COEFF_1_COEFF_13(matrix[2]) |
			MDP5_PIPE_CSC_1_MATRIX_COEFF_1_COEFF_21(matrix[3]));
	mdp5_write(mdp5_kms, REG_MDP5_PIPE_CSC_1_MATRIX_COEFF_2(pipe),
			MDP5_PIPE_CSC_1_MATRIX_COEFF_2_COEFF_22(matrix[4]) |
			MDP5_PIPE_CSC_1_MATRIX_COEFF_2_COEFF_23(matrix[5]));
	mdp5_write(mdp5_kms, REG_MDP5_PIPE_CSC_1_MATRIX_COEFF_3(pipe),
			MDP5_PIPE_CSC_1_MATRIX_COEFF_3_COEFF_31(matrix[6]) |
			MDP5_PIPE_CSC_1_MATRIX_COEFF_3_COEFF_32(matrix[7]));
	mdp5_write(mdp5_kms, REG_MDP5_PIPE_CSC_1_MATRIX_COEFF_4(pipe),
			MDP5_PIPE_CSC_1_MATRIX_COEFF_4_COEFF_33(matrix[8]));

	for (i = 0; i < ARRAY_SIZE(csc->pre_bias); i++) {
		uint32_t *pre_clamp = csc->pre_clamp;
		uint32_t *post_clamp = csc->post_clamp;

		mdp5_write(mdp5_kms, REG_MDP5_PIPE_CSC_1_PRE_CLAMP(pipe, i),
			MDP5_PIPE_CSC_1_PRE_CLAMP_REG_HIGH(pre_clamp[2*i+1]) |
			MDP5_PIPE_CSC_1_PRE_CLAMP_REG_LOW(pre_clamp[2*i]));

		mdp5_write(mdp5_kms, REG_MDP5_PIPE_CSC_1_POST_CLAMP(pipe, i),
			MDP5_PIPE_CSC_1_POST_CLAMP_REG_HIGH(post_clamp[2*i+1]) |
			MDP5_PIPE_CSC_1_POST_CLAMP_REG_LOW(post_clamp[2*i]));

		mdp5_write(mdp5_kms, REG_MDP5_PIPE_CSC_1_PRE_BIAS(pipe, i),
			MDP5_PIPE_CSC_1_PRE_BIAS_REG_VALUE(csc->pre_bias[i]));

		mdp5_write(mdp5_kms, REG_MDP5_PIPE_CSC_1_POST_BIAS(pipe, i),
			MDP5_PIPE_CSC_1_POST_BIAS_REG_VALUE(csc->post_bias[i]));
	}
}

#define PHASE_STEP_SHIFT	21
#define DOWN_SCALE_RATIO_MAX	32	/* 2^(26-21) */

static int calc_phase_step(uint32_t src, uint32_t dst, uint32_t *out_phase)
{
	uint32_t unit;

	if (src == 0 || dst == 0)
		return -EINVAL;

	/*
	 * PHASE_STEP_X/Y is coded on 26 bits (25:0),
	 * where 2^21 represents the unity "1" in fixed-point hardware design.
	 * This leaves 5 bits for the integer part (downscale case):
	 *	-> maximum downscale ratio = 0b1_1111 = 31
	 */
	if (src > (dst * DOWN_SCALE_RATIO_MAX))
		return -EOVERFLOW;

	unit = 1 << PHASE_STEP_SHIFT;
	*out_phase = mult_frac(unit, src, dst);

	return 0;
}

static int calc_scalex_steps(struct drm_plane *plane,
		uint32_t pixel_format, uint32_t src, uint32_t dest,
		uint32_t phasex_steps[COMP_MAX])
{
	struct mdp5_kms *mdp5_kms = get_kms(plane);
	struct device *dev = mdp5_kms->dev->dev;
	uint32_t phasex_step;
	unsigned int hsub;
	int ret;

	ret = calc_phase_step(src, dest, &phasex_step);
	if (ret) {
		dev_err(dev, "X scaling (%d->%d) failed: %d\n", src, dest, ret);
		return ret;
	}

	hsub = drm_format_horz_chroma_subsampling(pixel_format);

	phasex_steps[COMP_0]   = phasex_step;
	phasex_steps[COMP_3]   = phasex_step;
	phasex_steps[COMP_1_2] = phasex_step / hsub;

	return 0;
}

static int calc_scaley_steps(struct drm_plane *plane,
		uint32_t pixel_format, uint32_t src, uint32_t dest,
		uint32_t phasey_steps[COMP_MAX])
{
	struct mdp5_kms *mdp5_kms = get_kms(plane);
	struct device *dev = mdp5_kms->dev->dev;
	uint32_t phasey_step;
	unsigned int vsub;
	int ret;

	ret = calc_phase_step(src, dest, &phasey_step);
	if (ret) {
		dev_err(dev, "Y scaling (%d->%d) failed: %d\n", src, dest, ret);
		return ret;
	}

	vsub = drm_format_vert_chroma_subsampling(pixel_format);

	phasey_steps[COMP_0]   = phasey_step;
	phasey_steps[COMP_3]   = phasey_step;
	phasey_steps[COMP_1_2] = phasey_step / vsub;

	return 0;
}

static uint32_t get_scale_config(const struct mdp_format *format,
		uint32_t src, uint32_t dst, bool horz)
{
	bool scaling = format->is_yuv ? true : (src != dst);
	uint32_t sub, pix_fmt = format->base.pixel_format;
	uint32_t ya_filter, uv_filter;
	bool yuv = format->is_yuv;

	if (!scaling)
		return 0;

	if (yuv) {
		sub = horz ? drm_format_horz_chroma_subsampling(pix_fmt) :
			     drm_format_vert_chroma_subsampling(pix_fmt);
		uv_filter = ((src / sub) <= dst) ?
				   SCALE_FILTER_BIL : SCALE_FILTER_PCMN;
	}
	ya_filter = (src <= dst) ? SCALE_FILTER_BIL : SCALE_FILTER_PCMN;

	if (horz)
		return  MDP5_PIPE_SCALE_CONFIG_SCALEX_EN |
			MDP5_PIPE_SCALE_CONFIG_SCALEX_FILTER_COMP_0(ya_filter) |
			MDP5_PIPE_SCALE_CONFIG_SCALEX_FILTER_COMP_3(ya_filter) |
			COND(yuv, MDP5_PIPE_SCALE_CONFIG_SCALEX_FILTER_COMP_1_2(uv_filter));
	else
		return  MDP5_PIPE_SCALE_CONFIG_SCALEY_EN |
			MDP5_PIPE_SCALE_CONFIG_SCALEY_FILTER_COMP_0(ya_filter) |
			MDP5_PIPE_SCALE_CONFIG_SCALEY_FILTER_COMP_3(ya_filter) |
			COND(yuv, MDP5_PIPE_SCALE_CONFIG_SCALEY_FILTER_COMP_1_2(uv_filter));
}

static void calc_pixel_ext(const struct mdp_format *format,
		uint32_t src, uint32_t dst, uint32_t phase_step[2],
		int pix_ext_edge1[COMP_MAX], int pix_ext_edge2[COMP_MAX],
		bool horz)
{
	bool scaling = format->is_yuv ? true : (src != dst);
	int i;

	/*
	 * Note:
	 * We assume here that:
	 *     1. PCMN filter is used for downscale
	 *     2. bilinear filter is used for upscale
	 *     3. we are in a single pipe configuration
	 */

	for (i = 0; i < COMP_MAX; i++) {
		pix_ext_edge1[i] = 0;
		pix_ext_edge2[i] = scaling ? 1 : 0;
	}
}

static void mdp5_write_pixel_ext(struct mdp5_kms *mdp5_kms, enum mdp5_pipe pipe,
	const struct mdp_format *format,
	uint32_t src_w, int pe_left[COMP_MAX], int pe_right[COMP_MAX],
	uint32_t src_h, int pe_top[COMP_MAX], int pe_bottom[COMP_MAX])
{
	uint32_t pix_fmt = format->base.pixel_format;
	uint32_t lr, tb, req;
	int i;

	for (i = 0; i < COMP_MAX; i++) {
		uint32_t roi_w = src_w;
		uint32_t roi_h = src_h;

		if (format->is_yuv && i == COMP_1_2) {
			roi_w /= drm_format_horz_chroma_subsampling(pix_fmt);
			roi_h /= drm_format_vert_chroma_subsampling(pix_fmt);
		}

		lr  = (pe_left[i] >= 0) ?
			MDP5_PIPE_SW_PIX_EXT_LR_LEFT_RPT(pe_left[i]) :
			MDP5_PIPE_SW_PIX_EXT_LR_LEFT_OVF(pe_left[i]);

		lr |= (pe_right[i] >= 0) ?
			MDP5_PIPE_SW_PIX_EXT_LR_RIGHT_RPT(pe_right[i]) :
			MDP5_PIPE_SW_PIX_EXT_LR_RIGHT_OVF(pe_right[i]);

		tb  = (pe_top[i] >= 0) ?
			MDP5_PIPE_SW_PIX_EXT_TB_TOP_RPT(pe_top[i]) :
			MDP5_PIPE_SW_PIX_EXT_TB_TOP_OVF(pe_top[i]);

		tb |= (pe_bottom[i] >= 0) ?
			MDP5_PIPE_SW_PIX_EXT_TB_BOTTOM_RPT(pe_bottom[i]) :
			MDP5_PIPE_SW_PIX_EXT_TB_BOTTOM_OVF(pe_bottom[i]);

		req  = MDP5_PIPE_SW_PIX_EXT_REQ_PIXELS_LEFT_RIGHT(roi_w +
				pe_left[i] + pe_right[i]);

		req |= MDP5_PIPE_SW_PIX_EXT_REQ_PIXELS_TOP_BOTTOM(roi_h +
				pe_top[i] + pe_bottom[i]);

		mdp5_write(mdp5_kms, REG_MDP5_PIPE_SW_PIX_EXT_LR(pipe, i), lr);
		mdp5_write(mdp5_kms, REG_MDP5_PIPE_SW_PIX_EXT_TB(pipe, i), tb);
		mdp5_write(mdp5_kms, REG_MDP5_PIPE_SW_PIX_EXT_REQ_PIXELS(pipe, i), req);

		DBG("comp-%d (L/R): rpt=%d/%d, ovf=%d/%d, req=%d", i,
			FIELD(lr,  MDP5_PIPE_SW_PIX_EXT_LR_LEFT_RPT),
			FIELD(lr,  MDP5_PIPE_SW_PIX_EXT_LR_RIGHT_RPT),
			FIELD(lr,  MDP5_PIPE_SW_PIX_EXT_LR_LEFT_OVF),
			FIELD(lr,  MDP5_PIPE_SW_PIX_EXT_LR_RIGHT_OVF),
			FIELD(req, MDP5_PIPE_SW_PIX_EXT_REQ_PIXELS_LEFT_RIGHT));

		DBG("comp-%d (T/B): rpt=%d/%d, ovf=%d/%d, req=%d", i,
			FIELD(tb,  MDP5_PIPE_SW_PIX_EXT_TB_TOP_RPT),
			FIELD(tb,  MDP5_PIPE_SW_PIX_EXT_TB_BOTTOM_RPT),
			FIELD(tb,  MDP5_PIPE_SW_PIX_EXT_TB_TOP_OVF),
			FIELD(tb,  MDP5_PIPE_SW_PIX_EXT_TB_BOTTOM_OVF),
			FIELD(req, MDP5_PIPE_SW_PIX_EXT_REQ_PIXELS_TOP_BOTTOM));
	}
}


static int mdp5_plane_mode_set(struct drm_plane *plane,
		struct drm_crtc *crtc, struct drm_framebuffer *fb,
		struct drm_rect *src, struct drm_rect *dest)
{
	struct drm_plane_state *pstate = plane->state;
	struct mdp5_hw_pipe *hwpipe = to_mdp5_plane_state(pstate)->hwpipe;
	struct mdp5_kms *mdp5_kms = get_kms(plane);
	enum mdp5_pipe pipe = hwpipe->pipe;
	const struct mdp_format *format;
	uint32_t nplanes, config = 0;
	uint32_t phasex_step[COMP_MAX] = {0,}, phasey_step[COMP_MAX] = {0,};
	bool pe = hwpipe->caps & MDP_PIPE_CAP_SW_PIX_EXT;
	int pe_left[COMP_MAX], pe_right[COMP_MAX];
	int pe_top[COMP_MAX], pe_bottom[COMP_MAX];
	uint32_t hdecm = 0, vdecm = 0;
	uint32_t pix_format;
	unsigned int rotation;
	bool vflip, hflip;
	int crtc_x, crtc_y;
	unsigned int crtc_w, crtc_h;
	uint32_t src_x, src_y;
	uint32_t src_w, src_h;
	unsigned long flags;
	int ret;

	nplanes = fb->format->num_planes;

	/* bad formats should already be rejected: */
	if (WARN_ON(nplanes > pipe2nclients(pipe)))
		return -EINVAL;

	format = to_mdp_format(msm_framebuffer_format(fb));
	pix_format = format->base.pixel_format;

	src_x = src->x1;
	src_y = src->y1;
	src_w = drm_rect_width(src);
	src_h = drm_rect_height(src);

	crtc_x = dest->x1;
	crtc_y = dest->y1;
	crtc_w = drm_rect_width(dest);
	crtc_h = drm_rect_height(dest);

	/* src values are in Q16 fixed point, convert to integer: */
	src_x = src_x >> 16;
	src_y = src_y >> 16;
	src_w = src_w >> 16;
	src_h = src_h >> 16;

	DBG("%s: FB[%u] %u,%u,%u,%u -> CRTC[%u] %d,%d,%u,%u", plane->name,
			fb->base.id, src_x, src_y, src_w, src_h,
			crtc->base.id, crtc_x, crtc_y, crtc_w, crtc_h);

	ret = calc_scalex_steps(plane, pix_format, src_w, crtc_w, phasex_step);
	if (ret)
		return ret;

	ret = calc_scaley_steps(plane, pix_format, src_h, crtc_h, phasey_step);
	if (ret)
		return ret;

	if (hwpipe->caps & MDP_PIPE_CAP_SW_PIX_EXT) {
		calc_pixel_ext(format, src_w, crtc_w, phasex_step,
					 pe_left, pe_right, true);
		calc_pixel_ext(format, src_h, crtc_h, phasey_step,
					pe_top, pe_bottom, false);
	}

	/* TODO calc hdecm, vdecm */

	/* SCALE is used to both scale and up-sample chroma components */
	config |= get_scale_config(format, src_w, crtc_w, true);
	config |= get_scale_config(format, src_h, crtc_h, false);
	DBG("scale config = %x", config);

	rotation = drm_rotation_simplify(pstate->rotation,
					 DRM_ROTATE_0 |
					 DRM_REFLECT_X |
					 DRM_REFLECT_Y);
	hflip = !!(rotation & DRM_REFLECT_X);
	vflip = !!(rotation & DRM_REFLECT_Y);

	spin_lock_irqsave(&hwpipe->pipe_lock, flags);

	mdp5_write(mdp5_kms, REG_MDP5_PIPE_SRC_IMG_SIZE(pipe),
			MDP5_PIPE_SRC_IMG_SIZE_WIDTH(min(fb->width, src_w)) |
			MDP5_PIPE_SRC_IMG_SIZE_HEIGHT(min(fb->height, src_h)));

	mdp5_write(mdp5_kms, REG_MDP5_PIPE_SRC_SIZE(pipe),
			MDP5_PIPE_SRC_SIZE_WIDTH(src_w) |
			MDP5_PIPE_SRC_SIZE_HEIGHT(src_h));

	mdp5_write(mdp5_kms, REG_MDP5_PIPE_SRC_XY(pipe),
			MDP5_PIPE_SRC_XY_X(src_x) |
			MDP5_PIPE_SRC_XY_Y(src_y));

	mdp5_write(mdp5_kms, REG_MDP5_PIPE_OUT_SIZE(pipe),
			MDP5_PIPE_OUT_SIZE_WIDTH(crtc_w) |
			MDP5_PIPE_OUT_SIZE_HEIGHT(crtc_h));

	mdp5_write(mdp5_kms, REG_MDP5_PIPE_OUT_XY(pipe),
			MDP5_PIPE_OUT_XY_X(crtc_x) |
			MDP5_PIPE_OUT_XY_Y(crtc_y));

	mdp5_write(mdp5_kms, REG_MDP5_PIPE_SRC_FORMAT(pipe),
			MDP5_PIPE_SRC_FORMAT_A_BPC(format->bpc_a) |
			MDP5_PIPE_SRC_FORMAT_R_BPC(format->bpc_r) |
			MDP5_PIPE_SRC_FORMAT_G_BPC(format->bpc_g) |
			MDP5_PIPE_SRC_FORMAT_B_BPC(format->bpc_b) |
			COND(format->alpha_enable, MDP5_PIPE_SRC_FORMAT_ALPHA_ENABLE) |
			MDP5_PIPE_SRC_FORMAT_CPP(format->cpp - 1) |
			MDP5_PIPE_SRC_FORMAT_UNPACK_COUNT(format->unpack_count - 1) |
			COND(format->unpack_tight, MDP5_PIPE_SRC_FORMAT_UNPACK_TIGHT) |
			MDP5_PIPE_SRC_FORMAT_FETCH_TYPE(format->fetch_type) |
			MDP5_PIPE_SRC_FORMAT_CHROMA_SAMP(format->chroma_sample));

	mdp5_write(mdp5_kms, REG_MDP5_PIPE_SRC_UNPACK(pipe),
			MDP5_PIPE_SRC_UNPACK_ELEM0(format->unpack[0]) |
			MDP5_PIPE_SRC_UNPACK_ELEM1(format->unpack[1]) |
			MDP5_PIPE_SRC_UNPACK_ELEM2(format->unpack[2]) |
			MDP5_PIPE_SRC_UNPACK_ELEM3(format->unpack[3]));

	mdp5_write(mdp5_kms, REG_MDP5_PIPE_SRC_OP_MODE(pipe),
			(hflip ? MDP5_PIPE_SRC_OP_MODE_FLIP_LR : 0) |
			(vflip ? MDP5_PIPE_SRC_OP_MODE_FLIP_UD : 0) |
			COND(pe, MDP5_PIPE_SRC_OP_MODE_SW_PIX_EXT_OVERRIDE) |
			MDP5_PIPE_SRC_OP_MODE_BWC(BWC_LOSSLESS));

	/* not using secure mode: */
	mdp5_write(mdp5_kms, REG_MDP5_PIPE_SRC_ADDR_SW_STATUS(pipe), 0);

	if (hwpipe->caps & MDP_PIPE_CAP_SW_PIX_EXT)
		mdp5_write_pixel_ext(mdp5_kms, pipe, format,
				src_w, pe_left, pe_right,
				src_h, pe_top, pe_bottom);

	if (hwpipe->caps & MDP_PIPE_CAP_SCALE) {
		mdp5_write(mdp5_kms, REG_MDP5_PIPE_SCALE_PHASE_STEP_X(pipe),
				phasex_step[COMP_0]);
		mdp5_write(mdp5_kms, REG_MDP5_PIPE_SCALE_PHASE_STEP_Y(pipe),
				phasey_step[COMP_0]);
		mdp5_write(mdp5_kms, REG_MDP5_PIPE_SCALE_CR_PHASE_STEP_X(pipe),
				phasex_step[COMP_1_2]);
		mdp5_write(mdp5_kms, REG_MDP5_PIPE_SCALE_CR_PHASE_STEP_Y(pipe),
				phasey_step[COMP_1_2]);
		mdp5_write(mdp5_kms, REG_MDP5_PIPE_DECIMATION(pipe),
				MDP5_PIPE_DECIMATION_VERT(vdecm) |
				MDP5_PIPE_DECIMATION_HORZ(hdecm));
		mdp5_write(mdp5_kms, REG_MDP5_PIPE_SCALE_CONFIG(pipe), config);
	}

	if (hwpipe->caps & MDP_PIPE_CAP_CSC) {
		if (MDP_FORMAT_IS_YUV(format))
			csc_enable(mdp5_kms, pipe,
					mdp_get_default_csc_cfg(CSC_YUV2RGB));
		else
			csc_disable(mdp5_kms, pipe);
	}

	set_scanout_locked(plane, fb);

	spin_unlock_irqrestore(&hwpipe->pipe_lock, flags);

	return ret;
}

static int mdp5_update_cursor_plane_legacy(struct drm_plane *plane,
			struct drm_crtc *crtc, struct drm_framebuffer *fb,
			int crtc_x, int crtc_y,
			unsigned int crtc_w, unsigned int crtc_h,
			uint32_t src_x, uint32_t src_y,
			uint32_t src_w, uint32_t src_h,
			struct drm_modeset_acquire_ctx *ctx)
{
	struct drm_plane_state *plane_state, *new_plane_state;
	struct mdp5_plane_state *mdp5_pstate;
	struct drm_crtc_state *crtc_state = crtc->state;
	int ret;

	if (!crtc_state->active || drm_atomic_crtc_needs_modeset(crtc_state))
		goto slow;

	plane_state = plane->state;
	mdp5_pstate = to_mdp5_plane_state(plane_state);

	/* don't use fast path if we don't have a hwpipe allocated yet */
	if (!mdp5_pstate->hwpipe)
		goto slow;

	/* only allow changing of position(crtc x/y or src x/y) in fast path */
	if (plane_state->crtc != crtc ||
	    plane_state->src_w != src_w ||
	    plane_state->src_h != src_h ||
	    plane_state->crtc_w != crtc_w ||
	    plane_state->crtc_h != crtc_h ||
	    !plane_state->fb ||
	    plane_state->fb != fb)
		goto slow;

	new_plane_state = mdp5_plane_duplicate_state(plane);
	if (!new_plane_state)
		return -ENOMEM;

	new_plane_state->src_x = src_x;
	new_plane_state->src_y = src_y;
	new_plane_state->src_w = src_w;
	new_plane_state->src_h = src_h;
	new_plane_state->crtc_x = crtc_x;
	new_plane_state->crtc_y = crtc_y;
	new_plane_state->crtc_w = crtc_w;
	new_plane_state->crtc_h = crtc_h;

	ret = mdp5_plane_atomic_check_with_state(crtc_state, new_plane_state);
	if (ret)
		goto slow_free;

	if (new_plane_state->visible) {
		struct mdp5_ctl *ctl;

		ret = mdp5_plane_mode_set(plane, crtc, fb,
					  &new_plane_state->src,
					  &new_plane_state->dst);
		WARN_ON(ret < 0);

		ctl = mdp5_crtc_get_ctl(crtc);

		mdp5_ctl_commit(ctl, mdp5_plane_get_flush(plane));
	}

	*to_mdp5_plane_state(plane_state) =
		*to_mdp5_plane_state(new_plane_state);

	mdp5_plane_destroy_state(plane, new_plane_state);

	return 0;
slow_free:
	mdp5_plane_destroy_state(plane, new_plane_state);
slow:
	return drm_atomic_helper_update_plane(plane, crtc, fb,
					      crtc_x, crtc_y, crtc_w, crtc_h,
					      src_x, src_y, src_w, src_h, ctx);
}

enum mdp5_pipe mdp5_plane_pipe(struct drm_plane *plane)
{
	struct mdp5_plane_state *pstate = to_mdp5_plane_state(plane->state);

	if (WARN_ON(!pstate->hwpipe))
		return SSPP_NONE;

	return pstate->hwpipe->pipe;
}

uint32_t mdp5_plane_get_flush(struct drm_plane *plane)
{
	struct mdp5_plane_state *pstate = to_mdp5_plane_state(plane->state);

	if (WARN_ON(!pstate->hwpipe))
		return 0;

	return pstate->hwpipe->flush_mask;
}

/* initialize plane */
struct drm_plane *mdp5_plane_init(struct drm_device *dev,
				  enum drm_plane_type type)
{
	struct drm_plane *plane = NULL;
	struct mdp5_plane *mdp5_plane;
	int ret;

	mdp5_plane = kzalloc(sizeof(*mdp5_plane), GFP_KERNEL);
	if (!mdp5_plane) {
		ret = -ENOMEM;
		goto fail;
	}

	plane = &mdp5_plane->base;

	mdp5_plane->nformats = mdp_get_formats(mdp5_plane->formats,
		ARRAY_SIZE(mdp5_plane->formats), false);

	if (type == DRM_PLANE_TYPE_CURSOR)
		ret = drm_universal_plane_init(dev, plane, 0xff,
				&mdp5_cursor_plane_funcs,
				mdp5_plane->formats, mdp5_plane->nformats,
				type, NULL);
	else
		ret = drm_universal_plane_init(dev, plane, 0xff,
				&mdp5_plane_funcs,
				mdp5_plane->formats, mdp5_plane->nformats,
				type, NULL);
	if (ret)
		goto fail;

	drm_plane_helper_add(plane, &mdp5_plane_helper_funcs);

	mdp5_plane_install_properties(plane, &plane->base);

	return plane;

fail:
	if (plane)
		mdp5_plane_destroy(plane);

	return ERR_PTR(ret);
}
