/* SPDX-License-Identifier: MIT */
#include "oa2dp.h"
#include <devpropdef.h>
#include "trace.h"

TRACELOGGING_DEFINE_PROVIDER(g_Oa2dpTransportTrace, "XM6Audio.Transport",
    (0xbd2386c3, 0x72bc, 0x4f90, 0x82, 0x92, 0x24, 0x76, 0x26, 0x36, 0x2c, 0xb2));

static EVT_WDF_DRIVER_UNLOAD Oa2dpEvtDriverUnload;
static EVT_WDFDEVICE_WDM_IRP_PREPROCESS Oa2dpChildEnumerated;

static VOID Oa2dpEvtDriverUnload(WDFDRIVER driver)
{
    UNREFERENCED_PARAMETER(driver);
    TraceLoggingUnregister(g_Oa2dpTransportTrace);
}

/* Windows resolves an audio endpoint's profile through its own devnode,
 * not just the shared headset container. Keep the render PDO's identity
 * consistent with the Bluetooth service that this transport implements. */
static const DEVPROPKEY BluetoothAddressKey =
    {{0x2bd67d8b,0x8beb,0x48d5,{0x87,0xe0,0x6c,0xda,0x34,0x28,0x04,0x0a}},1};
static const DEVPROPKEY BluetoothServiceKey =
    {{0x2bd67d8b,0x8beb,0x48d5,{0x87,0xe0,0x6c,0xda,0x34,0x28,0x04,0x0a}},2};
static const GUID AudioSinkService =
    {0x0000110b,0x0000,0x1000,{0x80,0x00,0x00,0x80,0x5f,0x9b,0x34,0xfb}};
static const GUID BluetoothBusType =
    {0x4e72db9b,0xd9a3,0x4f6d,{0x9f,0x9d,0xb3,0xb4,0xed,0x94,0x1a,0x3f}};

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, Oa2dpEvtDeviceAdd)
#pragma alloc_text(PAGE, Oa2dpEvtPrepareHardware)
#pragma alloc_text(PAGE, Oa2dpEvtReleaseHardware)
#endif

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;
    WDF_DRIVER_CONFIG_INIT(&config, Oa2dpEvtDeviceAdd);
    config.DriverPoolTag = OA2DP_POOL_TAG;
    config.EvtDriverUnload = Oa2dpEvtDriverUnload;
    // A failed provider registration must not prevent audio from loading.
    (void)TraceLoggingRegister(g_Oa2dpTransportTrace);
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config,
                            WDF_NO_HANDLE);
    // DriverEntry failure does not invoke the framework unload callback.
    if (!NT_SUCCESS(status)) TraceLoggingUnregister(g_Oa2dpTransportTrace);
    return status;
}

NTSTATUS Oa2dpEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS status;
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_IO_QUEUE_CONFIG qcfg;
    WDF_FILEOBJECT_CONFIG files;
    OA2DP_DEVICE_CONTEXT *dev;
    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = Oa2dpEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = Oa2dpEvtReleaseHardware;
    pnp.EvtDeviceSelfManagedIoSuspend = Oa2dpEvtIoSuspend;
    pnp.EvtDeviceSelfManagedIoInit = Oa2dpEvtIoInit;
    pnp.EvtDeviceSelfManagedIoCleanup = Oa2dpEvtIoCleanup;
    pnp.EvtDeviceSelfManagedIoRestart = Oa2dpEvtIoRestart;
    pnp.EvtDeviceSurpriseRemoval = Oa2dpEvtSurpriseRemoval;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);
    WDF_FILEOBJECT_CONFIG_INIT(&files, Oa2dpEvtFileCreate, WDF_NO_EVENT_CALLBACK,
                               Oa2dpEvtFileCleanup);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, OA2DP_FILE_CONTEXT);
    attrs.ExecutionLevel = WdfExecutionLevelPassive;
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &files, &attrs);
    WdfDeviceInitSetExclusive(DeviceInit, FALSE); // one control owner + native AVRCP volume clients
    status = WdfDeviceInitAssignWdmIrpPreprocessCallback(DeviceInit, Oa2dpVolumePreprocess,
                                                       IRP_MJ_DEVICE_CONTROL, NULL, 0);
    if (!NT_SUCCESS(status))
        return status;

    /* The INF supplies SYSTEM/admin access for this PnP stack. Assigning
     * SDDL to this unnamed FDO makes WdfDeviceCreate fail with
     * STATUS_INVALID_SECURITY_DESCR. Enforce the stack ACL on child opens. */
    WdfDeviceInitSetCharacteristics(DeviceInit, FILE_DEVICE_SECURE_OPEN, TRUE);

    /* Every request carries a context for its BRB. */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, OA2DP_REQUEST_CONTEXT);
    WdfDeviceInitSetRequestAttributes(DeviceInit, &attrs);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, OA2DP_DEVICE_CONTEXT);
    attrs.EvtDestroyCallback = Oa2dpEvtDeviceDestroy;
    status = WdfDeviceCreate(&DeviceInit, &attrs, &device);
    if (!NT_SUCCESS(status))
        return status;

    dev = Oa2dpGetDevice(device);
    RtlZeroMemory(dev, sizeof(*dev));
    dev->Device = device;
    dev->IoTarget = WdfDeviceGetIoTarget(device);
    {
        PNP_BUS_INFORMATION bus;
        RtlZeroMemory(&bus, sizeof(bus));
        bus.BusTypeGuid = BluetoothBusType;
        bus.LegacyBusType = PNPBus;
        WdfDeviceSetBusInformationForChildren(device, &bus);
    }
    InitializeListHead(&dev->VolumeFiles);
    KeInitializeEvent(&dev->DisconnectedEvent, NotificationEvent, TRUE);
    KeInitializeEvent(&dev->ResponsesIdle, NotificationEvent, TRUE);
    dev->VolumeDb = -30L * 65536L;
    for (ULONG i = 0; i < Oa2dpChannelCount; i++) {
        dev->Channels[i].Dev = dev;
        dev->Channels[i].Kind = (OA2DP_CHANNEL_KIND)i;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ParentObject = device;
    status = WdfSpinLockCreate(&attrs, &dev->Lock);
    if (!NT_SUCCESS(status))
        return status;
    status = Oa2dpInitializePublication(dev);
    if (!NT_SUCCESS(status))
        return status;
    status = Oa2dpInitializeChannels(dev);
    if (!NT_SUCCESS(status))
        return status;

    /* Control/event queues stay available while self-managed transport I/O is
     * suspended. A long idle RECV/WAIT_EVENT must not pin a power-managed queue
     * in D0. The self-managed callbacks close/gate the actual Bluetooth I/O. */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qcfg, WdfIoQueueDispatchParallel);
    qcfg.PowerManaged = WdfFalse;
    qcfg.EvtIoDeviceControl = Oa2dpEvtIoDeviceControl;
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfIoQueueCreate(device, &qcfg, &attrs, &dev->DefaultQueue);
    if (!NT_SUCCESS(status))
        return status;

    WDF_IO_QUEUE_CONFIG_INIT(&qcfg, WdfIoQueueDispatchManual);
    qcfg.PowerManaged = WdfFalse;
    status = WdfIoQueueCreate(device, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, &dev->EventQueue);
    if (!NT_SUCCESS(status))
        return status;

    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_OA2DP_TRANSPORT, NULL);
    return NT_SUCCESS(status) ? Oa2dpRegisterVolumeInterface(dev) : status;
}

NTSTATUS Oa2dpEvtPrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Raw,
                                 _In_ WDFCMRESLIST Translated)
{
    OA2DP_DEVICE_CONTEXT *dev = Oa2dpGetDevice(Device);
    NTSTATUS status;
    UNREFERENCED_PARAMETER(Raw);
    UNREFERENCED_PARAMETER(Translated);
    PAGED_CODE();

    WdfSpinLockAcquire(dev->Lock);
    dev->TransportStopping = TRUE; // no admission until the whole prepare transaction succeeds
    dev->PowerSuspended = FALSE;
    dev->PowerQuiesced = FALSE;
    dev->SurpriseRemoved = FALSE;
    WdfSpinLockRelease(dev->Lock);
    // BTH's BRB allocation table is device-owned, not an individual hardware
    // resource. Reuse this referenced import across prepare/rebalance cycles;
    // replacing it would leak an import and could race a late BRB completion.
    if (!dev->ProfileAcquired) {
        status = WdfFdoQueryForInterface(Device, &GUID_BTHDDI_PROFILE_DRIVER_INTERFACE,
                                         (PINTERFACE)&dev->Profile, sizeof(dev->Profile),
                                         BTHDDI_PROFILE_DRIVER_INTERFACE_VERSION_FOR_QI, NULL);
        if (!NT_SUCCESS(status)) return status;
        dev->ProfileAcquired = TRUE;
    }
    if (!dev->Profile.BthAllocateBrb || !dev->Profile.BthFreeBrb)
        return STATUS_INVALID_DEVICE_STATE;

    status = Oa2dpQueryLocalAddress(dev);
    if (!NT_SUCCESS(status))
        return status;
    status = Oa2dpQueryRemoteAddress(dev);
    if (!NT_SUCCESS(status))
        return status;
    // Resource rebalance/restart can follow a failed cleanup. Finish old
    // ownership first; never reset rundown state or duplicate registration.
    Oa2dpMpmStop(dev);
    status = Oa2dpUnregisterServer(dev);
    if (!NT_SUCCESS(status)) return status;
    Oa2dpCloseAllChannels(dev);
    WdfSpinLockAcquire(dev->Lock);
    const BOOLEAN closed = (BOOLEAN)Oa2dpConnectionClosed(
        &dev->Channels[Oa2dpChannelSignaling].Life, &dev->Channels[Oa2dpChannelMedia].Life);
    WdfSpinLockRelease(dev->Lock);
    if (!closed) return STATUS_DEVICE_BUSY;
    WdfSpinLockAcquire(dev->Lock);
    // ReleaseHardware may have queued DeviceGone after the old waiter was
    // canceled. That event belongs to the previous PnP lifetime, never to a
    // replacement service session after resume/radio restart.
    dev->EventHead = dev->EventCount = 0;
    dev->EventOverflow = FALSE;
    WdfSpinLockRelease(dev->Lock);
    Oa2dpPublishConnected(dev, FALSE, 0);

    status = Oa2dpMpmStart(dev);
    TraceLoggingWrite(g_Oa2dpTransportTrace, "PrepareStepResult",
        TraceLoggingUInt32(1, "Step"), TraceLoggingNTStatus(status, "Status"));
    if (!NT_SUCCESS(status))
        return status;
    /* Establish policy before allowing peer-initiated channels. */
    status = Oa2dpRegisterServer(dev);
    TraceLoggingWrite(g_Oa2dpTransportTrace, "PrepareStepResult",
        TraceLoggingUInt32(2, "Step"), TraceLoggingNTStatus(status, "Status"));
    if (NT_SUCCESS(status)) {
        status = Oa2dpCreateAudioChild(dev);
        TraceLoggingWrite(g_Oa2dpTransportTrace, "PrepareStepResult",
            TraceLoggingUInt32(3, "Step"), TraceLoggingNTStatus(status, "Status"));
    }
    if (!NT_SUCCESS(status)) {
        (void)Oa2dpUnregisterServer(dev);
        Oa2dpMpmStop(dev);
        return status;
    }
    WdfSpinLockAcquire(dev->Lock);
    dev->TransportStopping = FALSE;
    WdfSpinLockRelease(dev->Lock);
    return status;
}

/* Self-managed suspend runs before WDF stops power-managed queues/D0 exit.
 * On ordinary sleep the lower stack is still usable for bounded teardown.
 * Keep policy and the render PDO: sleep is neither user disconnect nor remove. */
NTSTATUS Oa2dpEvtIoInit(WDFDEVICE Device)
{
    NTSTATUS status = Oa2dpRegisterLinkNotifications(Oa2dpGetDevice(Device));
    if (!NT_SUCCESS(status))
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
            "oa2dp: ACL arrival notifications unavailable 0x%08X\n", status));
    // Notification acceleration is optional; the existing session's bounded
    // connection retry still works when a radio lacks the notification path.
    return STATUS_SUCCESS;
}

VOID Oa2dpEvtIoCleanup(WDFDEVICE Device)
{
    NTSTATUS status = Oa2dpUnregisterLinkNotifications(Oa2dpGetDevice(Device));
    if (!NT_SUCCESS(status))
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "oa2dp: final ACL notification cleanup failed 0x%08X\n", status));
}

NTSTATUS Oa2dpEvtIoSuspend(WDFDEVICE Device)
{
    OA2DP_DEVICE_CONTEXT *dev = Oa2dpGetDevice(Device);
    BOOLEAN removed;
    WdfSpinLockAcquire(dev->Lock);
    dev->TransportStopping = TRUE;
    dev->PowerSuspended = TRUE;
    if (++dev->PowerSerial == 0) ++dev->PowerSerial;
    removed = dev->SurpriseRemoved;
    Oa2dpQueueEventLocked(dev, removed ? Oa2dpEventDeviceGone : Oa2dpEventPowerChanged, NULL, 0);
    WdfSpinLockRelease(dev->Lock);
    Oa2dpDeliverEvents(dev);
    if (!removed) {
        Oa2dpCloseAllChannels(dev);
        for (ULONG i = 0; i < Oa2dpChannelCount; ++i)
            WdfWorkItemFlush(dev->Channels[i].CloseWork);
        NTSTATUS status = Oa2dpPublishConnected(dev, FALSE, 0);
        if (!NT_SUCCESS(status))
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                "oa2dp: suspend teardown remains incomplete 0x%08X\n", status));
    }
    // A racing control close/worker must also leave its BRB submission before
    // D0 exit. Gate later cleanup IOCTLs while the lower stack is unavailable.
    (void)KeWaitForSingleObject(&dev->ResponsesIdle, Executive, KernelMode, FALSE, NULL);
    WdfWaitLockAcquire(dev->Channels[Oa2dpChannelMedia].CloseLock, NULL);
    WdfWaitLockAcquire(dev->Channels[Oa2dpChannelSignaling].CloseLock, NULL);
    WdfSpinLockAcquire(dev->Lock);
    dev->PowerQuiesced = TRUE;
    WdfSpinLockRelease(dev->Lock);
    WdfWaitLockRelease(dev->Channels[Oa2dpChannelSignaling].CloseLock);
    WdfWaitLockRelease(dev->Channels[Oa2dpChannelMedia].CloseLock);
    // Failed CLOSE retains its owner for retry after resume. Admission is
    // already gated; do not remove the whole device for a transient close error.
    return STATUS_SUCCESS;
}

NTSTATUS Oa2dpEvtIoRestart(WDFDEVICE Device)
{
    OA2DP_DEVICE_CONTEXT *dev = Oa2dpGetDevice(Device);
    WdfSpinLockAcquire(dev->Lock);
    if (dev->SurpriseRemoved) {
        WdfSpinLockRelease(dev->Lock);
        return STATUS_DELETE_PENDING;
    }
    dev->PowerSuspended = FALSE;
    dev->PowerQuiesced = FALSE;
    dev->TransportStopping = FALSE;
    if (++dev->PowerSerial == 0) ++dev->PowerSerial;
    Oa2dpQueueEventLocked(dev, Oa2dpEventPowerChanged, NULL, 0);
    WdfSpinLockRelease(dev->Lock);
    Oa2dpDeliverEvents(dev);
    (void)Oa2dpEvtIoInit(Device); // retry a missing notification registration once on resume
    return STATUS_SUCCESS;
}

VOID Oa2dpEvtSurpriseRemoval(WDFDEVICE Device)
{
    OA2DP_DEVICE_CONTEXT *dev = Oa2dpGetDevice(Device);
    WdfSpinLockAcquire(dev->Lock);
    dev->SurpriseRemoved = TRUE;
    dev->TransportStopping = TRUE;
    Oa2dpQueueEventLocked(dev, Oa2dpEventDeviceGone, NULL, 0);
    WdfSpinLockRelease(dev->Lock);
    Oa2dpDeliverEvents(dev);
}

NTSTATUS Oa2dpEvtReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Translated)
{
    OA2DP_DEVICE_CONTEXT *dev = Oa2dpGetDevice(Device);
    UNREFERENCED_PARAMETER(Translated);
    PAGED_CODE();
    WdfSpinLockAcquire(dev->Lock);
    dev->TransportStopping = TRUE;
    WdfSpinLockRelease(dev->Lock);
    NTSTATUS status = Oa2dpUnregisterLinkNotifications(dev);
    NTSTATUS serverStatus = Oa2dpUnregisterServer(dev);
    if (NT_SUCCESS(status) && !NT_SUCCESS(serverStatus)) status = serverStatus;
    Oa2dpCloseAllChannels(dev);
    for (ULONG i = 0; i < Oa2dpChannelCount; ++i)
        WdfWorkItemFlush(dev->Channels[i].CloseWork);
    NTSTATUS published = Oa2dpPublishConnected(dev, FALSE, 0);
    Oa2dpMpmStop(dev);
    if (NT_SUCCESS(status) && !NT_SUCCESS(published)) status = published;
    Oa2dpPostEvent(dev, Oa2dpEventDeviceGone, NULL, 0);
    return status;
}

/* ------------------------------------------------------------------------ */

VOID Oa2dpEvtDeviceDestroy(WDFOBJECT Object)
{
    OA2DP_DEVICE_CONTEXT *dev = Oa2dpGetDevice((WDFDEVICE)Object);
    // WDF destroys child objects before their parent; device destruction is
    // PASSIVE_LEVEL. Do not call methods on already-destroyed WDF children here.
    if (dev->ProfileAcquired) {
        INTERFACE imported = dev->Profile.Interface;
        dev->ProfileAcquired = FALSE;
        RtlZeroMemory(&dev->Profile, sizeof(dev->Profile));
        if (imported.InterfaceDereference)
            imported.InterfaceDereference(imported.Context);
    }
}

VOID Oa2dpEvtFileCleanup(_In_ WDFFILEOBJECT file)
{
    /* A crashed/restarted service must not inherit orphaned open channels. */
    OA2DP_DEVICE_CONTEXT *dev = Oa2dpGetDevice(WdfFileObjectGetDevice(file));
    const BOOLEAN control = Oa2dpGetFile(file)->ControlClient;
    Oa2dpVolumeFileCleanup(dev, file);
    if (control) {
        Oa2dpCloseAllChannels(dev);
        (void)Oa2dpPublishConnected(dev, FALSE, 0);
        WdfSpinLockAcquire(dev->Lock);
        if (dev->ServiceFile == file) dev->ServiceFile = NULL;
        WdfSpinLockRelease(dev->Lock);
    }
}

static VOID CompleteEvent(_In_ WDFREQUEST req, _In_ const OA2DP_EVENT *event)
{
    OA2DP_EVENT *ev;
    if (!NT_SUCCESS(WdfRequestRetrieveOutputBuffer(req, sizeof(*ev), (PVOID *)&ev, NULL))) {
        WdfRequestComplete(req, STATUS_BUFFER_TOO_SMALL);
        return;
    }
    *ev = *event;
    WdfRequestCompleteWithInformation(req, STATUS_SUCCESS, sizeof(*ev));
}

static VOID DeliverPendingEvents(_In_ OA2DP_DEVICE_CONTEXT *dev)
{
    for (;;) {
        WDFREQUEST req = NULL;
        OA2DP_EVENT event;
        WdfSpinLockAcquire(dev->Lock);
        if ((!dev->EventCount && !dev->EventOverflow) ||
            !NT_SUCCESS(WdfIoQueueRetrieveNextRequest(dev->EventQueue, &req))) {
            WdfSpinLockRelease(dev->Lock);
            return;
        }
        if (dev->EventOverflow) {
            RtlZeroMemory(&event, sizeof(event));
            event.Type = Oa2dpEventStateLost;
            dev->EventOverflow = FALSE;
        } else {
            event = dev->Events[dev->EventHead];
            dev->EventHead = (dev->EventHead + 1) % OA2DP_EVENT_CAPACITY;
            --dev->EventCount;
        }
        WdfSpinLockRelease(dev->Lock);
        CompleteEvent(req, &event);
    }
}

VOID Oa2dpQueueEventLocked(OA2DP_DEVICE_CONTEXT *dev, ULONG type,
                           OA2DP_CHANNEL *ch, ULONG generation)
{
    OA2DP_EVENT event;
    RtlZeroMemory(&event, sizeof(event));
    event.Type = type;
    if (ch) {
        if (!Oa2dpChannelEventReady(&ch->Life, generation, type == Oa2dpEventChannelOpened)) {
            return;
        }
        event.Kind = ch->Kind;
        event.Channel.Kind = ch->Kind;
        event.Channel.OutMtu = ch->OutMtu;
        event.Channel.InMtu = ch->InMtu;
        event.Channel.Generation = generation;
        event.Channel.ConnectionGeneration = ch->Life.ConnectionGeneration;
    }
    if (type == Oa2dpEventDeviceGone) {
        dev->EventHead = dev->EventCount = 0;
        dev->EventOverflow = FALSE;
    }
    if (type == Oa2dpEventLinkAvailable) {
        // Reconnect hints must not overflow the authoritative channel backlog.
        // Coalesce them; an omitted hint still has the existing timed retry.
        if (dev->EventOverflow || dev->EventCount == OA2DP_EVENT_CAPACITY) return;
        for (ULONG i = 0; i < dev->EventCount; ++i)
            if (dev->Events[(dev->EventHead + i) % OA2DP_EVENT_CAPACITY].Type == type)
                return;
    }
    if (!dev->EventOverflow) {
        if (dev->EventCount == OA2DP_EVENT_CAPACITY) {
            dev->EventHead = dev->EventCount = 0;
            dev->EventOverflow = TRUE;
        } else {
            ULONG tail = (dev->EventHead + dev->EventCount) % OA2DP_EVENT_CAPACITY;
            dev->Events[tail] = event;
            ++dev->EventCount;
        }
    }
}

VOID Oa2dpDeliverEvents(OA2DP_DEVICE_CONTEXT *dev)
{
    DeliverPendingEvents(dev);
}

VOID Oa2dpPostEvent(OA2DP_DEVICE_CONTEXT *dev, ULONG type,
                    OA2DP_CHANNEL *ch, ULONG generation)
{
    WdfSpinLockAcquire(dev->Lock);
    Oa2dpQueueEventLocked(dev, type, ch, generation);
    WdfSpinLockRelease(dev->Lock);
    DeliverPendingEvents(dev);
}

static VOID WaitEvent(_In_ OA2DP_DEVICE_CONTEXT *dev, _In_ WDFREQUEST request)
{
    OA2DP_EVENT *out;
    NTSTATUS status = WdfRequestRetrieveOutputBuffer(request, sizeof(*out), (PVOID *)&out, NULL);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(request, status);
        return;
    }
    /* Forward can dispatch another source-queue callback synchronously.
     * Never hold our lock across it. Both paths drain under the same lock
     * afterwards, so an event arriving before insertion is still delivered. */
    status = WdfRequestForwardToIoQueue(request, dev->EventQueue);
    if (NT_SUCCESS(status))
        DeliverPendingEvents(dev);
    else
        WdfRequestComplete(request, status);
}

static NTSTATUS Oa2dpChildEnumerated(WDFDEVICE child, PIRP irp)
{
    OA2DP_DEVICE_CONTEXT *dev = Oa2dpGetDevice(WdfPdoGetParent(child));
    PDEVICE_OBJECT pdo = WdfDeviceWdmGetDeviceObject(child);
    WCHAR address[13];
    ULONG property = 0;
    NTSTATUS status;
    PAGED_CODE();

    /* PnP now knows this PDO. A WDF property wrapper is still too early in
     * this preprocess callback: WDF has not processed DEVICE_ENUMERATED yet.
     * The documented WDM enumeration contract permits setting PDO properties
     * here, before user mode is notified and the audio FDO is installed. */
    status = RtlStringCchPrintfW(address, ARRAYSIZE(address), L"%012I64x",
                                (ULONGLONG)dev->RemoteAddress);
    if (NT_SUCCESS(status)) {
        property = 1;
        status = IoSetDevicePropertyData(pdo, &BluetoothAddressKey,
            LOCALE_NEUTRAL, 0, DEVPROP_TYPE_STRING, sizeof(address), address);
    }
    if (NT_SUCCESS(status)) {
        property = 2;
        status = IoSetDevicePropertyData(pdo, &BluetoothServiceKey,
            LOCALE_NEUTRAL, 0, DEVPROP_TYPE_GUID, sizeof(AudioSinkService), (PVOID)&AudioSinkService);
    }
    TraceLoggingWrite(g_Oa2dpTransportTrace, "ChildIdentityResult",
        TraceLoggingUInt32(property, "PropertyIndex"),
        TraceLoggingNTStatus(status, "Status"));
    if (!NT_SUCCESS(status)) {
        irp->IoStatus.Status = status;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return status;
    }
    IoSkipCurrentIrpStackLocation(irp);
    return WdfDeviceWdmDispatchPreprocessedIrp(child, irp);
}

NTSTATUS Oa2dpCreateAudioChild(_In_ OA2DP_DEVICE_CONTEXT *dev)
{
    NTSTATUS status;
    PWDFDEVICE_INIT init;
    WDFDEVICE child;
    WCHAR instance[24];
    UNICODE_STRING id;
    DECLARE_CONST_UNICODE_STRING(deviceId, L"OA2DP\\Render");
    DECLARE_CONST_UNICODE_STRING(hardwareId, L"OA2DP\\Render");

    if (dev->AudioChild)
        return STATUS_SUCCESS;

    init = WdfPdoInitAllocate(dev->Device);
    if (!init)
        return STATUS_INSUFFICIENT_RESOURCES;

    status = WdfPdoInitAssignDeviceID(init, &deviceId);
    if (NT_SUCCESS(status))
        status = WdfPdoInitAddHardwareID(init, &hardwareId);
    if (NT_SUCCESS(status)) {
        /* One child per headset: instance ID = Bluetooth address. */
        status = RtlStringCchPrintfW(instance, ARRAYSIZE(instance), L"%012I64X",
                                     (ULONGLONG)dev->RemoteAddress);
    }
    if (NT_SUCCESS(status)) {
        RtlInitUnicodeString(&id, instance);
        status = WdfPdoInitAssignInstanceID(init, &id);
    }
    if (NT_SUCCESS(status)) {
        UCHAR minor = IRP_MN_DEVICE_ENUMERATED;
        status = WdfDeviceInitAssignWdmIrpPreprocessCallback(init, Oa2dpChildEnumerated,
                                                           IRP_MJ_PNP, &minor, 1);
    }
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(init);
        return status;
    }

    status = WdfDeviceCreate(&init, WDF_NO_OBJECT_ATTRIBUTES, &child);
    if (!NT_SUCCESS(status)) {
        if (init)
            WdfDeviceInitFree(init);
        return status;
    }
    {
        /* The address is unique system-wide: without UniqueID the PnP manager
         * prefixes the instance ID with parent data, and the service (which
         * looks for "#<address>#" in oa2dpaudio's interface path) would not
         * find the endpoint. */
        WDF_DEVICE_PNP_CAPABILITIES caps;
        WDF_DEVICE_PNP_CAPABILITIES_INIT(&caps);
        caps.UniqueID = WdfTrue;
        WdfDeviceSetPnpCapabilities(child, &caps);
    }
    status = Oa2dpRegisterBusInterface(dev, child);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(child);
        return status;
    }
    status = WdfFdoAddStaticChild(dev->Device, child);
    if (NT_SUCCESS(status))
        dev->AudioChild = child;
    return status;
}

/* ------------------------------------------------------------------------ */

VOID Oa2dpEvtIoDeviceControl(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request,
                             _In_ size_t OutLen, _In_ size_t InLen, _In_ ULONG Code)
{
    OA2DP_DEVICE_CONTEXT *dev = Oa2dpGetDevice(WdfIoQueueGetDevice(Queue));
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info = 0;
    UNREFERENCED_PARAMETER(OutLen);
    UNREFERENCED_PARAMETER(InLen);

    WDFFILEOBJECT file = WdfRequestGetFileObject(Request);
    if (!file || !Oa2dpGetFile(file)->ControlClient) {
        WdfRequestComplete(Request, STATUS_ACCESS_DENIED);
        return;
    }

    switch (Code) {
    case IOCTL_OA2DP_GET_INFO: {
        OA2DP_DEVICE_INFO *out;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out), (PVOID *)&out, NULL);
        if (NT_SUCCESS(status)) {
            RtlZeroMemory(out, sizeof(*out));
            out->Version = OA2DP_IOCTL_VERSION;
            WdfSpinLockAcquire(dev->Lock);
            if (dev->MpmHandle && !dev->MpmStopping) out->Flags |= OA2DP_DEVICE_MPM_REGISTERED;
            if (dev->PowerSuspended) out->Flags |= OA2DP_DEVICE_SUSPENDED;
            if (dev->LinkNotifications && !dev->LinkNotificationsStopping)
                out->Flags |= OA2DP_DEVICE_LINK_NOTIFICATIONS;
            out->PowerSerial = dev->PowerSerial;
            WdfSpinLockRelease(dev->Lock);
            out->RemoteAddress = dev->RemoteAddress;
            out->LocalAddress = dev->LocalAddress;
            info = sizeof(*out);
        }
        break;
    }

    case IOCTL_OA2DP_OPEN_CHANNEL: {
        OA2DP_OPEN_CHANNEL_IN *in;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, NULL);
        if (NT_SUCCESS(status) && in->Kind >= Oa2dpChannelCount)
            status = STATUS_INVALID_PARAMETER;
        if (NT_SUCCESS(status)) {
            Oa2dpOpenChannel(dev, Request, in); /* completes the request */
            return;
        }
        break;
    }

    case IOCTL_OA2DP_CLOSE_CHANNEL: {
        OA2DP_CLOSE_CHANNEL_IN *in = NULL;
        OA2DP_CLOSE_CHANNEL_OUT *out = NULL;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, NULL);
        if (NT_SUCCESS(status))
            status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out), (PVOID *)&out, NULL);
        if (NT_SUCCESS(status) && in && out) {
            // METHOD_BUFFERED input/output may alias; snapshot before writing.
            const ULONG kind = in->Kind, connection = in->ConnectionGeneration;
            if (kind >= Oa2dpChannelCount) status = STATUS_INVALID_PARAMETER;
            else status = Oa2dpCloseConnectionChannel(dev, (OA2DP_CHANNEL_KIND)kind,
                                                      connection, &out->ConnectionGeneration);
            if (NT_SUCCESS(status)) info = sizeof(*out);
        } else if (NT_SUCCESS(status)) {
            status = STATUS_INVALID_USER_BUFFER;
        }
        break;
    }

    case IOCTL_OA2DP_SEND:
    case IOCTL_OA2DP_RECV: {
        OA2DP_SEND_IN *in;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, NULL);
        if (NT_SUCCESS(status) && in->Kind >= Oa2dpChannelCount)
            status = STATUS_INVALID_PARAMETER;
        if (NT_SUCCESS(status)) {
            Oa2dpTransfer(dev, Request, in, Code == IOCTL_OA2DP_SEND);
            return;
        }
        break;
    }

    case IOCTL_OA2DP_WAIT_EVENT:
        WaitEvent(dev, Request);
        return;

    case IOCTL_OA2DP_GET_TX_STATE: {
        OA2DP_TX_STATE *out;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out), (PVOID *)&out, NULL);
        if (NT_SUCCESS(status)) {
            out->QueuedPackets = (ULONG)InterlockedCompareExchange(&dev->TxQueuedPackets, 0, 0);
            out->QueuedBytes = (ULONG)InterlockedCompareExchange(&dev->TxQueuedBytes, 0, 0);
            out->SentPackets = (ULONGLONG)InterlockedCompareExchange64(&dev->TxSent, 0, 0);
            out->FailedPackets = (ULONGLONG)InterlockedCompareExchange64(&dev->TxFailed, 0, 0);
            info = sizeof(*out);
        }
        break;
    }

    default:
        break;
    }
    WdfRequestCompleteWithInformation(Request, status, info);
}
