/* SPDX-License-Identifier: MIT */
/* Production policy callbacks/status publishers against fake external APIs.
 * Deliberate reentry checks local handoff ordering; it does not emulate MPM. */
#include "../driver/common/channel_lifecycle.h"
#include "../driver/common/mpm_lifecycle.h"
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
#define NTAPI
#define NT_SUCCESS(s) ((s) >= 0)
#define TraceLoggingWrite(...) ((void)0)
#define KdPrintEx(...) ((void)0)
#define NT_ASSERT(x) REQUIRE(x)
#define _IRQL_requires_(x)
using NTSTATUS = int32_t;
using ULONG = uint32_t;
using BOOLEAN = bool;
using PVOID = void *;
using VOID = void;
constexpr BOOLEAN TRUE = true, FALSE = false;
constexpr NTSTATUS STATUS_SUCCESS = 0, STATUS_DEVICE_NOT_READY = -1;
constexpr NTSTATUS STATUS_DELETE_PENDING = -2, STATUS_UNSUCCESSFUL = -3;
constexpr NTSTATUS STATUS_DEVICE_BUSY = -4;
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
    OA2DP_MPM_REQUEST MpmRequest{}, MpmSuspendRequest{};
    ULONG PublishedConnection = 0;
    FakeEvent DisconnectedEvent;
    NTSTATUS (*AudioPolicyRequest)(PVOID, BOOLEAN, BOOLEAN, ULONG) = nullptr;
};
static OA2DP_DEVICE_CONTEXT *device_under_test;
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
static NTSTATUS apply_policy(PVOID, BOOLEAN, BOOLEAN, ULONG) {
    REQUIRE(device_under_test->Spin.Held);
    ++policy_calls;
    return STATUS_SUCCESS;
}
static NTSTATUS BtaMpmUpdateConnectionStatus(PVOID, ULONG, ULONG);
static NTSTATUS BtaMpmUpdateSuspendStatus(PVOID, ULONG, ULONG);

// Production fragments have the same declaration order as mpm.c.
// clang-format off
#include "mpm-callbacks-under-test.inc"
#include "mpm-connection-under-test.inc"
#include "mpm-suspend-under-test.inc"
#include "mpm-ack-suspend-under-test.inc"
#include "publication-validation-under-test.inc"
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
static void reset(OA2DP_DEVICE_CONTEXT &d, bool open = true) {
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
    REQUIRE(!d.MpmIsSuspended && !enqueues && d.MpmSuspendRequest.Pending);
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
    std::puts("production MPM reentry handoff checks passed (all external APIs fake)");
}
