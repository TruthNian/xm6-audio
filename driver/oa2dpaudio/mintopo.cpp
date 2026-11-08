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
 * and basetopo.cpp. Modified for OpenA2DP: a fixed two-pin topology
 * (wave bridge in, headphones out) with no nodes; volume and mute are left
 * to the audio engine's software endpoint volume.
 */
#include "driver.h"
static NTSTATUS PropertyHandler_Jack(_In_ PPCPROPERTY_REQUEST Request);
static NTSTATUS EventHandler_Jack(_In_ PPCEVENT_REQUEST Request);
#include "toptable.h"

class CMiniportTopology : public IMiniportTopology, public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportTopology);
    ~CMiniportTopology() { SAFE_RELEASE(Events); }

    OA2DP_AUDIO_DEVICE *Device = NULL;
    PPORTEVENTS Events = NULL;

    IMP_IMiniportTopology;
};

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
    if (!NT_SUCCESS(Port->QueryInterface(IID_IPortEvents, (PVOID *)&Events)))
        Events = NULL;
    return STATUS_SUCCESS;
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
    if (id != KSPROPERTY_JACK_DESCRIPTION && id != KSPROPERTY_JACK_DESCRIPTION2)
        return STATUS_INVALID_DEVICE_REQUEST;
    if (request->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
        ULONG flags = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT;
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
    if (!(request->Verb & KSPROPERTY_TYPE_GET) || request->InstanceSize < sizeof(ULONG) ||
        *(PULONG)request->Instance != OA2DP_TOPO_PIN_OUT)
        return STATUS_INVALID_PARAMETER;
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

extern "C" VOID Oa2dpSetConnected(_Inout_ OA2DP_AUDIO_DEVICE *dev, _In_ BOOLEAN connected)
{
    PAGED_CODE();
    LONG value = connected ? 1 : 0;
    if (InterlockedExchange(&dev->Connected, value) == value)
        return;
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
}
