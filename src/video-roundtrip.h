/*
 * Derived in part from OBS Retro Effects (FiniteSingularity), through the
 * myrqyry/obs-retro-effects fork. Substantially modified for framerot in 2026.
 * GNU GPL version 2. See COPYING and README.md for attribution.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* In-memory FFmpeg encoder -> optional MPEG-4 packet alteration -> decoder.
 * Pixel formats vary by codec; no B frames or container muxing.
 * Only MPEG-4 Part 2 currently allows compressed-packet manipulation.
 */
enum retro_video_codec {
	RETRO_VIDEO_H263 = 1,        /* ITU-T H.263 baseline */
	RETRO_VIDEO_MPEG4_PART2 = 2, /* MPEG-4 Visual Part 2, Simple Profile */
	RETRO_VIDEO_CINEPAK = 3,     /* RGB vector quantization, CD-ROM video */
	RETRO_VIDEO_SVQ1 = 4,        /* Sorenson Video 1, YUV410P */
	RETRO_VIDEO_MSVIDEO1 = 5,    /* Microsoft Video 1, RGB555LE */
	RETRO_VIDEO_FLV1 = 6,        /* Sorenson Spark / Flash H.263 variant */
	RETRO_VIDEO_MPEG1 = 7,       /* MPEG-1 video */
	RETRO_VIDEO_MJPEG = 8,       /* Intra-frame JPEG, 4:2:2 */
};

struct retro_video;

/* Modes 0-3, 6 and 7 manipulate real MPEG-4 Part 2 compressed packets.
 * Mode 4 uses exported decoder motion vectors to deform RGBA output.
 * Mode 5 rewrites the P-VOP motion interpolation flag in MPEG-4 bytes.
 * Mode 6 replaces whole compressed keyframes with earlier valid keyframes.
 * Mode 7 replaces P packets with earlier compressed P packets in one GOP.
 * Mode 8 collages previously decoded frames in horizontal bands.
 * No mode rewrites individual MPEG-4 motion-vector VLC values. */
enum retro_mosh_mode {
    RETRO_MOSH_KEYFRAME_SKIP = 0, /* classic: skip selected I frames */
    RETRO_MOSH_PFRAME_DROP = 1,   /* periodic P-frame loss */
    RETRO_MOSH_PFRAME_BURST = 2,  /* contiguous bursts of P-frame loss */
    RETRO_MOSH_PFRAME_ECHO = 3,   /* replay a previous P packet */
    RETRO_MOSH_VECTOR_DRIFT = 4, /* warp decoded pixels with real MPEG-4 motion-vector side data */
    RETRO_MOSH_MOTION_SHIMMER = 5, /* rewrite the MPEG-4 P-VOP motion rounding flag */
    RETRO_MOSH_KEYFRAME_ANCHOR = 6, /* replay stored I-frame; P-frames continue live */
    RETRO_MOSH_PFRAME_REWIND = 7, /* compressed P packets from N frames earlier in GOP */
    RETRO_MOSH_FRAME_PEEL = 8, /* temporal scanline-band collage after decoding */
};

struct retro_video_options {
	enum retro_video_codec codec;
	int width;
	int height;
	int fps;
	int bitrate;                 /* target bits/second */
	int gop_size;                /* keyframe distance in frames */
	int mosh_skip_keyframes;    /* classic: skipped I frames before recovery */
    enum retro_mosh_mode mosh_mode; /* default zero remains classic */
    int mosh_p_interval;       /* mode 1-3: P packet cadence (2..60) */
    int mosh_p_burst;          /* mode 2: packets dropped per cycle (1..interval-1) */
    int mosh_vector_strength;   /* mode 4: 0..500%, vector displacement */
    int mosh_vector_persistence;/* mode 4: 0..100%, previous warped frame contribution */
    int mosh_rounding_interval; /* mode 5: toggle P-VOP rounding every N P frames (1..60) */
    int mosh_anchor_span;       /* mode 6: keyframe groups before anchor refresh (2..12) */
    int mosh_rewind_depth;      /* mode 7: P-packet history 2..12 frames, resets at I */
    int mosh_slice_history;     /* mode 8: decoded frame history 2..8 */
    int mosh_slice_height;      /* mode 8: band height 1..64 pixels */
	int cinepak_codebook_passes; /* 0..5, encoder codebook iterations */
	int svq1_motion_estimation;  /* 0..2, zero/EPZS/X1 */
	int mjpeg_quantizer;         /* 2..31, lower is higher quality, 0=default */
};

/* Distinct, illustrative starting profiles. Codec-specific historical defaults
 * vary by capture device; they are not claims about every old video file.
 * All currently map to 4:3 display, including non-square-pixel QCIF/PAL.
 */
struct retro_video_options retro_video_default_options(enum retro_video_codec codec);
const char *retro_video_codec_name(enum retro_video_codec codec);
bool retro_video_supported_size(enum retro_video_codec codec, int width, int height);
struct retro_video *retro_video_create(const struct retro_video_options *options);
void retro_video_destroy(struct retro_video *video);

/* RGBA top-to-bottom; stride >= 4 * width.
 * 1 decoded frame available, 0 codec still buffering, -1 error.
 * Caller must provide input in temporal order at the selected fps.
 */
int retro_video_process(struct retro_video *video, const uint8_t *rgba, int rgba_stride, uint8_t *decoded,
			int decoded_stride);
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
