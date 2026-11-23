/* GPL-2.0; internal FFmpeg packet rewrite boundary.
 * The source bytes must not overlap the packet's owned storage: the MPEG-4
 * transcriber returns independent scratch data. */
#pragma once
#include <stddef.h>
#include <libavcodec/packet.h>

/* Overwrite a packet's encoded payload while preserving its timestamps,
 * flags, side-data and references. COW detaches shared payloads; unique
 * packets reuse existing storage when no growth is needed.
 * Returns 0 on success, negative on invalid input or FFmpeg failure. */
int framerot_packet_overwrite(AVPacket *packet,const uint8_t *bytes,size_t size);
