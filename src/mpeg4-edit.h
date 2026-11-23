/* GPL-2.0; see COPYING. */
#pragma once
#include <stddef.h>
#include <stdint.h>

struct mpeg4_edit_options {
    int width,height,fps;
    int x_scale,y_scale; /* signed percent of encoded motion deltas */
    int residual_scale,residual_loss; /* percent; inter coefficients/blocks only */
    uint32_t seed;
    int phantom_percent; /* 0..100: skipped MBs promoted to moving inter MBs */
    int frequency[3]; /* -100..200% delta, low/mid/high inter residual scan bands */
};
struct mpeg4_edit_stats {
    uint64_t vectors,coefficients,blocks,phantom_blocks;
    uint64_t frequency_coefficients[3]; /* actual changed coefficients per scan band */
};
/* Caller-owned reusable scratch. A successful borrowed result remains valid
 * until the next reuse call or destroy; never free the returned pointer. */
struct mpeg4_edit_buffer {
    uint8_t *data;
    size_t capacity;
};
void mpeg4_edit_buffer_destroy(struct mpeg4_edit_buffer *);
int mpeg4_edit_p_vop_reuse(const uint8_t *,size_t,const struct mpeg4_edit_options *,
    struct mpeg4_edit_buffer *,uint8_t **,size_t *,struct mpeg4_edit_stats *);
/* Own-encoder progressive, unpartitioned Simple Profile P-VOP only.
 * 1: malloc-owned replacement; 0: unchanged; -1: unsupported, leave original.
 * On every non-1 return output is null and size zero. */
int mpeg4_edit_p_vop(const uint8_t *,size_t,const struct mpeg4_edit_options *,
    uint8_t **,size_t *,struct mpeg4_edit_stats *);
