/* SPDX-License-Identifier: MIT */
/*
 * oa2dpaudio.sys — state shared by the PortCls adapter (adapter.cpp), the
 * WaveRT miniport (minwavert*.cpp), the control interface for oa2dpsvc.exe
 * (control.cpp) and the PCM ring producer (ring.c).
 *
 * Plain C so ring.c can use it. Include after <wdm.h>, <ntifs.h> or
 * <portcls.h>.
 */
#pragma once

#include "../../core/include/oa2dp/pcm_ring.h"
#include "../common/oa2dp_ioctl.h"
#include "../common/oa2dp_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OA2DP_AUDIO_TAG 'dA2O'

/* KS subdevice names; must match the reference strings in oa2dpaudio.inf. */
#define OA2DP_WAVE_NAME L"WaveOa2dp"
#define OA2DP_TOPO_NAME L"TopologyOa2dp"
/* Reference string of GUID_DEVINTERFACE_OA2DP_AUDIO on our PDO. */
#define OA2DP_CONTROL_NAME L"Oa2dpControl"

/* Wave filter pins (derived from SYSVAD's simple render endpoint). */
#define OA2DP_WAVE_PIN_SINK 0   /* host render stream, from the audio engine */
#define OA2DP_WAVE_PIN_BRIDGE 1 /* to the topology filter */
#define OA2DP_TOPO_PIN_IN 0
#define OA2DP_TOPO_PIN_OUT 1

/* The ring is allocated once for this many channels and never moves, so a
 * format change only rewrites its header (the service's mapping stays). */
#define OA2DP_RING_MAX_CHANNELS 2
#define OA2DP_PUMP_CHUNK 256 /* maximum integer PCM frames per admission step */

typedef struct _OA2DP_AUDIO_RING {
    KSPIN_LOCK Lock;        /* header changes vs. the producer */
    PMDL Mdl;
    oa2dp_pcm_ring *Ring;   /* kernel mapping, NULL once destroyed */
    SIZE_T Bytes;
    PVOID UserVa;           /* service mapping, if any */
    PEPROCESS UserProcess;  /* referenced while UserVa is set */
    PKEVENT DataEvent;      /* referenced, accessed only under Lock */
    ULONG MinimumFrames;
    OA2DP_FORMAT SourceFormat; /* full producer metadata, guarded by Lock */
    BOOLEAN SourceFormatValid;
    KEVENT ConsumerEvent;    /* RUN preparation completion, never a running clock gate */
    BOOLEAN ConsumerMapped; /* protected by Lock; removal wakes pending RUN */
    BOOLEAN ConsumerCancelled;
    ULONG ConsumerCancelSerial; /* invalidates a wait even across rapid re-enable */
} OA2DP_AUDIO_RING;

typedef struct _OA2DP_AUDIO_DEVICE {
    BOOLEAN Initialized;
    BOOLEAN Started;
    BOOLEAN Removed;
    PDEVICE_OBJECT Fdo;
    PDEVICE_OBJECT Pdo;
    /* Immutable while the KS filters are registered. */
    GUID ContainerId;
    BOOLEAN ContainerIdValid;
    OA2DP_BUS_INTERFACE Bus;

    /* Spin lock for everything below that streams read at DISPATCH_LEVEL. */
    KSPIN_LOCK Lock;
    OA2DP_FORMAT_LIST Formats;  /* what the negotiated codec accepts */
    OA2DP_LATENCY Latency;
    OA2DP_FORMAT Current;       /* format of the open (or last) stream */
    ULONG PcmSequence;          /* ring epoch committed with Current */
    BOOLEAN Running;
    BOOLEAN ConnectionEnabled;
    BOOLEAN SuspendRequested;
    ULONG ConnectionSerial;
    ULONG SuspendSerial;
    volatile LONG Connected;  /* standard KS jack presence, initially false */
    BOOLEAN EventPending;       /* Event not yet picked up by WAIT_FORMAT */
    OA2DP_FORMAT_CHANGE Event;

    OA2DP_AUDIO_RING Ring;

    /* Control interface (one handle, the service). */
    KEVENT ControlLock;         /* PASSIVE_LEVEL lock, used as a mutex */
    KEVENT ConnectionLock;      /* serializes parent publication + local jack update */
    ULONG ControlTag;           /* &ControlTag marks our FILE_OBJECTs */
    PFILE_OBJECT ControlFile;
    UNICODE_STRING ControlLink;
    IO_CSQ WaitCsq;             /* pending IOCTL_OA2DP_AUDIO_WAIT_FORMAT */
    KSPIN_LOCK WaitLock;
    LIST_ENTRY WaitList;
    WCHAR Name[RTL_FIELD_SIZE(OA2DP_ENDPOINT_NAME, Name) / sizeof(WCHAR)];

    /* KS filters (PortCls subdevices). */
    KEVENT FilterLock;          /* PASSIVE_LEVEL lock, used as a mutex */
    PVOID WavePort;             /* PUNKNOWN of the ports, referenced; */
    PVOID TopoPort;             /* opaque outside adapter.cpp       */
    PVOID WaveMiniport;         /* CMiniportWaveRT *, referenced */
} OA2DP_AUDIO_DEVICE;

/* ---- ring.c ---------------------------------------------------------------- */

NTSTATUS Oa2dpRingCreate(_Inout_ OA2DP_AUDIO_RING *r, _In_ ULONG rate, _In_ USHORT channels,
                         _In_ USHORT format);
VOID Oa2dpRingDestroy(_Inout_ OA2DP_AUDIO_RING *r);
NTSTATUS Oa2dpRingMapUser(_Inout_ OA2DP_AUDIO_RING *r, _Out_ OA2DP_RING_MAPPING *out);
VOID Oa2dpRingUnmapUser(_Inout_ OA2DP_AUDIO_RING *r);
NTSTATUS Oa2dpRingSetDataEvent(_Inout_ OA2DP_AUDIO_RING *r, HANDLE event,
                              ULONG minimumFrames, KPROCESSOR_MODE mode);
/* Claim a producer epoch at RUN, before its first PCM sample. */
BOOLEAN Oa2dpRingBeginStream(_Inout_ OA2DP_AUDIO_RING *r, _In_ const OA2DP_FORMAT *format, _Out_ ULONG *sequence);
BOOLEAN Oa2dpRingConsumerReady(_Inout_ OA2DP_AUDIO_RING *r, ULONG sequence);
NTSTATUS Oa2dpRingSignalConsumer(_Inout_ OA2DP_AUDIO_RING *r, ULONG sequence);
NTSTATUS Oa2dpRingWaitConsumer(_Inout_ OA2DP_AUDIO_RING *r, ULONG sequence);
VOID Oa2dpRingSetConsumerAvailable(_Inout_ OA2DP_AUDIO_RING *r, BOOLEAN available);
VOID Oa2dpRingCancelConsumer(_Inout_ OA2DP_AUDIO_RING *r);
VOID Oa2dpRingEndSource(_Inout_ OA2DP_AUDIO_RING *r, ULONG sequence);
VOID Oa2dpRingSetRunning(_Inout_ OA2DP_AUDIO_RING *r, _In_ BOOLEAN running);
/* Copy `frames` frames of the WaveRT cyclic buffer, starting at byte offset
 * `pos`, into the ring. `bits` is the container: 32 = IEEE float, 16/24 =
 * PCM (converted to 24-bit values in int32, OA2DP_RING_FMT_S32). */
VOID Oa2dpRingPump(_Inout_ OA2DP_AUDIO_RING *r, _In_reads_bytes_(cyclicBytes) const UCHAR *cyclic,
                   _In_ ULONG cyclicBytes, _In_ ULONG pos, _In_ ULONG frames,
                   _In_ const OA2DP_FORMAT *format, ULONG sequence);

/* ---- control.cpp ----------------------------------------------------------- */

VOID Oa2dpControlInit(_Inout_ OA2DP_AUDIO_DEVICE *dev);
VOID Oa2dpLoadSettings(_Inout_ OA2DP_AUDIO_DEVICE *dev);
NTSTATUS Oa2dpControlStart(_Inout_ OA2DP_AUDIO_DEVICE *dev);
VOID Oa2dpControlStop(_Inout_ OA2DP_AUDIO_DEVICE *dev);
VOID Oa2dpControlRemove(_Inout_ OA2DP_AUDIO_DEVICE *dev);
/* The Locked variant requires dev->Lock. Release-only permits UNMAP on stop. */
NTSTATUS Oa2dpControlStatusLocked(_In_ const OA2DP_AUDIO_DEVICE *dev, _In_ BOOLEAN releaseOnly);
NTSTATUS Oa2dpControlStatus(_Inout_ OA2DP_AUDIO_DEVICE *dev, _In_ BOOLEAN releaseOnly);
BOOLEAN Oa2dpControlIsCreate(_In_ PIRP irp);
BOOLEAN Oa2dpControlOwns(_In_ OA2DP_AUDIO_DEVICE *dev, _In_ PIRP irp);
NTSTATUS Oa2dpControlDispatch(_Inout_ OA2DP_AUDIO_DEVICE *dev, _Inout_ PIRP irp);
/* Stream state for WAIT_FORMAT (F4.3); callable up to DISPATCH_LEVEL. */
VOID Oa2dpReportStream(_Inout_ OA2DP_AUDIO_DEVICE *dev, _In_ const OA2DP_FORMAT *format,
                       _In_ BOOLEAN running, ULONG sequence);
/* Snapshot of the format list; callable up to DISPATCH_LEVEL. */
VOID Oa2dpGetFormats(_In_ OA2DP_AUDIO_DEVICE *dev, _Out_ OA2DP_FORMAT_LIST *out);
BOOLEAN Oa2dpFormatListed(_In_ const OA2DP_FORMAT_LIST *list, _In_ const OA2DP_FORMAT *f);
/* Total presentation delay in microseconds (F4.4). */
ULONG Oa2dpLatencyUs(_In_ OA2DP_AUDIO_DEVICE *dev);
VOID Oa2dpLock(_Inout_ PKEVENT lock);
VOID Oa2dpUnlock(_Inout_ PKEVENT lock);

/* ---- adapter.cpp / minwavert.cpp ------------------------------------------- */

/* PASSIVE_LEVEL, ControlLock held. Metadata only; never removes filters. */
NTSTATUS Oa2dpPublishNameLocked(_In_ OA2DP_AUDIO_DEVICE *dev);
/* PASSIVE_LEVEL, no caller locks held; first creation only, checks stop/remove. */
NTSTATUS Oa2dpEnsureNamedFilters(_Inout_ OA2DP_AUDIO_DEVICE *dev);

/* Tell the audio engine that the supported formats changed. */
VOID Oa2dpWaveFormatsChanged(_Inout_ OA2DP_AUDIO_DEVICE *dev);
NTSTATUS Oa2dpSetConnected(_Inout_ OA2DP_AUDIO_DEVICE *dev, _In_ BOOLEAN connected,
                          ULONG connection);
NTSTATUS Oa2dpApplyPolicy(_Inout_ OA2DP_AUDIO_DEVICE *dev, _In_ BOOLEAN enabled,
                         _In_ BOOLEAN suspended, ULONG suspendSerial, _Out_ PIRP *completion);

#ifdef __cplusplus
}
#endif
