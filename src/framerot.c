/*
 * framerot: experimental OBS MPEG-4 datamosh filter, developed in 2026.
 * GNU GPL version 2. See COPYING and README.md for upstream attribution.
 */
#include <obs-module.h>
#include <graphics/graphics.h>
#include <stdint.h>
#include <stdbool.h>
#include "video-obs.h"
#include "video-roundtrip.h"

OBS_DECLARE_MODULE()

struct mosh_filter {
    obs_source_t *source; /* OBS owns this borrowed handle */
    gs_texrender_t *capture;
    gs_texrender_t *output;
    struct retro_video_obs *codec;
    struct retro_video_options options;
    float elapsed;
    bool due;
};

static const char *mosh_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "framerot";
}

/* Defaults are per effect, not an arbitrary compromise across seven modes.
 * Set DEFAULT values only: OBS explicitly saved/user-entered values retain
 * priority even when a mode is changed or a scene is reloaded. */
struct mosh_showcase {
    int fps, kbps, gop, skip_keyframes, p_interval, p_burst;
    int vector_strength, vector_persistence, rounding_interval, anchor_span;
    int rewind_depth, slice_history, slice_height;
};

static const struct mosh_showcase showcase[] = {
    /* Classic: original crunchy, rapid I-frame recovery. */
    {12, 96, 6, 1, 5, 3, 250, 65, 1, 4, 4, 4, 12},
    /* Tear: regular P packet loss. */
    {15, 160, 12, 1, 5, 3, 250, 65, 1, 4, 4, 4, 12},
    /* Burst: longer predictive-frame interruptions. */
    {15, 160, 18, 1, 12, 5, 250, 65, 1, 4, 4, 4, 12},
    /* Echo: earlier compressed motion repeats frequently. */
    {15, 160, 18, 1, 3, 3, 250, 65, 1, 4, 4, 4, 12},
    /* Flow Drift: exported motion vectors and feedback. */
    {15, 230, 24, 1, 5, 3, 250, 65, 1, 4, 4, 4, 12},
    /* Motion Shimmer: modify every P-VOP interpolation rounding bit. */
    {15, 200, 24, 1, 5, 3, 250, 65, 1, 4, 4, 4, 12},
    /* Keyframe Anchor: held I-frame plus live P-frame continuation. */
    {15, 160, 12, 1, 5, 3, 250, 65, 1, 4, 4, 4, 12},
    /* Rewind: real P packets from four frames earlier. */
    {15, 220, 24, 1, 5, 3, 250, 65, 1, 4, 4, 4, 12},
    /* Peel: bands from four different decoded moments. */
    {15, 200, 18, 1, 5, 3, 250, 65, 1, 4, 4, 4, 12},
};

static void mosh_showcase_defaults(obs_data_t *settings, int mode)
{
    if (mode < RETRO_MOSH_KEYFRAME_SKIP || mode > RETRO_MOSH_FRAME_PEEL)
        mode = RETRO_MOSH_KEYFRAME_SKIP;
    const struct mosh_showcase *p = &showcase[mode];
    obs_data_set_default_int(settings, "fps", p->fps);
    obs_data_set_default_int(settings, "kbps", p->kbps);
    obs_data_set_default_int(settings, "gop", p->gop);
    obs_data_set_default_int(settings, "skip_keyframes", p->skip_keyframes);
    obs_data_set_default_int(settings, "p_interval", p->p_interval);
    obs_data_set_default_int(settings, "p_burst", p->p_burst);
    obs_data_set_default_int(settings, "vector_strength", p->vector_strength);
    obs_data_set_default_int(settings, "vector_persistence", p->vector_persistence);
    obs_data_set_default_int(settings, "rounding_interval", p->rounding_interval);
    obs_data_set_default_int(settings, "anchor_span", p->anchor_span);
    obs_data_set_default_int(settings, "rewind_depth", p->rewind_depth);
    obs_data_set_default_int(settings, "slice_history", p->slice_history);
    obs_data_set_default_int(settings, "slice_height", p->slice_height);
}

static void mosh_defaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings, "mode", RETRO_MOSH_KEYFRAME_SKIP);
    mosh_showcase_defaults(settings, RETRO_MOSH_KEYFRAME_SKIP);
}

static void mosh_update(void *data, obs_data_t *settings)
{
    struct mosh_filter *s = data;
    /* Handles saved modes when OBS restores an existing scene. */
    mosh_showcase_defaults(settings, (int)obs_data_get_int(settings, "mode"));
    struct retro_video_options next = retro_video_default_options(RETRO_VIDEO_MPEG4_PART2);
    next.width = 176;
    next.height = 144;
    next.fps = (int)obs_data_get_int(settings, "fps");
    next.bitrate = (int)obs_data_get_int(settings, "kbps") * 1000;
    next.gop_size = (int)obs_data_get_int(settings, "gop");
    next.mosh_skip_keyframes = (int)obs_data_get_int(settings, "skip_keyframes");
    next.mosh_mode = (enum retro_mosh_mode)obs_data_get_int(settings, "mode");
    next.mosh_p_interval = (int)obs_data_get_int(settings, "p_interval");
    next.mosh_p_burst = (int)obs_data_get_int(settings, "p_burst");
    next.mosh_vector_strength = (int)obs_data_get_int(settings, "vector_strength");
    next.mosh_vector_persistence = (int)obs_data_get_int(settings, "vector_persistence");
    next.mosh_rounding_interval = (int)obs_data_get_int(settings, "rounding_interval");
    next.mosh_anchor_span = (int)obs_data_get_int(settings, "anchor_span");
    next.mosh_rewind_depth = (int)obs_data_get_int(settings, "rewind_depth");
    next.mosh_slice_history = (int)obs_data_get_int(settings, "slice_history");
    next.mosh_slice_height = (int)obs_data_get_int(settings, "slice_height");
    if (next.fps < 1) next.fps = 1;
    if (next.fps > 30) next.fps = 30;
    if (next.bitrate < 8000) next.bitrate = 8000;
    if (next.bitrate > 2000000) next.bitrate = 2000000;
    if (next.gop_size < 2) next.gop_size = 2;
    if (next.gop_size > 120) next.gop_size = 120;
    if (next.mosh_skip_keyframes < 0) next.mosh_skip_keyframes = 0;
    if (next.mosh_skip_keyframes > 8) next.mosh_skip_keyframes = 8;
    if (next.mosh_mode < RETRO_MOSH_KEYFRAME_SKIP || next.mosh_mode > RETRO_MOSH_FRAME_PEEL)
        next.mosh_mode = RETRO_MOSH_KEYFRAME_SKIP;
    if (next.mosh_p_interval < 2) next.mosh_p_interval = 2;
    if (next.mosh_p_interval > 60) next.mosh_p_interval = 60;
    if (next.mosh_p_burst < 1) next.mosh_p_burst = 1;
    if (next.mosh_p_burst >= next.mosh_p_interval)
        next.mosh_p_burst = next.mosh_p_interval - 1;
    if (next.mosh_vector_strength < 0) next.mosh_vector_strength = 0;
    if (next.mosh_vector_strength > 500) next.mosh_vector_strength = 500;
    if (next.mosh_vector_persistence < 0) next.mosh_vector_persistence = 0;
    if (next.mosh_vector_persistence > 100) next.mosh_vector_persistence = 100;
    if (next.mosh_rounding_interval < 1) next.mosh_rounding_interval = 1;
    if (next.mosh_rounding_interval > 60) next.mosh_rounding_interval = 60;
    if (next.mosh_anchor_span < 2) next.mosh_anchor_span = 2;
    if (next.mosh_anchor_span > 12) next.mosh_anchor_span = 12;
    if (next.mosh_rewind_depth < 2) next.mosh_rewind_depth = 2;
    if (next.mosh_rewind_depth > 12) next.mosh_rewind_depth = 12;
    if (next.mosh_slice_history < 2) next.mosh_slice_history = 2;
    if (next.mosh_slice_history > 8) next.mosh_slice_history = 8;
    if (next.mosh_slice_height < 1) next.mosh_slice_height = 1;
    if (next.mosh_slice_height > 64) next.mosh_slice_height = 64;
    if (!retro_video_obs_matches(s->codec, &next))
        s->due = true;
    s->options = next;
}

static void mosh_property_visibility(obs_properties_t *props, int mode)
{
    obs_property_set_visible(obs_properties_get(props, "skip_keyframes"),
                             mode == RETRO_MOSH_KEYFRAME_SKIP);
    obs_property_set_visible(obs_properties_get(props, "p_interval"),
                             mode >= RETRO_MOSH_PFRAME_DROP && mode <= RETRO_MOSH_PFRAME_ECHO);
    obs_property_set_visible(obs_properties_get(props, "p_burst"),
                             mode == RETRO_MOSH_PFRAME_BURST);
    obs_property_set_visible(obs_properties_get(props, "vector_strength"),
                             mode == RETRO_MOSH_VECTOR_DRIFT);
    obs_property_set_visible(obs_properties_get(props, "vector_persistence"),
                             mode == RETRO_MOSH_VECTOR_DRIFT);
    obs_property_set_visible(obs_properties_get(props, "rounding_interval"),
                             mode == RETRO_MOSH_MOTION_SHIMMER);
    obs_property_set_visible(obs_properties_get(props, "anchor_span"),
                             mode == RETRO_MOSH_KEYFRAME_ANCHOR);
    obs_property_set_visible(obs_properties_get(props, "rewind_depth"),
                             mode == RETRO_MOSH_PFRAME_REWIND);
    obs_property_set_visible(obs_properties_get(props, "slice_history"),
                             mode == RETRO_MOSH_FRAME_PEEL);
    obs_property_set_visible(obs_properties_get(props, "slice_height"),
                             mode == RETRO_MOSH_FRAME_PEEL);
}

static bool mosh_mode_changed(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
    UNUSED_PARAMETER(property);
    const int mode = (int)obs_data_get_int(settings, "mode");
    mosh_showcase_defaults(settings, mode);
    mosh_property_visibility(props, mode);
    return true;
}

static obs_properties_t *mosh_properties(void *data)
{
    struct mosh_filter *s = data;
    obs_properties_t *props = obs_properties_create();
    obs_property_t *mode = obs_properties_add_list(props, "mode", "Effect",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(mode, "Classic — keyframe loss", RETRO_MOSH_KEYFRAME_SKIP);
    obs_property_list_add_int(mode, "Tear — periodic P-frame loss", RETRO_MOSH_PFRAME_DROP);
    obs_property_list_add_int(mode, "Burst — packets lost in runs", RETRO_MOSH_PFRAME_BURST);
    obs_property_list_add_int(mode, "Echo — replay compressed motion", RETRO_MOSH_PFRAME_ECHO);
    obs_property_list_add_int(mode, "Flow Drift — MPEG-4 motion vectors", RETRO_MOSH_VECTOR_DRIFT);
    obs_property_list_add_int(mode, "Motion Shimmer — P-frame rounding bit", RETRO_MOSH_MOTION_SHIMMER);
    obs_property_list_add_int(mode, "Keyframe Anchor — reuse earlier I-frame", RETRO_MOSH_KEYFRAME_ANCHOR);
    obs_property_list_add_int(mode, "P-Frame Rewind — older compressed motion", RETRO_MOSH_PFRAME_REWIND);
    obs_property_list_add_int(mode, "Frame Peel — temporal bands", RETRO_MOSH_FRAME_PEEL);
    obs_property_set_modified_callback(mode, mosh_mode_changed);
    obs_properties_add_int_slider(props, "skip_keyframes", "Classic: skipped keyframes", 0, 8, 1);
    obs_properties_add_int_slider(props, "p_interval", "P-frame interval (Tear/Burst/Echo)", 2, 60, 1);
    obs_properties_add_int_slider(props, "p_burst", "Burst: frames lost per interval", 1, 30, 1);
    obs_properties_add_int_slider(props, "vector_strength", "Flow: motion exaggeration (%)", 0, 500, 10);
    obs_properties_add_int_slider(props, "vector_persistence", "Flow: previous-frame persistence (%)", 0, 100, 5);
    obs_properties_add_int_slider(props, "rounding_interval", "Shimmer: edit every N P-frames", 1, 60, 1);
    obs_properties_add_int_slider(props, "anchor_span", "Anchor: keyframes per held image", 2, 12, 1);
    obs_properties_add_int_slider(props, "rewind_depth", "Rewind: P-frames back", 2, 12, 1);
    obs_properties_add_int_slider(props, "slice_history", "Peel: frames in history", 2, 8, 1);
    obs_properties_add_int_slider(props, "slice_height", "Peel: band height (codec pixels)", 1, 64, 1);
    obs_properties_add_int_slider(props, "gop", "Keyframe interval (frames)", 2, 120, 1);
    obs_properties_add_int_slider(props, "kbps", "Bitrate (kb/s)", 8, 2000, 1);
    obs_properties_add_int_slider(props, "fps", "Codec frame rate", 1, 30, 1);
    mosh_property_visibility(props, s ? (int)s->options.mosh_mode : RETRO_MOSH_KEYFRAME_SKIP);
    return props;
}

static void *mosh_create(obs_data_t *settings, obs_source_t *source)
{
    struct mosh_filter *s = bzalloc(sizeof(*s));
    if (!s) return NULL;
    s->source = source;
    s->due = true;
    mosh_update(s, settings);
    return s;
}

static void mosh_destroy(void *data)
{
    struct mosh_filter *s = data;
    if (!s) return;
    retro_video_obs_destroy(s->codec);
    obs_enter_graphics();
    if (s->capture) gs_texrender_destroy(s->capture);
    if (s->output) gs_texrender_destroy(s->output);
    obs_leave_graphics();
    bfree(s);
}

static void mosh_tick(void *data, float seconds)
{
    struct mosh_filter *s = data;
    if (s->options.fps <= 0) return;
    s->elapsed += seconds;
    const float interval = 1.0f / (float)s->options.fps;
    if (s->elapsed >= interval) {
        s->due = true;
        s->elapsed -= interval * (int)(s->elapsed / interval);
    }
}

static void draw_texture(gs_texture_t *texture, uint32_t width, uint32_t height)
{
    if (!texture) return;
    gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
    if (!image) return;
    gs_effect_set_texture(image, texture);
    gs_blend_state_push();
    gs_reset_blend_state();
    gs_enable_blending(false);
    /* Draw in OBS's existing source projection. Overwriting it with a
     * source-sized ortho corrupts scene item placement on larger canvases. */
    while (gs_effect_loop(effect, "Draw"))
        gs_draw_sprite(texture, 0, width, height);
    gs_blend_state_pop();
}

static void mosh_render(void *data, gs_effect_t *unused)
{
    UNUSED_PARAMETER(unused);
    struct mosh_filter *s = data;
    obs_source_t *target = obs_filter_get_target(s->source);
    const uint32_t width = target ? obs_source_get_base_width(target) : 0;
    const uint32_t height = target ? obs_source_get_base_height(target) : 0;
    if (!width || !height) {
        obs_source_skip_video_filter(s->source);
        return;
    }
    if (!s->capture) s->capture = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    if (!s->output) s->output = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    if (!s->capture || !s->output) {
        obs_source_skip_video_filter(s->source);
        return;
    }

    gs_texrender_reset(s->capture);
    if (!obs_source_process_filter_begin_with_color_space(
            s->source, GS_RGBA, GS_CS_SRGB, OBS_NO_DIRECT_RENDERING))
        return;
    if (!gs_texrender_begin(s->capture, width, height)) {
        obs_source_skip_video_filter(s->source);
        return;
    }
    gs_blend_state_push();
    gs_reset_blend_state();
    gs_enable_blending(false);
    gs_projection_push();
    gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);
    obs_source_process_filter_tech_end(
        s->source, obs_get_base_effect(OBS_EFFECT_DEFAULT),
        width, height, "DrawAlphaDivide");
    gs_projection_pop();
    gs_texrender_end(s->capture);
    gs_blend_state_pop();

    /* MPEG-4 dimensions must be macroblock-aligned. Keep the internal
     * processing height inexpensive while choosing the width from the live
     * source aspect, instead of cropping to a hard-coded 4:3 picture.
     * 1920x1080 -> 256x144; 640x480 -> 192x144; 1080x1920 -> 80x144.
     * OBS filter output itself remains the original width x height. */
    struct retro_video_options sized = s->options;
    uint64_t macroblocks = ((uint64_t)width * 144 + (uint64_t)height * 8) /
                           ((uint64_t)height * 16);
    if (macroblocks < 4) macroblocks = 4;
    if (macroblocks > 64) macroblocks = 64;
    sized.width = (int)(macroblocks * 16);
    sized.height = 144;
    if (!retro_video_obs_matches(s->codec, &sized)) {
        retro_video_obs_destroy(s->codec);
        s->codec = retro_video_obs_create(&sized);
        s->elapsed = 0.0f;
        s->due = true;
    }

    bool processed = s->codec && retro_video_obs_render(
        s->codec, s->capture, s->output, width, height, s->due);
    if (processed) s->due = false;
    gs_texrender_t *selected = processed ? s->output : s->capture;
    draw_texture(gs_texrender_get_texture(selected), width, height);
}

static struct obs_source_info mosh_info = {
    .id = "framerot_filter",
    .type = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
    .get_name = mosh_name,
    .create = mosh_create,
    .destroy = mosh_destroy,
    .update = mosh_update,
    .get_defaults = mosh_defaults,
    .get_properties = mosh_properties,
    .video_tick = mosh_tick,
    .video_render = mosh_render,
};

bool obs_module_load(void)
{
    obs_register_source(&mosh_info);
    return true;
}
