/*
 * Derived in part from OBS Retro Effects (FiniteSingularity), through the
 * myrqyry/obs-retro-effects fork. Substantially modified for gop-rot in 2026.
 * GNU GPL version 2. See COPYING and README.md for attribution.
 */
#include "video-obs.h"
#include "video-roundtrip.h"

#include "obs-utils.h"

#include <string.h>

struct retro_video_obs {
	struct retro_video *codec;
	enum retro_video_codec codec_type;
	gs_texrender_t *capture;
	gs_stagesurf_t *stage;
	gs_texture_t *decoded_texture;
	uint8_t *input_pixels;
	uint8_t *output_pixels;
	struct retro_video_options options;
	int width, height;
	bool decoded;
};

/* Codec dimensions are chosen close to the source aspect ratio; render the
 * entire image into that buffer and fill the entire OBS source canvas on
 * output. The old 4:3 crop/letterbox path discarded parts of wide sources. */
static void render_full_frame(gs_texture_t *source, gs_texrender_t *target,
                              uint32_t target_w, uint32_t target_h)
{
    gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
    if (!image) return;

    set_render_parameters();
    set_blending_parameters();
    gs_effect_set_texture(image, source);
    if (gs_texrender_begin(target, target_w, target_h)) {
        struct vec4 black;
        vec4_set(&black, 0.f, 0.f, 0.f, 1.f);
        gs_clear(GS_CLEAR_COLOR, &black, 1.0f, 0);
        gs_projection_push();
        gs_ortho(0.0f, (float)target_w, 0.0f, (float)target_h, -100.0f, 100.0f);
        while (gs_effect_loop(effect, "Draw"))
            gs_draw_sprite(source, 0, target_w, target_h);
        gs_projection_pop();
        gs_texrender_end(target);
    }
    gs_blend_state_pop();
}

struct retro_video_obs *retro_video_obs_create(const struct retro_video_options *options)
{
	if (!options || !retro_video_supported_size(options->codec, options->width, options->height))
		return NULL;

	struct retro_video_obs *state = bzalloc(sizeof(*state));
	if (!state)
		return NULL;
	state->codec_type = options->codec;
	state->width = options->width;
	state->height = options->height;
	state->options = *options;
	state->codec = retro_video_create(options);
	if (!state->codec)
		goto error;

	size_t bytes = (size_t)options->width * options->height * 4;
	state->input_pixels = bzalloc(bytes);
	state->output_pixels = bzalloc(bytes);
	state->capture = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	state->stage = gs_stagesurface_create(options->width, options->height, GS_RGBA);
	state->decoded_texture = gs_texture_create(options->width, options->height, GS_RGBA, 1, NULL, GS_DYNAMIC);
	if (!state->input_pixels || !state->output_pixels || !state->capture || !state->stage ||
	    !state->decoded_texture)
		goto error;
	return state;
error:
	retro_video_obs_destroy(state);
	return NULL;
}

void retro_video_obs_destroy(struct retro_video_obs *state)
{
	if (!state)
		return;
	retro_video_destroy(state->codec);
	obs_enter_graphics();
	if (state->decoded_texture)
		gs_texture_destroy(state->decoded_texture);
	if (state->stage)
		gs_stagesurface_destroy(state->stage);
	if (state->capture)
		gs_texrender_destroy(state->capture);
	obs_leave_graphics();
	bfree(state->input_pixels);
	bfree(state->output_pixels);
	bfree(state);
}

bool retro_video_obs_matches(const struct retro_video_obs *state, const struct retro_video_options *options)
{
	if (!state || !options)
		return false;
	const struct retro_video_options *a = &state->options;
	return a->codec == options->codec && a->width == options->width && a->height == options->height &&
	       a->fps == options->fps && a->bitrate == options->bitrate && a->gop_size == options->gop_size && a->mosh_skip_keyframes == options->mosh_skip_keyframes && a->mosh_mode == options->mosh_mode &&
           a->mosh_p_interval == options->mosh_p_interval &&
           a->mosh_p_burst == options->mosh_p_burst &&
           a->mosh_vector_strength == options->mosh_vector_strength &&
           a->mosh_vector_persistence == options->mosh_vector_persistence &&
           a->mosh_rounding_interval == options->mosh_rounding_interval &&
           a->mosh_anchor_span == options->mosh_anchor_span &&
           a->mosh_rewind_depth == options->mosh_rewind_depth &&
           a->mosh_slice_history == options->mosh_slice_history &&
           a->mosh_slice_height == options->mosh_slice_height &&
	       a->cinepak_codebook_passes == options->cinepak_codebook_passes &&
	       a->svq1_motion_estimation == options->svq1_motion_estimation &&
	       a->mjpeg_quantizer == options->mjpeg_quantizer;
}

bool retro_video_obs_render(struct retro_video_obs *state, gs_texrender_t *input, gs_texrender_t *output,
			    uint32_t output_width, uint32_t output_height, bool encode_frame)
{
	if (!state || !input || !output || !output_width || !output_height)
		return false;

	if (encode_frame || !state->decoded) {
		gs_texture_t *source = gs_texrender_get_texture(input);
		if (!source)
			return false;
		gs_texrender_reset(state->capture);
		render_full_frame(source, state->capture, state->width, state->height);
		gs_texture_t *captured = gs_texrender_get_texture(state->capture);
		if (!captured)
			return false;
		gs_stage_texture(state->stage, captured);
		uint8_t *mapped = NULL;
		uint32_t linesize = 0;
		if (!gs_stagesurface_map(state->stage, &mapped, &linesize))
			return false;
		for (int y = 0; y < state->height; ++y)
			memcpy(state->input_pixels + (size_t)y * state->width * 4, mapped + (size_t)y * linesize,
			       (size_t)state->width * 4);
		gs_stagesurface_unmap(state->stage);
		int result = retro_video_process(state->codec, state->input_pixels, state->width * 4,
						 state->output_pixels, state->width * 4);
		if (result < 0)
			return false;
		if (result == 1) {
			gs_texture_set_image(state->decoded_texture, state->output_pixels, state->width * 4, false);
			state->decoded = true;
		}
	}
	if (!state->decoded)
		return false;
	gs_texrender_reset(output);
	render_full_frame(state->decoded_texture, output, output_width, output_height);
	return true;
}
