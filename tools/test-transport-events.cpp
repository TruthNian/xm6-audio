/* SPDX-License-Identifier: MIT */
/* Real kernel event queue procedures with controlled WDF request boundaries. */
#include "../driver/common/channel_lifecycle.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
#ifndef _In_
#define _In_
#endif
#ifndef _In_opt_
#define _In_opt_
#endif
#define NT_SUCCESS(x) ((x) >= 0)
#define RtlZeroMemory(p,n) std::memset(p,0,n)
using NTSTATUS = int32_t;
using ULONG = uint32_t;
using VOID = void;
using PVOID = void *;
constexpr bool TRUE = true, FALSE = false;
constexpr NTSTATUS STATUS_SUCCESS = 0, STATUS_BUFFER_TOO_SMALL = -1, STATUS_NO_MORE_ENTRIES = -2;
constexpr ULONG OA2DP_EVENT_CAPACITY = 32;
enum { Oa2dpEventChannelOpened = 1, Oa2dpEventChannelClosed, Oa2dpEventDeviceGone,
       Oa2dpEventStateLost, Oa2dpEventPowerChanged, Oa2dpEventLinkAvailable };
struct OA2DP_CHANNEL_INFO { ULONG Kind, OutMtu, InMtu, Generation, ConnectionGeneration; };
struct OA2DP_EVENT { ULONG Type, Kind; OA2DP_CHANNEL_INFO Channel; };
struct OA2DP_CHANNEL { OA2DP_CHANNEL_LIFECYCLE Life{}; ULONG Kind = 0, OutMtu = 1000, InMtu = 672; };
struct Request {
    OA2DP_EVENT output{};
    bool valid = true, completed = false;
    NTSTATUS status = 99;
    size_t bytes = 0;
};
using WDFREQUEST = Request *;
struct FakeLock { bool held = false; };
struct Queue { std::deque<WDFREQUEST> waits; FakeLock *lock = nullptr; };
struct OA2DP_DEVICE_CONTEXT {
    FakeLock storage;
    FakeLock *Lock = &storage;
    Queue queue{{}, Lock};
    Queue *EventQueue = &queue;
    OA2DP_EVENT Events[OA2DP_EVENT_CAPACITY]{};
    ULONG EventHead = 0, EventCount = 0;
    bool EventOverflow = false;
};
void WdfSpinLockAcquire(FakeLock *l) { REQUIRE(!l->held); l->held = true; }
void WdfSpinLockRelease(FakeLock *l) { REQUIRE(l->held); l->held = false; }
NTSTATUS WdfRequestRetrieveOutputBuffer(WDFREQUEST r, size_t, PVOID *p, void *)
{ if (!r->valid) return STATUS_BUFFER_TOO_SMALL; *p = &r->output; return STATUS_SUCCESS; }
NTSTATUS WdfIoQueueRetrieveNextRequest(Queue *q, WDFREQUEST *r)
{
    REQUIRE(q->lock->held);
    if (q->waits.empty()) return STATUS_NO_MORE_ENTRIES;
    *r = q->waits.front(); q->waits.pop_front(); return STATUS_SUCCESS;
}
void WdfRequestComplete(WDFREQUEST r, NTSTATUS s)
{ REQUIRE(!r->completed); r->completed = true; r->status = s; }
void WdfRequestCompleteWithInformation(WDFREQUEST r, NTSTATUS s, size_t n)
{ WdfRequestComplete(r,s); r->bytes = n; }
static std::function<void()> before_insert;
NTSTATUS WdfRequestForwardToIoQueue(WDFREQUEST r, Queue *q)
{
    REQUIRE(!q->lock->held);
    if (before_insert) { auto callback = std::move(before_insert); before_insert = {}; callback(); }
    q->waits.push_back(r); return STATUS_SUCCESS;
}
#include "transport-events-under-test.inc"
static OA2DP_EVENT Take(OA2DP_DEVICE_CONTEXT &d)
{
    Request r;
    WaitEvent(&d, &r);
    REQUIRE(r.completed && r.status == STATUS_SUCCESS && r.bytes == sizeof(OA2DP_EVENT));
    return r.output;
}
int main()
{
    OA2DP_DEVICE_CONTEXT d;
    Request waiter;
    WaitEvent(&d, &waiter);
    REQUIRE(!waiter.completed);
    Oa2dpPostEvent(&d, Oa2dpEventPowerChanged, nullptr, 0);
    REQUIRE(waiter.completed && waiter.output.Type == Oa2dpEventPowerChanged);

    // Producer can run after validation but before queue insertion.
    Request racing;
    before_insert = [&] { Oa2dpPostEvent(&d, Oa2dpEventPowerChanged, nullptr, 0); };
    WaitEvent(&d, &racing);
    REQUIRE(racing.completed && racing.output.Type == Oa2dpEventPowerChanged);

    OA2DP_CHANNEL ch;
    REQUIRE(Oa2dpChannelBeginOpen(&ch.Life, true, false));
    const auto generation = ch.Life.Generation;
    REQUIRE(Oa2dpChannelFinishOpen(&ch.Life, generation, true));
    Oa2dpPostEvent(&d, Oa2dpEventChannelOpened, &ch, generation);
    auto e = Take(d);
    REQUIRE(e.Type == Oa2dpEventChannelOpened && e.Channel.Generation == generation);
    REQUIRE(e.Channel.OutMtu == ch.OutMtu && e.Channel.InMtu == ch.InMtu);
    Oa2dpChannelRequestClose(&ch.Life);
    REQUIRE(Oa2dpChannelFinishClose(&ch.Life, generation, true));
    Oa2dpPostEvent(&d, Oa2dpEventChannelClosed, &ch, generation);
    REQUIRE(Take(d).Type == Oa2dpEventChannelClosed);
    REQUIRE(Oa2dpChannelBeginOpen(&ch.Life, true, false));
    // Delayed notifications from the retired channel cannot enter the queue.
    Oa2dpPostEvent(&d, Oa2dpEventChannelClosed, &ch, generation);
    Oa2dpPostEvent(&d, Oa2dpEventChannelOpened, &ch, generation);
    REQUIRE(d.EventCount == 0);

    for (unsigned i = 0; i < 100; ++i) Oa2dpPostEvent(&d, Oa2dpEventLinkAvailable, nullptr, 0);
    REQUIRE(d.EventCount == 1 && !d.EventOverflow);
    REQUIRE(Take(d).Type == Oa2dpEventLinkAvailable);
    // Exercise actual wraparound, then overflow. Hints cannot evict real state.
    for (unsigned i = 0; i < OA2DP_EVENT_CAPACITY * 3; ++i) {
        Oa2dpPostEvent(&d, Oa2dpEventPowerChanged, nullptr, 0);
        REQUIRE(Take(d).Type == Oa2dpEventPowerChanged);
    }
    for (unsigned i = 0; i < OA2DP_EVENT_CAPACITY; ++i)
        Oa2dpPostEvent(&d, Oa2dpEventPowerChanged, nullptr, 0);
    Oa2dpPostEvent(&d, Oa2dpEventLinkAvailable, nullptr, 0);
    REQUIRE(!d.EventOverflow && d.EventCount == OA2DP_EVENT_CAPACITY);
    Oa2dpPostEvent(&d, Oa2dpEventPowerChanged, nullptr, 0);
    REQUIRE(d.EventOverflow && d.EventCount == 0);
    Oa2dpPostEvent(&d, Oa2dpEventLinkAvailable, nullptr, 0);
    auto lost = Take(d);
    REQUIRE(lost.Type == Oa2dpEventStateLost && lost.Channel.Generation == 0 && !d.EventOverflow);
    Oa2dpPostEvent(&d, Oa2dpEventPowerChanged, nullptr, 0);
    REQUIRE(Take(d).Type == Oa2dpEventPowerChanged);

    for (unsigned i = 0; i <= OA2DP_EVENT_CAPACITY; ++i)
        Oa2dpPostEvent(&d, Oa2dpEventPowerChanged, nullptr, 0);
    Oa2dpPostEvent(&d, Oa2dpEventDeviceGone, nullptr, 0);
    REQUIRE(!d.EventOverflow && d.EventCount == 1);
    REQUIRE(Take(d).Type == Oa2dpEventDeviceGone);

    // Cancellation removes the waiter, not the following state event.
    Request canceled;
    WaitEvent(&d, &canceled);
    REQUIRE(d.queue.waits.front() == &canceled);
    d.queue.waits.pop_front(); // WDF cancellation boundary
    Oa2dpPostEvent(&d, Oa2dpEventPowerChanged, nullptr, 0);
    Request invalid;
    invalid.valid = false;
    WaitEvent(&d, &invalid);
    REQUIRE(invalid.completed && invalid.status == STATUS_BUFFER_TOO_SMALL && d.EventCount == 1);
    REQUIRE(Take(d).Type == Oa2dpEventPowerChanged && !canceled.completed);
    std::puts("production transport event backlog, generation, cancellation and insertion-race checks passed");
}
