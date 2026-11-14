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
#define _Out_
#define _In_reads_bytes_(n)
#define _Out_writes_(n)
using NTSTATUS = int32_t;
using ULONG = uint32_t;
using USHORT = uint16_t;
using UCHAR = uint8_t;
using CHAR = int8_t;
using SHORT = int16_t;
using LONG = int32_t;
using LONGLONG = int64_t;
using ULONGLONG = uint64_t;
using BOOLEAN = bool;
using KIRQL = int;
using PVOID = void *;
using VOID = void;
using OA2DP_FORMAT = oa2dp_pcm_format;
enum KPROCESSOR_MODE { KernelMode, UserMode };
constexpr NTSTATUS STATUS_SUCCESS = 0, STATUS_INVALID_PARAMETER = -1, STATUS_DEVICE_NOT_READY = -2;
constexpr NTSTATUS STATUS_TIMEOUT = 258, STATUS_IO_TIMEOUT = -3;
constexpr NTSTATUS STATUS_DELETE_PENDING = -4;
constexpr int Executive = 0;
struct LARGE_INTEGER {LONGLONG QuadPart;};
#define NT_SUCCESS(s) ((s) >= 0)
constexpr bool FALSE = false, TRUE = true;
constexpr int IO_NO_INCREMENT = 0, EVENT_MODIFY_STATE = 2;
constexpr unsigned RING_FRAMES = 8, OA2DP_RING_MAX_CHANNELS = 2, OA2DP_PUMP_CHUNK = 256;
constexpr ULONG MAXULONG = UINT32_MAX;
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
    REQUIRE(!nested_ring_lock);
    *level = locked ? 1 : 0;
    if (locked) nested_ring_lock = true;
    else locked = true;
}
static void KeReleaseSpinLock(int *, KIRQL level) {
    REQUIRE(locked);
    if (level) { REQUIRE(nested_ring_lock); nested_ring_lock = false; }
    else { REQUIRE(!nested_ring_lock); locked = false; }
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
static void KeClearEvent(FakeEvent *e) { REQUIRE(locked); e->signalled = false; }
static ULONGLONG interrupt_time = 0;
static void (*on_consumer_wait)() = nullptr;
static ULONGLONG KeQueryInterruptTime() { REQUIRE(!locked); return interrupt_time; }
static NTSTATUS KeWaitForSingleObject(FakeEvent *e, int, KPROCESSOR_MODE, bool, LARGE_INTEGER *timeout) {
    REQUIRE(!locked && timeout->QuadPart < 0);
    if(on_consumer_wait) { auto callback=on_consumer_wait;on_consumer_wait=nullptr;callback(); }
    if(e->signalled)return STATUS_SUCCESS;
    interrupt_time+=static_cast<ULONGLONG>(-timeout->QuadPart);
    return STATUS_TIMEOUT;
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
    FakeEvent ConsumerEvent{1};
    bool ConsumerMapped = true;
    bool ConsumerCancelled = false;
    ULONG ConsumerCancelSerial = 0;
};
#include "pcm-notification-under-test.inc"
// Existing format-focused fixtures use the current producer. Epoch-race
// fixtures below pass explicit old/new identities to the production functions.
static bool Oa2dpRingBeginStream(OA2DP_AUDIO_RING *r,const OA2DP_FORMAT *f) {
    ULONG sequence; return Oa2dpRingBeginStream(r,f,&sequence);
}
static void Oa2dpRingPump(OA2DP_AUDIO_RING *r,const UCHAR *p,ULONG bytes,ULONG pos,ULONG frames,const OA2DP_FORMAT *f) {
    Oa2dpRingPump(r,p,bytes,pos,frames,f,r->Ring->format_sequence);
}
struct OA2DP_FORMAT_CHANGE {
    OA2DP_FORMAT Format{};
    ULONG PcmSequence = 0;
    bool Running = false, Prepared = false;
};
using PIRP = void *;
struct OA2DP_AUDIO_DEVICE {
    int Lock = 0, WaitCsq = 0;
    OA2DP_AUDIO_RING Ring;
    OA2DP_FORMAT Current{};
    ULONG PcmSequence = 0;
    bool Running = false, Prepared = false, Started = true, Removed = false, EventPending = false;
    bool ConnectionEnabled = true, SuspendRequested = false;
    LONG Connected = 1;
    ULONG SuspendSerial = 0, ConnectionSerial = 0;
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
    ev->Prepared = dev->Prepared;
}
static PIRP IoCsqRemoveNextIrp(int *, void *) { REQUIRE(locked); return nullptr; }
static void CompleteWait(PIRP, const OA2DP_FORMAT_CHANGE *) { REQUIRE(!locked); }
static void PrepareWait(PIRP, const OA2DP_FORMAT_CHANGE *) { REQUIRE(!locked); }
#include "pcm-report-under-test.inc"
#include "policy-apply-under-test.inc"
static void Oa2dpReportStream(OA2DP_AUDIO_DEVICE *d,const OA2DP_FORMAT *f,bool running,ULONG sequence) {
    Oa2dpReportStream(d,f,running,sequence,running);
}
static void Oa2dpReportStream(OA2DP_AUDIO_DEVICE *d,const OA2DP_FORMAT *f,bool running) {
    Oa2dpReportStream(d,f,running,d->Ring.Ring->format_sequence);
}
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
            REQUIRE(Oa2dpRingBeginStream(&r, &format));
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
        (void)Oa2dpRingBeginStream(&r, &before);
        oa2dp_ring_format old_snapshot{}, new_snapshot{};
        REQUIRE(oa2dp_ring_snapshot(r.Ring, &old_snapshot) == 0);
        const bool changed = Oa2dpRingBeginStream(&r, &after);
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
        REQUIRE(r.Ring->format_sequence == new_snapshot.sequence);
        REQUIRE(Oa2dpRingBeginStream(&r, &before));
        REQUIRE(r.Ring->format_sequence != old_snapshot.sequence && r.Ring->format_sequence != new_snapshot.sequence);
        REQUIRE(oa2dp_ring_read(r.Ring, &old_snapshot, output, sizeof(output), 2, &count) == OA2DP_ERR_STATE);

        OA2DP_AUDIO_DEVICE dev;
        dev.Ring.Ring = r.Ring;
        unsigned publications = 0;
        dev.Bus.Header.Context = &publications;
        dev.Bus.SetPlaying = [](void *context, bool) { REQUIRE(!locked); ++*static_cast<unsigned *>(context); };
        Oa2dpReportStream(&dev, &before, TRUE);
        REQUIRE(!dev.EventPending && publications == 0); // no committed producer format yet
        REQUIRE(Oa2dpRingBeginStream(&dev.Ring, &after));
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
static void new_clip_epoch() {
    std::vector<uint64_t> storage((oa2dp_ring_bytes(8,2)+7)/8);
    OA2DP_AUDIO_RING ring;
    ring.Ring=reinterpret_cast<oa2dp_pcm_ring *>(storage.data());
    OA2DP_FORMAT format{48000,2,32,32,OA2DP_PCM_INTEGER};
    REQUIRE(oa2dp_ring_init(ring.Ring,8,48000,2,OA2DP_RING_FMT_S32_FULL)==0);
    REQUIRE(Oa2dpRingBeginStream(&ring,&format));
    oa2dp_ring_format previous{},current{};
    REQUIRE(oa2dp_ring_snapshot(ring.Ring,&previous)==0);
    int32_t tail[4]={-1,-2,-3,-4},head[4]={11,12,13,14},output[4]{};
    REQUIRE(oa2dp_ring_write(ring.Ring,tail,2)==2);
    REQUIRE(Oa2dpRingBeginStream(&ring,&format)); // exact same PCM, different clip
    REQUIRE(oa2dp_ring_snapshot(ring.Ring,&current)==0);
    REQUIRE(current.sequence!=previous.sequence && current.start_pos==2);
    REQUIRE(oa2dp_ring_write(ring.Ring,head,2)==2);
    uint32_t frames=0;
    REQUIRE(oa2dp_ring_read(ring.Ring,&previous,output,sizeof(output),2,&frames)==OA2DP_ERR_STATE);
    REQUIRE(oa2dp_ring_read(ring.Ring,&current,output,sizeof(output),2,&frames)==0);
    REQUIRE(frames==2 && output[0]==11 && output[3]==14);
}
static void obsolete_producer() {
    std::vector<uint64_t> storage((oa2dp_ring_bytes(8,2)+7)/8);
    OA2DP_AUDIO_DEVICE d;
    d.Ring.Ring=reinterpret_cast<oa2dp_pcm_ring *>(storage.data());
    OA2DP_FORMAT f{48000,2,32,32,OA2DP_PCM_INTEGER};
    REQUIRE(oa2dp_ring_init(d.Ring.Ring,8,48000,2,OA2DP_RING_FMT_S32_FULL)==0);
    ULONG old=0,current=0;
    REQUIRE(Oa2dpRingBeginStream(&d.Ring,&f,&old));
    REQUIRE(Oa2dpRingBeginStream(&d.Ring,&f,&current) && current!=old);
    Oa2dpReportStream(&d,&f,true,current);
    d.EventPending=false;
    Oa2dpReportStream(&d,&f,false,old);
    REQUIRE(d.Running && !d.EventPending && (d.Ring.Ring->flags&OA2DP_RING_FLAG_RUNNING));
    int32_t pcm[4]={1,2,3,4};
    Oa2dpRingPump(&d.Ring,reinterpret_cast<const UCHAR *>(pcm),sizeof(pcm),0,2,&f,old);
    REQUIRE(d.Ring.Ring->write_pos==0);
    Oa2dpRingEndSource(&d.Ring,old);
    REQUIRE(!(d.Ring.Ring->flags&OA2DP_RING_FLAG_EOS));
    Oa2dpRingPump(&d.Ring,reinterpret_cast<const UCHAR *>(pcm),sizeof(pcm),0,2,&f,current);
    REQUIRE(d.Ring.Ring->write_pos==2);
    Oa2dpRingEndSource(&d.Ring,current);
    REQUIRE(d.Ring.Ring->flags&OA2DP_RING_FLAG_EOS);
    Oa2dpReportStream(&d,&f,false,current);
    d.Started=false;d.EventPending=false;
    Oa2dpRingCancelConsumer(&d.Ring);
    Oa2dpReportStream(&d,&f,true,current);
    REQUIRE(!d.Running && !d.EventPending && d.Ring.ConsumerCancelled);
    oa2dp_ring_format snapshot{};
    REQUIRE(oa2dp_ring_snapshot(d.Ring.Ring,&snapshot)==0);
    REQUIRE(oa2dp_ring_admit(d.Ring.Ring,&snapshot)==0);
    REQUIRE(!Oa2dpRingConsumerReady(&d.Ring,current)); // admission cannot undo cancellation
    d.Ring.ConsumerMapped = true;
    Oa2dpRingSetConsumerAvailable(&d.Ring,TRUE);
    REQUIRE(Oa2dpRingConsumerReady(&d.Ring,current) && !Oa2dpRingConsumerReady(&d.Ring,old));
    d.Started = true;
    Oa2dpReportStream(&d,&f,false,current,true);
    REQUIRE(d.Prepared && !d.Running && d.Event.Prepared && !d.Event.Running);
    REQUIRE(!(d.Ring.Ring->flags & OA2DP_RING_FLAG_RUNNING));
    Oa2dpReportStream(&d,&f,false,current,false);
    REQUIRE(!d.Prepared && !d.Event.Prepared);
}
static OA2DP_AUDIO_RING *waiting_ring;
static OA2DP_AUDIO_DEVICE *waiting_device;
static void policy_cancels_preparation() {
    std::vector<uint64_t> memory((oa2dp_ring_bytes(8,2)+7)/8);
    OA2DP_AUDIO_DEVICE d; d.Ring.Ring=reinterpret_cast<oa2dp_pcm_ring *>(memory.data());
    REQUIRE(oa2dp_ring_init(d.Ring.Ring,8,48000,2,OA2DP_RING_FMT_FLOAT32)==0);
    OA2DP_FORMAT format{48000,2,32,32,OA2DP_PCM_FLOAT};
    REQUIRE(Oa2dpRingBeginStream(&d.Ring,&format));
    Oa2dpReportStream(&d,&format,true);
    waiting_device=&d;
    const auto before=interrupt_time;
    on_consumer_wait=[] {
        PIRP completion=nullptr;
        REQUIRE(Oa2dpApplyPolicy(waiting_device,false,false,0,&completion)==STATUS_SUCCESS);
    };
    REQUIRE(Oa2dpRingWaitConsumer(&d.Ring,d.Ring.Ring->format_sequence)==STATUS_DEVICE_NOT_READY);
    REQUIRE(interrupt_time==before); // explicit disconnect must not spend the five-second budget
    d.Running=false;
    Oa2dpReportStream(&d,&format,true);
    REQUIRE(!d.Running && d.Ring.ConsumerCancelled); // late RUN cannot undo policy
    PIRP completion=nullptr;
    REQUIRE(Oa2dpApplyPolicy(&d,true,false,0,&completion)==STATUS_SUCCESS);
    on_consumer_wait=[] {
        PIRP c=nullptr;
        REQUIRE(Oa2dpApplyPolicy(waiting_device,true,true,1,&c)==STATUS_SUCCESS);
    };
    REQUIRE(Oa2dpRingWaitConsumer(&d.Ring,d.Ring.Ring->format_sequence)==STATUS_DEVICE_NOT_READY);
    REQUIRE(interrupt_time==before); // HFP suspension also cancels preparation
    REQUIRE(Oa2dpApplyPolicy(&d,true,false,2,&completion)==STATUS_SUCCESS);
    on_consumer_wait=[] {
        PIRP c=nullptr;
        REQUIRE(Oa2dpApplyPolicy(waiting_device,false,false,2,&c)==STATUS_SUCCESS);
        REQUIRE(Oa2dpApplyPolicy(waiting_device,true,false,2,&c)==STATUS_SUCCESS);
        oa2dp_ring_format f{};
        REQUIRE(oa2dp_ring_snapshot(waiting_device->Ring.Ring,&f)==0);
        REQUIRE(oa2dp_ring_admit(waiting_device->Ring.Ring,&f)==0);
        REQUIRE(Oa2dpRingSignalConsumer(&waiting_device->Ring,f.sequence)==0);
    };
    REQUIRE(Oa2dpRingWaitConsumer(&d.Ring,d.Ring.Ring->format_sequence)==STATUS_DEVICE_NOT_READY);
    REQUIRE(interrupt_time==before); // re-enable cannot erase an already-issued cancellation
    REQUIRE(Oa2dpRingWaitConsumer(&d.Ring,d.Ring.Ring->format_sequence)==STATUS_SUCCESS);
    // A coalesced profile reversal must not retire the still-running encoder.
    REQUIRE(Oa2dpApplyPolicy(&d,true,true,3,&completion)==STATUS_SUCCESS);
    Oa2dpReportStream(&d,&format,false,d.Ring.Ring->format_sequence,true);
    REQUIRE(!d.Running && d.Prepared && d.Ring.ConsumerCancelled);
    REQUIRE(oa2dp_ring_consumer_ready(d.Ring.Ring));
    REQUIRE(Oa2dpRingSignalConsumer(&d.Ring,d.Ring.Ring->format_sequence)==STATUS_SUCCESS);
    REQUIRE(Oa2dpRingWaitConsumer(&d.Ring,d.Ring.Ring->format_sequence)==STATUS_DEVICE_NOT_READY);
    REQUIRE(Oa2dpApplyPolicy(&d,true,false,4,&completion)==STATUS_SUCCESS);
    REQUIRE(Oa2dpRingWaitConsumer(&d.Ring,d.Ring.Ring->format_sequence)==STATUS_SUCCESS);
    d.Connected=0;d.Running=false;
    Oa2dpReportStream(&d,&format,true);
    REQUIRE(!d.Running && d.Ring.ConsumerCancelled);
    REQUIRE(Oa2dpRingWaitConsumer(&d.Ring,d.Ring.Ring->format_sequence)==STATUS_DEVICE_NOT_READY);
}
static void consumer_preparation() {
    std::vector<uint64_t> memory((oa2dp_ring_bytes(8,2)+7)/8);
    OA2DP_AUDIO_RING r; r.Ring=reinterpret_cast<oa2dp_pcm_ring *>(memory.data());
    REQUIRE(oa2dp_ring_init(r.Ring,8,48000,2,OA2DP_RING_FMT_FLOAT32)==0);
    OA2DP_FORMAT format{48000,2,32,32,OA2DP_PCM_FLOAT};
    REQUIRE(Oa2dpRingBeginStream(&r,&format)); waiting_ring=&r;
    const ULONG sequence=r.Ring->format_sequence;
    REQUIRE(Oa2dpRingSignalConsumer(&r,sequence)==STATUS_DEVICE_NOT_READY);
    on_consumer_wait=[] {
        interrupt_time+=9000000;
        oa2dp_ring_format f{};REQUIRE(oa2dp_ring_snapshot(waiting_ring->Ring,&f)==0);
        REQUIRE(oa2dp_ring_admit(waiting_ring->Ring,&f)==0);
        REQUIRE(Oa2dpRingSignalConsumer(waiting_ring,f.sequence)==0);
    };
    REQUIRE(Oa2dpRingWaitConsumer(&r,sequence)==0 && interrupt_time==9000000);
    REQUIRE(Oa2dpRingWaitConsumer(&r,sequence)==0); // already ready: no blocking
    REQUIRE(Oa2dpRingBeginStream(&r,&format));
    REQUIRE(Oa2dpRingSignalConsumer(&r,sequence)==STATUS_DEVICE_NOT_READY); // late ready
    REQUIRE(Oa2dpRingWaitConsumer(&r,r.Ring->format_sequence)==STATUS_IO_TIMEOUT);
    REQUIRE(interrupt_time==59000000); // single total five-second budget
    on_consumer_wait=[] {
        waiting_ring->ConsumerMapped=false;
        KIRQL irql;KeAcquireSpinLock(&waiting_ring->Lock,&irql);
        KeSetEvent(&waiting_ring->ConsumerEvent,0,false);
        KeReleaseSpinLock(&waiting_ring->Lock,irql);
    };
    REQUIRE(Oa2dpRingWaitConsumer(&r,r.Ring->format_sequence)==STATUS_DEVICE_NOT_READY);
    r.ConsumerMapped=true;
    on_consumer_wait=[] { Oa2dpRingCancelConsumer(waiting_ring); };
    REQUIRE(Oa2dpRingWaitConsumer(&r,r.Ring->format_sequence)==STATUS_DEVICE_NOT_READY);
    REQUIRE(r.ConsumerMapped && r.ConsumerCancelled); // PnP stop preserves the mapping
}
int main() {
    consumer_preparation();
    policy_cancels_preparation();
    obsolete_producer();
    new_clip_epoch();
    precision_epoch();
    std::vector<uint64_t> storage((oa2dp_ring_bytes(8, 2) + 7) / 8);
    OA2DP_AUDIO_RING r;
    r.Ring = reinterpret_cast<oa2dp_pcm_ring *>(storage.data());
    REQUIRE(oa2dp_ring_init(r.Ring, 8, 48000, 2, OA2DP_RING_FMT_FLOAT32) == 0);
    OA2DP_FORMAT f{48000, 2, 32, 32, OA2DP_PCM_FLOAT};
    REQUIRE(Oa2dpRingBeginStream(&r, &f));
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
    REQUIRE(Oa2dpRingBeginStream(&r, &rate_change) && wait(b));
    REQUIRE(oa2dp_ring_read(r.Ring, &format, out, sizeof(out), 4, &count) == OA2DP_ERR_STATE);
    REQUIRE(Oa2dpRingBeginStream(&r, &f) && wait(b));
    oa2dp_ring_format returned{};
    REQUIRE(oa2dp_ring_snapshot(r.Ring, &returned) == 0);
    REQUIRE(returned.sample_rate == format.sample_rate && returned.channels == format.channels &&
            returned.format == format.format && returned.sequence != format.sequence);
    REQUIRE(oa2dp_ring_read(r.Ring, &format, out, sizeof(out), 4, &count) == OA2DP_ERR_STATE);
    REQUIRE(Oa2dpRingBeginStream(&r, &rate_change) && wait(b));
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
