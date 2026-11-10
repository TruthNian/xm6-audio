/* SPDX-License-Identifier: MIT */
/* Production handle lifetime methods with a controllable fake handle table.
 * Real threads rendezvous at close/reuse, without racing unsynchronized h_
 * reads against its write. The observable mutex only records contention. */
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#define REQUIRE(x)                                                                                 \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                   \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)
using HANDLE = intptr_t;
using DWORD = uint32_t;
constexpr HANDLE INVALID_HANDLE_VALUE = -1, device_handle = 16;
constexpr DWORD GENERIC_READ = 1, GENERIC_WRITE = 2, OPEN_EXISTING = 3;
constexpr DWORD FILE_FLAG_OVERLAPPED = 4, ERROR_SUCCESS = 0;
constexpr DWORD ERROR_ACCESS_DENIED = 5, ERROR_UNRELATED = 1234;
static thread_local DWORD last_error;
static DWORD GetLastError() { return last_error; }
static void SetLastError(DWORD error) { last_error = error; }
struct Kernel {
    std::mutex lock;
    std::condition_variable wake;
    bool alive = false, reused = false, block_close = false, close_entered = false;
    bool release_close = false, cancel_returned = false;
    bool block_create = false, create_entered = false, release_create = false;
    DWORD create_error = ERROR_SUCCESS;
    unsigned cancellations = 0, wrong_object = 0, contenders = 0, closes = 0;
} kernel;

class ObservableMutex {
  public:
    void lock() {
        if (mutex_.try_lock())
            return;
        {
            std::lock_guard guard(kernel.lock);
            ++kernel.contenders;
            kernel.wake.notify_all();
        }
        mutex_.lock();
    }
    void unlock() {
        mutex_.unlock();
        // The C++ mutex API does not promise Win32 last-error preservation.
        SetLastError(ERROR_UNRELATED);
    }

  private:
    std::mutex mutex_;
};
static HANDLE CreateFileW(const wchar_t *, DWORD, DWORD, void *, DWORD, DWORD, void *) {
    std::unique_lock guard(kernel.lock);
    REQUIRE(!kernel.alive);
    if (kernel.create_error) {
        SetLastError(kernel.create_error);
        return INVALID_HANDLE_VALUE;
    }
    kernel.alive = true;
    kernel.reused = false;
    kernel.create_entered = true;
    kernel.wake.notify_all();
    if (kernel.block_create)
        REQUIRE(kernel.wake.wait_for(guard, std::chrono::seconds(5),
                                     [] { return kernel.release_create; }));
    return device_handle;
}
static bool CancelIoEx(HANDLE handle, void *) {
    std::lock_guard guard(kernel.lock);
    REQUIRE(handle == device_handle);
    ++kernel.cancellations;
    if (kernel.reused)
        ++kernel.wrong_object;
    kernel.wake.notify_all();
    return true;
}
static bool CloseHandle(HANDLE handle) {
    std::unique_lock guard(kernel.lock);
    REQUIRE(handle == device_handle && kernel.alive);
    ++kernel.closes;
    kernel.alive = false;
    kernel.reused = true; // another file now owns this numeric handle
    kernel.close_entered = true;
    kernel.wake.notify_all();
    if (kernel.block_close)
        REQUIRE(kernel.wake.wait_for(guard, std::chrono::seconds(5),
                                     [] { return kernel.release_close; }));
    return true;
}
struct DriverHandle {
    HANDLE h_ = INVALID_HANDLE_VALUE;
    ObservableMutex handle_mu_;
    ~DriverHandle() { close(); }
    bool open(const std::wstring &path);
    void close();
    void close_locked();
    void cancel_all();
};
#include "handle-lifetime-under-test.inc"

static void reset_kernel() {
    REQUIRE(!kernel.alive);
    kernel.reused = kernel.block_close = kernel.close_entered = false;
    kernel.release_close = kernel.cancel_returned = false;
    kernel.block_create = kernel.create_entered = kernel.release_create = false;
    kernel.create_error = ERROR_SUCCESS;
    kernel.cancellations = kernel.wrong_object = kernel.contenders = kernel.closes = 0;
}

static void cancel_during_close() {
    DriverHandle dev;
    REQUIRE(dev.open(L"fake-device"));
    kernel.block_close = true;
    std::thread closer([&] { dev.close(); });
    {
        std::unique_lock guard(kernel.lock);
        REQUIRE(kernel.wake.wait_for(guard, std::chrono::seconds(5),
                                     [] { return kernel.close_entered; }));
    }
    std::thread canceller([&] {
        dev.cancel_all();
        std::lock_guard guard(kernel.lock);
        kernel.cancel_returned = true;
        kernel.wake.notify_all();
    });
    {
        std::unique_lock guard(kernel.lock);
        // Unfixed code cancels the reused object. Fixed code waits on the
        // same lifetime lock until h_ is invalid, then performs no OS cancel.
        REQUIRE(kernel.wake.wait_for(guard, std::chrono::seconds(5),
                                     [] { return kernel.cancel_returned || kernel.contenders; }));
        kernel.release_close = true;
        kernel.wake.notify_all();
    }
    closer.join();
    canceller.join();
    REQUIRE(!kernel.wrong_object);
    REQUIRE(kernel.cancellations == 1 && dev.h_ == INVALID_HANDLE_VALUE);
    REQUIRE(kernel.closes == 1);
}

static void cancel_during_open() {
    DriverHandle dev;
    kernel.block_create = true;
    std::thread opener([&] { REQUIRE(dev.open(L"fake-device")); });
    {
        std::unique_lock guard(kernel.lock);
        REQUIRE(kernel.wake.wait_for(guard, std::chrono::seconds(5),
                                     [] { return kernel.create_entered; }));
    }
    std::thread canceller([&] {
        dev.cancel_all();
        std::lock_guard guard(kernel.lock);
        kernel.cancel_returned = true;
        kernel.wake.notify_all();
    });
    {
        std::unique_lock guard(kernel.lock);
        REQUIRE(kernel.wake.wait_for(guard, std::chrono::seconds(5),
                                     [] { return kernel.cancel_returned || kernel.contenders; }));
        // Cancellation observes the published handle after open releases its
        // lifetime lock, instead of missing a concurrent CreateFile entirely.
        REQUIRE(kernel.contenders && !kernel.cancel_returned);
        kernel.release_create = true;
        kernel.wake.notify_all();
    }
    opener.join();
    canceller.join();
    REQUIRE(kernel.cancellations == 1 && !kernel.wrong_object);
    dev.close();
    REQUIRE(kernel.cancellations == 2 && kernel.closes == 1);
}

int main() {
    cancel_during_close();
    reset_kernel();
    cancel_during_open();
    reset_kernel();
    {
        DriverHandle dev;
        REQUIRE(dev.open(L"first-device"));
        REQUIRE(dev.open(L"replacement-device"));
        REQUIRE(kernel.cancellations == 1 && kernel.closes == 1);
        dev.cancel_all();
        REQUIRE(kernel.cancellations == 2 && !kernel.wrong_object);
        dev.close();
        dev.close();
        dev.cancel_all();
        REQUIRE(kernel.cancellations == 3 && kernel.closes == 2 && !kernel.wrong_object);
        kernel.create_error = ERROR_ACCESS_DENIED;
        REQUIRE(!dev.open(L"denied-device"));
        REQUIRE(GetLastError() == ERROR_ACCESS_DENIED); // survives mutex unlock
        REQUIRE(dev.h_ == INVALID_HANDLE_VALUE);
        dev.cancel_all();
        REQUIRE(kernel.cancellations == 3);
    }
    std::puts("production handle lifetime checks passed (OS APIs fake, real threads)");
}
