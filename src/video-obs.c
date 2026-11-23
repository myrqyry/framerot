/*
 * Derived in part from OBS Retro Effects (FiniteSingularity), through the
 * myrqyry/obs-retro-effects fork. Substantially modified for framerot in 2026.
 * GNU GPL version 2. See COPYING and README.md for attribution.
 */
#include "video-obs.h"
#include "video-roundtrip.h"

#include "obs-utils.h"
#include "codec-worker.h"
#include <util/platform.h>

#include <string.h>

struct retro_video_obs {
	struct codec_worker *worker;
	gs_texrender_t *capture;
	gs_stagesurf_t *stage;
	gs_texture_t *decoded_texture;
	bool staged;
    uint64_t map_ns, map_max_ns, maps;
	uint8_t *output_pixels;
	struct retro_video_options options;
	int width, height;
	bool decoded;
};

/* Codec dimensions are chosen close to the source aspect ratio; render the
 * entire image into that buffer and fill the entire OBS source canvas on
 * output. The old 4:3 crop/letterbox path discarded parts of wide sources. */
static bool render_full_frame(gs_texture_t *source, gs_texrender_t *target,
                              uint32_t target_w, uint32_t target_h)
{
    gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
    if (!image || !source) return false;

    set_render_parameters();
    set_blending_parameters();
    gs_effect_set_texture(image, source);
    bool ok = gs_texrender_begin(target, target_w, target_h);
    if (ok) {
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
    return ok;
}

struct retro_video_obs *retro_video_obs_create(const struct retro_video_options *options)
{
	if (!options || !retro_video_supported_size(options->codec, options->width, options->height))
		return NULL;

	struct retro_video_obs *state = bzalloc(sizeof(*state));
	if (!state)
		return NULL;
	state->width = options->width;
	state->height = options->height;
	state->options = *options;
	state->worker = codec_worker_create(options);
	if (!state->worker)
		goto error;

	size_t bytes = (size_t)options->width * options->height * 4;
	state->output_pixels = bzalloc(bytes);
	state->capture = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	state->stage = gs_stagesurface_create(options->width, options->height, GS_RGBA);
	state->decoded_texture = gs_texture_create(options->width, options->height, GS_RGBA, 1, NULL, GS_DYNAMIC);
	if (!state->output_pixels || !state->capture || !state->stage ||
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
	struct codec_worker_stats stats = codec_worker_stats(state->worker);
    if (stats.submitted || state->maps)
        blog(LOG_INFO, "[framerot] codec %dx%d submitted=%llu completed=%llu dropped=%llu errors=%llu recovered=%llu avg_ms=%.3f max_ms=%.3f readback_avg_ms=%.3f readback_max_ms=%.3f",
            state->width,state->height,(unsigned long long)stats.submitted,(unsigned long long)stats.completed,(unsigned long long)stats.dropped,
            (unsigned long long)stats.errors,(unsigned long long)stats.recovered,
            (double)stats.total_codec_ns / (double)(stats.completed+stats.errors ? stats.completed+stats.errors : 1)/1e6,
            (double)stats.max_codec_ns/1e6, (double)state->map_ns/(double)(state->maps ? state->maps : 1)/1e6,
            (double)state->map_max_ns/1e6);
    if(stats.edited_packets||stats.edit_unsupported)
        blog(LOG_INFO,"[framerot] compressed packets=%llu motion_deltas=%llu coefficients=%llu erased_blocks=%llu phantom_blocks=%llu spectral_low=%llu spectral_mid=%llu spectral_high=%llu unsupported=%llu",
            (unsigned long long)stats.edited_packets,(unsigned long long)stats.edited_vectors,
            (unsigned long long)stats.edited_coefficients,(unsigned long long)stats.edited_blocks,
            (unsigned long long)stats.phantom_blocks,
            (unsigned long long)stats.frequency_coefficients[0],
            (unsigned long long)stats.frequency_coefficients[1],
            (unsigned long long)stats.frequency_coefficients[2],
            (unsigned long long)stats.edit_unsupported);
    codec_worker_destroy(state->worker);
	obs_enter_graphics();
	if (state->decoded_texture)
		gs_texture_destroy(state->decoded_texture);
	if (state->stage)
		gs_stagesurface_destroy(state->stage);
	if (state->capture)
		gs_texrender_destroy(state->capture);
	obs_leave_graphics();
	bfree(state->output_pixels);
	bfree(state);
}

bool retro_video_obs_matches(const struct retro_video_obs *state, const struct retro_video_options *options)
{
	if (!state || !options)
		return false;
	const struct retro_video_options *a = &state->options;
    return a->codec==options->codec && a->width==options->width && a->height==options->height &&
        a->fps==options->fps && a->bitrate==options->bitrate && a->gop_size==options->gop_size &&
        a->mosh_mode==options->mosh_mode &&
        (a->mosh_mode!=RETRO_MOSH_INSTRUMENT ||
         (a->mosh_reference==options->mosh_reference && a->mosh_predictive==options->mosh_predictive &&
          a->mosh_layers==options->mosh_layers)) &&
        a->mosh_rewind_depth==options->mosh_rewind_depth && a->mosh_slice_history==options->mosh_slice_history;
}

/* Map only a previously staged capture. This separates the GPU copy from
 * the CPU map; the driver may still block on map, so measure that cost. */
void retro_video_obs_controls(struct retro_video_obs *s,const struct retro_video_options *o) {if(s)codec_worker_set_controls(s->worker,o);}

void retro_video_obs_hold(struct retro_video_obs *state,bool hold) { if(state) codec_worker_set_anchor_hold(state->worker,hold); }

gs_texture_t *retro_video_obs_render(struct retro_video_obs *state, gs_texrender_t *input, bool encode_frame)
{
    if (!state) return NULL;
    if (codec_worker_faulted(state->worker)) state->decoded=false;
    if (codec_worker_take(state->worker,state->output_pixels)) {
        gs_texture_set_image(state->decoded_texture,state->output_pixels,state->width*4,false);
        state->decoded=true;
    }
    if (state->staged) {
        uint8_t *mapped=NULL; uint32_t stride=0;
        uint64_t start=os_gettime_ns();
        if (gs_stagesurface_map(state->stage,&mapped,&stride)) {
            uint64_t ns=os_gettime_ns()-start;
            state->maps++; state->map_ns+=ns;
            if(ns>state->map_max_ns) state->map_max_ns=ns;
            codec_worker_submit(state->worker,mapped,(int)stride);
            gs_stagesurface_unmap(state->stage);
        }
        state->staged=false;
    }
    if (encode_frame && input) {
        gs_texture_t *source=gs_texrender_get_texture(input);
        gs_texrender_reset(state->capture);
        if (render_full_frame(source,state->capture,state->width,state->height)) {
            gs_texture_t *captured=gs_texrender_get_texture(state->capture);
            if (captured) {gs_stage_texture(state->stage,captured);state->staged=true;}
        }
    }
    return state->decoded ? state->decoded_texture : NULL;
}
