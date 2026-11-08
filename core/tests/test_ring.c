/* SPDX-License-Identifier: MIT */
#include <stdlib.h>

#include "oa2dp/pcm_ring.h"
#include "test.h"

static uint32_t read_frames(oa2dp_pcm_ring *r, void *data, uint32_t frames)
{
    oa2dp_ring_format format;
    uint32_t got = 0;
    CHECK_EQ(oa2dp_ring_snapshot(r, &format), OA2DP_OK);
    CHECK_EQ(oa2dp_ring_read(r, &format, data, (size_t)frames * format.channels * 4, frames, &got), OA2DP_OK);
    return got;
}

void test_ring(void)
{
    size_t bytes = oa2dp_ring_bytes(8, 2);
    oa2dp_pcm_ring *r = calloc(1, bytes);
    int32_t in[2 * 16], out[2 * 16];
    for (int i = 0; i < 32; i++)
        in[i] = i + 1;

    CHECK_EQ(oa2dp_ring_init(r, 6, 48000, 2, OA2DP_RING_FMT_S32), OA2DP_ERR_INVAL);
    CHECK_EQ(oa2dp_ring_init(r, 8, 48000, 2, OA2DP_RING_FMT_S32), OA2DP_OK);
    CHECK_EQ(oa2dp_ring_validate(r, bytes), OA2DP_OK);
    CHECK_EQ(oa2dp_ring_validate(r, bytes - 1), OA2DP_ERR_PROTO);

    CHECK_EQ(oa2dp_ring_writable(r), 8);
    CHECK_EQ(oa2dp_ring_write(r, in, 5), 5);
    CHECK_EQ(oa2dp_ring_readable(r), 5);
    CHECK_EQ(read_frames(r, out, 3), 3);
    CHECK_MEM(out, in, 3 * 2 * sizeof(int32_t));

    /* Wrap around the end of the buffer. */
    CHECK_EQ(oa2dp_ring_write(r, in + 10, 6), 6);
    CHECK_EQ(oa2dp_ring_readable(r), 8);
    CHECK_EQ(oa2dp_ring_write(r, in, 1), 0); /* full: dropped */
    CHECK_EQ(r->overruns, 1);
    CHECK_EQ(read_frames(r, out, 8), 8);
    CHECK_MEM(out, in + 6, 2 * 2 * sizeof(int32_t));     /* frames 3,4 of first write */
    CHECK_MEM(out + 4, in + 10, 6 * 2 * sizeof(int32_t)); /* the wrapped write */
    CHECK_EQ(oa2dp_ring_readable(r), 0);
    CHECK_EQ(read_frames(r, out, 1), 0);

    /* Reconnection must play new PCM rather than the pre-disconnect backlog. */
    CHECK_EQ(oa2dp_ring_write(r, in, 8), 8);
    CHECK_EQ(oa2dp_ring_discard(r), 8);
    CHECK_EQ(oa2dp_ring_readable(r), 0);
    CHECK_EQ(oa2dp_ring_write(r, in + 16, 1), 1);
    CHECK_EQ(read_frames(r, out, 1), 1);
    CHECK_MEM(out, in + 16, 2 * sizeof(int32_t));

    /* Mono uses the same fixed storage slots, including across wraparound. */
    CHECK_EQ(oa2dp_ring_set_format(r, 48000, 1, OA2DP_RING_FMT_S32), 1);
    oa2dp_ring_format mono_format, stereo_format;
    CHECK_EQ(oa2dp_ring_snapshot(r, &mono_format), OA2DP_OK);
    CHECK_EQ(oa2dp_ring_write(r, in, 3), 3);
    CHECK_EQ(read_frames(r, out, 3), 3);
    CHECK_MEM(out, in, 3 * sizeof(int32_t));
    CHECK_EQ(oa2dp_ring_write(r, in, 8), 8);
    CHECK_EQ(oa2dp_ring_writable(r), 0);

    /* An old one-channel destination cannot be sized from a new stereo header. */
    uint32_t got = 99;
    int32_t guarded[5] = {-1, -1, -1, -1, 0x12345678};
    CHECK_EQ(oa2dp_ring_set_format(r, 44100, 2, OA2DP_RING_FMT_FLOAT32), 1);
    CHECK_EQ(oa2dp_ring_read(r, &mono_format, guarded, 4 * sizeof(int32_t), 4, &got), OA2DP_ERR_STATE);
    CHECK_EQ(got, 0);
    CHECK_EQ(guarded[0], -1);
    CHECK_EQ(guarded[4], 0x12345678);
    CHECK_EQ(oa2dp_ring_snapshot(r, &stereo_format), OA2DP_OK);
    uint64_t before_read = r->read_pos;
    CHECK_EQ(oa2dp_ring_read(r, &stereo_format, guarded, 4 * sizeof(int32_t), 4, &got), OA2DP_ERR_INVAL);
    CHECK_EQ(r->read_pos, before_read);
    CHECK_EQ(oa2dp_ring_read(r, &stereo_format, out, sizeof(out), 4, &got), OA2DP_OK);
    CHECK_EQ(got, 0);
    CHECK_EQ(r->read_pos, stereo_format.start_pos);
    CHECK_EQ(oa2dp_ring_writable(r), 8); /* old backlog released, not reinterpreted */

    float next[8] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
    CHECK_EQ(oa2dp_ring_write(r, next, 2), 2);
    before_read = r->read_pos;
    CHECK_EQ(oa2dp_ring_read(r, &stereo_format, out, sizeof(out), 4, &got), OA2DP_OK);
    CHECK_EQ(got, 0); /* partial encoder blocks must not be consumed and lost */
    CHECK_EQ(r->read_pos, before_read);
    CHECK_EQ(oa2dp_ring_write(r, next + 4, 2), 2);
    CHECK_EQ(oa2dp_ring_read(r, &stereo_format, out, sizeof(out), 4, &got), OA2DP_OK);
    CHECK_EQ(got, 4);
    CHECK_MEM(out, next, sizeof(next));
    CHECK_EQ(oa2dp_ring_set_format(r, 44100, 2, OA2DP_RING_FMT_FLOAT32), 0);
    CHECK_EQ(r->format_sequence, stereo_format.sequence);
    CHECK_EQ(oa2dp_ring_set_format(r, 44100, 2, OA2DP_RING_FMT_S32_FULL), 1);
    CHECK_EQ(oa2dp_ring_read(r, &stereo_format, out, sizeof(out), 1, &got), OA2DP_ERR_STATE);
    CHECK_EQ(oa2dp_ring_set_format(r, 48000, 3, OA2DP_RING_FMT_S32), OA2DP_ERR_INVAL);
    r->format_sequence |= 1;
    CHECK_EQ(oa2dp_ring_snapshot(r, &stereo_format), OA2DP_ERR_BUSY);
    r->format_sequence &= ~1u;
    r->version = 1;
    CHECK_EQ(oa2dp_ring_validate(r, bytes), OA2DP_ERR_PROTO);
    r->version = OA2DP_RING_VERSION;

    /* Corrupted header from the other side is rejected. */
    r->capacity = 1000;
    CHECK_EQ(oa2dp_ring_validate(r, bytes), OA2DP_ERR_PROTO);
    r->capacity = 1u << 20;
    CHECK_EQ(oa2dp_ring_validate(r, bytes), OA2DP_ERR_PROTO);
    free(r);

    float f[5] = {0.0f, 1.0f, -1.0f, 2.0f, 0.5f};
    int32_t s[5];
    oa2dp_pcm_float_to_s24(f, s, 5);
    CHECK_EQ(s[0], 0);
    CHECK_EQ(s[1], 8388607);
    CHECK_EQ(s[2], -8388608);
    CHECK_EQ(s[3], 8388607);
    CHECK_EQ(s[4], 4194304);

    int32_t st[4] = {100, 300, -10, 10}, mono[2];
    oa2dp_pcm_downmix_to_mono(st, mono, 2);
    CHECK_EQ(mono[0], 200);
    CHECK_EQ(mono[1], 0);
}
