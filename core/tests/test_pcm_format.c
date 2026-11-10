/* SPDX-License-Identifier: MIT */
#include <limits.h>
#include <math.h>
#include "oa2dp/pcm_format.h"
#include "oa2dp/pcm_ring.h"
#include "test.h"

static void test_encoder_preparation(void)
{
    oa2dp_ring_format f = {0};
    f.channels = 1; f.format = OA2DP_RING_FMT_FLOAT32;
    int32_t words[10] = {0}, out[10] = {0};
    const float mono[] = {0.5f, -0.5f, 0.25f, -0.25f};
    const int32_t expanded[] = {4194304, 4194304, -4194304, -4194304,
                              2097152, 2097152, -2097152, -2097152};
    memcpy(words, mono, sizeof(mono)); words[8] = 0x12345678; words[9] = 0x76543210;
    CHECK_EQ(oa2dp_pcm_prepare_encoder(&f, words, sizeof(mono), words, 8, 4, 2, 24), OA2DP_OK);
    CHECK_MEM(words, expanded, sizeof(expanded));
    CHECK_EQ(words[8], 0x12345678); CHECK_EQ(words[9], 0x76543210);

    const int32_t full[] = {INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX, INT32_MIN, INT32_MAX,
                           127, 383, -127, -383};
    const int32_t expected[] = {-8388608, 8388607, 0, 1, -1};
    uint8_t unaligned[sizeof(full) + 1]; memcpy(unaligned + 1, full, sizeof(full));
    f.channels = 2; f.format = OA2DP_RING_FMT_S32_FULL;
    CHECK_EQ(oa2dp_pcm_prepare_encoder(&f, unaligned + 1, sizeof(full), out, 5, 5, 1, 24), OA2DP_OK);
    CHECK_MEM(out, expected, sizeof(expected));
    memcpy(words, full, sizeof(full));
    CHECK_EQ(oa2dp_pcm_prepare_encoder(&f, words, sizeof(full), words, 5, 5, 1, 24), OA2DP_OK);
    CHECK_MEM(words, expected, sizeof(expected));

    const float nonfinite[] = {NAN, 0.5f, INFINITY, -INFINITY, INFINITY, 0, -INFINITY, 0};
    const int32_t expected_nonfinite[] = {2097152, 0, 8388607, -8388608};
    f.format = OA2DP_RING_FMT_FLOAT32;
    CHECK_EQ(oa2dp_pcm_prepare_encoder(&f, nonfinite, sizeof(nonfinite), out, 4, 4, 1, 24), OA2DP_OK);
    CHECK_MEM(out, expected_nonfinite, sizeof(expected_nonfinite));

    const int32_t ties[] = {-128, 128, -384, 384};
    const int32_t expected_ties[] = {-256, 256, -512, 512};
    f.channels = 1; f.format = OA2DP_RING_FMT_S32;
    CHECK_EQ(oa2dp_pcm_prepare_encoder(&f, ties, sizeof(ties), out, 4, 4, 1, 16), OA2DP_OK);
    CHECK_MEM(out, expected_ties, sizeof(expected_ties));
    f.channels = 2;
    CHECK_EQ(oa2dp_pcm_prepare_encoder(&f, full, 4 * sizeof(int32_t), out, 2, 2, 1, 24), OA2DP_OK);
    CHECK_EQ(out[0], -8388608); CHECK_EQ(out[1], 8388607); /* wide sum then saturation */

    int32_t guard[6]; for (size_t i = 0; i < 6; ++i) guard[i] = 0x12345678;
    int32_t before[6]; memcpy(before, guard, sizeof(guard));
    f.channels = 1; f.format = OA2DP_RING_FMT_S32_FULL;
    CHECK_EQ(oa2dp_pcm_prepare_encoder(&f, full, 3, guard + 1, 4, 1, 1, 24), OA2DP_ERR_INVAL);
    CHECK_EQ(oa2dp_pcm_prepare_encoder(&f, full, sizeof(full), guard + 1, 1, 2, 1, 24), OA2DP_ERR_NOSPC);
    CHECK_EQ(oa2dp_pcm_prepare_encoder(&f, full, sizeof(full), guard + 1, 4, 1, 0, 24), OA2DP_ERR_INVAL);
    CHECK_EQ(oa2dp_pcm_prepare_encoder(&f, full, sizeof(full), guard + 1, 4, 1, 3, 24), OA2DP_ERR_INVAL);
    CHECK_EQ(oa2dp_pcm_prepare_encoder(&f, full, sizeof(full), guard + 1, 4, 1, 1, 32), OA2DP_ERR_INVAL);
    CHECK_EQ(oa2dp_pcm_prepare_encoder(&f, NULL, sizeof(full), guard + 1, 4, 1, 1, 24), OA2DP_ERR_INVAL);
    f.channels = 0;
    CHECK_EQ(oa2dp_pcm_prepare_encoder(&f, full, sizeof(full), guard + 1, 4, 1, 1, 24), OA2DP_ERR_INVAL);
    f.channels = 3;
    CHECK_EQ(oa2dp_pcm_prepare_encoder(&f, full, sizeof(full), guard + 1, 4, 1, 1, 24), OA2DP_ERR_INVAL);
    f.channels = 1; f.format = 99;
    CHECK_EQ(oa2dp_pcm_prepare_encoder(&f, full, sizeof(full), guard + 1, 4, 1, 1, 24), OA2DP_ERR_INVAL);
    CHECK_MEM(guard, before, sizeof(guard));
    f.format = OA2DP_RING_FMT_S32_FULL;
    CHECK_EQ(oa2dp_pcm_prepare_encoder(&f, NULL, 0, NULL, 0, 0, 2, 24), OA2DP_OK);
}

void test_pcm_format(void)
{
    test_encoder_preparation();
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
    oa2dp_ring_format full_format = {0}; full_format.channels = 1; full_format.format = OA2DP_RING_FMT_S32_FULL;
    CHECK_EQ(oa2dp_pcm_prepare_encoder(&full_format, full, sizeof(full), converted, 7, 7, 1, 24), OA2DP_OK);
    CHECK_MEM(converted, expected24, sizeof(expected24));
    oa2dp_pcm_s32_to_quantized24(full, converted, 7, 16);
    CHECK_EQ(converted[0], -8388608);
    CHECK_EQ(converted[1], 32767 * 256);
    CHECK_EQ(converted[2], 0);

    const float source[] = {1.0f, -1.0f, 0.5f, -0.5f};
    const int32_t expected16[] = {32767 * 256, -8388608, 16384 * 256, -16384 * 256};
    oa2dp_pcm_float_to_quantized24(source, converted, 4, 16);
    CHECK_MEM(converted, expected16, sizeof(expected16));
    full_format.channels = 2; full_format.format = OA2DP_RING_FMT_FLOAT32;
    CHECK_EQ(oa2dp_pcm_prepare_encoder(&full_format, source, sizeof(source), converted, 4, 2, 2, 16), OA2DP_OK);
    CHECK_MEM(converted, expected16, sizeof(expected16));
}
