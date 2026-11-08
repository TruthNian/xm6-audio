/* SPDX-License-Identifier: MIT */
/* No devices opened; exercises the exact parser used by the miniport. */
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <ks.h>
#include <ksmedia.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../driver/oa2dpaudio/ks_attributes.h"

static unsigned checks;
static bool check(const void *buffer, ULONG bytes, NTSTATUS expected,
                  const GUID &expectedMode = AUDIO_SIGNALPROCESSINGMODE_DEFAULT,
                  ULONG expectedBytes = 0)
{
    GUID mode{}; ULONG consumed = 0xFFFFFFFF;
    const NTSTATUS status = Oa2dpParseModeAttributes(buffer, bytes, &mode, &consumed);
    ++checks;
    if (status != expected || consumed != expectedBytes ||
        (status == STATUS_SUCCESS && !IsEqualGUID(mode, expectedMode)))
    {
        std::fprintf(stderr, "KS attribute check %u failed: status=%08lX consumed=%lu\n",
            checks, static_cast<ULONG>(status), consumed);
        return false;
    }
    return true;
}
struct Request {
    KSMULTIPLE_ITEM items;
    KSATTRIBUTE_AUDIOSIGNALPROCESSING_MODE attribute;
};
static Request request(const GUID &mode)
{
    Request value{};
    value.items.Size = sizeof(value); value.items.Count = 1;
    value.attribute.AttributeHeader.Size = sizeof(value.attribute);
    value.attribute.AttributeHeader.Attribute = KSATTRIBUTEID_AUDIOSIGNALPROCESSING_MODE;
    value.attribute.SignalProcessingMode = mode;
    return value;
}
int main()
{
    static_assert(sizeof(Request) == 48, "KS wire-layout changed");
    bool ok = check(nullptr, 0, STATUS_SUCCESS);
    ok &= check(nullptr, 1, STATUS_INVALID_PARAMETER);
    KSMULTIPLE_ITEM empty{sizeof(KSMULTIPLE_ITEM), 0};
    ok &= check(&empty, sizeof(empty), STATUS_SUCCESS, AUDIO_SIGNALPROCESSINGMODE_DEFAULT, sizeof(empty));
    for (const GUID &mode : {AUDIO_SIGNALPROCESSINGMODE_DEFAULT, AUDIO_SIGNALPROCESSINGMODE_RAW})
    {
        auto value = request(mode);
        ok &= check(&value, sizeof(value), STATUS_SUCCESS, mode, sizeof(value));
        value.attribute.AttributeHeader.Flags = KSATTRIBUTE_REQUIRED;
        ok &= check(&value, sizeof(value), STATUS_SUCCESS, mode, sizeof(value));
        for (ULONG bytes = 1; bytes < sizeof(value); ++bytes)
            ok &= check(&value, bytes, STATUS_INVALID_PARAMETER);
    }
    auto value = request(AUDIO_SIGNALPROCESSINGMODE_COMMUNICATIONS);
    ok &= check(&value, sizeof(value), STATUS_NOT_SUPPORTED);
    value = request(AUDIO_SIGNALPROCESSINGMODE_DEFAULT);
    value.attribute.AttributeHeader.Attribute = GUID_NULL;
    ok &= check(&value, sizeof(value), STATUS_NOT_SUPPORTED);
    value = request(AUDIO_SIGNALPROCESSINGMODE_DEFAULT);
    value.attribute.AttributeHeader.Flags = 0x80000000;
    ok &= check(&value, sizeof(value), STATUS_NOT_SUPPORTED);
    value = request(AUDIO_SIGNALPROCESSINGMODE_DEFAULT);
    for (ULONG size : {0UL, 23UL, 24UL, 39UL, 41UL, 0xFFFFFFFFUL})
    {
        value.attribute.AttributeHeader.Size = size;
        ok &= check(&value, sizeof(value), STATUS_INVALID_PARAMETER);
    }
    value = request(AUDIO_SIGNALPROCESSINGMODE_DEFAULT);
    value.items.Count = 0xFFFFFFFF;
    ok &= check(&value, sizeof(value), STATUS_INVALID_PARAMETER);
    value.items.Count = 0;
    ok &= check(&value, sizeof(value), STATUS_INVALID_PARAMETER);
    value.items.Count = 1;
    value.items.Size = 7;
    ok &= check(&value, sizeof(value), STATUS_INVALID_PARAMETER);
    value.items.Size = sizeof(value) + 1;
    ok &= check(&value, sizeof(value), STATUS_INVALID_PARAMETER);
    value = request(AUDIO_SIGNALPROCESSINGMODE_DEFAULT);
    std::vector<BYTE> extra(sizeof(value) + 8, 0xEF);
    std::memcpy(extra.data(), &value, sizeof(value));
    ok &= check(extra.data(), static_cast<ULONG>(extra.size()), STATUS_SUCCESS,
        AUDIO_SIGNALPROCESSINGMODE_DEFAULT, sizeof(value));
    // The same supported mode appearing twice is still an ambiguous request.
    std::vector<BYTE> duplicate(sizeof(value) + sizeof(value.attribute));
    value.items.Size = static_cast<ULONG>(duplicate.size()); value.items.Count = 2;
    std::memcpy(duplicate.data(), &value, sizeof(value));
    std::memcpy(duplicate.data() + sizeof(value), &value.attribute, sizeof(value.attribute));
    ok &= check(duplicate.data(), static_cast<ULONG>(duplicate.size()), STATUS_INVALID_PARAMETER);
    if (!ok) return 1;
    std::printf("KS attributes PASS: %u offline contract checks; no audio device opened.\n", checks);
    return 0;
}
