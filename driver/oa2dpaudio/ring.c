/* SPDX-License-Identifier: MIT */
/*
 * oa2dpaudio.sys — shared PCM ring (producer side) and its user-mode
 * mapping for oa2dpsvc.exe. The WaveRT stream calls Oa2dpRingPump() from
 * its position timer with the frames Windows' audio engine wrote since the
 * last call (see minwavertstream.cpp).
 */
#include <ntifs.h>

#include "oa2dpaudio.h"

/* ~340 ms at 96 kHz; plenty for encoder + scheduling jitter. */
#define RING_FRAMES 32768

NTSTATUS Oa2dpRingCreate(_Inout_ OA2DP_AUDIO_RING *r, _In_ ULONG rate, _In_ USHORT channels,
                         _In_ USHORT format)
{
    PHYSICAL_ADDRESS low, high, skip;
    /* Sized for the largest channel count so the allocation never changes. */
    SIZE_T bytes = oa2dp_ring_bytes(RING_FRAMES, OA2DP_RING_MAX_CHANNELS);
    PMDL mdl;
    oa2dp_pcm_ring *ring;

    if (channels == 0 || channels > OA2DP_RING_MAX_CHANNELS)
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(r, sizeof(*r));
    KeInitializeSpinLock(&r->Lock);
    low.QuadPart = 0;
    high.QuadPart = (LONGLONG)-1;
    skip.QuadPart = 0;
    /* Non-paged, page-aligned memory we can map into the service later. */
    mdl = MmAllocatePagesForMdlEx(low, high, skip, bytes, MmCached,
                                  MM_ALLOCATE_FULLY_REQUIRED | MM_DONT_ZERO_ALLOCATION);
    if (!mdl)
        return STATUS_INSUFFICIENT_RESOURCES;
    if (MmGetMdlByteCount(mdl) < bytes) {
        MmFreePagesFromMdl(mdl);
        ExFreePool(mdl);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    ring = (oa2dp_pcm_ring *)MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority | MdlMappingNoExecute);
    if (!ring) {
        MmFreePagesFromMdl(mdl);
        ExFreePool(mdl);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(ring, bytes);
    /* Initialize the immutable storage stride from the allocation, not the
     * active format: a persisted mono preference must still allow stereo. */
    if (oa2dp_ring_init(ring, RING_FRAMES, rate, OA2DP_RING_MAX_CHANNELS, format) != OA2DP_OK ||
        oa2dp_ring_set_format(ring, rate, channels, format) < 0) {
        MmFreePagesFromMdl(mdl);
        ExFreePool(mdl);
        return STATUS_INVALID_PARAMETER;
    }
    r->Mdl = mdl;
    r->Ring = ring;
    r->Bytes = bytes;
    return STATUS_SUCCESS;
}

/* PASSIVE_LEVEL. Unmaps the service's view first (from any process). */
VOID Oa2dpRingDestroy(_Inout_ OA2DP_AUDIO_RING *r)
{
    KIRQL irql;
    PMDL mdl;

    Oa2dpRingUnmapUser(r);
    KeAcquireSpinLock(&r->Lock, &irql);
    mdl = r->Mdl;
    r->Ring = NULL; /* a late pump sees NULL and does nothing */
    r->Mdl = NULL;
    r->Bytes = 0;
    KeReleaseSpinLock(&r->Lock, irql);
    if (mdl) {
        MmFreePagesFromMdl(mdl);
        ExFreePool(mdl);
    }
}

/* Must run in the context of the service process: the IOCTL arrives at
 * PASSIVE_LEVEL in the caller's thread (we are the top of the stack). */
NTSTATUS Oa2dpRingMapUser(_Inout_ OA2DP_AUDIO_RING *r, _Out_ OA2DP_RING_MAPPING *out)
{
    PVOID va = NULL;

    RtlZeroMemory(out, sizeof(*out));
    if (!r->Mdl)
        return STATUS_DEVICE_NOT_READY;
    if (r->UserVa)
        return STATUS_DEVICE_BUSY; /* one consumer at a time */
    __try {
        va = MmMapLockedPagesSpecifyCache(r->Mdl, UserMode, MmCached, NULL, FALSE,
                                          NormalPagePriority | MdlMappingNoExecute);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
    if (!va)
        return STATUS_INSUFFICIENT_RESOURCES;
    r->UserVa = va;
    r->UserProcess = PsGetCurrentProcess();
    ObReferenceObject(r->UserProcess);
    out->Address = (ULONGLONG)(ULONG_PTR)va;
    out->Bytes = r->Bytes;
    return STATUS_SUCCESS;
}

/* PASSIVE_LEVEL. Normally called in the service's context (UNMAP_RING,
 * IRP_MJ_CLEANUP); on device removal we attach to the service process, since
 * pages must never stay mapped in user space after we free them. */
VOID Oa2dpRingUnmapUser(_Inout_ OA2DP_AUDIO_RING *r)
{
    KAPC_STATE apc;

    (void)Oa2dpRingSetDataEvent(r, NULL, 0, KernelMode);
    if (!r->UserVa)
        return;
    if (r->UserProcess == PsGetCurrentProcess()) {
        MmUnmapLockedPages(r->UserVa, r->Mdl);
    } else {
        KeStackAttachProcess((PRKPROCESS)r->UserProcess, &apc);
        MmUnmapLockedPages(r->UserVa, r->Mdl);
        KeUnstackDetachProcess(&apc);
    }
    ObDereferenceObject(r->UserProcess);
    r->UserVa = NULL;
    r->UserProcess = NULL;
}

/* No lost wakeup: register before checking data, and signal as a level
 * condition after every producer commit. The consumer rechecks the ring
 * before waiting, so auto-reset event coalescing never hides queued frames. */
NTSTATUS Oa2dpRingSetDataEvent(OA2DP_AUDIO_RING *r, HANDLE event,
                              ULONG minimumFrames, KPROCESSOR_MODE mode)
{
    PKEVENT next = NULL, old;
    KIRQL irql;
    NTSTATUS status = STATUS_SUCCESS;
    if ((!event && minimumFrames) || (event && (!minimumFrames || minimumFrames > RING_FRAMES)))
        return STATUS_INVALID_PARAMETER;
    if (event) {
        status = ObReferenceObjectByHandle(event, EVENT_MODIFY_STATE,
            *ExEventObjectType, mode, (PVOID *)&next, NULL);
        if (!NT_SUCCESS(status)) return status;
    }
    KeAcquireSpinLock(&r->Lock, &irql);
    old = NULL;
    if (next && !r->Ring) {
        status = STATUS_DEVICE_NOT_READY;
    } else {
        old = r->DataEvent;
        r->DataEvent = next;
        r->MinimumFrames = minimumFrames;
        next = NULL;
        if (r->DataEvent && oa2dp_ring_readable(r->Ring) >= minimumFrames)
            KeSetEvent(r->DataEvent, IO_NO_INCREMENT, FALSE);
    }
    KeReleaseSpinLock(&r->Lock, irql);
    if (old) ObDereferenceObject(old);
    if (next) ObDereferenceObject(next);
    return status;
}

/* Rewrites only the format fields: positions keep counting, so a consumer
 * that is still running never sees them jump backwards. */
static BOOLEAN SetProducerFormat(OA2DP_AUDIO_RING *r, const OA2DP_FORMAT *format, BOOLEAN newStream)
{
    KIRQL irql;
    BOOLEAN changed = FALSE;

    if (!oa2dp_pcm_format_valid(format))
        return FALSE;
    KeAcquireSpinLock(&r->Lock, &irql);
    if (r->Ring && (newStream || !r->SourceFormatValid || !oa2dp_pcm_format_same(&r->SourceFormat, format))) {
        changed = oa2dp_ring_begin_format(r->Ring, format->SampleRate, format->Channels,
            oa2dp_pcm_format_ring_type(format)) > 0;
        if (changed) {
            r->SourceFormat = *format;
            r->SourceFormatValid = TRUE;
        }
    }
    if (changed && r->DataEvent) KeSetEvent(r->DataEvent, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&r->Lock, irql);
    return changed;
}

BOOLEAN Oa2dpRingSetFormat(OA2DP_AUDIO_RING *r, const OA2DP_FORMAT *format)
{
    return SetProducerFormat(r, format, FALSE);
}

/* A fresh WaveRT clip is a new producer epoch even when its PCM format is
 * identical. Establish this boundary before the first sample can be pumped. */
BOOLEAN Oa2dpRingBeginStream(OA2DP_AUDIO_RING *r, const OA2DP_FORMAT *format)
{
    return SetProducerFormat(r, format, TRUE);
}

VOID Oa2dpRingSetRunning(_Inout_ OA2DP_AUDIO_RING *r, _In_ BOOLEAN running)
{
    KIRQL irql;

    KeAcquireSpinLock(&r->Lock, &irql);
    if (r->Ring) {
        if (running)
            InterlockedOr((volatile LONG *)&r->Ring->flags, OA2DP_RING_FLAG_RUNNING);
        else
            InterlockedAnd((volatile LONG *)&r->Ring->flags, ~(LONG)OA2DP_RING_FLAG_RUNNING);
    }
    if (r->DataEvent) KeSetEvent(r->DataEvent, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&r->Lock, irql);
}

/* DISPATCH_LEVEL (stream timer). Drops audio when the stream format does not
 * match the ring header, which only happens while a format change races a
 * closing stream. */
VOID Oa2dpRingPump(_Inout_ OA2DP_AUDIO_RING *r, _In_reads_bytes_(cyclicBytes) const UCHAR *cyclic,
                   _In_ ULONG cyclicBytes, _In_ ULONG pos, _In_ ULONG frames,
                   _In_ const OA2DP_FORMAT *format)
{
    KIRQL irql;
    USHORT channels = format->Channels;
    USHORT bits = format->BitsPerSample;
    ULONG frameBytes = (ULONG)channels * (bits / 8);
    USHORT want = oa2dp_pcm_format_ring_type(format);

    if (frameBytes == 0 || cyclicBytes < frameBytes || channels > OA2DP_RING_MAX_CHANNELS ||
        (bits != 16 && bits != 24 && bits != 32))
        return;
    if (pos >= cyclicBytes)
        pos = 0;
    KeAcquireSpinLock(&r->Lock, &irql);
    if (r->Ring && r->SourceFormatValid && oa2dp_pcm_format_same(&r->SourceFormat, format) &&
        r->Ring->sample_rate == format->SampleRate &&
        r->Ring->channels == channels && r->Ring->format == want) {
        while (frames) {
            ULONG untilWrap = (cyclicBytes - pos) / frameBytes;
            ULONG n = frames < untilWrap ? frames : untilWrap;
            if (n == 0) {
                pos = 0;
                continue;
            }
            if (bits == 32) {
                (void)oa2dp_ring_write(r->Ring, cyclic + pos, n);
            } else {
                if (n > OA2DP_PUMP_CHUNK)
                    n = OA2DP_PUMP_CHUNK;
                (void)oa2dp_ring_write_pcm(r->Ring, cyclic + pos, n, bits);
            }
            pos += n * frameBytes;
            if (pos >= cyclicBytes)
                pos = 0;
            frames -= n;
        }
        if (r->DataEvent && oa2dp_ring_readable(r->Ring) >= r->MinimumFrames)
            KeSetEvent(r->DataEvent, IO_NO_INCREMENT, FALSE);
    }
    KeReleaseSpinLock(&r->Lock, irql);
}
