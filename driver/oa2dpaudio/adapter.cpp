/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    adapter.cpp

Abstract:

    Setup and miniport installation.  No resources are used by sysvad.
    This sample is to demonstrate how to develop a full featured audio miniport driver.

--*/
/*
 * Derived from the SYSVAD sample (Windows-driver-samples/audio/sysvad,
 * commit 2dc3fd3a, MS-PL — see LICENSE.MS-PL in this folder): adapter.cpp
 * and the subdevice install/connect code of common.cpp. Modified for
 * OpenA2DP:
 *  - one render endpoint (wave + topology) per OA2DP\Render\<address> PDO;
 *    no adapter-common object, no KMDF, no PoFx, no registry knobs;
 *  - IRP_MJ_CREATE/CLEANUP/CLOSE/DEVICE_CONTROL are hooked so the control
 *    interface for oa2dpsvc.exe (control.cpp) can share PortCls's device
 *    object; everything else still goes to PortCls;
 *  - name updates publish metadata without re-registering working filters.
 */
// All the GUIDS for the port class and our interfaces end up in this object.
#include <initguid.h>
#include "driver.h"
#include <devpkey.h>
#include "minwavert.h"
#include "trace.h"

TRACELOGGING_DEFINE_PROVIDER(g_Oa2dpAudioTrace, "XM6Audio.WaveRT",
    (0xad2dba1f, 0x78a1, 0x4d25, 0xa6, 0x6e, 0xd3, 0x19, 0x5b, 0x9b, 0x1f, 0x72));

#define OA2DP_MAX_MINIPORTS 4   // wave + topology, with room for a re-registration

typedef void (*fnPcDriverUnload) (PDRIVER_OBJECT);
static fnPcDriverUnload gPCDriverUnloadRoutine = NULL;
static PDRIVER_DISPATCH gPcDispatch[IRP_MJ_MAXIMUM_FUNCTION + 1];

extern "C" DRIVER_INITIALIZE DriverEntry;
extern "C" DRIVER_UNLOAD DriverUnload;
DRIVER_ADD_DEVICE AddDevice;
_Dispatch_type_(IRP_MJ_PNP)
DRIVER_DISPATCH PnpHandler;
_Dispatch_type_(IRP_MJ_CREATE)
_Dispatch_type_(IRP_MJ_CLEANUP)
_Dispatch_type_(IRP_MJ_CLOSE)
_Dispatch_type_(IRP_MJ_DEVICE_CONTROL)
DRIVER_DISPATCH Oa2dpDispatch;

NTSTATUS StartDevice(_In_ PDEVICE_OBJECT, _In_ PIRP, _In_ PRESOURCELIST);

//=============================================================================
#pragma code_seg("PAGE")
extern "C"
void DriverUnload
(
    _In_ PDRIVER_OBJECT DriverObject
)
{
    PAGED_CODE();

    // Invoke the port unload.
    if (gPCDriverUnloadRoutine != NULL)
    {
        gPCDriverUnloadRoutine(DriverObject);
    }
    TraceLoggingUnregister(g_Oa2dpAudioTrace);
}

//=============================================================================
#pragma code_seg("INIT")
extern "C" NTSTATUS
DriverEntry
(
    _In_  PDRIVER_OBJECT          DriverObject,
    _In_  PUNICODE_STRING         RegistryPathName
)
/*++

Routine Description:

  Installable driver initialization entry point.

--*/
{
    static const UCHAR hooked[] = { IRP_MJ_CREATE, IRP_MJ_CLEANUP, IRP_MJ_CLOSE, IRP_MJ_DEVICE_CONTROL };
    NTSTATUS ntStatus;

    // Tell the class driver to initialize the driver.
    ntStatus = PcInitializeAdapterDriver(DriverObject,
                                         RegistryPathName,
                                         (PDRIVER_ADD_DEVICE)AddDevice);
    if (!NT_SUCCESS(ntStatus))
    {
        return ntStatus;
    }

    // To intercept stop/remove/surprise-remove.
    DriverObject->MajorFunction[IRP_MJ_PNP] = PnpHandler;

    // PortCls owns the dispatch table; the control interface shares the FDO,
    // so we look at these IRPs first and pass the rest on.
    for (ULONG i = 0; i < RTL_NUMBER_OF(hooked); i++)
    {
        gPcDispatch[hooked[i]] = DriverObject->MajorFunction[hooked[i]];
        DriverObject->MajorFunction[hooked[i]] = Oa2dpDispatch;
    }

    // Hook the port class unload function
    gPCDriverUnloadRoutine = DriverObject->DriverUnload;
    DriverObject->DriverUnload = DriverUnload;

    // Diagnostics must never prevent the audio driver from loading.
    (void)TraceLoggingRegister(g_Oa2dpAudioTrace);

    return STATUS_SUCCESS;
} // DriverEntry

// disable prefast warning 28152 because
// DO_DEVICE_INITIALIZING is cleared in PcAddAdapterDevice
#pragma warning(disable:28152)

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS AddDevice
(
    _In_  PDRIVER_OBJECT    DriverObject,
    _In_  PDEVICE_OBJECT    PhysicalDeviceObject
)
/*++

Routine Description:

  The Plug & Play subsystem is handing us a brand new PDO (OA2DP\Render\<addr>,
  created by oa2dp.sys). PortCls creates the FDO; our OA2DP_AUDIO_DEVICE lives
  after PortCls's part of its extension.

--*/
{
    PAGED_CODE();

    return PcAddAdapterDevice(DriverObject,
                              PhysicalDeviceObject,
                              PCPFNSTARTDEVICE(StartDevice),
                              OA2DP_MAX_MINIPORTS,
                              PORT_CLASS_DEVICE_EXTENSION_SIZE + sizeof(OA2DP_AUDIO_DEVICE));
} // AddDevice

//=============================================================================
#pragma code_seg()
NTSTATUS
Oa2dpDispatch
(
    _In_ DEVICE_OBJECT *DeviceObject,
    _Inout_ IRP *Irp
)
{
    OA2DP_AUDIO_DEVICE *dev = OA2DP_DEVICE(DeviceObject);
    PIO_STACK_LOCATION  sp = IoGetCurrentIrpStackLocation(Irp);
    BOOLEAN             ours;

    ours = sp->MajorFunction == IRP_MJ_CREATE ? Oa2dpControlIsCreate(Irp) : Oa2dpControlOwns(dev, Irp);
    if (ours)
    {
        return Oa2dpControlDispatch(dev, Irp);
    }
    // PortCls may complete/free the IRP. Save only scalar input before forwarding.
    const UCHAR major = sp->MajorFunction;
    const BOOLEAN related = sp->FileObject && sp->FileObject->RelatedFileObject;
    const USHORT nameBytes = sp->FileObject ? sp->FileObject->FileName.Length : 0;
    const ULONG ioctl = major == IRP_MJ_DEVICE_CONTROL ? sp->Parameters.DeviceIoControl.IoControlCode : 0;
    const ULONG inputBytes = major == IRP_MJ_DEVICE_CONTROL ? sp->Parameters.DeviceIoControl.InputBufferLength : 0;
    const ULONG outputBytes = major == IRP_MJ_DEVICE_CONTROL ? sp->Parameters.DeviceIoControl.OutputBufferLength : 0;
    NTSTATUS status = gPcDispatch[major](DeviceObject, Irp);
    if (major == IRP_MJ_CREATE)
    {
        TraceLoggingWrite(g_Oa2dpAudioTrace, "PortCreate",
            TraceLoggingBool(related, "Related"),
            TraceLoggingUInt16(nameBytes, "NameBytes"),
            TraceLoggingNTStatus(status, "Status"));
    }
    else if (major == IRP_MJ_DEVICE_CONTROL && ioctl == IOCTL_KS_PROPERTY)
    {
        TraceLoggingWrite(g_Oa2dpAudioTrace, "KsProperty",
            TraceLoggingUInt32(inputBytes, "InputBytes"),
            TraceLoggingUInt32(outputBytes, "OutputBytes"),
            TraceLoggingNTStatus(status, "Status"));
    }
    return status;
}

//=============================================================================
// Best effort: name the KS interfaces too (legacy WinMM device names).
#pragma code_seg("PAGE")
// ControlLock held; all five interface identities must match the INF.
extern "C" VOID Oa2dpPublishNameLocked(_In_ OA2DP_AUDIO_DEVICE *dev)
{
    static const struct { const GUID *Category; PCWSTR Reference; } interfaces[] = {
        { &KSCATEGORY_AUDIO, OA2DP_WAVE_NAME },
        { &KSCATEGORY_RENDER, OA2DP_WAVE_NAME },
        { &KSCATEGORY_REALTIME, OA2DP_WAVE_NAME },
        { &KSCATEGORY_AUDIO, OA2DP_TOPO_NAME },
        { &KSCATEGORY_TOPOLOGY, OA2DP_TOPO_NAME },
    };
    size_t len = 0;

    PAGED_CODE();

    if (!NT_SUCCESS(RtlStringCchLengthW(dev->Name, RTL_NUMBER_OF(dev->Name), &len)) || len == 0)
        return;
    const ULONG bytes = (ULONG)((len + 1) * sizeof(WCHAR));
    NTSTATUS status = IoSetDevicePropertyData(dev->Pdo, &DEVPKEY_Device_FriendlyName,
        LOCALE_NEUTRAL, PLUGPLAY_PROPERTY_PERSISTENT, DEVPROP_TYPE_STRING, bytes, dev->Name);
    if (!NT_SUCCESS(status))
        KdPrintEx((DPFLTR_IHVAUDIO_ID, DPFLTR_WARNING_LEVEL,
            "oa2dpaudio: publishing device name failed 0x%08X\n", status));
    for (ULONG i = 0; i < RTL_NUMBER_OF(interfaces); ++i) {
        UNICODE_STRING ref, link;
        RtlInitUnicodeString(&ref, interfaces[i].Reference);
        status = IoRegisterDeviceInterface(dev->Pdo, interfaces[i].Category, &ref, &link);
        if (NT_SUCCESS(status)) {
            status = IoSetDeviceInterfacePropertyData(&link, &DEVPKEY_DeviceInterface_FriendlyName,
                LOCALE_NEUTRAL, PLUGPLAY_PROPERTY_PERSISTENT, DEVPROP_TYPE_STRING, bytes, dev->Name);
            RtlFreeUnicodeString(&link);
        }
        if (!NT_SUCCESS(status))
            KdPrintEx((DPFLTR_IHVAUDIO_ID, DPFLTR_WARNING_LEVEL,
                "oa2dpaudio: publishing interface name %lu failed 0x%08X\n", i, status));
    }
}

//=============================================================================
#pragma code_seg("PAGE")
static NTSTATUS
InstallSubdevice
(
    _In_  OA2DP_AUDIO_DEVICE   *dev,
    _In_  PCWSTR                Name,
    _In_  REFGUID               PortClassId,
    _In_  PUNKNOWN              Miniport,
    _Out_ PUNKNOWN             *OutPort
)
/*++

Routine Description:

    This function creates and registers a subdevice consisting of a port
    driver, a minport driver and a set of resources bound together.

--*/
{
    PAGED_CODE();

    NTSTATUS    ntStatus;
    PPORT       port = NULL;

    *OutPort = NULL;

    // Create the port driver object
    ntStatus = PcNewPort(&port, PortClassId);

    // Init the port driver and miniport in one go.
    if (NT_SUCCESS(ntStatus))
    {
#pragma warning(push)
        // IPort::Init's annotation on ResourceList requires it to be non-NULL.  However,
        // for dynamic devices, we may no longer have the resource list and this should
        // still succeed.
#pragma warning(disable:6387)
        ntStatus = port->Init(dev->Fdo, NULL, Miniport, NULL, NULL);
#pragma warning (pop)
    }

    // Register the subdevice (port/miniport combination).
    if (NT_SUCCESS(ntStatus))
    {
        ntStatus = PcRegisterSubdevice(dev->Fdo, (PWSTR)Name, port);
    }

    if (NT_SUCCESS(ntStatus))
    {
        *OutPort = port;    // keep our reference
        port = NULL;
    }

    SAFE_RELEASE(port);
    return ntStatus;
} // InstallSubdevice

//=============================================================================
#pragma code_seg("PAGE")
static VOID
UnregisterSubdevice
(
    _In_     OA2DP_AUDIO_DEVICE *dev,
    _In_opt_ PUNKNOWN            UnknownPort
)
{
    PAGED_CODE();

    PUNREGISTERSUBDEVICE unregisterSubdevice = NULL;

    if (NULL == UnknownPort)
    {
        return;
    }

    if (NT_SUCCESS(UnknownPort->QueryInterface(IID_IUnregisterSubdevice, (PVOID *)&unregisterSubdevice)))
    {
        (void)unregisterSubdevice->UnregisterSubdevice(dev->Fdo, UnknownPort);
        unregisterSubdevice->Release();
    }
}

//=============================================================================
// FilterLock held.
#pragma code_seg("PAGE")
static VOID
RemoveFilters
(
    _Inout_ OA2DP_AUDIO_DEVICE *dev
)
{
    PAGED_CODE();

    PUNREGISTERPHYSICALCONNECTION unregisterPhysicalConnection = NULL;
    PUNKNOWN wavePort = static_cast<PUNKNOWN>(dev->WavePort);
    PUNKNOWN topoPort = static_cast<PUNKNOWN>(dev->TopoPort);

    if (topoPort && wavePort &&
        NT_SUCCESS(topoPort->QueryInterface(IID_IUnregisterPhysicalConnection,
                                            (PVOID *)&unregisterPhysicalConnection)))
    {
        (void)unregisterPhysicalConnection->UnregisterPhysicalConnection(
            dev->Fdo, wavePort, OA2DP_WAVE_PIN_BRIDGE, topoPort, OA2DP_TOPO_PIN_IN);
        unregisterPhysicalConnection->Release();
    }

    UnregisterSubdevice(dev, wavePort);
    UnregisterSubdevice(dev, topoPort);
    SAFE_RELEASE(wavePort);
    SAFE_RELEASE(topoPort);
    dev->WavePort = NULL;
    dev->TopoPort = NULL;
    if (dev->WaveMiniport)
    {
        static_cast<CMiniportWaveRT *>(dev->WaveMiniport)->Release();
        dev->WaveMiniport = NULL;
    }
}

//=============================================================================
// FilterLock held.
#pragma code_seg("PAGE")
static NTSTATUS
InstallFilters
(
    _Inout_ OA2DP_AUDIO_DEVICE *dev
)
{
    PAGED_CODE();

    NTSTATUS            ntStatus;
    PUNKNOWN            miniTopo = NULL;
    PUNKNOWN            topoPort = NULL;
    PUNKNOWN            wavePort = NULL;
    CMiniportWaveRT    *miniWave = NULL;

    // Restore metadata before PortCls exposes the filters. A driver reinstall
    // may have restored the INF defaults even when our cached name is unchanged.
    Oa2dpLock(&dev->ControlLock);
    Oa2dpPublishNameLocked(dev);
    Oa2dpUnlock(&dev->ControlLock);

    if (dev->WavePort != NULL)
    {
        return STATUS_SUCCESS;
    }

    // Topology first (the wave filter's bridge pin connects to it).
    ntStatus = CreateMiniportTopology(&miniTopo, dev);
    if (NT_SUCCESS(ntStatus))
    {
        ntStatus = InstallSubdevice(dev, OA2DP_TOPO_NAME, CLSID_PortTopology, miniTopo, &topoPort);
    }
    if (NT_SUCCESS(ntStatus))
    {
        ntStatus = CreateMiniportWaveRT(&miniWave, dev);
    }
    if (NT_SUCCESS(ntStatus))
    {
        ntStatus = InstallSubdevice(dev, OA2DP_WAVE_NAME, CLSID_PortWaveRT,
                                    PUNKNOWN(PMINIPORTWAVERT(miniWave)), &wavePort);
    }

    // register wave <=> topology connections
    if (NT_SUCCESS(ntStatus))
    {
        ntStatus = PcRegisterPhysicalConnection(dev->Fdo,
                                                wavePort, OA2DP_WAVE_PIN_BRIDGE,
                                                topoPort, OA2DP_TOPO_PIN_IN);
    }

    if (NT_SUCCESS(ntStatus))
    {
        dev->TopoPort = topoPort;
        dev->WavePort = wavePort;
        dev->WaveMiniport = miniWave;   // keeps our reference
        topoPort = NULL;
        wavePort = NULL;
        miniWave = NULL;
    }
    else
    {
        UnregisterSubdevice(dev, wavePort);
        UnregisterSubdevice(dev, topoPort);
    }

    SAFE_RELEASE(miniTopo);
    SAFE_RELEASE(miniWave);
    SAFE_RELEASE(wavePort);
    SAFE_RELEASE(topoPort);
    return ntStatus;
}

//=============================================================================
#pragma code_seg()
static NTSTATUS BusQueryComplete(PDEVICE_OBJECT, PIRP, PVOID context)
{
    KeSetEvent((PKEVENT)context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

#pragma code_seg("PAGE")
static NTSTATUS QueryVolumeBus(_Inout_ OA2DP_AUDIO_DEVICE *dev)
{
    PAGED_CODE();
    KEVENT event;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    RtlZeroMemory(&dev->Bus, sizeof(dev->Bus));
    PIRP irp = IoAllocateIrp(dev->Pdo->StackSize, FALSE);
    if (!irp) return STATUS_INSUFFICIENT_RESOURCES;
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    irp->RequestorMode = KernelMode;
    PIO_STACK_LOCATION stack = IoGetNextIrpStackLocation(irp);
    stack->MajorFunction = IRP_MJ_PNP;
    stack->MinorFunction = IRP_MN_QUERY_INTERFACE;
    stack->Parameters.QueryInterface.InterfaceType = &GUID_OA2DP_BUS_INTERFACE;
    stack->Parameters.QueryInterface.Size = sizeof(dev->Bus);
    stack->Parameters.QueryInterface.Version = OA2DP_BUS_VERSION;
    stack->Parameters.QueryInterface.Interface = (PINTERFACE)&dev->Bus;
    stack->Parameters.QueryInterface.InterfaceSpecificData = NULL;
    IoSetCompletionRoutine(irp, BusQueryComplete, &event, TRUE, TRUE, TRUE);
    NTSTATUS status = IoCallDriver(dev->Pdo, irp);
    if (status == STATUS_PENDING)
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
    status = irp->IoStatus.Status;
    IoFreeIrp(irp);
    if (!NT_SUCCESS(status)) return status;
    if (dev->Bus.Header.Version != OA2DP_BUS_VERSION || dev->Bus.Header.Size != sizeof(dev->Bus) ||
        !dev->Bus.Header.InterfaceReference || !dev->Bus.Header.InterfaceDereference ||
        !dev->Bus.GetVolume || !dev->Bus.SetVolume || !dev->Bus.SetConnected ||
        !dev->Bus.RequestConnection || !dev->Bus.SetPlaying || !dev->Bus.SetSuspended ||
        !dev->Bus.RegisterAudio || !dev->Bus.UnregisterAudio) {
        if (dev->Bus.Header.InterfaceDereference)
            dev->Bus.Header.InterfaceDereference(dev->Bus.Header.Context);
        RtlZeroMemory(&dev->Bus, sizeof(dev->Bus));
        return STATUS_REVISION_MISMATCH;
    }
    return STATUS_SUCCESS;
}

static VOID ReleaseVolumeBus(_Inout_ OA2DP_AUDIO_DEVICE *dev)
{
    if (dev->Bus.Header.InterfaceDereference)
        dev->Bus.Header.InterfaceDereference(dev->Bus.Header.Context);
    RtlZeroMemory(&dev->Bus, sizeof(dev->Bus));
}

static NTSTATUS
InitDevice
(
    _Inout_ OA2DP_AUDIO_DEVICE *dev,
    _In_    PDEVICE_OBJECT      DeviceObject
)
{
    PAGED_CODE();

    NTSTATUS            ntStatus;
    OA2DP_FORMAT_LIST   list;
    const OA2DP_FORMAT *f;

    dev->Fdo = DeviceObject;
    ntStatus = PcGetPhysicalDeviceObject(DeviceObject, &dev->Pdo);
    if (!NT_SUCCESS(ntStatus))
    {
        return ntStatus;
    }

    Oa2dpControlInit(dev);
    KeInitializeEvent(&dev->FilterLock, SynchronizationEvent, TRUE);
    ntStatus = QueryVolumeBus(dev);
    if (!NT_SUCCESS(ntStatus))
        return ntStatus;

    // Last formats and name the service gave us, if any.
    Oa2dpLoadSettings(dev);
    Oa2dpGetFormats(dev, &list);
    f = &list.Formats[list.Preferred];
    dev->Current = *f;

    ntStatus = Oa2dpRingCreate(&dev->Ring, f->SampleRate, f->Channels,
                               oa2dp_pcm_format_ring_type(f));
    if (!NT_SUCCESS(ntStatus))
    {
        ReleaseVolumeBus(dev);
        return ntStatus;
    }

    dev->Initialized = TRUE;
    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
StartDevice
(
    _In_  PDEVICE_OBJECT          DeviceObject,
    _In_  PIRP                    Irp,
    _In_  PRESOURCELIST           ResourceList
)
/*++

Routine Description:

  This function is called by the operating system when the device is
  started. It is responsible for starting the miniports.

--*/
{
    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(ResourceList);

    PAGED_CODE();

    ASSERT(DeviceObject);

    NTSTATUS            ntStatus = STATUS_SUCCESS;
    OA2DP_AUDIO_DEVICE *dev = OA2DP_DEVICE(DeviceObject);

    if (!dev->Initialized)
    {
        ntStatus = InitDevice(dev, DeviceObject);
        if (!NT_SUCCESS(ntStatus))
        {
            return ntStatus;
        }
    }

    // Install wave+topology filters for the render endpoint.
    Oa2dpLock(&dev->FilterLock);
    ntStatus = InstallFilters(dev);
    Oa2dpUnlock(&dev->FilterLock);

    if (NT_SUCCESS(ntStatus))
    {
        ntStatus = Oa2dpControlStart(dev);
    }

    if (!NT_SUCCESS(ntStatus))
    {
        Oa2dpLock(&dev->FilterLock);
        RemoveFilters(dev);
        Oa2dpUnlock(&dev->FilterLock);
    }

    return ntStatus;
} // StartDevice

//=============================================================================
#pragma code_seg("PAGE")
static VOID
StopDevice
(
    _Inout_ OA2DP_AUDIO_DEVICE *dev
)
{
    PAGED_CODE();

    Oa2dpControlStop(dev);
    Oa2dpLock(&dev->FilterLock);
    RemoveFilters(dev);
    Oa2dpUnlock(&dev->FilterLock);
}

#pragma code_seg()
static VOID
RemoveDevice
(
    _Inout_ OA2DP_AUDIO_DEVICE *dev
)
{

    KIRQL irql;

    // Reject new control operations before device removal.
    KeAcquireSpinLock(&dev->Lock, &irql);
    dev->Removed = TRUE;
    KeReleaseSpinLock(&dev->Lock, irql);
    // Unmaps the service's view of the ring (even from another process) and
    // frees it.
    Oa2dpControlRemove(dev);
    ReleaseVolumeBus(dev);

}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
PnpHandler
(
    _In_ DEVICE_OBJECT *_DeviceObject,
    _Inout_ IRP *_Irp
)
/*++

Routine Description:

  Handles PnP IRPs

--*/
{
    IO_STACK_LOCATION      *stack;
    OA2DP_AUDIO_DEVICE     *dev;

    // This method will be called in IRQL PASSIVE_LEVEL
#pragma warning(suppress: 28118)
    PAGED_CODE();

    ASSERT(_DeviceObject);
    ASSERT(_Irp);

    stack = IoGetCurrentIrpStackLocation(_Irp);
    dev = OA2DP_DEVICE(_DeviceObject);

    if (dev->Initialized)
    {
        switch (stack->MinorFunction)
        {
        case IRP_MN_QUERY_PNP_DEVICE_STATE:
            // A persistent wireless endpoint is present even without a link.
            // Jack events alone do not update PnP's device/container state.
            // Add our flag without erasing another driver's state bits; each
            // query starts with the stack's current answer, not our old flag.
            if (!InterlockedCompareExchange(&dev->Connected, 0, 0))
                _Irp->IoStatus.Information |= PNP_DEVICE_DISCONNECTED;
            _Irp->IoStatus.Status = STATUS_SUCCESS;
            break;

        case IRP_MN_STOP_DEVICE:
        case IRP_MN_SURPRISE_REMOVAL:
            StopDevice(dev);
            break;

        case IRP_MN_REMOVE_DEVICE:
            StopDevice(dev);
            RemoveDevice(dev);
            break;

        default:
            break;
        }
    }

    return PcDispatchIrp(_DeviceObject, _Irp);
}
#pragma code_seg()
