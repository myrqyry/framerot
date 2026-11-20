#include "../src/video-roundtrip.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void frame_data(uint8_t *dst, int w, int h, int t)
{
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = ((size_t)y * w + x) * 4;
            const int ball = (x - (t * 7) % w) * (x - (t * 7) % w) +
                             (y - (t * 3) % h) * (y - (t * 3) % h) < 1000;
            dst[i + 0] = (uint8_t)(ball ? 235 : (x * 2 + t) % 255);
            dst[i + 1] = (uint8_t)((x / 15 + t / 2 + y / 19) % 2 ? 221 : 55);
            dst[i + 2] = (uint8_t)((y * 3 + t * 8) & 255);
            dst[i + 3] = 255;
        }
}

static void compare_mode(enum retro_mosh_mode mode, int interval, int burst)
{
    struct retro_video_options options = retro_video_default_options(RETRO_VIDEO_MPEG4_PART2);
    options.width = 256;
    options.height = 144;
    options.fps = 12;
    options.bitrate = 160000;
    options.gop_size = 9;
    options.mosh_skip_keyframes = 0;
    options.mosh_mode = mode;
    options.mosh_p_interval = interval;
    options.mosh_p_burst = burst;
    struct retro_video *altered = retro_video_create(&options);
    assert(altered);
    options.mosh_mode = RETRO_MOSH_KEYFRAME_SKIP;
    options.mosh_p_interval = 0;
    options.mosh_p_burst = 0;
    struct retro_video *original = retro_video_create(&options);
    assert(original);
    size_t n = (size_t)options.width * options.height * 4;
    uint8_t *src = malloc(n), *out = malloc(n), *clean = malloc(n);
    assert(src && out && clean);
    int altered_frames = 0, compared = 0, differences = 0;
    for (int t = 0; t < 140; ++t) {
        frame_data(src, options.width, options.height, t);
        int a = retro_video_process(altered, src, options.width * 4, out, options.width * 4);
        int b = retro_video_process(original, src, options.width * 4, clean, options.width * 4);
        assert(a >= 0 && b >= 0);
        if (a == 1) altered_frames++;
        if (a == 1 && b == 1) {
            compared++;
            if (memcmp(out, clean, n) != 0) differences++;
        }
    }
    const unsigned long long p_drops = (unsigned long long)retro_video_dropped_pframes(altered);
    const unsigned long long p_replays = (unsigned long long)retro_video_replayed_pframes(altered);
    printf("MODE_%d decoded=%d common=%d changed=%d p_drops=%llu p_replays=%llu\n",
        (int)mode, altered_frames, compared, differences, p_drops, p_replays);
    assert(altered_frames >= 25);
    assert(compared >= 25);
    assert(differences >= 5);
    if (mode == RETRO_MOSH_PFRAME_ECHO) {
        assert(p_replays >= 10 && p_drops == 0);
    } else {
        assert(p_drops >= 10 && p_replays == 0);
    }
    assert(retro_video_dropped_keyframes(altered) == 0);
    free(src); free(out); free(clean);
    retro_video_destroy(altered);
    retro_video_destroy(original);
}

int main(void)
{
    struct retro_video_options o = retro_video_default_options(RETRO_VIDEO_MPEG4_PART2);
    o.mosh_mode = RETRO_MOSH_PFRAME_DROP;
    o.mosh_p_interval = 5;
    struct retro_video *valid = retro_video_create(&o);
    assert(valid != NULL); /* Actual libavcodec instance accepts mode. */
    retro_video_destroy(valid);
    o.mosh_mode = RETRO_MOSH_PFRAME_BURST;
    o.mosh_p_interval = 8;
    o.mosh_p_burst = 3;
    o.codec = (enum retro_video_codec)1;
    assert(retro_video_create(&o) == NULL); /* Packet mosh only on MPEG-4. */
    o.codec = RETRO_VIDEO_MPEG4_PART2;
    o.mosh_mode = 99;
    assert(retro_video_create(&o) == NULL);
    o.mosh_mode = RETRO_MOSH_PFRAME_BURST;
    o.mosh_p_burst = 9;
    assert(retro_video_create(&o) == NULL);
    compare_mode(RETRO_MOSH_PFRAME_DROP, 4, 1);
    compare_mode(RETRO_MOSH_PFRAME_BURST, 11, 4);
    compare_mode(RETRO_MOSH_PFRAME_ECHO, 4, 1);
    return 0;
}
