/*
 * Derived in part from OBS Retro Effects (FiniteSingularity), through the
 * myrqyry/obs-retro-effects fork. Substantially modified for framerot in 2026.
 * GNU GPL version 2. See COPYING and README.md for attribution.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* In-memory MPEG-4 Visual Simple Profile encoder, packet editor and decoder.
 * No container muxing or B frames. Historical codecs live in Retro Effects. */
enum retro_video_codec { RETRO_VIDEO_MPEG4_PART2 = 2 };

#ifdef __cplusplus
extern "C" {
#endif
struct retro_video;

/* Modes 0-3, 6 and 7 manipulate real MPEG-4 Part 2 compressed packets.
 * Mode 4 uses exported decoder motion vectors to deform RGBA output.
 * Mode 5 rewrites the P-VOP motion interpolation flag in MPEG-4 bytes.
 * Mode 6 replaces whole compressed keyframes with earlier valid keyframes.
 * Mode 7 replaces P packets with earlier compressed P packets in one GOP.
 * Mode 8 collages previously decoded frames in horizontal bands.
 * Instrument can rewrite compressed motion deltas and inter residuals. */
enum retro_mosh_mode {
    RETRO_MOSH_KEYFRAME_SKIP = 0, /* classic: skip selected I frames */
    RETRO_MOSH_PFRAME_DROP = 1,   /* periodic P-frame loss */
    RETRO_MOSH_PFRAME_BURST = 2,  /* contiguous bursts of P-frame loss */
    RETRO_MOSH_PFRAME_ECHO = 3,   /* replay a previous P packet */
    RETRO_MOSH_VECTOR_DRIFT = 4, /* warp decoded pixels with real MPEG-4 motion-vector side data */
    RETRO_MOSH_MOTION_SHIMMER = 5, /* rewrite the MPEG-4 P-VOP motion rounding flag */
    RETRO_MOSH_KEYFRAME_ANCHOR = 6, /* replay stored I-frame; P-frames continue live */
    RETRO_MOSH_PFRAME_REWIND = 7, /* compressed P packets from N frames earlier in GOP */
    RETRO_MOSH_INSTRUMENT = 9, /* independent packet stages and feedback layers */
    RETRO_MOSH_FRAME_PEEL = 8, /* temporal scanline-band collage after decoding */
};

enum retro_reference { RETRO_REFERENCE_LIVE, RETRO_REFERENCE_SKIP, RETRO_REFERENCE_ANCHOR };
enum retro_predictive { RETRO_PREDICTIVE_LIVE, RETRO_PREDICTIVE_DROP, RETRO_PREDICTIVE_BURST, RETRO_PREDICTIVE_ECHO, RETRO_PREDICTIVE_REWIND };
enum retro_layers { RETRO_LAYER_FLOW=1, RETRO_LAYER_PEEL=2, RETRO_LAYER_SHIMMER=4, RETRO_LAYER_COMPRESSED=8 };

struct retro_video_options {
	enum retro_video_codec codec;
	int width;
	int height;
	int fps;
	int bitrate;                 /* target bits/second */
	int gop_size;                /* keyframe distance in frames */
	int mosh_skip_keyframes;    /* classic: skipped I frames before recovery */
    enum retro_mosh_mode mosh_mode; /* default zero remains classic */
    enum retro_reference mosh_reference;
    enum retro_predictive mosh_predictive;
    unsigned mosh_layers;
    int mosh_mv_x, mosh_mv_y, mosh_residual, mosh_residual_loss;
    int mosh_phantom; /* percent of skipped blocks injected with motion, 0 disables */
    int mosh_frequency[3]; /* inter residual scan bands, -100..200% delta, 0 neutral */
    int mosh_p_interval;       /* mode 1-3: P packet cadence (2..60) */
    int mosh_p_burst;          /* mode 2: packets dropped per cycle (1..interval-1) */
    int mosh_vector_strength;   /* mode 4: 0..500%, vector displacement */
    int mosh_vector_persistence;/* mode 4: 0..100%, previous warped frame contribution */
    int mosh_rounding_interval; /* mode 5: toggle P-VOP rounding every N P frames (1..60) */
    int mosh_anchor_span;       /* mode 6: keyframe groups before anchor refresh (2..12) */
    int mosh_rewind_depth;      /* mode 7: P-packet history 2..12 frames, resets at I */
    int mosh_slice_history;     /* mode 8: decoded frame history 2..8 */
    int mosh_slice_height;      /* mode 8: band height 1..64 pixels */
};

/* Default MPEG-4 encoder options. */
struct retro_video_options retro_video_default_options(enum retro_video_codec codec);
/* Seeded, bounded creative Instrument starting point; reference dimensions 16:9. */
struct retro_video_options retro_video_random_instrument(uint64_t seed);
bool retro_video_supported_size(enum retro_video_codec codec, int width, int height);
struct retro_video *retro_video_create(const struct retro_video_options *options);
void retro_video_destroy(struct retro_video *video);
/* Codec-thread control, active on subsequent input. */
void retro_video_set_anchor_hold(struct retro_video *, bool hold);
bool retro_video_set_live_options(struct retro_video *, const struct retro_video_options *);

/* RGBA top-to-bottom; stride >= 4 * width.
 * 1 decoded frame available, 0 codec still buffering, -1 error.
 * Caller must provide input in temporal order at the selected fps.
 */
int retro_video_process(struct retro_video *video, const uint8_t *rgba, int rgba_stride, uint8_t *decoded,
			int decoded_stride);
uint64_t retro_video_compressed_packets(const struct retro_video *);
uint64_t retro_video_compressed_vectors(const struct retro_video *);
uint64_t retro_video_compressed_coefficients(const struct retro_video *);
uint64_t retro_video_compressed_blocks(const struct retro_video *);
uint64_t retro_video_compressed_phantom_blocks(const struct retro_video *);
uint64_t retro_video_compressed_frequency_coefficients(const struct retro_video *,int band);
uint64_t retro_video_compressed_unsupported(const struct retro_video *);
uint64_t retro_video_encoded_bytes(const struct retro_video *video);
uint64_t retro_video_encoded_packets(const struct retro_video *video);
uint64_t retro_video_encoded_keyframes(const struct retro_video *video);
uint64_t retro_video_dropped_keyframes(const struct retro_video *video);
uint64_t retro_video_dropped_pframes(const struct retro_video *video);
uint64_t retro_video_replayed_pframes(const struct retro_video *video);
uint64_t retro_video_motion_vectors_seen(const struct retro_video *video);
uint64_t retro_video_motion_blocks_warped(const struct retro_video *video);
uint64_t retro_video_motion_rounding_flips(const struct retro_video *video);
uint64_t retro_video_anchored_keyframes(const struct retro_video *video);
uint64_t retro_video_anchor_refreshes(const struct retro_video *video);
uint64_t retro_video_rewound_pframes(const struct retro_video *video);
uint64_t retro_video_temporal_rows_reused(const struct retro_video *video);

/* Parsed from the decoder's actual video bitstream after frame decoding.
 * Diagnostic metadata: FFmpeg profile ID and sample-aspect numerator/denominator.
 */
int retro_video_decoded_profile(const struct retro_video *video);
int retro_video_decoded_sar_num(const struct retro_video *video);
int retro_video_decoded_sar_den(const struct retro_video *video);

#ifdef __cplusplus
}
#endif
