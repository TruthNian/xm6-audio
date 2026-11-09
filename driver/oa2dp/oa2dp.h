/* SPDX-License-Identifier: MIT */
/*
 * oa2dp.sys — KMDF Bluetooth profile driver. Installed (per device, by
 * oa2dpctl) on the A2DP Audio Sink service node that BTHENUM creates for a
 * paired headset. It only moves L2CAP SDUs between the headset and
 * oa2dpsvc.exe; all AVDTP logic lives in user mode.
 *
 * Built from Microsoft's public Bluetooth profile driver documentation and
 * the MS-PL "bthecho" WDK sample structure.
 */
#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include <ntintsafe.h>
#include <ntstrsafe.h>
#include <bthdef.h>
#include <bthguid.h>
#include <bthioctl.h>
#include <bthddi.h>

#include "../common/oa2dp_ioctl.h"
#include "../common/oa2dp_bus.h"

#define OA2DP_POOL_TAG 'pd2A'
#define OA2DP_AVDTP_PSM 0x0019
#define OA2DP_DEFAULT_SIGNALING_MTU 672
#define OA2DP_DEFAULT_MEDIA_MTU 1005
#define OA2DP_INCOMING_QUEUE_DEPTH 50
#define OA2DP_EVENT_CAPACITY 32

typedef struct _OA2DP_DEVICE_CONTEXT OA2DP_DEVICE_CONTEXT;

typedef enum _OA2DP_CHANNEL_STATE {
    ChannelClosed = 0,
    ChannelOpening,
    ChannelOpen,
    ChannelClosing,
} OA2DP_CHANNEL_STATE;

typedef struct _OA2DP_CHANNEL {
    OA2DP_DEVICE_CONTEXT *Dev;
    OA2DP_CHANNEL_KIND Kind;
    OA2DP_CHANNEL_STATE State;
    L2CAP_CHANNEL_HANDLE Handle;
    USHORT OutMtu;
    USHORT InMtu;
    WDFREQUEST OpenRequest;  /* user request waiting for an outgoing open */
    struct _BRB *OpenBrb;
} OA2DP_CHANNEL;

struct _OA2DP_DEVICE_CONTEXT {
    WDFDEVICE Device;
    WDFIOTARGET IoTarget;
    BTH_PROFILE_DRIVER_INTERFACE Profile;
    BTH_ADDR LocalAddress;
    BTH_ADDR RemoteAddress;

    WDFSPINLOCK Lock;              /* protects Channels[] state */
    OA2DP_CHANNEL Channels[Oa2dpChannelCount];

    WDFQUEUE DefaultQueue;
    WDFQUEUE EventQueue;           /* pended WAIT_EVENT requests */
    OA2DP_EVENT Events[OA2DP_EVENT_CAPACITY]; /* protected by Lock */
    ULONG EventHead;
    ULONG EventCount;
    BOOLEAN EventOverflow;

    BOOLEAN PsmRegistered;
    L2CAP_SERVER_HANDLE ServerHandle;

    volatile LONG TxQueuedPackets;
    volatile LONG TxQueuedBytes;
    volatile LONG64 TxSent;
    volatile LONG64 TxFailed;

    WDFDEVICE AudioChild;
    WDFFILEOBJECT ServiceFile;
    LIST_ENTRY VolumeFiles;       /* native AVRCP clients, protected by Lock */
    ULONG VolumeFileCount;
    LONG VolumeDb;
    BOOLEAN VolumeMuted;
    PVOID AudioContext;
    OA2DP_VOLUME_NOTIFY AudioVolumeNotify;
    OA2DP_CONNECTION_REQUEST AudioConnectionRequest;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(OA2DP_DEVICE_CONTEXT, Oa2dpGetDevice)

typedef struct _OA2DP_FILE_CONTEXT {
    BOOLEAN VolumeClient;
    BOOLEAN ControlClient;
    BOOLEAN Closing;
    BOOLEAN Listed;
    LIST_ENTRY Entry;
    PKEVENT VolumeEvent;
} OA2DP_FILE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(OA2DP_FILE_CONTEXT, Oa2dpGetFile)

/* Per-request context for BRB-backed transfers. */
typedef struct _OA2DP_REQUEST_CONTEXT {
    struct _BRB *Brb;
    OA2DP_CHANNEL *Channel;
    ULONG Bytes;
} OA2DP_REQUEST_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(OA2DP_REQUEST_CONTEXT, Oa2dpGetRequest)

typedef struct _OA2DP_WORKITEM_CONTEXT {
    OA2DP_CHANNEL *Channel;
} OA2DP_WORKITEM_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(OA2DP_WORKITEM_CONTEXT, Oa2dpGetWorkItem)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD Oa2dpEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE Oa2dpEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE Oa2dpEvtReleaseHardware;
EVT_WDF_FILE_CLEANUP Oa2dpEvtFileCleanup;
EVT_WDF_DEVICE_FILE_CREATE Oa2dpEvtFileCreate;
EVT_WDFDEVICE_WDM_IRP_PREPROCESS Oa2dpVolumePreprocess;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL Oa2dpEvtIoDeviceControl;

/* l2cap.c */
NTSTATUS Oa2dpSubmitBrbSync(_In_ OA2DP_DEVICE_CONTEXT *dev, _In_ struct _BRB *brb, _In_ ULONG size);
NTSTATUS Oa2dpQueryLocalAddress(_In_ OA2DP_DEVICE_CONTEXT *dev);
NTSTATUS Oa2dpQueryRemoteAddress(_In_ OA2DP_DEVICE_CONTEXT *dev);
NTSTATUS Oa2dpRegisterServer(_In_ OA2DP_DEVICE_CONTEXT *dev);
VOID Oa2dpUnregisterServer(_In_ OA2DP_DEVICE_CONTEXT *dev);
VOID Oa2dpOpenChannel(_In_ OA2DP_DEVICE_CONTEXT *dev, _In_ WDFREQUEST request,
                      _In_ const OA2DP_OPEN_CHANNEL_IN *in);
NTSTATUS Oa2dpCloseChannel(_In_ OA2DP_DEVICE_CONTEXT *dev, _In_ OA2DP_CHANNEL_KIND kind);
VOID Oa2dpCloseAllChannels(_In_ OA2DP_DEVICE_CONTEXT *dev);
VOID Oa2dpTransfer(_In_ OA2DP_DEVICE_CONTEXT *dev, _In_ WDFREQUEST request,
                   _In_ OA2DP_CHANNEL_KIND kind, _In_ BOOLEAN send);

/* driver.c */
VOID Oa2dpPostEvent(_In_ OA2DP_DEVICE_CONTEXT *dev, _In_ ULONG type, _In_ OA2DP_CHANNEL *ch);
NTSTATUS Oa2dpCreateAudioChild(_In_ OA2DP_DEVICE_CONTEXT *dev);
NTSTATUS Oa2dpRegisterVolumeInterface(_In_ OA2DP_DEVICE_CONTEXT *dev);
NTSTATUS Oa2dpRegisterBusInterface(_In_ OA2DP_DEVICE_CONTEXT *dev, _In_ WDFDEVICE child);
VOID Oa2dpVolumeFileCleanup(_In_ OA2DP_DEVICE_CONTEXT *dev, _In_ WDFFILEOBJECT file);
VOID Oa2dpPublishConnected(_In_ OA2DP_DEVICE_CONTEXT *dev, _In_ BOOLEAN connected);
