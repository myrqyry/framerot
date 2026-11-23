/*
 * Derived in part from OBS Retro Effects (FiniteSingularity), through the
 * myrqyry/obs-retro-effects fork. Substantially modified for framerot in 2026.
 * GNU GPL version 2. See COPYING and README.md for attribution.
 */
#include "video-roundtrip.h"
#include "mpeg4-edit.h"
#include "packet-overwrite.h"

#include "ffmpeg-compat.h"
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/motion_vector.h>
#include <libswscale/swscale.h>

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

int framerot_packet_overwrite(AVPacket *packet,const uint8_t *bytes,size_t size)
{
    if (!packet || !packet->data || packet->size<0 || !bytes ||
        !size || size>(size_t)INT_MAX)
        return -1;
    /* av_packet_make_writable performs COW if Anchor/Echo/Rewind retained
     * a reference. For a unique packet with enough space, no new FFmpeg
     * payload allocation or AVPacket metadata copy is necessary. */
    if (av_packet_make_writable(packet)<0)
        return -1;
    if (size>(size_t)packet->size) {
        if (av_grow_packet(packet,(int)(size-(size_t)packet->size))<0)
            return -1;
    } else if (size<(size_t)packet->size) {
        av_shrink_packet(packet,(int)size);
    }
    memcpy(packet->data,bytes,size);
    return 0;
}

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
	int64_t pts;
	uint64_t bytes;
	uint64_t packets;
	uint64_t keyframes;
	uint64_t dropped_keyframes;
    uint64_t dropped_pframes;
    uint64_t replayed_pframes;
    uint64_t p_seen;
    enum retro_reference reference;
    enum retro_predictive predictive;
    unsigned layers;
    bool hold, release_anchor;
    struct mpeg4_edit_options editor;
    struct mpeg4_edit_buffer edit_scratch; /* codec-thread owned, reused P-VOP output */
    uint64_t edited_packets,edited_vectors,edited_coefficients,edited_blocks,edited_phantom_blocks,edit_unsupported;
    uint64_t edited_frequency_coefficients[3];
    int p_interval;
    int p_burst;
	int mosh_skip_keyframes;
	int skipped_in_cycle;
	bool has_decoder_reference;
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

struct retro_video_options retro_video_default_options(enum retro_video_codec codec)
{
    if(codec!=RETRO_VIDEO_MPEG4_PART2)return (struct retro_video_options){0};
    return (struct retro_video_options){.codec=codec,.width=176,.height=144,
        .fps=10,.bitrate=64000,.gop_size=30,.mosh_mv_x=100,.mosh_mv_y=100,.mosh_residual=100};
}

/* SplitMix64: local deterministic state, no process-global rand() or
 * shared mutable PRNG. This is creative randomness, not cryptography. */
static unsigned roll(uint64_t *seed,unsigned count)
{
    uint64_t z=(*seed+=UINT64_C(0x9e3779b97f4a7c15));
    z=(z^(z>>30))*UINT64_C(0xbf58476d1ce4e5b9);
    z=(z^(z>>27))*UINT64_C(0x94d049bb133111eb);
    return (unsigned)((z^(z>>31))%count);
}
static int operation_family(unsigned op)
{
    return op<2?0:op<6?1:2;
}
static void choose_operation(struct retro_video_options *o,unsigned op)
{
    if(op<2) o->mosh_reference=RETRO_REFERENCE_SKIP+(int)op;
    else if(op<6) o->mosh_predictive=RETRO_PREDICTIVE_DROP+(int)(op-2);
    else o->mosh_layers|=1u<<(op-6);
}
struct retro_video_options retro_video_random_instrument(uint64_t seed)
{
    struct retro_video_options o=retro_video_default_options(RETRO_VIDEO_MPEG4_PART2);
    o.mosh_mode=RETRO_MOSH_INSTRUMENT;
    /* One or two non-conflicting operations, never a completely clean roll. */
    unsigned primary=roll(&seed,10);
    choose_operation(&o,primary);
    if(roll(&seed,4)!=0) {
        unsigned choices[9],n=0;
        for(unsigned op=0;op<10;op++)
            if(op!=primary && (operation_family(primary)==2 ||
                operation_family(op)!=operation_family(primary))) choices[n++]=op;
        choose_operation(&o,choices[roll(&seed,n)]);
    }
    static const int heights[]={144,192,288},widths[]={256,336,512};
    static const int rates[]={12,15,18,24},gops[]={12,16,20,24};
    unsigned size=roll(&seed,3);
    o.height=heights[size];o.width=widths[size];
    o.fps=rates[roll(&seed,4)];o.gop_size=gops[roll(&seed,4)];
    o.bitrate=(160+10*(int)roll(&seed,50))*1000;
    o.mosh_skip_keyframes=1+(int)roll(&seed,2);
    o.mosh_anchor_span=2+(int)roll(&seed,3);
    o.mosh_p_interval=4+(int)roll(&seed,9);
    o.mosh_p_burst=1+(int)roll(&seed,(unsigned)(o.mosh_p_interval/2));
    o.mosh_rewind_depth=2+(int)roll(&seed,5);
    o.mosh_rounding_interval=1+(int)roll(&seed,6);
    o.mosh_mv_x=-150+5*(int)roll(&seed,81);
    o.mosh_mv_y=-150+5*(int)roll(&seed,81);
    o.mosh_residual=30+5*(int)roll(&seed,17);
    o.mosh_residual_loss=(int)roll(&seed,31);
    o.mosh_vector_strength=80+10*(int)roll(&seed,18);
    o.mosh_vector_persistence=25+5*(int)roll(&seed,11);
    o.mosh_slice_history=2+(int)roll(&seed,5);
    o.mosh_slice_height=4+(int)roll(&seed,21);
    return o;
}

bool retro_video_supported_size(enum retro_video_codec codec,int width,int height)
{
    return codec==RETRO_VIDEO_MPEG4_PART2 && width>=16 && height>=16 &&
        width<=2048 && height<=2048 && width%16==0 && height%16==0;
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
    mpeg4_edit_buffer_destroy(&ctx->edit_scratch);
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
        options->mosh_mode < RETRO_MOSH_KEYFRAME_SKIP || options->mosh_mode > RETRO_MOSH_INSTRUMENT)
        return NULL;
    enum retro_reference reference=RETRO_REFERENCE_LIVE;
    enum retro_predictive predictive=RETRO_PREDICTIVE_LIVE;
    unsigned layers=0;
    switch (options->mosh_mode) {
        case RETRO_MOSH_KEYFRAME_SKIP: reference=RETRO_REFERENCE_SKIP; break;
        case RETRO_MOSH_PFRAME_DROP: predictive=RETRO_PREDICTIVE_DROP; break;
        case RETRO_MOSH_PFRAME_BURST: predictive=RETRO_PREDICTIVE_BURST; break;
        case RETRO_MOSH_PFRAME_ECHO: predictive=RETRO_PREDICTIVE_ECHO; break;
        case RETRO_MOSH_PFRAME_REWIND: predictive=RETRO_PREDICTIVE_REWIND; break;
        case RETRO_MOSH_KEYFRAME_ANCHOR: reference=RETRO_REFERENCE_ANCHOR; break;
        case RETRO_MOSH_VECTOR_DRIFT: layers=RETRO_LAYER_FLOW; break;
        case RETRO_MOSH_FRAME_PEEL: layers=RETRO_LAYER_PEEL; break;
        case RETRO_MOSH_MOTION_SHIMMER: layers=RETRO_LAYER_SHIMMER; break;
        case RETRO_MOSH_INSTRUMENT: reference=options->mosh_reference; predictive=options->mosh_predictive;
            layers=options->mosh_layers; break;
    }
    if ((layers&RETRO_LAYER_COMPRESSED) &&
        (options->mosh_frequency[0]< -100 || options->mosh_frequency[0]>200 ||
         options->mosh_frequency[1]< -100 || options->mosh_frequency[1]>200 ||
         options->mosh_frequency[2]< -100 || options->mosh_frequency[2]>200))
        return NULL;
    if (reference<RETRO_REFERENCE_LIVE || reference>RETRO_REFERENCE_ANCHOR ||
        predictive<RETRO_PREDICTIVE_LIVE || predictive>RETRO_PREDICTIVE_REWIND ||
        (layers & ~(RETRO_LAYER_FLOW|RETRO_LAYER_PEEL|RETRO_LAYER_SHIMMER|RETRO_LAYER_COMPRESSED)) ||
        ((layers&RETRO_LAYER_COMPRESSED) &&
         (options->mosh_mv_x< -500 || options->mosh_mv_x>500 ||
          options->mosh_mv_y< -500 || options->mosh_mv_y>500 ||
          options->mosh_residual< -200 || options->mosh_residual>200 ||
          options->mosh_residual_loss<0 || options->mosh_residual_loss>100 ||
         options->mosh_phantom<0 || options->mosh_phantom>100)) ||
        options->mosh_skip_keyframes < 0 || options->mosh_skip_keyframes > 16 ||
        (predictive>=RETRO_PREDICTIVE_DROP && predictive<=RETRO_PREDICTIVE_ECHO &&
         (options->mosh_p_interval<2 || options->mosh_p_interval>60)) ||
        (predictive==RETRO_PREDICTIVE_BURST && (options->mosh_p_burst<1 || options->mosh_p_burst>=options->mosh_p_interval)) ||
        options->mosh_vector_strength<0 || options->mosh_vector_strength>500 ||
        options->mosh_vector_persistence<0 || options->mosh_vector_persistence>100 ||
        ((layers&RETRO_LAYER_SHIMMER) && (options->mosh_rounding_interval<1 || options->mosh_rounding_interval>60)) ||
        (reference==RETRO_REFERENCE_ANCHOR && (options->mosh_anchor_span<2 || options->mosh_anchor_span>12)) ||
        (predictive==RETRO_PREDICTIVE_REWIND && (options->mosh_rewind_depth<2 || options->mosh_rewind_depth>12)) ||
        ((layers&RETRO_LAYER_PEEL) && (options->mosh_slice_history<2 || options->mosh_slice_history>8 ||
         options->mosh_slice_height<1 || options->mosh_slice_height>64))) {
        return NULL;
    }

    const AVCodec *encoder=avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    const AVCodec *decoder=avcodec_find_decoder(AV_CODEC_ID_MPEG4);
    if(!decoder||!retro_encoder_supports_format(encoder,AV_PIX_FMT_YUV420P))return NULL;

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
    ctx->reference=reference; ctx->predictive=predictive; ctx->layers=layers;
    ctx->editor=(struct mpeg4_edit_options){
        .width=options->width,.height=options->height,.fps=options->fps,
        .x_scale=options->mosh_mv_x,.y_scale=options->mosh_mv_y,
        .residual_scale=options->mosh_residual,.residual_loss=options->mosh_residual_loss,
        .seed=0,.phantom_percent=options->mosh_phantom,
        .frequency={options->mosh_frequency[0],options->mosh_frequency[1],
                    options->mosh_frequency[2]},
    };
    ctx->p_interval = options->mosh_p_interval;
    ctx->p_burst = options->mosh_p_burst;
    ctx->rounding_interval = options->mosh_rounding_interval;
    ctx->anchor_span = options->mosh_anchor_span;
    ctx->rewind_depth = options->mosh_rewind_depth;
    ctx->slice_history = options->mosh_slice_history;
    ctx->slice_height = options->mosh_slice_height;
    if (ctx->predictive == RETRO_PREDICTIVE_REWIND) {
        ctx->rewind_incoming = av_packet_alloc();
        if (!ctx->rewind_incoming) goto error;
        for (int i = 0; i < ctx->rewind_depth; ++i) {
            ctx->rewind_packets[i] = av_packet_alloc();
            if (!ctx->rewind_packets[i]) goto error;
        }
    }
    if ((ctx->layers & RETRO_LAYER_PEEL)) {
        const size_t bytes = (size_t)options->width * (size_t)options->height * 4;
        /* A generic high-resolution API caller must not create a huge
         * unbounded feedback allocation; OBS's codec is only 144px high. */
        if (bytes > (32u * 1024u * 1024u) / (size_t)ctx->slice_history)
            goto error;
        ctx->slice_frames = calloc(ctx->slice_history, bytes);
        if (!ctx->slice_frames) goto error;
    }
    if (options->codec == RETRO_VIDEO_MPEG4_PART2) {
        ctx->anchor_packet = av_packet_alloc();
        if (!ctx->anchor_packet) goto error;
    }
    ctx->flow_strength = options->mosh_vector_strength;
    ctx->flow_persistence = options->mosh_vector_persistence;
    if ((ctx->layers & RETRO_LAYER_FLOW)) {
        ctx->flow_previous = calloc((size_t)options->width * options->height * 4, 1);
        if (!ctx->flow_previous) goto error;
        ctx->decoder->flags2 |= AV_CODEC_FLAG2_EXPORT_MVS;
    }
    if (ctx->predictive == RETRO_PREDICTIVE_ECHO) {
        ctx->echo_packet = av_packet_alloc();
        if (!ctx->echo_packet) goto error;
    }
	ctx->encoder->width = options->width;
	ctx->encoder->height = options->height;
	ctx->encoder->pix_fmt = AV_PIX_FMT_YUV420P;
	ctx->encoder->time_base = (AVRational){1, options->fps};
	ctx->encoder->framerate = (AVRational){options->fps, 1};
	ctx->encoder->bit_rate = options->bitrate;
	ctx->encoder->gop_size = options->gop_size;
	ctx->encoder->max_b_frames = 0;
    ctx->encoder->profile=RETRO_MPEG4_SIMPLE_PROFILE;
    ctx->encoder->sample_aspect_ratio=options->width==176 && options->height==144 ?
        (AVRational){12,11}:(AVRational){1,1};
	ctx->decoder->width = options->width;
	ctx->decoder->height = options->height;
	if (avcodec_open2(ctx->encoder, encoder, NULL) < 0 || avcodec_open2(ctx->decoder, decoder, NULL) < 0)
		goto error;

	ctx->input->width = options->width;
	ctx->input->height = options->height;
	ctx->input->format = AV_PIX_FMT_YUV420P;
	ctx->input->color_range = ctx->encoder->color_range;
	if (av_frame_get_buffer(ctx->input, 32) < 0)
		goto error;

    ctx->rgba_to_yuv=sws_getContext(options->width,options->height,AV_PIX_FMT_RGBA,
        options->width,options->height,AV_PIX_FMT_YUV420P,SWS_BILINEAR,NULL,NULL,NULL);
    if(!ctx->rgba_to_yuv)goto error;

	return ctx;
error:
	retro_video_destroy(ctx);
	return NULL;
}

bool retro_video_set_live_options(struct retro_video *ctx,const struct retro_video_options *o)
{
    if(!ctx||!o||o->mosh_skip_keyframes<0||o->mosh_skip_keyframes>16||
       (ctx->predictive>=RETRO_PREDICTIVE_DROP&&ctx->predictive<=RETRO_PREDICTIVE_ECHO&&
        (o->mosh_p_interval<2||o->mosh_p_interval>60))||
       (ctx->predictive==RETRO_PREDICTIVE_BURST&&(o->mosh_p_burst<1||o->mosh_p_burst>=o->mosh_p_interval))||
       (ctx->reference==RETRO_REFERENCE_ANCHOR&&(o->mosh_anchor_span<2||o->mosh_anchor_span>12))||
       (ctx->layers&RETRO_LAYER_SHIMMER&&(o->mosh_rounding_interval<1||o->mosh_rounding_interval>60))||
       (ctx->layers&RETRO_LAYER_PEEL&&(o->mosh_slice_height<1||o->mosh_slice_height>64))||
       o->mosh_vector_strength<0||o->mosh_vector_strength>500||
       o->mosh_vector_persistence<0||o->mosh_vector_persistence>100||
       (ctx->layers&RETRO_LAYER_COMPRESSED&&(o->mosh_mv_x< -500||o->mosh_mv_x>500||
        o->mosh_mv_y< -500||o->mosh_mv_y>500||o->mosh_residual< -200||o->mosh_residual>200||
        o->mosh_residual_loss<0||o->mosh_residual_loss>100||
        o->mosh_phantom<0||o->mosh_phantom>100||
        o->mosh_frequency[0]< -100||o->mosh_frequency[0]>200||
        o->mosh_frequency[1]< -100||o->mosh_frequency[1]>200||
        o->mosh_frequency[2]< -100||o->mosh_frequency[2]>200)))return false;
    ctx->mosh_skip_keyframes=o->mosh_skip_keyframes;ctx->p_interval=o->mosh_p_interval;
    ctx->p_burst=o->mosh_p_burst;ctx->anchor_span=o->mosh_anchor_span;
    ctx->rounding_interval=o->mosh_rounding_interval;ctx->slice_height=o->mosh_slice_height;
    ctx->flow_strength=o->mosh_vector_strength;ctx->flow_persistence=o->mosh_vector_persistence;
    ctx->editor.x_scale=o->mosh_mv_x;ctx->editor.y_scale=o->mosh_mv_y;
    ctx->editor.residual_scale=o->mosh_residual;ctx->editor.residual_loss=o->mosh_residual_loss;
    ctx->editor.phantom_percent=o->mosh_phantom;
    for(int band=0;band<3;band++)ctx->editor.frequency[band]=o->mosh_frequency[band];
    return true;
}

void retro_video_set_anchor_hold(struct retro_video *ctx, bool hold)
{
    if (!ctx || !ctx->anchor_packet) return;
    if (ctx->hold && !hold) ctx->release_anchor=true;
    if (hold) ctx->release_anchor=false;
    ctx->hold=hold;
}

int retro_video_process(struct retro_video *ctx, const uint8_t *rgba, int input_stride, uint8_t *decoded,
			int decoded_stride)
{
	if (!ctx || !rgba || !decoded || input_stride < ctx->encoder->width * 4 ||
	    decoded_stride < ctx->encoder->width * 4)
		return -1;

	if (av_frame_make_writable(ctx->input) < 0)
		return -1;

    const uint8_t *source[4]={rgba,NULL,NULL,NULL};
    int strides[4]={input_stride,0,0,0};
    if(sws_scale(ctx->rgba_to_yuv,source,strides,0,ctx->encoder->height,
                 ctx->input->data,ctx->input->linesize)!=ctx->encoder->height)return -1;
	ctx->input->pts = ctx->pts++;
    ctx->input->pict_type = ctx->release_anchor ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
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
        if (is_key && ctx->predictive == RETRO_PREDICTIVE_REWIND) {
            for (int i = 0; i < ctx->rewind_depth; ++i)
                av_packet_unref(ctx->rewind_packets[i]);
            ctx->rewind_seen = 0;
        }
        if (ctx->reference == RETRO_REFERENCE_SKIP && !ctx->hold && !ctx->release_anchor &&
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
        if (is_key && ctx->anchor_packet) {
            const bool scheduled = ctx->reference == RETRO_REFERENCE_ANCHOR;
            const bool refresh = !ctx->anchor_packet->buf || ctx->release_anchor ||
                (!ctx->hold && (!scheduled || ctx->anchor_keyframes_seen % (uint64_t)ctx->anchor_span == 0));
            ctx->release_anchor=false;
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
            ctx->predictive >= RETRO_PREDICTIVE_DROP &&
            ctx->predictive <= RETRO_PREDICTIVE_ECHO) {
            ctx->p_seen++;
            const uint64_t interval = (uint64_t)ctx->p_interval;
            bool drop = false;
            if (ctx->predictive == RETRO_PREDICTIVE_DROP)
                drop = ctx->p_seen % interval == 0;
            else if (ctx->predictive == RETRO_PREDICTIVE_BURST)
                drop = (ctx->p_seen - 1) % interval < (uint64_t)ctx->p_burst;
            if (drop) {
                ctx->dropped_pframes++;
                av_packet_unref(ctx->packet);
                continue;
            }
            if (ctx->predictive == RETRO_PREDICTIVE_ECHO) {
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
        if (!is_key && ctx->predictive == RETRO_PREDICTIVE_REWIND &&
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
        if (!is_key && (ctx->layers & RETRO_LAYER_SHIMMER)) {
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
        if (!is_key && (ctx->layers&RETRO_LAYER_COMPRESSED)) {
            uint8_t *bytes=NULL;size_t size=0;struct mpeg4_edit_stats stats;
            ctx->editor.seed=(uint32_t)ctx->packets;
            int edit=mpeg4_edit_p_vop_reuse(ctx->packet->data,(size_t)ctx->packet->size,
                &ctx->editor,&ctx->edit_scratch,&bytes,&size,&stats);
            if(edit<0)ctx->edit_unsupported++;
            if(edit==1) {
                /* Editor scratch remains codec-thread owned. Reuse the
                 * encoder packet itself, preserving PTS, flags and side data. */
                if(framerot_packet_overwrite(ctx->packet,bytes,size)<0) {
                    av_packet_unref(ctx->packet);
                    return -1;
                }
                ctx->edited_packets++;ctx->edited_vectors+=stats.vectors;
                ctx->edited_coefficients+=stats.coefficients;ctx->edited_blocks+=stats.blocks;
                ctx->edited_phantom_blocks+=stats.phantom_blocks;
                for(int band=0;band<3;band++)
                    ctx->edited_frequency_coefficients[band]+=stats.frequency_coefficients[band];
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
            if ((ctx->layers & RETRO_LAYER_PEEL)) {
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
            if ((ctx->layers & RETRO_LAYER_FLOW)) {
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

uint64_t retro_video_compressed_packets(const struct retro_video *s){return s?s->edited_packets:0;}
uint64_t retro_video_compressed_vectors(const struct retro_video *s){return s?s->edited_vectors:0;}
uint64_t retro_video_compressed_coefficients(const struct retro_video *s){return s?s->edited_coefficients:0;}
uint64_t retro_video_compressed_blocks(const struct retro_video *s){return s?s->edited_blocks:0;}
uint64_t retro_video_compressed_phantom_blocks(const struct retro_video *s){return s?s->edited_phantom_blocks:0;}
uint64_t retro_video_compressed_frequency_coefficients(const struct retro_video *s,int band)
{
    return s && band>=0 && band<3 ? s->edited_frequency_coefficients[band] : 0;
}
uint64_t retro_video_compressed_unsupported(const struct retro_video *s){return s?s->edit_unsupported:0;}
