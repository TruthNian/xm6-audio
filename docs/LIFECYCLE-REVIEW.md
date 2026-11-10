# Native lifecycle review

## 0.3.36: original-request outgoing OPEN, source only

Outgoing OPEN previously allocated a separate WDF request and kept the caller
waiting for that request's completion. CancelIoEx on the caller did not directly
cancel the lower OPEN. The driver now formats and forwards the original request,
as the ACL transfer path already does. This removes one WDF request allocation
per outgoing OPEN and the separate caller/lower lifetime bridge. The lower
8-second timeout remains; cancellation still depends on BTHPORT completing I/O.

The original output buffer is validated before starting a channel generation,
then retained until completion. Input/output can alias, so no output is written
before the input is consumed. In completion, when the driver owns the request
again, it checks cancellation. A late successful cancelled OPEN keeps the real
handle, latches closing and queues the existing close worker instead of exposing
an apparently usable channel. Failed current attempts still notify the Windows
state-publication path; obsolete failed completions cannot fail a replacement.
The BRB and request-context pointers are cleared before completing the caller;
the submit path does not touch the request after possible inline completion.

The implementation follows Microsoft's [request forwarding](https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/forwarding-i-o-requests),
[buffer lifetime](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestretrieveoutputbuffer)
and [cancellation ownership](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestiscanceled)
contracts. A production-body fake-API regression checks original-request identity,
invalid output, cancellation before/after submission, late success, inline
completion, allocation/send/OPEN failures, closing policy, stale failed
completion, and aliased signalling/media output. It does not emulate BTHPORT or
prove actual Windows Settings reconnect behavior. No host device, service,
installed package or boot policy was changed. ABI remains 11/bus 5/ring 2.

The local kernel/service build and all eleven CTest checks pass. As a regression
sensitivity check, replacing the completion's cancellation query with FALSE in
the generated build-only fixture produced the expected test failure; restoring
the production body passed. This does not establish a hardware performance
improvement or eliminate the private MPM call/reentry gap described below.

## 0.3.35: PCM notifications and cancellable media waits, source only

This batch changes the driver/service ABI to 11; both drivers and the service
must be deployed as one matching package. Ring memory layout remains v2 and
the kernel bus interface remains v5. It has not been installed on the developer
machine, whose last verified runtime is 0.3.32. Headset testing is still deferred.

### Data readiness instead of encoder polling

The encoder no longer calls Sleep(2) to poll PCM. It registers one auto-reset
event with the audio driver, using its actual samples-per-call as the wake
threshold. After producer commits the driver signals if a full block is
available; registration also signals already-buffered data. The consumer
checks the ring before waiting. Format and RUN changes signal the same event,
while separate stream/service stop events cancel the wait immediately. The
WaveRT software position timer remains; removing service polling is not a
claim that the whole virtual device is interrupt-driven.

The driver references the supplied event with the caller's access mode and
event object type, rejects a user handle in the wrong process context, and
holds the ring lock during use/replacement. It releases old references outside
the lock and unregisters on unmap/control cleanup, including an unmapped view.
See [ObReferenceObjectByHandle](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-obreferenceobjectbyhandle)
and [KeSetEvent](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-kesetevent).

WAIT_FORMAT/GET_STATE now carry the ring's committed PCM sequence, alongside
the format. A coalesced A -> B -> A change still restarts the old encoder.
Format and its epoch are committed together; startup defers if the snapshot
and ring differ. Reports whose format no longer matches the ring are ignored.
This avoids waiting forever for an old sequence after apparently unchanged
metadata, without reinterpreting samples from another epoch.

### Media cancellation and status publication

Media-slot waits accept the stream stop event and collect completion without
an unbounded GetOverlappedResult(TRUE) on the encoding thread. On timeout or
cancellation, pending OVERLAPPED objects and buffers remain owned by Transport.
The retiring encoder cancels its sends and uses one 250 ms total drain budget,
not a separate second per slot. An incomplete drain prevents new-slot reuse;
the existing session loop resumes only after those requests retire.

Final object destruction still waits for outstanding kernel I/O before freeing
its memory. Generic stack-buffer IOCTL cancellation and signalling receive
retirement retain their existing safety waits. There is no universal hard
shutdown deadline if the lower driver never completes an operation. This
batch bounds the realtime media path; it does not hide that remaining limit.

After physical close, failure of suspend/jack publication is reported as
state-update-pending and retried in the existing main loop while that link
remains closed. A successful new TRUE publication clears the obsolete retry.
The explicit user-disconnect path also preserves close/publication errors.
Periodic registry status writes moved off the encoder thread, and state
publication releases its status lock before registry I/O.

### Opt-in timing evidence

The service registers XM6Audio.Stream
(`{612cfc30-2fe1-4e0e-9b90-e23f67725a61}`). At verbose level its PcmWindow event
records data-wait count, maximum blocked-wait duration, sampled peak buffered
frames and overrun deltas about once per second. MaxDataWaitUs includes time
waiting for production; it is not producer-to-scheduler wakeup latency.
No addresses, filenames, audio samples or account data are recorded.

The existing XM6Audio.Transport provider adds opt-in verbose MediaSendComplete
events measuring driver submission-to-completion time for media sends.
These include neither radio packet contents nor an acoustic latency estimate.
Captures can be correlated with the existing WaveRT state/format events to
separate paused production, buffering and transport completion. No live trace
was started for this source batch. No measured hardware performance gain or
optimality claim follows from adding these probes.

### Offline evidence and limits

New checks compile the actual production ring-notification, media-slot/drain
and disconnect-publication bodies against fake OS APIs. They cover commit
before wait, threshold accumulation, event replacement/reference release,
format/RUN wakeup, A -> B -> A epoch changes, stale-format writes, stop
cancellation, timeout with retained I/O ownership, one shared drain budget,
spurious completion events and publication failures. Full local kernel/service
build and all ten CTest checks pass. The fake fixtures do not prove real KMDF
callback timing, Windows event security, RF behavior or audible playback.
Settings state, reconnect, existing-client recovery and measured performance
remain hardware acceptance requirements.

## 0.3.34: transport-owned disconnect publication, source only

Local channel close formerly relied on the encoder service subsequently
calling SET_CONNECTED through the audio child. But AudioUnavailable closes
that handle before transport teardown; its cleanup can unplug the jack while
the channels are still open. Once the transport later closed, no child handle
remained to clear the parent interface/MPM status or signal DisconnectedEvent.
The separate peer-close worker published FALSE, but the normal local-close
path did not. A pending Connect failure was also incorrectly a prerequisite
for queuing publication after a failed OPEN.

The common channel-close path now queues the parent's existing publication
worker after releasing CloseLock, including an already-closed reconciliation.
Current failed OPEN completions also queue it independently of manager-request
type. The worker rechecks the actual pair of channels and connection generation
under its existing publication serialization. Failed closes retain ownership;
obsolete closes do not publish against a replacement. Local closes still emit
no ChannelClosed echo to the service. The duplicate peer-worker publication
and request-dependent queuing were removed.

The new channel_close regression compiles the production CloseChannelGeneration
body against fake OS calls, rather than testing a rewritten close model. Named
source boundaries and configure dependencies prevent silently testing an old
copy. The 0.3.33 body fails its local-close publication assertion; the corrected
body passes. Checks also cover close failure/retry, stale generations,
already-closed reconciliation, peer-only service events, allocation failure,
power quiescence and pending-OPEN timeout. All seven local CTest checks and the
complete kernel/service build pass. This does not exercise actual KMDF
scheduling, radio behavior, Settings or audible recovery.

Repeated enqueue is supported by the project's KMDF 1.15 minimum. The worker
reads current state rather than assuming each enqueue represents a separate
transition, and the publication lock serializes potentially overlapping
callbacks. See Microsoft's [WdfWorkItemEnqueue contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfworkitem/nf-wdfworkitem-wdfworkitemenqueue).

This candidate has not been installed. The user's restriction on further
headset tests remains in effect; Windows Settings acceptance is still pending.

## 0.3.33: request ownership corrections, source only

The user confirmed that the Windows connection label and per-device
Disconnect/Connect are still incorrect on 0.3.32 after restarting Windows.
That build did establish native LDAC playback again. Further headset testing
is unavailable; this batch has not been installed or hardware-accepted.

The source review found three incorrect request boundaries:

- The KS handler called the MPM connection callback itself before asking
  Windows to start a disconnect. This could create a requested-operation
  confirmation before Windows had issued that operation. Connect also lacked
  the local fallback when Windows elides an already-at-target callback.
  The handler now asks MPM first and applies fallback intent only if no
  callback or newer caller superseded its policy revision. Fallback does not
  fabricate a pending manager operation.
- An automatic signalling OPEN could precede the Windows connect callback.
  Its completion retained the old callback serial, so failure could leave
  the adopted request pending. Ownership now follows the exact signalling
  generation. Closing an adopted connection before readiness, including a
  failed SEP negotiation, also reports failure. A closing old generation is
  not adopted by a later reconnect.
- Repair of a stale TRUE publication called the FALSE publisher while
  channels could still be draining. FALSE may withdraw readiness, but it
  cannot complete Disconnect until both channels are closed. Confirmation
  now checks that boundary. TRUE is also revalidated against its generation
  and policy immediately before the external MPM update.

The shared lifecycle checks exercise callback omission, callback/API-return
ordering, adoption of an in-flight OPEN, replaced generations, negotiation
failure, readiness withdrawal during close, and revision wrap. Both drivers
and the service build locally; all six CTest checks pass, including linked
descriptor verification. These checks do not execute Windows' private MPM
implementation or establish that Settings' label/button problem is fixed.
There is still a gap between the checked snapshot and an external MPM call;
the code does not claim that call is atomic with an asynchronous callback.

## 0.3.32 activation and earlier review

This source batch is not a declaration of hardware acceptance. Versions 0.3.23
through 0.3.30 were not installed. A controlled 0.3.31 hot-binding trial returned
installer code 3010, but the installation log also established a real start
failure: CM_PROB_FAILED_START, status 0xC0000184. It was rolled back without a
system reboot; the Microsoft binding was started and both custom images were
absent afterward. Version 0.3.32 corrects a confirmed pre-enumeration property
assignment defect and has now activated without a reboot, with both actual
loaded kernel images and the running codec image matching the verified package.
The headset is not connected at the latest check; actual sound and Settings
behavior remain unaccepted. A reboot cannot repair a source defect.

## Corrected child enumeration boundary

The new PDO's Bluetooth address/service properties were previously assigned
before it was reported to PnP. Microsoft's [WDF implementation](https://github.com/microsoft/Windows-Driver-Frameworks/blob/b6191d9543441329154da32f7ab9bdd97228dd3c/src/framework/shared/core/km/fxdevicekm.cpp#L671)
rejects property assignment when no PnP-known physical device is available,
with STATUS_INVALID_DEVICE_STATE. The old placement could not succeed on the
new PDO. The 0.3.31 trial's actual failure status matched that contract.

The 0.3.32 child preprocesses [IRP_MN_DEVICE_ENUMERATED](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/irp-mn-device-enumerated)
and writes only the two required properties with IoSetDevicePropertyData on
its PDO, then returns the IRP to WDF. A WDF property wrapper is deliberately
not used in that preprocess callback, because WDF itself has not consumed
the enumeration notification yet. Parent bus identity and endpoint identity
are retained.

Live ETW reported successful manager/server/child creation, successful child
identity assignment and successful WaveRT port creation. Independent CM
readback confirmed the address's string type/value and the Audio Sink GUID's
GUID type/value. Both parent and child started; actual kernel-image and codec
process paths/hashes were verified. A redundant second audio-INF installation
returned ERROR_NO_MORE_ITEMS after reporting Already exists; the private trial
controller removed that step and waited for normal child auto-enumeration.
No additional kernel change or system reboot was needed. Full local checks
and [0.3.32 CI](https://github.com/TruthNian/xm6-audio/actions/runs/42679246224)
passed. This establishes startup and identity assignment, not audible playback
or complete reconnect acceptance.

## Confirmed source defects and corrections

| Trigger | Defect | Correction |
| --- | --- | --- |
| Disconnect while OPEN BRB is pending | CLOSE returned success for Opening; late OPEN could resurrect a channel | Latch close intent, wait for bounded OPEN completion, close a late successful handle |
| Headset initiates after explicit disconnect | Incoming acceptance ignored local policy | Apply policy before selecting a channel and before exposing a completed OPEN |
| Remote and local CLOSE overlap | Both could submit CLOSE for the same handle; failure erased ownership | Serialize CLOSE per channel, retain the handle after failed CLOSE |
| Delayed notification/work after reconnect | No channel/link incarnation identity; an old close could tear down its replacement | Channel and connection generations in driver/service ABI 7; scope closes to their owning link, validate work/publication/consumption, and bar queued OPENs from already-closed links |
| Delayed encoder failure after reconfiguration | A retired encoder's queued error could terminate its replacement on the same media channel | Independent encoder-generation validation |
| Disconnect completion while transport is unfinished | Jack and parent carried independent completion events | One referenced transport-owned event, validated against both actual channel states; kernel bus ABI 3 |
| PnP stop/resume | A queued DeviceGone from the previous lifetime could terminate the replacement session | Clear the old event backlog before the new PnP lifetime starts |
| Rapid interface removal/arrival | New session could lose the exclusive control-open race and exit permanently | Retry transient open errors on the existing session thread until stop/removal |
| Earlier pending install, later installer returns success | Service/INF metadata was mistaken for active stack state; problem14 triggered rollback | Inspect devnode status/install flags, preserve 3010, defer service activation when reboot is pending |
| Explicit 24-bit format selection | Container width excluded valid 24-in-32 PCM | Filter useful precision separately from container width |
| Retired encoder/read reaches a reconnected channel | SEND/RECV selected whatever handle was current by kind | Mandatory channel/link binding in ABI 8, immutable per-encoder snapshot, persistent RECV binding and cancellation on epoch change; scope outgoing MEDIA OPEN to its signalling link |
| CLOSE races transfer submission | Handle could close between validation and WdfRequestSend | Short per-channel submission leases, drained after Closing rejects admission; never wait for idle RECV completion |
| Slow connection publication overlaps reconnect | An old FALSE could finish after a newer TRUE; event could be set after new OPEN cleared it | One passive publication lock, epoch-bound TRUE, post-call revalidation/repair, event update under OPEN's spin lock; bus ABI 4; serialize child jack updates too |
| Signalling read fails | Synthetic close command had zero epoch and was rejected by the consumer | Tag the close with the failed read's owning connection |
| Unsolicited state change or overlapping Windows requests | Every MPM connection update claimed to complete a requested operation | Distinguish unsolicited status from a matching pending request; serial-bound local retirement and failed outgoing OPEN completion |
| Device enters low power with idle receive/event wait | Power-managed queues had indefinite requests without EvtIoStop; no suspend/restart transport lifecycle | Explicit self-managed suspend/restart, non-power-managed control/event queues, gated/bounded Bluetooth teardown and current power state/serial in ABI 9 |
| Music idles or HFP suspend/resume requests overlap | Encoder stop was equated with profile suspension; cache changed before MPM accepted it, and replies had no request identity | Separate profile availability from idle PCM, echo suspend request serial and link epoch in ABI 10/bus ABI 5, serialize MPM calls, retain failed updates and reject stale replies |
| Server/PSM allocation or registration/unregistration fails | Setup ignored registration failure; cleanup erased ownership even when no unregister BRB could be allocated | Transactional registration state shared with off-host failure injection; retain each resource until confirmed release, gate admission through prepare and finish old ownership before a new registration |
| Repeated PrepareHardware on the same device object | Each query acquired another Bluetooth interface reference and overwrote the previous import; no matching dereference existed | Import the BRB allocation interface once per WDFDEVICE and release it during final device destruction |
| Headset establishes ACL after an unsuccessful page | The service waited for its periodic connection retry even though Windows had a fresh link notification | Subscribe to public target-device HCI notifications; accelerate the existing session's retry with peer-initiator grace, preserving explicit disconnect and actual channel ownership |
| Create the render PDO after the 0.3.21 identity change | WdfDeviceAssignProperty ran before the PDO was known to PnP, deterministically returning STATUS_INVALID_DEVICE_STATE | Assign the two Bluetooth identity properties through IoSetDevicePropertyData during IRP_MN_DEVICE_ENUMERATED, then return the IRP to WDF |

Channel close work is preallocated rather than allocated in a disconnect
indication. Allocation failure can no longer fabricate a completed remote
close. Failed or in-progress transport closes cannot acknowledge the native
disconnect property. An unavailable codec service may unplug its audio jack
before the physical transport is closed; these are distinct states.

OPEN/CLOSE notification enqueueing and their state transitions share the same
lock. A peer MEDIA OPEN cannot overtake its signalling OPEN notification.
Closing an old link returns its generation even if a replacement already
exists; it preserves the replacement handles. Cleanup of unknown pending
state records the closed generation, preventing a queued old OPEN from
recreating a phantom connection in the service. Lost event backlogs instead
force cleanup of unknown generations before negotiating a new link.

Data-plane binding is mandatory even when channel numbers happen to match.
The submission lease ends when asynchronous submission returns, including its
failure path, rather than when Bluetooth completes the transfer. The close
path can therefore end an idle receive without deadlocking on that receive.
No spin lock is held across WdfRequestSend or the external publication calls.

Publication runs at PASSIVE_LEVEL. Policy callbacks enqueue a preallocated
work item instead of directly publishing or acknowledging a stale snapshot.
That worker is drained after MPM callbacks stop during removal. Interface and
MPM errors are returned/logged rather than silently treated as success. This
does not prove the observed Windows-specific MPM contract is sufficient for
Settings; that remains a hardware acceptance item.

The observed inbox MPM call sites distinguish peer connect/disconnect
indications (third argument 0) from requested-operation confirmations (1).
This is an observed Windows 10 compatibility distinction, not a documented
cross-version API. Our former unconditional 1 did not preserve it. The new
production helper tracks an outstanding request, its target and serial; a
failed outgoing signalling OPEN can complete the owning connect with FALSE.
A stale OPEN failure cannot mark a replacement request failed, and a reentrant
request cannot be locally retired by an older external call's return. These
checks do not make the opaque MPM call transactional with policy callbacks or
prove Settings behavior. Microsoft images/symbols remain private; no vendor
A2DP driver was inspected and no disassembly is distributed.

Power handling keeps the render PDO and the service session. Suspend blocks
new opens/transfers before closing channels, waits for admitted peer-response
submissions and serializes with racing CLOSE workers before marking the target
quiesced. Later control closes retain ownership without submitting into a
powered-down target. Synchronous BRB control requests use a five-second
timeout. Surprise removal gates I/O without issuing a peer response from the
incoming callback. Resume removes the gate and notifies the existing session.

Power events are hints to query GET_INFO, rather than queued copies of old
power state. PowerSerial detects even a full suspend/resume cycle that passed
before the service consumed either event. The service retires the old AVDTP
epoch, preserves explicit user disconnect policy, keeps its audio mapping and
endpoint identity, and retries incomplete closes after resume before opening
another channel. No player restart or per-application routing repair is added.

This is source/build coverage, not power-transition acceptance. In particular,
WDF callback/target cancellation timing, surprise-removal lifetime, failed-close
recovery and actual uninterrupted application recovery still need hardware
validation; the helper tests do not exercise the kernel power state machine.

## Offline checks

Profile availability and encoder activity are distinct: normal idle PCM leaves
an otherwise configured A2DP profile available, and an HFP resume can complete
while music remains paused. Suspend replies echo the request serial through the
audio child and service, plus the owning signalling epoch. An older reply may
report its actual state but cannot complete a newer request or publish a
replacement link as unavailable. A matching reply can report failed resume as
still suspended. Cache/request retirement occurs only after successful MPM
return; the audio IOCTL returns native failures to the service. Policy callback
re-acknowledgement uses the existing passive publication worker.

The encoder is joined/drained before completing an HFP suspend. Ordinary
encoder stop no longer publishes profile suspension. A new HFP request arriving
during configuration can defer availability without destroying that valid
configuration. These are source-level corrections; hardware SCO/A2DP switching
and the meaning of the observed Windows-specific MPM contract remain acceptance
items. The helper tests prove local request ownership, not external MPM timing.

- Shared driver lifecycle transitions: opening/disconnect/late success,
  close failure/retry, stale completion and event publication, replacement
  generations and wraparound, mandatory data binding, close-before-submission
  return, and publication invalidation at close/reopen boundaries. The
  production helper is used by these checks; WDF/MPM calls are not emulated.
- MPM request ownership: unsolicited/duplicate state, requested connect
  failure, superseded failure, reentrant request during acknowledgement,
  and serial wraparound use the production request-state helper.
- Existing PCM format/ring and concurrency checks; Windows service and CLI
  builds; full kernel build and linked-descriptor checks in Windows CI.
- Real installer state classifier exercised with fake device properties;
  existing recovery tests use fake mutation APIs and never change devices.

These checks cover code paths and selected completion orders, not Windows
Bluetooth scheduling, remote RF behavior, full WDF removal timing or perceived
audio/video timing. They cannot prove those properties without hardware use.

For 0.3.31, the complete native build also succeeded locally with MSVC 19.29,
the Windows 10.0.19041 user-mode SDK and matching 10.0.26100 NuGet driver/shared
headers and kernel libraries. All six CTest checks passed, including the
linked PE/PDB descriptor check; both fake-API recovery/installation-state
scripts passed. No built driver was staged or loaded. The
[CI run](https://github.com/TruthNian/xm6-audio/actions/runs/42673358274)
and a separate workflow dispatch failed with GitHub's internal server error
before creating any jobs; they are not successful builds or compiler failures.
The previous [0.3.30 CI build](https://github.com/TruthNian/xm6-audio/actions/runs/42671110002)
passed. Local compilation and opt-in instrumentation still do not establish
the Settings root cause or hardware acceptance.

## Outstanding acceptance

ACL arrival notifications use the public BRB_GET_DEVICE_INTERFACE_STRING and
IoRegisterPlugPlayNotification contracts, not another private MPM registration
or a new runtime process. The bounded radio path is queried in two stages,
validated, and terminated before IoGetDeviceObjectPointer. Notification parsing
accepts only a sufficiently sized HCI ACL-connect event for this peer. The
production byte parser is tested with truncated, unaligned, foreign-peer, SCO,
disconnect and trailing-name payloads. Kernel C assertions check the documented
field offsets against the WDK type.

An ACL hint only shortens a future retry deadline, allowing the existing 1.5s
peer-initiator window and never postponing an already-due explicit connect.
It cannot mark a channel connected, tear down a healthy replacement or override
user disconnect. Duplicate hints coalesce and cannot overflow the authoritative
channel event backlog. Actual L2CAP completions still own the connection state.

Registration is performed after initial D0 entry and retried once if missing
on resume. Its actual capability is reported by DEVICE_INFO flags. Failure
leaves the existing protocol retry available and is logged. Unregister uses
IoUnregisterPlugPlayNotificationEx outside the driver spin lock, before final
device teardown, so callbacks finish before context release. Permanent removal,
radio notification delivery and physical reconnect still need hardware use;
parser/build tests cannot establish those properties or the Settings labels.

The Bluetooth allocation-interface reference now belongs to the device object,
not a prepare/stop cycle. Rebalance does not overwrite the imported function
table while a delayed BRB completion may still free its BRB. The device destroy
callback dereferences the import once, at PASSIVE_LEVEL, without accessing WDF
child objects. It relies on the existing channel/server callback teardown and
WDF child-before-parent destruction; failed permanent-removal cleanup is still
an acceptance item. No separate per-packet reference layer was added.

L2CAP server registration now follows the documented server-then-PSM sequence
with the explicit AVDTP PSM (0x19) for registration and unregister. The pure
resource transaction is the one called by the kernel driver. Fake callbacks
exercise failure before acquiring a server, failed PSM acquisition, failed
server release, partial cleanup, retry of only the remaining resource and
duplicate no-op calls. These cover ownership/ordering, not Bluetooth callback
cancellation or physical radio removal. Prepare reports registration failures
instead of enabling a partial profile, and release returns cleanup errors.

MPM unregister was separately inspected: the observed local implementation
consumes its opaque handle and returns zero. No speculative retry of a consumed
MPM handle was introduced. Its callback/API rundown remains separate from the
documented L2CAP server/PSM transaction. Permanent removal while lower-stack
cleanup fails remains a hardware/lifetime acceptance item; retaining ownership
is not proof that Windows completed cleanup.

Windows Settings still needs a complete per-device Disconnect -> Connect cycle
with correct final labels. In 0.3.21's local metadata experiment it recognized
"voice, music" and closed the Classic link, but retained Disconnecting/Connected.
The audio-child Bluetooth bus metadata and this lifecycle batch are not yet
hardware-validated. Do not claim this GUI issue fixed based on CI or a backend
command. Retained endpoint/app audio, controls/volume, startup and sleep/resume
also require regression acceptance after the final batch is actually loaded.

Shared-mode PCM describes the Windows mix, not an individual file's original
sample rate or precision. Concurrent music/video cannot each impose a different
physical stream rate. No RF link can guarantee 990 kbps under arbitrary distance
or interference; actual negotiated rate, quality and failures remain observable.

## Connection diagnosis before another deployment

The read-only inbox comparison found the AVDTP-connected property on the
Microsoft AUDIO-category SRC interface, not on its Bluetooth-audio/render
category registrations or retained wave/topology registrations. The render
child's container identity also matched the inbox endpoint. Nonpresent custom
registrations were distinguished from present Microsoft interfaces. This does
not justify changing container identity, duplicating the property onto every
child interface, or clearing AlwaysShowDeviceAsConnected. The Settings root
cause remains unproven.

Version 0.3.31 adds the opt-in kernel TraceLogging provider
`XM6Audio.Transport` (`{bd2386c3-72bc-4f90-8292-247626362cb2}`), alongside the
existing `XM6Audio.WaveRT` provider
(`{ad2dba1f-78a1-4d25-a66e-d3195b9b1f72}`). It records connection-policy request
serials, manager request/status results, interface-property results, validated
channel states and one-shot property completion. `KsControlResult` also records
the control code, buffer lengths and final status when a control-interface
request is rejected before decoding its property. The WaveRT provider records
the child's one-shot result too, so routing through the child and parent can
be distinguished. Channel states use `OA2DP_CHANNEL_STATE` from
`driver/common/channel_lifecycle.h`; interface indices are 0=AUDIO and
1=Bluetooth-audio. These events describe separate calls, not one atomic
snapshot of the Windows broker's state.

No packet-rate event, device address, pointer, interface path, audio content
or pairing material is logged. The providers are inactive unless enabled by
an ETW session. Transport registration failure is nonfatal; successful
registration is undone on driver unload or failed DriverEntry. The provider
follows Microsoft's [kernel TraceLogging lifecycle](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/tracelogging-for-kernel-mode-drivers-and-components).
This instrumentation changes no connection policy and does not itself fix
Settings. Capture both providers with the Windows broker trace during the next
authorized hardware cycle, rather than interpreting a successful backend
command as GUI acceptance. No trace or new driver was activated on the host
as part of this source-only change.

The saved Settings failure trace was subsequently decoded with TDH rather
than the Event Log XML renderer, which had emitted only decoding errors.
All 15 trace records decoded; the trace header reported no lost events or
buffers. The UI changed from Disconnecting back to Connected in about 10 ms
and recorded `FailedToDisconnect` with generic `0x80004005`. An earlier
`0x8007023e` belongs to a separate temporary activity; the observed UI call
order disposes that activity before invoking the broker. Neither code
identifies the underlying Bluetooth failure.

Read-only inspection of the local Microsoft call sites also found that the
broker tries endpoint one-shot commands, then separately requests Bluetooth
device disconnection. The latter public export's null-radio path aggregates
results and returns 1168 when no per-radio call succeeds, losing the individual
error codes. A later authorized capture must therefore distinguish property,
profile-manager, broker and per-radio results. The saved trace contains only
the UI provider's events and cannot supply the missing lower-level result.

The inbox driver's disconnect request also performs a bounded wait before
returning. There is no evidence that deleting our completion wait fixes the
UI. Microsoft's [one-shot disconnect contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/ksproperty-oneshot-disconnect)
describes success of the attempt, not proof of final disconnection. Keep that
distinction when interpreting either a property result or the jack state.
No connection-policy or completion-wait change was made from this trace.

References: Microsoft [device status and problem codes](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/retrieving-the-status-and-problem-code-for-a-device-instance),
[framework work items](https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/using-framework-work-items),
and [WdfWorkItemFlush](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfworkitem/nf-wdfworkitem-wdfworkitemflush).
Submission/publication constraints: [WdfRequestSend](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestsend)
and [WdfWaitLockAcquire](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfsync/nf-wdfsync-wdfwaitlockacquire).
Power contracts: [Using power-managed I/O queues](https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/using-power-managed-i-o-queues),
[Using self-managed I/O](https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/using-self-managed-i-o),
[Self-managed suspend](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfdevice/nc-wdfdevice-evt_wdf_device_self_managed_io_suspend),
and [self-managed restart](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfdevice/nc-wdfdevice-evt_wdf_device_self_managed_io_restart).
Server ownership/order: [L2CAP server registration](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/bthddi/ns-bthddi-_brb_l2ca_register_server),
[server unregister](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/bthddi/ns-bthddi-_brb_l2ca_unregister_server),
and [PSM registration](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/bthddi/ns-bthddi-_brb_psm).
Interface lifetime: [BTH profile interface](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/bthddi/ns-bthddi-_bth_profile_driver_interface),
[INTERFACE reference contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-_interface),
and [WDF destroy callback](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfobject/nc-wdfobject-evt_wdf_object_context_destroy).
Radio events: [device interface string BRB](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/bthddi/ns-bthddi-_brb_get_device_interface_string),
[Bluetooth event payloads](https://learn.microsoft.com/en-us/windows/win32/bluetooth/bluetooth-and-wm-devicechange-messages),
[HCI event fields](https://learn.microsoft.com/en-us/windows/win32/api/bthdef/ns-bthdef-bth_hci_event_info),
and [synchronous notification removal](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-iounregisterplugplaynotificationex).
