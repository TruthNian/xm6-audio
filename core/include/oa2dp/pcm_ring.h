/* SPDX-License-Identifier: MIT */
/*
 * Single-producer / single-consumer PCM ring shared between the virtual
 * audio driver (producer, kernel) and the service (consumer, user mode)
 * through one mapped memory section. Plain C, no OS headers, so the same
 * code builds in both worlds and in unit tests.
 *
 * Positions are 64-bit frame counters that only grow; the slot is
 * position % capacity. The producer never overwrites unread audio: when
 * the consumer lags, new audio is dropped and counted in `overruns`.
 */
#ifndef OA2DP_PCM_RING_H
#define OA2DP_PCM_RING_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OA2DP_RING_MAGIC 0x4D435041u /* "APCM" */
#define OA2DP_RING_VERSION 3

#define OA2DP_RING_FMT_FLOAT32 0 /* interleaved IEEE float, nominal [-1, 1] */
#define OA2DP_RING_FMT_S32 1     /* interleaved int32, 24-bit values */
#define OA2DP_RING_FMT_S32_FULL 2 /* signed full-scale int32, converted only at encoder boundary */

#define OA2DP_RING_FLAG_RUNNING 0x1        /* endpoint is rendering */
#define OA2DP_RING_FLAG_FORMAT_CHANGED 0x2 /* consumer must re-read format */
#define OA2DP_RING_FLAG_EOS 0x4            /* final valid source frame committed */

#if defined(_MSC_VER)
#define OA2DP_ALIGN8 __declspec(align(8))
#else
#define OA2DP_ALIGN8 __attribute__((aligned(8)))
#endif

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t capacity;      /* frames, power of two */
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t format;        /* OA2DP_RING_FMT_* */
    volatile uint32_t flags;
    OA2DP_ALIGN8 volatile uint64_t write_pos; /* written by producer only */
    OA2DP_ALIGN8 volatile uint64_t read_pos;  /* written by consumer only */
    OA2DP_ALIGN8 volatile uint64_t overruns;  /* frames dropped by producer */
    volatile uint32_t format_sequence; /* odd during a producer format update */
    uint16_t storage_channels; /* immutable slot stride; channels may be smaller */
    uint16_t reserved0;
    OA2DP_ALIGN8 volatile uint64_t format_start_pos; /* first frame of this format */
    volatile uint32_t consumer_sequence; /* admitted encoder epoch; odd = retired */
    uint32_t reserved[3];
    /* followed by capacity * storage_channels * 4 bytes of sample data */
} oa2dp_pcm_ring;

typedef struct {
    uint32_t sequence;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t format;
    uint32_t capacity;
    uint16_t storage_channels;
    uint64_t start_pos;
} oa2dp_ring_format;

/* Total bytes to allocate (header + fixed storage-channel slots). */
size_t oa2dp_ring_bytes(uint32_t capacity, uint16_t channels);

int oa2dp_ring_init(oa2dp_pcm_ring *r, uint32_t capacity, uint32_t sample_rate,
                    uint16_t channels, uint16_t format);

/* Validate a ring mapped from the other side. `mapped_bytes` is the size of
 * the mapping, so a hostile/buggy header cannot make us read past it. */
int oa2dp_ring_validate(const oa2dp_pcm_ring *r, size_t mapped_bytes);

/* Producer only, serialized with writes. Returns 1 changed, 0 unchanged, <0 error.
 * Storage stride and counters do not change. Old-format backlog is not retyped. */
int oa2dp_ring_set_format(oa2dp_pcm_ring *r, uint32_t sample_rate,
                          uint16_t channels, uint16_t format);
/* Producer only: start a new epoch even if normalized storage is unchanged.
 * Use when source PCM precision/container metadata changes within one ring
 * representation; old consumers must not process the new source's samples. */
int oa2dp_ring_begin_format(oa2dp_pcm_ring *r, uint32_t sample_rate,
                            uint16_t channels, uint16_t format);
/* A stable format transaction, or ERR_BUSY while the producer changes it. */
int oa2dp_ring_snapshot(const oa2dp_pcm_ring *r, oa2dp_ring_format *format);

uint32_t oa2dp_ring_readable(const oa2dp_pcm_ring *r);
uint32_t oa2dp_ring_writable(const oa2dp_pcm_ring *r);
/* Atomic diagnostic snapshot; use instead of reading a live counter directly. */
uint64_t oa2dp_ring_overruns(const oa2dp_pcm_ring *r);
/* The virtual hardware clock must not consume Windows' cyclic buffer before
 * the codec/transport accepts this epoch. Only the consumer publishes admission. */
int oa2dp_ring_admit(oa2dp_pcm_ring *r, const oa2dp_ring_format *format);
void oa2dp_ring_retire(oa2dp_pcm_ring *r);
bool oa2dp_ring_consumer_ready(const oa2dp_pcm_ring *r);

/* Producer: copy up to `frames`; returns frames written. */
uint32_t oa2dp_ring_write(oa2dp_pcm_ring *r, const void *data, uint32_t frames);
/* Producer: admit once, then convert only the admitted prefix directly into
 * ring storage. 16/24-bit little-endian integer input requires S32 ring format;
 * 32-bit containers are copied unchanged (integer or float per ring format).
 * Source bytes of rejected frames are never accessed. Producer serialization
 * and unread-frame/drop semantics match oa2dp_ring_write. */
uint32_t oa2dp_ring_write_pcm(oa2dp_pcm_ring *r, const void *data, uint32_t frames,
                             uint16_t container_bits);
/* Consumer: a complete block, or zero frames if insufficient data is available.
 * Never sizes a copy from a mutable format. ERR_STATE rejects an obsolete
 * snapshot (including changes during the copy); the copied block must then be
 * ignored. Partial current-format blocks stay queued; obsolete backlog is
 * released even if the new format has no full block yet. Output is bounded. */
int oa2dp_ring_read(oa2dp_pcm_ring *r, const oa2dp_ring_format *format,
                    void *data, size_t data_bytes, uint32_t frames, uint32_t *read_frames);
/* Service block reader: while RUNning, wait for a complete block. On PAUSE,
 * STOP or committed EOS, preserve any partial tail and zero-pad the remainder.
 * `ready` also permits paced silence after the source tail, flushing codec
 * lookahead during the existing idle grace period. `read_frames` counts only
 * real source frames, never padding. Caller must pace the resulting media. */
int oa2dp_ring_read_block(oa2dp_pcm_ring *r, const oa2dp_ring_format *format,
                    void *data, size_t data_bytes, uint32_t frames,
                    uint32_t *read_frames, bool *ready);
/* Consumer: explicitly abandon queued PCM. Never use at encoder admission:
 * the current clip may already be queued. Epoch boundaries reject old clips. */
uint32_t oa2dp_ring_discard(oa2dp_pcm_ring *r);

/* Sample conversion for the consumer. */
void oa2dp_pcm_float_to_s24(const float *in, int32_t *out, size_t n);
/* Quantize once to encoder precision (16/24), in the adapters' 24-bit scale. */
void oa2dp_pcm_float_to_quantized24(const float *in, int32_t *out, size_t n, uint8_t bits);
void oa2dp_pcm_s32_to_quantized24(const int32_t *in, int32_t *out, size_t n, uint8_t bits);
/* Consumer-only: map one/two channels before clipping/quantizing once to the
 * encoder's 16/24 useful bits, returned in the adapters' signed 24-bit scale.
 * Input contains ring-format 32-bit words (float bits/S24/full S32). Buffers
 * may be identical int32_t word storage (including raw float bits) or disjoint;
 * mono expansion in place is supported. Channel/representation/precision and
 * buffer bounds are checked before writing. Partial overlap is unsupported.
 * Nearest ties round away from zero; NaN channels are treated as silence. */
int oa2dp_pcm_prepare_encoder(const oa2dp_ring_format *format,
                              const void *input, size_t input_bytes,
                              int32_t *output, size_t output_samples,
                              uint32_t frames, uint16_t output_channels, uint8_t bits);
/* Preserve FLOAT32/full S32 representation for a native-input encoder. Channel
 * mapping precedes float sanitization/clipping; no fixed 16/24-bit quantization.
 * Integer downmix rounds once to full S32. Same-buffer expansion is supported. */
int oa2dp_pcm_prepare_native32(const oa2dp_ring_format *format,
                               const void *input, size_t input_bytes,
                               int32_t *output, size_t output_samples,
                               uint32_t frames, uint16_t output_channels);
/* Interleaved stereo -> mono average, in place allowed (out may == in). */
void oa2dp_pcm_downmix_to_mono(const int32_t *in, int32_t *out, size_t frames);

#ifdef __cplusplus
}
#endif

#endif
