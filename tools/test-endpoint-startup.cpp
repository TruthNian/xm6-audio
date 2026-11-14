/* SPDX-License-Identifier: MIT */
/* Real StartDevice body, controlled PortCls/service announcement boundaries. */
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <mutex>
#include <string>
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
#ifndef _In_
#define _In_
#endif
#ifndef _Inout_
#define _Inout_
#endif
#define PAGED_CODE() ((void)0)
#define ASSERT(x) REQUIRE(x)
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#define NT_SUCCESS(x) ((x) >= 0)
using NTSTATUS = int;
using VOID = void;
using BOOLEAN = bool;
constexpr bool FALSE = false;
using PIRP = void *;
using PRESOURCELIST = void *;
constexpr NTSTATUS STATUS_SUCCESS = 0, STATUS_DEVICE_NOT_READY = -1;
struct FakeLock { bool held = false; };
struct OA2DP_AUDIO_DEVICE { bool Initialized = true, Started = false, Removed = false; wchar_t Name[128]{}; FakeLock FilterLock, ControlLock, Lock; void *WavePort = nullptr; };
struct DEVICE_OBJECT { OA2DP_AUDIO_DEVICE *dev; };
using PDEVICE_OBJECT = DEVICE_OBJECT *;
static OA2DP_AUDIO_DEVICE *OA2DP_DEVICE(DEVICE_OBJECT *d) { return d->dev; }
static void Oa2dpLock(FakeLock *l) { REQUIRE(!l->held); l->held = true; }
static void Oa2dpUnlock(FakeLock *l) { REQUIRE(l->held); l->held = false; }
static unsigned installs, removes, controls;
static NTSTATUS install_status, control_status, init_status;
static NTSTATUS InitDevice(OA2DP_AUDIO_DEVICE *d, DEVICE_OBJECT *) { d->Initialized = init_status >= 0; return init_status; }
static NTSTATUS InstallFilters(OA2DP_AUDIO_DEVICE *d) { REQUIRE(d->FilterLock.held && d->Name[0]); ++installs; if(install_status >= 0) d->WavePort = d; return install_status; }
static void RemoveFilters(OA2DP_AUDIO_DEVICE *d) { REQUIRE(d->FilterLock.held); ++removes; }
static NTSTATUS Oa2dpControlStart(OA2DP_AUDIO_DEVICE *d) { ++controls; d->Started = control_status >= 0; return control_status; }
static void Oa2dpControlStop(OA2DP_AUDIO_DEVICE *d) { d->Started = false; }
static NTSTATUS Oa2dpControlStatus(OA2DP_AUDIO_DEVICE *d, bool releaseOnly) {
    REQUIRE(d->FilterLock.held && !releaseOnly);
    return d->Started && !d->Removed ? 0 : STATUS_DEVICE_NOT_READY;
}
#include "endpoint-start-device-under-test.inc"
#include "endpoint-ensure-named-under-test.inc"
static void reset() { installs = removes = controls = 0; install_status = control_status = init_status = 0; }
namespace admission {
#define LOGW(...) ((void)0)
#define LOGE(...) ((void)0)
struct OA2DP_FORMAT_CHANGE {
    unsigned ConnectionEnabled = 1, SuspendRequested = 0, SuspendSerial = 0;
    unsigned ConnectionSerial = 0, PcmSequence = 0, Running = 0, Prepared = 0, Format = 0;
};
struct Audio {
    bool name_ok = true; unsigned opens = 0, closes = 0, maps = 0, names = 0;
    std::wstring last_name;
    bool open(unsigned) { ++opens; return true; }
    bool set_name(const std::wstring &n) { ++names; last_name = n; return name_ok; }
    void close() { ++closes; }
    bool map_ring() { ++maps; return true; }
    bool get_state(OA2DP_FORMAT_CHANGE &) { return true; }
};
struct Session {
    bool audio_ready_ = false, stopping_ = false, connection_enabled_ = false;
    bool suspend_requested_ = false, audio_requested_ = false;
    unsigned suspend_serial_ = 0, connection_serial_ = 0, audio_format_ = 0, pcm_sequence_ = 0;
    int stop_event_ = 0, control_cancel_event_ = 1;
    struct { unsigned RemoteAddress = 1; } info_;
    Audio audio_; std::wstring name_; std::mutex mu_;
    bool ensure_audio_endpoint();
};
static void WaitForSingleObject(int, unsigned) { REQUIRE(false); }
static void SetEvent(int) {}
static void ResetEvent(int) {}
#include "endpoint-name-admission-under-test.inc"
static void check() {
    Session s; s.name_ = L"WH-1000XM6"; s.audio_.name_ok = false;
    REQUIRE(!s.ensure_audio_endpoint() && !s.audio_ready_);
    REQUIRE(s.audio_.closes == 1 && !s.audio_.maps && s.audio_.last_name == s.name_);
    s.audio_.name_ok = true;
    REQUIRE(s.ensure_audio_endpoint() && s.audio_ready_ && s.audio_.maps == 1);
    REQUIRE(s.ensure_audio_endpoint() && s.audio_.names == 2 && s.audio_.opens == 2);
    Session unknown;
    REQUIRE(unknown.ensure_audio_endpoint() && unknown.audio_.last_name == L"Bluetooth Headphones");
}
}
int main() {
    OA2DP_AUDIO_DEVICE dev; DEVICE_OBJECT object{&dev};
    reset();
    REQUIRE(StartDevice(&object, nullptr, nullptr) == 0);
    REQUIRE(dev.Started && controls == 1 && installs == 0); // no placeholder endpoint
    std::wcscpy(dev.Name, L"WH-1000XM6");
    reset(); dev.Started = false;
    REQUIRE(StartDevice(&object, nullptr, nullptr) == 0);
    REQUIRE(dev.Started && controls == 1 && installs == 1);
    reset(); dev.Started = false; install_status = -2;
    REQUIRE(StartDevice(&object, nullptr, nullptr) == -2);
    REQUIRE(!dev.Started && !controls && removes == 1);
    reset(); dev.Started = false; control_status = -3;
    REQUIRE(StartDevice(&object, nullptr, nullptr) == -3);
    REQUIRE(!dev.Started && installs == 1 && removes == 1);
    reset(); dev.Initialized = false; init_status = -4;
    REQUIRE(StartDevice(&object, nullptr, nullptr) == -4);
    REQUIRE(!installs && !controls);
    // Service supplies the first name, retries failures, and cannot recreate a
    // stopped/removed endpoint. A repeated name never re-registers filters.
    reset(); dev.Started = true; dev.WavePort = nullptr; install_status = -5;
    REQUIRE(Oa2dpEnsureNamedFilters(&dev) == -5 && installs == 1 && !dev.WavePort);
    install_status = 0;
    REQUIRE(Oa2dpEnsureNamedFilters(&dev) == 0 && installs == 2 && dev.WavePort);
    REQUIRE(Oa2dpEnsureNamedFilters(&dev) == 0 && installs == 2);
    dev.Started = false; dev.WavePort = nullptr;
    REQUIRE(Oa2dpEnsureNamedFilters(&dev) == STATUS_DEVICE_NOT_READY && installs == 2);
    dev.Started = true; dev.Removed = true;
    REQUIRE(Oa2dpEnsureNamedFilters(&dev) == STATUS_DEVICE_NOT_READY && installs == 2);
    REQUIRE(!dev.FilterLock.held && !dev.ControlLock.held);
    admission::check();
    std::puts("production startup defers first endpoint until its name exists; cached startup and failures passed");
}
