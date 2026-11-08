/* SPDX-License-Identifier: MIT */
#include <limits.h>
#include "oa2dp/pcm_format.h"
#include "oa2dp/pcm_ring.h"
#include "test.h"

void test_pcm_format(void)
{
    oa2dp_pcm_format integer32 = {44100, 2, 32, 32, OA2DP_PCM_INTEGER};
    oa2dp_pcm_format float32 = {44100, 2, 32, 32, OA2DP_PCM_FLOAT};
    CHECK(oa2dp_pcm_format_valid(&integer32));
    CHECK(oa2dp_pcm_format_valid(&float32));
    CHECK(!oa2dp_pcm_format_same(&integer32, &float32));
    CHECK_EQ(oa2dp_pcm_format_ring_type(&integer32), OA2DP_RING_FMT_S32_FULL);
    CHECK_EQ(oa2dp_pcm_format_ring_type(&float32), OA2DP_RING_FMT_FLOAT32);
    CHECK_EQ(oa2dp_pcm_format_ldac_bits(&integer32), 24);
    CHECK_EQ(oa2dp_pcm_format_ldac_bits(&float32), 24);
    oa2dp_pcm_format listed = integer32;
    CHECK(oa2dp_pcm_format_same(&listed, &integer32));
    integer32.ValidBitsPerSample = 24;
    CHECK(oa2dp_pcm_format_valid(&integer32)); /* 24-in-32 */
    CHECK(!oa2dp_pcm_format_same(&listed, &integer32));
    listed = integer32;
    integer32.SampleRate = 48000;
    CHECK(!oa2dp_pcm_format_same(&listed, &integer32));
    integer32.SampleRate = 44100;
    CHECK_EQ(oa2dp_pcm_format_ldac_bits(&integer32), 24);
    integer32.ValidBitsPerSample = 16;
    CHECK(oa2dp_pcm_format_valid(&integer32));
    CHECK_EQ(oa2dp_pcm_format_ldac_bits(&integer32), 16);
    float32.ValidBitsPerSample = 24;
    CHECK(!oa2dp_pcm_format_valid(&float32));
    integer32.ValidBitsPerSample = 33;
    CHECK(!oa2dp_pcm_format_valid(&integer32));
    integer32.ValidBitsPerSample = 24;
    integer32.Encoding = 99;
    CHECK(!oa2dp_pcm_format_valid(&integer32));

    const int32_t full[] = {INT32_MIN, INT32_MAX, 128, 127, -128, -127, 0x12345600};
    const int32_t expected24[] = {-8388608, 8388607, 1, 0, -1, 0, 0x123456};
    int32_t converted[7];
    oa2dp_pcm_s32_to_quantized24(full, converted, 7, 24);
    CHECK_MEM(converted, expected24, sizeof(expected24));
    oa2dp_pcm_s32_to_quantized24(full, converted, 7, 16);
    CHECK_EQ(converted[0], -8388608);
    CHECK_EQ(converted[1], 32767 * 256);
    CHECK_EQ(converted[2], 0);

    const float source[] = {1.0f, -1.0f, 0.5f, -0.5f};
    const int32_t expected16[] = {32767 * 256, -8388608, 16384 * 256, -16384 * 256};
    oa2dp_pcm_float_to_quantized24(source, converted, 4, 16);
    CHECK_MEM(converted, expected16, sizeof(expected16));
}
