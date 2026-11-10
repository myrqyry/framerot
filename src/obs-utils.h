/*
 * Derived in part from OBS Retro Effects (FiniteSingularity), through the
 * myrqyry/obs-retro-effects fork. Substantially modified for gop-rot in 2026.
 * GNU GPL version 2. See COPYING and README.md for attribution.
 */
#pragma once
#include <obs-module.h>
/* Adapted from GPL-2.0 obs-retro-effects rendering helpers. */
static inline void set_blending_parameters(void)
{
    gs_blend_state_push();
    gs_reset_blend_state();
    gs_enable_blending(false);
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
}
static inline void set_render_parameters(void)
{
    gs_set_cull_mode(GS_NEITHER);
    gs_enable_color(true, true, true, true);
    gs_enable_depth_test(false);
    gs_depth_function(GS_ALWAYS);
    gs_enable_stencil_test(false);
    gs_enable_stencil_write(false);
    gs_stencil_function(GS_STENCIL_BOTH, GS_ALWAYS);
    gs_stencil_op(GS_STENCIL_BOTH, GS_ZERO, GS_ZERO, GS_ZERO);
}
