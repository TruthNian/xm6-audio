// SPDX-License-Identifier: MIT
#include "session.h"
#include "../../driver/common/channel_lifecycle.h"

#include <bluetoothapis.h>
#include <avrt.h>

#include <algorithm>
#include <cstring>

#include "log.h"
#include "stream_trace.h"
#include "media_clock.h"
#include "pcm_feed.h"
#include "oa2dp/encoder.h"
#include "oa2dp/media.h"
#include "oa2dp/pcm_ring.h"

namespace oa2dp {
namespace {

constexpr DWORD kPeerInitiatorGraceMs = 1500; // let the headset initiate signalling/configuration first
constexpr DWORD kIdleSuspendMs = 3000;      // SUSPEND after this much silence
constexpr DWORD kReconnectPollMs = 5000;
constexpr uint32_t kAbrHighWater = 6;       // queued media packets
constexpr uint32_t kAbrLowWater = 2;
constexpr uint32_t kAbrDownHoldMs = 300;
constexpr uint32_t kAbrUpHoldMs = 5000;

bool is_after(DWORD now, DWORD deadline) { return static_cast<LONG>(now - deadline) >= 0; }

// Register and release on the streaming thread, including error returns.
class AudioScheduling {
public:
    AudioScheduling()
    {
        DWORD index = 0;
        task_ = AvSetMmThreadCharacteristicsW(L"Audio", &index);
        if (!task_)
            LOGE("MMCSS Audio registration failed (%lu); using normal thread scheduling", GetLastError());
    }
    ~AudioScheduling()
    {
        if (task_)
            AvRevertMmThreadCharacteristics(task_);
    }
    AudioScheduling(const AudioScheduling &) = delete;
    AudioScheduling &operator=(const AudioScheduling &) = delete;
private:
    HANDLE task_ = nullptr;
};

oa2dp_avdtp_caps make_config_caps(const oa2dp_codec_caps &cfg, bool delay_reporting)
{
    oa2dp_avdtp_caps c{};
    uint8_t ie[OA2DP_CODEC_IE_MAX];
    int n = oa2dp_codec_build(&cfg, ie, sizeof(ie));
    c.media_transport = true;
    c.delay_reporting = delay_reporting;
    if (n > 0) {
        c.media_codec = true;
        c.codec_len = static_cast<uint8_t>(n);
        std::memcpy(c.codec, ie, static_cast<size_t>(n));
    }
    return c;
}

std::string codec_label(const oa2dp_codec_caps &c) { return oa2dp_codec_name(c.codec); }

}  // namespace

Session::Session(std::wstring transport_path) : path_(std::move(transport_path))
{
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    control_cancel_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stream_stop_event_ = CreateEventW(nullptr, TRUE, TRUE, nullptr);
}

Session::~Session()
{
    stop();
    if (stop_event_)
        CloseHandle(stop_event_);
    if (control_cancel_event_)
        CloseHandle(control_cancel_event_);
    if (wake_event_)
        CloseHandle(wake_event_);
    if (stream_stop_event_) CloseHandle(stream_stop_event_);
}

void Session::start()
{
    if (!stop_event_ || !control_cancel_event_ || !wake_event_ || !stream_stop_event_) {
        LOGE("cannot create session synchronization events");
        return;
    }
    main_ = std::thread([this] { main_loop(); });
}

void Session::stop()
{
    {
        std::lock_guard<std::mutex> lock(mu_);
        stopping_ = true;
        SetEvent(stop_event_);
        SetEvent(control_cancel_event_);
    }
    SetEvent(stream_stop_event_);
    tr_.cancel_all();
    audio_.cancel_all();
    if (main_.joinable())
        main_.join();
}

void Session::post(const Command &c)
{
    {
        std::lock_guard<std::mutex> lock(mu_);
        cmd_q_.push_back(c);
        if (c.cmd == Cmd::AudioFormat) {
            // The main thread may be inside OPEN or an AVDTP request. Wake
            // that I/O now; only the main thread handles protocol teardown.
            // Reconnect can clear this transient policy, but never stop().
            if (stopping_ || !c.format.ConnectionEnabled)
                SetEvent(control_cancel_event_);
            else
                ResetEvent(control_cancel_event_);
        }
    }
    SetEvent(wake_event_);
}

// ---------------------------------------------------------------------------
// Helper threads
// ---------------------------------------------------------------------------

void Session::event_loop()
{
    while (!stopping_) {
        OA2DP_EVENT ev{};
        if (!tr_.wait_event(ev, stop_event_)) {
            WaitForSingleObject(stop_event_, 100);
            continue;
        }
        Command c{};
        c.channel = ev.Channel;
        switch (ev.Type) {
        case Oa2dpEventChannelOpened:
            c.cmd = ev.Kind == Oa2dpChannelMedia ? Cmd::MediaOpenedByPeer
                                                 : Cmd::SignalingOpenedByPeer;
            break;
        case Oa2dpEventChannelClosed:
            c.cmd = ev.Kind == Oa2dpChannelMedia ? Cmd::MediaClosed : Cmd::SignalingClosed;
            break;
        case Oa2dpEventDeviceGone:
            c.cmd = Cmd::DeviceGone;
            break;
        case Oa2dpEventStateLost:
            LOGE("Bluetooth event backlog overflow; reconnecting to recover state");
            c.cmd = Cmd::StreamFailed;
            break;
        case Oa2dpEventPowerChanged:
            c.cmd = Cmd::TransportPower;
            break;
        case Oa2dpEventLinkAvailable:
            c.cmd = Cmd::TransportAvailable;
            break;
        default:
            continue;
        }
        post(c);
    }
}

void Session::audio_loop()
{
    while (!stopping_) {
        if (!audio_ready_) {
            WaitForSingleObject(stop_event_, 100);
            continue;
        }
        Command c{};
        c.cmd = Cmd::AudioFormat;
        c.error = audio_.wait_format(c.format, stop_event_);
        if (c.error == ERROR_SUCCESS) {
            post(c);
        } else if (!stopping_) {
            // Do not touch this handle again until the main thread has joined
            // the encoder and released the old mapping/handle for PnP removal.
            audio_wait_failed_ = true;
            c.cmd = Cmd::AudioUnavailable;
            post(c);
            while (audio_wait_failed_ && !stopping_)
                WaitForSingleObject(stop_event_, 100);
        }
    }
}

// ---------------------------------------------------------------------------
// AVDTP plumbing (main thread)
// ---------------------------------------------------------------------------

int Session::cb_send(void *user, const uint8_t *pdu, size_t len)
{
    auto *s = static_cast<Session *>(user);
    const OA2DP_SEND_IN binding{Oa2dpChannelSignaling, s->sig_generation_, s->sig_generation_};
    return s->tr_.send(binding, pdu, len, s->control_cancel_event_) ? 0 : -1;
}

uint32_t Session::cb_now(void *) { return GetTickCount(); }

void Session::cb_event(void *user, const oa2dp_avdtp_event *ev)
{
    static_cast<Session *>(user)->on_avdtp_event(*ev);
}

uint8_t Session::cb_check(void *user, uint8_t seid, const oa2dp_avdtp_caps *cfg, uint8_t *cat)
{
    auto *s = static_cast<Session *>(user);
    *cat = OA2DP_AVDTP_CAT_MEDIA_CODEC;
    if (!s->connection_enabled_)
        return OA2DP_AVDTP_BAD_STATE;
    auto it = s->sep_codec_.find(seid);
    oa2dp_codec_caps want{}, local{};
    if (it == s->sep_codec_.end() || oa2dp_codec_parse(cfg->codec, cfg->codec_len, &want) != OA2DP_OK)
        return OA2DP_A2DP_NOT_SUPPORTED_CODEC_TYPE;
    oa2dp_codec_local_caps(it->second, &local);
    const uint8_t capability_error = oa2dp_codec_check_config(&local, &want);
    return capability_error ? capability_error : oa2dp_codec_check_settings(&want, &s->settings_);
}

bool Session::pump(DWORD timeout_ms)
{
    if (!sig_open_ || rx_failed_ || stopping_ || transport_suspended_ ||
        WaitForSingleObject(control_cancel_event_, 0) == WAIT_OBJECT_0)
        return false;
    uint8_t pdu[OA2DP_AVDTP_MSG_MAX + 3];
    const OA2DP_RECV_IN binding{Oa2dpChannelSignaling, sig_generation_, sig_generation_};
    int n = tr_.recv(binding, pdu, sizeof(pdu), timeout_ms, control_cancel_event_);
    if (n <= 0) {
        if (n < 0 && !stopping_) {
            rx_failed_ = true;
            Command command{Cmd::SignalingClosed};
            command.channel.Generation = sig_generation_;
            command.channel.ConnectionGeneration = sig_generation_;
            post(command);
        }
        return false;
    }
    // Receive, protocol state and reconnect are serialized. No old receive
    // worker or queued PDU can outlive a channel and affect its replacement.
    oa2dp_avdtp_input(&av_, pdu, static_cast<size_t>(n));
    return true;
}

bool Session::request(int label, oa2dp_avdtp_event &out, DWORD timeout_ms)
{
    if (label < 0)
        return false;
    DWORD deadline = GetTickCount() + timeout_ms;
    while (!stopping_ && sig_open_ && !rx_failed_ &&
           WaitForSingleObject(control_cancel_event_, 0) != WAIT_OBJECT_0) {
        // Core timeouts are completions, not peer commands. Deliver and
        // consume them before the caller's outer deadline can leave one
        // behind for a reused transaction label.
        oa2dp_avdtp_check_timeouts(&av_);
        auto it = responses_.find(label);
        if (it != responses_.end()) {
            out = it->second;
            responses_.erase(it);
            return !out.timed_out && out.msg_type == OA2DP_AVDTP_MSG_ACCEPT;
        }
        if (is_after(GetTickCount(), deadline)) {
            out = {};
            out.timed_out = true;
            return false;
        }
        pump(50);
    }
    return false;
}

void Session::on_avdtp_event(const oa2dp_avdtp_event &ev)
{
    if (ev.timed_out || ev.msg_type != OA2DP_AVDTP_MSG_COMMAND) {
        responses_[ev.label] = ev;
        return;
    }
    // Commands the headset sent and the core already accepted.
    switch (ev.signal_id) {
    case OA2DP_AVDTP_SET_CONFIGURATION:
        if (oa2dp_codec_parse(ev.caps.codec, ev.caps.codec_len, &config_) == OA2DP_OK) {
            rate_resume_pending_ = false;
            local_seid_ = ev.local_seid;
            have_remote_caps_ = false;
            peer_configuration_pending_ = true;
            next_peer_configuration_retry_ = GetTickCount();
            init_deadline_ = 0;
            LOGI("headset configured %s at %u Hz", oa2dp_codec_name(config_.codec),
                 oa2dp_codec_sample_rate(&config_));
            publish("configured");
        }
        break;
    case OA2DP_AVDTP_RECONFIGURE:
        if (ev.caps.media_codec &&
            oa2dp_codec_parse(ev.caps.codec, ev.caps.codec_len, &config_) == OA2DP_OK) {
            rate_resume_pending_ = false;
            peer_configuration_pending_ = true;
            next_peer_configuration_retry_ = GetTickCount();
        }
        break;
    case OA2DP_AVDTP_START:
        rate_resume_pending_ = false;
        begin_streaming();
        break;
    case OA2DP_AVDTP_SUSPEND:
    case OA2DP_AVDTP_CLOSE:
    case OA2DP_AVDTP_ABORT:
        rate_resume_pending_ = false;
        end_streaming();
        update_suspend_state();
        break;
    case OA2DP_AVDTP_DELAYREPORT:
        delay_report_us_ = static_cast<uint32_t>(ev.delay) * 100;
        if (audio_ready_)
            update_latency();
        LOGI("headset delay report: %u us", delay_report_us_.load());
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Procedures (main thread)
// ---------------------------------------------------------------------------

bool Session::connect_signaling()
{
    OA2DP_CHANNEL_INFO ci{};
    if (!tr_.open_channel(Oa2dpChannelSignaling, ci, 0, control_cancel_event_)) {
        teardown("signalling open failed");
        return false;
    }
    sig_generation_ = ci.ConnectionGeneration;
    sig_mtu_ = ci.OutMtu;
    av_.out_mtu = sig_mtu_ < 48 ? 48 : sig_mtu_;
    sig_open_ = true;
    rx_failed_ = false;
    LOGI("signalling channel open (MTU %u)", sig_mtu_);
    return true;
}

bool Session::open_media_channel()
{
    if (media_open_)
        return true;
    OA2DP_CHANNEL_INFO ci{};
    if (!tr_.open_channel(Oa2dpChannelMedia, ci, sig_generation_, control_cancel_event_))
        return false;
    media_generation_ = ci.Generation;
    media_mtu_ = ci.OutMtu;
    media_open_ = true;
    LOGI("media channel open (MTU %u)", media_mtu_);
    return true;
}

bool Session::configure_as_initiator()
{
    oa2dp_avdtp_event ev{};
    if (!request(oa2dp_avdtp_discover(&av_), ev)) {
        LOGW("DISCOVER failed");
        return false;
    }

    struct Candidate {
        uint8_t seid;
        oa2dp_codec_caps caps;
        bool delay_reporting;
    };
    std::vector<Candidate> found;
    for (uint8_t i = 0; i < ev.sep_count; i++) {
        const auto &sep = ev.seps[i];
        if (sep.in_use || sep.tsep != OA2DP_AVDTP_TSEP_SNK || sep.media_type != OA2DP_MEDIA_TYPE_AUDIO)
            continue;
        oa2dp_avdtp_event caps{};
        bool ok = request(oa2dp_avdtp_get_capabilities(&av_, sep.seid, true), caps);
        if (!ok && caps.msg_type == OA2DP_AVDTP_MSG_GENERAL_REJECT)  // AVDTP 1.0 peer
            ok = request(oa2dp_avdtp_get_capabilities(&av_, sep.seid, false), caps);
        Candidate c{};
        c.seid = sep.seid;
        c.delay_reporting = caps.caps.delay_reporting;
        if (ok && caps.caps.media_codec &&
            oa2dp_codec_parse(caps.caps.codec, caps.caps.codec_len, &c.caps) == OA2DP_OK) {
            LOGI("headset SEP %u: %s", sep.seid, oa2dp_codec_name(c.caps.codec));
            found.push_back(c);
        }
    }

    // Try codecs in the user's priority order; fall back on rejection.
    for (uint8_t p = 0; p < settings_.priority_count; p++) {
        oa2dp_codec_id want = settings_.priority[p];
        if (!oa2dp_encoder_available(want))
            continue;
        uint8_t local = 0;
        for (const auto &kv : sep_codec_)
            if (kv.second == want)
                local = kv.first;
        for (const auto &c : found) {
            if (c.caps.codec != want || !local)
                continue;
            oa2dp_codec_caps cfg{};
            oa2dp_settings selection = settings_;
            // Auto means follow the actual endpoint PCM, including an already
            // running stream. Do not wait for a second format event to undo
            // the generic 48 kHz negotiation default. This is a preference:
            // original explicit constraints still decide acceptance below.
            if (settings_.follow_source_rate && !settings_.sample_rate &&
                oa2dp_pcm_format_valid(&audio_format_))
                selection.sample_rate = audio_format_.SampleRate;
            if (oa2dp_codec_select(nullptr, &c.caps, &selection, &cfg) != OA2DP_OK ||
                oa2dp_codec_check_settings(&cfg, &settings_) != 0)
                continue;
            oa2dp_avdtp_caps avc = make_config_caps(cfg, c.delay_reporting);
            oa2dp_avdtp_event r{};
            if (!request(oa2dp_avdtp_set_configuration(&av_, local, c.seid, &avc), r)) {
                LOGW("%s rejected by headset (error 0x%02X)", oa2dp_codec_name(want), r.error_code);
                continue;
            }
            local_seid_ = local;
            config_ = cfg;
            remote_caps_ = c.caps;
            have_remote_caps_ = true;
            LOGI("configured %s, %u Hz, %u ch", oa2dp_codec_name(want),
                 oa2dp_codec_sample_rate(&cfg), oa2dp_codec_channels(&cfg));
            if (!request(oa2dp_avdtp_open(&av_, local), r)) {
                LOGW("OPEN failed");
                request(oa2dp_avdtp_abort(&av_, local), r);
                return false;
            }
            publish("configured");
            return open_media_channel() && setup_audio_endpoint();
        }
    }
    LOGE("no common codec with the headset");
    return false;
}

bool Session::ensure_audio_endpoint()
{
    if (!audio_ready_) {
        bool opened = false;
        for (int i = 0; i < 25 && !opened && !stopping_; i++) {  // child may still be starting
            opened = audio_.open(info_.RemoteAddress);
            if (!opened)
                WaitForSingleObject(stop_event_, 200);
        }
        if (!opened) {
            LOGE("audio endpoint for this headset not found (is oa2dpaudio.sys installed?)");
            return false;
        }
        if (!audio_.set_name(name_.empty() ? L"Bluetooth Headphones" : name_)) {
            LOGW("cannot publish the native endpoint name; will retry before mapping PCM");
            audio_.close();
            return false;
        }
        if (!audio_.map_ring())
            return false;
        OA2DP_FORMAT_CHANGE state{};
        if (!audio_.get_state(state)) {
            audio_.close();
            return false;
        }
        connection_enabled_ = state.ConnectionEnabled != 0;
        suspend_requested_ = state.SuspendRequested != 0;
        suspend_serial_ = state.SuspendSerial;
        connection_serial_ = state.ConnectionSerial;
        audio_format_ = state.Format;
        pcm_sequence_ = state.PcmSequence;
        audio_running_ = state.Running != 0;
        {
            // Initialize from the reopened endpoint before its waiter can
            // deliver newer snapshots. Do not retain an old disconnect latch.
            std::lock_guard<std::mutex> lock(mu_);
            if (stopping_ || !state.ConnectionEnabled)
                SetEvent(control_cancel_event_);
            else
                ResetEvent(control_cancel_event_);
        }
        audio_ready_ = true;
    }
    return true;
}

bool Session::setup_audio_endpoint()
{
    if (!ensure_audio_endpoint() || !connection_enabled_)
        return false;
    auto retry_setup = [this] {
        peer_configuration_pending_ = true;
        next_peer_configuration_retry_ = GetTickCount() + 1000;
        return false;
    };

    // Offer every rate the headset accepts for this codec, so Windows can
    // switch rate and we follow with RECONFIGURE instead of resampling.
    OA2DP_FORMAT_LIST list{};
    const uint32_t current = oa2dp_codec_sample_rate(&config_);
    static const uint32_t rates[] = {44100, 48000, 88200, 96000, 32000, 16000};
    for (uint32_t rate : rates) {
        if (list.Count >= OA2DP_MAX_FORMATS)
            break;
        oa2dp_codec_caps probe = config_;
        bool ok = rate == current ||
                  (have_remote_caps_ && settings_.follow_source_rate && !settings_.sample_rate &&
                   oa2dp_codec_set_rate(&probe, &remote_caps_, rate) == OA2DP_OK);
        if (!ok)
            continue;
        static const OA2DP_FORMAT representations[] = {
            {0, 0, 32, 32, OA2DP_PCM_FLOAT}, {0, 0, 32, 32, OA2DP_PCM_INTEGER},
            {0, 0, 32, 24, OA2DP_PCM_INTEGER},
            {0, 0, 24, 24, OA2DP_PCM_INTEGER}, {0, 0, 16, 16, OA2DP_PCM_INTEGER}};
        for (const OA2DP_FORMAT &representation : representations) {
            USHORT depth = representation.BitsPerSample;
            if (settings_.bits && representation.ValidBitsPerSample != settings_.bits)
                continue;
            if (list.Count >= OA2DP_MAX_FORMATS)
                break;
            if (rate == current &&
                (settings_.bits ? depth == settings_.bits : representation.Encoding == OA2DP_PCM_FLOAT))
                list.Preferred = list.Count;
            OA2DP_FORMAT &f = list.Formats[list.Count++];
            f = representation;
            f.SampleRate = rate;
            f.Channels = oa2dp_codec_channels(&config_);
            f.BitsPerSample = depth;
        }
    }
    // Preserve a supported current endpoint format in auto mode. Otherwise a
    // peer-selected codec rate could become Windows' preferred format and
    // reverse the intended direction of rate following. Keep other supported
    // rates/representations available for later Windows/exclusive clients.
    if (settings_.follow_source_rate && !settings_.sample_rate) {
        for (ULONG i = 0; i < list.Count; ++i) {
            if (oa2dp_pcm_format_same(&list.Formats[i], &audio_format_)) {
                list.Preferred = i;
                break;
            }
        }
    }
    if (!audio_.set_formats(list) || !update_latency()) {
        LOGE("cannot configure the native audio endpoint");
        return retry_setup();
    }
    if (!audio_.set_connected(true, sig_generation_)) return retry_setup();
    peer_configuration_pending_ = false;
    next_peer_configuration_retry_ = 0;
    publication_pending_ = false; // an old FALSE must not follow this new link
    // A newer HFP request can arrive while configuring. Its policy update
    // follows through WAIT_FORMAT; preserve this valid AVDTP configuration.
    update_suspend_state();
    // Signalling can deliver an accepted START before the event thread posts
    // media readiness. Reconcile that existing request after setup succeeds;
    // an unchanged, already-running PCM stream need not send another event.
    oa2dp_avdtp_sep *sep = oa2dp_avdtp_sep_get(&av_, local_seid_);
    if (sep && sep->state == OA2DP_SEP_STREAMING)
        begin_streaming();
    return true;
}

bool Session::start_stream()
{
    oa2dp_avdtp_sep *sep = oa2dp_avdtp_sep_get(&av_, local_seid_);
    if (!connection_enabled_ || transport_suspended_ || suspend_requested_ || teardown_pending_ || !sep || !media_open_)
        return false;
    if (sep->state == OA2DP_SEP_STREAMING) {
        begin_streaming();
        return streaming_;
    }
    if (sep->state != OA2DP_SEP_OPEN)
        return false;
    oa2dp_avdtp_event r{};
    if (!request(oa2dp_avdtp_start(&av_, local_seid_), r)) {
        LOGW("START rejected (0x%02X)", r.error_code);
        return false;
    }
    begin_streaming();
    return streaming_;
}

void Session::suspend_stream()
{
    oa2dp_avdtp_sep *sep = oa2dp_avdtp_sep_get(&av_, local_seid_);
    if (!sep || sep->state != OA2DP_SEP_STREAMING)
        return;
    end_streaming();
    oa2dp_avdtp_event r{};
    if (!request(oa2dp_avdtp_suspend(&av_, local_seid_), r))
        LOGW("SUSPEND failed");
    else
        publish("suspended");
    update_suspend_state();
}

void Session::reconfigure_rate(uint32_t rate)
{
    if (settings_.sample_rate && rate != settings_.sample_rate)
        return;
    oa2dp_avdtp_sep *sep = oa2dp_avdtp_sep_get(&av_, local_seid_);
    if (!sep)
        return;
    const uint8_t seid = local_seid_, remote = sep->remote_seid;
    const ULONG connection = sig_generation_;
    auto current = [&]() -> oa2dp_avdtp_sep * {
        oa2dp_avdtp_sep *s = oa2dp_avdtp_sep_get(&av_, seid);
        return !stopping_ && !transport_suspended_ && !teardown_pending_ &&
            connection_enabled_ && !suspend_requested_ && audio_ready_ && sig_open_ &&
            !rx_failed_ && media_open_ && local_seid_ == seid && sig_generation_ == connection &&
            s && s->remote_seid == remote &&
            (s->state == OA2DP_SEP_OPEN || s->state == OA2DP_SEP_STREAMING) ? s : nullptr;
    };
    if (!current()) return;
    auto retry = [&] {
        if (current()) {
            // Keep intent on the existing reconciliation loop. Rebuild from
            // current PCM/policy after backoff; never cache an obsolete rate.
            peer_configuration_pending_ = true;
            next_peer_configuration_retry_ = GetTickCount() + 1000;
        }
    };
    auto resume = [&] {
        if (!rate_resume_pending_ || !audio_running_) return;
        start_stream();
        oa2dp_avdtp_sep *s = current();
        if (s && s->state == OA2DP_SEP_STREAMING)
            rate_resume_pending_ = false; // accepted START; local admission has its own retry
        else
            retry();
    };
    if (rate == oa2dp_codec_sample_rate(&config_)) {
        resume();
        return;
    }
    if (sep->state == OA2DP_SEP_STREAMING)
        rate_resume_pending_ = true;
    if (!have_remote_caps_) {
        oa2dp_avdtp_event caps{};
        const bool received = request(oa2dp_avdtp_get_capabilities(&av_, remote, true), caps);
        if (!current()) return; // request() can accept peer CLOSE/ABORT
        oa2dp_codec_caps remote_caps{};
        if (!received || !caps.caps.media_codec ||
            oa2dp_codec_parse(caps.caps.codec, caps.caps.codec_len, &remote_caps) != OA2DP_OK ||
            remote_caps.codec != config_.codec) {
            retry();
            return;
        }
        remote_caps_ = remote_caps;
        have_remote_caps_ = true;
    }
    oa2dp_codec_caps next = config_;
    if (oa2dp_codec_set_rate(&next, &remote_caps_, rate) != OA2DP_OK)
        return; // no negotiated support; never send mismatched-rate PCM

    // Session::stop() can set stopping_ from another thread while selecting
    // the rate. Revalidate before dereferencing; an earlier successful check
    // does not make this later current() non-null.
    sep = current();
    if (!sep) return;
    bool was_streaming = sep->state == OA2DP_SEP_STREAMING;
    if (was_streaming)
        suspend_stream();
    sep = current();
    if (!sep) return;
    if (sep->state != OA2DP_SEP_OPEN) {
        retry(); // failed SUSPEND does not authorize RECONFIGURE or START
        return;
    }
    oa2dp_avdtp_caps avc = make_config_caps(next, false);
    oa2dp_avdtp_event r{};
    const bool accepted = request(oa2dp_avdtp_reconfigure(&av_, seid, &avc), r);
    if (!current()) return;
    if (accepted) {
        config_ = next;
        LOGI("reconfigured to %u Hz", rate);
        setup_audio_endpoint();
    } else {
        LOGW("RECONFIGURE to %u Hz rejected (0x%02X)", rate, r.error_code);
        retry();
        return; // retain the old codec without restarting mistimed PCM
    }
    resume();
}

void Session::reconcile_peer_configuration()
{
    if (!peer_configuration_pending_ || stopping_ || transport_suspended_ || teardown_pending_ ||
        !connection_enabled_ || !audio_ready_ || !sig_open_ || rx_failed_ || !media_open_ || !local_seid_ ||
        !is_after(GetTickCount(), next_peer_configuration_retry_))
        return;
    const uint8_t seid = local_seid_;
    const ULONG connection = sig_generation_;
    oa2dp_avdtp_sep *sep = oa2dp_avdtp_sep_get(&av_, seid);
    if (!sep || (sep->state != OA2DP_SEP_OPEN && sep->state != OA2DP_SEP_STREAMING))
        return;
    const uint8_t remote_seid = sep->remote_seid;
    if (!have_remote_caps_) {
        oa2dp_avdtp_event caps{};
        oa2dp_codec_caps remote{};
        const bool received = request(oa2dp_avdtp_get_capabilities(&av_, remote_seid, true), caps);
        // request() pumps peer commands. A stop/close or replacement binding
        // must not publish the old configuration when that wait returns.
        sep = oa2dp_avdtp_sep_get(&av_, local_seid_);
        if (stopping_ || transport_suspended_ || teardown_pending_ || !connection_enabled_ ||
            !audio_ready_ || !sig_open_ || rx_failed_ || !media_open_ ||
            sig_generation_ != connection || local_seid_ != seid || !sep ||
            sep->remote_seid != remote_seid ||
            (sep->state != OA2DP_SEP_OPEN && sep->state != OA2DP_SEP_STREAMING))
            return;
        if (received && caps.caps.media_codec &&
            oa2dp_codec_parse(caps.caps.codec, caps.caps.codec_len, &remote) == OA2DP_OK &&
            remote.codec == config_.codec) {
            remote_caps_ = remote;
            have_remote_caps_ = true;
        }
    }
    // Publish the complete format list before reconciling the accepted START.
    // Otherwise an unchanged PCM stream can remain at 96 kHz while the peer's
    // 48 kHz configuration permanently prevents local encoder admission.
    if (setup_audio_endpoint() && !stopping_ && !suspend_requested_ && have_remote_caps_ &&
        settings_.follow_source_rate && audio_running_)
        reconfigure_rate(audio_format_.SampleRate);
}

void Session::teardown(const char *why)
{
    LOGI("teardown: %s", why);
    rate_resume_pending_ = false;
    latency_publication_pending_ = false; // a retired link must not replay its delay
    end_streaming();
    // Also close an OPEN whose caller timed out before receiving its handle.
    // A failed close must retain ownership and suppress a replacement open.
    ULONG media_connection = 0, sig_connection = 0;
    const bool media_closed = tr_.close_channel(Oa2dpChannelMedia, sig_generation_, &media_connection);
    const bool sig_closed = tr_.close_channel(Oa2dpChannelSignaling, sig_generation_, &sig_connection);
    if (media_closed && Oa2dpChannelGenerationAfter(media_connection, closed_connection_generation_))
        closed_connection_generation_ = media_connection;
    if (sig_closed && Oa2dpChannelGenerationAfter(sig_connection, closed_connection_generation_))
        closed_connection_generation_ = sig_connection;
    if (media_closed) media_open_ = false;
    if (sig_closed) sig_open_ = false;
    teardown_pending_ = !media_closed || !sig_closed;
    init_deadline_ = 0;
    peer_configuration_pending_ = false;
    if (teardown_pending_) {
        LOGW("channel close is incomplete; retaining transport ownership");
        next_reconnect_ = GetTickCount() + kReconnectPollMs;
        publish("disconnect-pending");
        return;
    }
    // Transport closure and Windows/endpoint publication are separate results.
    // The parent independently publishes closure; do not hide child errors.
    publication_pending_ = !publish_disconnected_state();
    next_publication_retry_ = GetTickCount() + 1000;
    rx_failed_ = false;
    oa2dp_avdtp_reset(&av_);
    responses_.clear();
    local_seid_ = 0;
    init_deadline_ = 0;
    publish(publication_pending_ ? "state-update-pending" : "disconnected");
}

bool Session::publish_disconnected_state()
{
    if (!audio_ready_) return true; // unavailable child is already unplugged
    const bool policy = update_suspend_state();
    const bool endpoint = audio_.set_connected(false);
    return policy && endpoint;
}

void Session::refresh_transport_power()
{
    if (stopping_) return;
    OA2DP_DEVICE_INFO current{};
    if (!tr_.get_info(current) || current.Version != OA2DP_IOCTL_VERSION) {
        // The event is already consumed. Retain the query, not a guessed power
        // value: a failed resume query must not leave the session suspended
        // forever when no later notification arrives.
        power_refresh_pending_ = true;
        next_power_refresh_ = GetTickCount() + 1000;
        LOGW("cannot read transport power state; will retry");
        return;
    }
    power_refresh_pending_ = false;
    const bool suspended = (current.Flags & OA2DP_DEVICE_SUSPENDED) != 0;
    if (current.PowerSerial == power_serial_ && suspended == transport_suspended_) return;
    power_serial_ = current.PowerSerial;
    transport_suspended_ = suspended;
    // A complete sleep/resume may have occurred before this queued event was
    // consumed. Query the current state and retire the old AVDTP epoch either way.
    teardown(suspended ? "transport power suspended" : "transport power resumed");
    next_reconnect_ = GetTickCount();
    publish(suspended ? "power-suspended" : "power-resuming");
}

void Session::retry_transport_power()
{
    if (power_refresh_pending_ && !stopping_ &&
        is_after(GetTickCount(), next_power_refresh_))
        refresh_transport_power();
}

void Session::handle(const Command &c)
{
    switch (c.cmd) {
    case Cmd::SignalingOpenedByPeer:
        if (!Oa2dpChannelGenerationAfter(c.channel.ConnectionGeneration,
                                         closed_connection_generation_)) break;
        if (!Oa2dpChannelGenerationAfter(c.channel.Generation, sig_generation_)) break;
        sig_generation_ = c.channel.Generation;
        if (!connection_enabled_) {
            tr_.close_channel(Oa2dpChannelSignaling);
            break;
        }
        sig_mtu_ = c.channel.OutMtu;
        av_.out_mtu = sig_mtu_ < 48 ? 48 : sig_mtu_;
        sig_open_ = true;
        rx_failed_ = false;
        init_deadline_ = GetTickCount() + kPeerInitiatorGraceMs;
        LOGI("headset opened signalling");
        break;
    case Cmd::MediaOpenedByPeer:
        if (c.channel.ConnectionGeneration != sig_generation_ ||
            !Oa2dpChannelGenerationAfter(c.channel.ConnectionGeneration,
                                         closed_connection_generation_)) break;
        if (!Oa2dpChannelGenerationAfter(c.channel.Generation, media_generation_)) break;
        media_generation_ = c.channel.Generation;
        if (!connection_enabled_) {
            tr_.close_channel(Oa2dpChannelMedia);
            break;
        }
        media_mtu_ = c.channel.OutMtu;
        media_open_ = true;
        peer_configuration_pending_ = true;
        next_peer_configuration_retry_ = GetTickCount();
        break;
    case Cmd::SignalingClosed:
        if (!Oa2dpChannelEventCurrent(sig_generation_, c.channel.ConnectionGeneration)) break;
        teardown("signalling closed");
        break;
    case Cmd::MediaClosed:
        // An incoming OPEN can fail before it ever announces an open channel.
        // Its terminal CLOSED is still authoritative for this signalling link.
        // Ignore older media attempts and every replacement connection.
        if (c.channel.ConnectionGeneration != sig_generation_ ||
            (!Oa2dpChannelEventCurrent(media_generation_, c.channel.Generation) &&
             !Oa2dpChannelGenerationAfter(c.channel.Generation, media_generation_))) break;
        teardown("media closed");
        next_reconnect_ = GetTickCount() + kReconnectPollMs;
        break;
    case Cmd::StreamFailed:
        if (!c.channel.Generation) refresh_transport_power(); // includes lost power events
        if (c.channel.Generation &&
            !Oa2dpChannelEventCurrent(stream_generation_, c.channel.Generation)) break;
        if (!c.channel.Generation) sig_generation_ = 0; // driver backlog lost: close unknown epochs
        teardown("encoder or transport failure");
        publish("stream-error");
        next_reconnect_ = GetTickCount() + kReconnectPollMs;
        break;
    case Cmd::DeviceGone:
        stopping_ = true;
        SetEvent(stop_event_);
        break;
    case Cmd::AudioFormat:
        connection_enabled_ = c.format.ConnectionEnabled != 0;
        suspend_requested_ = c.format.SuspendRequested != 0;
        suspend_serial_ = c.format.SuspendSerial;
        if (connection_serial_ != c.format.ConnectionSerial) {
            connection_serial_ = c.format.ConnectionSerial;
            LOGI("Windows requested Bluetooth audio %s",
                 connection_enabled_ ? "connect" : "disconnect");
            if (connection_enabled_)
                next_reconnect_ = GetTickCount();
        }
        if (!connection_enabled_) {
            audio_format_ = c.format.Format;
            audio_running_ = c.format.Running != 0;
            teardown("Windows requested disconnect");
            if (!teardown_pending_)
                publish(publication_pending_ ? "state-update-pending" : "user-disconnected");
            break;
        }
        if (streaming_ && !oa2dp_pcm_format_same(&audio_format_, &c.format.Format))
            suspend_stream();
        audio_format_ = c.format.Format;
        pcm_sequence_ = c.format.PcmSequence;
        // A new clip with identical PCM is not a new Bluetooth stream. Keep
        // codec history and RTP identity; authorize the checked reader to move
        // to this producer epoch without SUSPEND/START or a receiver restart.
        approved_pcm_sequence_ = pcm_sequence_;
        if (audio_.data_event()) SetEvent(audio_.data_event());
        audio_running_ = c.format.Running != 0;
        if (teardown_pending_ || transport_suspended_) break;
        if (suspend_requested_) {
            suspend_stream();
            // Report stopped transport after the encoder has joined/drained.
            if (!streaming_ && audio_ready_)
                update_suspend_state();
            break;
        }
        if (audio_running_) {
            idle_since_ = 0;
            if (settings_.follow_source_rate)
                reconfigure_rate(c.format.Format.SampleRate);
            if (!sig_open_)
                next_reconnect_ = GetTickCount();  // user wants sound: connect now
            else if (!streaming_)
                start_stream();
        } else {
            idle_since_ = GetTickCount();
            // No PCM does not mean that HFP still owns the profile. A resume
            // request must complete even when the user has paused their music.
            update_suspend_state();
        }
        break;
    case Cmd::AudioUnavailable:
        LOGW("audio endpoint wait failed: %lu; releasing the old endpoint", c.error);
        end_streaming();
        audio_running_ = false;
        audio_ready_ = false;
        audio_.close();
        audio_wait_failed_ = false;
        teardown("audio endpoint unavailable");
        publish("audio-unavailable");
        next_reconnect_ = GetTickCount() + kReconnectPollMs;
        break;
    case Cmd::IdleCheck:
        break;
    case Cmd::TransportPower:
        refresh_transport_power();
        break;
    case Cmd::TransportAvailable: {
        // A delayed radio event is a retry hint, not authority to mutate a
        // healthy replacement connection or override explicit disconnect.
        if (!transport_suspended_ && connection_enabled_ && !sig_open_) {
            // Let an incoming AVDTP channel win before creating a competing
            // outgoing channel. Never postpone an already-due explicit connect.
            const DWORD earlier = GetTickCount() + kPeerInitiatorGraceMs;
            if (is_after(next_reconnect_, earlier)) next_reconnect_ = earlier;
        }
        break;
    }
    }
}

void Session::publish(const char *state)
{
    std::unique_lock<std::mutex> lock(status_mu_);
    status_.state = state;
    status_.input_container_bits = audio_format_.BitsPerSample;
    status_.input_valid_bits = audio_format_.ValidBitsPerSample;
    status_.input_encoding = audio_format_.Encoding;
    if (local_seid_) {
        status_.codec = codec_label(config_);
        status_.sample_rate = oa2dp_codec_sample_rate(&config_);
        status_.channels = oa2dp_codec_channels(&config_);
        oa2dp_settings effective = settings_;
        if (!effective.bits)
            effective.bits = oa2dp_pcm_format_ldac_bits(&audio_format_);
        status_.bits = oa2dp_codec_pcm_bits(&config_, &effective);
    }
    status_.delay_report_us = delay_report_us_;
    const LiveStatus snapshot = status_;
    lock.unlock(); // registry I/O must not hold the encoder's status lock
    publish_status(info_.RemoteAddress, snapshot);
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void Session::main_loop()
{
    while (!stopping_ && !tr_.open(path_)) {
        const DWORD error = GetLastError();
        // Interface arrival can precede cleanup of the retired session's
        // exclusive control file. Retry here in this existing session thread;
        // a one-time sharing violation must not disable audio until next boot.
        if (error != ERROR_SHARING_VIOLATION && error != ERROR_BUSY &&
            error != ERROR_NOT_READY && error != ERROR_FILE_NOT_FOUND &&
            error != ERROR_PATH_NOT_FOUND && error != ERROR_DEVICE_NOT_CONNECTED &&
            error != ERROR_DELETE_PENDING) {
            LOGE("cannot open native transport (%lu)", error);
            return;
        }
        WaitForSingleObject(stop_event_, 200);
    }
    if (stopping_) return;
    if (!tr_.get_info(info_) || info_.Version != OA2DP_IOCTL_VERSION) {
        LOGE("driver version mismatch or GET_INFO failed");
        tr_.close();
        return;
    }
    settings_ = load_settings(info_.RemoteAddress);
    transport_suspended_ = (info_.Flags & OA2DP_DEVICE_SUSPENDED) != 0;
    power_serial_ = info_.PowerSerial;
    if (!(info_.Flags & OA2DP_DEVICE_MPM_REGISTERED)) {
        LOGE("native profile-manager registration is unavailable");
        tr_.close();
        return;
    }
    LOGI("native Windows multi-profile manager registered");

    BLUETOOTH_DEVICE_INFO bdi{};
    bdi.dwSize = sizeof(bdi);
    bdi.Address.ullLong = info_.RemoteAddress;
    if (BluetoothGetDeviceInfo(nullptr, &bdi) == ERROR_SUCCESS)
        name_ = bdi.szName;
    LOGI("session for %ls (%ls)", address_key(info_.RemoteAddress).c_str(), name_.c_str());

    oa2dp_avdtp_callbacks cb{cb_send, cb_now, cb_event, cb_check, nullptr};
    oa2dp_avdtp_init(&av_, &cb, this, 672);
    for (uint8_t i = 0; i < settings_.priority_count; i++) {
        oa2dp_codec_id id = settings_.priority[i];
        oa2dp_codec_caps local{}, possible{};
        oa2dp_codec_local_caps(id, &local);
        if (oa2dp_codec_select(&local, &local, &settings_, &possible) != OA2DP_OK ||
            oa2dp_codec_check_settings(&possible, &settings_) != 0) {
            LOGW("codec %s cannot satisfy the current format policy; not advertising its SEP",
                 oa2dp_codec_name(id));
            continue;
        }
        if (!oa2dp_encoder_available(id)) {
            LOGW("encoder backend unavailable for %s; not advertising its SEP", oa2dp_codec_name(id));
            continue;
        }
        oa2dp_avdtp_caps caps = make_config_caps(local, true);
        int seid = oa2dp_avdtp_add_sep(&av_, OA2DP_AVDTP_TSEP_SRC, OA2DP_MEDIA_TYPE_AUDIO, &caps);
        if (seid > 0)
            sep_codec_[static_cast<uint8_t>(seid)] = id;
    }
    if (sep_codec_.empty()) {
        LOGE("no encoder available");
        publish("encoder-unavailable");
        tr_.close();
        return;
    }

    ev_ = std::thread([this] { event_loop(); });
    audio_thr_ = std::thread([this] { audio_loop(); });
    publish("connecting");
    next_reconnect_ = GetTickCount();
    DWORD next_status_write = GetTickCount() + 1000;

    while (!stopping_) {
        retry_transport_power(); // also runs while suspended or without an audio endpoint
        if (is_after(GetTickCount(), next_status_write)) {
            LiveStatus snapshot;
            {
                std::lock_guard<std::mutex> lock(status_mu_);
                snapshot = status_;
            }
            publish_status(info_.RemoteAddress, snapshot);
            next_status_write = GetTickCount() + 1000;
        }
        if (publication_pending_ && !transport_suspended_ && audio_ready_ && !sig_open_ && !media_open_ &&
            is_after(GetTickCount(), next_publication_retry_)) {
            next_publication_retry_ = GetTickCount() + 1000;
            if (publish_disconnected_state()) {
                publication_pending_ = false;
                publish("disconnected");
            }
        }
        if (!transport_suspended_ && teardown_pending_ && is_after(GetTickCount(), next_reconnect_))
            teardown("retry incomplete channel close");
        // Opening L2CAP establishes the ACL link when needed. Waiting for
        // BluetoothGetDeviceInfo.fConnected first deadlocks reconnect when
        // this profile is the only component that could create that link.
        if (!transport_suspended_ && !audio_ready_ && is_after(GetTickCount(), next_reconnect_)) {
            if (!ensure_audio_endpoint())
                next_reconnect_ = GetTickCount() + kReconnectPollMs;
        }
        if (!transport_suspended_ && audio_ready_ && connection_enabled_ && !teardown_pending_ && !sig_open_ &&
            is_after(GetTickCount(), next_reconnect_)) {
            next_reconnect_ = GetTickCount() + kReconnectPollMs;
            if (connect_signaling())
                init_deadline_ = GetTickCount() + kPeerInitiatorGraceMs;
        }
        if (!transport_suspended_ && !teardown_pending_ && sig_open_ && init_deadline_ && !local_seid_ && is_after(GetTickCount(), init_deadline_)) {
            init_deadline_ = 0;
            if (!configure_as_initiator())
                teardown("configuration failed");
            else if (audio_running_)
                start_stream();
        }
        if (streaming_ && !audio_running_ && idle_since_ &&
            is_after(GetTickCount(), idle_since_ + kIdleSuspendMs))
            suspend_stream();

        if (media_drain_pending_ && tr_.drain_media(0)) {
            media_drain_pending_ = false;
            if (audio_running_ && connection_enabled_ && !suspend_requested_ && !transport_suspended_)
                start_stream();
        }

        // Process received signalling and queued commands.
        while (pump(0)) {
        }
        std::deque<Command> cmds;
        HANDLE waits[] = {stop_event_, wake_event_, tr_.signaling_event()};
        WaitForMultipleObjects(sig_open_ && !rx_failed_ ? 3 : 2, waits, FALSE, 100);
        {
            std::lock_guard<std::mutex> lock(mu_);
            cmds.swap(cmd_q_);
        }
        for (const auto &c : cmds)
            handle(c);
        oa2dp_avdtp_check_timeouts(&av_);
        reconcile_peer_configuration();
        // Apply queued disconnect/suspend/format changes before retrying a
        // failed publication. No new PCM or player event is needed to recover.
        retry_profile_state();
    }

    teardown("session stopping");
    end_streaming();
    SetEvent(stop_event_);
    tr_.cancel_all();
    audio_.cancel_all();
    for (std::thread *t : {&ev_, &audio_thr_})
        if (t->joinable())
            t->join();
    audio_.close();
    tr_.close();
    clear_status(info_.RemoteAddress);
    LOGI("session for %ls ended", address_key(info_.RemoteAddress).c_str());
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

void Session::begin_streaming()
{
    stream_start_pending_ = false;
    if (stopping_ || peer_configuration_pending_ || !connection_enabled_ || transport_suspended_ ||
        suspend_requested_ || teardown_pending_ || streaming_ || !media_open_ || !audio_ready_)
        return;
    const oa2dp_avdtp_sep *sep = oa2dp_avdtp_sep_get(&av_, local_seid_);
    if (!sep || sep->state != OA2DP_SEP_STREAMING)
        return; // only an accepted peer/local START authorizes the sender
    if (audio_format_.SampleRate &&
        audio_format_.SampleRate != oa2dp_codec_sample_rate(&config_)) {
        LOGE("endpoint rate %lu does not match negotiated codec rate; not sending mistimed audio",
             audio_format_.SampleRate);
        publish("format-mismatch");
        return;
    }
    if (stream_.joinable())  // previous loop exited on its own (encoder error)
        stream_.join();
    if (!tr_.drain_media(0)) {
        media_drain_pending_ = true;
        publish("draining");
        return;
    }
    media_drain_pending_ = false;
    StreamContext context{};
    context.media = {Oa2dpChannelMedia, media_generation_, sig_generation_};
    context.config = config_;
    context.settings = settings_;
    context.pcm = audio_format_;
    context.ring = audio_.ring();
    context.mtu = media_mtu_;
    if (oa2dp_ring_snapshot(context.ring, &context.ring_format) != OA2DP_OK ||
        context.ring_format.channels > 2 || context.ring_format.sequence != pcm_sequence_) {
        LOGE("PCM format unavailable when starting the encoder");
        return;
    }
    approved_pcm_sequence_ = context.ring_format.sequence;
    // Publish availability before admitting a new sender. Otherwise a cached
    // suspended state could re-acknowledge HFP while this new thread starts.
    if (!update_suspend_state()) {
        stream_start_pending_ = true;
        publish("profile-policy-pending");
        return;
    }
    if (stopping_) // stop may have arrived during the synchronous publication
        return;
    streaming_ = true;
    if (!audio_running_ && !idle_since_) idle_since_ = GetTickCount();
    ResetEvent(stream_stop_event_);
    if (++stream_generation_ == 0) ++stream_generation_;
    context.generation = stream_generation_;
    {
        std::lock_guard<std::mutex> lock(status_mu_);
        status_.encoder_input.clear();
    }
    stream_ = std::thread([this, context] { stream_loop(context); });
    publish("streaming");
}

void Session::end_streaming()
{
    stream_start_pending_ = false;
    streaming_ = false;
    SetEvent(stream_stop_event_);
    if (stream_.joinable())
        stream_.join();
    {
        std::lock_guard<std::mutex> lock(status_mu_);
        status_.bitrate = 0;
        status_.encoder_input.clear();
    }
    publish("open");
}

bool Session::update_latency()
{
    const bool accepted = audio_.set_latency(0, delay_report_us_);
    latency_publication_pending_ = !accepted;
    if (!accepted)
        next_profile_retry_ = GetTickCount() + 1000;
    return accepted;
}

bool Session::update_suspend_state()
{
    if (!audio_ready_) return false;
    const bool unavailable = !connection_enabled_ || transport_suspended_ ||
        suspend_requested_ || !sig_open_ || !media_open_;
    // Failed outgoing OPENs can advance the kernel epoch without returning it
    // to this service. With no open signalling link, use the existing no-link
    // token: the driver's gate accepts zero only while its channel is Closed.
    // A newly opening/open replacement still rejects this stale publication.
    const ULONG connection = sig_open_ ? sig_generation_ : 0;
    const bool accepted = audio_.set_suspended(unavailable, suspend_serial_, connection);
    suspend_publication_pending_ = !accepted;
    if (!accepted)
        next_profile_retry_ = GetTickCount() + 1000;
    return accepted;
}

void Session::retry_profile_state()
{
    if ((!suspend_publication_pending_ && !stream_start_pending_ && !latency_publication_pending_) ||
        stopping_ || !audio_ready_ ||
        !is_after(GetTickCount(), next_profile_retry_))
        return;
    next_profile_retry_ = GetTickCount() + 1000;
    // Recompute availability and use the current serial/link rather than
    // replaying an obsolete reply after HFP, disconnect or endpoint recovery.
    if (suspend_publication_pending_ && !update_suspend_state())
        return;
    // Retry the latest value in place, without format publication, codec
    // renegotiation or restarting a working encoder. Teardown clears this
    // request; an unavailable transport waits for normal endpoint setup.
    if (latency_publication_pending_ && !transport_suspended_ && !teardown_pending_ &&
        connection_enabled_ && sig_open_ && media_open_)
        update_latency();
    if (stream_start_pending_) {
        stream_start_pending_ = false;
        oa2dp_avdtp_sep *sep = oa2dp_avdtp_sep_get(&av_, local_seid_);
        // Retry only local admission of an accepted START. Never originate a
        // new remote START after the peer has suspended or closed the stream.
        if (sep && sep->state == OA2DP_SEP_STREAMING)
            begin_streaming();
    }
}

void Session::stream_loop(const StreamContext &stream)
{
    AudioScheduling scheduling;
    const ULONG generation = stream.generation;
    auto fail_stream = [this, generation] {
        Command command{Cmd::StreamFailed};
        command.channel.Generation = generation;
        post(command);
    };
    const oa2dp_codec_caps cfg = stream.config;
    oa2dp_settings set = stream.settings;
    oa2dp_pcm_ring *ring = stream.ring;

    oa2dp_encoder *enc = nullptr;
    int err = oa2dp_encoder_create_for_pcm(&cfg, &set, stream.mtu, &stream.pcm, &enc);
    if (err != OA2DP_OK) {
        LOGE("encoder for %s failed: %s", oa2dp_codec_name(cfg.codec), oa2dp_strerror(err));
        fail_stream();
        return;
    }
    const oa2dp_encoder_info info = enc->info;
    const bool native_pcm = info.input_format != OA2DP_ENCODER_INPUT_S24;
    if (!set.bits)
        set.bits = oa2dp_pcm_format_ldac_bits(&stream.pcm);
    const uint8_t encoder_bits = oa2dp_codec_pcm_bits(&cfg, &set);
    const uint32_t spc = info.samples_per_call;
    const uint32_t spf = info.samples_per_frame ? info.samples_per_frame : spc;
    oa2dp_ring_format ring_format = stream.ring_format;
    // Windows may already have queued the beginning of this clip while AVDTP
    // START and encoder creation completed. The producer's epoch marks stale
    // audio; ring_read excludes that older epoch without dropping this prefix.
    const uint16_t ring_ch = ring_format.channels;
    const uint8_t ch = info.channels;
    if (!audio_.set_data_event(spc)) {
        LOGE("cannot register PCM data-ready notification");
        oa2dp_encoder_destroy(enc);
        fail_stream();
        return;
    }
    HANDLE pcm_waits[] = {stream_stop_event_, stop_event_, audio_.data_event()};
    // Admission follows accepted AVDTP START and encoder creation. Until this
    // point the WaveRT clock retains the OS's first cyclic-buffer samples.
    (void)oa2dp_ring_admit(ring, &ring_format);
    TraceLoggingWrite(g_Oa2dpStreamTrace, "EncoderStart",
        TraceLoggingLevel(5),
        TraceLoggingUInt32(generation, "StreamGeneration"),
        TraceLoggingUInt32(ring_format.sequence, "PcmSequence"),
        TraceLoggingUInt32(info.sample_rate, "SampleRate"),
        TraceLoggingUInt64(ring_format.start_pos, "EpochStartFrame"),
        TraceLoggingUInt64(static_cast<uint64_t>(ring->read_pos), "ReadFrame"),
        TraceLoggingUInt32(oa2dp_ring_readable(ring), "BufferedFrames"),
        TraceLoggingUInt64(oa2dp_ring_overruns(ring), "TotalOverrunFrames"));
    bool first_pcm = true;
    {
        std::lock_guard<std::mutex> lock(status_mu_);
        status_.encoder_input = info.input_format == OA2DP_ENCODER_INPUT_F32 ? "float32" :
            info.input_format == OA2DP_ENCODER_INPUT_S32 ? "int32" : "S24 scale";
    }

    std::vector<uint8_t> pkt(stream.mtu), out(info.max_output);
    std::vector<int32_t> pcm(static_cast<size_t>(spc) * std::max<uint16_t>(ring_ch, 2));
    oa2dp_rtp rtp{};
    oa2dp_rtp_init(&rtp, cfg.codec, pkt.data(), pkt.size(), GetTickCount() ^ 0xA2D9u);

    // Codecs whose encoder already sizes frames for the MTU, or that target
    // low latency, send one encoder output per packet.
    const bool flush_each = cfg.codec == OA2DP_CODEC_AAC || cfg.codec == OA2DP_CODEC_LDAC ||
                            cfg.codec == OA2DP_CODEC_APTX_LL;

    oa2dp_abr abr{};
    const bool use_abr = set.adaptive_bitrate && (info.levels > 1 || info.native_abr);
    oa2dp_abr_init(&abr, info.levels, kAbrHighWater, kAbrLowWater, kAbrDownHoldMs, kAbrUpHoldMs);

    uint64_t bytes_sent = 0;
    DWORD window_start = GetTickCount(), last_abr = window_start;
    LARGE_INTEGER clock_rate{};
    QueryPerformanceFrequency(&clock_rate);
    MediaClock packet_clock(static_cast<uint64_t>(clock_rate.QuadPart), info.sample_rate);
    // A waitable timer allows cancellation without a polling loop or changing
    // the process/system timer resolution.
    HANDLE packet_timer = CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!packet_timer) packet_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (!packet_timer) {
        oa2dp_ring_retire(ring);
        oa2dp_encoder_destroy(enc);
        fail_stream();
        return;
    }
    HANDLE packet_waits[] = {stream_stop_event_, stop_event_, packet_timer};
    uint64_t wait_count = 0, max_wait_us = 0;
    uint64_t source_frames = 0, silence_frames = 0;
    uint32_t peak_frames = 0;
    uint64_t previous_overruns = oa2dp_ring_overruns(ring);
    LOGI("streaming %s: %u Hz, %u ch, %u samples/call, adapter input %s", oa2dp_codec_name(cfg.codec),
         info.sample_rate, ch, spc, info.input_format == OA2DP_ENCODER_INPUT_F32 ? "float32" :
         info.input_format == OA2DP_ENCODER_INPUT_S32 ? "int32" : "S24 scale");

    auto send_packet = [&](size_t len, uint32_t frames) {
        if (len && streaming_) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            const uint64_t due = packet_clock.deadline(static_cast<uint64_t>(now.QuadPart));
            while (static_cast<uint64_t>(now.QuadPart) < due) {
                const uint64_t delta = due - static_cast<uint64_t>(now.QuadPart);
                const uint64_t hz = static_cast<uint64_t>(clock_rate.QuadPart);
                LARGE_INTEGER wait{};
                wait.QuadPart = -static_cast<LONGLONG>(delta / hz * 10000000 +
                    (delta % hz * 10000000 + hz - 1) / hz);
                if (!SetWaitableTimer(packet_timer, &wait, 0, nullptr, nullptr, FALSE)) {
                    fail_stream(); streaming_ = false; return;
                }
                const DWORD wake = WaitForMultipleObjects(3, packet_waits, FALSE, INFINITE);
                if (wake != WAIT_OBJECT_0 + 2) {
                    if (wake == WAIT_FAILED) fail_stream();
                    streaming_ = false; return;
                }
                QueryPerformanceCounter(&now);
            }
            if (tr_.send_media(stream.media, pkt.data(), len, stream_stop_event_)) {
                bytes_sent += len;
                TraceLoggingWrite(g_Oa2dpStreamTrace, "MediaSubmit",
                    TraceLoggingLevel(5),
                    TraceLoggingUInt32(generation, "StreamGeneration"),
                    TraceLoggingUInt32(ring_format.sequence, "PcmSequence"),
                    TraceLoggingUInt32(frames, "MediaFrames"),
                    TraceLoggingUInt64(source_frames, "SourceFrames"),
                    TraceLoggingUInt64(silence_frames, "PaddingFrames"));
            } else {
                if (streaming_) {
                    LOGE("Bluetooth media send failed");
                    fail_stream();
                }
                streaming_ = false;
            }
            if (frames) packet_clock.advance(frames);
        }
    };
    auto flush_packet = [&] {
        const uint32_t frames = rtp.samples;
        send_packet(oa2dp_rtp_flush(&rtp), frames);
    };

    while (streaming_) {
        const bool timing = TraceLoggingProviderEnabled(g_Oa2dpStreamTrace, 5, 0) != FALSE;
        if (timing) peak_frames = (std::max)(peak_frames, oa2dp_ring_readable(ring));
        if (ring_format.sample_rate != info.sample_rate) {
            LOGE("PCM and codec rates differ; refusing mistimed audio");
            fail_stream();
            break;
        }
        uint32_t read_frames = 0;
        bool block_ready = false;
        const size_t input_bytes = pcm.size() * sizeof(int32_t);
        const int read_status = oa2dp_ring_read_block(ring, &ring_format, pcm.data(), input_bytes,
            spc, &read_frames, &block_ready);
        if (read_status == OA2DP_ERR_STATE) {
            if (adopt_pcm_epoch(ring, ring_format, approved_pcm_sequence_.load())) {
                first_pcm = true;
                TraceLoggingWrite(g_Oa2dpStreamTrace, "PcmEpoch",
                    TraceLoggingLevel(5),
                    TraceLoggingUInt32(generation, "StreamGeneration"),
                    TraceLoggingUInt32(ring_format.sequence, "PcmSequence"),
                    TraceLoggingUInt32(info.sample_rate, "SampleRate"),
                    TraceLoggingUInt64(ring_format.start_pos, "EpochStartFrame"),
                    TraceLoggingUInt64(static_cast<uint64_t>(ring->read_pos), "ReadFrame"),
                    TraceLoggingUInt64(oa2dp_ring_overruns(ring), "TotalOverrunFrames"));
                continue;
            }
            // Full PCM changes still retire/reconfigure the encoder. A same-
            // format epoch waits for the policy thread's explicit authorization.
            const DWORD wake = WaitForMultipleObjects(3, pcm_waits, FALSE, INFINITE);
            if (wake != WAIT_OBJECT_0 + 2) {
                if (wake == WAIT_FAILED) fail_stream();
                break;
            }
            continue;
        }
        if (read_status == OA2DP_ERR_BUSY ||
            (read_status == OA2DP_OK && !block_ready)) {
            // Batching codecs may have complete frames without a full packet.
            // Do not hold that tail until more PCM arrives (possibly never).
            // BUSY has no validated format snapshot, so only flush on OK.
            if (read_status == OA2DP_OK && rtp.frames) {
                flush_packet();
                if (!streaming_) break;
            }
            LARGE_INTEGER before{}, after{};
            if (timing) QueryPerformanceCounter(&before);
            const DWORD wake = WaitForMultipleObjects(3, pcm_waits, FALSE, INFINITE);
            if (timing && clock_rate.QuadPart > 0) {
                QueryPerformanceCounter(&after);
                const auto elapsed = static_cast<uint64_t>(after.QuadPart - before.QuadPart);
                const auto hz = static_cast<uint64_t>(clock_rate.QuadPart);
                max_wait_us = (std::max)(max_wait_us, elapsed / hz * 1000000 + elapsed % hz * 1000000 / hz);
                ++wait_count;
            }
            if (wake != WAIT_OBJECT_0 + 2) {
                if (wake == WAIT_FAILED) fail_stream();
                break;
            }
            continue;
        }
        if (read_status != OA2DP_OK || !block_ready || read_frames > spc) {
            LOGE("invalid PCM block; stopping the encoder");
            fail_stream();
            break;
        }
        source_frames += read_frames;
        silence_frames += spc - read_frames;
        if (first_pcm && read_frames) {
            first_pcm = false;
            TraceLoggingWrite(g_Oa2dpStreamTrace, "FirstPcm",
                TraceLoggingLevel(5),
                TraceLoggingUInt32(generation, "StreamGeneration"),
                TraceLoggingUInt32(ring_format.sequence, "PcmSequence"),
                TraceLoggingUInt64(ring_format.start_pos, "EpochStartFrame"),
                TraceLoggingUInt64(static_cast<uint64_t>(ring->read_pos), "ReadFrame"),
                TraceLoggingUInt32(read_frames, "Frames"));
        }
        // Prepare codec PCM: channel layout before final precision.
        const int prepared = native_pcm ?
            oa2dp_pcm_prepare_native32(&ring_format, pcm.data(), input_bytes,
                pcm.data(), pcm.size(), spc, ch) :
            oa2dp_pcm_prepare_encoder(&ring_format, pcm.data(), input_bytes,
                pcm.data(), pcm.size(), spc, ch, encoder_bits);
        if (prepared != OA2DP_OK) {
            LOGE("invalid PCM preparation metadata or buffer size");
            fail_stream();
            break;
        }

        size_t len = 0;
        uint8_t frames = 0;
        if (oa2dp_encoder_encode(enc, pcm.data(), out.data(), out.size(), &len, &frames) != OA2DP_OK) {
            LOGE("encode failed; stopping instead of reporting a silent stream");
            fail_stream();
            break;
        }
        if (len == 0 || frames == 0)
            continue;

        int r = oa2dp_rtp_add_frames(&rtp, out.data(), len, frames, spf * frames);
        if (r == 0) {
            flush_packet();
            r = oa2dp_rtp_add_frames(&rtp, out.data(), len, frames, spf * frames);
        }
        if (r == OA2DP_ERR_NOSPC) {  // larger than one packet: fragment
            flush_packet();
            size_t off = 0;
            while (off < len && streaming_) {
                int n = oa2dp_rtp_fragment(&rtp, out.data(), len, spf * frames, &off);
                if (n <= 0) {
                    LOGE("RTP fragmentation failed (%d); stopping the stream", n);
                    fail_stream();
                    streaming_ = false;
                    break;
                }
                send_packet(static_cast<size_t>(n), off == len ? spf * frames : 0);
            }
        } else if (r != 1) {
            LOGE("RTP packetization failed (%d); stopping the stream", r);
            fail_stream();
            streaming_ = false;
        } else if (flush_each) {
            flush_packet();
        }

        DWORD now = GetTickCount();
        if (use_abr && now - last_abr >= 20) {
            last_abr = now;
            OA2DP_TX_STATE tx{};
            if (tr_.tx_state(tx)) {
                if (info.native_abr && enc->ops->queue_feedback) {
                    enc->ops->queue_feedback(enc, tx.QueuedPackets);
                } else {
                    int level = oa2dp_abr_update(&abr, tx.QueuedPackets, now);
                    if (level >= 0) {
                        oa2dp_encoder_set_level(enc, static_cast<uint8_t>(level));
                        LOGI("link quality: level %d of %u", level, info.levels);
                    }
                }
            }
        }
        if (now - window_start >= 1000) {
            if (timing) {
                const uint64_t overruns = oa2dp_ring_overruns(ring);
                TraceLoggingWrite(g_Oa2dpStreamTrace, "PcmWindow",
                    TraceLoggingLevel(5),
                    TraceLoggingUInt32(generation, "StreamGeneration"),
                    TraceLoggingUInt32(stream.media.ConnectionGeneration, "ConnectionGeneration"),
                    TraceLoggingUInt32(info.sample_rate, "SampleRate"),
                    TraceLoggingUInt64(wait_count, "DataWaitCount"),
                    TraceLoggingUInt64(max_wait_us, "MaxDataWaitUs"),
                    TraceLoggingUInt32(peak_frames, "PeakBufferedFrames"),
                    TraceLoggingUInt64(overruns - previous_overruns, "OverrunFrames"));
                previous_overruns = overruns;
                wait_count = max_wait_us = 0;
                peak_frames = 0;
            }
            std::lock_guard<std::mutex> lock(status_mu_);
            status_.bitrate = static_cast<uint32_t>(bytes_sent * 8000 / (now - window_start));
            status_.abr_level = abr.level;
            bytes_sent = 0;
            window_start = now;
        }
    }
    // Retired audio belongs to this encoder/link. Discard a partial packet.
    oa2dp_ring_retire(ring);
    CloseHandle(packet_timer);
    TraceLoggingWrite(g_Oa2dpStreamTrace, "EncoderEnd",
        TraceLoggingLevel(5),
        TraceLoggingUInt32(generation, "StreamGeneration"),
        TraceLoggingUInt64(source_frames, "SourceFrames"),
        TraceLoggingUInt64(silence_frames, "PaddingFrames"),
        TraceLoggingUInt64(oa2dp_ring_overruns(ring), "TotalOverrunFrames"),
        TraceLoggingUInt32(oa2dp_ring_readable(ring), "RemainingFrames"));
    tr_.cancel_media();
    if (!tr_.drain_media())
        LOGW("media drain budget expired; retaining pending I/O buffers");
    oa2dp_encoder_destroy(enc);
    {
        std::lock_guard<std::mutex> lock(status_mu_);
        status_.encoder_input.clear();
    }
}

}  // namespace oa2dp
