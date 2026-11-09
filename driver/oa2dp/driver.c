/* SPDX-License-Identifier: MIT */
#include "oa2dp.h"
#include <devpropdef.h>

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
    WDF_DRIVER_CONFIG_INIT(&config, Oa2dpEvtDeviceAdd);
    config.DriverPoolTag = OA2DP_POOL_TAG;
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config,
                           WDF_NO_HANDLE);
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
    status = Oa2dpInitializeChannels(dev);
    if (!NT_SUCCESS(status))
        return status;

    /* PASSIVE_LEVEL dispatch: CLOSE_CHANNEL submits BRBs synchronously. */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qcfg, WdfIoQueueDispatchParallel);
    qcfg.EvtIoDeviceControl = Oa2dpEvtIoDeviceControl;
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfIoQueueCreate(device, &qcfg, &attrs, &dev->DefaultQueue);
    if (!NT_SUCCESS(status))
        return status;

    WDF_IO_QUEUE_CONFIG_INIT(&qcfg, WdfIoQueueDispatchManual);
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

    status = WdfFdoQueryForInterface(Device, &GUID_BTHDDI_PROFILE_DRIVER_INTERFACE,
                                     (PINTERFACE)&dev->Profile, sizeof(dev->Profile),
                                     BTHDDI_PROFILE_DRIVER_INTERFACE_VERSION_FOR_QI, NULL);
    if (!NT_SUCCESS(status))
        return status;

    status = Oa2dpQueryLocalAddress(dev);
    if (!NT_SUCCESS(status))
        return status;
    status = Oa2dpQueryRemoteAddress(dev);
    if (!NT_SUCCESS(status))
        return status;
    WdfSpinLockAcquire(dev->Lock);
    // ReleaseHardware may have queued DeviceGone after the old waiter was
    // canceled. That event belongs to the previous PnP lifetime, never to a
    // replacement service session after resume/radio restart.
    dev->EventHead = dev->EventCount = 0;
    dev->EventOverflow = FALSE;
    WdfSpinLockRelease(dev->Lock);
    Oa2dpPublishConnected(dev, FALSE);

    dev->TransportStopping = FALSE;
    status = Oa2dpMpmStart(dev);
    if (!NT_SUCCESS(status))
        return status;
    /* Establish policy before allowing peer-initiated channels. */
    (void)Oa2dpRegisterServer(dev);
    status = Oa2dpCreateAudioChild(dev);
    if (!NT_SUCCESS(status)) {
        Oa2dpMpmStop(dev);
        Oa2dpUnregisterServer(dev);
    }
    return status;
}

NTSTATUS Oa2dpEvtReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Translated)
{
    OA2DP_DEVICE_CONTEXT *dev = Oa2dpGetDevice(Device);
    UNREFERENCED_PARAMETER(Translated);
    PAGED_CODE();
    WdfSpinLockAcquire(dev->Lock);
    dev->TransportStopping = TRUE;
    WdfSpinLockRelease(dev->Lock);
    Oa2dpUnregisterServer(dev);
    Oa2dpCloseAllChannels(dev);
    for (ULONG i = 0; i < Oa2dpChannelCount; ++i)
        WdfWorkItemFlush(dev->Channels[i].CloseWork);
    Oa2dpPublishConnected(dev, FALSE);
    Oa2dpMpmStop(dev);
    Oa2dpPostEvent(dev, Oa2dpEventDeviceGone, NULL, 0);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------ */

VOID Oa2dpEvtFileCleanup(_In_ WDFFILEOBJECT file)
{
    /* A crashed/restarted service must not inherit orphaned open channels. */
    OA2DP_DEVICE_CONTEXT *dev = Oa2dpGetDevice(WdfFileObjectGetDevice(file));
    const BOOLEAN control = Oa2dpGetFile(file)->ControlClient;
    Oa2dpVolumeFileCleanup(dev, file);
    if (control) {
        Oa2dpCloseAllChannels(dev);
        (void)Oa2dpPublishConnected(dev, FALSE);
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
    {
        WDF_DEVICE_PROPERTY_DATA property;
        WCHAR address[13];
        status = RtlStringCchPrintfW(address, ARRAYSIZE(address), L"%012I64x",
                                     (ULONGLONG)dev->RemoteAddress);
        if (NT_SUCCESS(status)) {
            WDF_DEVICE_PROPERTY_DATA_INIT(&property, &BluetoothAddressKey);
            status = WdfDeviceAssignProperty(child, &property,
                DEVPROP_TYPE_STRING, sizeof(address), address);
        }
        if (NT_SUCCESS(status)) {
            WDF_DEVICE_PROPERTY_DATA_INIT(&property, &BluetoothServiceKey);
            status = WdfDeviceAssignProperty(child, &property,
                DEVPROP_TYPE_GUID, sizeof(AudioSinkService), (PVOID)&AudioSinkService);
        }
        if (!NT_SUCCESS(status)) {
            WdfObjectDelete(child);
            return status;
        }
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
            if (dev->MpmHandle) out->Flags |= OA2DP_DEVICE_MPM_REGISTERED;
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
            Oa2dpTransfer(dev, Request, (OA2DP_CHANNEL_KIND)in->Kind, Code == IOCTL_OA2DP_SEND);
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
