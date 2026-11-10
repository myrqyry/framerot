#include "../src/video-roundtrip.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Strong scene changes and moving blocks ensure a reused I packet differs
 * from the I frame that the live MPEG-4 encoder would otherwise deliver. */
static void scene(uint8_t *dst, int w, int h, int t)
{
    const int epoch = t / 13;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t p = ((size_t)y * w + x) * 4;
            int tile = (((x + t * 5) / 11) ^ ((y + epoch * 23) / 14)) & 1;
            dst[p]     = (uint8_t)(tile ? (220 - epoch * 11) : (25 + epoch * 7));
            dst[p + 1] = (uint8_t)((x * 3 + t * 7 + epoch * 19) & 255);
            dst[p + 2] = (uint8_t)(tile ? 20 : 220);
            dst[p + 3] = 255;
        }
}

static void exercise(int interval)
{
    struct retro_video_options o = retro_video_default_options(RETRO_VIDEO_MPEG4_PART2);
    o.width = 256; o.height = 144; o.fps = 12; o.bitrate = 150000;
    o.gop_size = 9;
    o.mosh_mode = RETRO_MOSH_KEYFRAME_ANCHOR;
    o.mosh_anchor_span = interval;
    struct retro_video *altered = retro_video_create(&o);
    assert(altered);
    o.mosh_mode = RETRO_MOSH_KEYFRAME_SKIP;
    o.mosh_anchor_span = 0;
    struct retro_video *clean = retro_video_create(&o);
    assert(clean);
    const size_t n = (size_t)o.width * o.height * 4;
    uint8_t *input = malloc(n), *a = malloc(n), *b = malloc(n);
    assert(input && a && b);
    int frames = 0, changed = 0, compared = 0;
    bool first_same = false;
    for (int t = 0; t < 150; ++t) {
        scene(input, o.width, o.height, t);
        int aa = retro_video_process(altered, input, o.width * 4, a, o.width * 4);
        int bb = retro_video_process(clean, input, o.width * 4, b, o.width * 4);
        assert(aa >= 0 && bb >= 0);
        if (aa == 1) ++frames;
        if (aa == 1 && bb == 1) {
            ++compared;
            const bool eq = memcmp(a, b, n) == 0;
            if (t == 0) first_same = eq;
            if (!eq) changed++;
            for (size_t i = 3; i < n; i += 4) assert(a[i] == 255);
        }
    }
    const unsigned long long reused = (unsigned long long)retro_video_anchored_keyframes(altered);
    const unsigned long long refreshed = (unsigned long long)retro_video_anchor_refreshes(altered);
    printf("ANCHOR span=%d decoded=%d compared=%d changed=%d replayed=%llu refreshed=%llu\n",
           interval, frames, compared, changed, reused, refreshed);
    assert(first_same); /* First I-frame must not be corrupted. */
    assert(frames >= 135 && compared >= 130);
    assert(changed >= 70);
    assert(reused >= (interval == 2 ? 6 : 10));
    assert(refreshed >= 2);
    assert(retro_video_dropped_keyframes(altered) == 0);
    assert(retro_video_dropped_pframes(altered) == 0);
    assert(retro_video_anchored_keyframes(clean) == 0);
    retro_video_destroy(altered);
    retro_video_destroy(clean);
    free(input); free(a); free(b);
}
int main(void)
{
    struct retro_video_options o = retro_video_default_options(RETRO_VIDEO_MPEG4_PART2);
    o.mosh_mode = RETRO_MOSH_KEYFRAME_ANCHOR;
    o.mosh_anchor_span = 0;
    assert(retro_video_create(&o) == NULL);
    o.mosh_anchor_span = 13;
    assert(retro_video_create(&o) == NULL);
    o.mosh_anchor_span = 3;
    o.codec = RETRO_VIDEO_H263;
    assert(retro_video_create(&o) == NULL);
    exercise(2);
    exercise(4);
    exercise(8);
    return 0;
}
