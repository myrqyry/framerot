#include "../src/video-roundtrip.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void scene(uint8_t *pixels, int width, int height, int tick)
{
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const int px = (x + tick * 5) % width;
            const int py = (y + tick * 3) % height;
            const int tile = (((px / 9) ^ (py / 11) ^ (tick / 14)) & 1);
            size_t i = ((size_t)y * width + x) * 4;
            pixels[i] = tile ? (uint8_t)(230 - tick / 3) : (uint8_t)(15 + tick / 4);
            pixels[i + 1] = (uint8_t)((x * 2 + y + tick * 7) & 255);
            pixels[i + 2] = tile ? 30 : (uint8_t)(195 - tick / 5);
            pixels[i + 3] = 255;
        }
}

static void exercise(enum retro_mosh_mode mode, int history, int bands)
{
    struct retro_video_options opt = retro_video_default_options(RETRO_VIDEO_MPEG4_PART2);
    opt.width = 256;
    opt.height = 144;
    opt.fps = 15;
    opt.gop_size = 23;
    opt.bitrate = 240000;
    opt.mosh_mode = mode;
    opt.mosh_rewind_depth = history;
    opt.mosh_slice_history = history;
    opt.mosh_slice_height = bands;
    struct retro_video *effect = retro_video_create(&opt);
    assert(effect);
    opt.mosh_mode = RETRO_MOSH_KEYFRAME_SKIP;
    opt.mosh_rewind_depth = 0;
    opt.mosh_slice_history = 0;
    opt.mosh_slice_height = 0;
    struct retro_video *reference = retro_video_create(&opt);
    assert(reference);

    const size_t bytes = (size_t)opt.width * opt.height * 4;
    uint8_t *source = malloc(bytes), *a = malloc(bytes), *b = malloc(bytes);
    assert(source && a && b);
    int rendered = 0, changed = 0, compared = 0;
    for (int tick = 0; tick < 145; ++tick) {
        scene(source, opt.width, opt.height, tick);
        int current = retro_video_process(effect, source, opt.width * 4, a, opt.width * 4);
        int clean = retro_video_process(reference, source, opt.width * 4, b, opt.width * 4);
        assert(current >= 0 && clean >= 0);
        assert(current == clean);
        if (current == 1) {
            ++rendered;
            ++compared;
            if (tick == 0) assert(memcmp(a, b, bytes) == 0); /* no uninitialized history */
            if (memcmp(a, b, bytes) != 0) ++changed;
            for (size_t j = 3; j < bytes; j += 4) assert(a[j] == 255);
        }
    }
    unsigned long long rewound = (unsigned long long)retro_video_rewound_pframes(effect);
    unsigned long long stripes = (unsigned long long)retro_video_temporal_rows_reused(effect);
    printf("EXPANSION mode=%d history=%d band=%d decoded=%d compared=%d changed=%d rewound=%llu rows=%llu\n",
           (int)mode, history, bands, rendered, compared, changed, rewound, stripes);
    assert(rendered >= 135 && compared >= 135);
    assert(changed >= 30);
    if (mode == RETRO_MOSH_PFRAME_REWIND) {
        assert(rewound >= 40);
        assert(stripes == 0);
    } else {
        assert(stripes >= 1000);
        assert(rewound == 0);
    }
    assert(retro_video_rewound_pframes(reference) == 0);
    assert(retro_video_temporal_rows_reused(reference) == 0);
    free(source); free(a); free(b);
    retro_video_destroy(effect); retro_video_destroy(reference);
}

int main(void)
{
    struct retro_video_options o = retro_video_default_options(RETRO_VIDEO_MPEG4_PART2);
    o.mosh_mode = RETRO_MOSH_PFRAME_REWIND;
    o.mosh_rewind_depth = 1;
    assert(retro_video_create(&o) == NULL);
    o.mosh_rewind_depth = 13;
    assert(retro_video_create(&o) == NULL);
    o.mosh_rewind_depth = 4;
    o.codec = RETRO_VIDEO_H263;
    assert(retro_video_create(&o) == NULL);
    o.codec = RETRO_VIDEO_MPEG4_PART2;
    o.mosh_mode = RETRO_MOSH_FRAME_PEEL;
    o.mosh_slice_history = 1;
    o.mosh_slice_height = 12;
    assert(retro_video_create(&o) == NULL);
    o.mosh_slice_history = 9;
    assert(retro_video_create(&o) == NULL);
    o.mosh_slice_history = 4;
    o.mosh_slice_height = 0;
    assert(retro_video_create(&o) == NULL);
    o.mosh_slice_height = 65;
    assert(retro_video_create(&o) == NULL);
    /* Enforce a memory ceiling for the generic codec API even if the caller
     * requests full 2048x2048 rather than OBS's 144px-high internal codec. */
    o.mosh_slice_height = 12;
    o.mosh_slice_history = 8;
    o.width = 2048;
    o.height = 2048;
    assert(retro_video_create(&o) == NULL);

    exercise(RETRO_MOSH_PFRAME_REWIND, 4, 12);
    exercise(RETRO_MOSH_PFRAME_REWIND, 8, 12);
    exercise(RETRO_MOSH_FRAME_PEEL, 4, 12);
    exercise(RETRO_MOSH_FRAME_PEEL, 6, 6);
    return 0;
}
