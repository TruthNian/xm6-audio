# Native lifecycle review, 0.3.25

This source batch is not a declaration of hardware acceptance. Source review
and focused offline checks precede another hardware trial. Versions 0.3.23,
0.3.24 and 0.3.25 have not been installed on the host. The host was restored to Microsoft
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

## Offline checks

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

References: Microsoft [device status and problem codes](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/retrieving-the-status-and-problem-code-for-a-device-instance),
[framework work items](https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/using-framework-work-items),
and [WdfWorkItemFlush](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfworkitem/nf-wdfworkitem-wdfworkitemflush).
Submission/publication constraints: [WdfRequestSend](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestsend)
and [WdfWaitLockAcquire](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfsync/nf-wdfsync-wdfwaitlockacquire).
