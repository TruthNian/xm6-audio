/* SPDX-License-Identifier: MIT */
/* Actual service packetization block with the real RTP core. Windows also
 * feeds the actual compiled LDAC adapter; no Bluetooth/Windows audio I/O. */
#include "oa2dp/media.h"
#ifdef XM6_TEST_WITH_CODECS
#include "oa2dp/encoder.h"
#endif
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>
#define LOGE(...) ((void)0)

#define REQUIRE(x)                                                                                 \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                   \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)
static uint32_t be32(const uint8_t *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
using DWORD=uint32_t;
using HANDLE=unsigned;
constexpr bool FALSE=false;
constexpr DWORD WAIT_OBJECT_0=0, WAIT_FAILED=0xffffffffu, INFINITE=0xffffffffu;
struct LARGE_INTEGER { int64_t QuadPart=0; };
static void QueryPerformanceCounter(LARGE_INTEGER *value) {
    static int64_t ticks=0;
    value->QuadPart=++ticks;
}
struct Packetizer {
    std::vector<uint8_t> pkt;
    std::vector<std::vector<uint8_t>> sent;
    oa2dp_rtp rtp{};
    uint32_t spf;
    bool flush_each;
    bool streaming_ = true;
    bool fail_send = false;
    unsigned failures = 0;
    DWORD wake = WAIT_OBJECT_0+2;
    std::vector<size_t> packets_before_wait;
    void fail_stream() { ++failures; }
    Packetizer(oa2dp_codec_id codec, size_t mtu, uint32_t samples, bool flush)
        : pkt(mtu), spf(samples), flush_each(flush) {
        REQUIRE(oa2dp_rtp_init(&rtp, codec, pkt.data(), pkt.size(), 1) == OA2DP_OK);
    }
    void send_packet(size_t len) {
        REQUIRE(len <= pkt.size());
        if (len && streaming_) {
            if (fail_send) {
                fail_stream();streaming_=false;
                return;
            }
            sent.emplace_back(pkt.begin(), pkt.begin() + len);
        }
    }
    void add(const std::vector<uint8_t> &out, uint8_t frames) {
        const size_t len = out.size();
#include "service-rtp-under-test.inc"
    }
    void flush() { send_packet(oa2dp_rtp_flush(&rtp)); }
    DWORD WaitForMultipleObjects(unsigned count,HANDLE *handles,bool all,DWORD timeout) {
        REQUIRE(count==3 && handles[0]==1 && handles[1]==2 && handles[2]==3);
        REQUIRE(!all && timeout==INFINITE);
        packets_before_wait.push_back(sent.size());
        return wake;
    }
    void no_pcm(int read_status=OA2DP_OK) {
        const uint32_t read_frames=0;
        const bool timing=true;
        const LARGE_INTEGER clock_rate{1000000};
        uint64_t max_wait_us=0,wait_count=0;
        HANDLE pcm_waits[]={1,2,3};
        // One real service wait branch; OS waits and PCM availability controlled.
        for (unsigned pass=0; pass<1 && streaming_; ++pass) {
#include "service-pcm-wait-under-test.inc"
        }
    }
};
static void verify_ldac(const Packetizer &p, uint32_t represented, uint32_t first = 0) {
    uint32_t expected = first;
    uint16_t seq = 1;
    for (const auto &packet : p.sent) {
        REQUIRE(packet.size() > OA2DP_RTP_HEADER_LEN && packet.size() <= p.pkt.size());
        const uint32_t timestamp = be32(packet.data() + 4);
        if (timestamp != expected)
            std::fprintf(stderr, "LDAC RTP timestamp %u; expected %u\n", timestamp, expected);
        REQUIRE(timestamp == expected);
        REQUIRE(((uint16_t(packet[2]) << 8) | packet[3]) == seq++);
        REQUIRE((packet[12] & 0xf0) == 0 && (packet[12] & 0x0f) != 0);
        expected += (packet[12] & 0x0f) * p.spf;
    }
    if (p.rtp.timestamp != first + represented)
        std::fprintf(stderr, "LDAC clock %u; expected %u represented samples\n", p.rtp.timestamp,
                     first + represented);
    REQUIRE(p.rtp.timestamp == first + represented && expected == first + represented);
}
static void synthetic() {
    {
        // A complete SBC tail must leave before waiting for nonexistent input.
        Packetizer p(OA2DP_CODEC_SBC, 370, 128, false);
        p.add(std::vector<uint8_t>(119, 0x5a), 1);
        REQUIRE(p.sent.empty());
        p.no_pcm();
        REQUIRE(p.sent.size()==1 && p.packets_before_wait==std::vector<size_t>{1});
        REQUIRE(p.sent[0].size()==132 && p.sent[0][12]==1 && be32(p.sent[0].data()+4)==0);
        REQUIRE(p.rtp.timestamp==128 && !p.rtp.frames && !p.failures);
        p.no_pcm(); // empty wakeups cannot resend the tail or advance RTP time
        REQUIRE(p.sent.size()==1 && p.rtp.timestamp==128);
        p.add(std::vector<uint8_t>(119, 0xa5), 1);
        p.no_pcm();
        REQUIRE(p.sent.size()==2 && be32(p.sent[1].data()+4)==128 && p.rtp.timestamp==256);
    }
    {
        Packetizer p(OA2DP_CODEC_SBC, 370, 128, false);
        for(unsigned i=0;i<3;++i)p.add(std::vector<uint8_t>(119, 0x5a), 1);
        p.no_pcm();
        REQUIRE(p.sent.size()==1 && p.sent[0].size()==370 && p.sent[0][12]==3);
        REQUIRE(p.rtp.timestamp==384 && !p.failures);
    }
    {
        Packetizer p(OA2DP_CODEC_SBC, 370, 128, false);
        p.add(std::vector<uint8_t>(119, 0x5a), 1);
        p.no_pcm(OA2DP_ERR_BUSY); // format snapshot is unresolved: do not flush
        REQUIRE(p.sent.empty() && p.rtp.frames==1 && p.rtp.timestamp==0);
        REQUIRE(p.packets_before_wait==std::vector<size_t>{0});
        p.no_pcm();
        REQUIRE(p.sent.size()==1 && !p.failures);
    }
    {
        Packetizer p(OA2DP_CODEC_SBC, 370, 128, false);
        p.add(std::vector<uint8_t>(119, 0x5a), 1);
        p.fail_send=true;p.no_pcm();
        REQUIRE(p.failures==1 && !p.streaming_ && p.sent.empty());
        REQUIRE(p.packets_before_wait.empty()); // failed output cannot enter an infinite data wait
    }
    {
        Packetizer p(OA2DP_CODEC_LDAC, 679, 128, true);
        p.add(std::vector<uint8_t>(330, 0x5a), 1);
        p.no_pcm();
        REQUIRE(p.sent.size()==1 && p.rtp.timestamp==128 && !p.failures);
        p.wake=WAIT_FAILED;p.no_pcm();
        REQUIRE(p.failures==1 && p.sent.size()==1);
    }
    {
        // LDAC explicitly disallows RTP fragmentation. An oversized encoder
        // output must reach the failure path, not silently drop PCM forever.
        Packetizer p(OA2DP_CODEC_LDAC, 679, 128, true);
        p.add(std::vector<uint8_t>(1000, 0x5a), 3);
        REQUIRE(p.failures == 1 && !p.streaming_ && p.sent.empty() && !p.rtp.timestamp);
    }
    {
        Packetizer p(OA2DP_CODEC_LDAC, 679, 128, true);
        p.add(std::vector<uint8_t>(100, 0x5a), 16); // invalid LDAC frame count
        REQUIRE(p.failures == 1 && !p.streaming_ && p.sent.empty());
    }
    for (uint32_t spf : {128u, 256u}) {
        Packetizer p(OA2DP_CODEC_LDAC, 1024, spf, true);
        uint32_t represented = 0;
        for (uint8_t frames : {2, 1, 3, 2}) {
            p.add(std::vector<uint8_t>(330 * frames, 0x5a), frames);
            represented += spf * frames;
        }
        verify_ldac(p, represented);
    }
    {
        Packetizer p(OA2DP_CODEC_LDAC, 895, 256, true);
        const uint32_t first = UINT32_MAX - 255;
        p.rtp.timestamp = first;
        p.add(std::vector<uint8_t>(660, 0x5a), 2);
        p.add(std::vector<uint8_t>(330, 0x5a), 1);
        verify_ldac(p, 768, first);
    }
    {
        // Unflushed batching exercises the service's flush-and-retry branch.
        Packetizer p(OA2DP_CODEC_LDAC, 895, 128, false);
        p.add(std::vector<uint8_t>(660, 0x5a), 2);
        p.add(std::vector<uint8_t>(660, 0x5a), 2);
        p.flush();
        REQUIRE(p.sent.size() == 2);
        verify_ldac(p, 512);
    }
    {
        Packetizer p(OA2DP_CODEC_SBC, 370, 128, false);
        for (int i = 0; i < 5; ++i)
            p.add(std::vector<uint8_t>(119, 0x5a), 1);
        p.flush();
        REQUIRE(p.sent.size() == 2 && be32(p.sent[1].data() + 4) == 384);
        REQUIRE(p.rtp.timestamp == 640);
    }
    {
        Packetizer p(OA2DP_CODEC_AAC, 112, 1024, true);
        p.add(std::vector<uint8_t>(150, 0x5a), 1);
        p.add(std::vector<uint8_t>(80, 0x5a), 1);
        REQUIRE(p.sent.size() == 3);
        REQUIRE(be32(p.sent[0].data() + 4) == 0 && be32(p.sent[1].data() + 4) == 0);
        REQUIRE(be32(p.sent[2].data() + 4) == 1024 && p.rtp.timestamp == 2048);
    }
}
#ifdef XM6_TEST_WITH_CODECS
static void real_ldac() {
    REQUIRE(oa2dp_encoder_available(OA2DP_CODEC_LDAC));
    for (uint32_t rate : {96000u, 48000u, 44100u, 88200u})
        for (uint8_t bits : {16, 24})
            for (size_t mtu : {679u, 895u, 1024u}) {
                oa2dp_settings settings{};
                oa2dp_settings_default(&settings);
                settings.sample_rate = rate;
                settings.bits = bits;
                oa2dp_codec_caps remote{}, config{};
                oa2dp_codec_local_caps(OA2DP_CODEC_LDAC, &remote);
                REQUIRE(oa2dp_codec_select(nullptr, &remote, &settings, &config) == OA2DP_OK);
                oa2dp_encoder *encoder = nullptr;
                REQUIRE(oa2dp_encoder_create(&config, &settings, mtu, &encoder) == OA2DP_OK);
                const auto info = encoder->info;
                REQUIRE(info.sample_rate == rate && info.samples_per_call == 128);
                REQUIRE(info.samples_per_frame == (rate > 48000 ? 256u : 128u));
                Packetizer p(OA2DP_CODEC_LDAC, mtu, info.samples_per_frame, true);
                std::vector<int32_t> pcm(info.samples_per_call * info.channels, 0);
                std::vector<uint8_t> out(info.max_output);
                uint32_t represented = 0;
                unsigned multi = 0;
                for (int call = 0; call < 48; ++call) {
                    size_t len = 0;
                    uint8_t frames = 0;
                    REQUIRE(oa2dp_encoder_encode(encoder, pcm.data(), out.data(), out.size(), &len,
                                                 &frames) == OA2DP_OK);
                    if (!len || !frames)
                        continue;
                    multi += frames > 1;
                    p.add(std::vector<uint8_t>(out.begin(), out.begin() + len), frames);
                    represented += frames * info.samples_per_frame;
                }
                std::printf(
                    "actual LDAC %u Hz/%u bit MTU%zu: %zu packets, %u multi-frame outputs\n", rate,
                    bits, mtu, p.sent.size(), multi);
                REQUIRE(!p.sent.empty() && multi);
                verify_ldac(p, represented);
                oa2dp_encoder_destroy(encoder);
            }
}
#endif
int main(int argc, char **argv) {
#ifdef XM6_TEST_WITH_CODECS
    if (argc == 2 && !std::strcmp(argv[1], "--ldac")) {
        real_ldac();
        return 0;
    }
#else
    (void)argc;
    (void)argv;
#endif
    synthetic();
#ifdef XM6_TEST_WITH_CODECS
    real_ldac();
#endif
    std::puts("production service RTP timestamp/tail-before-wait checks passed (transport and waits controlled)");
}
