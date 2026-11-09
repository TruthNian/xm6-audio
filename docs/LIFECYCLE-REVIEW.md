# Native lifecycle review, 0.3.31

This source batch is not a declaration of hardware acceptance. Source review
and focused offline checks precede another hardware trial. Versions 0.3.23
through 0.3.31 have not been installed on the host. The host was restored to Microsoft
BthA2dp; the custom codec service is stopped. Current host audio is not evidence
of native LDAC. No further installation or reboot is part of this source review.

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
