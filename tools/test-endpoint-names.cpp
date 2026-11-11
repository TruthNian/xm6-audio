/* SPDX-License-Identifier: MIT */
/* Execute actual metadata publication, SetName and startup wiring. Windows
 * property APIs are controlled boundaries, not an EndpointBuilder simulation. */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <map>
#include <string>
#include <vector>
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
#ifndef _In_
#define _In_
#endif
#ifndef _Inout_
#define _Inout_
#endif
#define PAGED_CODE() ((void)0)
#define NT_SUCCESS(x) ((x) >= 0)
#define RTL_NUMBER_OF(x) (sizeof(x) / sizeof((x)[0]))
#define KdPrintEx(x) ((void)0)
using NTSTATUS = int32_t;
using ULONG = uint32_t;
using GUID = unsigned;
using VOID = void;
using WCHAR = wchar_t;
using PCWSTR = const WCHAR *;
constexpr NTSTATUS STATUS_SUCCESS = 0, STATUS_INVALID_PARAMETER = -1;
constexpr ULONG LOCALE_NEUTRAL = 0, PLUGPLAY_PROPERTY_PERSISTENT = 1, DEVPROP_TYPE_STRING = 18, REG_SZ = 1;
constexpr GUID KSCATEGORY_AUDIO = 1, KSCATEGORY_RENDER = 2, KSCATEGORY_REALTIME = 3, KSCATEGORY_TOPOLOGY = 4;
constexpr unsigned DEVPKEY_Device_FriendlyName = 1, DEVPKEY_DeviceInterface_FriendlyName = 2;
constexpr PCWSTR OA2DP_WAVE_NAME = L"WaveOa2dp", OA2DP_TOPO_NAME = L"TopologyOa2dp", NAME_VALUE = L"Oa2dpName";
struct Lock { bool held = false; };
struct OA2DP_AUDIO_DEVICE { int Pdo = 1; Lock ControlLock; WCHAR Name[128]{}; };
struct OA2DP_ENDPOINT_NAME { WCHAR Name[128]{}; };
struct UNICODE_STRING { std::wstring text; bool allocated = false; };
static OA2DP_AUDIO_DEVICE *selected;
static std::map<std::wstring, std::wstring> names;
static std::vector<std::wstring> attempted;
static unsigned saves, allocated, freed;
static unsigned ensure_calls;
static NTSTATUS ensure_status;
static std::wstring saved_name, register_failure, property_failure;
static bool fail_device;
static void Oa2dpLock(Lock *lock) { REQUIRE(!lock->held); lock->held = true; }
static void Oa2dpUnlock(Lock *lock) { REQUIRE(lock->held); lock->held = false; }
static NTSTATUS RtlStringCchLengthW(PCWSTR text, size_t cap, size_t *length) {
    for (size_t i = 0; i < cap; ++i) if (!text[i]) { *length = i; return 0; }
    return STATUS_INVALID_PARAMETER;
}
static void RtlStringCchCopyW(WCHAR *to, size_t cap, PCWSTR from) {
    REQUIRE(std::wcslen(from) < cap); std::wcscpy(to, from);
}
static void RtlInitUnicodeString(UNICODE_STRING *s, PCWSTR text) { s->text = text; s->allocated = false; }
static void RtlFreeUnicodeString(UNICODE_STRING *s) { REQUIRE(s->allocated); s->allocated = false; ++freed; }
static void require_property(unsigned key, unsigned expected, ULONG locale, ULONG flags, ULONG type, ULONG bytes, PCWSTR name) {
    REQUIRE(selected->ControlLock.held && key == expected && locale == 0 && flags == PLUGPLAY_PROPERTY_PERSISTENT);
    REQUIRE(type == DEVPROP_TYPE_STRING && bytes == (std::wcslen(name) + 1) * sizeof(WCHAR));
}
static NTSTATUS IoSetDevicePropertyData(int pdo, const unsigned *key, ULONG locale, ULONG flags, ULONG type, ULONG bytes, PCWSTR name) {
    REQUIRE(pdo == selected->Pdo);
    require_property(*key, DEVPKEY_Device_FriendlyName, locale, flags, type, bytes, name);
    attempted.push_back(L"device");
    if (fail_device) return -2;
    names[L"device"] = name;
    return 0;
}
static NTSTATUS IoRegisterDeviceInterface(int pdo, const GUID *category, UNICODE_STRING *ref, UNICODE_STRING *link) {
    REQUIRE(selected->ControlLock.held && pdo == selected->Pdo);
    link->text = std::to_wstring(*category) + L":" + ref->text;
    attempted.push_back(link->text);
    if (link->text == register_failure) return -3;
    link->allocated = true; ++allocated;
    return 0;
}
static NTSTATUS IoSetDeviceInterfacePropertyData(UNICODE_STRING *link, const unsigned *key, ULONG locale, ULONG flags, ULONG type, ULONG bytes, PCWSTR name) {
    REQUIRE(link->allocated);
    require_property(*key, DEVPKEY_DeviceInterface_FriendlyName, locale, flags, type, bytes, name);
    if (link->text == property_failure) return -4;
    names[link->text] = name;
    return 0;
}
static void SaveValue(OA2DP_AUDIO_DEVICE *dev, PCWSTR key, ULONG type, PCWSTR name, ULONG bytes) {
    REQUIRE(dev == selected && dev->ControlLock.held && std::wcscmp(key, NAME_VALUE) == 0 && type == REG_SZ);
    REQUIRE(bytes == (std::wcslen(name) + 1) * sizeof(WCHAR));
    ++saves; saved_name = name;
}
#include "endpoint-name-publication-under-test.inc"
static NTSTATUS Oa2dpEnsureNamedFilters(OA2DP_AUDIO_DEVICE *dev) {
    REQUIRE(dev == selected && !dev->ControlLock.held && dev->Name[0]);
    ++ensure_calls; return ensure_status;
}
#include "endpoint-name-update-under-test.inc"
static NTSTATUS startup(OA2DP_AUDIO_DEVICE *dev) {
    NTSTATUS ntStatus;
#include "endpoint-name-startup-under-test.inc"
    return ntStatus;
}
static void check_names(PCWSTR expected) {
    REQUIRE(names.size() == 6 && allocated == freed && !selected->ControlLock.held);
    for (const auto &entry : names) REQUIRE(entry.second == expected);
    for (PCWSTR key : {L"device", L"1:WaveOa2dp", L"2:WaveOa2dp", L"3:WaveOa2dp", L"1:TopologyOa2dp", L"4:TopologyOa2dp"})
        REQUIRE(names.count(key) == 1);
}
int main() {
    OA2DP_AUDIO_DEVICE dev;
    selected = &dev;
    REQUIRE(startup(&dev) == STATUS_INVALID_PARAMETER && attempted.empty());
    std::wcscpy(dev.Name, L"WH-1000XM6");
    REQUIRE(startup(&dev) == 0); check_names(dev.Name); REQUIRE(saves == 0);
    OA2DP_ENDPOINT_NAME in;
    std::wcscpy(in.Name, dev.Name);
    names.clear(); // INF reinstall overwrote published properties, cache did not change
    REQUIRE(SetName(&dev, &in) == 0); check_names(in.Name); REQUIRE(saves == 0);
    names.clear(); attempted.clear();
    fail_device = true; register_failure = L"2:WaveOa2dp"; property_failure = L"4:TopologyOa2dp";
    REQUIRE(SetName(&dev, &in) == 0 && attempted.size() == 6 && names.size() == 3);
    REQUIRE(allocated == freed && !dev.ControlLock.held);
    REQUIRE(startup(&dev) < 0); // initial registration must see publication errors
    fail_device = false; register_failure.clear(); property_failure.clear();
    REQUIRE(SetName(&dev, &in) == 0); check_names(in.Name); // equal name retries failed properties
    std::wcscpy(in.Name, L"XM6 \u8033\u673a");
    REQUIRE(SetName(&dev, &in) == 0); check_names(in.Name);
    REQUIRE(saves == 1 && saved_name == in.Name && std::wcscmp(dev.Name, in.Name) == 0);
    ensure_status = -9;
    REQUIRE(SetName(&dev, &in) == -9 && saves == 1); // first-filter failure reaches service
    ensure_status = 0;
    REQUIRE(SetName(&dev, &in) == 0 && saves == 1); // cached retry can complete creation
    attempted.clear(); in.Name[0] = 0;
    REQUIRE(SetName(&dev, &in) == STATUS_INVALID_PARAMETER && attempted.empty() && saves == 1);
    REQUIRE(ensure_calls == 6);
    std::puts("production endpoint-name startup, cached retry, category coverage and failure isolation passed (OS APIs fake)");
}
