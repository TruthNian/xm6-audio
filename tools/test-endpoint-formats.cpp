/* SPDX-License-Identifier: MIT */
/* Actual initial-selection block and endpoint-setup method, with real core
 * codec/PCM policy and fake audio IOCTLs. No Windows engine or headset implied. */
#include "oa2dp/a2dp_codecs.h"
#include "oa2dp/avdtp.h"
#include "oa2dp/pcm_format.h"
#include "oa2dp/settings.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

static unsigned failures;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)
#define LOGE(...) ((void)0)
using USHORT = uint16_t;
using ULONG = uint32_t;
using OA2DP_FORMAT = oa2dp_pcm_format;
constexpr ULONG OA2DP_MAX_FORMATS = 32;
struct OA2DP_FORMAT_LIST {
    ULONG Count = 0, Preferred = 0;
    OA2DP_FORMAT Formats[OA2DP_MAX_FORMATS]{};
};
struct Session {
    struct Audio {
        OA2DP_FORMAT_LIST formats;
        bool formats_ok = true, latency_ok = true, connected_ok = true;
        bool formats_set = false, latency_set = false, connected = false;
        bool set_formats(const OA2DP_FORMAT_LIST &list) {
            formats = list;
            formats_set = formats_ok;
            return formats_ok;
        }
        bool set_latency(unsigned, unsigned) {
            latency_set = latency_ok;
            return latency_ok;
        }
        bool set_connected(bool value, ULONG) {
            connected = value && connected_ok;
            return connected;
        }
    } audio_;
    oa2dp_settings settings_{};
    oa2dp_codec_caps config_{}, remote_caps_{};
    OA2DP_FORMAT audio_format_{48000, 2, 32, 32, OA2DP_PCM_FLOAT};
    bool connection_enabled_ = true, have_remote_caps_ = true, publication_pending_ = false;
    uint32_t delay_report_us_ = 0;
    ULONG sig_generation_ = 1;
    oa2dp_avdtp av_{};
    uint8_t local_seid_ = 0;
    unsigned begin_calls = 0;
    void begin_streaming() {
        CHECK(audio_.formats_set && audio_.latency_set && audio_.connected);
        ++begin_calls;
    }
    Session() {
        oa2dp_settings_default(&settings_);
        oa2dp_codec_local_caps(OA2DP_CODEC_LDAC, &remote_caps_);
        CHECK(oa2dp_codec_select(nullptr, &remote_caps_, &settings_, &config_) == OA2DP_OK);
    }
    bool ensure_audio_endpoint() { return true; }
    bool update_suspend_state() { return true; }
    bool setup_audio_endpoint();
    bool select(oa2dp_codec_caps &out) {
        struct {
            oa2dp_codec_caps caps;
        } c{remote_caps_};
        for (int once = 0; once < 1; ++once) {
#include "codec-initial-selection-under-test.inc"
            out = cfg;
            return true;
        }
        return false;
    }
    OA2DP_FORMAT preferred() const { return audio_.formats.Formats[audio_.formats.Preferred]; }
};
#include "endpoint-formats-under-test.inc"

int main() {
    const uint32_t rates[] = {44100, 48000, 88200, 96000};
    const OA2DP_FORMAT representations[] = {{0, 2, 32, 32, OA2DP_PCM_FLOAT},
                                            {0, 2, 32, 32, OA2DP_PCM_INTEGER},
                                            {0, 2, 32, 24, OA2DP_PCM_INTEGER},
                                            {0, 2, 24, 24, OA2DP_PCM_INTEGER},
                                            {0, 2, 16, 16, OA2DP_PCM_INTEGER}};
    for (auto rate : rates)
        for (const auto &representation : representations) {
            Session s;
            s.audio_format_ = representation;
            s.audio_format_.SampleRate = rate;
            oa2dp_codec_caps cfg{};
            CHECK(s.select(cfg));
            CHECK(oa2dp_codec_sample_rate(&cfg) == rate);
            CHECK(s.settings_.sample_rate == 0 && s.settings_.bits == 0);
            // Both a lower and higher peer-selected rate must not override PCM.
            for (uint32_t negotiated : {48000u, 96000u}) {
                CHECK(oa2dp_codec_set_rate(&s.config_, &s.remote_caps_, negotiated) == OA2DP_OK);
                CHECK(s.setup_audio_endpoint());
                const auto preferred = s.preferred();
                CHECK(oa2dp_pcm_format_same(&preferred, &s.audio_format_));
                CHECK(s.audio_.formats.Count == 20); // retain all rates/representations
            }
        }
    {
        Session s;
        s.settings_.sample_rate = 96000;
        s.audio_format_.SampleRate = 48000;
        oa2dp_codec_caps cfg{};
        CHECK(s.select(cfg) && oa2dp_codec_sample_rate(&cfg) == 96000);
        s.config_ = cfg;
        CHECK(s.setup_audio_endpoint());
        CHECK(s.preferred().SampleRate == 96000 && s.audio_.formats.Count == 5);
    }
    {
        Session s;
        s.settings_.follow_source_rate = false;
        s.audio_format_.SampleRate = 96000;
        oa2dp_codec_caps cfg{};
        CHECK(s.select(cfg) && oa2dp_codec_sample_rate(&cfg) == 48000);
        CHECK(s.setup_audio_endpoint());
        CHECK(s.preferred().SampleRate == 48000);
    }
    {
        Session s;
        s.settings_.bits = 16;
        s.audio_format_ = {44100, 2, 16, 16, OA2DP_PCM_INTEGER};
        CHECK(s.setup_audio_endpoint());
        const auto preferred = s.preferred();
        CHECK(oa2dp_pcm_format_same(&preferred, &s.audio_format_));
        CHECK(s.audio_.formats.Count == 4);
    }
    {
        Session s;
        s.remote_caps_.u.ldac.freq = OA2DP_LDAC_FREQ_44100 | OA2DP_LDAC_FREQ_48000;
        s.audio_format_.SampleRate = 96000;
        oa2dp_codec_caps cfg{};
        // An auto hint is not a new hard constraint on unsupported codecs.
        CHECK(s.select(cfg) && oa2dp_codec_sample_rate(&cfg) == 48000);
        s.settings_.sample_rate = 96000;
        CHECK(!s.select(cfg)); // explicit fixed request still fails accurately
    }
    {
        Session s;
        s.audio_format_.SampleRate = 192000; // valid PCM but not supported LDAC here
        oa2dp_codec_caps cfg{};
        CHECK(s.select(cfg) && oa2dp_codec_sample_rate(&cfg) == 48000);
        CHECK(s.setup_audio_endpoint());
        CHECK(s.preferred().SampleRate == 48000);
        s.audio_format_ = {}; // no endpoint state yet: keep established default
        CHECK(s.select(cfg) && oa2dp_codec_sample_rate(&cfg) == 48000);
    }
    for (int failure = 0; failure < 4; ++failure) {
        Session s;
        s.av_.sep_count = 1;
        s.av_.seps[0].seid = 1;
        s.av_.seps[0].state = OA2DP_SEP_STREAMING;
        s.local_seid_ = 1;
        if (failure == 1)
            s.audio_.formats_ok = false;
        if (failure == 2)
            s.audio_.latency_ok = false;
        if (failure == 3)
            s.audio_.connected_ok = false;
        CHECK(s.setup_audio_endpoint() == (failure == 0));
        CHECK(s.begin_calls == (failure == 0 ? 1u : 0u));
    }
    std::puts("production endpoint-format preference checks passed (audio IOCTLs fake)");
    return failures ? 1 : 0;
}
