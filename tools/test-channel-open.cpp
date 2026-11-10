/* SPDX-License-Identifier: MIT */
/* Actual outgoing OPEN/completion bodies; OS APIs are fake. This checks request
 * ownership/order, not KMDF's implementation of IRP cancellation or Bluetooth. */
#include "../driver/common/channel_lifecycle.h"
#include "../driver/common/mpm_lifecycle.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define REQUIRE(x)                                                                                 \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                   \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)
#define _In_
#define UNREFERENCED_PARAMETER(x) (void)(x)
#define NT_SUCCESS(s) ((s) >= 0)
using NTSTATUS = int32_t;
using ULONG = uint32_t;
using USHORT = uint16_t;
using ULONG_PTR = uintptr_t;
using BOOLEAN = bool;
using VOID = void;
using PVOID = void *;
using WDFCONTEXT = void *;
using WDFIOTARGET = void *;
constexpr bool TRUE = true, FALSE = false;
constexpr NTSTATUS STATUS_SUCCESS = 0, STATUS_PENDING = 259;
constexpr NTSTATUS STATUS_CANCELLED = -1, STATUS_BUFFER_TOO_SMALL = -2;
constexpr NTSTATUS STATUS_CONNECTION_DISCONNECTED = -3, STATUS_DEVICE_NOT_READY = -4;
constexpr NTSTATUS STATUS_INSUFFICIENT_RESOURCES = -5, STATUS_IO_TIMEOUT = -6;
constexpr int IO_NO_INCREMENT = 0, BRB_L2CA_OPEN_CHANNEL = 0, OA2DP_POOL_TAG = 0;
constexpr USHORT OA2DP_DEFAULT_MEDIA_MTU = 1005, OA2DP_DEFAULT_SIGNALING_MTU = 672;
enum OA2DP_CHANNEL_KIND { Oa2dpChannelSignaling, Oa2dpChannelMedia, Oa2dpChannelCount };
struct OA2DP_OPEN_CHANNEL_IN {
    ULONG Kind;
    USHORT InMtu, Reserved;
    ULONG ConnectionGeneration;
};
struct OA2DP_CHANNEL_INFO {
    ULONG Kind;
    USHORT OutMtu, InMtu;
    ULONG Generation, ConnectionGeneration;
};
struct FakeEvent {
    bool Signaled = true;
};
struct FakeLock {
    bool Held = false;
};
struct OA2DP_CHANNEL {
    OA2DP_CHANNEL_LIFECYCLE Life{};
    OA2DP_CHANNEL_KIND Kind{};
    void *Handle = nullptr;
    USHORT OutMtu = 0, InMtu = 0;
    FakeEvent OpenFinished;
    void *CloseWork = nullptr;
};
struct _BRB {};
using PBRB = _BRB *;
struct _BRB_L2CA_OPEN_CHANNEL {
    uint64_t BtAddress;
    USHORT Psm;
    void *ChannelHandle;
    struct {
        struct {
            USHORT Mtu;
        } Params;
    } OutResults, InResults;
    void (*Callback)();
};
struct OA2DP_REQUEST_CONTEXT {
    _BRB *Brb = nullptr;
    OA2DP_CHANNEL *Channel = nullptr;
    ULONG Generation = 0;
    OA2DP_CHANNEL_INFO *OpenOutput = nullptr;
};
struct FakeRequest {
    OA2DP_REQUEST_CONTEXT Context;
    alignas(OA2DP_CHANNEL_INFO) uint8_t Buffer[sizeof(OA2DP_CHANNEL_INFO)]{};
    bool Canceled = false, Completed = false, Owned = true, Formatted = false;
    NTSTATUS OutputStatus = STATUS_SUCCESS, Result = STATUS_PENDING;
    ULONG_PTR Information = 0;
    unsigned Completions = 0;
};
using WDFREQUEST = FakeRequest *;
struct WDF_REQUEST_COMPLETION_PARAMS {
    struct {
        NTSTATUS Status;
    } IoStatus;
};
using PWDF_REQUEST_COMPLETION_PARAMS = WDF_REQUEST_COMPLETION_PARAMS *;
using EVT_WDF_REQUEST_COMPLETION_ROUTINE = void(WDFREQUEST, WDFIOTARGET,
                                                PWDF_REQUEST_COMPLETION_PARAMS, WDFCONTEXT);
constexpr USHORT OA2DP_AVDTP_PSM = 25;
static FakeLock spin;
static bool allocation_fails, complete_inline, cancel_at_submit;
static NTSTATUS submit_status;
static unsigned allocated, freed, submitted, publications, closes, failed_connects;
static _BRB *live_brb;
static WDFREQUEST submitted_request;
static EVT_WDF_REQUEST_COMPLETION_ROUTINE *completion;
static void *completion_context;
static void *Allocate(int, int) {
    if (allocation_fails)
        return nullptr;
    REQUIRE(!live_brb);
    ++allocated;
    live_brb = reinterpret_cast<_BRB *>(std::calloc(1, sizeof(_BRB_L2CA_OPEN_CHANNEL)));
    REQUIRE(live_brb);
    return live_brb;
}
static void Free(PBRB brb) {
    REQUIRE(brb == live_brb);
    std::free(brb);
    live_brb = nullptr;
    ++freed;
}
struct OA2DP_DEVICE_CONTEXT {
    OA2DP_CHANNEL Channels[Oa2dpChannelCount];
    FakeLock *Lock = &spin;
    bool PolicyConnected = true, TransportStopping = false;
    uint64_t RemoteAddress = 0;
    FakeEvent DisconnectedEvent;
    OA2DP_MPM_REQUEST MpmRequest{};
    void *PublicationWork = nullptr;
    struct {
        void *(*BthAllocateBrb)(int, int) = Allocate;
        void (*BthFreeBrb)(PBRB) = Free;
    } Profile;
};
static OA2DP_REQUEST_CONTEXT *Oa2dpGetRequest(WDFREQUEST r) {
    REQUIRE(r && r->Owned && !r->Completed);
    return &r->Context;
}
static NTSTATUS WdfRequestRetrieveOutputBuffer(WDFREQUEST r, size_t size, PVOID *out, void *) {
    REQUIRE(r->Owned && !r->Completed && !r->Formatted && size == sizeof(OA2DP_CHANNEL_INFO));
    if (NT_SUCCESS(r->OutputStatus))
        *out = r->Buffer;
    return r->OutputStatus;
}
static BOOLEAN WdfRequestIsCanceled(WDFREQUEST r) {
    REQUIRE(r->Owned && !r->Completed);
    return r->Canceled;
}
static void WdfRequestCompleteWithInformation(WDFREQUEST r, NTSTATUS status, ULONG_PTR info) {
    REQUIRE(r->Owned && !r->Completed && !spin.Held);
    REQUIRE(!r->Context.Brb && !r->Context.OpenOutput && !live_brb);
    r->Result = status;
    r->Information = info;
    ++r->Completions;
    r->Completed = true;
    r->Owned = false;
}
static void WdfRequestComplete(WDFREQUEST r, NTSTATUS s) {
    WdfRequestCompleteWithInformation(r, s, 0);
}
static void WdfSpinLockAcquire(FakeLock *l) {
    REQUIRE(!l->Held);
    l->Held = true;
}
static void WdfSpinLockRelease(FakeLock *l) {
    REQUIRE(l->Held);
    l->Held = false;
}
static void KeClearEvent(FakeEvent *e) {
    REQUIRE(spin.Held);
    e->Signaled = false;
}
static void KeSetEvent(FakeEvent *e, int, bool) {
    REQUIRE(spin.Held);
    e->Signaled = true;
}
static void WdfWorkItemEnqueue(void *work) {
    REQUIRE(!spin.Held);
    if (work)
        ++closes;
    else
        ++publications;
}
static void Oa2dpMpmConnectFailed(OA2DP_DEVICE_CONTEXT *d, ULONG generation) {
    REQUIRE(!spin.Held);
    ++failed_connects;
    (void)Oa2dpMpmFailConnect(&d->MpmRequest, generation);
}
static void IndicationCallback() {}
static void FillChannelConfig(_BRB_L2CA_OPEN_CHANNEL *brb, OA2DP_CHANNEL *, USHORT mtu) {
    brb->InResults.Params.Mtu = mtu;
    brb->OutResults.Params.Mtu = 679;
    brb->ChannelHandle = &spin;
}
static void finish_open(NTSTATUS status) {
    REQUIRE(submitted_request && !submitted_request->Owned && !submitted_request->Completed);
    submitted_request->Owned = true;
    WDF_REQUEST_COMPLETION_PARAMS params{{status}};
    completion(submitted_request, nullptr, &params, completion_context);
}
static NTSTATUS SubmitBrbAsync(OA2DP_DEVICE_CONTEXT *d, WDFREQUEST r, _BRB *brb, ULONG size,
                               EVT_WDF_REQUEST_COMPLETION_ROUTINE *done, void *ctx, ULONG timeout) {
    REQUIRE(!spin.Held && r->Owned && !r->Completed && !r->Formatted);
    REQUIRE(brb == live_brb && size == sizeof(_BRB_L2CA_OPEN_CHANNEL) && timeout == 8000);
    REQUIRE(r->Context.Brb == brb &&
            r->Context.OpenOutput == reinterpret_cast<OA2DP_CHANNEL_INFO *>(r->Buffer));
    REQUIRE(d == ctx);
    ++submitted;
    r->Formatted = true;
    submitted_request = r;
    completion = done;
    completion_context = ctx;
    if (cancel_at_submit)
        r->Canceled = true;
    if (submit_status != STATUS_PENDING)
        return submit_status;
    r->Owned = false;
    if (complete_inline)
        finish_open(STATUS_SUCCESS);
    return STATUS_PENDING;
}

#include "channel-open-under-test.inc"

static OA2DP_DEVICE_CONTEXT device() {
    REQUIRE(!live_brb);
    spin = {};
    allocation_fails = complete_inline = cancel_at_submit = false;
    submit_status = STATUS_PENDING;
    allocated = freed = submitted = publications = closes = failed_connects = 0;
    submitted_request = nullptr;
    completion = nullptr;
    completion_context = nullptr;
    OA2DP_DEVICE_CONTEXT d;
    for (unsigned i = 0; i < Oa2dpChannelCount; ++i) {
        d.Channels[i].Kind = static_cast<OA2DP_CHANNEL_KIND>(i);
        d.Channels[i].CloseWork = &spin;
    }
    Oa2dpMpmBeginRequest(&d.MpmRequest, 1);
    return d;
}
static void start(OA2DP_DEVICE_CONTEXT &d, FakeRequest &r, ULONG kind = Oa2dpChannelSignaling,
                  ULONG connection = 0, USHORT mtu = 0) {
    const OA2DP_OPEN_CHANNEL_IN in{kind, mtu, 0, connection};
    std::memcpy(r.Buffer, &in, sizeof(in)); // exercise aliased METHOD_BUFFERED buffers
    Oa2dpOpenChannel(&d, &r, reinterpret_cast<OA2DP_OPEN_CHANNEL_IN *>(r.Buffer));
    if (submitted)
        REQUIRE(submitted_request == &r); // original caller, no lower request
}
static void result(const FakeRequest &r, NTSTATUS status, ULONG_PTR info = 0) {
    REQUIRE(r.Completed && r.Completions == 1 && r.Result == status && r.Information == info);
    REQUIRE(!live_brb && allocated == freed);
}
int main() {
    auto d = device();
    FakeRequest r;
    r.OutputStatus = STATUS_BUFFER_TOO_SMALL;
    start(d, r);
    result(r, STATUS_BUFFER_TOO_SMALL);
    REQUIRE(!submitted && !allocated && !d.Channels[0].Life.Generation && !publications);

    d = device();
    r = {};
    r.Canceled = true;
    start(d, r);
    result(r, STATUS_CANCELLED);
    REQUIRE(!allocated && !submitted && !d.Channels[0].Life.Generation);

    d = device();
    r = {};
    start(d, r);
    REQUIRE(!r.Completed && !r.Owned && live_brb && d.Channels[0].Life.State == ChannelOpening);
    REQUIRE(!d.Channels[0].OpenFinished.Signaled && d.MpmRequest.AttemptGeneration == 1);
    finish_open(STATUS_SUCCESS);
    result(r, STATUS_SUCCESS, sizeof(OA2DP_CHANNEL_INFO));
    auto out = reinterpret_cast<OA2DP_CHANNEL_INFO *>(r.Buffer);
    REQUIRE(out->Generation == 1 && out->ConnectionGeneration == 1 &&
            out->InMtu == OA2DP_DEFAULT_SIGNALING_MTU);
    REQUIRE(d.Channels[0].Life.State == ChannelOpen && d.Channels[0].Handle == &spin && !closes);

    d = device();
    r = {};
    complete_inline = true;
    start(d, r);
    result(r, STATUS_SUCCESS, sizeof(OA2DP_CHANNEL_INFO)); // no access after synchronous completion

    d = device();
    r = {};
    complete_inline = cancel_at_submit = true;
    start(d, r);
    result(r, STATUS_CANCELLED);
    REQUIRE(closes == 1 && d.Channels[0].Handle == &spin);
    REQUIRE(d.Channels[0].Life.State == ChannelClosing && d.Channels[0].Life.CloseRequested);
    REQUIRE(!Oa2dpChannelBeginOpen(&d.Channels[0].Life, 1, 0));

    d = device();
    r = {};
    start(d, r);
    r.Canceled = true;
    finish_open(STATUS_CANCELLED);
    result(r, STATUS_CANCELLED);
    REQUIRE(d.Channels[0].Life.State == ChannelClosed && publications == 1 &&
            failed_connects == 1 && !closes);
    REQUIRE(d.MpmRequest.FailedConnect == d.MpmRequest.Serial);

    d = device();
    r = {};
    allocation_fails = true;
    start(d, r);
    result(r, STATUS_INSUFFICIENT_RESOURCES);
    REQUIRE(!submitted && publications == 1 && failed_connects == 1 &&
            d.Channels[0].OpenFinished.Signaled);

    d = device();
    r = {};
    submit_status = STATUS_IO_TIMEOUT;
    start(d, r);
    result(r, STATUS_IO_TIMEOUT);
    REQUIRE(freed == 1 && publications == 1 && failed_connects == 1);

    d = device();
    r = {};
    start(d, r);
    finish_open(STATUS_IO_TIMEOUT);
    result(r, STATUS_IO_TIMEOUT);
    REQUIRE(publications == 1 && failed_connects == 1);

    for (unsigned condition = 0; condition < 3; ++condition) {
        d = device();
        r = {};
        start(d, r);
        if (condition == 0)
            Oa2dpChannelRequestClose(&d.Channels[0].Life);
        if (condition == 1)
            d.PolicyConnected = false;
        if (condition == 2)
            d.TransportStopping = true;
        finish_open(STATUS_SUCCESS);
        result(r, STATUS_CANCELLED);
        REQUIRE(closes == 1 && d.Channels[0].Life.State == ChannelClosing && d.Channels[0].Handle);
    }

    d = device();
    r = {};
    start(d, r);
    ++d.Channels[0].Life.Generation;
    d.Channels[0].Life.State = ChannelOpen;
    d.Channels[0].Handle = &d; // obsolete failed completion cannot fail/clear the replacement
    d.MpmRequest.AttemptGeneration = d.Channels[0].Life.Generation;
    finish_open(STATUS_IO_TIMEOUT);
    result(r, STATUS_CANCELLED);
    REQUIRE(d.Channels[0].Handle == &d && d.Channels[0].Life.State == ChannelOpen);
    REQUIRE(!publications && !closes && !failed_connects && !d.MpmRequest.FailedConnect);

    d = device();
    r = {};
    start(d, r, Oa2dpChannelMedia, 1);
    result(r, STATUS_CONNECTION_DISCONNECTED);
    REQUIRE(!submitted && !allocated);

    d = device();
    r = {};
    d.Channels[0].Life.State = ChannelOpen;
    d.Channels[0].Life.Generation = d.Channels[0].Life.ConnectionGeneration = 5;
    start(d, r, Oa2dpChannelMedia, 5, 1234);
    finish_open(STATUS_SUCCESS);
    result(r, STATUS_SUCCESS, sizeof(OA2DP_CHANNEL_INFO));
    out = reinterpret_cast<OA2DP_CHANNEL_INFO *>(r.Buffer);
    REQUIRE(out->Kind == Oa2dpChannelMedia && out->Generation == 1 &&
            out->ConnectionGeneration == 5 && out->InMtu == 1234);
    std::puts("production outgoing OPEN ownership checks passed (all OS calls fake)");
}
