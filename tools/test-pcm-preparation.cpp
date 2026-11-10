/* SPDX-License-Identifier: MIT */
/* Actual session preparation block and S16 adapter converter, memory only. */
#include "oa2dp/pcm_ring.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#define REQUIRE(x)                                                                                 \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                   \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)
#define LOGE(...) ((void)0)
#include "pcm-adapter-s16-under-test.inc"

static std::vector<int32_t> session_prepare(const void *source, uint16_t type, uint16_t ring_ch,
                                            uint8_t ch, uint8_t encoder_bits, uint32_t spc) {
    oa2dp_ring_format ring_format{};
    ring_format.channels = ring_ch;
    ring_format.format = type;
    std::vector<int32_t> pcm(static_cast<size_t>(spc) * std::max<uint16_t>(ring_ch, 2));
    const size_t source_bytes = static_cast<size_t>(spc) * ring_ch * 4;
    std::memcpy(pcm.data(), source, source_bytes);
    const size_t input_bytes = pcm.size() * sizeof(int32_t);
    bool failed = false;
    auto fail_stream = [&] { failed = true; };
    do {
#include "pcm-preparation-under-test.inc"
    } while (false);
    REQUIRE(!failed);
    pcm.resize(static_cast<size_t>(spc) * ch);
    return pcm;
}
int main() {
    const float fine[] = {0.1f / 32768, 0.8f / 32768};
    auto mono = session_prepare(fine, OA2DP_RING_FMT_FLOAT32, 2, 1, 16, 1);
    int16_t encoded;
    oa2dp_pcm_to_s16(mono.data(), &encoded, 1);
    if (encoded != 0) {
        std::fprintf(stderr,
                     "normal-range mono S16: got %d, expected 0 after mixing before quantization\n",
                     encoded);
        return 1;
    }
    const float headroom[] = {1.5f, -0.5f};
    mono = session_prepare(headroom, OA2DP_RING_FMT_FLOAT32, 2, 1, 24, 1);
    if (mono[0] != 4194304) {
        std::fprintf(stderr,
                     "mono headroom: got %d, expected 4194304 after mixing before clipping\n",
                     mono[0]);
        return 1;
    }
    const int32_t full[] = {127, 383, -127, -383};
    mono = session_prepare(full, OA2DP_RING_FMT_S32_FULL, 2, 1, 24, 2);
    REQUIRE(mono[0] == 1 && mono[1] == -1);
    for (uint16_t type : {OA2DP_RING_FMT_FLOAT32, OA2DP_RING_FMT_S32, OA2DP_RING_FMT_S32_FULL}) {
        for (uint8_t bits : {16, 24}) {
            for (uint16_t input_channels : {1, 2}) {
                for (uint8_t output_channels : {1, 2}) {
                    const float values[] = {0.5f,  -0.25f, -0.5f,  0.25f,
                                            0.25f, 0.75f,  -0.25f, -0.75f};
                    const float solo[] = {0.5f, -0.5f, 0.25f, -0.25f};
                    int32_t source[8]{};
                    const size_t count = 4 * input_channels;
                    for (size_t i = 0; i < count; ++i) {
                        const float value = input_channels == 1 ? solo[i] : values[i];
                        if (type == OA2DP_RING_FMT_FLOAT32)
                            std::memcpy(source + i, &value, sizeof(value));
                        else
                            source[i] = static_cast<int32_t>(
                                static_cast<double>(value) *
                                (type == OA2DP_RING_FMT_S32_FULL ? 2147483648.0 : 8388608.0));
                    }
                    const auto output =
                        session_prepare(source, type, input_channels, output_channels, bits, 4);
                    for (size_t frame = 0; frame < 4; ++frame) {
                        for (size_t channel = 0; channel < output_channels; ++channel) {
                            const double value = input_channels == 1 ? solo[frame]
                                                 : output_channels == 1
                                                     ? (static_cast<double>(values[2 * frame]) +
                                                        values[2 * frame + 1]) /
                                                           2
                                                     : values[2 * frame + channel];
                            REQUIRE(output[frame * output_channels + channel] ==
                                    static_cast<int32_t>(value * 8388608));
                        }
                    }
                    if (bits == 16) {
                        std::vector<int16_t> packed(output.size());
                        oa2dp_pcm_to_s16(output.data(), packed.data(), output.size());
                        for (size_t i = 0; i < output.size(); ++i) {
                            REQUIRE(output[i] % 256 == 0);
                            REQUIRE(packed[i] == output[i] / 256);
                        }
                    }
                }
            }
        }
    }
    std::puts("production session channel/precision preparation checks passed (memory only)");
}
