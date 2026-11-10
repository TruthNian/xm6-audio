/* SPDX-License-Identifier: MIT */
/* Execute the production close routine with fake OS calls. No Bluetooth,
 * driver installation, device access or kernel emulation is involved. */
#include "../driver/common/channel_lifecycle.h"
#include "../driver/common/mpm_lifecycle.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
using NTSTATUS = int32_t;
using ULONG = uint32_t;
using PULONG = ULONG *;
using BOOLEAN = bool;
using L2CAP_CHANNEL_HANDLE = void *;
constexpr BOOLEAN TRUE = true, FALSE = false;
constexpr NTSTATUS STATUS_SUCCESS = 0, STATUS_TIMEOUT = 258;
constexpr NTSTATUS STATUS_IO_TIMEOUT = -1, STATUS_NOT_FOUND = -2;
constexpr NTSTATUS STATUS_DEVICE_NOT_READY = -3, STATUS_INSUFFICIENT_RESOURCES = -4;
constexpr NTSTATUS STATUS_CONNECTION_DISCONNECTED = -5, STATUS_INVALID_HANDLE = -6;
#define NT_SUCCESS(s) ((s) >= 0)
constexpr int Executive = 0, KernelMode = 0, BRB_L2CA_CLOSE_CHANNEL = 0, OA2DP_POOL_TAG = 0;
enum OA2DP_CHANNEL_KIND { Oa2dpChannelSignaling, Oa2dpChannelMedia, Oa2dpChannelCount };
constexpr int Oa2dpEventChannelClosed = 0;
struct LARGE_INTEGER { int64_t QuadPart; };
struct FakeLock { bool Held = false; };
struct FakeEvent {};
struct _BRB {};
using PBRB = _BRB *;
struct _BRB_L2CA_CLOSE_CHANNEL { uint64_t BtAddress; L2CAP_CHANNEL_HANDLE ChannelHandle; };
struct OA2DP_CHANNEL {
    OA2DP_CHANNEL_LIFECYCLE Life{};
    L2CAP_CHANNEL_HANDLE Handle = nullptr;
    FakeLock *CloseLock = nullptr;
    FakeEvent OpenFinished, SubmissionsIdle;
};
static NTSTATUS close_status, open_wait_status;
static bool allocation_fails;
static unsigned queued, submitted, service_events, delivered, failed_connects;
static FakeLock spin, closes[Oa2dpChannelCount];
static void *Allocate(int, int) {
    return allocation_fails ? nullptr : std::calloc(1, sizeof(_BRB_L2CA_CLOSE_CHANNEL));
}
static void Free(PBRB b) { std::free(b); }
struct OA2DP_DEVICE_CONTEXT {
    OA2DP_CHANNEL Channels[Oa2dpChannelCount];
    FakeLock *Lock = &spin;
    bool PowerQuiesced = false, SurpriseRemoved = false;
    uint64_t RemoteAddress = 0;
    void *PublicationWork = nullptr;
    struct { void *(*BthAllocateBrb)(int, int) = Allocate; void (*BthFreeBrb)(PBRB) = Free; } Profile;
    OA2DP_MPM_REQUEST Request{};
};
static NTSTATUS WdfWaitLockAcquire(FakeLock *l, void *) {
    REQUIRE(!l->Held); l->Held = true; return STATUS_SUCCESS;
}
static void WdfWaitLockRelease(FakeLock *l) { REQUIRE(l->Held); l->Held = false; }
static void WdfSpinLockAcquire(FakeLock *l) { REQUIRE(!l->Held); l->Held = true; }
static void WdfSpinLockRelease(FakeLock *l) { REQUIRE(l->Held); l->Held = false; }
static NTSTATUS KeWaitForSingleObject(FakeEvent *, int, int, bool, LARGE_INTEGER *timeout) {
    REQUIRE(!spin.Held); return timeout ? open_wait_status : STATUS_SUCCESS;
}
static NTSTATUS Oa2dpSubmitBrbSync(OA2DP_DEVICE_CONTEXT *, _BRB *, ULONG) {
    REQUIRE(!spin.Held); ++submitted; return close_status;
}
static void Oa2dpQueueEventLocked(OA2DP_DEVICE_CONTEXT *, int, OA2DP_CHANNEL *, ULONG) {
    REQUIRE(spin.Held); ++service_events;
}
static void Oa2dpDeliverEvents(OA2DP_DEVICE_CONTEXT *) { REQUIRE(!spin.Held); ++delivered; }
static void Oa2dpMpmConnectFailed(OA2DP_DEVICE_CONTEXT *d, ULONG generation) {
    REQUIRE(!spin.Held); ++failed_connects;
    (void)Oa2dpMpmFailConnect(&d->Request, generation);
}
static void WdfWorkItemEnqueue(void *) {
    REQUIRE(!spin.Held);
    for (const auto &lock : closes) REQUIRE(!lock.Held);
    ++queued;
}

#include "channel-close-under-test.inc"

static OA2DP_DEVICE_CONTEXT device() {
    spin = {}; for (auto &lock : closes) lock = {};
    queued = submitted = service_events = delivered = failed_connects = 0;
    close_status = open_wait_status = STATUS_SUCCESS; allocation_fails = false;
    OA2DP_DEVICE_CONTEXT d;
    for (unsigned i = 0; i < Oa2dpChannelCount; ++i) {
        auto &c = d.Channels[i]; c.CloseLock = &closes[i];
        auto generation = Oa2dpChannelBeginOpen(&c.Life, 1, 0);
        c.Life.ConnectionGeneration = generation;
        REQUIRE(Oa2dpChannelFinishOpen(&c.Life, generation, 1));
        c.Handle = &closes[i];
    }
    return d;
}
int main() {
    auto d = device();
    // No audio child or encoder callback is present. Actual local closes
    // must still queue Windows-state publication, without service echo.
    auto request = Oa2dpMpmBeginRequest(&d.Request, 0);
    REQUIRE(CloseChannelGeneration(&d, Oa2dpChannelMedia, 0, 1, nullptr) == STATUS_SUCCESS);
    REQUIRE(queued == 1 && !Oa2dpConnectionClosed(&d.Channels[0].Life, &d.Channels[1].Life));
    REQUIRE(CloseChannelGeneration(&d, Oa2dpChannelSignaling, 0, 1, nullptr) == STATUS_SUCCESS);
    REQUIRE(queued == 2 && submitted == 2 && !service_events && !delivered);
    REQUIRE(Oa2dpConnectionClosed(&d.Channels[0].Life, &d.Channels[1].Life));
    REQUIRE(Oa2dpMpmConnectionCompletion(&d.Request, 0, 1) == request);

    d = device(); close_status = STATUS_IO_TIMEOUT;
    REQUIRE(CloseChannelGeneration(&d, Oa2dpChannelMedia, 0, 1, nullptr) == STATUS_IO_TIMEOUT);
    REQUIRE(!queued && d.Channels[1].Life.State == ChannelClosing && d.Channels[1].Handle);
    close_status = STATUS_SUCCESS;
    REQUIRE(CloseChannelGeneration(&d, Oa2dpChannelMedia, 0, 1, nullptr) == STATUS_SUCCESS);
    REQUIRE(queued == 1 && !d.Channels[1].Handle);
    REQUIRE(CloseChannelGeneration(&d, Oa2dpChannelMedia, 0, 1, nullptr) == STATUS_SUCCESS);
    REQUIRE(queued == 2 && submitted == 2); // already closed still reconciles state

    d = device();
    REQUIRE(CloseChannelGeneration(&d, Oa2dpChannelMedia, 0, 9, nullptr) == STATUS_SUCCESS);
    REQUIRE(!queued && !submitted && d.Channels[1].Life.State == ChannelOpen);
    REQUIRE(CloseChannelGeneration(&d, Oa2dpChannelMedia, 9, 0, nullptr) == STATUS_NOT_FOUND);
    REQUIRE(!queued && !submitted); // obsolete close cannot mutate replacement
    REQUIRE(CloseChannelGeneration(&d, Oa2dpChannelMedia, 1, 0, nullptr) == STATUS_SUCCESS);
    REQUIRE(queued == 1 && service_events == 1 && delivered == 1); // peer-close retains service event

    d = device(); allocation_fails = true;
    REQUIRE(CloseChannelGeneration(&d, Oa2dpChannelMedia, 0, 1, nullptr) == STATUS_INSUFFICIENT_RESOURCES);
    REQUIRE(!queued && !submitted && d.Channels[1].Handle);
    d = device(); d.PowerQuiesced = true;
    REQUIRE(CloseChannelGeneration(&d, Oa2dpChannelMedia, 0, 1, nullptr) == STATUS_DEVICE_NOT_READY);
    REQUIRE(!queued && !submitted && d.Channels[1].Handle);
    d = device(); d.Channels[1].Life.State = ChannelOpening; open_wait_status = STATUS_TIMEOUT;
    REQUIRE(CloseChannelGeneration(&d, Oa2dpChannelMedia, 0, 1, nullptr) == STATUS_IO_TIMEOUT);
    REQUIRE(!queued && !submitted && d.Channels[1].Life.CloseRequested);
    std::puts("production close-path publication checks passed (all OS calls fake)");
}
