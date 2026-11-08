/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    speakertoptable.h

Abstract:

    Declaration of topology tables.

--*/
/*
 * Derived from the SYSVAD sample (Windows-driver-samples/audio/sysvad,
 * commit 2dc3fd3a, MS-PL — see LICENSE.MS-PL in this folder). Modified for
 * OpenA2DP: headphone output with Bluetooth jack presence notification;
 * resource-group and posture properties removed.
 */
#pragma once

static KSDATARANGE Oa2dpTopoPinDataRangesBridge[] =
{
    {
        sizeof(KSDATARANGE),
        0,
        0,
        0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
    }
};

static PKSDATARANGE Oa2dpTopoPinDataRangePointersBridge[] =
{
    &Oa2dpTopoPinDataRangesBridge[0]
};

static PCPIN_DESCRIPTOR Oa2dpTopoMiniportPins[] =
{
    // OA2DP_TOPO_PIN_IN (from the wave filter)
    {
        0,
        0,
        0,                                                  // InstanceCount
        NULL,                                               // AutomationTable
        {                                                   // KsPinDescriptor
            0,                                              // InterfacesCount
            NULL,                                           // Interfaces
            0,                                              // MediumsCount
            NULL,                                           // Mediums
            RTL_NUMBER_OF(Oa2dpTopoPinDataRangePointersBridge), // DataRangesCount
            Oa2dpTopoPinDataRangePointersBridge,            // DataRanges
            KSPIN_DATAFLOW_IN,                              // DataFlow
            KSPIN_COMMUNICATION_NONE,                       // Communication
            &KSCATEGORY_AUDIO,                              // Category
            NULL,                                           // Name
            0                                               // Reserved
        }
    },
    // OA2DP_TOPO_PIN_OUT (the headset)
    {
        0,
        0,
        0,                                                  // InstanceCount
        NULL,                                               // AutomationTable
        {                                                   // KsPinDescriptor
            0,                                              // InterfacesCount
            NULL,                                           // Interfaces
            0,                                              // MediumsCount
            NULL,                                           // Mediums
            RTL_NUMBER_OF(Oa2dpTopoPinDataRangePointersBridge), // DataRangesCount
            Oa2dpTopoPinDataRangePointersBridge,            // DataRanges
            KSPIN_DATAFLOW_OUT,                             // DataFlow
            KSPIN_COMMUNICATION_NONE,                       // Communication
            &KSNODETYPE_HEADPHONES,                         // Category
            NULL,                                           // Name
            0                                               // Reserved
        }
    }
};

static PCCONNECTION_DESCRIPTOR Oa2dpTopoMiniportConnections[] =
{
    { PCFILTER_NODE, OA2DP_TOPO_PIN_IN, PCFILTER_NODE, OA2DP_TOPO_PIN_OUT }
};

static PCPROPERTY_ITEM Oa2dpJackProperties[] =
{
    { &KSPROPSETID_Jack, KSPROPERTY_JACK_DESCRIPTION,
      KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT, PropertyHandler_Jack },
    { &KSPROPSETID_Jack, KSPROPERTY_JACK_DESCRIPTION2,
      KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT, PropertyHandler_Jack }
};

static PCEVENT_ITEM Oa2dpJackEvents[] =
{
    { &KSEVENTSETID_PinCapsChange, KSEVENT_PINCAPS_JACKINFOCHANGE,
      KSEVENT_TYPE_ENABLE | KSEVENT_TYPE_BASICSUPPORT, EventHandler_Jack }
};

DEFINE_PCAUTOMATION_TABLE_PROP_EVENT(Oa2dpJackAutomation, Oa2dpJackProperties, Oa2dpJackEvents);

static PCFILTER_DESCRIPTOR Oa2dpTopoMiniportFilterDescriptor =
{
    0,                                              // Version
    &Oa2dpJackAutomation,                            // AutomationTable
    sizeof(PCPIN_DESCRIPTOR),                       // PinSize
    RTL_NUMBER_OF(Oa2dpTopoMiniportPins),           // PinCount
    Oa2dpTopoMiniportPins,                          // Pins
    sizeof(PCNODE_DESCRIPTOR),                      // NodeSize
    0,                                              // NodeCount
    NULL,                                           // Nodes
    RTL_NUMBER_OF(Oa2dpTopoMiniportConnections),    // ConnectionCount
    Oa2dpTopoMiniportConnections,                   // Connections
    0,                                              // CategoryCount
    NULL                                            // Categories
};
