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
__declspec(dllimport) NTSTATUS NTAPI BtaMpmUpdateConnectionStatus(
    PVOID Handle, ULONG Connected, ULONG RequestedCompletion);
__declspec(dllimport) NTSTATUS NTAPI BtaMpmUpdateSuspendStatus(
    PVOID Handle, ULONG Suspended, ULONG RequestedCompletion);
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
        (void)Oa2dpMpmBeginRequest(&dev->MpmRequest, enabled);
        if (!enabled) KeClearEvent(&dev->DisconnectedEvent);
        if (dev->AudioPolicyRequest)
            status = dev->AudioPolicyRequest(dev->AudioContext, enabled, dev->PolicySuspended,
                                             dev->MpmSuspendRequest.Serial);
        acknowledge = NT_SUCCESS(status) && dev->MpmConnected == enabled &&
            (enabled || (dev->Channels[Oa2dpChannelSignaling].Life.State == ChannelClosed &&
                         dev->Channels[Oa2dpChannelMedia].Life.State == ChannelClosed));
    }
    WdfSpinLockRelease(dev->Lock);
    // Policy callbacks may run above PASSIVE_LEVEL or reenter publication.
    // Re-acknowledge through the same serialized, state-validating publisher.
    if (acknowledge) WdfWorkItemEnqueue(dev->PublicationWork);
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
        (void)Oa2dpMpmBeginRequest(&dev->MpmSuspendRequest, suspended);
        if (dev->AudioPolicyRequest)
            status = dev->AudioPolicyRequest(dev->AudioContext, dev->PolicyConnected, suspended,
                                             dev->MpmSuspendRequest.Serial);
        acknowledge = NT_SUCCESS(status) && dev->MpmIsSuspended == suspended;
    }
    WdfSpinLockRelease(dev->Lock);
    if (acknowledge) WdfWorkItemEnqueue(dev->PublicationWork);
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
    if (dev->MpmHandle) return STATUS_DEVICE_BUSY; // do not overwrite a live registration
    ExInitializeRundownProtection(&dev->MpmCalls);
    ExInitializeRundownProtection(&dev->MpmCallbacks);
    dev->MpmStopping = FALSE;
    dev->PolicyConnected = TRUE;
    dev->PolicySuspended = FALSE;
    dev->MpmConnected = FALSE;
    RtlZeroMemory(&dev->MpmRequest, sizeof(dev->MpmRequest));
    RtlZeroMemory(&dev->MpmSuspendRequest, sizeof(dev->MpmSuspendRequest));
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
    // Observed local contract consumes this handle and returns zero. Do not
    // invent a retryable-failure ownership contract for this private API.
    if (handle) (void)BtaMpmUnregister(handle);
    ExWaitForRundownProtectionRelease(&dev->MpmCallbacks);
    // No callback can enqueue a publication after this final drain.
    WdfWorkItemFlush(dev->PublicationWork);
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

NTSTATUS Oa2dpMpmConnected(OA2DP_DEVICE_CONTEXT *dev, BOOLEAN connected)
{
    NTSTATUS status;
    ULONG serial;
    if (!AcquireManager(dev))
        return !connected && !dev->MpmHandle ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
    WdfSpinLockAcquire(dev->Lock);
    serial = Oa2dpMpmCompletion(&dev->MpmRequest, connected);
    WdfSpinLockRelease(dev->Lock);
    // Observed inbox distinction: peer indications pass 0, requested
    // confirmations pass 1. The last argument is not a generic "notify" flag.
    status = BtaMpmUpdateConnectionStatus(dev->MpmHandle, connected, serial != 0);
    ExReleaseRundownProtection(&dev->MpmCalls);
    if (!NT_SUCCESS(status)) return status;
    WdfSpinLockAcquire(dev->Lock);
    dev->MpmConnected = connected;
    // The external call may reenter policy. Never retire a replacement request.
    (void)Oa2dpMpmFinishRequest(&dev->MpmRequest, serial);
    WdfSpinLockRelease(dev->Lock);
    return status;
}

VOID Oa2dpMpmOpenFailed(OA2DP_DEVICE_CONTEXT *dev, ULONG policySerial)
{
    BOOLEAN publish;
    WdfSpinLockAcquire(dev->Lock);
    publish = (BOOLEAN)Oa2dpMpmFailConnect(&dev->MpmRequest, policySerial);
    WdfSpinLockRelease(dev->Lock);
    // A requested connect that fails must also finish its MPM operation.
    // The worker still validates actual closed channels before publishing.
    if (publish) WdfWorkItemEnqueue(dev->PublicationWork);
}

VOID Oa2dpMpmPlaying(PVOID context, BOOLEAN playing)
{
    OA2DP_DEVICE_CONTEXT *dev = context;
    if (!AcquireManager(dev)) return;
    (void)BtaMpmUpdatePlayStatus(dev->MpmHandle, playing);
    ExReleaseRundownProtection(&dev->MpmCalls);
}

/* Caller owns PublicationLock at PASSIVE_LEVEL. Cache only accepted updates,
 * and never complete a policy request that this service reply did not handle. */
static NTSTATUS PublishSuspended(OA2DP_DEVICE_CONTEXT *dev, BOOLEAN suspended,
                                 ULONG requestSerial, ULONG connection)
{
    ULONG serial;
    NTSTATUS status = STATUS_SUCCESS;
    WdfSpinLockAcquire(dev->Lock);
    OA2DP_CHANNEL *signal = &dev->Channels[Oa2dpChannelSignaling];
    if ((connection && connection != signal->Life.ConnectionGeneration) ||
        (!connection && signal->Life.State != ChannelClosed))
        status = STATUS_DEVICE_NOT_READY;
    if (!suspended && (dev->PolicySuspended ||
        !Oa2dpConnectionReady(&signal->Life, &dev->Channels[Oa2dpChannelMedia].Life,
            connection, dev->PolicyConnected, dev->TransportStopping || dev->MpmStopping)))
        status = STATUS_DEVICE_NOT_READY;
    serial = Oa2dpMpmOwnedReply(&dev->MpmSuspendRequest, requestSerial);
    WdfSpinLockRelease(dev->Lock);
    if (!NT_SUCCESS(status)) return status;
    if (!AcquireManager(dev)) return STATUS_DEVICE_NOT_READY;
    status = BtaMpmUpdateSuspendStatus(dev->MpmHandle, suspended, serial != 0);
    ExReleaseRundownProtection(&dev->MpmCalls);
    if (NT_SUCCESS(status)) {
        WdfSpinLockAcquire(dev->Lock);
        dev->MpmIsSuspended = suspended;
        (void)Oa2dpMpmFinishRequest(&dev->MpmSuspendRequest, serial);
        WdfSpinLockRelease(dev->Lock);
    } else {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
            "oa2dp: MPM suspend notification failed 0x%08X\n", status));
    }
    return status;
}

NTSTATUS Oa2dpMpmSuspended(PVOID context, BOOLEAN suspended, ULONG requestSerial,
                          ULONG connection)
{
    OA2DP_DEVICE_CONTEXT *dev = context;
    NT_ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    WdfWaitLockAcquire(dev->PublicationLock, NULL);
    NTSTATUS status = PublishSuspended(dev, suspended, requestSerial, connection);
    WdfWaitLockRelease(dev->PublicationLock);
    return status;
}

VOID Oa2dpMpmAcknowledgeSuspend(OA2DP_DEVICE_CONTEXT *dev)
{
    ULONG serial, connection;
    BOOLEAN suspended;
    WdfWaitLockAcquire(dev->PublicationLock, NULL);
    WdfSpinLockAcquire(dev->Lock);
    suspended = dev->MpmIsSuspended;
    serial = dev->MpmSuspendRequest.Pending && dev->MpmSuspendRequest.Target == suspended
        ? dev->MpmSuspendRequest.Serial : 0;
    connection = dev->Channels[Oa2dpChannelSignaling].Life.ConnectionGeneration;
    WdfSpinLockRelease(dev->Lock);
    if (serial) (void)PublishSuspended(dev, suspended, serial, connection);
    WdfWaitLockRelease(dev->PublicationLock);
}
