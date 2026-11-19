/*
 * Derived in part from OBS Retro Effects (FiniteSingularity), through the
 * myrqyry/obs-retro-effects fork. Substantially modified for framerot in 2026.
 * GNU GPL version 2. See COPYING and README.md for attribution.
 */
#include "video-roundtrip.h"

#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/motion_vector.h>
#include <libswscale/swscale.h>

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct retro_video {
	AVCodecContext *encoder;
	AVCodecContext *decoder;
	AVFrame *input;
	AVFrame *decoded;
	AVPacket *packet;
	AVPacket *echo_packet;
    AVPacket *anchor_packet;
    uint64_t anchor_keyframes_seen;
    uint64_t anchor_replays;
    uint64_t anchor_refreshes;
    int anchor_span;
    AVPacket *rewind_packets[12]; /* fixed-depth compressed P history, never grows */
    AVPacket *rewind_incoming;
    int rewind_depth;
    uint64_t rewind_seen;
    uint64_t rewind_replays;
    uint8_t *slice_frames; /* bounded ring of clean decoded RGBA pictures */
    int slice_history;
    int slice_height;
    int slice_count;
    int slice_next;
    uint64_t slice_reused_rows;
    uint8_t *flow_previous; /* tight RGBA, last warped frame */
    bool flow_has_previous;
    int flow_strength;
    int flow_persistence;
    uint64_t motion_vectors_seen;
    uint64_t motion_blocks_warped;
    int rounding_interval;
    uint64_t rounding_p_seen;
    uint64_t rounding_flips;
	struct SwsContext *rgba_to_yuv;
	struct SwsContext *yuv_to_rgba;
	enum AVPixelFormat encode_pixel_format;
	int64_t pts;
	uint64_t bytes;
	uint64_t packets;
	uint64_t keyframes;
	uint64_t dropped_keyframes;
    uint64_t dropped_pframes;
    uint64_t replayed_pframes;
    uint64_t p_seen;
    enum retro_mosh_mode mosh_mode;
    int p_interval;
    int p_burst;
	int mosh_skip_keyframes;
	int skipped_in_cycle;
	bool has_decoder_reference;
};

struct retro_video_preset {
	enum retro_video_codec kind;
	const char *name;
	enum AVCodecID avcodec;
	enum AVPixelFormat input_format;
	int width, height, fps, bitrate, gop;
};

/* Capture targets are examples chosen by video era; not archival device claims.
 * Cinepak and MS Video 1 are RGB; SVQ1 uses YUV410P; MJPEG uses full-range
 * JPEG YUV422P. Do not flatten these to a shared YUV420P pipeline. */
static const struct retro_video_preset presets[] = {
	{RETRO_VIDEO_H263, "H.263 baseline", AV_CODEC_ID_H263, AV_PIX_FMT_YUV420P, 176, 144, 10, 64000, 30},
	{RETRO_VIDEO_MPEG4_PART2, "MPEG-4 Visual Simple Profile", AV_CODEC_ID_MPEG4, AV_PIX_FMT_YUV420P, 176, 144, 10,
	 64000, 30},
	{RETRO_VIDEO_CINEPAK, "Cinepak", AV_CODEC_ID_CINEPAK, AV_PIX_FMT_RGB24, 320, 240, 15, 300000, 15},
	{RETRO_VIDEO_SVQ1, "Sorenson Video 1 (SVQ1)", AV_CODEC_ID_SVQ1, AV_PIX_FMT_YUV410P, 320, 240, 15, 250000, 15},
	{RETRO_VIDEO_MSVIDEO1, "Microsoft Video 1", AV_CODEC_ID_MSVIDEO1, AV_PIX_FMT_RGB555LE, 160, 120, 15, 180000,
	 15},
	{RETRO_VIDEO_FLV1, "Sorenson Spark (FLV1)", AV_CODEC_ID_FLV1, AV_PIX_FMT_YUV420P, 320, 240, 15, 200000, 15},
	{RETRO_VIDEO_MPEG1, "MPEG-1 PAL CD video", AV_CODEC_ID_MPEG1VIDEO, AV_PIX_FMT_YUV420P, 352, 288, 25, 1150000,
	 15},
	{RETRO_VIDEO_MJPEG, "Motion JPEG 4:2:2", AV_CODEC_ID_MJPEG, AV_PIX_FMT_YUVJ422P, 320, 240, 15, 1500000, 15},
};

/* Bounded bit reader for the specific MPEG-4 Simple Profile packets generated
 * by this filter's encoder. Fail closed if header markers do not match.
 * This is NOT a generic MPEG-4 parser (VOL changes, B/S-VOP, resync packets,
 * data partitioning and interlacing are out of scope). */
static int get_packet_bit(const uint8_t *data, size_t len, size_t *bit)
{
    if (len > SIZE_MAX / 8 || *bit >= len * 8)
        return -1;
    int value = (data[*bit / 8] >> (7 - *bit % 8)) & 1;
    (*bit)++;
    return value;
}

static bool locate_rounding_bit(const uint8_t *data, size_t len, int fps,
                                size_t *out_byte, uint8_t *out_mask)
{
    if (!data || !out_byte || !out_mask || fps < 1 || fps > 60 || len < 10)
        return false;
    size_t base = SIZE_MAX;
    for (size_t i = 0; i + 4 < len; ++i)
        if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 1 && data[i+3] == 0xb6) {
            if (base != SIZE_MAX) return false; /* never edit multi-VOP packets */
            base = i + 4;
        }
    if (base == SIZE_MAX) return false;
    size_t bit = 0;
    int type = get_packet_bit(data + base, len - base, &bit);
    int type_low = get_packet_bit(data + base, len - base, &bit);
    if (type != 0 || type_low != 1) return false; /* P only */
    int b = 0;
    for (int i = 0; i < 16; ++i) {
        b = get_packet_bit(data + base, len - base, &bit);
        if (b <= 0) break;
    }
    if (b != 0 || get_packet_bit(data + base, len - base, &bit) != 1)
        return false;
    int bits = 1;
    while ((1 << bits) < fps) bits++;
    for (int i = 0; i < bits; ++i)
        if (get_packet_bit(data + base, len - base, &bit) < 0)
            return false;
    if (get_packet_bit(data + base, len - base, &bit) != 1 ||
        get_packet_bit(data + base, len - base, &bit) != 1)
        return false; /* marker bit and vop_coded */
    size_t rounding_bit = bit;
    if (get_packet_bit(data + base, len - base, &bit) < 0)
        return false;
    for (int i = 0; i < 3; ++i) /* intra_dc_vlc_thr */
        if (get_packet_bit(data + base, len - base, &bit) < 0)
            return false;
    int q = 0, fcode = 0;
    for (int i = 0; i < 5; ++i) {
        int v = get_packet_bit(data + base, len - base, &bit);
        if (v < 0) return false;
        q = (q << 1) | v;
    }
    for (int i = 0; i < 3; ++i) {
        int v = get_packet_bit(data + base, len - base, &bit);
        if (v < 0) return false;
        fcode = (fcode << 1) | v;
    }
    if (q == 0 || fcode == 0)
        return false;
    *out_byte = base + rounding_bit / 8;
    *out_mask = (uint8_t)(1 << (7 - rounding_bit % 8));
    return true;
}

static const struct retro_video_preset *find_preset(enum retro_video_codec codec)
{
	for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); ++i)
		if (presets[i].kind == codec)
			return &presets[i];
	return NULL;
}

struct retro_video_options retro_video_default_options(enum retro_video_codec codec)
{
	const struct retro_video_preset *preset = find_preset(codec);
	if (!preset)
		return (struct retro_video_options){0};
	return (struct retro_video_options){.codec = codec,
					    .width = preset->width,
					    .height = preset->height,
					    .fps = preset->fps,
					    .bitrate = preset->bitrate,
					    .gop_size = preset->gop};
}

const char *retro_video_codec_name(enum retro_video_codec codec)
{
	const struct retro_video_preset *preset = find_preset(codec);
	return preset ? preset->name : NULL;
}

bool retro_video_supported_size(enum retro_video_codec codec, int width, int height)
{
	if (codec == RETRO_VIDEO_H263)
		return (width == 128 && height == 96) || (width == 176 && height == 144) ||
		       (width == 352 && height == 288) || (width == 704 && height == 576) ||
		       (width == 1408 && height == 1152);
	if (codec == RETRO_VIDEO_MPEG4_PART2)
		return width >= 16 && height >= 16 && width <= 2048 && height <= 2048 && width % 16 == 0 &&
		       height % 16 == 0;
	if (find_preset(codec))
		return width >= 16 && height >= 16 && width <= 2048 && height <= 2048 && width % 4 == 0 &&
		       height % 4 == 0;
	return false;
}

void retro_video_destroy(struct retro_video *ctx)
{
	if (!ctx)
		return;
	sws_freeContext(ctx->rgba_to_yuv);
	sws_freeContext(ctx->yuv_to_rgba);
    free(ctx->slice_frames);
    for (int i = 0; i < 12; ++i) av_packet_free(&ctx->rewind_packets[i]);
    av_packet_free(&ctx->rewind_incoming);
    free(ctx->flow_previous);
	av_packet_free(&ctx->anchor_packet);
	av_packet_free(&ctx->echo_packet);
	av_packet_free(&ctx->packet);
	av_frame_free(&ctx->decoded);
	av_frame_free(&ctx->input);
	avcodec_free_context(&ctx->decoder);
	avcodec_free_context(&ctx->encoder);
	free(ctx);
}

struct retro_video *retro_video_create(const struct retro_video_options *options)
{
	if (!options || !retro_video_supported_size(options->codec, options->width, options->height) ||
	    options->fps < 1 || options->fps > 60 || options->bitrate < 1000 || options->bitrate > 20000000 ||
	    options->gop_size < 1 || options->gop_size > 1000 ||
	    options->mosh_skip_keyframes < 0 || options->mosh_skip_keyframes > 16 ||
        options->mosh_mode < RETRO_MOSH_KEYFRAME_SKIP || options->mosh_mode > RETRO_MOSH_FRAME_PEEL ||
        (options->mosh_mode >= RETRO_MOSH_PFRAME_DROP && options->mosh_mode <= RETRO_MOSH_PFRAME_ECHO &&
         (options->mosh_p_interval < 2 || options->mosh_p_interval > 60)) ||
        (options->mosh_mode == RETRO_MOSH_PFRAME_BURST &&
         (options->mosh_p_burst < 1 || options->mosh_p_burst >= options->mosh_p_interval)) ||
        options->mosh_vector_strength < 0 || options->mosh_vector_strength > 500 ||
        options->mosh_vector_persistence < 0 || options->mosh_vector_persistence > 100 ||
        (options->mosh_mode == RETRO_MOSH_MOTION_SHIMMER &&
         (options->mosh_rounding_interval < 1 || options->mosh_rounding_interval > 60)) ||
        (options->mosh_mode == RETRO_MOSH_KEYFRAME_ANCHOR &&
         (options->mosh_anchor_span < 2 || options->mosh_anchor_span > 12)) ||
        (options->mosh_mode == RETRO_MOSH_PFRAME_REWIND &&
         (options->mosh_rewind_depth < 2 || options->mosh_rewind_depth > 12)) ||
        (options->mosh_mode == RETRO_MOSH_FRAME_PEEL &&
         (options->mosh_slice_history < 2 || options->mosh_slice_history > 8 ||
          options->mosh_slice_height < 1 || options->mosh_slice_height > 64)) ||
        ((options->mosh_skip_keyframes > 0 || options->mosh_mode != RETRO_MOSH_KEYFRAME_SKIP) &&
         options->codec != RETRO_VIDEO_MPEG4_PART2))
		return NULL;

	const struct retro_video_preset *preset = find_preset(options->codec);
	if (!preset)
		return NULL;
	const AVCodec *encoder = avcodec_find_encoder(preset->avcodec);
	const AVCodec *decoder = avcodec_find_decoder(preset->avcodec);
	if (encoder && encoder->pix_fmts) {
		bool format_supported = false;
		for (const enum AVPixelFormat *fmt = encoder->pix_fmts; *fmt != AV_PIX_FMT_NONE; ++fmt)
			if (*fmt == preset->input_format)
				format_supported = true;
		if (!format_supported)
			return NULL;
	}
	if (!encoder || !decoder)
		return NULL;

	struct retro_video *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return NULL;

	ctx->encoder = avcodec_alloc_context3(encoder);
	ctx->decoder = avcodec_alloc_context3(decoder);
	ctx->input = av_frame_alloc();
	ctx->decoded = av_frame_alloc();
	ctx->packet = av_packet_alloc();
	if (!ctx->encoder || !ctx->decoder || !ctx->input || !ctx->decoded || !ctx->packet)
		goto error;

	ctx->mosh_skip_keyframes = options->mosh_skip_keyframes;
    ctx->mosh_mode = options->mosh_mode;
    ctx->p_interval = options->mosh_p_interval;
    ctx->p_burst = options->mosh_p_burst;
    ctx->rounding_interval = options->mosh_rounding_interval;
    ctx->anchor_span = options->mosh_anchor_span;
    ctx->rewind_depth = options->mosh_rewind_depth;
    ctx->slice_history = options->mosh_slice_history;
    ctx->slice_height = options->mosh_slice_height;
    if (ctx->mosh_mode == RETRO_MOSH_PFRAME_REWIND) {
        ctx->rewind_incoming = av_packet_alloc();
        if (!ctx->rewind_incoming) goto error;
        for (int i = 0; i < ctx->rewind_depth; ++i) {
            ctx->rewind_packets[i] = av_packet_alloc();
            if (!ctx->rewind_packets[i]) goto error;
        }
    }
    if (ctx->mosh_mode == RETRO_MOSH_FRAME_PEEL) {
        const size_t bytes = (size_t)options->width * (size_t)options->height * 4;
        /* A generic high-resolution API caller must not create a huge
         * unbounded feedback allocation; OBS's codec is only 144px high. */
        if (bytes > (32u * 1024u * 1024u) / (size_t)ctx->slice_history)
            goto error;
        ctx->slice_frames = calloc(ctx->slice_history, bytes);
        if (!ctx->slice_frames) goto error;
    }
    if (ctx->mosh_mode == RETRO_MOSH_KEYFRAME_ANCHOR) {
        ctx->anchor_packet = av_packet_alloc();
        if (!ctx->anchor_packet) goto error;
    }
    ctx->flow_strength = options->mosh_vector_strength;
    ctx->flow_persistence = options->mosh_vector_persistence;
    if (ctx->mosh_mode == RETRO_MOSH_VECTOR_DRIFT) {
        ctx->flow_previous = calloc((size_t)options->width * options->height * 4, 1);
        if (!ctx->flow_previous) goto error;
        ctx->decoder->flags2 |= AV_CODEC_FLAG2_EXPORT_MVS;
    }
    if (ctx->mosh_mode == RETRO_MOSH_PFRAME_ECHO) {
        ctx->echo_packet = av_packet_alloc();
        if (!ctx->echo_packet) goto error;
    }
	ctx->encoder->width = options->width;
	ctx->encoder->height = options->height;
	ctx->encoder->pix_fmt = preset->input_format;
	ctx->encode_pixel_format = preset->input_format;
	ctx->encoder->time_base = (AVRational){1, options->fps};
	ctx->encoder->framerate = (AVRational){options->fps, 1};
	ctx->encoder->bit_rate = options->bitrate;
	ctx->encoder->gop_size = options->gop_size;
	ctx->encoder->max_b_frames = 0;
	/* Only expose parameters implemented by the selected encoder. */
	if (options->codec == RETRO_VIDEO_CINEPAK) {
		if (options->cinepak_codebook_passes < 0 || options->cinepak_codebook_passes > 5 ||
		    av_opt_set_int(ctx->encoder->priv_data, "max_extra_cb_iterations", options->cinepak_codebook_passes,
				   0) < 0)
			goto error;
	} else if (options->codec == RETRO_VIDEO_SVQ1) {
		if (options->svq1_motion_estimation < 0 || options->svq1_motion_estimation > 2 ||
		    av_opt_set_int(ctx->encoder->priv_data, "motion-est", options->svq1_motion_estimation, 0) < 0)
			goto error;
	} else if (options->codec == RETRO_VIDEO_MJPEG) {
		int q = options->mjpeg_quantizer ? options->mjpeg_quantizer : 5;
		if (q < 2 || q > 31)
			goto error;
		ctx->encoder->flags |= AV_CODEC_FLAG_QSCALE;
		ctx->encoder->global_quality = FF_QP2LAMBDA * q;
	}
	/* MJPEG has full-range image chroma; RGB-family codecs do not use YUV
	 * conversion at all. This is actual encoder input, not a lookalike pass. */
	if (options->codec == RETRO_VIDEO_MJPEG)
		ctx->encoder->color_range = AVCOL_RANGE_JPEG;
	if (options->codec == RETRO_VIDEO_MPEG1)
		ctx->encoder->sample_aspect_ratio = (AVRational){12, 11};
	if (options->codec == RETRO_VIDEO_MPEG4_PART2) {
		ctx->encoder->profile = FF_PROFILE_MPEG4_SIMPLE;
		/* An explicit QCIF 4:3 sample aspect (12:11) in MPEG-4 VOL.
		 * Larger square-pixel modes use 1:1. */
		ctx->encoder->sample_aspect_ratio =
			options->width == 176 && options->height == 144 ? (AVRational){12, 11} : (AVRational){1, 1};
	}

	/* AVI-era codecs such as Cinepak can require frame dimensions from the
	 * container header; our in-memory round-trip has no container. */
	ctx->decoder->width = options->width;
	ctx->decoder->height = options->height;
	if (avcodec_open2(ctx->encoder, encoder, NULL) < 0 || avcodec_open2(ctx->decoder, decoder, NULL) < 0)
		goto error;

	ctx->input->width = options->width;
	ctx->input->height = options->height;
	ctx->input->format = preset->input_format;
	ctx->input->color_range = ctx->encoder->color_range;
	if (av_frame_get_buffer(ctx->input, 32) < 0)
		goto error;

	/* RGB555 is a packed, non-subsampled RGB pixel format. libswscale
	 * currently reports an unsupported full-chroma interpolation path when
	 * RGBA is sent directly to RGB555LE. Pack the 15-bit pixels ourselves,
	 * retaining the actual pixel layout required by Microsoft Video 1. */
	if (options->codec != RETRO_VIDEO_MSVIDEO1) {
		ctx->rgba_to_yuv = sws_getContext(options->width, options->height, AV_PIX_FMT_RGBA, options->width,
						  options->height, preset->input_format, SWS_BILINEAR, NULL, NULL,
						  NULL);
		if (!ctx->rgba_to_yuv)
			goto error;
	}

	return ctx;
error:
	retro_video_destroy(ctx);
	return NULL;
}

int retro_video_process(struct retro_video *ctx, const uint8_t *rgba, int input_stride, uint8_t *decoded,
			int decoded_stride)
{
	if (!ctx || !rgba || !decoded || input_stride < ctx->encoder->width * 4 ||
	    decoded_stride < ctx->encoder->width * 4)
		return -1;

	if (av_frame_make_writable(ctx->input) < 0)
		return -1;

	if (ctx->encode_pixel_format == AV_PIX_FMT_RGB555LE) {
		for (int y = 0; y < ctx->encoder->height; ++y) {
			const uint8_t *row = rgba + (size_t)y * (size_t)input_stride;
			uint8_t *target = ctx->input->data[0] + (size_t)y * (size_t)ctx->input->linesize[0];
			for (int x = 0; x < ctx->encoder->width; ++x) {
				const uint8_t *px = row + (size_t)x * 4;
				/* Round 8-bit RGB to 5 bits/channel. Little-endian packed RGB555:
				 * bits 10..14 = red, 5..9 = green, 0..4 = blue. */
				uint16_t red = ((uint16_t)px[0] * 31 + 127) / 255;
				uint16_t green = ((uint16_t)px[1] * 31 + 127) / 255;
				uint16_t blue = ((uint16_t)px[2] * 31 + 127) / 255;
				uint16_t packed = (uint16_t)((red << 10) | (green << 5) | blue);
				target[2 * x] = (uint8_t)(packed & 0xff);
				target[2 * x + 1] = (uint8_t)(packed >> 8);
			}
		}
	} else {
		const uint8_t *source[4] = {rgba, NULL, NULL, NULL};
		int strides[4] = {input_stride, 0, 0, 0};
		if (sws_scale(ctx->rgba_to_yuv, source, strides, 0, ctx->encoder->height, ctx->input->data,
			      ctx->input->linesize) != ctx->encoder->height)
			return -1;
	}

	if (ctx->encoder->codec_id == AV_CODEC_ID_MJPEG && (ctx->encoder->flags & AV_CODEC_FLAG_QSCALE))
		ctx->input->quality = ctx->encoder->global_quality;
	ctx->input->pts = ctx->pts++;
	if (avcodec_send_frame(ctx->encoder, ctx->input) < 0)
		return -1;

	int have_frame = 0;
	int code;
	while ((code = avcodec_receive_packet(ctx->encoder, ctx->packet)) == 0) {
		ctx->bytes += ctx->packet->size;
		ctx->packets++;
		if (ctx->packet->flags & AV_PKT_FLAG_KEY)
			ctx->keyframes++;

		/* The encoded MPEG-4 bitstream is modified BEFORE the decoder sees it.
		 * Skipping a later intra picture causes P pictures to use stale
		 * references; the next admitted intra picture restores decoding.
		 * Always admit the very first keyframe to establish a reference. */
        const bool is_key = (ctx->packet->flags & AV_PKT_FLAG_KEY) != 0;
        if (is_key && ctx->echo_packet) av_packet_unref(ctx->echo_packet);
        if (is_key && ctx->mosh_mode == RETRO_MOSH_PFRAME_REWIND) {
            for (int i = 0; i < ctx->rewind_depth; ++i)
                av_packet_unref(ctx->rewind_packets[i]);
            ctx->rewind_seen = 0;
        }
        if (ctx->mosh_mode == RETRO_MOSH_KEYFRAME_SKIP &&
            (ctx->packet->flags & AV_PKT_FLAG_KEY) && ctx->has_decoder_reference) {
			if (ctx->mosh_skip_keyframes > 0 &&
			    ctx->skipped_in_cycle < ctx->mosh_skip_keyframes) {
				ctx->skipped_in_cycle++;
				ctx->dropped_keyframes++;
				av_packet_unref(ctx->packet);
				continue;
			}
			ctx->skipped_in_cycle = 0;
		}
        if (is_key) ctx->has_decoder_reference = true;

        /* Store one intra packet, then reuse it on later intra boundaries.
         * The live encoder still creates all P packets normally, but the
         * decoder's reference picture now comes from an earlier instant.
         * Refresh the anchor after a bounded number of keyframes.
         * Only MPEG-4 Simple Profile generated by the same encoder is used. */
        if (is_key && ctx->mosh_mode == RETRO_MOSH_KEYFRAME_ANCHOR) {
            const bool refresh = ctx->anchor_keyframes_seen % (uint64_t)ctx->anchor_span == 0;
            ctx->anchor_keyframes_seen++;
            if (refresh) {
                av_packet_unref(ctx->anchor_packet);
                if (av_packet_ref(ctx->anchor_packet, ctx->packet) < 0) {
                    av_packet_unref(ctx->packet);
                    return -1;
                }
                ctx->anchor_refreshes++;
            } else if (ctx->anchor_packet->buf) {
                const int64_t pts = ctx->packet->pts;
                const int64_t dts = ctx->packet->dts;
                const int64_t duration = ctx->packet->duration;
                av_packet_unref(ctx->packet);
                if (av_packet_ref(ctx->packet, ctx->anchor_packet) < 0)
                    return -1;
                ctx->packet->pts = pts;
                ctx->packet->dts = dts;
                ctx->packet->duration = duration;
                ctx->anchor_replays++;
            }
        }

        if (!is_key && ctx->has_decoder_reference &&
            ctx->mosh_mode >= RETRO_MOSH_PFRAME_DROP &&
            ctx->mosh_mode <= RETRO_MOSH_PFRAME_ECHO) {
            ctx->p_seen++;
            const uint64_t interval = (uint64_t)ctx->p_interval;
            bool drop = false;
            if (ctx->mosh_mode == RETRO_MOSH_PFRAME_DROP)
                drop = ctx->p_seen % interval == 0;
            else if (ctx->mosh_mode == RETRO_MOSH_PFRAME_BURST)
                drop = (ctx->p_seen - 1) % interval < (uint64_t)ctx->p_burst;
            if (drop) {
                ctx->dropped_pframes++;
                av_packet_unref(ctx->packet);
                continue;
            }
            if (ctx->mosh_mode == RETRO_MOSH_PFRAME_ECHO) {
                if (ctx->p_seen % interval == 0 && ctx->echo_packet->buf) {
                    /* Substitute compressed P-frame motion, not raw pixels.
                     * Preserve the current slot's packet metadata; only the
                     * actual MPEG-4 payload comes from the previous P. */
                    const int64_t pts = ctx->packet->pts;
                    const int64_t dts = ctx->packet->dts;
                    const int64_t duration = ctx->packet->duration;
                    av_packet_unref(ctx->packet);
                    if (av_packet_ref(ctx->packet, ctx->echo_packet) < 0)
                        return -1;
                    ctx->packet->pts = pts;
                    ctx->packet->dts = dts;
                    ctx->packet->duration = duration;
                    ctx->replayed_pframes++;
                } else {
                    av_packet_unref(ctx->echo_packet);
                    if (av_packet_ref(ctx->echo_packet, ctx->packet) < 0)
                        return -1;
                }
            }
        }

        /* Packet Rewind keeps original P packets from this GOP in a fixed
         * ring. Each later P is substituted with the packet N P frames ago.
         * Cache the *unmodified* incoming packet, not a re-replayed packet.
         * All packet timestamps are mapped to the current output slot. */
        if (!is_key && ctx->mosh_mode == RETRO_MOSH_PFRAME_REWIND &&
            ctx->has_decoder_reference) {
            const int slot_index = (int)(ctx->rewind_seen % (uint64_t)ctx->rewind_depth);
            AVPacket *old = ctx->rewind_packets[slot_index];
            if (ctx->rewind_seen >= (uint64_t)ctx->rewind_depth && old->buf) {
                const int64_t pts = ctx->packet->pts, dts = ctx->packet->dts;
                const int64_t duration = ctx->packet->duration;
                av_packet_move_ref(ctx->rewind_incoming, ctx->packet);
                if (av_packet_ref(ctx->packet, old) < 0) {
                    av_packet_unref(ctx->rewind_incoming);
                    return -1;
                }
                ctx->packet->pts = pts;
                ctx->packet->dts = dts;
                ctx->packet->duration = duration;
                av_packet_unref(old);
                av_packet_move_ref(old, ctx->rewind_incoming);
                ctx->rewind_replays++;
            } else {
                av_packet_unref(old);
                if (av_packet_ref(old, ctx->packet) < 0) {
                    av_packet_unref(ctx->packet);
                    return -1;
                }
            }
            ctx->rewind_seen++;
        }

        /* Rounding is a one-bit, length-preserving MPEG-4 motion
         * interpolation decision, unlike variable-length motion vectors.
         * Only edit valid P-VOP headers emitted by our own encoder. */
        if (!is_key && ctx->mosh_mode == RETRO_MOSH_MOTION_SHIMMER) {
            ctx->rounding_p_seen++;
            if ((ctx->rounding_p_seen - 1) % (uint64_t)ctx->rounding_interval == 0) {
                size_t byte = 0;
                uint8_t mask = 0;
                if (locate_rounding_bit(ctx->packet->data, ctx->packet->size,
                                        ctx->encoder->framerate.num, &byte, &mask)) {
                    if (av_packet_make_writable(ctx->packet) < 0) {
                        av_packet_unref(ctx->packet);
                        return -1;
                    }
                    ctx->packet->data[byte] ^= mask;
                    ctx->rounding_flips++;
                }
            }
        }
        if (avcodec_send_packet(ctx->decoder, ctx->packet) < 0) {
			av_packet_unref(ctx->packet);
			return -1;
		}
		av_packet_unref(ctx->packet);

		while ((code = avcodec_receive_frame(ctx->decoder, ctx->decoded)) == 0) {
			ctx->yuv_to_rgba = sws_getCachedContext(ctx->yuv_to_rgba, ctx->decoded->width,
								ctx->decoded->height,
								(enum AVPixelFormat)ctx->decoded->format,
								ctx->decoded->width, ctx->decoded->height,
								AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
			if (!ctx->yuv_to_rgba) {
				av_frame_unref(ctx->decoded);
				return -1;
			}
			uint8_t *target[4] = {decoded, NULL, NULL, NULL};
			int target_stride[4] = {decoded_stride, 0, 0, 0};
			if (sws_scale(ctx->yuv_to_rgba, (const uint8_t *const *)ctx->decoded->data,
				      ctx->decoded->linesize, 0, ctx->decoded->height, target,
				      target_stride) != ctx->decoded->height) {
				av_frame_unref(ctx->decoded);
				return -1;
			}
            if (ctx->mosh_mode == RETRO_MOSH_FRAME_PEEL) {
                const int w = ctx->decoded->width;
                const int h = ctx->decoded->height;
                const size_t row_bytes = (size_t)w * 4;
                const size_t frame_bytes = row_bytes * (size_t)h;
                const int newest = ctx->slice_next;
                uint8_t *storage = ctx->slice_frames + (size_t)newest * frame_bytes;
                /* Save the clean decoded picture first so feedback cannot
                 * recursively accumulate accidental band artifacts. */
                for (int y = 0; y < h; ++y)
                    memcpy(storage + (size_t)y * row_bytes,
                           decoded + (size_t)y * decoded_stride, row_bytes);
                if (ctx->slice_count < ctx->slice_history) ctx->slice_count++;
                for (int y = 0; y < h; ++y) {
                    const int age = (y / ctx->slice_height) % ctx->slice_history;
                    if (age == 0 || age >= ctx->slice_count) continue;
                    const int past = (newest + ctx->slice_history - age) % ctx->slice_history;
                    const uint8_t *src = ctx->slice_frames +
                        (size_t)past * frame_bytes + (size_t)y * row_bytes;
                    memcpy(decoded + (size_t)y * decoded_stride, src, row_bytes);
                    ctx->slice_reused_rows++;
                }
                ctx->slice_next = (newest + 1) % ctx->slice_history;
            }
            if (ctx->mosh_mode == RETRO_MOSH_VECTOR_DRIFT) {
                const int width = ctx->decoded->width;
                const int height = ctx->decoded->height;
                const AVFrameSideData *side = av_frame_get_side_data(ctx->decoded, AV_FRAME_DATA_MOTION_VECTORS);
                const size_t count = side ? side->size / sizeof(AVMotionVector) : 0;
                ctx->motion_vectors_seen += count;
                if (ctx->flow_has_previous && ctx->flow_strength && ctx->flow_persistence && side) {
                    const AVMotionVector *vectors = (const AVMotionVector *)side->data;
                    for (size_t k = 0; k < count; ++k) {
                        const AVMotionVector *v = &vectors[k];
                        if (!v->motion_scale || v->source >= 0 || !v->w || !v->h)
                            continue;
                        /* AVMotionVector uses src = dst + motion / motion_scale.
                         * Limit the displacement to one image dimension, and
                         * use the unmodified previous frame as a stable source. */
                        int64_t dx64 = (int64_t)v->motion_x * ctx->flow_strength /
                                       ((int64_t)v->motion_scale * 100);
                        int64_t dy64 = (int64_t)v->motion_y * ctx->flow_strength /
                                       ((int64_t)v->motion_scale * 100);
                        if (dx64 > width) dx64 = width;
                        if (dx64 < -width) dx64 = -width;
                        if (dy64 > height) dy64 = height;
                        if (dy64 < -height) dy64 = -height;
                        int dx = (int)dx64, dy = (int)dy64;
                        if (!dx && !dy) continue;
                        int start_x = (int)v->dst_x - v->w / 2;
                        int start_y = (int)v->dst_y - v->h / 2;
                        int end_x = start_x + v->w;
                        int end_y = start_y + v->h;
                        if (start_x < 0) start_x = 0;
                        if (start_y < 0) start_y = 0;
                        if (end_x > width) end_x = width;
                        if (end_y > height) end_y = height;
                        if (start_x >= end_x || start_y >= end_y) continue;
                        for (int y = start_y; y < end_y; ++y) {
                            int sample_y = y + dy;
                            if (sample_y < 0) sample_y = 0;
                            if (sample_y >= height) sample_y = height - 1;
                            for (int x = start_x; x < end_x; ++x) {
                                int sample_x = x + dx;
                                if (sample_x < 0) sample_x = 0;
                                if (sample_x >= width) sample_x = width - 1;
                                uint8_t *out = decoded + (size_t)y * decoded_stride + (size_t)x * 4;
                                const uint8_t *previous = ctx->flow_previous +
                                    ((size_t)sample_y * width + sample_x) * 4;
                                for (int c = 0; c < 3; ++c)
                                    out[c] = (uint8_t)((out[c] * (100 - ctx->flow_persistence) +
                                        previous[c] * ctx->flow_persistence + 50) / 100);
                            }
                        }
                        ctx->motion_blocks_warped++;
                    }
                }
                for (int y = 0; y < height; ++y)
                    memcpy(ctx->flow_previous + (size_t)y * width * 4,
                           decoded + (size_t)y * decoded_stride, (size_t)width * 4);
                ctx->flow_has_previous = true;
            }
            av_frame_unref(ctx->decoded);
            have_frame = 1;
		}
		if (code != AVERROR(EAGAIN) && code != AVERROR_EOF)
			return -1;
	}

	return code == AVERROR(EAGAIN) || code == AVERROR_EOF ? have_frame : -1;
}

uint64_t retro_video_encoded_bytes(const struct retro_video *ctx)
{
	return ctx ? ctx->bytes : 0;
}

uint64_t retro_video_encoded_packets(const struct retro_video *ctx)
{
	return ctx ? ctx->packets : 0;
}

uint64_t retro_video_encoded_keyframes(const struct retro_video *ctx)
{
	return ctx ? ctx->keyframes : 0;
}

int retro_video_decoded_profile(const struct retro_video *video)
{
	return video ? video->decoder->profile : -1;
}

int retro_video_decoded_sar_num(const struct retro_video *video)
{
	return video ? video->decoder->sample_aspect_ratio.num : 0;
}

int retro_video_decoded_sar_den(const struct retro_video *video)
{
	return video ? video->decoder->sample_aspect_ratio.den : 0;
}

uint64_t retro_video_dropped_keyframes(const struct retro_video *ctx)
{
	return ctx ? ctx->dropped_keyframes : 0;
}

uint64_t retro_video_dropped_pframes(const struct retro_video *ctx)
{
    return ctx ? ctx->dropped_pframes : 0;
}

uint64_t retro_video_replayed_pframes(const struct retro_video *ctx)
{
    return ctx ? ctx->replayed_pframes : 0;
}

uint64_t retro_video_motion_vectors_seen(const struct retro_video *ctx)
{
    return ctx ? ctx->motion_vectors_seen : 0;
}

uint64_t retro_video_motion_blocks_warped(const struct retro_video *ctx)
{
    return ctx ? ctx->motion_blocks_warped : 0;
}


uint64_t retro_video_motion_rounding_flips(const struct retro_video *ctx)
{
    return ctx ? ctx->rounding_flips : 0;
}


uint64_t retro_video_anchored_keyframes(const struct retro_video *ctx)
{
    return ctx ? ctx->anchor_replays : 0;
}

uint64_t retro_video_anchor_refreshes(const struct retro_video *ctx)
{
    return ctx ? ctx->anchor_refreshes : 0;
}


uint64_t retro_video_rewound_pframes(const struct retro_video *ctx)
{
    return ctx ? ctx->rewind_replays : 0;
}

uint64_t retro_video_temporal_rows_reused(const struct retro_video *ctx)
{
    return ctx ? ctx->slice_reused_rows : 0;
}
