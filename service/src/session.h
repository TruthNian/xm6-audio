// SPDX-License-Identifier: MIT
// One Session per headset bound to oa2dp.sys: AVDTP signalling (as INT or
// ACP), codec choice, audio endpoint setup and the encode/send loop.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "driver_io.h"
#include "oa2dp/a2dp_codecs.h"
#include "oa2dp/avdtp.h"
#include "settings_store.h"

namespace oa2dp {

class Session {
public:
    explicit Session(std::wstring transport_path);
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    void start();
    void stop();
    const std::wstring &path() const { return path_; }

private:
    enum class Cmd {
        SignalingOpenedByPeer,
        SignalingClosed,
        MediaOpenedByPeer,
        MediaClosed,
        DeviceGone,
        AudioFormat,
        AudioUnavailable,
        StreamFailed,
        IdleCheck,
    };
    struct Command {
        Cmd cmd;
        OA2DP_FORMAT_CHANGE format{};
        OA2DP_CHANNEL_INFO channel{};
        DWORD error = ERROR_SUCCESS;
    };

    // threads
    void main_loop();
    void event_loop();
    void audio_loop();
    void stream_loop(ULONG generation);

    // signalling helpers (main thread only)
    void post(const Command &c);
    bool pump(DWORD timeout_ms);  // feed one received SDU, false on timeout
    bool request(int label, oa2dp_avdtp_event &out, DWORD timeout_ms = 4000);
    void handle(const Command &c);
    bool connect_signaling();
    bool configure_as_initiator();
    bool open_media_channel();
    bool setup_audio_endpoint();
    bool ensure_audio_endpoint();
    bool start_stream();
    void suspend_stream();
    void reconfigure_rate(uint32_t rate);
    void teardown(const char *why);
    void on_avdtp_event(const oa2dp_avdtp_event &ev);
    void begin_streaming();
    void end_streaming();
    void publish(const char *state);

    // AVDTP callbacks
    static int cb_send(void *user, const uint8_t *pdu, size_t len);
    static uint32_t cb_now(void *user);
    static void cb_event(void *user, const oa2dp_avdtp_event *ev);
    static uint8_t cb_check(void *user, uint8_t seid, const oa2dp_avdtp_caps *cfg, uint8_t *cat);

    std::wstring path_;
    Transport tr_;
    AudioEndpoint audio_;
    OA2DP_DEVICE_INFO info_{};
    oa2dp_settings settings_{};
    std::wstring name_;

    oa2dp_avdtp av_{};
    std::map<uint8_t, oa2dp_codec_id> sep_codec_;  // local SEID -> codec
    std::map<int, oa2dp_avdtp_event> responses_;   // label -> response
    uint16_t sig_mtu_ = 0, media_mtu_ = 0;
    bool sig_open_ = false, media_open_ = false;
    ULONG sig_generation_ = 0, media_generation_ = 0;
    ULONG stream_generation_ = 0;
    bool teardown_pending_ = false;
    bool rx_failed_ = false;   // owned by the signalling thread

    // negotiated stream
    uint8_t local_seid_ = 0;
    oa2dp_codec_caps config_{};
    oa2dp_codec_caps remote_caps_{};  // capabilities of the peer SEP we use
    bool have_remote_caps_ = false;
    bool audio_running_ = false;
    bool connection_enabled_ = true;
    bool suspend_requested_ = false;
    ULONG connection_serial_ = 0;
    OA2DP_FORMAT audio_format_{};  // actual Windows stream, not file metadata
    DWORD idle_since_ = 0;
    DWORD init_deadline_ = 0;      // become INT if the peer has not configured us by then
    DWORD next_reconnect_ = 0;
    bool need_remote_caps_ = false;
    std::atomic<bool> audio_ready_{false};
    // A failed waiter stays quiescent until the main thread closes its handle.
    std::atomic<bool> audio_wait_failed_{false};

    // thread plumbing
    HANDLE stop_event_ = nullptr;
    HANDLE wake_event_ = nullptr;
    std::thread main_, ev_, audio_thr_, stream_;
    std::mutex mu_;
    std::deque<Command> cmd_q_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> streaming_{false};

    // shared with the stream thread
    std::atomic<uint32_t> delay_report_us_{0};
    LiveStatus status_;
    std::mutex status_mu_;
};

}  // namespace oa2dp
