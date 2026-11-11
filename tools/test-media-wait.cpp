/* SPDX-License-Identifier: MIT */
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
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
constexpr DWORD ERROR_SUCCESS = 0, ERROR_IO_INCOMPLETE = 996, ERROR_OPERATION_ABORTED = 995,
                ERROR_IO_PENDING = 997, ERROR_CANCELLED = 1223, IOCTL_OA2DP_SEND = 1;
struct Event {
    bool signalled = false;
};
using HANDLE = Event *;
struct OVERLAPPED {
    int completion = 0;
    HANDLE hEvent = nullptr;
}; // 0 pending, 1 success, 2 terminal cancellation, 3 terminal error
struct OA2DP_SEND_IN {};
static DWORD error;
static ULONGLONG now;
static unsigned cancels;
static bool fail_wait;
static int complete_on_wait;
static OVERLAPPED *last_polled;
static DWORD submit_error = ERROR_IO_PENDING;
static unsigned submissions;
static std::vector<std::string> logs;
static void log_error(const char *fmt, ...) {
    char text[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    logs.emplace_back(text);
}
#define LOGE(...) log_error(__VA_ARGS__)
static DWORD GetLastError() { return error; }
static BOOL GetOverlappedResult(int, OVERLAPPED *o, DWORD *, BOOL block) {
    REQUIRE(!block);
    last_polled = o;
    if (o->completion == 1)
        return true;
    error = o->completion == 2 ? ERROR_OPERATION_ABORTED :
            o->completion == 3 ? 31 : ERROR_IO_INCOMPLETE;
    return false;
}
static DWORD WaitForMultipleObjects(DWORD n, HANDLE *events, BOOL all, DWORD timeout) {
    REQUIRE(!all);
    if (fail_wait) {
        error = 6;
        return WAIT_FAILED;
    }
    if (complete_on_wait) {
        last_polled->completion = complete_on_wait;
        events[n - 1]->signalled = true;
    }
    for (DWORD i = 0; i < n; ++i)
        if (events[i]->signalled)
            return WAIT_OBJECT_0 + i;
    REQUIRE(timeout != INFINITE);
    now += timeout;
    return WAIT_TIMEOUT;
}
static void CancelIoEx(int, OVERLAPPED *) { ++cancels; error = 1168; } // not completion
static DWORD WaitForSingleObject(HANDLE event, DWORD timeout) {
    REQUIRE(timeout == 0);
    error = 4321; // post-failure cancellation checks must not replace the native error
    return event->signalled ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}
static void ResetEvent(HANDLE event) { event->signalled = false; }
static BOOL DeviceIoControl(int, DWORD, void *, DWORD, void *, DWORD, void *, OVERLAPPED *ov) {
    ++submissions;
    error = submit_error;
    if (submit_error == ERROR_SUCCESS) ov->completion = 1;
    return submit_error == ERROR_SUCCESS;
}
static ULONGLONG GetTickCount64() { return now; }
struct Transport {
    struct Slot {
        OVERLAPPED ov;
        Event event_storage;
        HANDLE event = &event_storage;
        bool busy = false;
        std::vector<uint8_t> data;
        OA2DP_SEND_IN binding{};
    };
    struct Device {
        bool is_open() const { return true; }
        int get() const { return 1; }
    } dev_;
    static constexpr size_t kSlots = 8;
    std::array<Slot, kSlots> slots_{};
    size_t next_slot_ = 0;
    struct MediaFailure { const char *stage = "none"; DWORD error = ERROR_SUCCESS; };
    bool finish_slot(Slot &, DWORD, HANDLE = nullptr, MediaFailure * = nullptr);
    bool send_media(const OA2DP_SEND_IN &, const uint8_t *, size_t, HANDLE);
    bool drain_media(DWORD);
};
#include "media-drain-under-test.inc"
#include "media-finish-under-test.inc"
#include "media-send-under-test.inc"
int main() {
    Transport tr;
    auto &s = tr.slots_[0];
    s.busy = true;
    now = 0;
    cancels = 0;
    Transport::MediaFailure failure;
    REQUIRE(!tr.finish_slot(s, 10, nullptr, &failure) && s.busy && cancels == 1 && now == 10);
    REQUIRE(failure.error == WAIT_TIMEOUT && std::strcmp(failure.stage, "wait-timeout") == 0);
    REQUIRE(s.ov.completion == 0); // timeout retains the actual I/O object
    Event stop{true};
    REQUIRE(!tr.finish_slot(s, 1000, &stop, &failure) && s.busy && now == 10);
    REQUIRE(failure.error == ERROR_CANCELLED && std::strcmp(failure.stage, "cancel") == 0);
    s.ov.completion = 2;
    REQUIRE(!tr.finish_slot(s, 0, nullptr, &failure) && !s.busy);
    REQUIRE(failure.error == ERROR_OPERATION_ABORTED && std::strcmp(failure.stage, "completion") == 0);
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
    REQUIRE(!tr.finish_slot(s, 0, nullptr, &failure) && s.busy);
    REQUIRE(failure.error == ERROR_IO_INCOMPLETE && std::strcmp(failure.stage, "completion-pending") == 0);
    s.event_storage.signalled = false;
    fail_wait = true;
    REQUIRE(!tr.finish_slot(s, 0, nullptr, &failure) && s.busy);
    REQUIRE(failure.error == 6 && std::strcmp(failure.stage, "wait") == 0);
    fail_wait = false;
    s.ov.completion = 1;
    REQUIRE(tr.finish_slot(s, 0, nullptr, &failure) && !s.busy);
    REQUIRE(failure.error == ERROR_SUCCESS && std::strcmp(failure.stage, "none") == 0);
    for (int completion : {1, 3}) {
        s.busy = true;
        s.ov.completion = 0;
        s.event_storage.signalled = false;
        complete_on_wait = completion;
        REQUIRE(tr.finish_slot(s, 10, nullptr, &failure) == (completion == 1));
        REQUIRE(!s.busy && failure.error == (completion == 1 ? 0u : 31u));
    }
    complete_on_wait = 0;
    REQUIRE(logs.empty()); // drain never logs normal shutdown cancellations
    Transport sender;
    const uint8_t packet[] = {1, 2, 3};
    OA2DP_SEND_IN binding{};
    Event running{};
    submit_error = 31;
    REQUIRE(!sender.send_media(binding, packet, sizeof(packet), &running));
    REQUIRE(!sender.slots_[0].busy && submissions == 1);
    REQUIRE(logs.size() == 1 && logs.back().find("submission failed: error=31 bytes=3") != std::string::npos);
    logs.clear();
    submit_error = ERROR_IO_PENDING;
    REQUIRE(sender.send_media(binding, packet, sizeof(packet), &running));
    auto &pending = sender.slots_[1];
    REQUIRE(pending.busy && pending.data == std::vector<uint8_t>({1, 2, 3}) && logs.empty());
    sender.next_slot_ = 1;
    const auto *owned = pending.data.data();
    REQUIRE(!sender.send_media(binding, packet, sizeof(packet), &running));
    REQUIRE(pending.busy && pending.data.data() == owned && submissions == 2);
    REQUIRE(logs.size() == 1 && logs.back().find("stage=wait-timeout error=258 slot=1 pending=1") != std::string::npos);
    logs.clear();
    pending.ov.completion = 3;
    REQUIRE(!sender.send_media(binding, packet, sizeof(packet), &running));
    REQUIRE(!pending.busy && submissions == 2);
    REQUIRE(logs.size() == 1 && logs.back().find("stage=completion error=31 slot=1 pending=0") != std::string::npos);
    logs.clear();
    REQUIRE(!sender.send_media(binding, packet, sizeof(packet), &stop) && logs.empty());
    REQUIRE(submissions == 2);
    submit_error = ERROR_SUCCESS;
    REQUIRE(sender.send_media(binding, packet, sizeof(packet), &running) && logs.empty());
    REQUIRE(sender.finish_slot(pending, 0) && !pending.busy);
    std::puts("production media error attribution, cancellation, ownership and total-budget checks passed (OS "
              "APIs fake)");
}
