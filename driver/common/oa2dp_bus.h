/* SPDX-License-Identifier: MIT */
/* Kernel-only, referenced interface between the transport PDO and PortCls. */
#pragma once
#include "oa2dp_ioctl.h"

#define OA2DP_BUS_VERSION 2
#define OA2DP_VOLUME_MIN_DB (-127L * 32768L)
#define OA2DP_VOLUME_MAX_DB 0L
#define OA2DP_VOLUME_STEP_DB 32768L

typedef VOID (*OA2DP_VOLUME_NOTIFY)(PVOID Context);
typedef NTSTATUS (*OA2DP_POLICY_REQUEST)(PVOID Context, BOOLEAN Enabled, BOOLEAN Suspended);
typedef struct _OA2DP_BUS_INTERFACE {
    INTERFACE Header;
    VOID (*GetVolume)(PVOID Context, PLONG Db, PBOOLEAN Muted);
    VOID (*SetVolume)(PVOID Context, LONG Db, BOOLEAN Muted, BOOLEAN HostWrite);
    VOID (*SetConnected)(PVOID Context, BOOLEAN Connected);
    NTSTATUS (*RequestConnection)(PVOID Context, BOOLEAN Enabled);
    VOID (*SetPlaying)(PVOID Context, BOOLEAN Playing);
    VOID (*SetSuspended)(PVOID Context, BOOLEAN Suspended);
    NTSTATUS (*RegisterAudio)(PVOID Context, PVOID AudioContext,
                             OA2DP_VOLUME_NOTIFY Notify,
                             OA2DP_POLICY_REQUEST RequestPolicy);
    VOID (*UnregisterAudio)(PVOID Context, PVOID AudioContext);
} OA2DP_BUS_INTERFACE;
