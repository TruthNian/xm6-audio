# Event, notification and transport review

Reviewed 2026-11-11 against the 0.3.50/0.3.51 production source. The headset
was unavailable. Runtime conclusions below concern the disconnected Windows
path; protocol fixtures do not substitute for connected hardware acceptance.

## Confirmed corrections

1. **Windows wireless state had a missing consumer contract.** The PortCls
   child supplied KS jack state but omitted PnP disconnected state. Version
   0.3.50 handles QUERY_PNP_DEVICE_STATE and invalidates the PDO after actual
   changes. A native49 -> inbox -> native49 display-model trace reproduced the
   difference; deployed50 finally remained Disconnected, with actual image
   hashes verified and zero trace loss. The endpoint is retained across links.
2. **Timeouts could act as peer commands.** A core timeout/reset event's zero
   message type was confused with COMMAND. A real START timeout caused local
   admission in the old production callback. Version51 classifies timeout
   completions first, consumes them in the request wait and independently
   requires STREAMING state at sender admission. Same-label retry is covered.
3. **A failed delay notification lost its latest value.** Version51 retains
   failed endpoint delay publication and retries current state on the existing
   main thread. No new worker, timer or stream renegotiation is involved.
4. **RTP errors could silently discard audio.** Unsupported LDAC fragmentation
   previously returned to the PCM loop without reporting failure. Version51
   stops the sender and uses the existing generation-bound recovery command.

## Producer-to-consumer contracts checked

| Path | Authority, delivery and completion | Evidence and limit |
|---|---|---|
| Radio/PnP arrival -> service session | Register interface notifications before enumerating; deduplicate arrivals; retire removed sessions outside the callback and join before SCM stop. | Source inspection of main.cpp; real device-interface-triggered startup observed on the host. Whole sleep/reconnect acceptance remains open. |
| HCI ACL hint -> reconnect scheduling | Hints are address-filtered and coalesced. They may accelerate retry but cannot declare audio connected or override explicit disconnect. Notification unregister waits for callbacks outside the spin lock. | Source inspection of link.c; channel/policy truth remains mandatory. Actual remote ACL arrival is untested in this session. |
| L2CAP callback -> service command | Channel and connection generations reject obsolete opens/closes. The bounded event backlog explicitly emits StateLost on overflow; removal replaces pending history. Wait insertion rechecks pending work. | New production event-queue fixture covers wrap, insertion race, canceled waiter, invalid buffer, stale generations, hint floods, overflow and removal. WDF APIs are controlled boundaries. |
| Windows profile request -> policy/acknowledgment | Request serials and connection generations distinguish policy intent from transport completion. External publication is serialized outside channel locks; reentry cannot retire a replacement request. | Existing production MPM/publication tests cover reentry and stale-state repair. BtaMpm is an observed Windows10 private ABI, not a published cross-version guarantee. |
| Policy/PCM change -> WAIT_FORMAT | Current snapshot, committed PCM epoch and one pending latest-state record survive coalescing. Cancellation-safe queue insertion has a post-insertion state recheck; stop closes admission before draining. | Existing production wait/policy fixtures cover cancellation, cleanup, producer/stop insertion races and registration. |
| Validated link -> Windows endpoint and device model | Parent validates both channels, then child commits connection state and notifies PnP plus KS jack consumers. Repeated state does not duplicate notifications; failed TRUE cannot connect the jack. | New production PnP/SetConnected tests and the actual disconnected display-model comparison. Connected UI/button completion still requires hardware. |
| AVDTP completion/command -> sender admission | A timeout is a completion, not a remote command. Accepted START and local media readiness may arrive in either order; setup failure retains work. Final admission requires STREAMING, matching PCM epoch/rate, live media and current policy. | Production request/callback/admission fixtures with real AVDTP cores; START/reset/label reuse, readiness ordering and failed setup recovery covered. |
| WaveRT clock -> ring -> encoder wake | Position and packet notifications share committed progress. Ring changes are sequenced; stale producer rate is rejected. Register data-ready event before checking the level; consumer rechecks before sleeping. | Existing production WaveRT/ring-notification tests and concurrent core ring tests. Actual DPC/MMCSS jitter and Chrome underruns are unaccepted. |
| Encoder -> RTP -> overlapped media -> BTHPORT | Each output advances RTP by represented samples. Send buffers retain ownership until terminal completion; timeout/cancel alone does not release them. Packetization failure now reaches stream recovery. | Actual LDAC/RTP checks at supported rates/MTUs, error-path regressions and production media-wait budget/ownership checks. No RF success or audible stability is implied. |
| Stop/power/remove -> closed transport -> final publication | Gate admission, retire sender and receives, close generation-owned handles, publish closure, cancel/join waits before handle/mapping destruction. Failed closes retain ownership. | Existing production channel-close/disconnect/handle fixtures plus new delayed-notification checks. Full service restart/sleep/resume needs later acceptance. |
| Headset/Windows volume and media controls | Windows retains AVRCP/HFP; the referenced hardware-volume cache notifies topology consumers. No simulated keys or per-player loopback. | Source inspection and earlier user acceptance only; no new headset-control test in this session. |

## Measurement and acceptance boundaries

The actual offline trace observes Microsoft's DevicesFlowUI model, including
its final settled state, rather than a rendered Settings screenshot. Both
native49 and inbox transiently reported Connected during enumeration; their
final states differed. A single immediate container snapshot missed the defect.
The fix was not a cosmetic property overwrite or pairing reset.

Version51 was subsequently activated without a computer restart. Both actual
kernel images and the running service matched the signed source package;
transport/render restart flags were clear. All 305 display-trace events were
decoded with zero loss, and the final state remained Disconnected(3). The
read-only AEP, container and service report also agreed on disconnection.
All 21 local checks and the exact
[source CI](https://github.com/TruthNian/xm6-audio/actions/runs/42979844718) passed.

The service bitrate is accepted media-submission bytes per time window,
including RTP overhead. Kernel completion counts/timing and PCM buffer/wait
events are separate measurements. None proves that the headphone rendered
every packet. Opt-in ETW can correlate these on a later connected run without
recording PCM content or pairing secrets.

No connected headset was available to establish per-device disconnect/button
completion, automatic reconnect, sound recovery in already-open clients,
wear detection, Chrome audio stability or RF performance. Those remain open.
The finite fixtures and source inspection do not establish absence of all bugs
or optimal performance. Private-ABI behavior, blocking kernel cancellation and
real scheduler/radio timing remain material external boundaries.
