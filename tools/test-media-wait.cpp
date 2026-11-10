/* SPDX-License-Identifier: MIT */
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#define REQUIRE(x)                                                                                 \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                   \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)
using DWORD = uint32_t;
using ULONGLONG = uint64_t;
using BOOL = bool;
constexpr bool TRUE = true, FALSE = false;
constexpr DWORD WAIT_OBJECT_0 = 0, WAIT_TIMEOUT = 258, WAIT_FAILED = 0xffffffff,
                INFINITE = 0xffffffff;
constexpr DWORD ERROR_IO_INCOMPLETE = 996, ERROR_OPERATION_ABORTED = 995;
struct Event {
    bool signalled = false;
};
using HANDLE = Event *;
struct OVERLAPPED {
    int completion = 0;
}; // 0 pending, 1 success, 2 terminal cancellation
static DWORD error;
static ULONGLONG now;
static unsigned cancels;
static bool fail_wait;
static DWORD GetLastError() { return error; }
static BOOL GetOverlappedResult(int, OVERLAPPED *o, DWORD *, BOOL block) {
    REQUIRE(!block);
    if (o->completion == 1)
        return true;
    error = o->completion == 2 ? ERROR_OPERATION_ABORTED : ERROR_IO_INCOMPLETE;
    return false;
}
static DWORD WaitForMultipleObjects(DWORD n, HANDLE *events, BOOL all, DWORD timeout) {
    REQUIRE(!all);
    if (fail_wait)
        return WAIT_FAILED;
    for (DWORD i = 0; i < n; ++i)
        if (events[i]->signalled)
            return WAIT_OBJECT_0 + i;
    REQUIRE(timeout != INFINITE);
    now += timeout;
    return WAIT_TIMEOUT;
}
static void CancelIoEx(int, OVERLAPPED *) { ++cancels; } // does not imply completion
static ULONGLONG GetTickCount64() { return now; }
struct Transport {
    struct Slot {
        OVERLAPPED ov;
        Event event_storage;
        HANDLE event = &event_storage;
        bool busy = false;
    };
    struct Device {
        bool is_open() const { return true; }
        int get() const { return 1; }
    } dev_;
    std::array<Slot, 8> slots_{};
    bool finish_slot(Slot &, DWORD, HANDLE = nullptr);
    bool drain_media(DWORD);
};
#include "media-drain-under-test.inc"
#include "media-finish-under-test.inc"
int main() {
    Transport tr;
    auto &s = tr.slots_[0];
    s.busy = true;
    now = 0;
    cancels = 0;
    REQUIRE(!tr.finish_slot(s, 10) && s.busy && cancels == 1 && now == 10);
    REQUIRE(s.ov.completion == 0); // timeout retains the actual I/O object
    Event stop{true};
    REQUIRE(!tr.finish_slot(s, 1000, &stop) && s.busy && now == 10);
    s.ov.completion = 2;
    REQUIRE(!tr.finish_slot(s, 0) && !s.busy); // terminal cancellation safely retires
    for (auto &slot : tr.slots_) {
        slot.busy = true;
        slot.ov.completion = 0;
    }
    now = 0;
    REQUIRE(!tr.drain_media(250) && now == 250); // one budget, not 8 * 250 ms
    for (auto &slot : tr.slots_)
        REQUIRE(slot.busy);
    for (auto &slot : tr.slots_)
        slot.ov.completion = 2;
    REQUIRE(tr.drain_media(0) && now == 250);
    s.busy = true;
    s.ov.completion = 0;
    s.event_storage.signalled = true;
    REQUIRE(!tr.finish_slot(s, 0) && s.busy); // spurious event is not completion
    s.event_storage.signalled = false;
    fail_wait = true;
    REQUIRE(!tr.finish_slot(s, 0) && s.busy); // wait failure does not free buffers
    fail_wait = false;
    s.ov.completion = 1;
    REQUIRE(tr.finish_slot(s, 0) && !s.busy);
    std::puts("production media wait cancellation, ownership and total-budget checks passed (OS "
              "APIs fake)");
}
