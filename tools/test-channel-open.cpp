/* SPDX-License-Identifier: MIT */
/* Actual incoming/outgoing OPEN/completion bodies; OS APIs are fake. This checks request
 * ownership/order, not KMDF's implementation of IRP cancellation or Bluetooth. */
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
#ifndef _In_
#define _In_
#endif
#define NT_ASSERT(x) REQUIRE(x)
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
constexpr int BRB_L2CA_OPEN_CHANNEL_RESPONSE = 1;
constexpr ULONG CONNECT_RSP_RESULT_SUCCESS = 0, CONNECT_RSP_RESULT_NO_RESOURCES = 4;
constexpr ULONG Oa2dpEventChannelOpened = 1, Oa2dpEventChannelClosed = 2;
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
    ULONG Response;
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
    bool Internal = false, Deleted = false;
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
static bool allocation_fails, complete_inline, cancel_at_submit, request_creation_fails;
static NTSTATUS submit_status;
static unsigned allocated, freed, submitted, publications, closes, failed_connects;
static _BRB *live_brb;
static WDFREQUEST submitted_request;
static EVT_WDF_REQUEST_COMPLETION_ROUTINE *completion;
static void *completion_context;
static unsigned created_requests, deleted_requests, delivered_events;
struct QueuedEvent { ULONG type, kind, generation, connection; };
static std::vector<QueuedEvent> channel_events;
static FakeRequest internal_request;
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
    bool PowerQuiesced = false, SurpriseRemoved = false;
    uint64_t RemoteAddress = 0;
    FakeEvent DisconnectedEvent;
    FakeEvent ResponsesIdle;
    ULONG ResponseSubmissions = 0;
    void *Device = nullptr, *IoTarget = nullptr;
    OA2DP_MPM_REQUEST MpmRequest{};
    void *PublicationWork = nullptr;
    struct {
        void *(*BthAllocateBrb)(int, int) = Allocate;
        void (*BthFreeBrb)(PBRB) = Free;
    } Profile;
};
static OA2DP_REQUEST_CONTEXT *Oa2dpGetRequest(WDFREQUEST r) {
    REQUIRE(r && r->Owned && !r->Completed && !r->Deleted);
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
struct INDICATION_PARAMETERS {
    uint64_t BtAddress = 0;
    void *ConnectionHandle = &spin;
    struct { struct { struct { USHORT PSM = OA2DP_AVDTP_PSM; } Request; } Connect; } Parameters;
};
using PINDICATION_PARAMETERS = INDICATION_PARAMETERS *;
struct WDF_OBJECT_ATTRIBUTES { void *ParentObject = nullptr; };
#define WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(p, t) (*(p) = WDF_OBJECT_ATTRIBUTES{})
static NTSTATUS WdfRequestCreate(WDF_OBJECT_ATTRIBUTES *, void *, WDFREQUEST *out) {
    REQUIRE(!spin.Held);
    if (request_creation_fails) return STATUS_INSUFFICIENT_RESOURCES;
    internal_request = {};
    internal_request.Internal = true;
    ++created_requests;
    *out = &internal_request;
    return STATUS_SUCCESS;
}
static void WdfObjectDelete(WDFREQUEST r) {
    REQUIRE(r->Internal && r->Owned && !r->Deleted && !r->Completed && !spin.Held);
    r->Deleted = true;
    r->Owned = false;
    ++deleted_requests;
}
static void Oa2dpQueueEventLocked(OA2DP_DEVICE_CONTEXT *, ULONG type, OA2DP_CHANNEL *ch,
                                 ULONG generation) {
    REQUIRE(spin.Held && (type == Oa2dpEventChannelOpened || type == Oa2dpEventChannelClosed));
    REQUIRE(Oa2dpChannelEventReady(&ch->Life, generation, type == Oa2dpEventChannelOpened));
    channel_events.push_back({type, static_cast<ULONG>(ch->Kind), generation, ch->Life.ConnectionGeneration});
}
static void Oa2dpDeliverEvents(OA2DP_DEVICE_CONTEXT *) {
    REQUIRE(!spin.Held);
    ++delivered_events;
}
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
    REQUIRE(r->Context.Brb == brb);
    REQUIRE(r->Internal ? r->Context.OpenOutput == nullptr :
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
static EVT_WDF_REQUEST_COMPLETION_ROUTINE OpenResponseDone;
#include "channel-incoming-under-test.inc"

static OA2DP_DEVICE_CONTEXT device() {
    REQUIRE(!live_brb);
    spin = {};
    allocation_fails = complete_inline = cancel_at_submit = request_creation_fails = false;
    submit_status = STATUS_PENDING;
    allocated = freed = submitted = publications = closes = failed_connects = 0;
    submitted_request = nullptr;
    completion = nullptr;
    completion_context = nullptr;
    created_requests = deleted_requests = delivered_events = 0;
    channel_events.clear();
    OA2DP_DEVICE_CONTEXT d;
    for (unsigned i = 0; i < Oa2dpChannelCount; ++i) {
        d.Channels[i].Kind = static_cast<OA2DP_CHANNEL_KIND>(i);
        d.Channels[i].CloseWork = &spin;
    }
    Oa2dpMpmBeginRequest(&d.MpmRequest, 1);
    return d;
}
static void incoming_failure_checks() {
    INDICATION_PARAMETERS indication;
    // An incoming attempt must own the pending Windows connect request, too.
    auto d = device();
    AcceptIncoming(&d, &indication);
    REQUIRE(d.MpmRequest.AttemptGeneration == d.Channels[0].Life.Generation);
    finish_open(STATUS_IO_TIMEOUT);
    REQUIRE(d.Channels[0].Life.State == ChannelClosed && d.Channels[0].OpenFinished.Signaled);
    REQUIRE(d.MpmRequest.FailedConnect == d.MpmRequest.Serial && failed_connects == 1);
    REQUIRE(publications == 1 && !closes && channel_events.size() == 1);
    REQUIRE(channel_events[0].type == Oa2dpEventChannelClosed && delivered_events == 1);
    REQUIRE(!live_brb && created_requests == deleted_requests && d.ResponsesIdle.Signaled);

    // Allocation/request/submission failures follow that same terminal path.
    for (unsigned failure = 0; failure < 3; ++failure) {
        d = device();
        allocation_fails = failure == 0;
        request_creation_fails = failure == 1;
        if (failure == 2) submit_status = STATUS_IO_TIMEOUT;
        AcceptIncoming(&d, &indication);
        REQUIRE(d.Channels[0].Life.State == ChannelClosed && d.Channels[0].OpenFinished.Signaled);
        REQUIRE(d.MpmRequest.FailedConnect == d.MpmRequest.Serial && failed_connects == 1);
        REQUIRE(publications == 1 && !closes && !live_brb && allocated == freed);
        REQUIRE(channel_events.size() == 1 && channel_events[0].type == Oa2dpEventChannelClosed);
        REQUIRE(channel_events[0].generation == d.Channels[0].Life.Generation && delivered_events == 1);
        REQUIRE(created_requests == deleted_requests && d.ResponsesIdle.Signaled);
    }

}
static void incoming_media_failure_checks() {
    INDICATION_PARAMETERS indication;
    auto d = device();
    // A failed incoming media OPEN cannot leave a configured signal link
    // stranded indefinitely. The service must learn that this attempt closed,
    // even though no OPEN event preceded it, and use its retrying teardown.
    for (unsigned failure = 0; failure < 4; ++failure) {
        d = device();
        auto &signal = d.Channels[0];
        signal.Life.State = ChannelOpen;
        signal.Life.Generation = signal.Life.ConnectionGeneration = 7;
        signal.Handle = &spin;
        Oa2dpMpmBindAttempt(&d.MpmRequest, 7);
        allocation_fails = failure == 0;
        request_creation_fails = failure == 1;
        if (failure == 2) submit_status = STATUS_IO_TIMEOUT;
        AcceptIncoming(&d, &indication);
        if (failure == 3) finish_open(STATUS_IO_TIMEOUT);
        REQUIRE(d.Channels[1].Life.State == ChannelClosed && d.Channels[1].OpenFinished.Signaled);
        REQUIRE(signal.Life.State == ChannelOpen && !signal.Life.CloseRequested && !closes);
        REQUIRE(publications == 1 && !failed_connects && channel_events.size() == 1);
        REQUIRE(channel_events[0].type == Oa2dpEventChannelClosed && channel_events[0].kind == 1);
        REQUIRE(channel_events[0].connection == 7 && channel_events[0].generation == 1 && delivered_events == 1);
        REQUIRE(!live_brb && allocated == freed && created_requests == deleted_requests);
        REQUIRE(d.ResponsesIdle.Signaled);
    }
}
static void incoming_success_and_race_checks() {
    INDICATION_PARAMETERS indication;
    for (unsigned inline_completion = 0; inline_completion < 2; ++inline_completion) {
        auto d = device();
        complete_inline = inline_completion != 0;
        AcceptIncoming(&d, &indication);
        if (!complete_inline) finish_open(STATUS_SUCCESS);
        REQUIRE(d.Channels[0].Life.State == ChannelOpen && d.Channels[0].Handle == &spin);
        REQUIRE(d.MpmRequest.AttemptGeneration == 1 && !d.MpmRequest.FailedConnect);
        REQUIRE(channel_events.size() == 1 && channel_events[0].type == Oa2dpEventChannelOpened);
        REQUIRE(channel_events[0].connection == 1 && channel_events[0].generation == 1);
        REQUIRE(!publications && !closes && !failed_connects && !live_brb);
        REQUIRE(created_requests == deleted_requests && d.ResponsesIdle.Signaled);
    }
    auto d = device();
    d.Channels[0].Life = {ChannelOpen, 7, 7, 0, 0};
    Oa2dpMpmBindAttempt(&d.MpmRequest, 7);
    AcceptIncoming(&d, &indication);
    finish_open(STATUS_SUCCESS);
    REQUIRE(d.MpmRequest.AttemptGeneration == 7 && d.Channels[1].Life.State == ChannelOpen);
    REQUIRE(channel_events.size() == 1 && channel_events[0].kind == 1 && channel_events[0].connection == 7);

    for (unsigned condition = 0; condition < 3; ++condition) {
        d = device();
        AcceptIncoming(&d, &indication);
        if (condition == 0) Oa2dpChannelRequestClose(&d.Channels[0].Life);
        if (condition == 1) d.PolicyConnected = false;
        if (condition == 2) d.TransportStopping = true;
        finish_open(STATUS_SUCCESS);
        REQUIRE(d.Channels[0].Life.State == ChannelClosing && d.Channels[0].Handle == &spin);
        REQUIRE(closes == 1 && channel_events.empty() && !failed_connects && !live_brb);
    }
    // A delayed failed completion must not wake or fail a replacement OPEN.
    for (unsigned kind = 0; kind < 2; ++kind) {
        d = device();
        if (kind) d.Channels[0].Life = {ChannelOpen, 7, 7, 0, 0};
        AcceptIncoming(&d, &indication);
        auto &channel = d.Channels[kind];
        ++channel.Life.Generation;
        channel.Life.State = ChannelOpening;
        channel.OpenFinished.Signaled = false;
        Oa2dpMpmBindAttempt(&d.MpmRequest, channel.Life.Generation);
        finish_open(STATUS_IO_TIMEOUT);
        REQUIRE(channel.Life.State == ChannelOpening && !channel.OpenFinished.Signaled);
        REQUIRE(!failed_connects && !d.MpmRequest.FailedConnect && !publications && !closes);
        REQUIRE(channel_events.empty() && !live_brb && created_requests == deleted_requests);
    }
    // A media failure belongs to its original signal connection, not a new
    // connection that appeared before the lower completion was dispatched.
    d = device();
    d.Channels[0].Life = {ChannelOpen, 7, 7, 0, 0};
    AcceptIncoming(&d, &indication);
    d.Channels[0].Life = {ChannelOpen, 8, 8, 0, 0};
    Oa2dpMpmBindAttempt(&d.MpmRequest, 8);
    finish_open(STATUS_IO_TIMEOUT);
    REQUIRE(d.Channels[0].Life.State == ChannelOpen && !d.Channels[0].Life.CloseRequested);
    REQUIRE(channel_events.size() == 1 && channel_events[0].connection == 7);
    REQUIRE(!failed_connects && !closes && d.MpmRequest.AttemptGeneration == 8);

    for (unsigned condition = 0; condition < 3; ++condition) {
        d = device();
        if (condition == 0) d.TransportStopping = true;
        if (condition == 1) d.PowerQuiesced = true;
        if (condition == 2) d.SurpriseRemoved = true;
        AcceptIncoming(&d, &indication);
        REQUIRE(!allocated && !submitted && !d.ResponseSubmissions && channel_events.empty());
    }
    // A policy rejection answers the peer without owning any new channel.
    d = device();
    d.PolicyConnected = false;
    AcceptIncoming(&d, &indication);
    REQUIRE(reinterpret_cast<_BRB_L2CA_OPEN_CHANNEL *>(live_brb)->Response == CONNECT_RSP_RESULT_NO_RESOURCES);
    REQUIRE(!submitted_request->Context.Channel);
    finish_open(STATUS_SUCCESS);
    REQUIRE(!failed_connects && !publications && !closes && channel_events.empty());
    REQUIRE(!live_brb && created_requests == deleted_requests && d.ResponsesIdle.Signaled);
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
int main(int argc, char **argv) {
    if (argc == 2 && std::strcmp(argv[1], "--incoming-media") == 0) {
        incoming_media_failure_checks();
        return 0;
    }
    incoming_failure_checks();
    incoming_media_failure_checks();
    incoming_success_and_race_checks();
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
    std::puts("production incoming/outgoing OPEN, failure notification and ownership checks passed (OS APIs fake)");
}
