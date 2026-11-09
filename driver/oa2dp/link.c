/* SPDX-License-Identifier: MIT */
/* Public HCI notifications wake the existing native session on ACL arrival.
 * They are hints only; channel generations remain the source of link truth. */
#include "oa2dp.h"
#include "../common/radio_events.h"

C_ASSERT(HCI_CONNECTION_TYPE_ACL == OA2DP_HCI_ACL);
C_ASSERT(FIELD_OFFSET(BTH_HCI_EVENT_INFO, connectionType) == sizeof(BTH_ADDR));
C_ASSERT(FIELD_OFFSET(BTH_HCI_EVENT_INFO, connected) == sizeof(BTH_ADDR) + 1);

/* Caller frees the bounded, terminated nonpaged buffer with OA2DP_POOL_TAG. */
static NTSTATUS QueryRadioPath(OA2DP_DEVICE_CONTEXT *dev, PWSTR *path)
{
    ULONG bytes, copied;
    NTSTATUS status;
    struct _BRB_GET_DEVICE_INTERFACE_STRING *brb;
    *path = NULL;
    brb = (struct _BRB_GET_DEVICE_INTERFACE_STRING *)dev->Profile.BthAllocateBrb(
        BRB_GET_DEVICE_INTERFACE_STRING, OA2DP_POOL_TAG);
    if (!brb) return STATUS_INSUFFICIENT_RESOURCES;
    brb->DeviceInterfaceString = NULL;
    brb->DeviceInterfaceStringCbLength = 0;
    status = Oa2dpSubmitBrbSync(dev, (PBRB)brb, sizeof(*brb));
    bytes = brb->DeviceInterfaceStringCbLength;
    dev->Profile.BthFreeBrb((PBRB)brb);
    // MORE_ENTRIES is informational (NT_SUCCESS), but contains no path yet.
    if (status != STATUS_SUCCESS && status != STATUS_MORE_ENTRIES &&
        status != STATUS_BUFFER_TOO_SMALL && status != STATUS_BUFFER_OVERFLOW)
        return status;
    if (bytes < sizeof(WCHAR) || bytes > MAXUSHORT - sizeof(WCHAR) || bytes % sizeof(WCHAR))
        return STATUS_INVALID_BUFFER_SIZE;
    PWSTR buffer = ExAllocatePoolWithTag(NonPagedPoolNx, bytes + sizeof(WCHAR), OA2DP_POOL_TAG);
    if (!buffer) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(buffer, bytes + sizeof(WCHAR));
    brb = (struct _BRB_GET_DEVICE_INTERFACE_STRING *)dev->Profile.BthAllocateBrb(
        BRB_GET_DEVICE_INTERFACE_STRING, OA2DP_POOL_TAG);
    if (!brb) {
        ExFreePoolWithTag(buffer, OA2DP_POOL_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    brb->DeviceInterfaceString = buffer;
    brb->DeviceInterfaceStringCbLength = bytes;
    status = Oa2dpSubmitBrbSync(dev, (PBRB)brb, sizeof(*brb));
    copied = brb->DeviceInterfaceStringCbLength;
    dev->Profile.BthFreeBrb((PBRB)brb);
    if (status == STATUS_SUCCESS && copied && copied <= bytes && !(copied % sizeof(WCHAR)) && buffer[0]) {
        buffer[copied / sizeof(WCHAR)] = L'\0';
        *path = buffer;
        return STATUS_SUCCESS;
    }
    ExFreePoolWithTag(buffer, OA2DP_POOL_TAG);
    return NT_SUCCESS(status) ? STATUS_INVALID_BUFFER_SIZE : status;
}

static NTSTATUS RadioChanged(PVOID notification, PVOID context)
{
    TARGET_DEVICE_CUSTOM_NOTIFICATION *event = notification;
    OA2DP_DEVICE_CONTEXT *dev = context;
    const SIZE_T offset = FIELD_OFFSET(TARGET_DEVICE_CUSTOM_NOTIFICATION, CustomDataBuffer);
    if (!event || event->Version != 1 || event->Size < offset ||
        !IsEqualGUID(&event->Event, &GUID_BLUETOOTH_HCI_EVENT) ||
        !Oa2dpAclConnectHint(event->CustomDataBuffer, event->Size - offset, dev->RemoteAddress))
        return STATUS_SUCCESS;
    WdfSpinLockAcquire(dev->Lock);
    const BOOLEAN deliver = !dev->LinkNotificationsStopping && !dev->SurpriseRemoved;
    if (deliver) Oa2dpQueueEventLocked(dev, Oa2dpEventLinkAvailable, NULL, 0);
    WdfSpinLockRelease(dev->Lock);
    if (deliver) Oa2dpDeliverEvents(dev);
    return STATUS_SUCCESS;
}

NTSTATUS Oa2dpRegisterLinkNotifications(OA2DP_DEVICE_CONTEXT *dev)
{
    PWSTR path = NULL;
    PFILE_OBJECT file = NULL;
    PDEVICE_OBJECT target = NULL;
    UNICODE_STRING name;
    PVOID entry = NULL;
    NTSTATUS status;
    PAGED_CODE();
    WdfSpinLockAcquire(dev->Lock);
    const BOOLEAN existing = dev->LinkNotifications != NULL;
    const BOOLEAN stopping = dev->LinkNotificationsStopping;
    WdfSpinLockRelease(dev->Lock);
    if (existing) return stopping ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
    status = QueryRadioPath(dev, &path);
    if (!NT_SUCCESS(status)) return status;
    RtlInitUnicodeString(&name, path);
    status = IoGetDeviceObjectPointer(&name, FILE_READ_DATA, &file, &target);
    ExFreePoolWithTag(path, OA2DP_POOL_TAG);
    if (!NT_SUCCESS(status)) return status;
    WdfSpinLockAcquire(dev->Lock);
    dev->LinkNotificationsStopping = FALSE;
    WdfSpinLockRelease(dev->Lock);
    status = IoRegisterPlugPlayNotification(EventCategoryTargetDeviceChange, 0, file,
        WdfDriverWdmGetDriverObject(WdfDeviceGetDriver(dev->Device)),
        RadioChanged, dev, &entry);
    ObDereferenceObject(file);
    if (NT_SUCCESS(status)) {
        WdfSpinLockAcquire(dev->Lock);
        dev->LinkNotifications = entry;
        WdfSpinLockRelease(dev->Lock);
    }
    return status;
}

NTSTATUS Oa2dpUnregisterLinkNotifications(OA2DP_DEVICE_CONTEXT *dev)
{
    PVOID entry;
    PAGED_CODE();
    WdfSpinLockAcquire(dev->Lock);
    dev->LinkNotificationsStopping = TRUE;
    entry = dev->LinkNotifications;
    WdfSpinLockRelease(dev->Lock);
    if (!entry) return STATUS_SUCCESS;
    // Ex guarantees no callback after return. Never wait under dev->Lock.
    NTSTATUS status = IoUnregisterPlugPlayNotificationEx(entry);
    if (NT_SUCCESS(status)) {
        WdfSpinLockAcquire(dev->Lock);
        dev->LinkNotifications = NULL;
        WdfSpinLockRelease(dev->Lock);
    }
    return status;
}
