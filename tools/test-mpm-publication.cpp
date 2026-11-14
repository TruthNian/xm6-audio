/* SPDX-License-Identifier: MIT */
/* Production policy callbacks/status publishers against fake external APIs.
 * Deliberate reentry checks local handoff ordering; it does not emulate MPM. */
#include "../driver/common/channel_lifecycle.h"
#include "../driver/common/mpm_lifecycle.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define REQUIRE(x)                                                                                 \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                   \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)
#define NTAPI
#define NT_SUCCESS(s) ((s) >= 0)
#define TraceLoggingWrite(...) ((void)0)
#define KdPrintEx(...) ((void)0)
#define NT_ASSERT(x) REQUIRE(x)
#define _IRQL_requires_(x)
#define _In_
#define _In_opt_
#define _Inout_
#define _Out_
#define UNREFERENCED_PARAMETER(x) (void)(x)
#define RtlZeroMemory(p, n) std::memset(p, 0, n)
#define RtlCopyMemory(d, s, n) std::memcpy(d, s, n)
using NTSTATUS = int32_t;
using ULONG = uint32_t;
using BOOLEAN = bool;
using PVOID = void *;
using VOID = void;
constexpr BOOLEAN TRUE = true, FALSE = false;
constexpr NTSTATUS STATUS_SUCCESS = 0, STATUS_DEVICE_NOT_READY = -1;
constexpr NTSTATUS STATUS_DELETE_PENDING = -2, STATUS_UNSUCCESSFUL = -3;
constexpr NTSTATUS STATUS_DEVICE_BUSY = -4;
constexpr NTSTATUS STATUS_PENDING = 259, STATUS_CANCELLED = -5;
constexpr NTSTATUS STATUS_BUFFER_TOO_SMALL = -6, STATUS_SHARING_VIOLATION = -7;
constexpr int IO_NO_INCREMENT = 0;
constexpr int PASSIVE_LEVEL = 0;
using WDFWORKITEM = void *;
using WDFDEVICE = void *;
using EVT_WDF_WORKITEM = VOID(WDFWORKITEM);
enum OA2DP_CHANNEL_KIND { Oa2dpChannelSignaling, Oa2dpChannelMedia, Oa2dpChannelCount };
struct FakeLock {
    bool Held = false;
};
struct FakeRundown {
    bool Allowed = true;
    unsigned Active = 0;
};
struct FakeEvent {
    bool Signaled = true;
};
struct OA2DP_FORMAT_CHANGE {
    ULONG Format, PcmSequence, Running, Prepared, ConnectionEnabled, ConnectionSerial;
    ULONG SuspendRequested, SuspendSerial;
};
struct FakeFile {
    unsigned References = 1;
};
using PFILE_OBJECT = FakeFile *;
static FakeFile primary_file, replacement_file;
struct FakeIrp {
    OA2DP_FORMAT_CHANGE Payload{};
    struct {
        void *SystemBuffer;
    } AssociatedIrp{&Payload};
    struct {
        NTSTATUS Status = STATUS_PENDING;
        size_t Information = 0;
    } IoStatus;
    bool Canceled = false, Queued = false, Pending = false;
    unsigned Completions = 0;
    struct {
        PFILE_OBJECT FileObject = &primary_file;
    } Stack;
};
using PIRP = FakeIrp *;
struct IO_CSQ {
    std::vector<PIRP> Queue;
};
using PIO_CSQ = IO_CSQ *;
using KIRQL = unsigned;
struct OA2DP_AUDIO_DEVICE {
    FakeLock Lock, ControlLock;
    PFILE_OBJECT ControlFile = &primary_file;
    int Ring = 0;
    IO_CSQ WaitCsq;
    bool Started = false, Removed = false, Running = false, Prepared = false;
    bool Connected = true;
    bool ConnectionEnabled = true, SuspendRequested = false, EventPending = false;
    ULONG Current = 42, PcmSequence = 2, ConnectionSerial = 0, SuspendSerial = 0;
    OA2DP_FORMAT_CHANGE Event{};
};
using OA2DP_VOLUME_NOTIFY = VOID (*)(PVOID);
using OA2DP_POLICY_REQUEST = NTSTATUS (*)(PVOID, BOOLEAN, BOOLEAN, ULONG, PIRP *);
struct OA2DP_CHANNEL {
    OA2DP_CHANNEL_LIFECYCLE Life{};
};
struct OA2DP_DEVICE_CONTEXT {
    FakeLock Spin, Publication;
    FakeLock *Lock = &Spin, *PublicationLock = &Publication;
    FakeRundown MpmCalls, MpmCallbacks;
    void *MpmHandle = this, *PublicationWork = this, *AudioContext = nullptr;
    OA2DP_CHANNEL Channels[Oa2dpChannelCount];
    bool MpmStopping = false, TransportStopping = false;
    bool PolicyConnected = true, PolicySuspended = false;
    bool MpmConnected = false, MpmIsSuspended = false;
    ULONG MpmSuspendConnection = 0;
    OA2DP_MPM_REQUEST MpmRequest{}, MpmSuspendRequest{};
    ULONG PublishedConnection = 0;
    FakeEvent DisconnectedEvent;
    OA2DP_VOLUME_NOTIFY AudioVolumeNotify = nullptr;
    OA2DP_POLICY_REQUEST AudioPolicyRequest = nullptr;
};
static OA2DP_DEVICE_CONTEXT *device_under_test;
static OA2DP_AUDIO_DEVICE child;
static void (*before_insert)(), (*after_insert)(), (*completion_hook)();
static PIRP replacement_waiter;
static unsigned enqueues, callback_enqueues, policy_calls;
static ULONG replacement_serial;
static bool commit_on_acquire;
enum class Reentry { None, Same, Opposite, Generation, Stop };
static Reentry reentry;
static NTSTATUS external_status;
static std::vector<unsigned> confirmations;
static BOOLEAN ExAcquireRundownProtection(FakeRundown *r) {
    if (!r->Allowed)
        return false;
    ++r->Active;
    return true;
}
static void ExReleaseRundownProtection(FakeRundown *r) {
    REQUIRE(r->Active);
    --r->Active;
}
static void WdfSpinLockAcquire(FakeLock *l) {
    REQUIRE(!l->Held);
    l->Held = true;
}
static void WdfSpinLockRelease(FakeLock *l) {
    REQUIRE(l->Held);
    l->Held = false;
}
static void KeAcquireSpinLock(FakeLock *l, KIRQL *irql) {
    *irql = 0;
    WdfSpinLockAcquire(l);
}
static void KeReleaseSpinLock(FakeLock *l, KIRQL) { WdfSpinLockRelease(l); }
static void IoCompleteRequest(PIRP r, int) {
    REQUIRE(!device_under_test->Spin.Held && !child.Lock.Held);
    REQUIRE(!r->Queued && !r->Completions && r->AssociatedIrp.SystemBuffer == &r->Payload);
    ++r->Completions;
    if (completion_hook) {
        const auto hook = completion_hook;
        completion_hook = nullptr;
        hook();
    }
}
static auto IoGetCurrentIrpStackLocation(PIRP r) -> decltype(&r->Stack) {
    REQUIRE(!r->Completions);
    return &r->Stack;
}
static void ObReferenceObject(PFILE_OBJECT f) {
    REQUIRE(f && f->References);
    ++f->References;
}
static void ObDereferenceObject(PFILE_OBJECT f) {
    REQUIRE(f && f->References > 1 && !child.Lock.Held && !device_under_test->Spin.Held);
    --f->References;
}
// clang-format off
#include "wait-cancel-under-test.inc"
#include "wait-snapshot-under-test.inc"
#include "wait-status-under-test.inc"
// clang-format on
static PIRP IoCsqRemoveNextIrp(PIO_CSQ q, void *file) {
    size_t index = 0;
    while (index < q->Queue.size() && file && q->Queue[index]->Stack.FileObject != file)
        ++index;
    if (index == q->Queue.size())
        return nullptr;
    const auto r = q->Queue[index];
    q->Queue.erase(q->Queue.begin() + index);
    REQUIRE(r->Queued && !r->Canceled && !r->Completions);
    r->Queued = false;
    return r;
}
static void IoCsqInsertIrp(PIO_CSQ q, PIRP r, void *) {
    REQUIRE(!device_under_test->Spin.Held);
    REQUIRE(!r->Queued && !r->Completions);
    if (before_insert) {
        const auto hook = before_insert;
        before_insert = nullptr;
        hook();
    }
    r->Pending = true;
    if (r->Canceled) {
        CsqCompleteCanceled(q, r);
        return;
    }
    q->Queue.push_back(r);
    r->Queued = true;
    if (after_insert) {
        const auto hook = after_insert;
        after_insert = nullptr;
        hook();
    }
    // A producer/canceller may already have completed r; no further access.
}
// clang-format off
static void Oa2dpRingSetConsumerAvailable(int *ring, bool available) {
    REQUIRE(child.Lock.Held); *ring = available ? 1 : 0;
}
#include "policy-apply-under-test.inc"
#include "wait-format-under-test.inc"
#include "wait-fail-under-test.inc"
// clang-format on
static void Oa2dpLock(FakeLock *l) { WdfSpinLockAcquire(l); }
static void Oa2dpUnlock(FakeLock *l) { WdfSpinLockRelease(l); }
static void Oa2dpRingUnmapUser(int *) { REQUIRE(!child.Lock.Held); }
static NTSTATUS Oa2dpSetConnected(OA2DP_AUDIO_DEVICE *, BOOLEAN, ULONG) {
    REQUIRE(!child.Lock.Held && !child.ControlLock.Held);
    return STATUS_SUCCESS;
}
#include "wait-cleanup-under-test.inc"
static void WdfWaitLockAcquire(FakeLock *l, void *) {
    REQUIRE(!l->Held);
    if (commit_on_acquire) {
        // Model the preceding publisher finishing while this worker waits
        // for its publication lock, before the acquisition returns.
        device_under_test->PublishedConnection = device_under_test->Channels[0].Life.Generation;
        commit_on_acquire = false;
    }
    l->Held = true;
}
static void WdfWaitLockRelease(FakeLock *l) {
    REQUIRE(l->Held);
    l->Held = false;
}
static void KeClearEvent(FakeEvent *e) {
    REQUIRE(device_under_test->Spin.Held);
    e->Signaled = false;
}
static void KeSetEvent(FakeEvent *e, int, bool) {
    REQUIRE(device_under_test->Spin.Held);
    e->Signaled = true;
}
static int KeGetCurrentIrql() { return PASSIVE_LEVEL; }
static void *WdfWorkItemGetParentObject(WDFWORKITEM work) { return work; }
static OA2DP_DEVICE_CONTEXT *Oa2dpGetDevice(WDFDEVICE d) {
    return static_cast<OA2DP_DEVICE_CONTEXT *>(d);
}
static void WdfWorkItemEnqueue(void *work) {
    auto &d = *device_under_test;
    REQUIRE(work == d.PublicationWork && !d.Spin.Held);
    // Teardown must not pass rundown and finish its final flush before a
    // status publisher/callback can issue its last enqueue.
    REQUIRE(d.MpmCalls.Active || d.MpmCallbacks.Active);
    ++enqueues;
}
static NTSTATUS apply_policy(PVOID, BOOLEAN enabled, BOOLEAN suspended, ULONG serial,
                             PIRP *completion) {
    REQUIRE(device_under_test->Spin.Held);
    ++policy_calls;
    return Oa2dpApplyPolicy(&child, enabled, suspended, serial, completion);
}
static NTSTATUS BtaMpmUpdateConnectionStatus(PVOID, ULONG, ULONG);
static NTSTATUS BtaMpmUpdateSuspendStatus(PVOID, ULONG, ULONG);
static NTSTATUS BtaMpmConnectionRequest(PVOID, ULONG, ULONG);

// Production fragments have the same declaration order as mpm.c.
// clang-format off
#include "mpm-callbacks-under-test.inc"
#include "mpm-request-under-test.inc"
#include "mpm-connection-under-test.inc"
#include "mpm-suspend-under-test.inc"
#include "mpm-ack-suspend-under-test.inc"
#include "publication-validation-under-test.inc"
#include "policy-registration-under-test.inc"
// clang-format on

// Registry/interface publication is fake; use the actual manager publisher.
static NTSTATUS PublishState(OA2DP_DEVICE_CONTEXT *d, BOOLEAN value, ULONG connection) {
    REQUIRE(d->Publication.Held && !d->Spin.Held);
    return Oa2dpMpmConnected(d, value, connection);
}
#include "publication-work-under-test.inc"

static NTSTATUS external_update(PVOID handle, ULONG value, ULONG requested, bool suspend) {
    auto &d = *device_under_test;
    REQUIRE(handle == d.MpmHandle && d.MpmCalls.Active == 1 && !d.Spin.Held);
    REQUIRE(d.Publication.Held); // real callers serialize passive publishers
    confirmations.push_back(requested);
    const auto inject = reentry;
    reentry = Reentry::None;
    if (inject == Reentry::Stop)
        d.MpmStopping = true;
    else if (inject != Reentry::None) {
        const auto target = inject == Reentry::Opposite ? !value : value;
        REQUIRE((suspend ? SuspendRequest(&d, target) : ConnectionRequest(&d, target)) ==
                STATUS_SUCCESS);
        REQUIRE(!d.MpmCallbacks.Active);
        replacement_serial = suspend ? d.MpmSuspendRequest.Serial : d.MpmRequest.Serial;
        callback_enqueues = enqueues;
        if (inject == Reentry::Generation) {
            for (auto &ch : d.Channels) {
                ++ch.Life.Generation;
                ++ch.Life.ConnectionGeneration;
            }
        }
    }
    return external_status;
}
static NTSTATUS BtaMpmUpdateConnectionStatus(PVOID h, ULONG v, ULONG r) {
    return external_update(h, v, r, false);
}
static NTSTATUS BtaMpmUpdateSuspendStatus(PVOID h, ULONG v, ULONG r) {
    return external_update(h, v, r, true);
}
static NTSTATUS BtaMpmConnectionRequest(PVOID handle, ULONG, ULONG automatic) {
    REQUIRE(handle == device_under_test->MpmHandle && device_under_test->MpmCalls.Active == 1);
    REQUIRE(!device_under_test->Spin.Held && !automatic);
    return external_status;
}
static void reset(OA2DP_DEVICE_CONTEXT &d, bool open = true) {
    REQUIRE(child.WaitCsq.Queue.empty());
    REQUIRE(primary_file.References == 1 && replacement_file.References == 1);
    child = {};
    before_insert = after_insert = completion_hook = nullptr;
    replacement_waiter = nullptr;
    d = {};
    // The local fixture owns the object in-place; no copied self pointers.
    d.Lock = &d.Spin;
    d.PublicationLock = &d.Publication;
    d.MpmHandle = d.PublicationWork = &d;
    d.AudioPolicyRequest = apply_policy;
    device_under_test = &d;
    enqueues = callback_enqueues = policy_calls = replacement_serial = 0;
    commit_on_acquire = false;
    reentry = Reentry::None;
    external_status = STATUS_SUCCESS;
    confirmations.clear();
    for (auto &ch : d.Channels) {
        ch.Life.Generation = ch.Life.ConnectionGeneration = 7;
        ch.Life.State = open ? ChannelOpen : ChannelClosed;
    }
}
static NTSTATUS connected(OA2DP_DEVICE_CONTEXT &d, bool value) {
    WdfWaitLockAcquire(d.PublicationLock, nullptr);
    const auto s = Oa2dpMpmConnected(&d, value, value ? d.Channels[0].Life.Generation : 0);
    WdfWaitLockRelease(d.PublicationLock);
    REQUIRE(!d.MpmCalls.Active && !d.MpmCallbacks.Active && !d.Spin.Held);
    return s;
}
static NTSTATUS suspended(OA2DP_DEVICE_CONTEXT &d, bool value, ULONG serial, ULONG connection = 7) {
    WdfWaitLockAcquire(d.PublicationLock, nullptr);
    const auto s = PublishSuspended(&d, value, serial, connection);
    WdfWaitLockRelease(d.PublicationLock);
    REQUIRE(!d.MpmCalls.Active && !d.MpmCallbacks.Active && !d.Spin.Held);
    return s;
}
static void policy_during_insert() {
    REQUIRE(SuspendRequest(device_under_test, true) == STATUS_SUCCESS);
}
static void cancel_queued_waiter() {
    REQUIRE(!child.WaitCsq.Queue.empty());
    const auto r = child.WaitCsq.Queue.front();
    child.WaitCsq.Queue.erase(child.WaitCsq.Queue.begin());
    r->Queued = false;
    r->Canceled = true;
    CsqCompleteCanceled(&child.WaitCsq, r);
}
static void unregister_during_completion() {
    auto &d = *device_under_test;
    BusUnregisterAudio(&d, d.AudioContext);
    child.Removed = true;
}
static void cleanup_during_insert() { ControlCleanup(&child, &primary_file); }
static void replace_consumer_during_insert() {
    cleanup_during_insert();
    child.ControlFile = &replacement_file;
    replacement_waiter->Stack.FileObject = &replacement_file;
    REQUIRE(WaitFormat(&child, replacement_waiter, sizeof(replacement_waiter->Payload)) ==
            STATUS_PENDING);
}
static void policy_delivery_and_waits(OA2DP_DEVICE_CONTEXT &d) {
    for (unsigned path = 0; path != 4; ++path) {
        reset(d);
        child.Started = true;
        d.AudioContext = &child;
        FakeIrp r;
        REQUIRE(WaitFormat(&child, &r, sizeof(r.Payload)) == STATUS_PENDING && r.Queued);
        if (path == 0)
            REQUIRE(ConnectionRequest(&d, false) == STATUS_SUCCESS);
        if (path == 1)
            REQUIRE(SuspendRequest(&d, true) == STATUS_SUCCESS);
        if (path == 2)
            REQUIRE(Oa2dpMpmRequestConnection(&d, false) == STATUS_SUCCESS);
        if (path == 3) {
            completion_hook = unregister_during_completion;
            REQUIRE(BusRegisterAudio(&d, &child, nullptr, apply_policy) == STATUS_SUCCESS);
            REQUIRE(!d.AudioContext && !d.AudioPolicyRequest && child.Removed);
        }
        REQUIRE(r.Completions == 1 && r.Pending && !r.Queued && child.WaitCsq.Queue.empty());
        REQUIRE(r.IoStatus.Status == STATUS_SUCCESS && r.IoStatus.Information == sizeof(r.Payload));
        REQUIRE(r.Payload.Format == 42 && r.Payload.PcmSequence == 2 &&
                r.Payload.ConnectionSerial == 1);
        REQUIRE(!d.Spin.Held && !child.Lock.Held && !d.MpmCalls.Active && !d.MpmCallbacks.Active);
        if (path == 0 || path == 2)
            REQUIRE(!r.Payload.ConnectionEnabled);
        if (path == 1)
            REQUIRE(r.Payload.SuspendRequested &&
                    r.Payload.SuspendSerial == d.MpmSuspendRequest.Serial);
    }

    reset(d);
    child.Removed = true;
    REQUIRE(BusRegisterAudio(&d, &child, nullptr, apply_policy) == STATUS_DELETE_PENDING);
    REQUIRE(!d.AudioContext && !d.AudioPolicyRequest && child.WaitCsq.Queue.empty());

    reset(d);
    REQUIRE(ConnectionRequest(&d, false) == STATUS_SUCCESS && child.EventPending);
    child.Started = true;
    {
        FakeIrp r;
        REQUIRE(WaitFormat(&child, &r, sizeof(r.Payload)) == STATUS_SUCCESS);
        REQUIRE(r.Completions == 1 && !r.Payload.ConnectionEnabled && !child.EventPending);
    }

    reset(d);
    child.Started = true;
    {
        FakeIrp r;
        r.Canceled = true;
        REQUIRE(WaitFormat(&child, &r, sizeof(r.Payload)) == STATUS_PENDING);
        REQUIRE(r.Completions == 1 && r.IoStatus.Status == STATUS_CANCELLED &&
                child.WaitCsq.Queue.empty());
    }

    reset(d);
    child.Started = true;
    before_insert = policy_during_insert;
    {
        FakeIrp r;
        REQUIRE(WaitFormat(&child, &r, sizeof(r.Payload)) == STATUS_PENDING);
        REQUIRE(r.Completions == 1 && r.Payload.SuspendRequested && !child.EventPending);
    }

    reset(d);
    child.Started = true;
    after_insert = policy_during_insert;
    {
        FakeIrp r;
        REQUIRE(WaitFormat(&child, &r, sizeof(r.Payload)) == STATUS_PENDING);
        REQUIRE(r.Completions == 1 && r.Payload.SuspendRequested && child.WaitCsq.Queue.empty());
    }

    reset(d);
    child.Started = true;
    before_insert = [] { child.Started = false; };
    {
        FakeIrp r;
        REQUIRE(WaitFormat(&child, &r, sizeof(r.Payload)) == STATUS_PENDING);
        REQUIRE(r.Completions == 1 && r.IoStatus.Status == STATUS_DEVICE_NOT_READY);
    }

    reset(d);
    child.Started = true;
    before_insert = policy_during_insert;
    {
        FakeIrp canceled;
        canceled.Canceled = true;
        REQUIRE(WaitFormat(&child, &canceled, sizeof(canceled.Payload)) == STATUS_PENDING);
        REQUIRE(canceled.Completions == 1 && canceled.IoStatus.Status == STATUS_CANCELLED &&
                child.EventPending);
        FakeIrp next;
        REQUIRE(WaitFormat(&child, &next, sizeof(next.Payload)) == STATUS_SUCCESS);
        REQUIRE(next.Completions == 1 && next.Payload.SuspendRequested && !child.EventPending);
    }

    reset(d);
    child.Started = true;
    after_insert = cancel_queued_waiter;
    {
        FakeIrp r;
        REQUIRE(WaitFormat(&child, &r, sizeof(r.Payload)) == STATUS_PENDING);
        REQUIRE(r.Completions == 1 && r.IoStatus.Status == STATUS_CANCELLED);
    }

    for (bool inserted : {false, true}) {
        reset(d);
        child.Started = true;
        if (inserted)
            after_insert = cleanup_during_insert;
        else
            before_insert = cleanup_during_insert;
        FakeIrp r;
        REQUIRE(WaitFormat(&child, &r, sizeof(r.Payload)) == STATUS_PENDING);
        REQUIRE(r.Completions == 1 && r.IoStatus.Status == STATUS_CANCELLED && !child.ControlFile);
        REQUIRE(child.WaitCsq.Queue.empty() && primary_file.References == 1);
    }
    reset(d);
    child.Started = true;
    {
        FakeIrp old, current;
        replacement_waiter = &current;
        before_insert = replace_consumer_during_insert;
        REQUIRE(WaitFormat(&child, &old, sizeof(old.Payload)) == STATUS_PENDING);
        REQUIRE(old.Completions == 1 && old.IoStatus.Status == STATUS_CANCELLED);
        REQUIRE(!current.Completions && current.Queued && child.ControlFile == &replacement_file);
        REQUIRE(SuspendRequest(&d, true) == STATUS_SUCCESS);
        REQUIRE(current.Completions == 1 && current.Payload.SuspendRequested &&
                child.WaitCsq.Queue.empty());
        REQUIRE(primary_file.References == 1 && replacement_file.References == 1);
    }
    reset(d);
    child.Started = true;
    {
        FakeIrp old, current;
        // Model an old insertion still in flight after cleanup and a fresh
        // consumer already queued. Production must select the current file.
        old.Queued = old.Pending = true;
        child.WaitCsq.Queue.push_back(&old);
        child.ControlFile = &replacement_file;
        current.Stack.FileObject = &replacement_file;
        REQUIRE(WaitFormat(&child, &current, sizeof(current.Payload)) == STATUS_PENDING);
        REQUIRE(SuspendRequest(&d, true) == STATUS_SUCCESS && current.Completions == 1 &&
                !old.Completions);
        ControlCleanup(&child, &primary_file);
        REQUIRE(old.Completions == 1 && old.IoStatus.Status == STATUS_CANCELLED);
        REQUIRE(child.ControlFile == &replacement_file && child.WaitCsq.Queue.empty());
    }

    reset(d);
    child.Started = true;
    {
        FakeIrp first, second;
        REQUIRE(WaitFormat(&child, &first, sizeof(first.Payload)) == STATUS_PENDING);
        before_insert = policy_during_insert;
        REQUIRE(WaitFormat(&child, &second, sizeof(second.Payload)) == STATUS_PENDING);
        REQUIRE(first.Completions == 1 && first.Payload.ConnectionSerial == 1 &&
                !second.Completions);
        REQUIRE(SuspendRequest(&d, false) == STATUS_SUCCESS);
        REQUIRE(second.Completions == 1 && second.Payload.ConnectionSerial == 2 &&
                !second.Payload.SuspendRequested);
    }

    reset(d);
    child.Started = true;
    {
        FakeIrp r;
        REQUIRE(WaitFormat(&child, &r, sizeof(r.Payload) - 1) == STATUS_BUFFER_TOO_SMALL &&
                r.Completions == 1);
    }
    reset(d);
    {
        FakeIrp r;
        REQUIRE(WaitFormat(&child, &r, sizeof(r.Payload)) == STATUS_DEVICE_NOT_READY &&
                r.Completions == 1);
    }
    REQUIRE(child.WaitCsq.Queue.empty());
}
int main() {
    OA2DP_DEVICE_CONTEXT d;
    reset(d);
    REQUIRE(connected(d, true) == STATUS_SUCCESS && !enqueues && confirmations.back() == 0);
    reset(d);
    REQUIRE(suspended(d, true, 0) == STATUS_SUCCESS && !enqueues && confirmations.back() == 0);
    for (bool target : {false, true}) {
        reset(d, target);
        d.MpmConnected = !target;
        REQUIRE(ConnectionRequest(&d, target) == STATUS_SUCCESS);
        const auto old = d.MpmRequest.Serial;
        REQUIRE(!enqueues);
        reentry = Reentry::Same;
        REQUIRE(connected(d, target) == STATUS_SUCCESS);
        REQUIRE(replacement_serial != old && d.MpmRequest.Pending && !callback_enqueues);
        REQUIRE(enqueues == 1 && d.MpmConnected == target);
        REQUIRE(connected(d, target) == STATUS_SUCCESS); // queued re-acknowledgement
        REQUIRE(!d.MpmRequest.Pending && enqueues == 1 &&
                confirmations == std::vector<unsigned>({1, 1}));
    }
    // Even an unsolicited state update can be adopted by a new MPM request
    // while its callback still sees the old state cache.
    reset(d);
    reentry = Reentry::Same;
    REQUIRE(connected(d, true) == STATUS_SUCCESS && enqueues == 1 && d.MpmRequest.Pending);
    REQUIRE(connected(d, true) == STATUS_SUCCESS && !d.MpmRequest.Pending);
    REQUIRE(confirmations == std::vector<unsigned>({0, 1}));

    reset(d);
    REQUIRE(ConnectionRequest(&d, true) == STATUS_SUCCESS);
    reentry = Reentry::Opposite;
    REQUIRE(connected(d, true) == STATUS_SUCCESS);
    REQUIRE(d.MpmRequest.Pending && !d.MpmRequest.Target && !enqueues);

    reset(d);
    REQUIRE(ConnectionRequest(&d, true) == STATUS_SUCCESS);
    reentry = Reentry::Generation;
    REQUIRE(connected(d, true) == STATUS_SUCCESS && d.MpmRequest.Pending && !enqueues);
    REQUIRE(d.Channels[0].Life.Generation == 8); // old link cannot confirm replacement readiness

    reset(d);
    d.MpmConnected = true;
    REQUIRE(ConnectionRequest(&d, false) == STATUS_SUCCESS);
    d.Channels[0].Life.State = ChannelClosing;
    reentry = Reentry::Same;
    REQUIRE(connected(d, false) == STATUS_SUCCESS && d.MpmRequest.Pending && !enqueues);
    REQUIRE(confirmations.back() == 0); // readiness withdrawal isn't a completed Disconnect

    reset(d);
    REQUIRE(ConnectionRequest(&d, true) == STATUS_SUCCESS);
    reentry = Reentry::Same;
    external_status = STATUS_UNSUCCESSFUL;
    REQUIRE(connected(d, true) == STATUS_UNSUCCESSFUL && !d.MpmConnected && d.MpmRequest.Pending &&
            !enqueues);
    reset(d);
    reentry = Reentry::Stop;
    REQUIRE(connected(d, true) == STATUS_SUCCESS && !enqueues);
    reset(d);
    d.MpmCalls.Allowed = false;
    REQUIRE(connected(d, true) == STATUS_DEVICE_NOT_READY && confirmations.empty());
    reset(d, false);
    d.MpmHandle = nullptr;
    REQUIRE(connected(d, false) == STATUS_SUCCESS && confirmations.empty());

    for (bool target : {false, true}) {
        reset(d);
        d.MpmIsSuspended = !target;
        d.PolicySuspended = !target;
        REQUIRE(SuspendRequest(&d, target) == STATUS_SUCCESS);
        const auto old = d.MpmSuspendRequest.Serial;
        REQUIRE(!enqueues);
        reentry = Reentry::Same;
        REQUIRE(suspended(d, target, old) == STATUS_SUCCESS);
        REQUIRE(replacement_serial != old && d.MpmSuspendRequest.Pending && !callback_enqueues);
        REQUIRE(enqueues == 1 && d.MpmIsSuspended == target);
        Oa2dpMpmAcknowledgeSuspend(&d); // actual production worker acknowledgement path
        REQUIRE(!d.MpmSuspendRequest.Pending && enqueues == 1 && !d.MpmCalls.Active);
        REQUIRE(confirmations == std::vector<unsigned>({1, 1}));
    }
    reset(d);
    reentry = Reentry::Same;
    REQUIRE(suspended(d, true, 0) == STATUS_SUCCESS && enqueues == 1 &&
            d.MpmSuspendRequest.Pending);
    Oa2dpMpmAcknowledgeSuspend(&d);
    REQUIRE(!d.MpmSuspendRequest.Pending && confirmations == std::vector<unsigned>({0, 1}));

    reset(d);
    REQUIRE(SuspendRequest(&d, true) == STATUS_SUCCESS);
    reentry = Reentry::Opposite;
    REQUIRE(suspended(d, true, d.MpmSuspendRequest.Serial) == STATUS_SUCCESS);
    REQUIRE(d.MpmSuspendRequest.Pending && !d.MpmSuspendRequest.Target &&
            enqueues == callback_enqueues);

    reset(d);
    REQUIRE(SuspendRequest(&d, true) == STATUS_SUCCESS);
    reentry = Reentry::Generation;
    REQUIRE(suspended(d, true, d.MpmSuspendRequest.Serial) == STATUS_SUCCESS && !enqueues &&
            d.MpmSuspendRequest.Pending);
    reset(d);
    REQUIRE(SuspendRequest(&d, true) == STATUS_SUCCESS);
    reentry = Reentry::Same;
    external_status = STATUS_UNSUCCESSFUL;
    REQUIRE(suspended(d, true, d.MpmSuspendRequest.Serial) == STATUS_UNSUCCESSFUL);
    REQUIRE(!d.MpmIsSuspended && d.MpmSuspendConnection == 0 && !enqueues &&
            d.MpmSuspendRequest.Pending);
    reset(d);
    reentry = Reentry::Stop;
    REQUIRE(suspended(d, true, 0) == STATUS_SUCCESS && !enqueues);

    reset(d);
    d.PolicySuspended = true;
    REQUIRE(suspended(d, false, 0) == STATUS_DEVICE_NOT_READY && confirmations.empty());
    reset(d);
    REQUIRE(suspended(d, true, 0, 6) == STATUS_DEVICE_NOT_READY && confirmations.empty());
    reset(d);
    d.MpmCalls.Allowed = false;
    REQUIRE(suspended(d, true, 0) == STATUS_DEVICE_NOT_READY && confirmations.empty());

    for (bool target : {false, true}) {
        reset(d);
        d.MpmIsSuspended = !target;
        d.PolicySuspended = !target;
        REQUIRE(SuspendRequest(&d, target) == STATUS_SUCCESS);
        reentry = Reentry::Generation;
        REQUIRE(suspended(d, target, d.MpmSuspendRequest.Serial) == STATUS_SUCCESS);
        REQUIRE(d.MpmSuspendRequest.Pending && !enqueues);
        const auto sent = confirmations.size();
        Oa2dpMpmAcknowledgeSuspend(&d);
        REQUIRE(d.MpmSuspendRequest.Pending && confirmations.size() == sent);
        // A later request also cannot adopt an accepted state from link 7
        // merely because the new link 8 has the same suspend/resume target.
        REQUIRE(SuspendRequest(&d, target) == STATUS_SUCCESS && !enqueues);
        REQUIRE(suspended(d, target, d.MpmSuspendRequest.Serial, 8) == STATUS_SUCCESS);
        REQUIRE(!d.MpmSuspendRequest.Pending && d.MpmSuspendConnection == 8);
    }
    reset(d);
    d.MpmIsSuspended = true; // disconnected startup baseline, not link 7's state
    REQUIRE(SuspendRequest(&d, true) == STATUS_SUCCESS && !enqueues);
    Oa2dpMpmAcknowledgeSuspend(&d);
    REQUIRE(d.MpmSuspendRequest.Pending && confirmations.empty());
    reset(d, false);
    d.MpmIsSuspended = true;
    REQUIRE(SuspendRequest(&d, true) == STATUS_SUCCESS && enqueues == 1);
    Oa2dpMpmAcknowledgeSuspend(&d);
    REQUIRE(!d.MpmSuspendRequest.Pending && d.MpmSuspendConnection == 0);

    reset(d);
    REQUIRE(ConnectionRequest(&d, true) == STATUS_SUCCESS && !enqueues);
    commit_on_acquire = true;
    PublishWorkItem(&d); // must snapshot the committed epoch after lock acquisition
    REQUIRE(!d.MpmRequest.Pending && d.PublishedConnection == 7 && !d.DisconnectedEvent.Signaled);
    REQUIRE(confirmations == std::vector<unsigned>({1}) && !commit_on_acquire);
    REQUIRE(!d.Publication.Held && !d.MpmCalls.Active);

    reset(d, false);
    d.MpmConnected = true;
    d.PublishedConnection = 7;
    REQUIRE(ConnectionRequest(&d, false) == STATUS_SUCCESS);
    PublishWorkItem(&d);
    REQUIRE(!d.MpmRequest.Pending && !d.MpmConnected && !d.PublishedConnection &&
            d.DisconnectedEvent.Signaled);
    REQUIRE(confirmations == std::vector<unsigned>({1}));

    reset(d);
    d.PublishedConnection = 6;
    REQUIRE(ConnectionRequest(&d, true) == STATUS_SUCCESS);
    PublishWorkItem(&d); // worker cannot invent service readiness for an unaccepted link
    REQUIRE(d.MpmRequest.Pending && confirmations.empty());
    policy_delivery_and_waits(d);
    std::puts("production MPM reentry, policy completion and waiter checks passed (all external "
              "APIs fake)");
}
