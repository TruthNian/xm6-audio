/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    mintopo.cpp

Abstract:

    Implementation of topology miniport.

--*/
/*
 * Derived from the SYSVAD sample (Windows-driver-samples/audio/sysvad,
 * commit 2dc3fd3a, MS-PL — see LICENSE.MS-PL in this folder): mintopo.cpp
 * and basetopo.cpp. Modified for OpenA2DP: a headphone topology with hardware
 * volume/mute backed by the parent A2DP volume cache and Windows' AVRCP.
 */
#include "driver.h"
static NTSTATUS PropertyHandler_Jack(_In_ PPCPROPERTY_REQUEST Request);
static NTSTATUS EventHandler_Jack(_In_ PPCEVENT_REQUEST Request);
static NTSTATUS PropertyHandler_Bluetooth(_In_ PPCPROPERTY_REQUEST Request);
static NTSTATUS PropertyHandler_Volume(_In_ PPCPROPERTY_REQUEST Request);
static NTSTATUS EventHandler_Volume(_In_ PPCEVENT_REQUEST Request);
#include "toptable.h"

class CMiniportTopology : public IMiniportTopology, public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportTopology);
    ~CMiniportTopology()
    {
        if (Attached) VolumeBus.UnregisterAudio(VolumeBus.Header.Context, this);
        SAFE_RELEASE(Events);
        if (VolumeBus.Header.InterfaceDereference)
            VolumeBus.Header.InterfaceDereference(VolumeBus.Header.Context);
    }

    OA2DP_AUDIO_DEVICE *Device = NULL;
    PPORTEVENTS Events = NULL;
    OA2DP_BUS_INTERFACE VolumeBus{};
    BOOLEAN Attached = FALSE;
    static VOID VolumeChanged(PVOID context);
    static NTSTATUS PolicyRequested(PVOID context, BOOLEAN enabled, BOOLEAN suspended,
                                    ULONG suspendSerial);

    IMP_IMiniportTopology;
};

#pragma code_seg()
VOID CMiniportTopology::VolumeChanged(PVOID context)
{
    CMiniportTopology *mini = static_cast<CMiniportTopology *>(context);
    if (mini->Events) {
        mini->Events->GenerateEventList((GUID *)&KSEVENTSETID_AudioControlChange,
            KSEVENT_CONTROL_CHANGE, FALSE, 0, TRUE, 0);
        mini->Events->GenerateEventList((GUID *)&KSEVENTSETID_AudioControlChange,
            KSEVENT_CONTROL_CHANGE, FALSE, 0, TRUE, 1);
    }
}

NTSTATUS CMiniportTopology::PolicyRequested(PVOID context, BOOLEAN enabled, BOOLEAN suspended,
                                            ULONG suspendSerial)
{
    return Oa2dpApplyPolicy(static_cast<CMiniportTopology *>(context)->Device, enabled, suspended,
                            suspendSerial);
}

#pragma code_seg("PAGE")
NTSTATUS CreateMiniportTopology(_Out_ PUNKNOWN *Out, _In_ OA2DP_AUDIO_DEVICE *Device)
{
    PAGED_CODE();

    CMiniportTopology *obj = new (POOL_FLAG_NON_PAGED, OA2DP_AUDIO_TAG) CMiniportTopology(NULL);
    if (obj == NULL)
    {
        *Out = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    obj->AddRef();
    obj->Device = Device;
    *Out = PUNKNOWN(PMINIPORTTOPOLOGY(obj));
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportTopology::NonDelegatingQueryInterface
(
    _In_ REFIID Interface,
    _COM_Outptr_ PVOID *Object
)
{
    PAGED_CODE();
    ASSERT(Object);

    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = PVOID(PUNKNOWN(PMINIPORTTOPOLOGY(this)));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniport))
    {
        *Object = PVOID(PMINIPORT(this));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportTopology))
    {
        *Object = PVOID(PMINIPORTTOPOLOGY(this));
    }
    else
    {
        *Object = NULL;
    }

    if (*Object)
    {
        PUNKNOWN(*Object)->AddRef();
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_PARAMETER;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportTopology::Init
(
    _In_ PUNKNOWN       UnknownAdapter,
    _In_ PRESOURCELIST  ResourceList,
    _In_ PPORTTOPOLOGY  Port
)
{
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    PAGED_CODE();
    NTSTATUS status = Port->QueryInterface(IID_IPortEvents, (PVOID *)&Events);
    if (!NT_SUCCESS(status)) return status;
    VolumeBus = Device->Bus;
    VolumeBus.Header.InterfaceReference(VolumeBus.Header.Context);
    status = VolumeBus.RegisterAudio(VolumeBus.Header.Context, this,
                                    VolumeChanged, PolicyRequested);
    Attached = NT_SUCCESS(status);
    return status;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportTopology::GetDescription
(
    _Out_ PPCFILTER_DESCRIPTOR *OutFilterDescriptor
)
{
    PAGED_CODE();
    ASSERT(OutFilterDescriptor);
    *OutFilterDescriptor = &Oa2dpTopoMiniportFilterDescriptor;
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportTopology::DataRangeIntersection
(
    _In_        ULONG           PinId,
    _In_        PKSDATARANGE    ClientDataRange,
    _In_        PKSDATARANGE    MyDataRange,
    _In_        ULONG           OutputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength)
                PVOID           ResultantFormat,
    _Out_       PULONG          ResultantFormatLength
)
{
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(ClientDataRange);
    UNREFERENCED_PARAMETER(MyDataRange);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(ResultantFormat);
    UNREFERENCED_PARAMETER(ResultantFormatLength);
    PAGED_CODE();
    // Bridge pins only: let the class handler do it.
    return STATUS_NOT_IMPLEMENTED;
}
#pragma code_seg()

static NTSTATUS PropertyHandler_Jack(_In_ PPCPROPERTY_REQUEST request)
{
    PAGED_CODE();
    CMiniportTopology *mini = reinterpret_cast<CMiniportTopology *>(request->MajorTarget);
    if (!mini || !mini->Device)
        return STATUS_INVALID_DEVICE_STATE;
    ULONG id = request->PropertyItem->Id;
    if (id != KSPROPERTY_JACK_DESCRIPTION && id != KSPROPERTY_JACK_DESCRIPTION2 &&
        id != KSPROPERTY_JACK_CONTAINERID)
        return STATUS_INVALID_DEVICE_REQUEST;
    if (request->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
        ULONG flags = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT;
        if (request->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION)) {
            PKSPROPERTY_DESCRIPTION desc = (PKSPROPERTY_DESCRIPTION)request->Value;
            RtlZeroMemory(desc, sizeof(*desc));
            desc->AccessFlags = flags;
            desc->DescriptionSize = sizeof(*desc);
            desc->PropTypeSet.Set = KSPROPTYPESETID_General;
            desc->PropTypeSet.Id = id == KSPROPERTY_JACK_CONTAINERID ? VT_CLSID : VT_ILLEGAL;
            request->ValueSize = sizeof(*desc);
            return STATUS_SUCCESS;
        }
        if (request->ValueSize < sizeof(ULONG)) {
            request->ValueSize = 0;
            return STATUS_BUFFER_TOO_SMALL;
        }
        *(PULONG)request->Value = flags;
        request->ValueSize = sizeof(ULONG);
        return STATUS_SUCCESS;
    }
    if (!(request->Verb & KSPROPERTY_TYPE_GET) || request->InstanceSize < sizeof(ULONG) ||
        *(PULONG)request->Instance != OA2DP_TOPO_PIN_OUT)
        return STATUS_INVALID_PARAMETER;
    if (id == KSPROPERTY_JACK_CONTAINERID) {
        if (!mini->Device->ContainerIdValid)
            return STATUS_NOT_FOUND;
        if (!request->ValueSize) {
            request->ValueSize = sizeof(GUID);
            return STATUS_BUFFER_OVERFLOW;
        }
        if (request->ValueSize < sizeof(GUID)) {
            request->ValueSize = 0;
            return STATUS_BUFFER_TOO_SMALL;
        }
        *(GUID *)request->Value = mini->Device->ContainerId;
        request->ValueSize = sizeof(GUID);
        return STATUS_SUCCESS;
    }
    ULONG required = sizeof(KSMULTIPLE_ITEM) +
        (id == KSPROPERTY_JACK_DESCRIPTION ? sizeof(KSJACK_DESCRIPTION) : sizeof(KSJACK_DESCRIPTION2));
    if (!request->ValueSize) {
        request->ValueSize = required;
        return STATUS_BUFFER_OVERFLOW;
    }
    if (request->ValueSize < required) {
        request->ValueSize = 0;
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlZeroMemory(request->Value, required);
    PKSMULTIPLE_ITEM items = (PKSMULTIPLE_ITEM)request->Value;
    items->Size = required;
    items->Count = 1;
    if (id == KSPROPERTY_JACK_DESCRIPTION) {
        PKSJACK_DESCRIPTION jack = (PKSJACK_DESCRIPTION)(items + 1);
        jack->ChannelMapping = KSAUDIO_SPEAKER_STEREO;
        jack->ConnectionType = eConnTypeOtherDigital;
        jack->GeoLocation = eGeoLocNotApplicable;
        jack->GenLocation = eGenLocOther;
        jack->PortConnection = ePortConnUnknown;
        jack->IsConnected = InterlockedCompareExchange(&mini->Device->Connected, 0, 0) != 0;
    } else {
        PKSJACK_DESCRIPTION2 jack = (PKSJACK_DESCRIPTION2)(items + 1);
        jack->JackCapabilities = JACKDESC2_PRESENCE_DETECT_CAPABILITY;
    }
    request->ValueSize = required;
    return STATUS_SUCCESS;
}

static NTSTATUS PropertyHandler_Bluetooth(_In_ PPCPROPERTY_REQUEST request)
{
    PAGED_CODE();
    CMiniportTopology *mini = reinterpret_cast<CMiniportTopology *>(request->MajorTarget);
    return mini ? Oa2dpBluetoothProperty(mini->Device, request) : STATUS_INVALID_DEVICE_STATE;
}

NTSTATUS Oa2dpBluetoothProperty(_Inout_ OA2DP_AUDIO_DEVICE *dev,
                               _In_ PPCPROPERTY_REQUEST request)
{
    PAGED_CODE();
    if (!dev)
        return STATUS_INVALID_DEVICE_STATE;
    const ULONG id = request->PropertyItem->Id;
    if (id != KSPROPERTY_ONESHOT_RECONNECT && id != KSPROPERTY_ONESHOT_DISCONNECT)
        return STATUS_INVALID_DEVICE_REQUEST;
    // These commands are GET requests with no property value, per ksmedia.h.
    if (request->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
        const ULONG flags = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT;
        if (request->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION)) {
            PKSPROPERTY_DESCRIPTION desc = (PKSPROPERTY_DESCRIPTION)request->Value;
            RtlZeroMemory(desc, sizeof(*desc));
            desc->AccessFlags = flags;
            desc->DescriptionSize = sizeof(*desc);
            desc->PropTypeSet.Set = KSPROPTYPESETID_General;
            desc->PropTypeSet.Id = VT_ILLEGAL;
            request->ValueSize = sizeof(*desc);
            return STATUS_SUCCESS;
        }
        if (request->ValueSize < sizeof(ULONG)) {
            request->ValueSize = 0;
            return STATUS_BUFFER_TOO_SMALL;
        }
        *(PULONG)request->Value = flags;
        request->ValueSize = sizeof(ULONG);
        return STATUS_SUCCESS;
    }
    if (!(request->Verb & KSPROPERTY_TYPE_GET) || request->InstanceSize != 0)
        return STATUS_INVALID_PARAMETER;
    request->ValueSize = 0;
    NTSTATUS status = dev->Bus.RequestConnection(dev->Bus.Header.Context,
        id == KSPROPERTY_ONESHOT_RECONNECT);
    if (NT_SUCCESS(status) && id == KSPROPERTY_ONESHOT_DISCONNECT) {
        // A successfully queued command is not a completed disconnect. Keep
        // the KS request alive until the transport really closes AVDTP. Both
        // interfaces wait on that one referenced event, not a jack-state copy.
        LARGE_INTEGER timeout;
        timeout.QuadPart = -10LL * 1000 * 1000 * 10;
        if (!dev->Bus.DisconnectedEvent) return STATUS_DEVICE_NOT_READY;
        status = KeWaitForSingleObject(dev->Bus.DisconnectedEvent, Executive,
            KernelMode, FALSE, &timeout);
        if (status == STATUS_TIMEOUT) status = STATUS_IO_TIMEOUT;
    }
    return status;
}

static NTSTATUS PropertyHandler_Volume(_In_ PPCPROPERTY_REQUEST request)
{
    PAGED_CODE();
    CMiniportTopology *mini = reinterpret_cast<CMiniportTopology *>(request->MajorTarget);
    if (!mini || !mini->Attached) return STATUS_DEVICE_NOT_READY;
    const BOOLEAN volume = request->Node == 0 && request->PropertyItem->Id == KSPROPERTY_AUDIO_VOLUMELEVEL;
    const BOOLEAN mute = request->Node == 1 && request->PropertyItem->Id == KSPROPERTY_AUDIO_MUTE;
    const BOOLEAN cpu = request->Node < 2 && request->PropertyItem->Id == KSPROPERTY_AUDIO_CPU_RESOURCES;
    if (!volume && !mute && !cpu) return STATUS_INVALID_DEVICE_REQUEST;
    if (request->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
        const ULONG flags = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT |
                            (cpu ? 0 : KSPROPERTY_TYPE_SET);
        const ULONG full = sizeof(KSPROPERTY_DESCRIPTION) +
            (cpu ? 0 : sizeof(KSPROPERTY_MEMBERSHEADER) + 2 * sizeof(KSPROPERTY_STEPPING_LONG));
        if (request->ValueSize < sizeof(ULONG)) {
            request->ValueSize = 0;
            return STATUS_BUFFER_TOO_SMALL;
        }
        if (request->ValueSize < sizeof(KSPROPERTY_DESCRIPTION)) {
            *(PULONG)request->Value = flags;
            request->ValueSize = sizeof(ULONG);
            return STATUS_SUCCESS;
        }
        auto desc = static_cast<PKSPROPERTY_DESCRIPTION>(request->Value);
        RtlZeroMemory(desc, sizeof(*desc));
        desc->AccessFlags = flags;
        desc->DescriptionSize = full;
        desc->PropTypeSet.Set = KSPROPTYPESETID_General;
        desc->PropTypeSet.Id = mute ? VT_BOOL : VT_I4;
        desc->MembersListCount = cpu ? 0 : 1;
        if (!cpu && request->ValueSize >= full) {
            auto members = reinterpret_cast<PKSPROPERTY_MEMBERSHEADER>(desc + 1);
            RtlZeroMemory(members, sizeof(*members));
            members->MembersFlags = KSPROPERTY_MEMBER_STEPPEDRANGES;
            members->MembersSize = sizeof(KSPROPERTY_STEPPING_LONG);
            members->MembersCount = 2;
            members->Flags = KSPROPERTY_MEMBER_FLAG_BASICSUPPORT_MULTICHANNEL |
                             KSPROPERTY_MEMBER_FLAG_BASICSUPPORT_UNIFORM;
            auto range = reinterpret_cast<PKSPROPERTY_STEPPING_LONG>(members + 1);
            RtlZeroMemory(range, 2 * sizeof(*range));
            for (ULONG channel = 0; channel < 2; ++channel) {
                range[channel].SteppingDelta = volume ? OA2DP_VOLUME_STEP_DB : 1;
                range[channel].Bounds.SignedMinimum = volume ? OA2DP_VOLUME_MIN_DB : 0;
                range[channel].Bounds.SignedMaximum = volume ? OA2DP_VOLUME_MAX_DB : 1;
            }
            request->ValueSize = full;
        } else {
            request->ValueSize = sizeof(*desc);
        }
        return STATUS_SUCCESS;
    }
    if (cpu) {
        if (!(request->Verb & KSPROPERTY_TYPE_GET)) return STATUS_INVALID_DEVICE_REQUEST;
        if (request->ValueSize < sizeof(ULONG)) {
            request->ValueSize = sizeof(ULONG);
            return STATUS_BUFFER_TOO_SMALL;
        }
        *(PULONG)request->Value = KSAUDIO_CPU_RESOURCES_NOT_HOST_CPU;
        request->ValueSize = sizeof(ULONG);
        return STATUS_SUCCESS;
    }
    if (request->InstanceSize < sizeof(ULONG)) return STATUS_INVALID_PARAMETER;
    const ULONG channel = *(PULONG)request->Instance;
    if (channel != ULONG(-1) && channel > 1) return STATUS_INVALID_PARAMETER;
    if (!request->ValueSize) {
        request->ValueSize = sizeof(LONG);
        return STATUS_BUFFER_OVERFLOW;
    }
    if (request->ValueSize < sizeof(LONG)) {
        request->ValueSize = 0;
        return STATUS_BUFFER_TOO_SMALL;
    }
    LONG db;
    BOOLEAN muted;
    mini->VolumeBus.GetVolume(mini->VolumeBus.Header.Context, &db, &muted);
    if (request->Verb & KSPROPERTY_TYPE_GET) {
        *(PLONG)request->Value = volume ? db : (muted ? TRUE : FALSE);
    } else if (request->Verb & KSPROPERTY_TYPE_SET) {
        const LONG value = *(PLONG)request->Value;
        if (volume) {
            db = value;
            muted = db <= OA2DP_VOLUME_MIN_DB;
        } else {
            muted = value != 0;
        }
        mini->VolumeBus.SetVolume(mini->VolumeBus.Header.Context, db, muted, TRUE);
    } else {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    request->ValueSize = sizeof(LONG);
    return STATUS_SUCCESS;
}

static NTSTATUS EventHandler_Volume(_In_ PPCEVENT_REQUEST request)
{
    CMiniportTopology *mini = reinterpret_cast<CMiniportTopology *>(request->MajorTarget);
    if (!mini || *request->EventItem->Set != KSEVENTSETID_AudioControlChange ||
        request->EventItem->Id != KSEVENT_CONTROL_CHANGE)
        return STATUS_INVALID_PARAMETER;
    switch (request->Verb) {
    case PCEVENT_VERB_SUPPORT:
    case PCEVENT_VERB_REMOVE:
        return STATUS_SUCCESS;
    case PCEVENT_VERB_ADD:
        if (!request->EventEntry || !mini->Events) return STATUS_INVALID_DEVICE_STATE;
        mini->Events->AddEventToEventList(request->EventEntry);
        return STATUS_SUCCESS;
    default:
        return STATUS_INVALID_PARAMETER;
    }
}

static NTSTATUS EventHandler_Jack(_In_ PPCEVENT_REQUEST request)
{
    CMiniportTopology *mini = reinterpret_cast<CMiniportTopology *>(request->MajorTarget);
    if (!mini || *request->EventItem->Set != KSEVENTSETID_PinCapsChange ||
        request->EventItem->Id != KSEVENT_PINCAPS_JACKINFOCHANGE)
        return STATUS_INVALID_PARAMETER;
    switch (request->Verb) {
    case PCEVENT_VERB_SUPPORT:
    case PCEVENT_VERB_REMOVE:
        return STATUS_SUCCESS;
    case PCEVENT_VERB_ADD:
        if (!request->EventEntry || !mini->Events)
            return STATUS_INVALID_DEVICE_STATE;
        mini->Events->AddEventToEventList(request->EventEntry);
        return STATUS_SUCCESS;
    default:
        return STATUS_INVALID_PARAMETER;
    }
}

extern "C" NTSTATUS Oa2dpSetConnected(_Inout_ OA2DP_AUDIO_DEVICE *dev, _In_ BOOLEAN connected,
                                     ULONG connection)
{
    PAGED_CODE();
    Oa2dpLock(&dev->ConnectionLock);
    KIRQL irql;
    KeAcquireSpinLock(&dev->Lock, &irql);
    NTSTATUS status = connected ? Oa2dpControlStatusLocked(dev, FALSE) : STATUS_SUCCESS;
    if (NT_SUCCESS(status) && connected && !dev->ConnectionEnabled)
        status = STATUS_CONNECTION_DISCONNECTED;
    if (!NT_SUCCESS(status)) {
        KeReleaseSpinLock(&dev->Lock, irql);
        Oa2dpUnlock(&dev->ConnectionLock);
        return status;
    }
    KeReleaseSpinLock(&dev->Lock, irql);
    if (dev->Bus.SetConnected) {
        status = dev->Bus.SetConnected(dev->Bus.Header.Context, connected, connection);
        if (!NT_SUCCESS(status) && connected) {
            Oa2dpUnlock(&dev->ConnectionLock);
            return status;
        }
    }
    // The transport validates completion first. Failed/unfinished channel
    // closes cannot acknowledge the KS disconnect. An unavailable encoder may
    // still unplug its audio jack before the physical transport finishes.
    LONG value = connected ? 1 : 0;
    const BOOLEAN changed = InterlockedExchange(&dev->Connected, value) != value;
    if (!changed) {
        Oa2dpUnlock(&dev->ConnectionLock);
        return status;
    }
    PPORTEVENTS events = NULL;
    Oa2dpLock(&dev->FilterLock);
    if (dev->TopoPort)
        (void)static_cast<PUNKNOWN>(dev->TopoPort)->QueryInterface(IID_IPortEvents, (PVOID *)&events);
    Oa2dpUnlock(&dev->FilterLock);
    if (events) {
        events->GenerateEventList((GUID *)&KSEVENTSETID_PinCapsChange,
            KSEVENT_PINCAPS_JACKINFOCHANGE, TRUE, OA2DP_TOPO_PIN_OUT, FALSE, ULONG(-1));
        events->Release();
    }
    Oa2dpUnlock(&dev->ConnectionLock);
    return status;
}
