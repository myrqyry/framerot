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

/* Returns the latest decoded texture (codec resolution), or NULL to bypass.
 * Input is optional on ticks with no new capture. OBS owns final placement. */
void retro_video_obs_controls(struct retro_video_obs *, const struct retro_video_options *);
void retro_video_obs_hold(struct retro_video_obs *, bool hold);
gs_texture_t *retro_video_obs_render(struct retro_video_obs *, gs_texrender_t *input, bool encode_frame);
