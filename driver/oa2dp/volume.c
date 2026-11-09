/* SPDX-License-Identifier: MIT */
/* Windows AVRCP remains the protocol owner. This control-only KS interface
 * supplies its A2DP volume cache on the actual Bluetooth service devnode.
 * The PortCls child uses the same cache through a referenced bus interface.
 * The cache GUID/id are a Windows 10 compatibility contract, not an AVRCP
 * wire protocol. No remote packets, keys or software PCM gain are emulated.
 */
#include <ntifs.h>
#include "oa2dp.h"
#include <windef.h>
#include <ks.h>
#include <ksmedia.h>
#include <devpropdef.h>

static const GUID VolumeCacheSet =
    {0x401e566b,0xb47e,0x4e34,{0xb0,0xfc,0x94,0x82,0x6e,0x9e,0x2a,0x1a}};
static const GUID AudioCategory =
    {0x6994ad04,0x93ef,0x11d0,{0xa3,0xcc,0x00,0xa0,0xc9,0x22,0x31,0x96}};
/* Observed category on the Windows 10 inbox A2DP filter. Settings discovers
 * Bluetooth-audio interfaces separately from ordinary audio/volume filters. */
static const GUID BluetoothAudioCategory =
    {0xe630310c,0x1733,0x42bb,{0x98,0x47,0x7e,0x45,0xd1,0xa2,0x74,0x90}};
static const GUID BluetoothAudioSet =
    {0x7fa06c40,0xb8f6,0x4c7e,{0x85,0x56,0xe8,0xc3,0x3a,0x12,0xe5,0x4d}};
static const GUID GeneralPropertyType = { STATIC_KSPROPTYPESETID_General };
static const DEVPROPKEY AvdtpConnectedKey =
    {{0x29ce83d4,0x7a82,0x4744,{0xbd,0x1d,0xab,0xec,0x85,0x32,0x1d,0xd6}},2};

/* Caller holds dev->Lock. TRUE requires the configured service's exact link. */
static BOOLEAN ConnectionReady(OA2DP_DEVICE_CONTEXT *dev, ULONG connection)
{
    return (BOOLEAN)Oa2dpConnectionReady(&dev->Channels[Oa2dpChannelSignaling].Life,
        &dev->Channels[Oa2dpChannelMedia].Life, connection,
        dev->PolicyConnected, dev->TransportStopping || dev->MpmStopping);
}

static BOOLEAN ConnectionClosed(OA2DP_DEVICE_CONTEXT *dev)
{
    return (BOOLEAN)Oa2dpConnectionClosed(&dev->Channels[Oa2dpChannelSignaling].Life,
        &dev->Channels[Oa2dpChannelMedia].Life);
}

/* PublicationLock serializes external calls; never hold the channel spin lock
 * across registry/MPM calls, which may block or call profile policy back. */
static NTSTATUS PublishState(OA2DP_DEVICE_CONTEXT *dev, BOOLEAN connected)
{
    NTSTATUS result = STATUS_SUCCESS;
    const DEVPROP_BOOLEAN value = connected ? DEVPROP_TRUE : DEVPROP_FALSE;
    for (ULONG i = 0; i < ARRAYSIZE(dev->AudioInterfacePaths); ++i) {
        if (dev->AudioInterfacePaths[i]) {
            UNICODE_STRING path;
            WdfStringGetUnicodeString(dev->AudioInterfacePaths[i], &path);
            NTSTATUS status = IoSetDeviceInterfacePropertyData(&path, &AvdtpConnectedKey,
                LOCALE_NEUTRAL, 0, DEVPROP_TYPE_BOOLEAN, sizeof(value), (PVOID)&value);
            if (!NT_SUCCESS(status)) {
                if (NT_SUCCESS(result)) result = status;
                KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                    "oa2dp: interface connection notification failed 0x%08X\n", status));
            }
        }
    }
    // Older candidates wrote the right key on the wrong object. Remove that
    // devnode-only value; the native contract belongs to the audio interface.
    (void)IoSetDevicePropertyData(WdfDeviceWdmGetPhysicalDevice(dev->Device),
        &AvdtpConnectedKey, LOCALE_NEUTRAL, 0, DEVPROP_TYPE_EMPTY, 0, NULL);
    NTSTATUS status = Oa2dpMpmConnected(dev, connected);
    if (!NT_SUCCESS(status)) {
        if (NT_SUCCESS(result)) result = status;
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
            "oa2dp: MPM connection notification failed 0x%08X\n", status));
    }
    return result;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS Oa2dpPublishConnected(OA2DP_DEVICE_CONTEXT *dev, BOOLEAN connected, ULONG connection)
{
    NTSTATUS status;
    BOOLEAN valid;
    NT_ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    WdfWaitLockAcquire(dev->PublicationLock, NULL);
    WdfSpinLockAcquire(dev->Lock);
    valid = connected ? ConnectionReady(dev, connection) : ConnectionClosed(dev);
    WdfSpinLockRelease(dev->Lock);
    if (!valid) {
        WdfWaitLockRelease(dev->PublicationLock);
        return connected ? STATUS_DEVICE_NOT_READY : STATUS_DEVICE_BUSY;
    }

    status = PublishState(dev, connected);
    WdfSpinLockAcquire(dev->Lock);
    valid = connected ? ConnectionReady(dev, connection) : ConnectionClosed(dev);
    if (connected && (!valid || !NT_SUCCESS(status))) {
        // A slow TRUE write can outlive its link. Remove it before allowing
        // a replacement TRUE publisher through this passive serialization.
        dev->PublishedConnection = 0;
        WdfSpinLockRelease(dev->Lock);
        NTSTATUS repair = PublishState(dev, FALSE);
        if (!NT_SUCCESS(repair)) status = repair;
        else if (NT_SUCCESS(status)) status = STATUS_DEVICE_NOT_READY;
        WdfSpinLockAcquire(dev->Lock);
    } else {
        dev->PublishedConnection = connected ? connection : 0;
        if (!valid && NT_SUCCESS(status)) status = STATUS_DEVICE_BUSY;
    }
    // Validation and event update share OPEN's lock. A new OPEN can never
    // clear the event immediately before an obsolete FALSE sets it again.
    if (ConnectionClosed(dev))
        KeSetEvent(&dev->DisconnectedEvent, IO_NO_INCREMENT, FALSE);
    else
        KeClearEvent(&dev->DisconnectedEvent);
    WdfSpinLockRelease(dev->Lock);
    WdfWaitLockRelease(dev->PublicationLock);
    return status;
}

static EVT_WDF_WORKITEM PublishWorkItem;
static VOID PublishWorkItem(WDFWORKITEM work)
{
    OA2DP_DEVICE_CONTEXT *dev = Oa2dpGetDevice((WDFDEVICE)WdfWorkItemGetParentObject(work));
    ULONG connection;
    WdfSpinLockAcquire(dev->Lock);
    connection = dev->PublishedConnection;
    WdfSpinLockRelease(dev->Lock);
    if (!NT_SUCCESS(Oa2dpPublishConnected(dev, TRUE, connection)))
        (void)Oa2dpPublishConnected(dev, FALSE, 0);
    Oa2dpMpmAcknowledgeSuspend(dev);
}

NTSTATUS Oa2dpInitializePublication(OA2DP_DEVICE_CONTEXT *dev)
{
    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_WORKITEM_CONFIG work;
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ParentObject = dev->Device;
    NTSTATUS status = WdfWaitLockCreate(&attrs, &dev->PublicationLock);
    if (!NT_SUCCESS(status)) return status;
    WDF_WORKITEM_CONFIG_INIT(&work, PublishWorkItem);
    work.AutomaticSerialization = FALSE;
    return WdfWorkItemCreate(&work, &attrs, &dev->PublicationWork);
}

static NTSTATUS BusSetConnected(PVOID context, BOOLEAN connected, ULONG connection)
{
    return Oa2dpPublishConnected(context, connected, connection);
}

static VOID BusReference(PVOID context) { WdfObjectReference(((OA2DP_DEVICE_CONTEXT *)context)->Device); }
static VOID BusDereference(PVOID context) { WdfObjectDereference(((OA2DP_DEVICE_CONTEXT *)context)->Device); }

static VOID BusGetVolume(PVOID context, PLONG db, PBOOLEAN muted)
{
    OA2DP_DEVICE_CONTEXT *dev = context;
    WdfSpinLockAcquire(dev->Lock);
    *db = dev->VolumeDb;
    *muted = dev->VolumeMuted;
    WdfSpinLockRelease(dev->Lock);
}

static VOID BusSetVolume(PVOID context, LONG db, BOOLEAN muted, BOOLEAN hostWrite)
{
    OA2DP_DEVICE_CONTEXT *dev = context;
    if (db < OA2DP_VOLUME_MIN_DB) db = OA2DP_VOLUME_MIN_DB;
    if (db > OA2DP_VOLUME_MAX_DB) db = OA2DP_VOLUME_MAX_DB;
    WdfSpinLockAcquire(dev->Lock);
    // A remote mute acknowledgement must not erase the level needed by a
    // later Windows unmute. Effective AVRCP cache reads still return silence.
    if (!hostWrite && muted && db == OA2DP_VOLUME_MIN_DB)
        db = dev->VolumeDb;
    if (dev->VolumeDb != db || dev->VolumeMuted != muted) {
        dev->VolumeDb = db;
        dev->VolumeMuted = muted;
        if (hostWrite) {
            PLIST_ENTRY entry;
            for (entry = dev->VolumeFiles.Flink; entry != &dev->VolumeFiles; entry = entry->Flink) {
                OA2DP_FILE_CONTEXT *file = CONTAINING_RECORD(entry, OA2DP_FILE_CONTEXT, Entry);
                if (file->VolumeEvent) KeSetEvent(file->VolumeEvent, IO_NO_INCREMENT, FALSE);
            }
        }
        // Registration/removal use this same lock. No callback can outlive
        // the topology object, and the callback itself must be nonpageable.
        if (dev->AudioVolumeNotify) dev->AudioVolumeNotify(dev->AudioContext);
    }
    WdfSpinLockRelease(dev->Lock);
}

static NTSTATUS BusRegisterAudio(PVOID context, PVOID audioContext,
                                OA2DP_VOLUME_NOTIFY notify, OA2DP_POLICY_REQUEST request)
{
    OA2DP_DEVICE_CONTEXT *dev = context;
    NTSTATUS status = STATUS_SUCCESS;
    WdfSpinLockAcquire(dev->Lock);
    if (dev->AudioContext && dev->AudioContext != audioContext) {
        status = STATUS_SHARING_VIOLATION;
    } else {
        dev->AudioContext = audioContext;
        dev->AudioVolumeNotify = notify;
        dev->AudioPolicyRequest = request;
        status = request(audioContext, dev->PolicyConnected, dev->PolicySuspended,
                          dev->MpmSuspendRequest.Serial);
        if (!NT_SUCCESS(status)) {
            dev->AudioContext = NULL;
            dev->AudioVolumeNotify = NULL;
            dev->AudioPolicyRequest = NULL;
        }
    }
    WdfSpinLockRelease(dev->Lock);
    return status;
}

static VOID BusUnregisterAudio(PVOID context, PVOID audioContext)
{
    OA2DP_DEVICE_CONTEXT *dev = context;
    WdfSpinLockAcquire(dev->Lock);
    if (dev->AudioContext == audioContext) {
        dev->AudioContext = NULL;
        dev->AudioVolumeNotify = NULL;
        dev->AudioPolicyRequest = NULL;
    }
    WdfSpinLockRelease(dev->Lock);
}

NTSTATUS Oa2dpRegisterBusInterface(OA2DP_DEVICE_CONTEXT *dev, WDFDEVICE child)
{
    OA2DP_BUS_INTERFACE bus;
    WDF_QUERY_INTERFACE_CONFIG config;
    RtlZeroMemory(&bus, sizeof(bus));
    bus.Header.Size = sizeof(bus);
    bus.Header.Version = OA2DP_BUS_VERSION;
    bus.Header.Context = dev;
    bus.Header.InterfaceReference = BusReference;
    bus.Header.InterfaceDereference = BusDereference;
    bus.GetVolume = BusGetVolume;
    bus.SetVolume = BusSetVolume;
    bus.SetConnected = BusSetConnected;
    bus.DisconnectedEvent = &dev->DisconnectedEvent;
    bus.RequestConnection = Oa2dpMpmRequestConnection;
    bus.SetPlaying = Oa2dpMpmPlaying;
    bus.SetSuspended = Oa2dpMpmSuspended;
    bus.RegisterAudio = BusRegisterAudio;
    bus.UnregisterAudio = BusUnregisterAudio;
    WDF_QUERY_INTERFACE_CONFIG_INIT(&config, (PINTERFACE)&bus, &GUID_OA2DP_BUS_INTERFACE, NULL);
    return WdfDeviceAddQueryInterface(child, &config);
}

NTSTATUS Oa2dpRegisterVolumeInterface(OA2DP_DEVICE_CONTEXT *dev)
{
    DECLARE_CONST_UNICODE_STRING(reference, L"AvrcpVolume");
    const GUID *categories[] = {&AudioCategory, &BluetoothAudioCategory};
    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ParentObject = dev->Device;
    for (ULONG i = 0; i < ARRAYSIZE(categories); ++i) {
        NTSTATUS status = WdfDeviceCreateDeviceInterface(dev->Device, categories[i], &reference);
        if (NT_SUCCESS(status))
            status = WdfStringCreate(NULL, &attrs, &dev->AudioInterfacePaths[i]);
        if (NT_SUCCESS(status))
            status = WdfDeviceRetrieveDeviceInterfaceString(dev->Device, categories[i],
                &reference, dev->AudioInterfacePaths[i]);
        if (!NT_SUCCESS(status)) return status;
    }
    return STATUS_SUCCESS;
}

static BOOLEAN PrivilegedControlCaller(WDFREQUEST request)
{
    PIRP irp = WdfRequestWdmGetIrp(request);
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    PIO_SECURITY_CONTEXT security = stack->Parameters.Create.SecurityContext;
    BOOLEAN allowed = FALSE;
    if (irp->RequestorMode == KernelMode) return TRUE;
    if (!security || !security->AccessState) return FALSE;
    PSECURITY_SUBJECT_CONTEXT subject = &security->AccessState->SubjectSecurityContext;
    SeLockSubjectContext(subject);
    PACCESS_TOKEN token = SeQuerySubjectContextToken(subject);
    if (token && SeTokenIsAdmin(token)) {
        allowed = TRUE;
    } else if (token) {
        PTOKEN_USER user = NULL;
        if (NT_SUCCESS(SeQueryInformationToken(token, TokenUser, (PVOID *)&user)) && user) {
            allowed = RtlEqualSid(user->User.Sid, SeExports->SeLocalSystemSid);
            ExFreePool(user);
        }
    }
    SeUnlockSubjectContext(subject);
    return allowed;
}

VOID Oa2dpEvtFileCreate(WDFDEVICE device, WDFREQUEST request, WDFFILEOBJECT fileObject)
{
    OA2DP_DEVICE_CONTEXT *dev = Oa2dpGetDevice(device);
    OA2DP_FILE_CONTEXT *file = Oa2dpGetFile(fileObject);
    PUNICODE_STRING name = WdfFileObjectGetFileName(fileObject);
    DECLARE_CONST_UNICODE_STRING(volumeName, L"\\AvrcpVolume");
    DECLARE_CONST_UNICODE_STRING(rootName, L"\\");
    NTSTATUS status = STATUS_SUCCESS;
    RtlZeroMemory(file, sizeof(*file));
    file->VolumeClient = name && RtlEqualUnicodeString(name, &volumeName, TRUE);
    if (!file->VolumeClient && name && name->Length && !RtlEqualUnicodeString(name, &rootName, TRUE))
        status = STATUS_OBJECT_NAME_NOT_FOUND;
    if (NT_SUCCESS(status) && !file->VolumeClient && !PrivilegedControlCaller(request))
        status = STATUS_ACCESS_DENIED;
    if (NT_SUCCESS(status)) {
        WdfSpinLockAcquire(dev->Lock);
        if (file->VolumeClient) {
            if (dev->VolumeFileCount >= 8) {
                status = STATUS_INSUFFICIENT_RESOURCES;
            } else {
                InsertTailList(&dev->VolumeFiles, &file->Entry);
                ++dev->VolumeFileCount;
                file->Listed = TRUE;
            }
        } else if (dev->ServiceFile) {
            status = STATUS_SHARING_VIOLATION;
        } else {
            dev->ServiceFile = fileObject;
            file->ControlClient = TRUE;
        }
        WdfSpinLockRelease(dev->Lock);
    }
    WdfRequestComplete(request, status);
}

VOID Oa2dpVolumeFileCleanup(OA2DP_DEVICE_CONTEXT *dev, WDFFILEOBJECT fileObject)
{
    OA2DP_FILE_CONTEXT *file = Oa2dpGetFile(fileObject);
    PKEVENT event;
    WdfSpinLockAcquire(dev->Lock);
    event = file->VolumeEvent;
    file->Closing = TRUE;
    file->VolumeEvent = NULL;
    if (file->Listed) {
        RemoveEntryList(&file->Entry);
        --dev->VolumeFileCount;
        file->Listed = FALSE;
    }
    WdfSpinLockRelease(dev->Lock);
    if (event) ObDereferenceObject(event);
}

/* These three KS IOCTLs use METHOD_NEITHER. Run in the caller's original
 * thread before KMDF queues the request; copy only bounded known structures,
 * probe user addresses and retain a referenced event object, never a user VA.
 */
static NTSTATUS ReadCaller(PIRP irp, PVOID destination, const VOID *source, SIZE_T bytes)
{
    if (!source) return STATUS_INVALID_USER_BUFFER;
    __try {
        if (irp->RequestorMode != KernelMode) ProbeForRead(source, bytes, 1);
        RtlCopyMemory(destination, source, bytes);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
    return STATUS_SUCCESS;
}

static NTSTATUS WriteCaller(PIRP irp, PVOID destination, const VOID *source, SIZE_T bytes)
{
    if (!destination) return STATUS_INVALID_USER_BUFFER;
    __try {
        if (irp->RequestorMode != KernelMode) ProbeForWrite(destination, bytes, 1);
        RtlCopyMemory(destination, source, bytes);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
    return STATUS_SUCCESS;
}

static NTSTATUS WriteSupport(PIRP irp, ULONG capacity, ULONG flags, ULONG type,
                             ULONG_PTR *returned)
{
    NTSTATUS status;
    if (capacity < sizeof(ULONG)) return STATUS_BUFFER_TOO_SMALL;
    if (capacity < sizeof(KSPROPERTY_DESCRIPTION)) {
        status = WriteCaller(irp, irp->UserBuffer, &flags, sizeof(flags));
        if (NT_SUCCESS(status)) *returned = sizeof(flags);
    } else {
        KSPROPERTY_DESCRIPTION description;
        RtlZeroMemory(&description, sizeof(description));
        description.AccessFlags = flags;
        description.DescriptionSize = sizeof(description);
        description.PropTypeSet.Set = GeneralPropertyType;
        description.PropTypeSet.Id = type;
        status = WriteCaller(irp, irp->UserBuffer, &description, sizeof(description));
        if (NT_SUCCESS(status)) *returned = sizeof(description);
    }
    return status;
}

NTSTATUS Oa2dpVolumePreprocess(WDFDEVICE device, PIRP irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    WDFFILEOBJECT fileObject = stack->FileObject ? WdfDeviceGetFileObject(device, stack->FileObject) : NULL;
    OA2DP_FILE_CONTEXT *file = fileObject ? Oa2dpGetFile(fileObject) : NULL;
    if (!file || !file->VolumeClient) {
        IoSkipCurrentIrpStackLocation(irp);
        return WdfDeviceWdmDispatchPreprocessedIrp(device, irp);
    }
    OA2DP_DEVICE_CONTEXT *dev = Oa2dpGetDevice(device);
    const ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    const ULONG inputBytes = stack->Parameters.DeviceIoControl.InputBufferLength;
    const ULONG outputBytes = stack->Parameters.DeviceIoControl.OutputBufferLength;
    const PVOID input = stack->Parameters.DeviceIoControl.Type3InputBuffer;
    KSPROPERTY property;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR returned = 0;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || IoIs32bitProcess(irp)) {
        status = STATUS_NOT_SUPPORTED;
        goto done;
    }
    if (code == IOCTL_KS_DISABLE_EVENT) {
        PKEVENT old;
        WdfSpinLockAcquire(dev->Lock);
        old = file->VolumeEvent;
        file->VolumeEvent = NULL;
        WdfSpinLockRelease(dev->Lock);
        if (old) ObDereferenceObject(old);
        status = STATUS_SUCCESS;
        goto done;
    }
    if ((code != IOCTL_KS_PROPERTY && code != IOCTL_KS_ENABLE_EVENT) || inputBytes != sizeof(property))
        goto done;
    status = ReadCaller(irp, &property, input, sizeof(property));
    if (!NT_SUCCESS(status)) goto done;
    if (code == IOCTL_KS_PROPERTY && IsEqualGUID(&property.Set, &BluetoothAudioSet) &&
        property.Id <= KSPROPERTY_ONESHOT_DISCONNECT) {
        if (property.Flags == KSPROPERTY_TYPE_BASICSUPPORT) {
            ULONG flags = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT;
            status = WriteSupport(irp, outputBytes, flags, 0xffff, &returned);
        } else if (property.Flags == KSPROPERTY_TYPE_GET) {
            status = Oa2dpMpmRequestConnection(dev,
                property.Id == KSPROPERTY_ONESHOT_RECONNECT);
            if (NT_SUCCESS(status) && property.Id == KSPROPERTY_ONESHOT_DISCONNECT) {
                // Windows' one-shot disconnect waits for the real channel close.
                // The callback above only queues the command and cannot wait
                // while the parent spin lock protects its topology registration.
                LARGE_INTEGER timeout;
                timeout.QuadPart = -10LL * 1000 * 1000 * 10;
                status = KeWaitForSingleObject(&dev->DisconnectedEvent, Executive,
                    KernelMode, FALSE, &timeout);
                if (status == STATUS_TIMEOUT) status = STATUS_IO_TIMEOUT;
            }
        } else {
            status = STATUS_INVALID_PARAMETER;
        }
        goto done;
    }
    if (!IsEqualGUID(&property.Set, &VolumeCacheSet) || property.Id != 1) {
        status = STATUS_NOT_SUPPORTED;
        goto done;
    }
    if (code == IOCTL_KS_ENABLE_EVENT) {
        KSEVENTDATA data;
        PKEVENT event = NULL, old;
        if (property.Flags != KSEVENT_TYPE_ENABLE || outputBytes != sizeof(data)) {
            status = STATUS_INVALID_PARAMETER;
            goto done;
        }
        status = ReadCaller(irp, &data, irp->UserBuffer, sizeof(data));
        if (!NT_SUCCESS(status)) goto done;
        if (data.NotificationType != KSEVENTF_EVENT_HANDLE) {
            status = STATUS_NOT_SUPPORTED;
            goto done;
        }
        status = ObReferenceObjectByHandle(data.EventHandle.Event, EVENT_MODIFY_STATE,
            *ExEventObjectType, irp->RequestorMode, (PVOID *)&event, NULL);
        if (!NT_SUCCESS(status)) goto done;
        WdfSpinLockAcquire(dev->Lock);
        old = NULL;
        if (file->Closing) {
            status = STATUS_FILE_CLOSED;
        } else {
            old = file->VolumeEvent;
            file->VolumeEvent = event;
            event = NULL;
        }
        WdfSpinLockRelease(dev->Lock);
        if (old) ObDereferenceObject(old);
        if (event) ObDereferenceObject(event);
        goto done;
    }
    if (property.Flags == KSPROPERTY_TYPE_BASICSUPPORT) {
        ULONG flags = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT;
        status = WriteSupport(irp, outputBytes, flags, 3 /* VT_I4 */, &returned);
    } else if (property.Flags == KSPROPERTY_TYPE_GET) {
        LONG db;
        BOOLEAN muted;
        if (outputBytes < sizeof(db)) { status = STATUS_BUFFER_TOO_SMALL; goto done; }
        BusGetVolume(dev, &db, &muted);
        if (muted) db = OA2DP_VOLUME_MIN_DB;
        status = WriteCaller(irp, irp->UserBuffer, &db, sizeof(db));
        if (NT_SUCCESS(status)) returned = sizeof(db);
    } else if (property.Flags == KSPROPERTY_TYPE_SET) {
        LONG db;
        if (outputBytes != sizeof(db)) { status = STATUS_BUFFER_TOO_SMALL; goto done; }
        status = ReadCaller(irp, &db, irp->UserBuffer, sizeof(db));
        if (NT_SUCCESS(status)) BusSetVolume(dev, db, db <= OA2DP_VOLUME_MIN_DB, FALSE);
    } else {
        status = STATUS_INVALID_DEVICE_REQUEST;
    }
done:
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = returned;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}
