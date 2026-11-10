/* SPDX-License-Identifier: MIT */
/* Production WaveRT clock, packet, state and buffer-release bodies with fake
 * OS/port APIs. No timer, audio device or installed driver is accessed. */
#include "oa2dp/pcm_format.h"
#include <cstddef>
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
#define _In_
#define _In_opt_
#define _Out_
#define _IRQL_requires_max_(x)
#define _IRQL_limited_to_(x)
#define STDMETHODCALLTYPE
#define STDMETHODIMP_(t) t
#define UNREFERENCED_PARAMETER(x) (void)(x)
#define ASSERT(x) REQUIRE(x)
#define CONTAINING_RECORD(p, type, field)                                                          \
    reinterpret_cast<type *>(reinterpret_cast<char *>(p) - offsetof(type, field))
#ifdef ULONG_MAX
#undef ULONG_MAX
#endif
#define ULONG_MAX UINT32_MAX
using NTSTATUS = int32_t;
using ULONG = uint32_t;
using DWORD = uint32_t;
using LONG = int32_t;
using ULONGLONG = uint64_t;
using UINT64 = uint64_t;
using LONGLONG = int64_t;
using BOOLEAN = bool;
using BYTE = uint8_t;
using VOID = void;
using PVOID = void *;
using PMDL = void *;
using KIRQL = int;
using OA2DP_FORMAT = oa2dp_pcm_format;
constexpr bool TRUE = true, FALSE = false;
constexpr NTSTATUS STATUS_SUCCESS = 0, STATUS_NOT_SUPPORTED = -1, STATUS_INVALID_DEVICE_STATE = -2;
constexpr NTSTATUS STATUS_DATA_LATE_ERROR = -3, STATUS_DATA_OVERRUN = -4,
                   STATUS_INVALID_PARAMETER = -5;
constexpr ULONGLONG OA2DP_HNS_PER_SECOND = 10000000, OA2DP_HNS_PER_MS = 10000;
constexpr DWORD KSSTREAM_HEADER_OPTIONSF_ENDOFSTREAM = 0x200;
enum KSSTATE : int { KSSTATE_STOP, KSSTATE_ACQUIRE, KSSTATE_PAUSE, KSSTATE_RUN };
struct LARGE_INTEGER {
    LONGLONG QuadPart;
};
struct KSAUDIO_POSITION {
    ULONGLONG PlayOffset, WriteOffset;
};
struct KSAUDIO_PRESENTATION_POSITION {
    ULONGLONG u64PositionInBlocks, u64QPCPosition;
};
struct LIST_ENTRY {
    LIST_ENTRY *Flink, *Blink;
};
using PLIST_ENTRY = LIST_ENTRY *;
struct FakeEvent {
    unsigned Signals = 0;
};
using PKEVENT = FakeEvent *;
struct NotificationListEntry {
    LIST_ENTRY ListEntry;
    PKEVENT NotificationEvent;
};
struct FakeTimer {
    bool Active = false;
    unsigned Sets = 0, Cancels = 0;
};
using PEX_TIMER = FakeTimer *;
struct FakeRing {
    bool Running = false;
};
struct OA2DP_AUDIO_DEVICE {
    FakeRing Ring;
    std::vector<bool> Reports;
};
static bool locked, mapped;
static LONGLONG now;
static void (*before_lock)();
static void KeAcquireSpinLock(int *, KIRQL *level) {
    REQUIRE(!locked);
    if (before_lock) {
        const auto hook = before_lock;
        before_lock = nullptr;
        hook();
    }
    locked = true;
    *level = 0;
}
static void KeReleaseSpinLock(int *, KIRQL) {
    REQUIRE(locked);
    locked = false;
}
static LARGE_INTEGER KeQueryPerformanceCounter(LARGE_INTEGER *frequency) {
    if (frequency)
        frequency->QuadPart = OA2DP_HNS_PER_SECOND;
    return {now};
}
static ULONGLONG performance_hns(ULONGLONG frequency, LARGE_INTEGER time) {
    const auto ticks = static_cast<ULONGLONG>(time.QuadPart);
    return ticks / frequency * OA2DP_HNS_PER_SECOND +
           ticks % frequency * OA2DP_HNS_PER_SECOND / frequency;
}
#define KSCONVERT_PERFORMANCE_TIME(f, t) performance_hns(f, t)
static void KeSetEvent(PKEVENT e, int, bool wait) {
    REQUIRE(locked && !wait);
    ++e->Signals;
}
static void ExCancelTimer(PEX_TIMER t, void *) {
    REQUIRE(!locked);
    t->Active = false;
    ++t->Cancels;
}
static void ExSetTimer(PEX_TIMER t, LONGLONG due, ULONGLONG period, void *) {
    REQUIRE(!locked && due == -static_cast<LONGLONG>(OA2DP_HNS_PER_MS) &&
            period == OA2DP_HNS_PER_MS);
    t->Active = true;
    ++t->Sets;
}
static void Oa2dpRingSetRunning(FakeRing *r, bool running) {
    REQUIRE(!locked);
    r->Running = running;
}
static void Oa2dpReportStream(OA2DP_AUDIO_DEVICE *d, const OA2DP_FORMAT *, bool running) {
    REQUIRE(!locked);
    d->Reports.push_back(running);
}
static ULONG Oa2dpLatencyUs(OA2DP_AUDIO_DEVICE *) {
    REQUIRE(!locked);
    return 45000;
}
struct PumpCall {
    ULONG Position, Frames;
};
static std::vector<PumpCall> pumped;
static void Oa2dpRingPump(FakeRing *, const BYTE *, ULONG bytes, ULONG pos, ULONG frames,
                          const OA2DP_FORMAT *f) {
    REQUIRE(locked && mapped && bytes && pos < bytes);
    REQUIRE(frames <= bytes / (f->Channels * (f->BitsPerSample / 8)));
    pumped.push_back({pos, frames});
}
struct FakePort {
    unsigned Unmapped = 0, Freed = 0;
    void UnmapAllocatedPages(BYTE *, PMDL) {
        REQUIRE(!locked && mapped);
        mapped = false;
        ++Unmapped;
    }
    void FreePagesFromMdl(PMDL) {
        REQUIRE(!locked && !mapped);
        ++Freed;
    }
};
class CMiniportWaveRTStream {
  public:
    ULONGLONG RunningHns(LARGE_INTEGER);
    VOID Pump(ULONGLONG);
    VOID UpdatePosition(LARGE_INTEGER);
    VOID NotifyPackets();
    NTSTATUS GetPosition(KSAUDIO_POSITION *);
    NTSTATUS SetWritePacket(ULONG, DWORD, ULONG);
    NTSTATUS GetOutputStreamPresentationPosition(KSAUDIO_PRESENTATION_POSITION *);
    NTSTATUS GetPacketCount(ULONG *);
    NTSTATUS SetState(KSSTATE);
    VOID FreeAudioBuffer(PMDL, ULONG);
    OA2DP_AUDIO_DEVICE *m_Device;
    FakePort *m_pPortStream;
    OA2DP_FORMAT m_Format;
    ULONG m_ulBlockAlign, m_ulDmaBufferSize, m_ulNotificationsPerBuffer;
    BYTE *m_pDmaBuffer;
    LIST_ENTRY m_NotificationList;
    PEX_TIMER m_pNotificationTimer;
    int m_PositionSpinLock = 0;
    KSSTATE m_KsState;
    LARGE_INTEGER m_PerfFrequency, m_RunStartQpc;
    ULONGLONG m_hnsRunBase, m_ullLinearFrames, m_ullPumpedFrames, m_ullEosFrame;
    LONGLONG m_llPacketCounter;
    ULONG m_ulLastOsWritePacket;
    BOOLEAN m_bEoSReceived;
};
// Preserve production dependency order.
// clang-format off
#include "wavert-time-under-test.inc"
#include "wavert-clock-under-test.inc"
#include "wavert-methods-under-test.inc"
#include "wavert-free-under-test.inc"
#include "wavert-timer-under-test.inc"
// clang-format on
static BYTE buffer[7680]; // 20 ms, 48 kHz stereo float32
static FakeEvent notification;
static NotificationListEntry entry;
static FakeTimer timer;
static FakePort port;
static OA2DP_AUDIO_DEVICE device;
static CMiniportWaveRTStream *current;
static void reset(CMiniportWaveRTStream &s, KSSTATE state = KSSTATE_RUN) {
    REQUIRE(!locked);
    s = {};
    device = {};
    timer = {};
    port = {};
    notification = {};
    locked = false;
    mapped = true;
    now = 0;
    before_lock = nullptr;
    pumped.clear();
    current = &s;
    s.m_Device = &device;
    s.m_pPortStream = &port;
    s.m_Format = {48000, 2, 32, 32, OA2DP_PCM_FLOAT};
    s.m_ulBlockAlign = 8;
    s.m_pDmaBuffer = buffer;
    s.m_ulDmaBufferSize = sizeof(buffer);
    s.m_ulNotificationsPerBuffer = 2;
    s.m_PerfFrequency.QuadPart = OA2DP_HNS_PER_SECOND;
    s.m_KsState = state;
    s.m_ulLastOsWritePacket = ULONG_MAX;
    s.m_pNotificationTimer = &timer;
    s.m_NotificationList.Flink = s.m_NotificationList.Blink = &entry.ListEntry;
    entry.ListEntry.Flink = entry.ListEntry.Blink = &s.m_NotificationList;
    entry.NotificationEvent = &notification;
}
int main() {
    CMiniportWaveRTStream s;
    reset(s);
    now = 30 * OA2DP_HNS_PER_MS;
    ULONG packets = 99;
    REQUIRE(s.GetPacketCount(&packets) == STATUS_SUCCESS && packets == 3);
    REQUIRE(s.m_ullLinearFrames == 1440 && notification.Signals == 1);
    REQUIRE(pumped.size() == 1 && pumped[0].Position == 3840 && pumped[0].Frames == 960);
    TimerNotifyRT(&timer, &s);
    REQUIRE(notification.Signals == 1 && pumped.size() == 1);
    now = 40 * OA2DP_HNS_PER_MS;
    TimerNotifyRT(&timer, &s);
    REQUIRE(s.GetPacketCount(&packets) == STATUS_SUCCESS && packets == 4 &&
            notification.Signals == 2);
    reset(s);
    now = 30 * OA2DP_HNS_PER_MS;
    REQUIRE(s.SetWritePacket(4, 0, 0) == STATUS_SUCCESS && s.m_llPacketCounter == 3);
    reset(s);
    REQUIRE(s.SetWritePacket(1, 0x80000000u, 0) == STATUS_INVALID_PARAMETER);
    reset(s, KSSTATE_PAUSE);
    REQUIRE(s.SetWritePacket(0, KSSTREAM_HEADER_OPTIONSF_ENDOFSTREAM, 1) ==
            STATUS_INVALID_PARAMETER);
    REQUIRE(s.SetWritePacket(0, KSSTREAM_HEADER_OPTIONSF_ENDOFSTREAM, 0) == STATUS_SUCCESS);
    REQUIRE(s.SetWritePacket(0, 0, 0) == STATUS_INVALID_DEVICE_STATE);
    reset(s);
    before_lock = [] { current->FreeAudioBuffer(&port, sizeof(buffer)); };
    REQUIRE(s.SetWritePacket(1, 0, 0) == STATUS_NOT_SUPPORTED && port.Freed == 1 &&
            port.Unmapped == 1);
    reset(s);
    before_lock = [] { current->FreeAudioBuffer(&port, sizeof(buffer)); };
    REQUIRE(s.GetPacketCount(&packets) == STATUS_NOT_SUPPORTED && !mapped);
    reset(s);
    KSAUDIO_PRESENTATION_POSITION presentation{};
    before_lock = [] { current->FreeAudioBuffer(&port, sizeof(buffer)); };
    REQUIRE(s.GetOutputStreamPresentationPosition(&presentation) == STATUS_NOT_SUPPORTED &&
            !mapped);
    TimerNotifyRT(&timer, &s); // a callback already queued before buffer release is harmless
    REQUIRE(pumped.empty());

    reset(s);
    now = 30 * OA2DP_HNS_PER_MS;
    KSAUDIO_POSITION position{};
    REQUIRE(s.GetPosition(&position) == STATUS_SUCCESS && s.m_llPacketCounter == 3 &&
            notification.Signals == 1);
    reset(s);
    now = 30 * OA2DP_HNS_PER_MS;
    REQUIRE(s.GetOutputStreamPresentationPosition(&presentation) == STATUS_SUCCESS);
    REQUIRE(s.m_llPacketCounter == 3 && notification.Signals == 1 &&
            presentation.u64QPCPosition == static_cast<UINT64>(now));

    reset(s);
    s.m_ulNotificationsPerBuffer = 0;
    now = 5 * OA2DP_HNS_PER_MS;
    REQUIRE(s.GetPosition(&position) == STATUS_SUCCESS && s.m_ullLinearFrames == 240);
    REQUIRE(!notification.Signals && pumped.size() == 1 && pumped[0].Frames == 240);
    REQUIRE(s.GetPacketCount(&packets) == STATUS_NOT_SUPPORTED &&
            s.SetWritePacket(0, 0, 0) == STATUS_NOT_SUPPORTED);

    reset(s);
    s.m_Format.SampleRate = 44100;
    now = OA2DP_HNS_PER_MS;
    REQUIRE(s.GetPosition(&position) == STATUS_SUCCESS && s.m_ullLinearFrames == 44);
    now = 1000 * OA2DP_HNS_PER_MS;
    REQUIRE(s.GetPacketCount(&packets) == STATUS_SUCCESS && s.m_ullLinearFrames == 44100 &&
            packets == 91);
    const auto calls = pumped.size();
    TimerNotifyRT(&timer, &s);
    REQUIRE(pumped.size() == calls); // no re-copy at the same timestamp

    reset(s);
    now = static_cast<LONGLONG>((static_cast<UINT64>(UINT32_MAX) + 2) * 10 * OA2DP_HNS_PER_MS);
    REQUIRE(s.GetPacketCount(&packets) == STATUS_SUCCESS && packets == 1);
    REQUIRE(s.m_llPacketCounter == static_cast<LONGLONG>(UINT32_MAX) + 2);
    REQUIRE(s.SetWritePacket(2, 0, 0) == STATUS_SUCCESS); // OS packet numbers wrap at 32 bits

    reset(s);
    now = 30 * OA2DP_HNS_PER_MS;
    REQUIRE(s.SetWritePacket(1, 0, 0) == STATUS_DATA_LATE_ERROR);
    REQUIRE(s.SetWritePacket(5, 0, 0) == STATUS_DATA_OVERRUN);
    REQUIRE(s.SetWritePacket(4, 0, 0) == STATUS_SUCCESS);

    reset(s, KSSTATE_PAUSE);
    REQUIRE(s.SetWritePacket(0, 0, 0) == STATUS_SUCCESS);
    REQUIRE(s.SetState(KSSTATE_RUN) == STATUS_SUCCESS);
    REQUIRE(s.SetWritePacket(1, KSSTREAM_HEADER_OPTIONSF_ENDOFSTREAM, 16) == STATUS_SUCCESS &&
            s.m_ullEosFrame == 482);
    now = 15 * OA2DP_HNS_PER_MS;
    TimerNotifyRT(&timer, &s);
    REQUIRE(pumped.size() == 1 && pumped[0].Frames == 482);
    now = 25 * OA2DP_HNS_PER_MS;
    TimerNotifyRT(&timer, &s);
    REQUIRE(pumped.size() == 1); // nothing read or copied past the valid EOS boundary

    reset(s, KSSTATE_STOP);
    REQUIRE(s.SetState(KSSTATE_ACQUIRE) == STATUS_SUCCESS &&
            s.SetState(KSSTATE_PAUSE) == STATUS_SUCCESS);
    now = 3 * OA2DP_HNS_PER_MS;
    REQUIRE(s.SetState(KSSTATE_RUN) == STATUS_SUCCESS && timer.Active && device.Ring.Running);
    REQUIRE(s.SetState(KSSTATE_RUN) == STATUS_SUCCESS && timer.Sets == 1 &&
            device.Reports.size() == 1);
    now = 8 * OA2DP_HNS_PER_MS;
    REQUIRE(s.GetPosition(&position) == STATUS_SUCCESS && s.m_ullLinearFrames == 240);
    REQUIRE(s.SetState(KSSTATE_PAUSE) == STATUS_SUCCESS && !timer.Active && !device.Ring.Running);
    now = 108 * OA2DP_HNS_PER_MS;
    TimerNotifyRT(&timer, &s);
    REQUIRE(s.m_ullLinearFrames == 240 && s.SetState(KSSTATE_RUN) == STATUS_SUCCESS);
    now = 113 * OA2DP_HNS_PER_MS;
    REQUIRE(s.GetPacketCount(&packets) == STATUS_SUCCESS && packets == 1 &&
            s.m_ullLinearFrames == 480);
    REQUIRE(s.SetWritePacket(2, KSSTREAM_HEADER_OPTIONSF_ENDOFSTREAM, 0) == STATUS_SUCCESS);
    REQUIRE(s.SetState(KSSTATE_PAUSE) == STATUS_SUCCESS &&
            s.SetState(KSSTATE_ACQUIRE) == STATUS_SUCCESS);
    REQUIRE(s.SetState(KSSTATE_STOP) == STATUS_SUCCESS);
    REQUIRE(!s.m_ullLinearFrames && !s.m_ullPumpedFrames && !s.m_llPacketCounter &&
            !s.m_hnsRunBase);
    REQUIRE(!s.m_bEoSReceived && s.m_ulLastOsWritePacket == ULONG_MAX && !timer.Active &&
            !device.Ring.Running);
    REQUIRE(s.SetState(static_cast<KSSTATE>(99)) == STATUS_INVALID_PARAMETER &&
            s.m_KsState == KSSTATE_STOP);
    REQUIRE(device.Reports == std::vector<bool>({true, false, true, false}));
    REQUIRE(!locked);
    std::puts("production WaveRT clock/packet checks passed (OS APIs fake)");
}
