/* SPDX-License-Identifier: MIT */
#include "../driver/common/channel_lifecycle.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#define REQUIRE(x)                                                                                 \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                   \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)
#define LOGI(...) ((void)0)
#define LOGW(...) ((void)0)
using ULONG = uint32_t;
constexpr int Oa2dpChannelMedia = 1, Oa2dpChannelSignaling = 0, kReconnectPollMs = 5000;
static uint32_t GetTickCount() { return 100; }
static void oa2dp_avdtp_reset(int *) {}
struct Session {
    struct Transport {
        bool result[2]{true, true};
        bool close_channel(int k, ULONG, ULONG *out) {
            *out = 1;
            return result[k];
        }
    } tr_;
    struct Audio {
        bool result = true;
        int calls = 0;
        bool set_connected(bool connected) {
            REQUIRE(!connected);
            ++calls;
            return result;
        }
    } audio_;
    bool policy = true, audio_ready_ = true, media_open_ = true, sig_open_ = true;
    bool teardown_pending_ = false, publication_pending_ = false, rx_failed_ = false,
         peer_configuration_pending_ = true;
    ULONG sig_generation_ = 1, closed_connection_generation_ = 0;
    uint32_t init_deadline_ = 1, next_reconnect_ = 0, next_publication_retry_ = 0;
    int av_ = 0, local_seid_ = 1;
    std::vector<int> responses_;
    std::string state;
    void end_streaming() {}
    bool update_suspend_state() { return policy; }
    void publish(const char *s) { state = s; }
    bool publish_disconnected_state();
    void teardown(const char *);
};
#include "disconnect-status-under-test.inc"
int main() {
    Session s;
    s.audio_.result = false;
    s.teardown("test");
    REQUIRE(s.publication_pending_ && s.state == "state-update-pending" && !s.teardown_pending_);
    s = Session{};
    s.policy = false;
    s.teardown("test");
    REQUIRE(s.publication_pending_ && s.state == "state-update-pending" && s.audio_.calls == 1);
    s = Session{};
    s.teardown("test");
    REQUIRE(!s.publication_pending_ && s.state == "disconnected");
    s = Session{};
    s.tr_.result[1] = false;
    s.teardown("test");
    REQUIRE(s.teardown_pending_ && s.state == "disconnect-pending" && !s.audio_.calls &&
            s.media_open_);
    s = Session{};
    s.audio_ready_ = false;
    s.teardown("test");
    REQUIRE(!s.audio_.calls && s.state == "disconnected");
    std::puts("production disconnect status propagation checks passed (OS/endpoint APIs fake)");
}
