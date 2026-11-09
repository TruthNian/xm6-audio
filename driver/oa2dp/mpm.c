/* SPDX-License-Identifier: MIT */
/* Windows 10 multi-profile manager compatibility contract, observed in the
 * local Microsoft implementation/public symbols. This is not a documented
 * cross-version ABI. Windows retains HFP/AVRCP, radio and pairing ownership.
 * Callbacks only publish policy through the existing audio waiter. */
#include "oa2dp.h"

typedef NTSTATUS (NTAPI *MPM_CALLBACK)(PVOID, ULONG);
__declspec(dllimport) NTSTATUS NTAPI BtaMpmRegister(
    BTH_ADDR Address, ULONG Profile, MPM_CALLBACK Connection, MPM_CALLBACK Suspend,
    MPM_CALLBACK SuspendStatus, MPM_CALLBACK PlayStatus, PVOID Context, PVOID *Handle);
__declspec(dllimport) NTSTATUS NTAPI BtaMpmUnregister(PVOID Handle);
__declspec(dllimport) NTSTATUS NTAPI BtaMpmConnectionRequest(PVOID Handle, ULONG Connect, ULONG Automatic);
__declspec(dllimport) NTSTATUS NTAPI BtaMpmUpdateConnectionStatus(PVOID Handle, ULONG Connected, ULONG Notify);
__declspec(dllimport) NTSTATUS NTAPI BtaMpmUpdateSuspendStatus(PVOID Handle, ULONG Suspended, ULONG Notify);
__declspec(dllimport) NTSTATUS NTAPI BtaMpmUpdatePlayStatus(PVOID Handle, ULONG Playing);

static BOOLEAN AcquireManager(OA2DP_DEVICE_CONTEXT *dev)
{
    if (!dev->MpmHandle || !ExAcquireRundownProtection(&dev->MpmCalls)) return FALSE;
    if (!dev->MpmHandle) {
        ExReleaseRundownProtection(&dev->MpmCalls);
        return FALSE;
    }
    return TRUE;
}

static NTSTATUS NTAPI ConnectionRequest(PVOID context, ULONG connect)
{
    OA2DP_DEVICE_CONTEXT *dev = context;
    NTSTATUS status = STATUS_SUCCESS;
    BOOLEAN acknowledge = FALSE;
    const BOOLEAN enabled = connect != 0;
    if (!ExAcquireRundownProtection(&dev->MpmCallbacks)) return STATUS_DELETE_PENDING;
    WdfSpinLockAcquire(dev->Lock);
    if (dev->MpmStopping) {
        status = STATUS_DELETE_PENDING;
    } else {
        dev->PolicyConnected = enabled;
        if (!enabled) KeClearEvent(&dev->DisconnectedEvent);
        if (dev->AudioPolicyRequest)
            status = dev->AudioPolicyRequest(dev->AudioContext, enabled, dev->PolicySuspended);
        acknowledge = NT_SUCCESS(status) && dev->MpmConnected == enabled &&
            (enabled || (dev->Channels[Oa2dpChannelSignaling].Life.State == ChannelClosed &&
                         dev->Channels[Oa2dpChannelMedia].Life.State == ChannelClosed));
    }
    WdfSpinLockRelease(dev->Lock);
    if (acknowledge && AcquireManager(dev)) {
        (void)BtaMpmUpdateConnectionStatus(dev->MpmHandle, enabled, 1);
        if (!enabled) KeSetEvent(&dev->DisconnectedEvent, IO_NO_INCREMENT, FALSE);
        ExReleaseRundownProtection(&dev->MpmCalls);
    }
    ExReleaseRundownProtection(&dev->MpmCallbacks);
    return status;
}

static NTSTATUS NTAPI SuspendRequest(PVOID context, ULONG suspend)
{
    OA2DP_DEVICE_CONTEXT *dev = context;
    NTSTATUS status = STATUS_SUCCESS;
    BOOLEAN acknowledge = FALSE;
    const BOOLEAN suspended = suspend != 0;
    if (!ExAcquireRundownProtection(&dev->MpmCallbacks)) return STATUS_DELETE_PENDING;
    WdfSpinLockAcquire(dev->Lock);
    if (dev->MpmStopping) {
        status = STATUS_DELETE_PENDING;
    } else {
        dev->PolicySuspended = suspended;
        if (dev->AudioPolicyRequest)
            status = dev->AudioPolicyRequest(dev->AudioContext, dev->PolicyConnected, suspended);
        acknowledge = NT_SUCCESS(status) && dev->MpmIsSuspended == suspended;
    }
    WdfSpinLockRelease(dev->Lock);
    if (acknowledge) Oa2dpMpmSuspended(dev, suspended);
    ExReleaseRundownProtection(&dev->MpmCallbacks);
    return status;
}

static NTSTATUS NTAPI SuspendStatus(PVOID context, ULONG suspended)
{
    // Like the inbox A2DP notification callback, no extra protocol action is
    // needed here. Requests and actual encoder completion are separate paths.
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(suspended);
    return STATUS_SUCCESS;
}

NTSTATUS Oa2dpMpmStart(OA2DP_DEVICE_CONTEXT *dev)
{
    PVOID handle = NULL;
    NTSTATUS status;
    PAGED_CODE();
    ExInitializeRundownProtection(&dev->MpmCalls);
    ExInitializeRundownProtection(&dev->MpmCallbacks);
    dev->MpmStopping = FALSE;
    dev->PolicyConnected = TRUE;
    dev->PolicySuspended = FALSE;
    dev->MpmConnected = FALSE;
    dev->MpmIsSuspended = TRUE;
    WdfObjectReference(dev->Device);
    // Profile 1 is A2DP on the tested Windows 10 build. Context is retained
    // until unregister and callback rundown both finish.
    status = BtaMpmRegister(dev->RemoteAddress, 1, ConnectionRequest,
        SuspendRequest, SuspendStatus, NULL, dev, &handle);
    if (NT_SUCCESS(status)) {
        InterlockedExchangePointer(&dev->MpmHandle, handle);
    } else {
        WdfObjectDereference(dev->Device);
    }
    return status;
}

VOID Oa2dpMpmStop(OA2DP_DEVICE_CONTEXT *dev)
{
    PVOID handle;
    PAGED_CODE();
    if (!dev->MpmHandle) return;
    WdfSpinLockAcquire(dev->Lock);
    dev->MpmStopping = TRUE;
    WdfSpinLockRelease(dev->Lock);
    // Stop exported-API users before freeing the opaque registration. Keep
    // callback context alive while Windows removes its queued notifications.
    ExWaitForRundownProtectionRelease(&dev->MpmCalls);
    handle = InterlockedExchangePointer(&dev->MpmHandle, NULL);
    if (handle) (void)BtaMpmUnregister(handle);
    ExWaitForRundownProtectionRelease(&dev->MpmCallbacks);
    WdfObjectDereference(dev->Device);
}

NTSTATUS Oa2dpMpmRequestConnection(PVOID context, BOOLEAN enabled)
{
    OA2DP_DEVICE_CONTEXT *dev = context;
    NTSTATUS status;
    if (!AcquireManager(dev)) return STATUS_DEVICE_NOT_READY;
    // A pending automatic OPEN may precede MPM's connected state. Explicit
    // user disconnect must latch local policy even if MPM has no live profile
    // transition to call back for yet. Coordination still goes through MPM.
    if (!enabled) {
        status = ConnectionRequest(dev, FALSE);
        if (!NT_SUCCESS(status)) {
            ExReleaseRundownProtection(&dev->MpmCalls);
            return status;
        }
    }
    status = BtaMpmConnectionRequest(dev->MpmHandle, enabled, 0);
    ExReleaseRundownProtection(&dev->MpmCalls);
    // A one-shot property queues policy; it is not a pending KS method IRP.
    return NT_SUCCESS(status) ? STATUS_SUCCESS : status;
}

VOID Oa2dpMpmConnected(OA2DP_DEVICE_CONTEXT *dev, BOOLEAN connected)
{
    WdfSpinLockAcquire(dev->Lock);
    dev->MpmConnected = connected;
    WdfSpinLockRelease(dev->Lock);
    if (!AcquireManager(dev)) return;
    (void)BtaMpmUpdateConnectionStatus(dev->MpmHandle, connected, 1);
    ExReleaseRundownProtection(&dev->MpmCalls);
}

VOID Oa2dpMpmPlaying(PVOID context, BOOLEAN playing)
{
    OA2DP_DEVICE_CONTEXT *dev = context;
    if (!AcquireManager(dev)) return;
    (void)BtaMpmUpdatePlayStatus(dev->MpmHandle, playing);
    ExReleaseRundownProtection(&dev->MpmCalls);
}

VOID Oa2dpMpmSuspended(PVOID context, BOOLEAN suspended)
{
    OA2DP_DEVICE_CONTEXT *dev = context;
    WdfSpinLockAcquire(dev->Lock);
    dev->MpmIsSuspended = suspended;
    WdfSpinLockRelease(dev->Lock);
    if (!AcquireManager(dev)) return;
    (void)BtaMpmUpdateSuspendStatus(dev->MpmHandle, suspended, 1);
    ExReleaseRundownProtection(&dev->MpmCalls);
}
