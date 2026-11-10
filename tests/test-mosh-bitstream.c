#include "../src/video-roundtrip.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void motion_scene(uint8_t *dst, int w, int h, int t)
{
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            size_t j = ((size_t)y * w + x) * 4;
            int offset = (t * 7) % w;
            int tile = ((x + offset) / 9 + (y + t * 3) / 13) & 1;
            dst[j] = (uint8_t)(tile ? 244 : 12);
            dst[j + 1] = (uint8_t)((x * 5 + t * 3) & 255);
            dst[j + 2] = (uint8_t)(tile ? 34 : 193);
            dst[j + 3] = 255;
        }
    }
}

static void compare(int fps, int cadence)
{
    struct retro_video_options opt = retro_video_default_options(RETRO_VIDEO_MPEG4_PART2);
    opt.width = 256;
    opt.height = 144;
    opt.bitrate = 200000;
    opt.fps = fps;
    opt.gop_size = 23;
    opt.mosh_mode = RETRO_MOSH_MOTION_SHIMMER;
    opt.mosh_rounding_interval = cadence;
    struct retro_video *altered = retro_video_create(&opt);
    assert(altered);
    opt.mosh_mode = RETRO_MOSH_KEYFRAME_SKIP;
    opt.mosh_rounding_interval = 0;
    struct retro_video *clean = retro_video_create(&opt);
    assert(clean);

    size_t bytes = (size_t)opt.width * opt.height * 4;
    uint8_t *src = malloc(bytes), *a = malloc(bytes), *b = malloc(bytes);
    assert(src && a && b);
    int compared = 0, changed = 0, frames = 0;
    for (int i = 0; i < 138; ++i) {
        motion_scene(src, opt.width, opt.height, i);
        int x = retro_video_process(altered, src, opt.width * 4, a, opt.width * 4);
        int y = retro_video_process(clean, src, opt.width * 4, b, opt.width * 4);
        assert(x >= 0 && y >= 0);
        if (x == 1) ++frames;
        assert(x == y);
        if (x == 1 && y == 1) {
            ++compared;
            changed += memcmp(a, b, bytes) != 0;
            for (size_t j = 3; j < bytes; j += 4) assert(a[j] == 255);
        }
    }
    unsigned long long flips = (unsigned long long)retro_video_motion_rounding_flips(altered);
    printf("SHIMMER fps=%d every=%d frames=%d compared=%d changed=%d modified_headers=%llu\n",
           fps, cadence, frames, compared, changed, flips);
    assert(frames >= 125);
    assert(compared >= 125);
    assert(changed >= 20);
    assert(flips >= 20);
    assert(flips < 138);
    assert(retro_video_motion_rounding_flips(clean) == 0);
    assert(retro_video_dropped_pframes(altered) == 0);
    free(src); free(a); free(b);
    retro_video_destroy(altered);
    retro_video_destroy(clean);
}

int main(void)
{
    struct retro_video_options o = retro_video_default_options(RETRO_VIDEO_MPEG4_PART2);
    o.mosh_mode = RETRO_MOSH_MOTION_SHIMMER;
    o.mosh_rounding_interval = 0;
    assert(retro_video_create(&o) == NULL);
    o.mosh_rounding_interval = 61;
    assert(retro_video_create(&o) == NULL);
    o.mosh_rounding_interval = 2;
    o.codec = RETRO_VIDEO_H263;
    assert(retro_video_create(&o) == NULL);
    compare(12, 1);
    compare(15, 2);
    compare(30, 3);
    return 0;
}
