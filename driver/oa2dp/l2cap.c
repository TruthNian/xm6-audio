/* SPDX-License-Identifier: MIT */
/* L2CAP channel management through Bluetooth Request Blocks (BRBs). */
#include "oa2dp.h"

static NTSTATUS CloseChannelGeneration(OA2DP_DEVICE_CONTEXT *dev,
                                       OA2DP_CHANNEL_KIND kind, ULONG generation,
                                       ULONG connection, PULONG closedConnection);

/* ------------------------------------------------------------------------ */
/* Submission helpers                                                       */
/* ------------------------------------------------------------------------ */

NTSTATUS Oa2dpSubmitBrbSync(_In_ OA2DP_DEVICE_CONTEXT *dev, _In_ struct _BRB *brb, _In_ ULONG size)
{
    WDF_MEMORY_DESCRIPTOR desc;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&desc, brb, size);
    return WdfIoTargetSendInternalIoctlOthersSynchronously(
        dev->IoTarget, NULL, IOCTL_INTERNAL_BTH_SUBMIT_BRB, &desc, NULL, NULL, NULL, NULL);
}

static NTSTATUS SubmitBrbAsync(_In_ OA2DP_DEVICE_CONTEXT *dev, _In_ WDFREQUEST request,
                               _In_ struct _BRB *brb, _In_ ULONG size,
                               _In_ PFN_WDF_REQUEST_COMPLETION_ROUTINE done, _In_ PVOID ctx,
                               ULONG timeoutMs)
{
    NTSTATUS status;
    WDFMEMORY mem;
    WDF_OBJECT_ATTRIBUTES attrs;

    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ParentObject = request;
    status = WdfMemoryCreatePreallocated(&attrs, brb, size, &mem);
    if (!NT_SUCCESS(status))
        return status;
    status = WdfIoTargetFormatRequestForInternalIoctlOthers(
        dev->IoTarget, request, IOCTL_INTERNAL_BTH_SUBMIT_BRB, mem, NULL, NULL, NULL, NULL, NULL);
    if (!NT_SUCCESS(status))
        return status;
    WdfRequestSetCompletionRoutine(request, done, ctx);
    WDF_REQUEST_SEND_OPTIONS options;
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, timeoutMs ? WDF_REQUEST_SEND_OPTION_TIMEOUT : 0);
    if (timeoutMs)
        WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_MS(timeoutMs));
    if (!WdfRequestSend(request, dev->IoTarget, &options))
        return WdfRequestGetStatus(request);
    return STATUS_PENDING;
}

/* ------------------------------------------------------------------------ */
/* Addresses                                                                */
/* ------------------------------------------------------------------------ */

NTSTATUS Oa2dpQueryLocalAddress(_In_ OA2DP_DEVICE_CONTEXT *dev)
{
    struct _BRB_GET_LOCAL_BD_ADDR *brb = (struct _BRB_GET_LOCAL_BD_ADDR *)
        dev->Profile.BthAllocateBrb(BRB_HCI_GET_LOCAL_BD_ADDR, OA2DP_POOL_TAG);
    NTSTATUS status;
    if (!brb)
        return STATUS_INSUFFICIENT_RESOURCES;
    status = Oa2dpSubmitBrbSync(dev, (struct _BRB *)brb, sizeof(*brb));
    if (NT_SUCCESS(status))
        dev->LocalAddress = brb->BtAddress;
    dev->Profile.BthFreeBrb((PBRB)brb);
    return status;
}

NTSTATUS Oa2dpQueryRemoteAddress(_In_ OA2DP_DEVICE_CONTEXT *dev)
{
    BTH_DEVICE_INFO info;
    WDF_MEMORY_DESCRIPTOR out;
    NTSTATUS status;
    RtlZeroMemory(&info, sizeof(info));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out, &info, sizeof(info));
    /* BTHENUM tells a profile driver which remote device it serves. */
    status = WdfIoTargetSendInternalIoctlSynchronously(
        dev->IoTarget, NULL, IOCTL_INTERNAL_BTHENUM_GET_DEVINFO, NULL, &out, NULL, NULL);
    if (NT_SUCCESS(status))
        dev->RemoteAddress = info.address;
    return status;
}

/* ------------------------------------------------------------------------ */
/* Indications (remote connect / disconnect)                                */
/* ------------------------------------------------------------------------ */

static EVT_WDF_WORKITEM CloseWorkItem;
static EVT_WDF_REQUEST_COMPLETION_ROUTINE OpenResponseDone;

NTSTATUS Oa2dpInitializeChannels(OA2DP_DEVICE_CONTEXT *dev)
{
    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_WORKITEM_CONFIG work;
    NTSTATUS status;
    for (ULONG i = 0; i < Oa2dpChannelCount; ++i) {
        OA2DP_CHANNEL *ch = &dev->Channels[i];
        KeInitializeEvent(&ch->OpenFinished, NotificationEvent, TRUE);
        KeInitializeEvent(&ch->SubmissionsIdle, NotificationEvent, TRUE);
        WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
        attrs.ParentObject = dev->Device;
        status = WdfWaitLockCreate(&attrs, &ch->CloseLock);
        if (!NT_SUCCESS(status)) return status;
        WDF_WORKITEM_CONFIG_INIT(&work, CloseWorkItem);
        work.AutomaticSerialization = FALSE;
        WDF_OBJECT_ATTRIBUTES_SET_CONTEXT_TYPE(&attrs, OA2DP_WORKITEM_CONTEXT);
        status = WdfWorkItemCreate(&work, &attrs, &ch->CloseWork);
        if (!NT_SUCCESS(status)) return status;
        Oa2dpGetWorkItem(ch->CloseWork)->Channel = ch;
    }
    return STATUS_SUCCESS;
}

static void FillChannelConfig(struct _BRB_L2CA_OPEN_CHANNEL *brb, OA2DP_CHANNEL *ch, USHORT inMtu)
{
    brb->ChannelFlags = CF_ROLE_EITHER;
    brb->ConfigIn.Flags = CFG_MTU;
    brb->ConfigIn.Mtu.Max = inMtu;
    brb->ConfigIn.Mtu.Min = 48;
    brb->ConfigIn.Mtu.Preferred = inMtu;
    brb->ConfigOut.Flags = 0; /* accept whatever the headset can receive */
    brb->CallbackFlags = CALLBACK_DISCONNECT;
    brb->Callback = (PFNBTHPORT_INDICATION_CALLBACK)NULL; /* set by caller */
    brb->CallbackContext = ch;
    brb->ReferenceObject = WdfDeviceWdmGetDeviceObject(ch->Dev->Device);
    brb->IncomingQueueDepth = OA2DP_INCOMING_QUEUE_DEPTH;
}

static void IndicationCallback(_In_ PVOID Context, _In_ INDICATION_CODE Indication,
                               _In_ PINDICATION_PARAMETERS Params);

static void AcceptIncoming(OA2DP_DEVICE_CONTEXT *dev, PINDICATION_PARAMETERS p)
{
    OA2DP_CHANNEL *ch = NULL;
    struct _BRB_L2CA_OPEN_CHANNEL *brb;
    WDFREQUEST req;
    WDF_OBJECT_ATTRIBUTES attrs;
    ULONG generation = 0;

    WdfSpinLockAcquire(dev->Lock);
    /* First channel from the headset is signalling, the second is media. */
    if (dev->PolicyConnected && !dev->TransportStopping) {
        ch = &dev->Channels[Oa2dpChannelSignaling];
        generation = Oa2dpChannelBeginOpen(&ch->Life, TRUE, FALSE);
        if (!generation && ch->Life.State == ChannelOpen && !ch->Life.CloseRequested) {
            ch = &dev->Channels[Oa2dpChannelMedia];
            generation = Oa2dpChannelBeginOpen(&ch->Life, TRUE, FALSE);
        }
        if (generation) {
            ch->Life.ConnectionGeneration = ch->Kind == Oa2dpChannelSignaling
                ? generation : dev->Channels[Oa2dpChannelSignaling].Life.Generation;
            KeClearEvent(&ch->OpenFinished);
            KeClearEvent(&dev->DisconnectedEvent);
        }
    }
    if (!generation) ch = NULL;
    WdfSpinLockRelease(dev->Lock);

    brb = (struct _BRB_L2CA_OPEN_CHANNEL *)dev->Profile.BthAllocateBrb(
        BRB_L2CA_OPEN_CHANNEL_RESPONSE, OA2DP_POOL_TAG);
    if (!brb)
        goto fail_state;
    brb->BtAddress = p->BtAddress;
    brb->Psm = p->Parameters.Connect.Request.PSM;
    brb->ChannelHandle = p->ConnectionHandle;
    brb->Response = ch ? CONNECT_RSP_RESULT_SUCCESS : CONNECT_RSP_RESULT_NO_RESOURCES;
    if (ch) {
        FillChannelConfig(brb, ch, ch->Kind == Oa2dpChannelMedia ? OA2DP_DEFAULT_MEDIA_MTU
                                                                 : OA2DP_DEFAULT_SIGNALING_MTU);
        brb->Callback = IndicationCallback;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, OA2DP_REQUEST_CONTEXT);
    attrs.ParentObject = dev->Device;
    if (!NT_SUCCESS(WdfRequestCreate(&attrs, dev->IoTarget, &req)))
        goto fail_brb;
    Oa2dpGetRequest(req)->Brb = (struct _BRB *)brb;
    Oa2dpGetRequest(req)->Channel = ch;
    Oa2dpGetRequest(req)->Generation = generation;
    if (SubmitBrbAsync(dev, req, (struct _BRB *)brb, sizeof(*brb), OpenResponseDone, dev, 8000) !=
        STATUS_PENDING) {
        WdfObjectDelete(req);
        goto fail_brb;
    }
    return;

fail_brb:
    dev->Profile.BthFreeBrb((PBRB)brb);
fail_state:
    if (ch) {
        WdfSpinLockAcquire(dev->Lock);
        (void)Oa2dpChannelFinishOpen(&ch->Life, generation, FALSE);
        KeSetEvent(&ch->OpenFinished, IO_NO_INCREMENT, FALSE);
        WdfSpinLockRelease(dev->Lock);
    }
}

static void OpenResponseDone(_In_ WDFREQUEST Request, _In_ WDFIOTARGET Target,
                             _In_ PWDF_REQUEST_COMPLETION_PARAMS Params, _In_ WDFCONTEXT Context)
{
    OA2DP_DEVICE_CONTEXT *dev = (OA2DP_DEVICE_CONTEXT *)Context;
    OA2DP_REQUEST_CONTEXT *rc = Oa2dpGetRequest(Request);
    struct _BRB_L2CA_OPEN_CHANNEL *brb = (struct _BRB_L2CA_OPEN_CHANNEL *)rc->Brb;
    OA2DP_CHANNEL *ch = rc->Channel;
    UNREFERENCED_PARAMETER(Target);

    if (ch) {
        BOOLEAN close;
        BOOLEAN opened;
        WdfSpinLockAcquire(dev->Lock);
        opened = Oa2dpChannelFinishOpen(&ch->Life, rc->Generation,
                                        NT_SUCCESS(Params->IoStatus.Status));
        if (opened && NT_SUCCESS(Params->IoStatus.Status)) {
            ch->Handle = brb->ChannelHandle;
            ch->OutMtu = brb->OutResults.Params.Mtu;
            ch->InMtu = brb->InResults.Params.Mtu;
        }
        close = opened && ch->Life.State == ChannelOpen &&
            (ch->Life.CloseRequested || !dev->PolicyConnected || dev->TransportStopping);
        if (close) Oa2dpChannelRequestClose(&ch->Life);
        if (opened) KeSetEvent(&ch->OpenFinished, IO_NO_INCREMENT, FALSE);
        // Queue OPEN before making this lock available to a peer MEDIA open
        // or a remote CLOSE. Event delivery keeps the same transition order.
        if (!close && opened && NT_SUCCESS(Params->IoStatus.Status))
            Oa2dpQueueEventLocked(dev, Oa2dpEventChannelOpened, ch, rc->Generation);
        WdfSpinLockRelease(dev->Lock);
        if (close)
            WdfWorkItemEnqueue(ch->CloseWork);
        Oa2dpDeliverEvents(dev);
    }
    dev->Profile.BthFreeBrb((PBRB)brb);
    WdfObjectDelete(Request);
}

static void IndicationCallback(_In_ PVOID Context, _In_ INDICATION_CODE Indication,
                               _In_ PINDICATION_PARAMETERS Params)
{
    /* Context is the device for server indications and the channel for
     * per-channel indications; both start with a pointer we can test. */
    switch (Indication) {
    case IndicationRemoteConnect: {
        OA2DP_DEVICE_CONTEXT *dev = (OA2DP_DEVICE_CONTEXT *)Context;
        if (Params->Parameters.Connect.Request.PSM == OA2DP_AVDTP_PSM &&
            Params->BtAddress == dev->RemoteAddress)
            AcceptIncoming(dev, Params);
        break;
    }
    case IndicationRemoteDisconnect: {
        OA2DP_CHANNEL *ch = (OA2DP_CHANNEL *)Context;
        OA2DP_DEVICE_CONTEXT *dev = ch->Dev;
        WdfSpinLockAcquire(dev->Lock);
        if (ch->Life.State != ChannelOpen || Params->ConnectionHandle != ch->Handle) {
            WdfSpinLockRelease(dev->Lock);
            break;
        }
        Oa2dpChannelRequestClose(&ch->Life);
        WdfSpinLockRelease(dev->Lock);
        /* Closing the handle needs PASSIVE_LEVEL; do it from a work item. */
        WdfWorkItemEnqueue(ch->CloseWork);
        break;
    }
    default:
        break;
    }
}

static VOID CloseWorkItem(_In_ WDFWORKITEM WorkItem)
{
    OA2DP_CHANNEL *ch = Oa2dpGetWorkItem(WorkItem)->Channel;
    OA2DP_DEVICE_CONTEXT *dev = ch->Dev;
    ULONG generation;
    WdfSpinLockAcquire(dev->Lock);
    generation = ch->Life.CloseRequested ? ch->Life.Generation : 0;
    WdfSpinLockRelease(dev->Lock);
    if (generation && NT_SUCCESS(CloseChannelGeneration(dev, ch->Kind, generation, 0, NULL))) {
        (void)Oa2dpPublishConnected(dev, FALSE, 0);
    }
}

/* ------------------------------------------------------------------------ */
/* Server registration                                                      */
/* ------------------------------------------------------------------------ */

NTSTATUS Oa2dpRegisterServer(_In_ OA2DP_DEVICE_CONTEXT *dev)
{
    NTSTATUS status;
    struct _BRB_PSM *psm = (struct _BRB_PSM *)dev->Profile.BthAllocateBrb(BRB_REGISTER_PSM,
                                                                          OA2DP_POOL_TAG);
    if (!psm)
        return STATUS_INSUFFICIENT_RESOURCES;
    psm->Psm = OA2DP_AVDTP_PSM;
    status = Oa2dpSubmitBrbSync(dev, (struct _BRB *)psm, sizeof(*psm));
    dev->Profile.BthFreeBrb((PBRB)psm);
    if (!NT_SUCCESS(status))
        return status;
    dev->PsmRegistered = TRUE;

    struct _BRB_L2CA_REGISTER_SERVER *srv = (struct _BRB_L2CA_REGISTER_SERVER *)
        dev->Profile.BthAllocateBrb(BRB_L2CA_REGISTER_SERVER, OA2DP_POOL_TAG);
    if (!srv)
        return STATUS_INSUFFICIENT_RESOURCES;
    srv->BtAddress = dev->RemoteAddress; /* only our headset */
    srv->PSM = 0;                        /* every PSM we registered */
    srv->IndicationCallback = IndicationCallback;
    srv->IndicationCallbackContext = dev;
    srv->IndicationFlags = 0;
    srv->ReferenceObject = WdfDeviceWdmGetDeviceObject(dev->Device);
    status = Oa2dpSubmitBrbSync(dev, (struct _BRB *)srv, sizeof(*srv));
    if (NT_SUCCESS(status))
        dev->ServerHandle = srv->ServerHandle;
    dev->Profile.BthFreeBrb((PBRB)srv);
    return status;
}

VOID Oa2dpUnregisterServer(_In_ OA2DP_DEVICE_CONTEXT *dev)
{
    if (dev->ServerHandle) {
        struct _BRB_L2CA_UNREGISTER_SERVER *b = (struct _BRB_L2CA_UNREGISTER_SERVER *)
            dev->Profile.BthAllocateBrb(BRB_L2CA_UNREGISTER_SERVER, OA2DP_POOL_TAG);
        if (b) {
            b->BtAddress = dev->RemoteAddress;
            b->Psm = 0;
            b->ServerHandle = dev->ServerHandle;
            (void)Oa2dpSubmitBrbSync(dev, (struct _BRB *)b, sizeof(*b));
            dev->Profile.BthFreeBrb((PBRB)b);
        }
        dev->ServerHandle = NULL;
    }
    if (dev->PsmRegistered) {
        struct _BRB_PSM *psm = (struct _BRB_PSM *)dev->Profile.BthAllocateBrb(
            BRB_UNREGISTER_PSM, OA2DP_POOL_TAG);
        if (psm) {
            psm->Psm = OA2DP_AVDTP_PSM;
            (void)Oa2dpSubmitBrbSync(dev, (struct _BRB *)psm, sizeof(*psm));
            dev->Profile.BthFreeBrb((PBRB)psm);
        }
        dev->PsmRegistered = FALSE;
    }
}

/* ------------------------------------------------------------------------ */
/* Outgoing open / close                                                    */
/* ------------------------------------------------------------------------ */

static EVT_WDF_REQUEST_COMPLETION_ROUTINE OpenDone;

VOID Oa2dpOpenChannel(_In_ OA2DP_DEVICE_CONTEXT *dev, _In_ WDFREQUEST request,
                      _In_ const OA2DP_OPEN_CHANNEL_IN *in)
{
    OA2DP_CHANNEL *ch = &dev->Channels[in->Kind];
    struct _BRB_L2CA_OPEN_CHANNEL *brb;
    WDFREQUEST lower;
    WDF_OBJECT_ATTRIBUTES attrs;
    NTSTATUS status;
    ULONG generation;
    ULONG policySerial = 0;
    USHORT mtu = in->InMtu ? in->InMtu
                           : (in->Kind == Oa2dpChannelMedia ? OA2DP_DEFAULT_MEDIA_MTU
                                                            : OA2DP_DEFAULT_SIGNALING_MTU);

    WdfSpinLockAcquire(dev->Lock);
    OA2DP_CHANNEL *signal = &dev->Channels[Oa2dpChannelSignaling];
    if (in->Kind == Oa2dpChannelMedia &&
        !Oa2dpChannelTransferCurrent(&signal->Life, in->ConnectionGeneration,
                                     in->ConnectionGeneration)) {
        WdfSpinLockRelease(dev->Lock);
        WdfRequestComplete(request, STATUS_CONNECTION_DISCONNECTED);
        return;
    }
    generation = Oa2dpChannelBeginOpen(&ch->Life, dev->PolicyConnected,
                                      dev->TransportStopping);
    if (!generation) {
        WdfSpinLockRelease(dev->Lock);
        WdfRequestComplete(request, STATUS_DEVICE_NOT_READY);
        return;
    }
    KeClearEvent(&ch->OpenFinished);
    KeClearEvent(&dev->DisconnectedEvent);
    ch->Life.ConnectionGeneration = ch->Kind == Oa2dpChannelSignaling
        ? generation : dev->Channels[Oa2dpChannelSignaling].Life.Generation;
    if (ch->Kind == Oa2dpChannelSignaling && dev->MpmRequest.Pending && dev->MpmRequest.Target)
        policySerial = dev->MpmRequest.Serial;
    WdfSpinLockRelease(dev->Lock);

    brb = (struct _BRB_L2CA_OPEN_CHANNEL *)dev->Profile.BthAllocateBrb(BRB_L2CA_OPEN_CHANNEL,
                                                                       OA2DP_POOL_TAG);
    if (!brb) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto fail;
    }
    brb->BtAddress = dev->RemoteAddress;
    brb->Psm = OA2DP_AVDTP_PSM;
    FillChannelConfig(brb, ch, mtu);
    brb->Callback = IndicationCallback;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, OA2DP_REQUEST_CONTEXT);
    attrs.ParentObject = dev->Device;
    status = WdfRequestCreate(&attrs, dev->IoTarget, &lower);
    if (!NT_SUCCESS(status)) {
        dev->Profile.BthFreeBrb((PBRB)brb);
        goto fail;
    }
    Oa2dpGetRequest(lower)->Brb = (struct _BRB *)brb;
    Oa2dpGetRequest(lower)->Channel = ch;
    Oa2dpGetRequest(lower)->Generation = generation;
    Oa2dpGetRequest(lower)->PolicySerial = policySerial;
    Oa2dpGetRequest(lower)->UserRequest = request;
    status = SubmitBrbAsync(dev, lower, (struct _BRB *)brb, sizeof(*brb), OpenDone, dev, 8000);
    if (status != STATUS_PENDING) {
        WdfObjectDelete(lower);
        dev->Profile.BthFreeBrb((PBRB)brb);
        goto fail;
    }
    return;

fail:
    WdfSpinLockAcquire(dev->Lock);
    (void)Oa2dpChannelFinishOpen(&ch->Life, generation, FALSE);
    KeSetEvent(&ch->OpenFinished, IO_NO_INCREMENT, FALSE);
    WdfSpinLockRelease(dev->Lock);
    Oa2dpMpmOpenFailed(dev, policySerial);
    WdfRequestComplete(request, status);
}

static void OpenDone(_In_ WDFREQUEST Lower, _In_ WDFIOTARGET Target,
                     _In_ PWDF_REQUEST_COMPLETION_PARAMS Params, _In_ WDFCONTEXT Context)
{
    OA2DP_DEVICE_CONTEXT *dev = (OA2DP_DEVICE_CONTEXT *)Context;
    OA2DP_REQUEST_CONTEXT *rc = Oa2dpGetRequest(Lower);
    struct _BRB_L2CA_OPEN_CHANNEL *brb = (struct _BRB_L2CA_OPEN_CHANNEL *)rc->Brb;
    OA2DP_CHANNEL *ch = rc->Channel;
    NTSTATUS status = Params->IoStatus.Status;
    WDFREQUEST user = rc->UserRequest;
    OA2DP_CHANNEL_INFO *out = NULL;
    ULONG_PTR info = 0;
    BOOLEAN close = FALSE;
    OA2DP_CHANNEL_INFO openedInfo = {0};
    BOOLEAN current;
    UNREFERENCED_PARAMETER(Target);

    WdfSpinLockAcquire(dev->Lock);
    current = Oa2dpChannelFinishOpen(&ch->Life, rc->Generation, NT_SUCCESS(status));
    if (current && NT_SUCCESS(status)) {
        ch->Handle = brb->ChannelHandle;
        ch->OutMtu = brb->OutResults.Params.Mtu;
        ch->InMtu = brb->InResults.Params.Mtu;
        close = ch->Life.CloseRequested || !dev->PolicyConnected || dev->TransportStopping;
        if (close) Oa2dpChannelRequestClose(&ch->Life);
        openedInfo.Kind = ch->Kind;
        openedInfo.OutMtu = ch->OutMtu;
        openedInfo.InMtu = ch->InMtu;
        openedInfo.Generation = rc->Generation;
        openedInfo.ConnectionGeneration = ch->Life.ConnectionGeneration;
    }
    if (current) KeSetEvent(&ch->OpenFinished, IO_NO_INCREMENT, FALSE);
    WdfSpinLockRelease(dev->Lock);
    if (current && !NT_SUCCESS(status) && ch->Kind == Oa2dpChannelSignaling)
        Oa2dpMpmOpenFailed(dev, rc->PolicySerial);
    if (!current) status = STATUS_CANCELLED;
    if (close) {
        WdfWorkItemEnqueue(ch->CloseWork);
        status = STATUS_CANCELLED;
    }

    if (user) {
        if (NT_SUCCESS(status) &&
            NT_SUCCESS(WdfRequestRetrieveOutputBuffer(user, sizeof(*out), (PVOID *)&out, NULL))) {
            *out = openedInfo;
            info = sizeof(*out);
        }
        WdfRequestCompleteWithInformation(user, status, info);
    }
    dev->Profile.BthFreeBrb((PBRB)brb);
    WdfObjectDelete(Lower);
}

static NTSTATUS CloseChannelGeneration(OA2DP_DEVICE_CONTEXT *dev,
                                       OA2DP_CHANNEL_KIND kind, ULONG expectedGeneration,
                                       ULONG expectedConnection, PULONG closedConnection)
{
    OA2DP_CHANNEL *ch = &dev->Channels[kind];
    struct _BRB_L2CA_CLOSE_CHANNEL *brb;
    L2CAP_CHANNEL_HANDLE handle;
    NTSTATUS status;
    ULONG generation;
    BOOLEAN opening;
    BOOLEAN closed;

    // The remote-close worker and service can arrive concurrently. Exactly
    // one owns a CLOSE BRB, and a pending OPEN is never treated as closed.
    status = WdfWaitLockAcquire(ch->CloseLock, NULL);
    if (!NT_SUCCESS(status)) return status;
    WdfSpinLockAcquire(dev->Lock);
    generation = ch->Life.Generation;
    if (closedConnection)
        *closedConnection = expectedConnection ? expectedConnection : ch->Life.ConnectionGeneration;
    if (expectedConnection &&
        !Oa2dpChannelCloseTargets(&ch->Life, 0, expectedConnection)) {
        // The requested old connection has ended. It does not own this
        // replacement handle, so closing it would destroy a fresh link.
        WdfSpinLockRelease(dev->Lock);
        status = STATUS_SUCCESS;
        goto done;
    }
    if (expectedGeneration && generation != expectedGeneration) {
        WdfSpinLockRelease(dev->Lock);
        status = STATUS_NOT_FOUND;
        goto done;
    }
    Oa2dpChannelRequestClose(&ch->Life);
    opening = ch->Life.State == ChannelOpening;
    WdfSpinLockRelease(dev->Lock);
    if (opening) {
        LARGE_INTEGER timeout;
        timeout.QuadPart = -10LL * 1000 * 1000 * 10;
        status = KeWaitForSingleObject(&ch->OpenFinished, Executive, KernelMode, FALSE, &timeout);
        if (status != STATUS_SUCCESS) {
            // Keep CloseRequested latched. A late successful OPEN queues the
            // preallocated close worker and cannot become a usable channel.
            if (status == STATUS_TIMEOUT) status = STATUS_IO_TIMEOUT;
            goto done;
        }
    }
    WdfSpinLockAcquire(dev->Lock);
    if (ch->Life.Generation != generation) {
        WdfSpinLockRelease(dev->Lock);
        status = STATUS_NOT_FOUND;
        goto done;
    }
    if (ch->Life.State == ChannelClosed) {
        WdfSpinLockRelease(dev->Lock);
        status = STATUS_SUCCESS;
        goto done;
    }
    Oa2dpChannelRequestClose(&ch->Life);
    handle = ch->Handle;
    WdfSpinLockRelease(dev->Lock);

    // Closing rejects new leases under the same lock used by transfers. An
    // already-admitted submit must reach BTHPORT before its handle is closed.
    // Do not wait for transfer completion: an idle receive needs CLOSE to end.
    (void)KeWaitForSingleObject(&ch->SubmissionsIdle, Executive, KernelMode, FALSE, NULL);

    brb = (struct _BRB_L2CA_CLOSE_CHANNEL *)dev->Profile.BthAllocateBrb(BRB_L2CA_CLOSE_CHANNEL,
                                                                        OA2DP_POOL_TAG);
    if (!brb) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }
    brb->BtAddress = dev->RemoteAddress;
    brb->ChannelHandle = handle;
    status = Oa2dpSubmitBrbSync(dev, (struct _BRB *)brb, sizeof(*brb));
    dev->Profile.BthFreeBrb((PBRB)brb);

    closed = NT_SUCCESS(status) || status == STATUS_CONNECTION_DISCONNECTED ||
             status == STATUS_INVALID_HANDLE;
    WdfSpinLockAcquire(dev->Lock);
    if (Oa2dpChannelFinishClose(&ch->Life, generation, closed)) {
        ch->Handle = NULL;
        if (expectedGeneration)
            Oa2dpQueueEventLocked(dev, Oa2dpEventChannelClosed, ch, generation);
    }
    WdfSpinLockRelease(dev->Lock);
    if (closed && expectedGeneration) Oa2dpDeliverEvents(dev);
    if (closed) status = STATUS_SUCCESS;
    /* The caller already knows a local close completed. Only the remote
     * disconnect work item emits a notification; a local echo could tear
     * down a new connection after the service reconnects. */
done:
    WdfWaitLockRelease(ch->CloseLock);
    return status;
}

NTSTATUS Oa2dpCloseChannel(OA2DP_DEVICE_CONTEXT *dev, OA2DP_CHANNEL_KIND kind)
{
    return CloseChannelGeneration(dev, kind, 0, 0, NULL);
}

NTSTATUS Oa2dpCloseConnectionChannel(OA2DP_DEVICE_CONTEXT *dev, OA2DP_CHANNEL_KIND kind,
                                    ULONG expectedConnection, PULONG closedConnection)
{
    return CloseChannelGeneration(dev, kind, 0, expectedConnection, closedConnection);
}

VOID Oa2dpCloseAllChannels(_In_ OA2DP_DEVICE_CONTEXT *dev)
{
    /* Media first: AVDTP closes the transport before signalling. */
    (void)Oa2dpCloseChannel(dev, Oa2dpChannelMedia);
    (void)Oa2dpCloseChannel(dev, Oa2dpChannelSignaling);
}

/* ------------------------------------------------------------------------ */
/* Data transfer                                                            */
/* ------------------------------------------------------------------------ */

static EVT_WDF_REQUEST_COMPLETION_ROUTINE TransferDone;

VOID Oa2dpTransfer(_In_ OA2DP_DEVICE_CONTEXT *dev, _In_ WDFREQUEST request,
                   _In_ const OA2DP_SEND_IN *in, _In_ BOOLEAN send)
{
    const OA2DP_CHANNEL_KIND kind = (OA2DP_CHANNEL_KIND)in->Kind;
    OA2DP_CHANNEL *ch = &dev->Channels[kind];
    struct _BRB_L2CA_ACL_TRANSFER *brb;
    PMDL mdl;
    NTSTATUS status;
    L2CAP_CHANNEL_HANDLE handle;
    USHORT outMtu;
    ULONG len;

    status = WdfRequestRetrieveOutputWdmMdl(request, &mdl);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(request, status);
        return;
    }
    len = MmGetMdlByteCount(mdl);

    WdfSpinLockAcquire(dev->Lock);
    handle = ch->Handle;
    outMtu = ch->OutMtu;
    status = Oa2dpChannelTransferCurrent(&ch->Life, in->Generation, in->ConnectionGeneration)
        ? STATUS_SUCCESS : STATUS_CONNECTION_DISCONNECTED;
    if (NT_SUCCESS(status) && send && len > outMtu)
        status = STATUS_INVALID_BUFFER_SIZE;
    if (NT_SUCCESS(status)) {
        OA2DP_CHANNEL *signal = &dev->Channels[Oa2dpChannelSignaling];
        if (dev->TransportStopping || !dev->PolicyConnected ||
            !Oa2dpChannelTransferCurrent(&signal->Life, in->ConnectionGeneration,
                                          in->ConnectionGeneration) ||
            !Oa2dpChannelBeginSubmit(&ch->Life, in->Generation, in->ConnectionGeneration))
            status = STATUS_CONNECTION_DISCONNECTED;
        else if (ch->Life.Submissions == 1)
            KeClearEvent(&ch->SubmissionsIdle);
    }
    WdfSpinLockRelease(dev->Lock);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(request, status);
        return;
    }

    brb = (struct _BRB_L2CA_ACL_TRANSFER *)dev->Profile.BthAllocateBrb(BRB_L2CA_ACL_TRANSFER,
                                                                       OA2DP_POOL_TAG);
    if (!brb) {
        WdfSpinLockAcquire(dev->Lock);
        if (Oa2dpChannelEndSubmit(&ch->Life)) KeSetEvent(&ch->SubmissionsIdle, IO_NO_INCREMENT, FALSE);
        WdfSpinLockRelease(dev->Lock);
        WdfRequestComplete(request, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }
    brb->BtAddress = dev->RemoteAddress;
    brb->ChannelHandle = handle;
    brb->TransferFlags = send ? ACL_TRANSFER_DIRECTION_OUT
                              : (ACL_TRANSFER_DIRECTION_IN | ACL_SHORT_TRANSFER_OK);
    brb->BufferMDL = mdl;
    brb->Buffer = NULL;
    brb->BufferSize = len;
    brb->Timeout = 0;

    OA2DP_REQUEST_CONTEXT *rc = Oa2dpGetRequest(request);
    rc->Brb = (struct _BRB *)brb;
    rc->Channel = ch;
    rc->Bytes = len;

    if (send && kind == Oa2dpChannelMedia) {
        InterlockedIncrement(&dev->TxQueuedPackets);
        InterlockedExchangeAdd(&dev->TxQueuedBytes, (LONG)len);
    }
    status = SubmitBrbAsync(dev, request, (struct _BRB *)brb, sizeof(*brb), TransferDone, dev, 0);
    // Completion may already have run and completed request. Only touch the
    // stable channel/device here; the completion routine never owns this lease.
    WdfSpinLockAcquire(dev->Lock);
    if (Oa2dpChannelEndSubmit(&ch->Life)) KeSetEvent(&ch->SubmissionsIdle, IO_NO_INCREMENT, FALSE);
    WdfSpinLockRelease(dev->Lock);
    if (status != STATUS_PENDING) {
        if (send && kind == Oa2dpChannelMedia) {
            InterlockedDecrement(&dev->TxQueuedPackets);
            InterlockedExchangeAdd(&dev->TxQueuedBytes, -(LONG)len);
        }
        dev->Profile.BthFreeBrb((PBRB)brb);
        WdfRequestComplete(request, status);
    }
}

static void TransferDone(_In_ WDFREQUEST Request, _In_ WDFIOTARGET Target,
                         _In_ PWDF_REQUEST_COMPLETION_PARAMS Params, _In_ WDFCONTEXT Context)
{
    OA2DP_DEVICE_CONTEXT *dev = (OA2DP_DEVICE_CONTEXT *)Context;
    OA2DP_REQUEST_CONTEXT *rc = Oa2dpGetRequest(Request);
    struct _BRB_L2CA_ACL_TRANSFER *brb = (struct _BRB_L2CA_ACL_TRANSFER *)rc->Brb;
    NTSTATUS status = Params->IoStatus.Status;
    BOOLEAN send = (brb->TransferFlags & ACL_TRANSFER_DIRECTION_IN) == 0;
    ULONG bytes = NT_SUCCESS(status) ? (ULONG)brb->BufferSize : 0;
    UNREFERENCED_PARAMETER(Target);

    if (send && rc->Channel->Kind == Oa2dpChannelMedia) {
        InterlockedDecrement(&dev->TxQueuedPackets);
        InterlockedExchangeAdd(&dev->TxQueuedBytes, -(LONG)rc->Bytes);
        if (NT_SUCCESS(status))
            InterlockedIncrement64(&dev->TxSent);
        else
            InterlockedIncrement64(&dev->TxFailed);
    }
    dev->Profile.BthFreeBrb((PBRB)brb);
    rc->Brb = NULL;
    WdfRequestCompleteWithInformation(Request, status, bytes);
}
