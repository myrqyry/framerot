/* GPL-2.0; see COPYING. */
#pragma once
#include <stddef.h>
#include <stdint.h>

struct mpeg4_edit_options {
    int width,height,fps;
    int x_scale,y_scale; /* signed percent of encoded motion deltas */
    int residual_scale,residual_loss; /* percent; inter coefficients/blocks only */
    uint32_t seed;
};
struct mpeg4_edit_stats {
    uint64_t vectors,coefficients,blocks;
};
/* Own-encoder progressive, unpartitioned Simple Profile P-VOP only.
 * 1: malloc-owned replacement; 0: unchanged; -1: unsupported, leave original.
 * On every non-1 return output is null and size zero. */
int mpeg4_edit_p_vop(const uint8_t *,size_t,const struct mpeg4_edit_options *,
    uint8_t **,size_t *,struct mpeg4_edit_stats *);
