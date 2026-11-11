# Event, notification and transport review

Reviewed 2026-11-11 against the 0.3.50–0.3.59 production source. The headset
was unavailable. Runtime conclusions below concern the disconnected Windows
path; protocol fixtures do not substitute for connected hardware acceptance.

## Confirmed corrections

The [0.3.59 buffering correction](PERFORMANCE-REVIEW.md) flushes complete
batched RTP frames before waiting for more PCM. The old service wait branch
withheld a short SBC tail; the expanded production-body fixture reproduces
that omission. Format uncertainty, failed sends and actual LDAC packet clocks
remain covered without claiming hardware timing or a Chrome-stutter fix.

The [0.3.56–0.3.58 lifecycle corrections](LIFECYCLE-REVIEW.md) carry session
stop through control-I/O submission, prevent stale startup enumeration from
recreating removed interfaces, and retain failed power-state queries until
the latest driver state is read successfully. Each has a production-body
regression check. They do not establish physical sleep/reconnect acceptance.

The [0.3.55 lifecycle correction](LIFECYCLE-REVIEW.md) adds the missing
incoming-OPEN failure chain: attempt binding, terminal closed event and
Windows-state publication. The service accepts a failed incoming-media
generation even when it never received a successful OPEN, then uses its
existing teardown/retry. Production-body fixtures fail against each old
contract. Connected Windows/radio acceptance remains pending.

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
5. **A rejected rate transition could leave permanent silence.** Version52
   retains current-format reconciliation after SUSPEND, RECONFIGURE or START
   failure. A single resume-intent bit identifies an interrupted local rate
   change; peer pause/configuration/close and teardown cancel obsolete intent.
   Each request boundary rechecks the connection/SEP before publication.
6. **Codec offers must be usable.** Version53 filters offers by fixed policy
   and actual AAC backend availability, and unifies AAC target selection with
   the real MFT media type. See the [codec review](CODEC-REVIEW.md), including
   the reproduced 64-to-96 kbit/s target overshoot and missing-backend cases.

## Producer-to-consumer contracts checked

| Path | Authority, delivery and completion | Evidence and limit |
|---|---|---|
| Radio/PnP arrival -> service session | Register before enumerating; reconcile startup removals with snapshot entries; deduplicate genuine arrivals; retire removed sessions outside the callback and join before SCM stop. | Production DeviceManager orderings cover stale snapshots, real replacement, unrelated devices and startup retry. Real device-interface-triggered startup observed on the host; whole sleep/reconnect acceptance remains open. |
| Power notification -> latest-state query -> reconnect gates | Query current power serial/flags; retain a failed query independently of endpoint readiness or the cached suspend state. Retry on the existing loop, clearing intent on success and honoring service stop. | Production refresh/retry bodies cover lost-query recovery, coalesced state, duplicate notification, tick wrap and stop with controlled driver/time. Actual sleep/resume and a host recovery deadline remain unaccepted. |
| HCI ACL hint -> reconnect scheduling | Hints are address-filtered and coalesced. They may accelerate retry but cannot declare audio connected or override explicit disconnect. Notification unregister waits for callbacks outside the spin lock. | Source inspection of link.c; channel/policy truth remains mandatory. Actual remote ACL arrival is untested in this session. |
| L2CAP callback -> service command | Channel and connection generations reject obsolete opens/closes. The bounded event backlog explicitly emits StateLost on overflow; removal replaces pending history. Wait insertion rechecks pending work. | New production event-queue fixture covers wrap, insertion race, canceled waiter, invalid buffer, stale generations, hint floods, overflow and removal. WDF APIs are controlled boundaries. |
| Windows profile request -> policy/acknowledgment | Request serials and connection generations distinguish policy intent from transport completion. External publication is serialized outside channel locks; reentry cannot retire a replacement request. | Existing production MPM/publication tests cover reentry and stale-state repair. BtaMpm is an observed Windows10 private ABI, not a published cross-version guarantee. |
| Policy/PCM change -> WAIT_FORMAT | Current snapshot, committed PCM epoch and one pending latest-state record survive coalescing. Cancellation-safe queue insertion has a post-insertion state recheck; stop closes admission before draining. | Existing production wait/policy fixtures cover cancellation, cleanup, producer/stop insertion races and registration. |
| Validated link -> Windows endpoint and device model | Parent validates both channels, then child commits connection state and notifies PnP plus KS jack consumers. Repeated state does not duplicate notifications; failed TRUE cannot connect the jack. | New production PnP/SetConnected tests and the actual disconnected display-model comparison. Connected UI/button completion still requires hardware. |
| AVDTP completion/command -> sender admission | A timeout is a completion, not a remote command. Accepted START and local media readiness may arrive in either order; setup failure retains work. Final admission requires STREAMING, matching PCM epoch/rate, live media and current policy. | Production request/callback/admission fixtures with real AVDTP cores; START/reset/label reuse, readiness ordering and failed setup recovery covered. |
| WaveRT clock -> ring -> encoder wake | Position and packet notifications share committed progress. Ring changes are sequenced; stale producer rate is rejected. Register data-ready event before checking the level; consumer rechecks before sleeping. | Existing production WaveRT/ring-notification tests and concurrent core ring tests. Actual DPC/MMCSS jitter and Chrome underruns are unaccepted. |
| Encoder -> RTP -> overlapped media -> BTHPORT | Each output advances RTP by represented samples. Complete batched frames flush before a valid PCM-underflow wait. Send buffers retain ownership until terminal completion; timeout/cancel alone does not release them. Packetization/send failures reach stream recovery. | Actual LDAC/RTP checks at supported rates/MTUs, short-SBC-tail/underflow regression, error-path regressions and production media-wait budget/ownership checks. No RF success or audible stability is implied. |
| Stop/power/remove -> closed transport -> final publication | Gate admission, retire sender and receives, close generation-owned handles, publish closure, cancel/join waits before handle/mapping destruction. Failed closes retain ownership. | Existing production channel-close/disconnect/handle fixtures plus new delayed-notification checks. Full service restart/sleep/resume needs later acceptance. |
| Headset/Windows volume and media controls | Windows retains AVRCP/HFP; the referenced hardware-volume cache notifies topology consumers. No simulated keys or per-player loopback. | Source inspection and earlier user acceptance only; no new headset-control test in this session. |

## Measurement and acceptance boundaries

The actual offline trace observes Microsoft's DevicesFlowUI model, including
its final settled state, rather than a rendered Settings screenshot. Both
native49 and inbox transiently reported Connected during enumeration; their
final states differed. A single immediate container snapshot missed the defect.
The fix was not a cosmetic property overwrite or pairing reset.

Re-reading the retained controlled traces confirms that the inbox driver also
reported transient Connected at about 3783 ms before settling to Disconnected
at about 4930 ms. Version54 transiently reported Connected at about 4383 ms,
then settled to Disconnected at about 5392 ms. The similar startup interval
alone does not identify a custom-driver defect or establish visible UI flicker.
It must be distinguished from the reproduced version49 failure that finally
returned to Connected. Literal rendered text/button acceptance remains open.

`Disconnected(3)` is the internal model enumeration, **not the expected screen
text**. For a paired, disconnected headset, the expected English UI label is
**Paired**. The installed Windows10 EN-US resources were read offline using
the SDK MakePRI tool: both `SystemSettings_Device_Bluetooth_Disconnected` and
`SystemSettings_Device_Bluetooth_DisconnectedAudio` have the string value
`Paired`. AEP Paired=True/Connected=False and the settled model support that
state; they are not a direct observation of rendered screen text. Literal
Settings-label/button acceptance must retain that distinction.

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
