/* SPDX-License-Identifier: MIT */
/*
 * Contract between the kernel drivers and oa2dpsvc.exe. Shared by kernel
 * and user mode; include after <windows.h> or <ntddk.h>/<wdf.h>.
 *
 * Bump OA2DP_IOCTL_VERSION on any incompatible change; the service refuses
 * to talk to a driver with a different version.
 */
#ifndef OA2DP_IOCTL_H
#define OA2DP_IOCTL_H

#define OA2DP_IOCTL_VERSION 11

#include "../../core/include/oa2dp/pcm_format.h"

/* {8EF6C0BE-1B48-409A-A689-9F9FD9E82E12} transport (oa2dp.sys) */
DEFINE_GUID(GUID_DEVINTERFACE_OA2DP_TRANSPORT, 0x8ef6c0be, 0x1b48, 0x409a, 0xa6, 0x89,
            0x9f, 0x9f, 0xd9, 0xe8, 0x2e, 0x12);
/* {14629484-B6DD-41B5-8CAF-B5A4E36A5466} audio endpoint (oa2dpaudio.sys) */
DEFINE_GUID(GUID_DEVINTERFACE_OA2DP_AUDIO, 0x14629484, 0xb6dd, 0x41b5, 0x8c, 0xaf, 0xb5,
            0xa4, 0xe3, 0x6a, 0x54, 0x66);
/* {955E5E3D-C70C-4024-99DB-402F9110DF44} bus interface oa2dp.sys -> child */
DEFINE_GUID(GUID_OA2DP_BUS_INTERFACE, 0x955e5e3d, 0xc70c, 0x4024, 0x99, 0xdb, 0x40, 0x2f,
            0x91, 0x10, 0xdf, 0x44);

#define OA2DP_DEVICE_TYPE 0x8A2D
#define OA2DP_CTL(fn, method) CTL_CODE(OA2DP_DEVICE_TYPE, 0x800 + (fn), method, FILE_ANY_ACCESS)

/* ---- transport (oa2dp.sys) ---------------------------------------------- */

#define IOCTL_OA2DP_GET_INFO      OA2DP_CTL(0, METHOD_BUFFERED)
#define IOCTL_OA2DP_OPEN_CHANNEL  OA2DP_CTL(1, METHOD_BUFFERED)
#define IOCTL_OA2DP_CLOSE_CHANNEL OA2DP_CTL(2, METHOD_BUFFERED)
#define IOCTL_OA2DP_SEND          OA2DP_CTL(3, METHOD_IN_DIRECT)
#define IOCTL_OA2DP_RECV          OA2DP_CTL(4, METHOD_OUT_DIRECT)
#define IOCTL_OA2DP_WAIT_EVENT    OA2DP_CTL(5, METHOD_BUFFERED)
#define IOCTL_OA2DP_GET_TX_STATE  OA2DP_CTL(6, METHOD_BUFFERED)
#define OA2DP_DEVICE_MPM_REGISTERED 0x00000001UL
#define OA2DP_DEVICE_SUSPENDED      0x00000002UL
#define OA2DP_DEVICE_LINK_NOTIFICATIONS 0x00000004UL

typedef enum _OA2DP_CHANNEL_KIND {
    Oa2dpChannelSignaling = 0,
    Oa2dpChannelMedia = 1,
    Oa2dpChannelCount = 2
} OA2DP_CHANNEL_KIND;

typedef struct _OA2DP_DEVICE_INFO {
    ULONG Version;            /* OA2DP_IOCTL_VERSION */
    ULONG Flags;
    ULONG PowerSerial;        /* detects an entire sleep/resume between service wakeups */
    ULONGLONG RemoteAddress;  /* BTH_ADDR of the headset */
    ULONGLONG LocalAddress;   /* BTH_ADDR of the radio */
} OA2DP_DEVICE_INFO;

typedef struct _OA2DP_OPEN_CHANNEL_IN {
    ULONG Kind;               /* OA2DP_CHANNEL_KIND */
    USHORT InMtu;             /* our receive MTU, 0 = default (672 / 1005) */
    USHORT Reserved;
    ULONG ConnectionGeneration; /* MEDIA must belong to this live signalling link; SIGNALING = 0 */
} OA2DP_OPEN_CHANNEL_IN;

typedef struct _OA2DP_CHANNEL_INFO {
    ULONG Kind;
    USHORT OutMtu;            /* negotiated: max SDU we may send */
    USHORT InMtu;
    ULONG Generation;        /* channel incarnation; never a kernel handle */
    ULONG ConnectionGeneration; /* signalling incarnation shared with its media channel */
} OA2DP_CHANNEL_INFO;

typedef struct _OA2DP_CLOSE_CHANNEL_IN {
    ULONG Kind;
    ULONG ConnectionGeneration; /* 0 = cleanup unknown/pending state */
} OA2DP_CLOSE_CHANNEL_IN;
typedef struct _OA2DP_CLOSE_CHANNEL_OUT {
    ULONG ConnectionGeneration; /* closed/requested old epoch, even if a replacement exists */
} OA2DP_CLOSE_CHANNEL_OUT;

/* SEND: input buffer = OA2DP_SEND_IN; output (direct) buffer = SDU bytes.
 * RECV: input buffer = OA2DP_RECV_IN; output (direct) buffer receives one
 *       SDU; Information = bytes. Pends until data arrives; completes with
 *       STATUS_CONNECTION_DISCONNECTED when the channel closes. */
typedef struct _OA2DP_SEND_IN {
    ULONG Kind;
    ULONG Generation;
    ULONG ConnectionGeneration; /* both mandatory; stale SEND/RECV must not use a replacement */
} OA2DP_SEND_IN, OA2DP_RECV_IN;

typedef enum _OA2DP_EVENT_TYPE {
    Oa2dpEventChannelOpened = 1,  /* remote opened a channel to us */
    Oa2dpEventChannelClosed = 2,
    Oa2dpEventDeviceGone = 3,
    Oa2dpEventStateLost = 4,     /* event backlog overflow: reconnect, never guess state */
    Oa2dpEventPowerChanged = 5,  /* query GET_INFO; queued old events must not restore old power state */
    Oa2dpEventLinkAvailable = 6, /* ACL arrival hint; not authority to close or mark a channel open */
} OA2DP_EVENT_TYPE;

typedef struct _OA2DP_EVENT {
    ULONG Type;               /* OA2DP_EVENT_TYPE */
    ULONG Kind;               /* channel kind for channel events */
    OA2DP_CHANNEL_INFO Channel;
} OA2DP_EVENT;

typedef struct _OA2DP_TX_STATE {
    ULONG QueuedPackets;      /* media SDUs submitted and not yet completed */
    ULONG QueuedBytes;
    ULONGLONG SentPackets;
    ULONGLONG FailedPackets;
} OA2DP_TX_STATE;

/* ---- audio endpoint (oa2dpaudio.sys) ------------------------------------- */

#define IOCTL_OA2DP_AUDIO_MAP_RING     OA2DP_CTL(32, METHOD_BUFFERED)
#define IOCTL_OA2DP_AUDIO_UNMAP_RING   OA2DP_CTL(33, METHOD_BUFFERED)
#define IOCTL_OA2DP_AUDIO_SET_FORMATS  OA2DP_CTL(34, METHOD_BUFFERED)
#define IOCTL_OA2DP_AUDIO_SET_LATENCY  OA2DP_CTL(35, METHOD_BUFFERED)
#define IOCTL_OA2DP_AUDIO_WAIT_FORMAT  OA2DP_CTL(36, METHOD_BUFFERED)
#define IOCTL_OA2DP_AUDIO_SET_NAME     OA2DP_CTL(37, METHOD_BUFFERED)
#define IOCTL_OA2DP_AUDIO_SET_CONNECTED OA2DP_CTL(38, METHOD_BUFFERED)
#define IOCTL_OA2DP_AUDIO_GET_STATE     OA2DP_CTL(39, METHOD_BUFFERED)
#define IOCTL_OA2DP_AUDIO_SET_SUSPENDED OA2DP_CTL(40, METHOD_BUFFERED)
#define IOCTL_OA2DP_AUDIO_SET_DATA_EVENT OA2DP_CTL(41, METHOD_BUFFERED)
#define IOCTL_OA2DP_AUDIO_CONSUMER_READY OA2DP_CTL(42, METHOD_BUFFERED)
/* The service admits this PCM epoch only after START and encoder preparation.
 * A successful reply wakes a pending KS PAUSE preparation; it is not audio data. */
typedef struct _OA2DP_CONSUMER_READY_IN {
    ULONG PcmSequence;
    ULONG Reserved;
} OA2DP_CONSUMER_READY_IN;

/* Referenced by the driver until replaced, unmapped or the control file closes.
 * Fixed-width handle permits explicit validation on both architectures. */
typedef struct _OA2DP_DATA_EVENT_IN {
    ULONGLONG EventHandle;    /* 0 unregisters; otherwise an event owned by caller */
    ULONG MinimumFrames;     /* wake when a complete encoder input block exists */
    ULONG Reserved;
} OA2DP_DATA_EVENT_IN;

typedef struct _OA2DP_RING_MAPPING {
    ULONGLONG Address;        /* user-mode address of oa2dp_pcm_ring */
    ULONGLONG Bytes;
} OA2DP_RING_MAPPING;

#define OA2DP_MAX_FORMATS 32
typedef oa2dp_pcm_format OA2DP_FORMAT;

typedef struct _OA2DP_FORMAT_LIST {
    ULONG Count;
    ULONG Preferred;          /* index of the default device format */
    OA2DP_FORMAT Formats[OA2DP_MAX_FORMATS];
} OA2DP_FORMAT_LIST;

typedef struct _OA2DP_LATENCY {
    ULONG TransportUs;        /* encoder + packet queue + air */
    ULONG DeviceUs;           /* AVDTP delay report from the headset */
} OA2DP_LATENCY;

typedef struct _OA2DP_CONNECTED_IN {
    ULONG Connected;
    ULONG ConnectionGeneration; /* mandatory for TRUE, 0 for unavailable/closed */
} OA2DP_CONNECTED_IN;

typedef struct _OA2DP_SUSPENDED_IN {
    ULONG Suspended;          /* profile unavailable for A2DP, not merely idle PCM */
    ULONG RequestSerial;      /* 0 = unsolicited; echo the handled SuspendSerial */
    ULONG ConnectionGeneration;
} OA2DP_SUSPENDED_IN;

/* Completed when Windows opens a stream with a new format (F4.3). */
typedef struct _OA2DP_FORMAT_CHANGE {
    OA2DP_FORMAT Format;
    ULONG PcmSequence;        /* distinguishes A -> B -> A even if events coalesce */
    ULONG Running;            /* 1 = stream started, 0 = stopped */
    ULONG Prepared;           /* PAUSE/RUN owns prepared transport; not audible playback */
    ULONG ConnectionEnabled;  /* Windows one-shot disconnect suppresses retries */
    ULONG ConnectionSerial;   /* distinguishes an explicit reconnect request */
    ULONG SuspendRequested;   /* native multi-profile manager pauses A2DP for SCO */
    ULONG SuspendSerial;      /* exact MPM suspend/resume request, echoed on completion */
} OA2DP_FORMAT_CHANGE;

typedef struct _OA2DP_ENDPOINT_NAME {
    WCHAR Name[128];          /* friendly name shown in Sound settings */
} OA2DP_ENDPOINT_NAME;

#endif
