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
#include "../common/channel_lifecycle.h"
#include "../common/mpm_lifecycle.h"
#include "../common/server_lifecycle.h"

#define OA2DP_POOL_TAG 'pd2A'
#define OA2DP_AVDTP_PSM 0x0019
#define OA2DP_DEFAULT_SIGNALING_MTU 672
#define OA2DP_DEFAULT_MEDIA_MTU 1005
#define OA2DP_INCOMING_QUEUE_DEPTH 50
#define OA2DP_EVENT_CAPACITY 32

typedef struct _OA2DP_DEVICE_CONTEXT OA2DP_DEVICE_CONTEXT;

typedef struct _OA2DP_CHANNEL {
    OA2DP_DEVICE_CONTEXT *Dev;
    OA2DP_CHANNEL_KIND Kind;
    OA2DP_CHANNEL_LIFECYCLE Life;
    L2CAP_CHANNEL_HANDLE Handle;
    USHORT OutMtu;
    USHORT InMtu;
    KEVENT OpenFinished;
    KEVENT SubmissionsIdle;
    WDFWAITLOCK CloseLock;
    WDFWORKITEM CloseWork;
} OA2DP_CHANNEL;

struct _OA2DP_DEVICE_CONTEXT {
    WDFDEVICE Device;
    WDFIOTARGET IoTarget;
    BTH_PROFILE_DRIVER_INTERFACE Profile;
    BOOLEAN ProfileAcquired; /* one imported reference for this WDFDEVICE lifetime */
    BTH_ADDR LocalAddress;
    BTH_ADDR RemoteAddress;

    WDFSPINLOCK Lock;              /* protects Channels[] state */
    OA2DP_CHANNEL Channels[Oa2dpChannelCount];
    KEVENT ResponsesIdle;
    ULONG ResponseSubmissions;    /* peer responses admitted before suspend */

    WDFQUEUE DefaultQueue;
    WDFQUEUE EventQueue;           /* pended WAIT_EVENT requests */
    OA2DP_EVENT Events[OA2DP_EVENT_CAPACITY]; /* protected by Lock */
    ULONG EventHead;
    ULONG EventCount;
    BOOLEAN EventOverflow;

    OA2DP_SERVER_REGISTRATION Server;

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
    KEVENT DisconnectedEvent;     /* actual AVDTP teardown, not command receipt */
    WDFWAITLOCK PublicationLock;   /* PASSIVE_LEVEL external state publication */
    WDFWORKITEM PublicationWork;
    ULONG PublishedConnection;
    WDFSTRING AudioInterfacePaths[2]; /* audio + Windows Bluetooth-audio category */
    PVOID AudioContext;
    OA2DP_VOLUME_NOTIFY AudioVolumeNotify;
    OA2DP_POLICY_REQUEST AudioPolicyRequest;
    PVOID volatile MpmHandle;
    EX_RUNDOWN_REF MpmCalls;
    EX_RUNDOWN_REF MpmCallbacks;
    BOOLEAN MpmStopping;
    BOOLEAN TransportStopping;
    BOOLEAN PowerSuspended;
    BOOLEAN PowerQuiesced;
    ULONG PowerSerial;
    BOOLEAN SurpriseRemoved;
    BOOLEAN PolicyConnected;
    BOOLEAN PolicySuspended;
    BOOLEAN MpmConnected;
    OA2DP_MPM_REQUEST MpmRequest;
    OA2DP_MPM_REQUEST MpmSuspendRequest;
    BOOLEAN MpmIsSuspended;
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
    ULONG Generation;
    ULONG PolicySerial;      /* owning Windows connect request, if any */
    WDFREQUEST UserRequest;   /* exact outgoing caller; never a replacement channel's request */
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
EVT_WDF_DEVICE_CONTEXT_DESTROY Oa2dpEvtDeviceDestroy;
EVT_WDF_DEVICE_SELF_MANAGED_IO_SUSPEND Oa2dpEvtIoSuspend;
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT Oa2dpEvtIoInit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_RESTART Oa2dpEvtIoRestart;
EVT_WDF_DEVICE_SURPRISE_REMOVAL Oa2dpEvtSurpriseRemoval;
EVT_WDF_FILE_CLEANUP Oa2dpEvtFileCleanup;
EVT_WDF_DEVICE_FILE_CREATE Oa2dpEvtFileCreate;
EVT_WDFDEVICE_WDM_IRP_PREPROCESS Oa2dpVolumePreprocess;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL Oa2dpEvtIoDeviceControl;

/* l2cap.c */
NTSTATUS Oa2dpSubmitBrbSync(_In_ OA2DP_DEVICE_CONTEXT *dev, _In_ struct _BRB *brb, _In_ ULONG size);
NTSTATUS Oa2dpQueryLocalAddress(_In_ OA2DP_DEVICE_CONTEXT *dev);
NTSTATUS Oa2dpQueryRemoteAddress(_In_ OA2DP_DEVICE_CONTEXT *dev);
NTSTATUS Oa2dpRegisterServer(_In_ OA2DP_DEVICE_CONTEXT *dev);
NTSTATUS Oa2dpInitializeChannels(OA2DP_DEVICE_CONTEXT *dev);
NTSTATUS Oa2dpUnregisterServer(_In_ OA2DP_DEVICE_CONTEXT *dev);
VOID Oa2dpOpenChannel(_In_ OA2DP_DEVICE_CONTEXT *dev, _In_ WDFREQUEST request,
                      _In_ const OA2DP_OPEN_CHANNEL_IN *in);
NTSTATUS Oa2dpCloseChannel(_In_ OA2DP_DEVICE_CONTEXT *dev, _In_ OA2DP_CHANNEL_KIND kind);
NTSTATUS Oa2dpCloseConnectionChannel(OA2DP_DEVICE_CONTEXT *dev, OA2DP_CHANNEL_KIND kind,
                                    ULONG expectedConnection, PULONG closedConnection);
VOID Oa2dpCloseAllChannels(_In_ OA2DP_DEVICE_CONTEXT *dev);
VOID Oa2dpTransfer(_In_ OA2DP_DEVICE_CONTEXT *dev, _In_ WDFREQUEST request,
                   _In_ const OA2DP_SEND_IN *in, _In_ BOOLEAN send);

/* driver.c */
VOID Oa2dpPostEvent(_In_ OA2DP_DEVICE_CONTEXT *dev, _In_ ULONG type,
                   _In_opt_ OA2DP_CHANNEL *ch, ULONG generation);
VOID Oa2dpQueueEventLocked(OA2DP_DEVICE_CONTEXT *dev, ULONG type,
                           OA2DP_CHANNEL *ch, ULONG generation);
VOID Oa2dpDeliverEvents(OA2DP_DEVICE_CONTEXT *dev);
NTSTATUS Oa2dpCreateAudioChild(_In_ OA2DP_DEVICE_CONTEXT *dev);
NTSTATUS Oa2dpRegisterVolumeInterface(_In_ OA2DP_DEVICE_CONTEXT *dev);
NTSTATUS Oa2dpRegisterBusInterface(_In_ OA2DP_DEVICE_CONTEXT *dev, _In_ WDFDEVICE child);
VOID Oa2dpVolumeFileCleanup(_In_ OA2DP_DEVICE_CONTEXT *dev, _In_ WDFFILEOBJECT file);
NTSTATUS Oa2dpInitializePublication(OA2DP_DEVICE_CONTEXT *dev);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS Oa2dpPublishConnected(_In_ OA2DP_DEVICE_CONTEXT *dev, _In_ BOOLEAN connected,
                               ULONG connection);
/* mpm.c: observed Windows 10 profile-manager compatibility contract. */
NTSTATUS Oa2dpMpmStart(OA2DP_DEVICE_CONTEXT *dev);
VOID Oa2dpMpmStop(OA2DP_DEVICE_CONTEXT *dev);
NTSTATUS Oa2dpMpmRequestConnection(PVOID context, BOOLEAN enabled);
NTSTATUS Oa2dpMpmConnected(OA2DP_DEVICE_CONTEXT *dev, BOOLEAN connected);
VOID Oa2dpMpmOpenFailed(OA2DP_DEVICE_CONTEXT *dev, ULONG policySerial);
VOID Oa2dpMpmPlaying(PVOID context, BOOLEAN playing);
NTSTATUS Oa2dpMpmSuspended(PVOID context, BOOLEAN suspended, ULONG requestSerial,
                          ULONG connection);
VOID Oa2dpMpmAcknowledgeSuspend(OA2DP_DEVICE_CONTEXT *dev);
