/* SPDX-License-Identifier: MIT */
#pragma once

/* Include after the platform's KS and NTSTATUS headers. Keep this parser
 * allocation-free and shared with the user-mode offline contract tests. */
static inline NTSTATUS Oa2dpParseModeAttributes(
    _In_reads_bytes_opt_(Bytes) const void *Buffer,
    _In_ ULONG Bytes,
    _Out_ GUID *Mode,
    _Out_ ULONG *SerializedBytes)
{
    *Mode = AUDIO_SIGNALPROCESSINGMODE_DEFAULT;
    *SerializedBytes = 0;
    if (Bytes == 0)
        return STATUS_SUCCESS;
    if (!Buffer || Bytes < sizeof(KSMULTIPLE_ITEM))
        return STATUS_INVALID_PARAMETER;

    KSMULTIPLE_ITEM items;
    RtlCopyMemory(&items, Buffer, sizeof(items));
    if (items.Size < sizeof(items) || items.Size > Bytes)
        return STATUS_INVALID_PARAMETER;

    ULONG offset = sizeof(items);
    BOOLEAN foundMode = FALSE;
    for (ULONG n = 0; n < items.Count; ++n)
    {
        const ULONG remaining = items.Size - offset;
        if (remaining < sizeof(KSATTRIBUTE))
            return STATUS_INVALID_PARAMETER;
        KSATTRIBUTE header;
        RtlCopyMemory(&header, static_cast<const BYTE *>(Buffer) + offset, sizeof(header));
        if (header.Size < sizeof(header) || header.Size > remaining)
            return STATUS_INVALID_PARAMETER;
        if (!IsEqualGUID(header.Attribute, KSATTRIBUTEID_AUDIOSIGNALPROCESSING_MODE) ||
            (header.Flags & ~KSATTRIBUTE_REQUIRED) != 0)
            return STATUS_NOT_SUPPORTED;
        if (foundMode || header.Size != sizeof(KSATTRIBUTE_AUDIOSIGNALPROCESSING_MODE))
            return STATUS_INVALID_PARAMETER;

        KSATTRIBUTE_AUDIOSIGNALPROCESSING_MODE attribute;
        RtlCopyMemory(&attribute, static_cast<const BYTE *>(Buffer) + offset, sizeof(attribute));
        if (!IsEqualGUID(attribute.SignalProcessingMode, AUDIO_SIGNALPROCESSINGMODE_DEFAULT) &&
            !IsEqualGUID(attribute.SignalProcessingMode, AUDIO_SIGNALPROCESSINGMODE_RAW))
            return STATUS_NOT_SUPPORTED;
        *Mode = attribute.SignalProcessingMode;
        foundMode = TRUE;

        const ULONG padding = (8 - (header.Size & 7)) & 7;
        if (padding > remaining - header.Size)
            return STATUS_INVALID_PARAMETER;
        offset += header.Size + padding;
    }
    // Do not echo bytes that were not described and validated by the list.
    if (offset != items.Size)
        return STATUS_INVALID_PARAMETER;
    *SerializedBytes = items.Size;
    return STATUS_SUCCESS;
}
