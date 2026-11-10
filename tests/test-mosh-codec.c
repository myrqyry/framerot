#include "../src/video-roundtrip.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void moving_scene(uint8_t *dst, int w, int h, int frame)
{
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int i = 4 * (y * w + x);
            const int stripe = ((x + 5 * frame) / 12 + y / 17) & 1;
            dst[i + 0] = (uint8_t)((x + frame * 7) & 255);
            dst[i + 1] = (uint8_t)(stripe ? 220 : 28);
            dst[i + 2] = (uint8_t)((y * 3 + frame) & 255);
            dst[i + 3] = 255;
        }
}

int main(void)
{
    struct retro_video_options options = retro_video_default_options(RETRO_VIDEO_MPEG4_PART2);
    options.width = 176;
    options.height = 144;
    options.fps = 12;
    options.bitrate = 96000;
    options.gop_size = 6;
    options.mosh_skip_keyframes = 1;
    struct retro_video_options unsupported = options;
    unsupported.codec = RETRO_VIDEO_H263;
    assert(retro_video_create(&unsupported) == NULL); /* No unsupported codec bitstream mosh. */
    unsupported = options;
    unsupported.mosh_skip_keyframes = 17;
    assert(retro_video_create(&unsupported) == NULL); /* Bound the keyframe skip cycle. */

    struct retro_video *mosh = retro_video_create(&options);
    options.mosh_skip_keyframes = 0;
    struct retro_video *clean = retro_video_create(&options);
    assert(mosh && clean);
    const size_t size = (size_t)options.width * options.height * 4;
    uint8_t *src = malloc(size), *a = malloc(size), *b = malloc(size);
    assert(src && a && b);
    int clean_frames = 0, mosh_frames = 0, changed_frames = 0, common_frames = 0, recovered_frames = 0;
    uint64_t changed_pixels = 0;
    for (int frame = 0; frame < 84; ++frame) {
        moving_scene(src, options.width, options.height, frame);
        const int c = retro_video_process(clean, src, options.width * 4, a, options.width * 4);
        const int m = retro_video_process(mosh, src, options.width * 4, b, options.width * 4);
        assert(c >= 0 && m >= 0);
        clean_frames += (c == 1);
        mosh_frames += (m == 1);
        if (c == 1 && m == 1) {
            ++common_frames;
            int changed = 0;
            for (size_t i = 0; i < size; i += 4) {
                changed += (a[i] != b[i] || a[i+1] != b[i+1] || a[i+2] != b[i+2]);
                assert(b[i+3] == 255);
            }
            changed_frames += (changed != 0);
            recovered_frames += (changed == 0);
            changed_pixels += (uint64_t)changed;
        }
    }
    assert(retro_video_dropped_keyframes(clean) == 0);
    assert(retro_video_dropped_keyframes(mosh) >= 3);
    assert(clean_frames >= 75);
    assert(mosh_frames >= 20);
    assert(common_frames >= 15);
    assert(changed_frames >= 2);
    assert(recovered_frames >= 4); /* Periodic unmodified I frames restore image. */
    assert(changed_pixels > 500);
    printf("MOSH_ACTUAL_PACKETS_OK clean=%d mosh=%d common=%d changed=%d pixels=%llu dropped=%llu\n",
           clean_frames, mosh_frames, common_frames, changed_frames,
           (unsigned long long)changed_pixels,
           (unsigned long long)retro_video_dropped_keyframes(mosh));
    retro_video_destroy(clean);
    retro_video_destroy(mosh);
    free(src);
    free(a);
    free(b);
    return 0;
}
