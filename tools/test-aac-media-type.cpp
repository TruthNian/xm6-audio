/* SPDX-License-Identifier: MIT */
/* Run the production AAC media-type setup against the actual Windows MFT.
 * No audio device, Bluetooth radio, pairing or player is opened. */
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <vector>
#include "oa2dp/encoder.h"
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)
namespace {
const GUID kClsidAacEncoder = {0x93af0c51,0x2275,0x45d2,{0xa3,0x5b,0xf2,0xba,0x21,0xca,0xed,0x00}};
#include "aac-backend-under-test.inc"
#include "aac-media-type-under-test.inc"
}
int main() {
    AacBackend direct;
    const bool installed = SUCCEEDED(start_backend(&direct));
    close_backend(&direct);
    REQUIRE(oa2dp_encoder_available(OA2DP_CODEC_AAC) == installed);
    if (!installed) {
        std::puts("AAC media-type checks skipped: OS backend unavailable; fallback covered separately");
        return 0;
    }
    const uint32_t limits[] = {64000,0,95999,96000,127999,128000,159999,160000,191999,192000,320000};
    const uint32_t expected[] = {0,192000,0,96000,96000,128000,128000,160000,160000,192000,192000};
    unsigned accepted_types = 0, rejected_limits = 0;
    for (UINT32 rate : {44100u,48000u})
        for (UINT32 channels : {1u,2u})
            for (size_t i = 0; i < sizeof(limits)/sizeof(limits[0]); ++i) {
                AacBackend backend;
                REQUIRE(SUCCEEDED(start_backend(&backend)));
                const HRESULT hr = set_types(backend.mft,rate,channels,limits[i]);
                IMFMediaType *type = nullptr;
                if (expected[i]) {
                    ++accepted_types;
                    REQUIRE(SUCCEEDED(hr));
                    REQUIRE(SUCCEEDED(backend.mft->GetOutputCurrentType(0,&type)));
                    UINT32 actual = 0;
                    REQUIRE(SUCCEEDED(type->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,&actual)));
                    REQUIRE(actual * 8 == expected[i]);
                    REQUIRE(!limits[i] || actual * 8 <= limits[i]);
                    REQUIRE(SUCCEEDED(type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,&actual)) && actual == rate);
                    REQUIRE(SUCCEEDED(type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS,&actual)) && actual == channels);
                    release(type);
                } else {
                    ++rejected_limits;
                    if (SUCCEEDED(hr) && SUCCEEDED(backend.mft->GetOutputCurrentType(0,&type))) {
                        UINT32 actual = 0;
                        type->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,&actual);
                        std::fprintf(stderr,"AAC limit %u unexpectedly accepted at %u bit/s\n",limits[i],actual*8);
                        release(type);
                    }
                    REQUIRE(hr == MF_E_INVALIDMEDIATYPE);
                    REQUIRE(FAILED(backend.mft->GetOutputCurrentType(0,&type)));
                    release(type);
                }
                close_backend(&backend);
            }
    oa2dp_settings settings{};
    oa2dp_settings_default(&settings);
    oa2dp_codec_caps remote{}, config{};
    oa2dp_codec_local_caps(OA2DP_CODEC_AAC,&remote);
    REQUIRE(oa2dp_codec_select(nullptr,&remote,&settings,&config) == OA2DP_OK);
    oa2dp_encoder *encoder = nullptr;
    config.u.aac.bitrate = 64000;
    REQUIRE(oa2dp_encoder_create(&config,&settings,895,&encoder) == OA2DP_ERR_UNSUPPORTED && !encoder);
    config.u.aac.bitrate = 192000;
    settings.aac_bitrate = 64000;
    REQUIRE(oa2dp_encoder_create(&config,&settings,895,&encoder) == OA2DP_ERR_UNSUPPORTED && !encoder);
    settings.aac_bitrate = 0;
    REQUIRE(oa2dp_encoder_create(&config,&settings,895,&encoder) == OA2DP_OK && encoder);
    std::vector<int32_t> pcm(encoder->info.samples_per_call * encoder->info.channels,0);
    std::vector<uint8_t> output(encoder->info.max_output);
    unsigned frames_total = 0;
    for (unsigned i = 0; i < 16; ++i) {
        // A diagnostic/another session's probe must not shut down a live MFT.
        REQUIRE(oa2dp_encoder_available(OA2DP_CODEC_AAC));
        size_t bytes = 0;
        uint8_t frames = 0;
        REQUIRE(oa2dp_encoder_encode(encoder,pcm.data(),output.data(),output.size(),&bytes,&frames) == OA2DP_OK);
        REQUIRE(bytes <= output.size());
        frames_total += frames;
    }
    REQUIRE(frames_total > 0);
    oa2dp_encoder_destroy(encoder);
    std::printf("actual Windows AAC MFT: %u accepted media types, %u rejected limits; factory rejection checks passed\n",
                 accepted_types,rejected_limits);
}
