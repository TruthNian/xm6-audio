/* SPDX-License-Identifier: MIT */
/* Production session configuration/format procedures and two real AVDTP cores.
 * Audio IOCTLs and encoder admission are fake; no hardware acceptance implied. */
#include "oa2dp/a2dp_codecs.h"
#include "oa2dp/avdtp.h"
#include "oa2dp/pcm_format.h"
#include "oa2dp/settings.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <vector>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                   \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)
#define LOGE(...) ((void)0)
#define LOGW(...) ((void)0)
#define LOGI(...) ((void)0)
using ULONG = uint32_t;
using USHORT = uint16_t;
using OA2DP_FORMAT = oa2dp_pcm_format;
constexpr ULONG OA2DP_MAX_FORMATS = 32;
struct OA2DP_FORMAT_LIST {
    ULONG Count = 0, Preferred = 0;
    OA2DP_FORMAT Formats[OA2DP_MAX_FORMATS]{};
};
struct Session {
    struct Audio {
        unsigned publications = 0;
        OA2DP_FORMAT_LIST formats;
        bool set_formats(const OA2DP_FORMAT_LIST &list) {
            formats = list;
            ++publications;
            return true;
        }
        bool set_latency(unsigned, unsigned) { return true; }
        bool set_connected(bool value, ULONG) { return value; }
    } audio_;
    oa2dp_settings settings_{};
    oa2dp_codec_caps config_{}, remote_caps_{};
    OA2DP_FORMAT audio_format_{96000, 2, 32, 32, OA2DP_PCM_FLOAT};
    bool connection_enabled_ = true, transport_suspended_ = false, suspend_requested_ = false;
    bool teardown_pending_ = false, streaming_ = false, media_open_ = false, audio_ready_ = true;
    bool stopping_ = false, publication_pending_ = false;
    bool sig_open_ = true, rx_failed_ = false;
    bool have_remote_caps_ = false, peer_configuration_pending_ = false, audio_running_ = true;
    uint32_t delay_report_us_ = 0, init_deadline_ = 0;
    ULONG sig_generation_ = 1;
    oa2dp_avdtp av_{}, peer_{};
    uint8_t local_seid_ = 0, peer_seid_ = 0;
    std::map<int, oa2dp_avdtp_event> responses_;
    struct Packet {
        bool to_peer;
        std::vector<uint8_t> bytes;
    };
    std::deque<Packet> packets;
    struct Side {
        Session *s;
        bool is_peer;
    } local_side{this, false}, peer_side{this, true};
    unsigned admissions = 0, starts = 0, suspends = 0, reconfigures = 0;
    std::function<void(uint8_t)> during_request;
    static int send(void *context, const uint8_t *p, size_t n) {
        auto &side = *static_cast<Side *>(context);
        side.s->packets.push_back({!side.is_peer, {p, p + n}});
        return 0;
    }
    static void event(void *context, const oa2dp_avdtp_event *ev) {
        auto &side = *static_cast<Side *>(context);
        if (!side.is_peer)
            side.s->on_avdtp_event(*ev);
        else if (ev->msg_type == OA2DP_AVDTP_MSG_COMMAND) {
            if (ev->signal_id == OA2DP_AVDTP_START)
                ++side.s->starts;
            if (ev->signal_id == OA2DP_AVDTP_SUSPEND)
                ++side.s->suspends;
            if (ev->signal_id == OA2DP_AVDTP_RECONFIGURE)
                ++side.s->reconfigures;
        }
    }
    Session() {
        oa2dp_settings_default(&settings_);
        oa2dp_avdtp_callbacks cb{send, nullptr, event, nullptr, nullptr};
        oa2dp_avdtp_init(&av_, &cb, &local_side, 672);
        oa2dp_avdtp_init(&peer_, &cb, &peer_side, 672);
        oa2dp_codec_caps codec{};
        oa2dp_codec_local_caps(OA2DP_CODEC_LDAC, &codec);
        auto caps = make_config_caps(codec, true);
        CHECK(oa2dp_avdtp_add_sep(&av_, OA2DP_AVDTP_TSEP_SRC, OA2DP_MEDIA_TYPE_AUDIO, &caps) == 1);
        CHECK(oa2dp_avdtp_add_sep(&peer_, OA2DP_AVDTP_TSEP_SNK, OA2DP_MEDIA_TYPE_AUDIO, &caps) ==
              1);
        peer_seid_ = 1;
    }
    static oa2dp_avdtp_caps make_config_caps(const oa2dp_codec_caps &codec, bool delay) {
        oa2dp_avdtp_caps caps{};
        caps.media_transport = caps.media_codec = true;
        caps.delay_reporting = delay;
        const int n = oa2dp_codec_build(&codec, caps.codec, sizeof(caps.codec));
        CHECK(n > 0);
        caps.codec_len = static_cast<uint8_t>(n);
        return caps;
    }
    void pump_all() {
        unsigned limit = 100;
        while (!packets.empty()) {
            CHECK(limit-- > 0);
            auto p = std::move(packets.front());
            packets.pop_front();
            CHECK(oa2dp_avdtp_input(p.to_peer ? &peer_ : &av_, p.bytes.data(), p.bytes.size()) ==
                  OA2DP_OK);
        }
    }
    bool request(int label, oa2dp_avdtp_event &out) {
        if (label < 0)
            return false;
        if (during_request)
            during_request(av_.pending[label].signal_id);
        pump_all();
        auto it = responses_.find(label);
        CHECK(it != responses_.end());
        out = it->second;
        responses_.erase(it);
        return out.msg_type == OA2DP_AVDTP_MSG_ACCEPT;
    }
    oa2dp_avdtp_caps selected(uint32_t rate) {
        oa2dp_codec_caps codec{}, caps{};
        oa2dp_codec_local_caps(OA2DP_CODEC_LDAC, &caps);
        auto settings = settings_;
        settings.sample_rate = rate;
        CHECK(oa2dp_codec_select(nullptr, &caps, &settings, &codec) == OA2DP_OK);
        return make_config_caps(codec, false);
    }
    void peer_configure_and_start(uint32_t rate) {
        auto caps = selected(rate);
        CHECK(oa2dp_avdtp_set_configuration(&peer_, peer_seid_, 1, &caps) >= 0);
        pump_all();
        CHECK(oa2dp_avdtp_open(&peer_, peer_seid_) >= 0);
        pump_all();
        media_open_ = true; // physical media channel is ready, before deferred setup
        CHECK(oa2dp_avdtp_start(&peer_, peer_seid_) >= 0);
        pump_all();
    }
    bool ensure_audio_endpoint() { return audio_ready_; }
    bool update_suspend_state() { return true; }
    void publish(const char *) {}
    void begin_streaming() {
        if (stopping_ || peer_configuration_pending_ || !connection_enabled_ ||
            transport_suspended_ || suspend_requested_ || teardown_pending_ || streaming_ ||
            !media_open_ || !audio_ready_ ||
            audio_format_.SampleRate != oa2dp_codec_sample_rate(&config_))
            return;
        streaming_ = true;
        ++admissions;
    }
    void end_streaming() { streaming_ = false; }
    void on_avdtp_event(const oa2dp_avdtp_event &ev);
    bool setup_audio_endpoint();
    bool start_stream();
    void suspend_stream();
    void reconfigure_rate(uint32_t rate);
    void reconcile_peer_configuration();
};
// clang-format off
#include "peer-events-under-test.inc"
#include "endpoint-formats-under-test.inc"
#include "peer-stream-procedures-under-test.inc"
#include "peer-reconciliation-under-test.inc"
// clang-format on

int main() {
    for (auto rates : {std::pair<uint32_t, uint32_t>{96000, 48000},
                       {48000, 96000},
                       {44100, 48000},
                       {88200, 96000}}) {
        Session s;
        s.audio_format_.SampleRate = rates.first;
        s.peer_configure_and_start(rates.second);
        CHECK(!s.streaming_);
        s.reconcile_peer_configuration(); // no new PCM/player event
        CHECK(oa2dp_codec_sample_rate(&s.config_) == rates.first);
        CHECK(s.streaming_ && s.admissions == 1);
        CHECK(s.reconfigures == 1 && s.suspends == 1 && s.starts == 1);
        auto peer = oa2dp_avdtp_sep_get(&s.peer_, s.peer_seid_);
        oa2dp_codec_caps cfg{};
        CHECK(oa2dp_codec_parse(peer->config.codec, peer->config.codec_len, &cfg) == OA2DP_OK);
        CHECK(oa2dp_codec_sample_rate(&cfg) == rates.first);
        CHECK(s.audio_format_.SampleRate == rates.first &&
              s.audio_format_.Encoding == OA2DP_PCM_FLOAT && s.audio_format_.BitsPerSample == 32 &&
              s.audio_format_.ValidBitsPerSample == 32);
        const auto publications = s.audio_.publications;
        s.reconcile_peer_configuration();
        CHECK(s.audio_.publications == publications && s.admissions == 1 && s.reconfigures == 1);
    }
    {
        Session s;
        s.peer_configure_and_start(96000);
        CHECK(!s.streaming_); // peer START is deferred until complete endpoint publication
        s.reconcile_peer_configuration();
        CHECK(s.streaming_ && s.admissions == 1);
        CHECK(s.starts == 0 && s.suspends == 0 && s.reconfigures == 0);
    }
    {
        Session s;
        s.peer_configure_and_start(48000);
        s.media_open_ = false; // opposite cross-channel notification order
        s.reconcile_peer_configuration();
        CHECK(s.peer_configuration_pending_ && !s.have_remote_caps_ && !s.audio_.publications);
        s.media_open_ = true;
        s.reconcile_peer_configuration();
        CHECK(s.streaming_ && s.admissions == 1 && s.reconfigures == 1);
    }
    {
        Session s;
        s.audio_format_.SampleRate = 48000;
        s.peer_configure_and_start(48000);
        s.reconcile_peer_configuration();
        CHECK(s.streaming_ && s.admissions == 1);
        CHECK(oa2dp_avdtp_suspend(&s.peer_, s.peer_seid_) >= 0);
        s.pump_all();
        auto caps = s.selected(96000);
        CHECK(oa2dp_avdtp_reconfigure(&s.peer_, s.peer_seid_, &caps) >= 0);
        s.pump_all();
        CHECK(s.peer_configuration_pending_ && !s.streaming_);
        CHECK(oa2dp_avdtp_start(&s.peer_, s.peer_seid_) >= 0);
        s.pump_all();
        CHECK(!s.streaming_ && s.admissions == 1);
        s.reconcile_peer_configuration();
        CHECK(s.streaming_ && s.admissions == 2 && s.reconfigures == 1);
        CHECK(oa2dp_codec_sample_rate(&s.config_) == 48000);
    }
    for (unsigned gate = 0; gate < 9; ++gate) {
        Session s;
        s.peer_configure_and_start(48000);
        switch (gate) {
        case 0:
            s.stopping_ = true;
            break;
        case 1:
            s.connection_enabled_ = false;
            break;
        case 2:
            s.transport_suspended_ = true;
            break;
        case 3:
            s.teardown_pending_ = true;
            break;
        case 4:
            s.audio_ready_ = false;
            break;
        case 5:
            s.local_seid_ = 0;
            break;
        case 6:
            oa2dp_avdtp_sep_get(&s.av_, s.local_seid_)->state = OA2DP_SEP_CLOSING;
            break;
        case 7:
            s.sig_open_ = false;
            break;
        case 8:
            s.rx_failed_ = true;
            break;
        }
        s.reconcile_peer_configuration();
        CHECK(!s.streaming_ && s.audio_.publications == 0 && s.reconfigures == 0);
    }
    {
        Session s;
        s.peer_configure_and_start(48000);
        s.suspend_requested_ = true;
        s.reconcile_peer_configuration();
        CHECK(!s.streaming_ && s.reconfigures == 0);
        CHECK(s.audio_.formats.Formats[s.audio_.formats.Preferred].SampleRate == 96000);
    }
    for (unsigned change = 0; change < 4; ++change) {
        Session s;
        s.peer_configure_and_start(48000);
        s.during_request = [&](uint8_t signal) {
            if (signal != OA2DP_AVDTP_GET_ALL_CAPABILITIES)
                return;
            switch (change) {
            case 0:
                s.stopping_ = true;
                break;
            case 1:
                s.connection_enabled_ = false;
                break;
            case 2:
                ++s.sig_generation_;
                break;
            case 3:
                CHECK(oa2dp_avdtp_close(&s.peer_, s.peer_seid_) >= 0);
                break;
            }
        };
        s.reconcile_peer_configuration();
        CHECK(!s.streaming_ && s.audio_.publications == 0 && s.reconfigures == 0);
    }
    {
        Session s;
        oa2dp_codec_caps peer_caps{};
        oa2dp_codec_local_caps(OA2DP_CODEC_LDAC, &peer_caps);
        peer_caps.u.ldac.freq = OA2DP_LDAC_FREQ_48000;
        oa2dp_avdtp_sep_get(&s.peer_, s.peer_seid_)->caps = s.make_config_caps(peer_caps, true);
        s.peer_configure_and_start(48000);
        s.reconcile_peer_configuration();
        CHECK(!s.streaming_ && s.reconfigures == 0); // unsupported PCM cannot be relabelled
        CHECK(s.audio_.formats.Count == 5);
        CHECK(s.audio_.formats.Formats[s.audio_.formats.Preferred].SampleRate == 48000);
    }
    for (bool fixed : {false, true}) {
        Session s;
        if (fixed)
            s.settings_.sample_rate = 48000;
        else
            s.settings_.follow_source_rate = false;
        s.peer_configure_and_start(48000);
        s.reconcile_peer_configuration();
        CHECK(s.reconfigures == 0 && oa2dp_codec_sample_rate(&s.config_) == 48000);
        CHECK(s.audio_.formats.Formats[s.audio_.formats.Preferred].SampleRate == 48000);
    }
    std::puts("production peer-configuration rate checks passed (audio IOCTLs/admission fake)");
}
