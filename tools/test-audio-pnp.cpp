/* SPDX-License-Identifier: MIT */
/* Execute production PnP dispatch and connection publication. OS calls are
 * controlled boundaries, including a synchronous query during invalidation.
 * Windows container aggregation itself requires the live ETW comparison. */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
#ifndef _In_
#define _In_
#endif
#ifndef _Inout_
#define _Inout_
#endif
#define PAGED_CODE() ((void)0)
#define ASSERT(x) REQUIRE(x)
#define NT_SUCCESS(x) ((x) >= 0)
using NTSTATUS = int32_t;
using LONG = int32_t;
using ULONG = uint32_t;
using BOOLEAN = bool;
using KIRQL = unsigned;
using PVOID = void *;
using GUID = unsigned;
constexpr bool TRUE = true, FALSE = false;
constexpr NTSTATUS STATUS_SUCCESS = 0, STATUS_CONNECTION_DISCONNECTED = -1;
constexpr NTSTATUS STATUS_DELETE_PENDING = -2, STATUS_DEVICE_NOT_READY = -3;
constexpr ULONG PNP_DEVICE_DISCONNECTED = 0x40;
constexpr unsigned IRP_MN_QUERY_PNP_DEVICE_STATE = 0x14, IRP_MN_STOP_DEVICE = 4;
constexpr unsigned IRP_MN_SURPRISE_REMOVAL = 0x17, IRP_MN_REMOVE_DEVICE = 2;
constexpr GUID KSEVENTSETID_PinCapsChange = 1, IID_IPortEvents = 2;
constexpr unsigned KSEVENT_PINCAPS_JACKINFOCHANGE = 0, OA2DP_TOPO_PIN_OUT = 1;
struct FakeLock { bool held = false; };
void Oa2dpLock(FakeLock *l) { REQUIRE(!l->held); l->held = true; }
void Oa2dpUnlock(FakeLock *l) { REQUIRE(l->held); l->held = false; }
void KeAcquireSpinLock(FakeLock *l, KIRQL *i) { *i = 0; Oa2dpLock(l); }
void KeReleaseSpinLock(FakeLock *l, KIRQL) { Oa2dpUnlock(l); }
LONG InterlockedExchange(volatile LONG *p, LONG v) { LONG old = *p; *p = v; return old; }
LONG InterlockedCompareExchange(volatile LONG *p, LONG v, LONG c) { LONG old = *p; if (old == c) *p = v; return old; }
struct OA2DP_AUDIO_DEVICE;
struct DEVICE_OBJECT { OA2DP_AUDIO_DEVICE *dev; };
struct IO_STACK_LOCATION { unsigned MinorFunction; };
struct IRP {
    struct { NTSTATUS Status = -99; uintptr_t Information = 0; } IoStatus;
    IO_STACK_LOCATION stack{};
};
using PIRP = IRP *;
IO_STACK_LOCATION *IoGetCurrentIrpStackLocation(IRP *irp) { return &irp->stack; }
OA2DP_AUDIO_DEVICE *OA2DP_DEVICE(DEVICE_OBJECT *d) { return d->dev; }
static std::vector<unsigned> calls;
enum { Parent, Invalidate, Jack, Dispatch, Stop, Remove };
struct FakePort {
    void QueryInterface(GUID, PVOID *out) { *out = this; }
    void GenerateEventList(GUID *, unsigned, bool, unsigned, bool, ULONG) { calls.push_back(Jack); }
    void Release() {}
};
using PUNKNOWN = FakePort *;
using PPORTEVENTS = FakePort *;
struct OA2DP_AUDIO_DEVICE {
    bool Initialized = true, Started = true, Removed = false, ConnectionEnabled = true;
    bool SuspendRequested = false, Ring = false;
    volatile LONG Connected = 0;
    FakeLock ConnectionLock, Lock, FilterLock;
    DEVICE_OBJECT pdo{this};
    DEVICE_OBJECT *Pdo = &pdo;
    FakePort port;
    void *TopoPort = &port;
    struct {
        struct { void *Context = nullptr; } Header;
        NTSTATUS (*SetConnected)(void *, bool, ULONG) = nullptr;
    } Bus;
};
static NTSTATUS parent_status = STATUS_SUCCESS, dispatch_status = 17;
static ULONG queried_flags;
NTSTATUS Oa2dpControlStatusLocked(OA2DP_AUDIO_DEVICE *d, bool)
{ REQUIRE(d->Lock.held); return d->Removed ? STATUS_DELETE_PENDING : STATUS_SUCCESS; }
NTSTATUS Publish(void *, bool, ULONG) { calls.push_back(Parent); return parent_status; }
void StopDevice(OA2DP_AUDIO_DEVICE *) { calls.push_back(Stop); }
void RemoveDevice(OA2DP_AUDIO_DEVICE *) { calls.push_back(Remove); }
NTSTATUS PcDispatchIrp(DEVICE_OBJECT *, IRP *) { calls.push_back(Dispatch); return dispatch_status; }
#include "audio-pnp-under-test.inc"
void Oa2dpRingSetConsumerAvailable(bool *ring, bool available) { *ring=available; }
void IoInvalidateDeviceState(DEVICE_OBJECT *pdo)
{
    calls.push_back(Invalidate);
    REQUIRE(!pdo->dev->Lock.held && !pdo->dev->FilterLock.held);
    IRP query;
    query.stack.MinorFunction = IRP_MN_QUERY_PNP_DEVICE_STATE;
    PnpHandler(pdo, &query);
    REQUIRE(query.IoStatus.Status == STATUS_SUCCESS);
    queried_flags = static_cast<ULONG>(query.IoStatus.Information);
}
#include "audio-connection-under-test.inc"

int main()
{
    OA2DP_AUDIO_DEVICE d;
    d.Bus.SetConnected = Publish;
    IRP query;
    query.stack.MinorFunction = IRP_MN_QUERY_PNP_DEVICE_STATE;
    query.IoStatus.Information = uintptr_t(1) << (sizeof(uintptr_t) * 8 - 1);
    const auto other = query.IoStatus.Information;
    REQUIRE(PnpHandler(d.Pdo, &query) == dispatch_status);
    REQUIRE(query.IoStatus.Status == STATUS_SUCCESS);
    REQUIRE(query.IoStatus.Information == (other | PNP_DEVICE_DISCONNECTED));
    d.Connected = 1;
    query.IoStatus.Information = other;
    PnpHandler(d.Pdo, &query);
    REQUIRE(query.IoStatus.Information == other);
    // A connected function must not clear a different driver's disconnected bit.
    query.IoStatus.Information |= PNP_DEVICE_DISCONNECTED;
    PnpHandler(d.Pdo, &query);
    REQUIRE(query.IoStatus.Information == (other | PNP_DEVICE_DISCONNECTED));

    d.Connected = 0;
    calls.clear();
    REQUIRE(Oa2dpSetConnected(&d, true, 9) == STATUS_SUCCESS);
    REQUIRE(d.Connected == 1 && queried_flags == 0 && d.Ring);
    REQUIRE((calls == std::vector<unsigned>{Parent, Invalidate, Dispatch, Jack}));
    calls.clear();
    Oa2dpSetConnected(&d, true, 9);
    REQUIRE((calls == std::vector<unsigned>{Parent}));
    calls.clear();
    REQUIRE(Oa2dpSetConnected(&d, false, 0) == STATUS_SUCCESS);
    REQUIRE(d.Connected == 0 && queried_flags == PNP_DEVICE_DISCONNECTED && !d.Ring);
    REQUIRE((calls == std::vector<unsigned>{Parent, Invalidate, Dispatch, Jack}));
    calls.clear();
    Oa2dpSetConnected(&d, false, 0);
    REQUIRE((calls == std::vector<unsigned>{Parent}) && !d.Ring);
    parent_status = STATUS_DEVICE_NOT_READY;
    calls.clear();
    REQUIRE(Oa2dpSetConnected(&d, true, 10) == parent_status);
    REQUIRE(!d.Connected && (calls == std::vector<unsigned>{Parent}));
    // Local audio may unplug before physical teardown, but must return failure.
    d.Connected = 1;
    d.TopoPort = nullptr;
    calls.clear();
    REQUIRE(Oa2dpSetConnected(&d, false, 0) == parent_status);
    REQUIRE(!d.Connected && queried_flags == PNP_DEVICE_DISCONNECTED);
    REQUIRE((calls == std::vector<unsigned>{Parent, Invalidate, Dispatch}));
    calls.clear();
    d.ConnectionEnabled = false;
    REQUIRE(Oa2dpSetConnected(&d, true, 11) == STATUS_CONNECTION_DISCONNECTED);
    REQUIRE(calls.empty());
    d.Removed = true;
    REQUIRE(Oa2dpSetConnected(&d, true, 11) == STATUS_DELETE_PENDING);
    REQUIRE(calls.empty());
    // Uninitialized devices and unrelated PnP requests still reach PortCls.
    d.Initialized = false;
    query.IoStatus.Status = -99;
    query.IoStatus.Information = other;
    PnpHandler(d.Pdo, &query);
    REQUIRE(query.IoStatus.Status == -99 && query.IoStatus.Information == other);
    d.Initialized = true;
    for (unsigned minor : {IRP_MN_STOP_DEVICE, IRP_MN_SURPRISE_REMOVAL, IRP_MN_REMOVE_DEVICE}) {
        calls.clear();
        query.stack.MinorFunction = minor;
        PnpHandler(d.Pdo, &query);
        REQUIRE(calls.front() == Stop && calls.back() == Dispatch);
        REQUIRE(calls.size() == (minor == IRP_MN_REMOVE_DEVICE ? 3u : 2u));
    }
    std::puts("production audio PnP and connection notification contracts passed");
}
