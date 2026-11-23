/* GPL-2.0; see COPYING. */
#pragma once
#include "video-roundtrip.h"

#ifdef __cplusplus
extern "C" {
#endif
struct codec_worker;
struct codec_worker_stats {
    uint64_t submitted, completed, dropped, errors, recovered, total_codec_ns, max_codec_ns;
    uint64_t edited_packets,edited_vectors,edited_coefficients,edited_blocks,phantom_blocks,edit_unsupported;
    uint64_t frequency_coefficients[3];
    unsigned max_pending;
};
/* One pending input, one in-flight input, latest decoded output.
 * No codec calls on the submitting thread. Submit/take never wait on the codec.
 * Each worker owns an immutable configuration and independent generation. */
struct codec_worker *codec_worker_create(const struct retro_video_options *);
void codec_worker_destroy(struct codec_worker *);
bool codec_worker_submit(struct codec_worker *, const uint8_t *, int stride);
/* Returns true only after storing the latest scalar controls. This mailbox
 * has its own brief lock, independent of the queue/result mutex, so no slider
 * update is silently lost to queue or output contention. */
bool codec_worker_set_controls(struct codec_worker *, const struct retro_video_options *);
void codec_worker_set_anchor_hold(struct codec_worker *, bool hold);
bool codec_worker_faulted(struct codec_worker *);
bool codec_worker_take(struct codec_worker *, uint8_t *);
struct codec_worker_stats codec_worker_stats(struct codec_worker *);

#ifdef __cplusplus
}
#endif
