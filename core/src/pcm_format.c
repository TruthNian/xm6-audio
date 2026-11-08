/* SPDX-License-Identifier: MIT */
#include "oa2dp/pcm_format.h"
#include "oa2dp/pcm_ring.h"

bool oa2dp_pcm_format_valid(const oa2dp_pcm_format *f)
{
    if (!f || f->SampleRate < 8000 || f->SampleRate > 192000 ||
        f->Channels < 1 || f->Channels > 2)
        return false;
    if (f->Encoding == OA2DP_PCM_FLOAT)
        return f->BitsPerSample == 32 && f->ValidBitsPerSample == 32;
    return f->Encoding == OA2DP_PCM_INTEGER &&
        (f->BitsPerSample == 16 || f->BitsPerSample == 24 || f->BitsPerSample == 32) &&
        f->ValidBitsPerSample > 0 && f->ValidBitsPerSample <= f->BitsPerSample;
}

bool oa2dp_pcm_format_same(const oa2dp_pcm_format *a, const oa2dp_pcm_format *b)
{
    return a->SampleRate == b->SampleRate && a->Channels == b->Channels &&
        a->BitsPerSample == b->BitsPerSample && a->ValidBitsPerSample == b->ValidBitsPerSample &&
        a->Encoding == b->Encoding;
}

uint16_t oa2dp_pcm_format_ring_type(const oa2dp_pcm_format *f)
{
    return f->Encoding == OA2DP_PCM_FLOAT ? OA2DP_RING_FMT_FLOAT32 :
        f->BitsPerSample == 32 ? OA2DP_RING_FMT_S32_FULL : OA2DP_RING_FMT_S32;
}

uint8_t oa2dp_pcm_format_ldac_bits(const oa2dp_pcm_format *f)
{
    return oa2dp_pcm_format_valid(f) && f->Encoding == OA2DP_PCM_INTEGER &&
        f->ValidBitsPerSample <= 16 ? 16 : 24;
}
