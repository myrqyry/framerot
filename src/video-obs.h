/*
 * Derived in part from OBS Retro Effects (FiniteSingularity), through the
 * myrqyry/obs-retro-effects fork. Substantially modified for framerot in 2026.
 * GNU GPL version 2. See COPYING and README.md for attribution.
 */
#pragma once

#include "video-roundtrip.h"

#include <obs-module.h>
#include <stdbool.h>
#include <stdint.h>

struct retro_video_obs;

struct retro_video_obs *retro_video_obs_create(const struct retro_video_options *options);
void retro_video_obs_destroy(struct retro_video_obs *state);
bool retro_video_obs_matches(const struct retro_video_obs *state, const struct retro_video_options *options);

/* Real video encode/decode. The output is the source-canvas-sized frame,
 * using the whole source canvas. Holds the last decoded frame until due.
 * false = unavailable/errored; caller may bypass.
 */
bool retro_video_obs_render(struct retro_video_obs *state, gs_texrender_t *input, gs_texrender_t *output,
			    uint32_t output_width, uint32_t output_height, bool encode_frame);
