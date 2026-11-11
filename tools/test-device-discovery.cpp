/* SPDX-License-Identifier: MIT */
/* Execute the production manager across enumeration/notification orderings.
 * CM callbacks and Sessions are controlled; retirement uses real std::async. */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)
#define LOGI(...) ((void)0)
#define LOGE(...) ((void)0)
#define CALLBACK
using DWORD=uint32_t;
using PVOID=void*;
using HCMNOTIFICATION=void*;
constexpr int CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE=1, GUID_DEVINTERFACE_OA2DP_TRANSPORT=9;
constexpr DWORD CR_SUCCESS=0, ERROR_SUCCESS=0;
enum CM_NOTIFY_ACTION { CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL, CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL };
struct CM_NOTIFY_FILTER {
    size_t cbSize;
    int FilterType;
    struct { struct { int ClassGuid; } DeviceInterface; } u;
};
struct CM_NOTIFY_EVENT_DATA { struct { struct { const wchar_t *SymbolicLink; } DeviceInterface; } u; };
using PCM_NOTIFY_EVENT_DATA=CM_NOTIFY_EVENT_DATA*;
using Callback=DWORD(*)(HCMNOTIFICATION,PVOID,CM_NOTIFY_ACTION,PCM_NOTIFY_EVENT_DATA,DWORD);
static Callback callback;
static PVOID callback_context;
static bool registration_fails;
static unsigned unregisters;
static std::vector<std::wstring> snapshot;
static std::function<void()> during_registration,after_snapshot;
static void emit(CM_NOTIFY_ACTION action,const wchar_t *path) {
    REQUIRE(callback);
    CM_NOTIFY_EVENT_DATA data{{{path}}};
    REQUIRE(callback(callback_context,callback_context,action,&data,sizeof(data))==ERROR_SUCCESS);
}
static DWORD CM_Register_Notification(CM_NOTIFY_FILTER *filter,PVOID context,Callback cb,HCMNOTIFICATION *out) {
    REQUIRE(filter->cbSize==sizeof(*filter) && filter->FilterType==CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE);
    REQUIRE(filter->u.DeviceInterface.ClassGuid==GUID_DEVINTERFACE_OA2DP_TRANSPORT);
    if(registration_fails) return 1;
    callback=cb;callback_context=context;*out=context;
    if(during_registration) during_registration();
    return CR_SUCCESS;
}
static void CM_Unregister_Notification(HCMNOTIFICATION handle) {
    REQUIRE(handle==callback_context && callback);
    ++unregisters;callback=nullptr;callback_context=nullptr;
}
static std::vector<std::wstring> enumerate_interfaces(int guid) {
    REQUIRE(guid==GUID_DEVINTERFACE_OA2DP_TRANSPORT && callback);
    const auto result=snapshot; // snapshot precedes these later notifications
    if(after_snapshot) after_snapshot();
    return result;
}
static std::atomic<unsigned> starts{0},stops{0},live{0};
struct Session {
    explicit Session(std::wstring value):path_(std::move(value)) { ++live; }
    ~Session() { stop();--live; }
    const std::wstring &path() const { return path_; }
    void start() { REQUIRE(!started_);started_=true;++starts; }
    void stop() { if(started_) {started_=false;++stops;} }
    std::wstring path_;
    bool started_=false;
};
#include "device-discovery-under-test.inc"
static void reset() {
    REQUIRE(!callback && live==0 && starts==stops);
    starts=stops=0;unregisters=0;registration_fails=false;
    snapshot={L"native-interface-A"};during_registration={};after_snapshot={};
}
static void settled(DeviceManager &manager,unsigned expected_starts) {
    manager.stop();
    REQUIRE(starts==expected_starts && stops==expected_starts && live==0 && unregisters==1);
}
int main() {
    reset();
    {
        DeviceManager manager;
        after_snapshot=[] {emit(CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL,L"NATIVE-INTERFACE-A");};
        REQUIRE(manager.start());
        REQUIRE(starts==0); // stale enumeration must not resurrect the removed path
        settled(manager,0);
    }
    reset();
    {
        DeviceManager manager;
        during_registration=[] {emit(CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL,L"native-interface-A");};
        REQUIRE(manager.start());
        REQUIRE(starts==0);
        settled(manager,0);
    }
    reset();
    {
        DeviceManager manager;
        after_snapshot=[] {emit(CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL,L"NATIVE-INTERFACE-A");};
        REQUIRE(manager.start());
        REQUIRE(starts==1); // documented duplicate arrival + enumeration
        settled(manager,1);
    }
    reset();
    {
        DeviceManager manager;
        after_snapshot=[] {
            emit(CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL,L"native-interface-A");
            emit(CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL,L"NATIVE-INTERFACE-A");
        };
        REQUIRE(manager.start());
        REQUIRE(starts==1); // the retired session must not be recreated from the snapshot
        settled(manager,1);
    }
    reset();
    {
        DeviceManager manager;
        after_snapshot=[] {
            emit(CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL,L"native-interface-A");
            emit(CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL,L"NATIVE-INTERFACE-A");
        };
        REQUIRE(manager.start());
        REQUIRE(starts==1); // a real arrival can still create the replacement
        emit(CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL,L"native-interface-A");
        emit(CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL,L"native-interface-A");
        REQUIRE(starts==2);
        settled(manager,2);
    }
    reset();
    {
        DeviceManager manager;
        snapshot={L"native-interface-A",L"native-interface-B",L"NATIVE-INTERFACE-B"};
        after_snapshot=[] {emit(CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL,L"native-interface-A");};
        REQUIRE(manager.start());
        REQUIRE(starts==1); // an unrelated existing device is not suppressed
        settled(manager,1);
        // A later start has a fresh enumeration lifetime.
        after_snapshot={};snapshot={L"native-interface-A"};
        REQUIRE(manager.start());
        REQUIRE(starts==2);
        manager.stop();
        REQUIRE(stops==2 && live==0 && unregisters==2);
    }
    reset();
    {
        DeviceManager manager;
        registration_fails=true;
        REQUIRE(!manager.start());
        manager.stop();
        REQUIRE(!starts && !stops && !unregisters);
        registration_fails=false;
        REQUIRE(manager.start());
        settled(manager,1);
    }
    reset();
    std::puts("production discovery snapshot/removal/arrival orderings passed (CM and Sessions controlled, real async retirement)");
}
