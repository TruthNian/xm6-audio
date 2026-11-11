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
using DWORD = uint32_t;
constexpr DWORD WAIT_OBJECT_0 = 0, WAIT_TIMEOUT = 258;
static DWORD WaitForSingleObject(const bool *event, DWORD timeout) {
    CHECK(timeout == 0);
    return *event ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}
static DWORD now = 100;
static DWORD GetTickCount() { return now; }
static bool is_after(DWORD a, DWORD b) { return static_cast<int32_t>(a - b) >= 0; }
using OA2DP_FORMAT = oa2dp_pcm_format;
constexpr ULONG OA2DP_MAX_FORMATS = 32;
struct OA2DP_FORMAT_LIST {
    ULONG Count = 0, Preferred = 0;
    OA2DP_FORMAT Formats[OA2DP_MAX_FORMATS]{};
};
struct Session {
    struct Audio {
        unsigned publications = 0;
        unsigned failed_stage = 0, failed_attempt = 1;
        OA2DP_FORMAT_LIST formats;
        bool set_formats(const OA2DP_FORMAT_LIST &list) {
            formats = list;
            ++publications;
            return passes(1);
        }
        bool passes(unsigned stage) const {
            return stage != failed_stage || publications != failed_attempt;
        }
        unsigned delay = 0;
        bool set_latency(unsigned, unsigned value) { delay = value; return passes(2); }
        bool set_connected(bool value, ULONG) { return value && passes(3); }
    } audio_;
    oa2dp_settings settings_{};
    oa2dp_codec_caps config_{}, remote_caps_{};
    OA2DP_FORMAT audio_format_{96000, 2, 32, 32, OA2DP_PCM_FLOAT};
    bool connection_enabled_ = true, transport_suspended_ = false, suspend_requested_ = false;
    bool teardown_pending_ = false, streaming_ = false, media_open_ = false, audio_ready_ = true;
    bool stopping_ = false, publication_pending_ = false;
    bool control_cancelled_ = false;
    const bool *control_cancel_event_ = &control_cancelled_;
    bool sig_open_ = true, rx_failed_ = false;
    bool have_remote_caps_ = false, peer_configuration_pending_ = false, audio_running_ = true;
    bool rate_resume_pending_ = false;
    DWORD next_peer_configuration_retry_ = 0;
    DWORD next_profile_retry_ = 0;
    bool latency_publication_pending_ = false;
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
    std::function<void()> during_wait;
    uint8_t rejected_signal = 0;
    unsigned rejections = 0;
    static uint8_t approve(void *context, uint8_t, uint8_t signal) {
        auto &s = *static_cast<Side *>(context)->s;
        if (s.rejected_signal == signal && s.rejections) {
            --s.rejections;
            return OA2DP_AVDTP_BAD_STATE;
        }
        return 0;
    }
    static uint8_t check_config(void *context, uint8_t, const oa2dp_avdtp_caps *, uint8_t *category) {
        auto &s = *static_cast<Side *>(context)->s;
        *category = OA2DP_AVDTP_CAT_MEDIA_CODEC;
        if (s.rejected_signal == OA2DP_AVDTP_RECONFIGURE && s.rejections) {
            --s.rejections;
            return OA2DP_AVDTP_UNSUPPORTED_CONFIGURATION;
        }
        return 0;
    }
    static uint32_t clock(void *) { return now; }
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
        oa2dp_avdtp_callbacks cb{send, clock, event, nullptr, nullptr};
        oa2dp_avdtp_init(&av_, &cb, &local_side, 672);
        oa2dp_avdtp_init(&peer_, &cb, &peer_side, 672);
        peer_.cb.approve = approve;
        peer_.cb.check_config = check_config;
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
    bool wait_request(int label, oa2dp_avdtp_event &out, DWORD timeout_ms = 4000);
    bool pump(DWORD timeout) {
        if (during_wait) during_wait();
        else pump_all();
        now += timeout;
        return true;
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
    bool update_latency();
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
// Compile the production waiting method alongside the immediate helper used
// for configuration tests. Only its name changes; the body remains unchanged.
#define request wait_request
#include "signaling-request-under-test.inc"
#undef request
#include "latency-publication-under-test.inc"
#include "endpoint-formats-under-test.inc"
#include "peer-stream-procedures-under-test.inc"
#include "peer-reconciliation-under-test.inc"
// clang-format on

int main() {
    for (bool before : {false, true}) {
        Session s;
        now = 100;
        s.control_cancelled_ = before;
        s.during_wait = [&] { s.control_cancelled_ = true; };
        oa2dp_avdtp_event response{};
        const int label = oa2dp_avdtp_discover(&s.av_);
        CHECK(label >= 0);
        CHECK(!s.wait_request(label, response));
        CHECK(now == (before ? 100u : 150u)); // no wait for the 3-second core timeout
        CHECK(!s.stopping_ && s.sig_open_ && !s.rx_failed_);
        // The caller's existing teardown retires the pending transaction.
        oa2dp_avdtp_reset(&s.av_);
        s.packets.clear(); s.responses_.clear();
        s.control_cancelled_ = false; s.during_wait = {};
        CHECK(s.wait_request(oa2dp_avdtp_discover(&s.av_), response));
    }
    for (uint8_t signal : {OA2DP_AVDTP_SUSPEND, OA2DP_AVDTP_RECONFIGURE}) {
        Session s;
        now = 100;
        s.peer_configure_and_start(96000);
        s.reconcile_peer_configuration();
        CHECK(s.streaming_ && s.admissions == 1);
        s.audio_format_.SampleRate = 48000;
        s.rejected_signal = signal;
        s.rejections = 1;
        s.reconfigure_rate(48000);
        CHECK(!s.streaming_ && s.peer_configuration_pending_);
        CHECK(oa2dp_codec_sample_rate(&s.config_) == 96000);
        const auto publications = s.audio_.publications;
        now = 1099;
        s.reconcile_peer_configuration();
        CHECK(s.audio_.publications == publications && !s.streaming_);
        now = 1100;
        s.reconcile_peer_configuration(); // no second PCM or player event
        CHECK(s.streaming_ && !s.peer_configuration_pending_ && s.admissions == 2);
        CHECK(oa2dp_codec_sample_rate(&s.config_) == 48000);
    }
    {
        Session s;
        now = 100;
        s.peer_configure_and_start(96000);
        s.reconcile_peer_configuration();
        s.audio_format_.SampleRate = 48000;
        s.rejected_signal = OA2DP_AVDTP_START;
        s.rejections = 1;
        s.reconfigure_rate(48000);
        CHECK(!s.streaming_ && s.peer_configuration_pending_ && s.rate_resume_pending_);
        CHECK(oa2dp_codec_sample_rate(&s.config_) == 48000 && s.reconfigures == 1);
        now = 1100;
        s.reconcile_peer_configuration();
        CHECK(s.streaming_ && !s.rate_resume_pending_ && s.admissions == 2);
        CHECK(s.reconfigures == 1 && s.suspends == 1); // retry START, not the completed rate change
    }
    for (uint32_t latest : {44100u, 96000u}) {
        Session s;
        now = UINT32_MAX - 499;
        s.peer_configure_and_start(96000);
        s.reconcile_peer_configuration();
        s.audio_format_.SampleRate = 48000;
        s.rejected_signal = OA2DP_AVDTP_RECONFIGURE;
        s.rejections = 1;
        s.reconfigure_rate(48000);
        CHECK(!s.streaming_ && s.rate_resume_pending_);
        s.audio_format_.SampleRate = latest;
        now = 499;
        const auto publications = s.audio_.publications;
        s.reconcile_peer_configuration();
        CHECK(s.audio_.publications == publications && !s.streaming_);
        now = 500;
        s.reconcile_peer_configuration();
        CHECK(s.streaming_ && !s.rate_resume_pending_);
        CHECK(oa2dp_codec_sample_rate(&s.config_) == latest);
        CHECK(s.reconfigures == (latest == 96000 ? 0u : 1u));
    }
    {
        Session s;
        now = 100;
        s.peer_configure_and_start(96000);
        s.reconcile_peer_configuration();
        s.audio_format_.SampleRate = 48000;
        s.rejected_signal = OA2DP_AVDTP_RECONFIGURE;
        s.rejections = 1;
        s.reconfigure_rate(48000);
        s.audio_running_ = false; // player paused during the failed transition
        now = 1100;
        s.reconcile_peer_configuration();
        CHECK(!s.streaming_ && !s.starts);
        s.audio_running_ = true;
        s.reconfigure_rate(48000);
        CHECK(s.streaming_ && !s.rate_resume_pending_);
    }
    for (uint8_t boundary : {OA2DP_AVDTP_SUSPEND, OA2DP_AVDTP_RECONFIGURE}) {
        for (unsigned change = 0; change < 3; ++change) {
            Session s;
            now = 100;
            s.peer_configure_and_start(96000);
            s.reconcile_peer_configuration();
            const auto publications = s.audio_.publications;
            s.audio_format_.SampleRate = 48000;
            s.during_request = [&](uint8_t signal) {
                if (signal != boundary) return;
                if (change == 0) s.stopping_ = true;
                if (change == 1) ++s.sig_generation_;
                if (change == 2) {
                    CHECK(oa2dp_avdtp_abort(&s.peer_, s.peer_seid_) >= 0);
                    // The peer ABORT reaches us before the old response.
                    auto abort = std::move(s.packets.back());
                    s.packets.pop_back();
                    s.packets.push_front(std::move(abort));
                }
            };
            s.reconfigure_rate(48000);
            CHECK(!s.streaming_ && !s.starts && s.audio_.publications == publications);
            CHECK(oa2dp_codec_sample_rate(&s.config_) == 96000);
            if (change == 2) CHECK(!s.rate_resume_pending_);
        }
    }
    {
        Session s;
        now = 100;
        s.peer_configure_and_start(96000);
        s.reconcile_peer_configuration();
        s.have_remote_caps_ = false;
        s.audio_format_.SampleRate = 48000;
        s.during_request = [&](uint8_t signal) {
            if (signal != OA2DP_AVDTP_GET_ALL_CAPABILITIES) return;
            CHECK(oa2dp_avdtp_suspend(&s.peer_, s.peer_seid_) >= 0);
            auto pause = std::move(s.packets.back());
            s.packets.pop_back();
            s.packets.push_front(std::move(pause));
        };
        s.reconfigure_rate(48000); // peer pause crosses our capability request
        CHECK(!s.streaming_ && !s.rate_resume_pending_ && !s.starts);
        CHECK(oa2dp_codec_sample_rate(&s.config_) == 48000);
    }
    {
        Session s;
        now = 100;
        s.peer_configure_and_start(48000);
        s.reconcile_peer_configuration();
        s.suspend_stream();
        CHECK(!s.streaming_);
        const auto admitted = s.admissions;
        const int label = oa2dp_avdtp_start(&s.av_, s.local_seid_);
        CHECK(label >= 0);
        s.during_wait = [&] { s.packets.clear(); }; // peer never receives START
        oa2dp_avdtp_event result{};
        CHECK(!s.wait_request(label, result));
        CHECK(!s.streaming_ && s.admissions == admitted);
        CHECK(result.timed_out && result.signal_id == OA2DP_AVDTP_START);
        CHECK(s.responses_.empty() && now == 100 + OA2DP_AVDTP_TIMEOUT_MS);
        // The same label may be used again without consuming an old timeout.
        s.av_.next_label = static_cast<uint8_t>(label);
        const int replacement = oa2dp_avdtp_start(&s.av_, s.local_seid_);
        CHECK(replacement == label);
        s.during_wait = {};
        CHECK(s.wait_request(replacement, result) && !result.timed_out);
        CHECK(s.responses_.empty());
    }
    {
        Session s;
        now = 100;
        s.peer_configure_and_start(48000);
        s.reconcile_peer_configuration();
        s.suspend_stream();
        const auto admitted = s.admissions;
        const int label = oa2dp_avdtp_start(&s.av_, s.local_seid_);
        CHECK(label >= 0);
        oa2dp_avdtp_reset(&s.av_);
        CHECK(!s.streaming_ && s.admissions == admitted);
        CHECK(s.responses_.at(label).timed_out);
    }
    {
        Session s;
        s.audio_.failed_stage = 2;
        s.audio_.failed_attempt = 0;
        oa2dp_avdtp_event delay{};
        delay.msg_type = OA2DP_AVDTP_MSG_COMMAND;
        delay.signal_id = OA2DP_AVDTP_DELAYREPORT;
        delay.delay = 1200;
        now = 100;
        s.on_avdtp_event(delay);
        CHECK(s.latency_publication_pending_ && s.next_profile_retry_ == 1100);
        CHECK(s.delay_report_us_ == 120000 && s.audio_.delay == 120000);
        s.audio_.failed_stage = 0;
        delay.delay = 1500;
        s.on_avdtp_event(delay);
        CHECK(!s.latency_publication_pending_ && s.audio_.delay == 150000);
        s.audio_ready_ = false;
        delay.delay = 1700;
        s.on_avdtp_event(delay);
        CHECK(s.delay_report_us_ == 170000 && s.audio_.delay == 150000);
    }
    for (unsigned stage = 1; stage <= 3; ++stage) {
        Session s;
        now = 100;
        s.audio_.failed_stage = stage;
        s.peer_configure_and_start(96000);
        s.reconcile_peer_configuration();
        CHECK(!s.streaming_ && s.peer_configuration_pending_);
        CHECK(s.audio_.publications == 1);
        now = 1099;
        s.reconcile_peer_configuration();
        CHECK(s.audio_.publications == 1 && !s.streaming_);
        now = 1100;
        s.reconcile_peer_configuration(); // transient failure cleared, no new PCM/START
        CHECK(s.streaming_ && s.admissions == 1 && !s.peer_configuration_pending_);
        CHECK(s.audio_.publications == 2 && s.reconfigures == 0 && s.starts == 0);
    }
    for (unsigned stage = 1; stage <= 3; ++stage) {
        Session s;
        now = 200;
        s.audio_.failed_stage = stage;
        s.audio_.failed_attempt = 2; // failure after the codec has changed to 96 kHz
        s.peer_configure_and_start(48000);
        s.reconcile_peer_configuration();
        CHECK(oa2dp_codec_sample_rate(&s.config_) == 96000);
        CHECK(!s.streaming_ && s.peer_configuration_pending_ && s.audio_.publications == 2);
        CHECK(s.starts == 1 && s.reconfigures == 1);
        now = 1199;
        s.reconcile_peer_configuration();
        CHECK(s.audio_.publications == 2 && !s.streaming_);
        now = 1200;
        s.reconcile_peer_configuration();
        CHECK(s.streaming_ && s.admissions == 1 && !s.peer_configuration_pending_);
        CHECK(s.audio_.publications == 3 && s.starts == 1 && s.reconfigures == 1);
    }
    {
        Session s;
        now = 100;
        s.audio_.failed_stage = 1;
        s.peer_configure_and_start(96000);
        s.reconcile_peer_configuration();
        s.audio_.failed_attempt = 2; // a second failure is also rate-limited
        now = 1100;
        s.reconcile_peer_configuration();
        CHECK(!s.streaming_ && s.peer_configuration_pending_ && s.audio_.publications == 2);
        now = 2099;
        s.reconcile_peer_configuration();
        CHECK(s.audio_.publications == 2);
        now = 2100;
        s.reconcile_peer_configuration();
        CHECK(s.streaming_ && s.admissions == 1 && s.audio_.publications == 3);
    }
    {
        Session s;
        now = UINT32_MAX - 499;
        s.audio_.failed_stage = 1;
        s.peer_configure_and_start(96000);
        s.reconcile_peer_configuration();
        CHECK(!s.streaming_ && s.peer_configuration_pending_);
        now = 499;
        s.reconcile_peer_configuration();
        CHECK(!s.streaming_ && s.audio_.publications == 1);
        now = 500;
        s.reconcile_peer_configuration();
        CHECK(s.streaming_ && s.audio_.publications == 2);
    }
    {
        Session s;
        now = 100;
        s.audio_.failed_stage = 1;
        s.peer_configure_and_start(96000);
        s.reconcile_peer_configuration();
        s.audio_format_.SampleRate = 48000; // retry must use current PCM, not a cached format list
        now = 1100;
        s.reconcile_peer_configuration();
        CHECK(s.streaming_ && s.admissions == 1 && s.reconfigures == 1);
        CHECK(oa2dp_codec_sample_rate(&s.config_) == 48000);
        CHECK(s.audio_.formats.Formats[s.audio_.formats.Preferred].SampleRate == 48000);
    }
    for (unsigned policy = 0; policy < 4; ++policy) {
        Session s;
        now = 100;
        s.audio_.failed_stage = 1;
        s.peer_configure_and_start(96000);
        s.reconcile_peer_configuration();
        if (policy == 0)
            s.connection_enabled_ = false;
        if (policy == 1)
            s.stopping_ = true;
        if (policy >= 2) {
            CHECK((policy == 2 ? oa2dp_avdtp_suspend(&s.peer_, s.peer_seid_)
                               : oa2dp_avdtp_close(&s.peer_, s.peer_seid_)) >= 0);
            s.pump_all();
        }
        now = 1100;
        s.reconcile_peer_configuration();
        CHECK(!s.streaming_ && s.admissions == 0 && s.starts == 0 && s.reconfigures == 0);
        CHECK(s.audio_.publications == (policy == 2 ? 2u : 1u));
    }
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
