# Native lifecycle review, 0.3.22

This source batch is not a declaration of hardware acceptance. Source review
and focused offline checks precede another hardware trial. No 0.3.22 driver has
been installed on the host.

## Confirmed source defects and corrections

| Trigger | Defect | Correction |
| --- | --- | --- |
| Disconnect while OPEN BRB is pending | CLOSE returned success for Opening; late OPEN could resurrect a channel | Latch close intent, wait for bounded OPEN completion, close a late successful handle |
| Headset initiates after explicit disconnect | Incoming acceptance ignored local policy | Apply policy before selecting a channel and before exposing a completed OPEN |
| Remote and local CLOSE overlap | Both could submit CLOSE for the same handle; failure erased ownership | Serialize CLOSE per channel, retain the handle after failed CLOSE |
| Delayed notification/work after reconnect | No channel incarnation identity; an old close could tear down its replacement | Channel generations in driver/service ABI 6; validate work, publication and service consumption |
| Delayed encoder failure after reconfiguration | A retired encoder's queued error could terminate its replacement on the same media channel | Independent encoder-generation validation |
| Disconnect completion while transport is unfinished | Jack and parent carried independent completion events | One referenced transport-owned event, validated against both actual channel states; kernel bus ABI 3 |
| PnP stop/resume | A queued DeviceGone from the previous lifetime could terminate the replacement session | Clear the old event backlog before the new PnP lifetime starts |
| Rapid interface removal/arrival | New session could lose the exclusive control-open race and exit permanently | Retry transient open errors on the existing session thread until stop/removal |
| Earlier pending install, later installer returns success | Service/INF metadata was mistaken for active stack state; problem14 triggered rollback | Inspect devnode status/install flags, preserve 3010, defer service activation when reboot is pending |
| Explicit 24-bit format selection | Container width excluded valid 24-in-32 PCM | Filter useful precision separately from container width |

Channel close work is preallocated rather than allocated in a disconnect
indication. Allocation failure can no longer fabricate a completed remote
close. Failed or in-progress transport closes cannot acknowledge the native
disconnect property. An unavailable codec service may unplug its audio jack
before the physical transport is closed; these are distinct states.

## Offline checks

- Shared driver lifecycle transitions: opening/disconnect/late success,
  close failure/retry, stale completion and event publication, replacement
  generations and wraparound. The production helper is used by these checks.
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
