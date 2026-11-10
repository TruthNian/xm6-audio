/* SPDX-License-Identifier: MIT */
#include "oa2dp/pcm_format.h"
#include "oa2dp/pcm_ring.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#define REQUIRE(x)                                                                                 \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                   \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)
#define _Inout_
#define _In_
#define _In_reads_bytes_(n)
#define _Out_writes_(n)
using NTSTATUS = int32_t;
using ULONG = uint32_t;
using USHORT = uint16_t;
using UCHAR = uint8_t;
using CHAR = int8_t;
using SHORT = int16_t;
using LONG = int32_t;
using BOOLEAN = bool;
using KIRQL = int;
using PVOID = void *;
using VOID = void;
using OA2DP_FORMAT = oa2dp_pcm_format;
enum KPROCESSOR_MODE { KernelMode, UserMode };
constexpr NTSTATUS STATUS_SUCCESS = 0, STATUS_INVALID_PARAMETER = -1, STATUS_DEVICE_NOT_READY = -2;
#define NT_SUCCESS(s) ((s) >= 0)
constexpr bool FALSE = false, TRUE = true;
constexpr int IO_NO_INCREMENT = 0, EVENT_MODIFY_STATE = 2;
constexpr unsigned RING_FRAMES = 8, OA2DP_RING_MAX_CHANNELS = 2, OA2DP_PUMP_CHUNK = 256;
struct FakeEvent {
    int references = 0, signals = 0;
    bool signalled = false, valid = true;
};
using PKEVENT = FakeEvent *;
using HANDLE = FakeEvent *;
static void *event_type = nullptr;
static void **ExEventObjectType = &event_type;
static bool locked = false;
static NTSTATUS ObReferenceObjectByHandle(HANDLE h, int rights, void *, KPROCESSOR_MODE mode,
                                          void **out, void *) {
    REQUIRE(mode == UserMode && rights == EVENT_MODIFY_STATE);
    if (!h->valid)
        return STATUS_INVALID_PARAMETER;
    ++h->references;
    *out = h;
    return 0;
}
static void ObDereferenceObject(FakeEvent *e) {
    REQUIRE(!locked && e->references > 0);
    --e->references;
}
static void KeAcquireSpinLock(int *, KIRQL *level) {
    REQUIRE(!locked);
    locked = true;
    *level = 0;
}
static void KeReleaseSpinLock(int *, KIRQL) {
    REQUIRE(locked);
    locked = false;
}
static void KeSetEvent(FakeEvent *e, int, bool wait) {
    REQUIRE(locked && !wait && e->references > 0);
    e->signalled = true;
    ++e->signals;
}
static LONG InterlockedOr(volatile LONG *p, LONG b) {
    auto v = *p;
    *p = v | b;
    return v;
}
static LONG InterlockedAnd(volatile LONG *p, LONG b) {
    auto v = *p;
    *p = v & b;
    return v;
}
struct OA2DP_AUDIO_RING {
    int Lock = 0;
    oa2dp_pcm_ring *Ring = nullptr;
    PKEVENT DataEvent = nullptr;
    ULONG MinimumFrames = 0;
    int32_t Scratch[512]{};
};
#include "pcm-notification-under-test.inc"
static bool wait(FakeEvent &e) {
    bool signalled = e.signalled;
    e.signalled = false;
    return signalled;
}
int main() {
    std::vector<uint64_t> storage((oa2dp_ring_bytes(8, 2) + 7) / 8);
    OA2DP_AUDIO_RING r;
    r.Ring = reinterpret_cast<oa2dp_pcm_ring *>(storage.data());
    REQUIRE(oa2dp_ring_init(r.Ring, 8, 48000, 2, OA2DP_RING_FMT_FLOAT32) == 0);
    OA2DP_FORMAT f{48000, 2, 32, 32, OA2DP_PCM_FLOAT};
    FakeEvent a, b, bad;
    bad.valid = false;
    REQUIRE(Oa2dpRingSetDataEvent(&r, &a, 4, UserMode) == 0 && a.references == 1);
    float samples[8]{.1f, .2f, .3f, .4f, .5f, .6f, .7f, .8f}, out[8]{};
    Oa2dpRingPump(&r, reinterpret_cast<UCHAR *>(samples), sizeof(samples), 0, 2, &f);
    REQUIRE(!wait(a));
    oa2dp_ring_format format{};
    REQUIRE(oa2dp_ring_snapshot(r.Ring, &format) == 0);
    uint32_t count = 0;
    REQUIRE(oa2dp_ring_read(r.Ring, &format, out, sizeof(out), 4, &count) == 0 && !count);
    // Producer commits after the empty read, before the consumer waits.
    Oa2dpRingPump(&r, reinterpret_cast<UCHAR *>(samples), sizeof(samples), 16, 2, &f);
    REQUIRE(wait(a));
    REQUIRE(oa2dp_ring_read(r.Ring, &format, out, sizeof(out), 4, &count) == 0 && count == 4);
    REQUIRE(out[0] == samples[0] && out[7] == samples[7]);
    REQUIRE(!wait(a));
    Oa2dpRingPump(&r, reinterpret_cast<UCHAR *>(samples), sizeof(samples), 0, 4, &f);
    // Data was ready before event replacement: registration must signal it.
    REQUIRE(Oa2dpRingSetDataEvent(&r, &b, 4, UserMode) == 0 && a.references == 0 &&
            b.references == 1);
    REQUIRE(wait(b));
    REQUIRE(Oa2dpRingSetDataEvent(&r, &bad, 4, UserMode) != 0 && r.DataEvent == &b);
    REQUIRE(Oa2dpRingSetDataEvent(&r, &a, 0, UserMode) != 0 && r.DataEvent == &b);
    REQUIRE(Oa2dpRingSetDataEvent(&r, &a, 9, UserMode) != 0 && r.DataEvent == &b);
    REQUIRE(Oa2dpRingSetFormat(&r, 44100, 2, OA2DP_RING_FMT_FLOAT32) && wait(b));
    REQUIRE(oa2dp_ring_read(r.Ring, &format, out, sizeof(out), 4, &count) == OA2DP_ERR_STATE);
    REQUIRE(Oa2dpRingSetFormat(&r, 48000, 2, OA2DP_RING_FMT_FLOAT32) && wait(b));
    oa2dp_ring_format returned{};
    REQUIRE(oa2dp_ring_snapshot(r.Ring, &returned) == 0);
    REQUIRE(returned.sample_rate == format.sample_rate && returned.channels == format.channels &&
            returned.format == format.format && returned.sequence != format.sequence);
    REQUIRE(oa2dp_ring_read(r.Ring, &format, out, sizeof(out), 4, &count) == OA2DP_ERR_STATE);
    REQUIRE(Oa2dpRingSetFormat(&r, 44100, 2, OA2DP_RING_FMT_FLOAT32) && wait(b));
    int signals = b.signals;
    Oa2dpRingPump(&r, reinterpret_cast<UCHAR *>(samples), sizeof(samples), 0, 4, &f);
    REQUIRE(b.signals == signals); // stale-rate stream cannot write or notify
    Oa2dpRingSetRunning(&r, FALSE);
    REQUIRE(wait(b));
    REQUIRE(Oa2dpRingSetDataEvent(&r, nullptr, 0, KernelMode) == 0 && b.references == 0);
    f.SampleRate = 44100;
    Oa2dpRingPump(&r, reinterpret_cast<UCHAR *>(samples), sizeof(samples), 0, 4, &f);
    REQUIRE(!b.signalled && b.signals == signals + 1);
    r.Ring = nullptr;
    REQUIRE(Oa2dpRingSetDataEvent(&r, &a, 4, UserMode) == STATUS_DEVICE_NOT_READY &&
            a.references == 0);
    std::puts("production PCM notification ordering, thresholds and reference checks passed (OS "
              "APIs fake)");
}
