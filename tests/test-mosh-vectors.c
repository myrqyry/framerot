#include "../src/video-roundtrip.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void scrolling_pattern(uint8_t *frame, int width, int height, int t)
{
    const int offset = 4 * t;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const int source_x = (x + width - offset % width) % width;
            const int stripe = ((source_x / 16) ^ (y / 16)) & 1;
            const size_t p = ((size_t)y * width + x) * 4;
            frame[p] = stripe ? 240 : 24;
            frame[p + 1] = (uint8_t)(source_x * 3);
            frame[p + 2] = stripe ? 38 : 205;
            frame[p + 3] = 255;
        }
}

static void run_test(int strength, int persistence)
{
    struct retro_video_options opt = retro_video_default_options(RETRO_VIDEO_MPEG4_PART2);
    opt.width = 256;
    opt.height = 144;
    opt.fps = 15;
    opt.bitrate = 230000;
    opt.gop_size = 24;
    opt.mosh_mode = RETRO_MOSH_VECTOR_DRIFT;
    opt.mosh_vector_strength = strength;
    opt.mosh_vector_persistence = persistence;
    struct retro_video *flow = retro_video_create(&opt);
    assert(flow);
    opt.mosh_mode = RETRO_MOSH_KEYFRAME_SKIP;
    opt.mosh_vector_strength = 0;
    opt.mosh_vector_persistence = 0;
    struct retro_video *clean = retro_video_create(&opt);
    assert(clean);

    const size_t bytes = (size_t)opt.width * opt.height * 4;
    uint8_t *source = malloc(bytes), *raw = malloc(bytes), *warped = malloc(bytes);
    assert(source && raw && warped);
    int frames = 0, comparisons = 0, modified = 0;
    for (int i = 0; i < 110; ++i) {
        scrolling_pattern(source, opt.width, opt.height, i);
        int a = retro_video_process(flow, source, opt.width * 4, warped, opt.width * 4);
        int b = retro_video_process(clean, source, opt.width * 4, raw, opt.width * 4);
        assert(a >= 0 && b >= 0);
        frames += a == 1;
        if (a == 1 && b == 1) {
            comparisons++;
            modified += memcmp(warped, raw, bytes) != 0;
            for (size_t pos = 3; pos < bytes; pos += 4) assert(warped[pos] == 255);
        }
    }
    printf("VECTOR_DRIFT strength=%d persistence=%d frames=%d compared=%d modified=%d vectors=%llu blocks=%llu\n",
           strength, persistence, frames, comparisons, modified,
           (unsigned long long)retro_video_motion_vectors_seen(flow),
           (unsigned long long)retro_video_motion_blocks_warped(flow));
    assert(frames > 80);
    assert(comparisons > 80);
    assert(retro_video_motion_vectors_seen(flow) > 100);
    if (strength > 0 && persistence > 0) {
        assert(modified > 30);
        assert(retro_video_motion_blocks_warped(flow) > 50);
    } else {
        assert(modified == 0);
        assert(retro_video_motion_blocks_warped(flow) == 0);
    }
    free(source); free(raw); free(warped);
    retro_video_destroy(clean);
    retro_video_destroy(flow);
}

int main(void)
{
    struct retro_video_options opt = retro_video_default_options(RETRO_VIDEO_MPEG4_PART2);
    opt.mosh_mode = RETRO_MOSH_VECTOR_DRIFT;
    opt.mosh_vector_strength = 250;
    opt.mosh_vector_persistence = 65;
    opt.codec = (enum retro_video_codec)1;
    assert(retro_video_create(&opt) == NULL);
    opt.codec = RETRO_VIDEO_MPEG4_PART2;
    opt.mosh_vector_strength = 501;
    assert(retro_video_create(&opt) == NULL);
    opt.mosh_vector_strength = 250;
    opt.mosh_vector_persistence = 101;
    assert(retro_video_create(&opt) == NULL);
    run_test(250, 65);
    run_test(0, 65); /* No offset means exact ordinary decode. */
    run_test(250, 0); /* No history blend means exact ordinary decode. */
    return 0;
}
