/* SPDX-License-Identifier: MIT */
#ifndef OA2DP_PCM_FORMAT_H
#define OA2DP_PCM_FORMAT_H
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OA2DP_PCM_INTEGER 0
#define OA2DP_PCM_FLOAT 1

/* Container width, useful precision and representation are independent. */
typedef struct {
    uint32_t SampleRate;
    uint16_t Channels;
    uint16_t BitsPerSample;
    uint16_t ValidBitsPerSample;
    uint16_t Encoding;
} oa2dp_pcm_format;

bool oa2dp_pcm_format_valid(const oa2dp_pcm_format *f);
bool oa2dp_pcm_format_same(const oa2dp_pcm_format *a, const oa2dp_pcm_format *b);
uint16_t oa2dp_pcm_format_ring_type(const oa2dp_pcm_format *f);
/* LDAC input precision; float and integers wider than 24 retain 24 useful bits. */
uint8_t oa2dp_pcm_format_ldac_bits(const oa2dp_pcm_format *f);

#ifdef __cplusplus
}
#endif
#endif
