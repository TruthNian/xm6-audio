/* SPDX-License-Identifier: MIT */
/* Actual session admission/publication bodies with fake profile/OS APIs.
 * The worker is real but performs no encoding or I/O. This is not an MPM test. */
#include "oa2dp/pcm_ring.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define REQUIRE(x)                                                                                 \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                   \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)
#define LOGE(...) ((void)0)
#define LOGW(...) ((void)0)
using ULONG = uint32_t;
using DWORD = uint32_t;
using USHORT = uint16_t;
using HANDLE = bool *;
static DWORD now;
static DWORD GetTickCount() { return now; }
static bool is_after(DWORD a, DWORD b) { return static_cast<int32_t>(a - b) >= 0; }
static void SetEvent(HANDLE h) { *h = true; }
static void ResetEvent(HANDLE h) { *h = false; }
constexpr int Oa2dpChannelMedia = 1;
constexpr int OA2DP_SEP_OPEN = 2, OA2DP_SEP_STREAMING = 3;
struct oa2dp_avdtp_sep {
    int state = OA2DP_SEP_OPEN;
};
struct oa2dp_avdtp_event {
    int error_code = 0;
};
struct FakeAvdtp {
    oa2dp_avdtp_sep sep;
};
struct FakeConfig {
    uint32_t rate = 48000;
};
struct FakeFormat {
    uint32_t SampleRate = 48000;
};
struct OA2DP_SEND_IN {
    int Kind;
    ULONG Generation, ConnectionGeneration;
};
static oa2dp_avdtp_sep *oa2dp_avdtp_sep_get(FakeAvdtp *av, int seid) {
    return seid ? &av->sep : nullptr;
}
static int oa2dp_avdtp_start(FakeAvdtp *, int) { return 1; }
static uint32_t oa2dp_codec_sample_rate(const FakeConfig *cfg) { return cfg->rate; }

struct Session {
    struct Transport {
        bool drained = true;
        bool drain_media(DWORD) { return drained; }
    } tr_;
    struct Audio {
        std::vector<uint64_t> storage;
        unsigned failures = 0, calls = 0;
        bool suspended = false;
        ULONG serial = 0, connection = 0;
        std::function<void()> during_publication;
        Audio() : storage((oa2dp_ring_bytes(16, 2) + 7) / 8) {
            REQUIRE(oa2dp_ring_init(ring(), 16, 48000, 2, OA2DP_RING_FMT_S32) == OA2DP_OK);
        }
        oa2dp_pcm_ring *ring() { return reinterpret_cast<oa2dp_pcm_ring *>(storage.data()); }
        bool set_suspended(bool value, ULONG request, ULONG epoch) {
            ++calls;
            suspended = value;
            serial = request;
            connection = epoch;
            if (during_publication)
                during_publication();
            if (!failures)
                return true;
            --failures;
            return false;
        }
    } audio_;
    struct StreamContext {
        ULONG generation;
        OA2DP_SEND_IN media;
        FakeConfig config;
        int settings;
        FakeFormat pcm;
        oa2dp_ring_format ring_format;
        oa2dp_pcm_ring *ring;
        USHORT mtu;
    };
    FakeAvdtp av_;
    FakeConfig config_;
    FakeFormat audio_format_;
    int local_seid_ = 1, settings_ = 0;
    bool connection_enabled_ = true, transport_suspended_ = false, suspend_requested_ = false;
    bool teardown_pending_ = false, streaming_ = false, media_open_ = true, sig_open_ = true;
    bool audio_ready_ = true, media_drain_pending_ = false;
    bool publication_pending_ = false;
    bool stopping_ = false, suspend_publication_pending_ = false, stream_start_pending_ = false;
    DWORD next_profile_retry_ = 0;
    ULONG pcm_sequence_ = 0, media_generation_ = 2, sig_generation_ = 1;
    ULONG stream_generation_ = 0, suspend_serial_ = 1;
    USHORT media_mtu_ = 1000;
    bool stream_stop_ = true;
    HANDLE stream_stop_event_ = &stream_stop_;
    std::thread stream_;
    std::mutex status_mu_;
    struct {
        unsigned bitrate = 0;
    } status_;
    std::string state;
    unsigned starts = 0;
    std::atomic<unsigned> workers{0};
    ~Session() {
        if (stream_.joinable())
            stream_.join();
    }
    bool request(int, oa2dp_avdtp_event &) {
        ++starts;
        av_.sep.state = OA2DP_SEP_STREAMING;
        return true;
    }
    void publish(const char *s) { state = s; }
    void stream_loop(const StreamContext &) { ++workers; }
    bool start_stream();
    void begin_streaming();
    void end_streaming();
    bool update_suspend_state();
    void retry_profile_state();
    bool finish_endpoint_setup();
};
// clang-format off
#include "stream-start-under-test.inc"
#include "stream-admission-under-test.inc"
// clang-format on
bool Session::finish_endpoint_setup() {
#include "endpoint-ready-under-test.inc"
    return true;
}

int main() {
    {
        Session s;
        now = 10;
        s.media_open_ = false;
        s.av_.sep.state = OA2DP_SEP_STREAMING; // accepted peer START from signalling
        s.begin_streaming();                   // media-open notification has not reached main yet
        REQUIRE(!s.streaming_ && !s.workers);
        s.media_open_ = true;
        REQUIRE(s.finish_endpoint_setup()); // late media readiness, no format/player event
        REQUIRE(s.streaming_ && s.starts == 0);
        s.end_streaming();
        REQUIRE(s.workers == 1);
    }
    {
        Session s;
        now = 11;
        s.av_.sep.state = OA2DP_SEP_OPEN;
        REQUIRE(s.finish_endpoint_setup());
        REQUIRE(!s.streaming_ && s.starts == 0 && s.workers == 0);
        s.av_.sep.state = OA2DP_SEP_STREAMING;
        s.begin_streaming(); // opposite ordering: media ready before peer START
        REQUIRE(s.streaming_ && s.starts == 0);
        REQUIRE(s.finish_endpoint_setup()); // duplicate successful readiness
        s.end_streaming();
        REQUIRE(s.workers == 1);
    }
    // Reconciliation must not create a new remote START or bypass admission.
    for (int condition = 0; condition < 8; ++condition) {
        Session s;
        now = 12;
        s.av_.sep.state = OA2DP_SEP_STREAMING;
        switch (condition) {
        case 0:
            s.connection_enabled_ = false;
            break;
        case 1:
            s.suspend_requested_ = true;
            break;
        case 2:
            s.transport_suspended_ = true;
            break;
        case 3:
            s.teardown_pending_ = true;
            break;
        case 4:
            s.media_open_ = false;
            break;
        case 5:
            s.audio_ready_ = false;
            break;
        case 6:
            s.stopping_ = true;
            break;
        case 7:
            s.local_seid_ = 0;
            break;
        }
        REQUIRE(s.finish_endpoint_setup());
        REQUIRE(!s.streaming_ && s.starts == 0 && s.workers == 0);
    }
    {
        Session s;
        now = 13;
        s.av_.sep.state = OA2DP_SEP_STREAMING;
        s.audio_.failures = 2; // setup publication and then local admission
        REQUIRE(s.finish_endpoint_setup());
        REQUIRE(!s.streaming_ && s.stream_start_pending_ && s.suspend_publication_pending_);
        now += 1000;
        s.retry_profile_state();
        REQUIRE(s.streaming_ && s.starts == 0);
    }
    {
        Session s;
        now = 14;
        s.av_.sep.state = OA2DP_SEP_STREAMING;
        // Stop can arrive after the initial gate, while its IOCTL is pending.
        s.audio_.during_publication = [&] { s.stopping_ = true; };
        s.begin_streaming();
        REQUIRE(!s.streaming_ && !s.workers && s.starts == 0);
    }
    {
        Session s;
        now = 100;
        s.audio_.failures = 2;
        // Remote START acceptance is not proof that the local encoder started.
        REQUIRE(!s.start_stream());
        REQUIRE(!s.streaming_ && s.av_.sep.state == OA2DP_SEP_STREAMING && s.starts == 1);
        REQUIRE(s.suspend_publication_pending_ && s.stream_start_pending_);
        now = 1099;
        s.retry_profile_state();
        REQUIRE(s.audio_.calls == 1 && !s.streaming_);
        now = 1100;
        s.retry_profile_state();
        REQUIRE(s.audio_.calls == 2 && !s.streaming_ && s.stream_start_pending_);
        now = 2100;
        s.retry_profile_state();
        REQUIRE(s.streaming_ && s.starts == 1 && !s.suspend_publication_pending_ &&
                !s.stream_start_pending_);
        const unsigned calls = s.audio_.calls;
        now += 1000;
        s.retry_profile_state();
        REQUIRE(s.audio_.calls == calls); // no publication polling after success
        s.end_streaming();
        REQUIRE(s.workers == 1);
    }
    {
        Session s;
        now = 10;
        s.audio_.failures = 1;
        REQUIRE(!s.start_stream());
        // Another successful publisher must not lose the failed-start intent.
        REQUIRE(s.update_suspend_state());
        REQUIRE(s.stream_start_pending_ && !s.suspend_publication_pending_);
        now = 1010;
        s.retry_profile_state();
        REQUIRE(s.streaming_ && s.starts == 1);
    }
    {
        Session s;
        now = 20;
        s.audio_.failures = 1;
        REQUIRE(!s.start_stream());
        s.end_streaming(); // actual peer suspend/close/teardown admission path
        REQUIRE(!s.stream_start_pending_);
        s.av_.sep.state = OA2DP_SEP_OPEN;
        s.suspend_requested_ = true;
        s.suspend_serial_ = 23;
        s.sig_generation_ = 17;
        now = 1020;
        s.retry_profile_state();
        REQUIRE(!s.streaming_ && !s.stream_start_pending_ && !s.suspend_publication_pending_);
        REQUIRE(s.audio_.suspended && s.audio_.serial == 23 && s.audio_.connection == 17);
        REQUIRE(s.starts == 1 && s.workers == 0);
    }
    // Stop, removal, policy denial and protocol state changes prohibit restart.
    for (int condition = 0; condition < 8; ++condition) {
        Session s;
        now = 30;
        s.audio_.failures = 1;
        REQUIRE(!s.start_stream());
        switch (condition) {
        case 0:
            s.stopping_ = true;
            break;
        case 1:
            s.audio_ready_ = false;
            break;
        case 2:
            s.connection_enabled_ = false;
            break;
        case 3:
            s.suspend_requested_ = true;
            break;
        case 4:
            s.transport_suspended_ = true;
            break;
        case 5:
            s.teardown_pending_ = true;
            break;
        case 6:
            s.av_.sep.state = OA2DP_SEP_OPEN;
            break;
        case 7:
            s.local_seid_ = 0;
            break;
        }
        now = 1030;
        s.retry_profile_state();
        REQUIRE(!s.streaming_ && s.starts == 1 && s.workers == 0);
        if (condition < 2)
            REQUIRE(s.audio_.calls == 1);
    }
    {
        Session s;
        now = 40;
        s.suspend_requested_ = true;
        s.audio_.failures = 1;
        REQUIRE(!s.update_suspend_state()); // idle availability also needs recovery
        s.suspend_requested_ = false;
        s.suspend_serial_ = 25;
        s.sig_generation_ = 19;
        now = 1040;
        s.retry_profile_state();
        REQUIRE(!s.suspend_publication_pending_ && !s.stream_start_pending_ && !s.streaming_);
        REQUIRE(!s.audio_.suspended && s.audio_.serial == 25 && s.audio_.connection == 19);
        REQUIRE(s.starts == 0 && s.workers == 0);
    }
    {
        Session s;
        now = UINT32_MAX - 99;
        s.audio_.failures = 1;
        REQUIRE(!s.start_stream());
        now = 899;
        s.retry_profile_state();
        REQUIRE(s.audio_.calls == 1);
        now = 900;
        s.retry_profile_state();
        REQUIRE(s.streaming_ && s.starts == 1);
    }
    std::puts("production profile retry checks passed (external APIs fake, no headset)");
}
