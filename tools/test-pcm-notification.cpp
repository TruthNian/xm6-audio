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
static bool nested_ring_lock = false;
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
    REQUIRE(locked && !nested_ring_lock);
    locked = false;
}
static void KeAcquireSpinLockAtDpcLevel(int *) {
    REQUIRE(locked && !nested_ring_lock); nested_ring_lock = true;
}
static void KeReleaseSpinLockFromDpcLevel(int *) {
    REQUIRE(locked && nested_ring_lock); nested_ring_lock = false;
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
    OA2DP_FORMAT SourceFormat{};
    bool SourceFormatValid = false;
};
#include "pcm-notification-under-test.inc"
struct OA2DP_FORMAT_CHANGE {
    OA2DP_FORMAT Format{};
    ULONG PcmSequence = 0;
    bool Running = false;
};
using PIRP = void *;
struct OA2DP_AUDIO_DEVICE {
    int Lock = 0, WaitCsq = 0;
    OA2DP_AUDIO_RING Ring;
    OA2DP_FORMAT Current{};
    ULONG PcmSequence = 0;
    bool Running = false, Started = false, EventPending = false;
    void *ControlFile = nullptr;
    OA2DP_FORMAT_CHANGE Event;
    struct {
        struct { void *Context = nullptr; } Header;
        void (*SetPlaying)(void *, bool) = nullptr;
    } Bus;
};
static void SnapshotStateLocked(OA2DP_AUDIO_DEVICE *dev, OA2DP_FORMAT_CHANGE *ev) {
    REQUIRE(locked && !nested_ring_lock);
    ev->Format = dev->Current; ev->PcmSequence = dev->PcmSequence; ev->Running = dev->Running;
}
static PIRP IoCsqRemoveNextIrp(int *, void *) { REQUIRE(locked); return nullptr; }
static void CompleteWait(PIRP, const OA2DP_FORMAT_CHANGE *) { REQUIRE(!locked); }
#include "pcm-report-under-test.inc"
static bool wait(FakeEvent &e) {
    bool signalled = e.signalled;
    e.signalled = false;
    return signalled;
}
static void integer_pump() {
    for (USHORT bits : {16, 24, 32}) {
        for (USHORT channels : {1, 2}) {
            std::vector<uint64_t> storage((oa2dp_ring_bytes(8, 2) + 7) / 8);
            OA2DP_AUDIO_RING r;
            r.Ring = reinterpret_cast<oa2dp_pcm_ring *>(storage.data());
            const auto type = bits == 32 ? OA2DP_RING_FMT_S32_FULL : OA2DP_RING_FMT_S32;
            REQUIRE(oa2dp_ring_init(r.Ring, 8, 48000, 2, type) == 0);
            OA2DP_FORMAT format{48000, channels, bits, bits, OA2DP_PCM_INTEGER};
            REQUIRE(Oa2dpRingSetFormat(&r, &format));
            const int32_t values[] = {-32768, 32767, -1, 0, 1, -1234, 1234, 42};
            const size_t width = bits / 8, frame_bytes = channels * width;
            std::vector<UCHAR> input(8 * frame_bytes);
            for (size_t i = 0; i < 8u * channels; ++i) {
                const auto value = static_cast<uint32_t>(values[i % 8]);
                for (size_t b = 0; b < width; ++b)
                    input[i * width + b] = static_cast<UCHAR>(value >> (8 * b));
            }
            FakeEvent event;
            REQUIRE(Oa2dpRingSetDataEvent(&r, &event, 4, UserMode) == 0);
            Oa2dpRingPump(&r, input.data(), static_cast<ULONG>(input.size()),
                          static_cast<ULONG>(6 * frame_bytes), 4, &format);
            REQUIRE(wait(event));
            oa2dp_ring_format snapshot{};
            int32_t output[16]{};
            uint32_t count = 0;
            REQUIRE(oa2dp_ring_snapshot(r.Ring, &snapshot) == 0);
            REQUIRE(oa2dp_ring_read(r.Ring, &snapshot, output, sizeof(output), 4, &count) == 0 &&
                    count == 4);
            for (size_t i = 0; i < 4u * channels; ++i) {
                const size_t source = ((6 + i / channels) % 8) * channels + i % channels;
                REQUIRE(output[i] == values[source % 8] * (bits == 16 ? 256 : 1));
            }
            Oa2dpRingPump(&r, input.data(), static_cast<ULONG>(input.size()), 0, 8, &format);
            REQUIRE(wait(event));
            Oa2dpRingPump(&r, input.data(), static_cast<ULONG>(input.size()), 0, 4, &format);
            REQUIRE(wait(event) && oa2dp_ring_overruns(r.Ring) == 4 &&
                    oa2dp_ring_readable(r.Ring) == 8);
            REQUIRE(oa2dp_ring_read(r.Ring, &snapshot, output, sizeof(output), 8, &count) == 0 &&
                    count == 8);
            for (size_t i = 0; i < 8u * channels; ++i)
                REQUIRE(output[i] == values[i % 8] * (bits == 16 ? 256 : 1));
            REQUIRE(Oa2dpRingSetDataEvent(&r, nullptr, 0, KernelMode) == 0 && !event.references);
        }
    }
}
static void precision_epoch() {
    const USHORT widths[][4] = {{16, 16, 24, 24}, {32, 16, 32, 24}, {32, 24, 32, 32},
                                {24, 20, 24, 24}, {32, 24, 32, 20}, {16, 16, 24, 16}};
    for (const auto &width : widths) {
        std::vector<uint64_t> storage((oa2dp_ring_bytes(8, 2) + 7) / 8);
        OA2DP_AUDIO_RING r;
        r.Ring = reinterpret_cast<oa2dp_pcm_ring *>(storage.data());
        OA2DP_FORMAT before{48000, 2, width[0], width[1], OA2DP_PCM_INTEGER};
        OA2DP_FORMAT after{48000, 2, width[2], width[3], OA2DP_PCM_INTEGER};
        REQUIRE(oa2dp_ring_init(r.Ring, 8, 48000, 2, oa2dp_pcm_format_ring_type(&before)) == 0);
        (void)Oa2dpRingSetFormat(&r, &before);
        oa2dp_ring_format old_snapshot{}, new_snapshot{};
        REQUIRE(oa2dp_ring_snapshot(r.Ring, &old_snapshot) == 0);
        const bool changed = Oa2dpRingSetFormat(&r, &after);
        REQUIRE(oa2dp_ring_snapshot(r.Ring, &new_snapshot) == 0);
        UCHAR samples[32]{};
        Oa2dpRingPump(&r, samples, sizeof(samples), 0, 2, &after);
        int32_t output[4]{}; uint32_t count = 0;
        const int read_status = oa2dp_ring_read(r.Ring, &old_snapshot, output, sizeof(output), 2, &count);
        if (read_status != OA2DP_ERR_STATE)
            std::fprintf(stderr, "precision %u/%u -> %u/%u: changed=%u, epoch=%u->%u, old read=%d frames=%u\n",
                width[0], width[1], width[2], width[3], changed, old_snapshot.sequence, new_snapshot.sequence, read_status, count);
        REQUIRE(read_status == OA2DP_ERR_STATE && count == 0);
        REQUIRE(changed && old_snapshot.sequence != new_snapshot.sequence);
        const auto written = r.Ring->write_pos;
        Oa2dpRingPump(&r, samples, sizeof(samples), 0, 2, &before);
        REQUIRE(r.Ring->write_pos == written); // obsolete producer metadata cannot enter the new epoch
        REQUIRE(!Oa2dpRingSetFormat(&r, &after));
        REQUIRE(r.Ring->format_sequence == new_snapshot.sequence);
        REQUIRE(Oa2dpRingSetFormat(&r, &before));
        REQUIRE(r.Ring->format_sequence != old_snapshot.sequence && r.Ring->format_sequence != new_snapshot.sequence);
        REQUIRE(oa2dp_ring_read(r.Ring, &old_snapshot, output, sizeof(output), 2, &count) == OA2DP_ERR_STATE);

        OA2DP_AUDIO_DEVICE dev;
        dev.Ring.Ring = r.Ring;
        unsigned publications = 0;
        dev.Bus.Header.Context = &publications;
        dev.Bus.SetPlaying = [](void *context, bool) { REQUIRE(!locked); ++*static_cast<unsigned *>(context); };
        Oa2dpReportStream(&dev, &before, TRUE);
        REQUIRE(!dev.EventPending && publications == 0); // no committed producer format yet
        REQUIRE(Oa2dpRingSetFormat(&dev.Ring, &after));
        FakeEvent notification;
        REQUIRE(Oa2dpRingSetDataEvent(&dev.Ring, &notification, 1, UserMode) == 0);
        Oa2dpReportStream(&dev, &after, TRUE);
        REQUIRE(dev.Running && dev.EventPending && (dev.Ring.Ring->flags & OA2DP_RING_FLAG_RUNNING));
        REQUIRE(dev.PcmSequence == dev.Ring.Ring->format_sequence && publications == 1);
        REQUIRE(wait(notification));
        dev.EventPending = false;
        Oa2dpReportStream(&dev, &before, FALSE);
        REQUIRE(dev.Running && !dev.EventPending && (dev.Ring.Ring->flags & OA2DP_RING_FLAG_RUNNING));
        REQUIRE(oa2dp_pcm_format_same(&dev.Current, &after) && publications == 1 && !wait(notification));
        Oa2dpReportStream(&dev, &after, FALSE);
        REQUIRE(!dev.Running && dev.EventPending && !(dev.Ring.Ring->flags & OA2DP_RING_FLAG_RUNNING));
        REQUIRE(!dev.Event.Running && oa2dp_pcm_format_same(&dev.Event.Format, &after));
        REQUIRE(publications == 2 && wait(notification));
        REQUIRE(Oa2dpRingSetDataEvent(&dev.Ring, nullptr, 0, KernelMode) == 0 && !notification.references);
    }
}
int main() {
    precision_epoch();
    std::vector<uint64_t> storage((oa2dp_ring_bytes(8, 2) + 7) / 8);
    OA2DP_AUDIO_RING r;
    r.Ring = reinterpret_cast<oa2dp_pcm_ring *>(storage.data());
    REQUIRE(oa2dp_ring_init(r.Ring, 8, 48000, 2, OA2DP_RING_FMT_FLOAT32) == 0);
    OA2DP_FORMAT f{48000, 2, 32, 32, OA2DP_PCM_FLOAT};
    REQUIRE(Oa2dpRingSetFormat(&r, &f));
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
    OA2DP_FORMAT rate_change = f; rate_change.SampleRate = 44100;
    REQUIRE(Oa2dpRingSetFormat(&r, &rate_change) && wait(b));
    REQUIRE(oa2dp_ring_read(r.Ring, &format, out, sizeof(out), 4, &count) == OA2DP_ERR_STATE);
    REQUIRE(Oa2dpRingSetFormat(&r, &f) && wait(b));
    oa2dp_ring_format returned{};
    REQUIRE(oa2dp_ring_snapshot(r.Ring, &returned) == 0);
    REQUIRE(returned.sample_rate == format.sample_rate && returned.channels == format.channels &&
            returned.format == format.format && returned.sequence != format.sequence);
    REQUIRE(oa2dp_ring_read(r.Ring, &format, out, sizeof(out), 4, &count) == OA2DP_ERR_STATE);
    REQUIRE(Oa2dpRingSetFormat(&r, &rate_change) && wait(b));
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
    integer_pump();
    std::puts("production PCM precision epochs, stale report rejection, conversion, notification ordering, thresholds and reference checks "
              "passed (OS "
              "APIs fake)");
}
