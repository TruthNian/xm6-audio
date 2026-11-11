# Native lifecycle review

## 0.3.57: reconcile startup discovery with removal notifications

The service registers interface notifications before enumerating existing
interfaces, as required by the
[CM registration contract](https://learn.microsoft.com/en-us/windows/win32/api/cfgmgr32/nf-cfgmgr32-cm_register_notification).
That order allows notifications to interleave with consumption of the initial
snapshot. Previously, removal of a path without a Session was ignored; the
stale snapshot could then create a Session for that removed interface and
leave its transient-open retry running until another removal or service stop.
This ordering defect was reproduced in our production-body fixture; it is
not a claim that Microsoft documents this specific bug.

During startup reconciliation only, remember removed paths under the existing
manager mutex. Skip their snapshot additions even when no Session existed at
removal time. Real arrival notifications remain eligible to create a fresh
Session, and unrelated snapshot entries remain eligible. Release the removal
history when enumeration ends or registration fails. There is no new worker,
poll, timer, kernel ABI or connected-state publication path.

The fixture runs the actual DeviceManager with controlled CM callbacks and
Sessions and real asynchronous retirement. The previous code fails the first
check by starting a removed interface. Coverage includes removal during
registration or after snapshot capture, duplicate/case-insensitive arrivals,
arrival then removal, removal then genuine arrival, unrelated devices, runtime
replacement, stop/restart and registration failure followed by retry.
This proves these discovery orderings, not Windows callback timing, rendered
Settings labels, headset reconnection or audible playback. In particular,
this fix has not been established as the cause of the reported UI symptom.

The local native x64 Release build and all 27 checks pass. Deployment and
exact-source CI are recorded separately after verification.

## 0.3.56: carry stop through control-I/O submission

Session stop signals a persistent event and cancels currently submitted I/O.
Its signalling/media OPEN and AVDTP-send wrappers previously omitted that
event. A request submitted after the one-shot CancelIoEx could therefore wait
for its ordinary timeout. The production Session -> Transport -> IOCTL chain
reproduces a full 15,000 simulated milliseconds for signalling OPEN in the
controlled stop-before-submit ordering; this is not a measured host delay.

Pass the existing stop event through those operations. The common IOCTL helper
rejects a request when cancellation is already signalled; later cancellation
is included in its overlapped wait. Pending requests still undergo cancellation
and terminal completion before releasing OVERLAPPED storage. Cleanup CLOSE
and final drain remain available after stop rather than abandoning ownership.
No new runtime thread, poll or timer is introduced.

The fixture executes all three production call chains with stop before/during
submission, ordinary completion, simultaneous completion/stop and a normal
timeout. It checks that the pre-stopped path submits nothing, the in-flight
path cancels/drains before cleanup, and successful binding/output survives.
The previous body fails its elapsed-budget assertion. This establishes the
submission/cancellation contract, not a universal shutdown deadline: lower
Windows/Bluetooth I/O must still reach terminal completion. Actual connected
recovery and audio stability remain separate acceptance requirements.

Version56 was activated through a radio cycle without rebooting the computer.
Both actual kernel images and the running service match the signed payload.
All 26 local checks and [source CI](https://github.com/TruthNian/xm6-audio/actions/runs/43003121997)
passed. The offline state remains paired/disconnected; the
[runtime record](HARDWARE-BRINGUP.md) preserves the limits of this observation.

## 0.3.55: complete failed incoming OPEN notifications

Outgoing channel failures already bind and finish the pending Windows connect
attempt. Incoming signalling OPEN omitted that binding; its allocation,
request-creation, submission and asynchronous failure paths also omitted
Windows-state publication and the service's channel-closed notification.
For incoming media, the service could retain a configured signalling link
without a media channel or an event that would trigger teardown.

Bind the pending connect request when admitting incoming signalling. Finish
each failed incoming attempt through one generation-checked helper: commit
the closed state, release the OPEN waiter, queue the existing CLOSED event,
mark the matching signalling connect attempt failed, and schedule existing
state publication. No successful OPEN is fabricated. The service now accepts
a same-connection media CLOSED at its current or a later generation, allowing
a failed OPEN with no prior OPEN event to trigger its existing teardown and
close-retry path. Older media generations and other signalling connections
remain ignored. No new ABI, thread, timer or event type is required.

Production-body checks reproduce all three old omissions against source
`1508791bcebb19c24c8df3cfb60727d6369def32`: missing attempt binding, missing
failure publication/event, and ignoring a failed incoming-media CLOSED.
The expanded fixtures cover successful synchronous/asynchronous responses,
allocation/request/submission failures, delayed failure, policy rejection,
stop/removal gates, close-during-open, obsolete completions, replacement
connections and generation wrap. The actual service event handler and
teardown execute with a failed CLOSE followed by a successful retry.
OS/Bluetooth calls remain controlled; this does not establish that these
paths caused a particular observed UI error or prove headset acceptance.

Version55 was then activated without rebooting the computer; both actual
kernel images and the running service match the signed payload. All 25 local
checks and [source CI](https://github.com/TruthNian/xm6-audio/actions/runs/43000936449)
passed. The final offline display model and AEP/container state remain paired
and disconnected; see the [runtime record](HARDWARE-BRINGUP.md) for trace scope
and the separate, still-incomplete connected/rendered-UI acceptance.

## 0.3.54: PCM cost and AAC output ownership

The [performance/output review](PERFORMANCE-REVIEW.md) removes measured
per-sample integer division while preserving the numeric PCM contract, and
corrects AAC events-only/NO_SAMPLE handling and returned-object cleanup.
The independent division oracle matches 2,424,844 samples. Paired computation
timing shows about 5.2x improvement for full-int32 stereo preparation, without
an attributed float-path or whole-stream gain. Controlled old drain bodies
fail the new ownership/events-only checks. Actual MFT encoding still passes
all four mono/stereo and 44.1/48 kHz combinations. Hardware sound, reconnect
and Chrome stability remain separate, incomplete acceptance items.

Version54 was activated through a radio cycle, without rebooting the computer.
Both actual kernel images and the running service match the signed payload.
All 25 local checks and source CI pass. The offline display model settles to
the state whose installed English resource is Paired, but still records an
approximately one-second transient Connected beforehand. This is not complete
display lifecycle or rendered-UI acceptance; see the [runtime record](HARDWARE-BRINGUP.md).

## 0.3.53: constrain codec offers to actual backend and policy

The [codec review](CODEC-REVIEW.md) reproduces AAC target overshoot with the
actual Windows MFT, unavailable AAC being treated as usable, and candidate
selection/advertisement that ignores fixed user constraints. Shared AAC target
selection, balanced runtime backend probing and policy-filtered offers correct
those contracts. New production-body and real-MFT checks cover the failure
paths without opening a headset or audio endpoint. Hardware fallback and
connected stability acceptance remain open.

Version53 was then activated without rebooting the computer, with both actual
kernel images and the running service verified against the signed package.
All 23 local checks and source CI passed; see the codec review for test scope
and [the runtime record](HARDWARE-BRINGUP.md) for activation evidence.

## 0.3.52: recover interrupted sample-rate changes

Two real AVDTP cores reproduce another lost intent: after a transient SUSPEND
or RECONFIGURE rejection, the production method forgets the requested rate.
The unchanged PCM stream cannot restart at the old codec rate and no new
format/player event is guaranteed. Rejecting START after an accepted rate
change has the same problem. The old production body fails the new pending
reconciliation assertion.

Retain failed work on the existing one-second configuration reconciliation
loop. One resume-intent bit distinguishes an interrupted local rate change
from an intentional peer pause; it does not introduce another loop, timer or
thread. Rebuild the rate from current PCM at retry time. An accepted rate is
not renegotiated merely because its subsequent START failed. Peer configuration,
START, SUSPEND, CLOSE, ABORT and teardown retire obsolete local resume intent.
After every synchronous request, revalidate the connection/SEP identity and
current policy before publishing or resuming. Reject malformed/wrong-codec
capabilities rather than installing them as the current peer capabilities.

The production rate-change/callback/reconciliation methods execute with actual
AVDTP packets. Checks cover SUSPEND/RECONFIGURE/START rejection and recovery
without another PCM event, deadline wrap, replacing the pending PCM target,
returning to the original rate without another RECONFIGURE, pause during
backoff, and stop/replacement-connection/peer ABORT during either protocol
boundary. The peer rejection and OS/encoder boundaries remain controlled;
none establishes radio or audible acceptance.

Version52 was then activated and its actual kernel/service payload hashes
verified without rebooting the computer. All 21 local checks and source CI
passed. The offline state and installed Paired-label resource were also
verified separately; see [the runtime record](HARDWARE-BRINGUP.md).

## 0.3.51: timeout classification, delay delivery and RTP failure propagation

The expanded [event-chain review](EVENT-CHAIN-REVIEW.md) found three additional
failure paths. Core timeout/reset completions have `timed_out=true` and a
zero-initialized message type, the same numeric value as COMMAND. The service
previously interpreted a timed-out START as an accepted remote START. Classify
timeouts as responses first. The production request wait now expires core
transactions during its loop and consumes their result before its outer
deadline, rather than leaving a completion for label reuse. Admission itself
also requires an actual STREAMING SEP.

An unsuccessful DELAYREPORT-to-endpoint update previously discarded the new
delay. Retain the latest value for the existing one-second profile retry,
without publishing formats, renegotiating or restarting an encoder. Endpoint
setup uses that same publication method. Teardown clears the pending delay
request so a retired link cannot replay it.

The RTP service block previously broke out of a failed fragmentation operation
and kept consuming PCM. LDAC intentionally rejects fragmentation. Unexpected
packetization/fragmentation failures now post the existing generation-bound
stream failure and stop sending; they cannot masquerade as a healthy stream.

Production-body fixtures reproduce the old timeout-triggered admission, lost
delay report/retry and silent LDAC fragmentation failure. Restoring each old
behavior fails its new assertion. Two real AVDTP cores cover START timeout,
reset and label reuse. Admission tests cover non-STREAMING/missing SEPs.
Delay checks include failure recovery without another report, latest-value
replacement, current-policy/transport gates and tick wrap. The actual kernel
event queue now has coverage for insertion races, cancellation, generation
rejection, wraparound, hint coalescing, overflow recovery and device removal.
OS notification APIs and radio behavior remain external boundaries.

## 0.3.50: wireless PnP state reporting

A controlled native49 -> inbox -> native49 radio-cycle comparison with the
headset unavailable captured the actual DevicesFlowUI model, not just an
immediate container snapshot. Native49 finally returned to Connected(7),
although the Bluetooth API reported disconnected. Inbox finally remained
Disconnected(3). Restoring native49 reproduced the erroneous final state.
Earlier instantaneous container comparisons missed this event-order difference.

The persistent PortCls child reported KS jack changes but never handled
IRP_MN_QUERY_PNP_DEVICE_STATE or invalidated its PnP state. Implement the
[documented wireless disconnected flag](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/handling-an-irp-mn-surprise-removal-request#about-pnp_device_state)
without replacing another driver's bits. Invalidate the PDO state only after
committing an actual connection change. Keep PortCls dispatch, the same PDO,
KS jack events and the parent's validated transport publication. The initial
post-START query reports disconnection without needing a transition first.

The production dispatch and SetConnected bodies run against controlled OS
boundaries. Coverage includes initial state, unrelated state bits, connected
and disconnected transitions, a query during invalidation, duplicate reports,
parent publication failures, policy/removal rejection, absent topology,
uninitialized devices and stop/remove dispatch. This fixture cannot establish
Windows container aggregation or real connected playback; repeat the actual
offline display-model trace after deployment before calling the UI defect fixed.
That deployment check subsequently passed: both actual 0.3.50 kernel images
matched the signed package, the service was running, and DevicesFlowUI finally
remained Disconnected(3). No trace events/buffers were lost. The radio cycle
activated the candidate without rebooting the computer. This establishes the
previously reproduced offline path, not real connected per-device operations.

## 0.3.49: retain failed endpoint setup for retry, source only

The production setup/reconciliation fixture demonstrates another lost request:
after format, latency or connected-state publication fails, the old code has
already cleared pending configuration. A temporary failure leaves an accepted
peer START without a local sender, even after the external call would succeed.
The old production body fails the new pending-request assertion.

Keep configuration pending until all three publication steps succeed. A failed
publication schedules a one-second backoff, consumed by the existing main
thread reconciliation method using current PCM, codec, connection and policy.
Successful setup clears both the request and backoff before local admission.
Fresh peer configuration/media readiness can still request immediate work.
There is one new deadline field, with no worker, timer object or process.

The two-AVDTP-core fixture checks each failed publication at initial setup and
after an accepted rate change, recovery without another PCM/START, no retry
before its deadline, repeated failures, tick-counter wrap, current PCM changed
during backoff, and disconnect/stop/peer SUSPEND/CLOSE before recovery. An
accepted rate change is retained and recovery does not originate another
START. Production setup/reconciliation/rate/start/suspend procedures execute;
audio calls and worker admission remain controlled fakes. The actual admission
fixture separately checks the pending gate and its release. These checks do
not establish a Windows Settings fix, actual radio/headset recovery, Chrome
stutter resolution or measured performance gains. Version 0.3.49 was later
activated; see [the runtime record](HARDWARE-BRINGUP.md).

## 0.3.48: reconcile peer configuration with running PCM, source only

Two real AVDTP cores and the production session event/configuration/setup
procedures reproduce a missed rate transition: the peer configures and starts
48 kHz while the unchanged Windows PCM is 96 kHz. The old capability-discovery
block preserves 96 kHz as the preferred endpoint format, but never reconfigures
the codec. Local admission correctly refuses the mismatched rate, so no new
PCM notification means persistent silence. Peer RECONFIGURE also previously
updated the codec without scheduling endpoint/rate reconciliation.

Replace the old capability-discovery flag with pending peer-configuration
work. Consume it on the existing main thread after queued policy/format
commands and after media readiness. Discover capabilities before publishing
the format list, then use the existing rate-reconfiguration procedure for
running PCM in automatic mode. Defer encoder admission while this work is
pending. A synchronous capability request may pump peer commands; recheck
stop, channel identity and the current SEP before publishing its result.
There is no new thread, timer, polling process or per-file mix selection.

The old production block fails the new test at the required 96 kHz codec
result. The replacement covers 96/48, 48/96, 44.1/48 and 88.2/96 kHz pairs;
same-rate startup without renegotiation; reversed media readiness; an accepted
peer RECONFIGURE followed by START; duplicate reconciliation; unsupported
rates, explicit fixed/follow-off settings and HFP suspension; policy/state
gates and stop/disconnect/channel replacement/peer CLOSE during capability
discovery. Event handling, setup, rate/start/suspend procedures and AVDTP
packets are production code. Audio IOCTLs and encoder admission are fake;
the actual admission fixture separately checks the pending-configuration
gate. This does not establish Windows GUI correctness, actual headset
format-transition acceptance, the Chrome stutter cause or optimal performance.
Version 0.3.48 is not installed on the host; the observed runtime remains 0.3.44.

## 0.3.47: reconcile accepted START after endpoint readiness, source only

The service drains signalling before consuming queued transport commands.
The peer can have completed its media channel and sent START while the
service's event thread has not yet delivered MediaOpenedByPeer to main.
The AVDTP core accepts START from its OPEN state independently of that local
notification. begin_streaming then returns because media_open is still false.
The later media handler sets readiness and configures the endpoint, but did
not revisit the accepted START. With an already-running, unchanged PCM stream,
neither a new format event nor another START is guaranteed. This leaves a
valid streaming SEP without a local encoder.

Before changing the source, a fixture with the actual admission methods and
actual successful-setup tail reproduced that silent path: START admission
before local media readiness was rejected, and later successful setup left
streaming false. Successful endpoint setup now checks the current SEP and
calls normal local admission when it is already STREAMING. No remote START,
new state machine, polling, worker or player operation is introduced. Failed
setup returns before this check. Normal rate/epoch/drain/policy checks still
apply. begin_streaming also checks stopping, preventing an asynchronous stop
during setup/protocol work from admitting a new encoder thread afterward.

The extended profile fixture compiles the production setup tail and actual
begin/end/start/retry functions. It covers START-before-readiness and the
opposite order, duplicate setup without a second worker, pending publication
failure/recovery, disconnect/HFP/power/teardown/media/child/stop/missing-SEP
gates and no additional outgoing START. The full production setup method is
also compiled with the real codec/PCM core: format, latency or connected-state
publication failure cannot call its fake admission hook, and success calls
it only after those steps. External OS/profile APIs and the notification order
are controlled fixtures; a real thread runs no encoding or audio I/O.

The full local kernel/service build and all eighteen CTest checks pass.
Driver/service11, bus6 and ring2 remain unchanged. This is source-only work;
the installed host remains44. This audit does not establish a new root cause
for the Windows Settings state/disconnect error or the Chrome audio-stutter
report. Those acceptance failures and measured performance remain unresolved.

## 0.3.46: multi-frame RTP duration, source only

The service passed samples_per_frame as the duration of an entire encoder
output to rtp_add_frames. The RTP core accumulates the supplied total duration;
it does not multiply it by the separate frame count. With multi-frame LDAC
output, the following packet therefore received a timestamp that advanced by
only one frame. The existing encoder tests checked bitrate/frame counts, and
the core RTP tests appended one frame at a time, so neither covered this
service integration.

Before changing the production caller, the new fixture compiled its actual
packetization block and fed the actual LDAC adapter. At 96 kHz, stereo HQ and
MTU679, the adapter returned two 256-sample frames per packet. The second RTP
timestamp was 256 rather than 512. A separate synthetic stream reproduced the
same accounting error; the wire frame counts were correct while its clock was
wrong. The actual adapter also reproduced the corresponding 128-versus-256
step at 44.1 kHz. This is an established packet timing defect, not proof that
this specific headset uses those timestamps to buffer/play out audio or that
it caused the reported Chrome stutter.

Both normal append and flush/retry now pass samples_per_frame * frame_count.
The fragmentation path already used that total and is unchanged. The API
comment explicitly defines samples as the whole chunk's per-channel PCM
duration. No RTP-core API behavior, codec payload, negotiation, PCM precision,
sample rate, HQ bitrate or ABR policy changes. Single-frame paths retain the
same duration. The rule follows the audio sampling-clock basis in
[RFC3550 section5.1](https://www.rfc-editor.org/rfc/rfc3550#section-5.1).

The portable fixture covers mixed one/two/three-frame LDAC chunks, actual
on-wire timestamps and sequence numbers, 32-bit clock wrap, buffered
flush/retry, SBC batching and AAC fragmentation. Windows additionally uses
the real LDAC adapter at 44.1/48/88.2/96 kHz, 16/24-bit input and MTUs
679/895/1024, checking frame-count-derived duration rather than input-call
count (encoder priming is retained). No Bluetooth or Windows audio I/O occurs.

The full local kernel/service build and all eighteen CTest checks pass.
ABI remains driver/service11, bus6, ring2. This is an uninstalled source
candidate; the host remains44. The current audit found no new evidenced
root cause for the Settings state/disconnect fault. WaveRT already uses a
high-resolution timer, so no speculative timer replacement was made.
Source-only work continues; actual sound/jitter/connection acceptance and
measured performance remain incomplete.

## 0.3.45: follow current endpoint PCM in initial negotiation/proposals, source only

The source codec selector defaults to 48 kHz when no rate is specified. The
service previously passed that default directly during initial selection,
even if it already knew that the endpoint PCM was 96 kHz. Later format events
can reconfigure the codec, but another event is not guaranteed for an already
running, unchanged format. Separately, setup_audio_endpoint preferred the
current codec rate's representation, even when another supported endpoint
format was current. A peer-selected 96 kHz configuration could therefore
produce a 96 kHz proposal while endpoint PCM was 48 kHz. This establishes a
proposal/selection mismatch, not proof that Windows changes every existing
shared mix or that either path caused the user's Chrome stutter.

An actual-production-block/method fixture reproduced both mismatches before
the fix. Automatic initial selection now hints the valid current PCM rate;
acceptance still uses the original user constraints, so an unsupported hint
does not become a hard requirement. Proposals prefer the exact current PCM
format when listed, including representation, container and valid bits. The
remaining supported formats stay available. Fixed-rate settings and disabled
rate following retain their behavior. No extra worker, polling, resampler or
per-packet work is added. This does not force 48 kHz or 96 kHz globally.

The production tests cover 44.1/48/88.2/96 kHz across float32, integer32,
24-in-32, packed24 and integer16, preserving the preferred PCM against both
48 and 96 kHz codec configurations. They also cover explicit rate/depth,
disabled following, unsupported auto hints, accurate fixed-rate rejection
and missing PCM state. Audio IOCTLs are fake; real Windows format negotiation,
reconfigure rejection and audible switching remain unaccepted.

The configuration name follow_source_rate is retained for compatibility, but
its source means the Windows endpoint PCM. Original per-player file/quality
metadata is not available at this boundary. Shared clients have already been
mixed by Windows. No per-file auto-mix policy is implemented or claimed.
Current hardware remains 0.3.44 with outstanding Settings/disconnect and
Chrome audio-stutter reports. No candidate45 deployment or new headset test
was performed. Driver/service ABI11, bus6 and ring2 remain unchanged.

## 0.3.44: driver-handle lifetime versus stop cancellation, source only

Session::stop calls transport/audio cancel_all from the manager's thread,
while the session's main thread can be closing/reopening the audio child or
opening/closing the transport. DriverHandle formerly read/wrote its plain
HANDLE without lifetime serialization. A cancel could read the old value
while close was retiring it, then pass that value to CancelIoEx after another
object had reused it. The unsynchronized read/write is also a C++ data race.

A production-body regression uses real threads and a fake handle table to
pause closure after the numeric value becomes reusable, before the owner
clears h_. The old cancel_all deterministically targets the replacement object.
The fixture orders h_ reads before its final write, so that particular failure
does not depend on triggering C++ undefined behavior. This demonstrates the
source lifetime gap; it is not proof of the historical Settings/silence cause.

Open, close and cancel_all now share one mutex, held through CreateFile,
CancelIoEx and CloseHandle. Open calls a private locked cleanup helper to
avoid recursive locking; failed-open errors are saved/restored across unlock
because recovery classifies GetLastError. Other I/O/get users retain their
session lifetime and completion obligations. No mutex is added to per-packet
send, ring, encoding or ordinary IOCTL completion; this is not a general lease
for arbitrary concurrent close/I/O. Cancel during CreateFile waits for handle
publication. A blocked synchronous create/close can therefore delay it, and
no hard shutdown bound is claimed.

The new fixture covers close/reuse/cancel ordering, cancellation concurrent
with open publication, replacement open, idempotent close, invalid-handle
no-op cancellation and exact failed-open error preservation even when the
observable test mutex changes last-error. It records lock contention while
using real mutex/thread synchronization; OS APIs and the handle table are fake.
The full local kernel/service build and all sixteen CTest checks pass.

Cancellation remains a request, with pending buffers retained until I/O
completion; see Microsoft's [CancelIoEx contract](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex)
and [handle closure contract](https://learn.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-closehandle).
ABI remains driver/service 11, bus 6, mapped ring 2. No device operation,
deployment, signature or reboot occurred. Real connection UI, reconnect,
sound/controls/formats and measured performance remain unaccepted.

## 0.3.43: retry profile publication and blocked local start, source only

The production START procedure previously returned success after the peer
accepted START even when local encoder admission failed. In particular,
begin_streaming returned without creating a worker if the suspend/resume
publication failed. That failure was not retained for retry; the remote SEP
could remain STREAMING while no local encoder existed, with no further PCM
format/player event guaranteed to try again. A source fixture compiled the
old production bodies and reproduced the false-success return using one
injected publication failure. This demonstrates the source path, not that it
caused the previously observed hardware silence or Settings failure.

START now returns the actual local admission flag. Failed availability
publications retain retry intent, including idle suspend/resume replies.
The existing main loop retries after processing queued policy/format commands,
at one-second intervals while work remains; it does no publication polling
after success. Each attempt recomputes availability and uses the latest
request serial and connection generation rather than replaying an old reply.
An encoder start blocked on publication retains a separate intent so that
another successful publication cannot lose the start. A retry starts the local
worker only if its peer SEP is still STREAMING and the existing admission
gates permit it; it never sends a replacement remote START. End-streaming
clears that intent, covering suspend, close and teardown. Stop and an absent
audio child suppress retries. No worker, PCM polling, format, codec, gain or
driver/service contract is added or changed.

Production-body checks cover repeated failure followed by recovery without
another player event, no redundant remote START, publication success from a
separate caller, idle availability, current request/link binding, stop/removal,
disconnect, HFP/transport suspension, teardown, peer OPEN/missing SEP and
32-bit deadline wrap. External profile/OS APIs are fake; a real test worker
performs no audio or I/O. These checks do not emulate MPM or Windows scheduling.
The full kernel/service build and all fifteen CTest checks pass locally.

ABI remains driver/service 11, bus 6, mapped ring 2. This candidate is source
only: no device operation, deployment or reboot occurred. Actual Settings
Paired/Connected, per-device disconnect/reconnect, sound/controls/formats and
CPU/jitter/latency still require acceptance. No optimality or measured
performance claim follows from these tests.

## 0.3.42: channel mapping before final quantization, source only

The service formerly quantized float/full-S32 channels independently, then
averaged their already-quantized results for mono. With valid normal-range
float inputs at 0.1 and 0.8 of one S16 LSB, this produced a final S16 value of
1; averaging first yields 0.45 LSB and rounds to 0. Full-S32 values 127/383
also exposed lost averaging precision at the S24 boundary. For finite float
headroom 1.5/-0.5, channel-wise clipping reduced the intended mono 0.5 to
approximately 0.25. This last example tests over-nominal headroom handling;
it is not evidence that this host normally emits such levels.

A consumer-only preparation helper now maps channels first, then clips and
rounds to the codec's actual 16/24 useful bits in the existing signed-S24
adapter representation. Float averaging uses double intermediates; integer
averaging uses a wide sum and one combined quantization divisor. The mean's
existing 1/2 weights are retained. Mono expansion is in reverse order when
needed, so the same buffer can hold ring input and codec output. Float words
are loaded with memcpy, preserving aliasing/alignment correctness when raw
float bits reside in int32 storage. No floating-point work enters the kernel.

The service's separate float vector and independent layout loop are removed.
Equal-channel float/full-S32 processing has a flat path, and already-S24 data
at 24-bit precision passes through unchanged. Channel, representation,
precision and input/output bounds are checked before any write. Core scalar
quantizers are shared with the existing conversion helpers. Rounding uses
nearest with half ties away from zero, including S24-to-S16 preparation; this
aligns that path with the existing float/full-S32 policy. The adapter's S16
packing then sees values already aligned to its precision and is idempotent.
Source container/valid-bit metadata, sample rates, codec choice and gain
settings are unchanged. Full32 input remains supported; the codec's supported
precision remains a separate boundary, not a forced source format.

The production service block plus actual S16 adapter converter reproduced
both the normal-range final-code error and the headroom example before the
fix. Corrected bodies pass both. Core and integration checks additionally
cover float/S24/full-S32, mono/stereo layouts, 16/24 target bits, same-address
and disjoint storage, unaligned input, wide extrema/cancellation, NaN/Inf,
signed rounding ties, input/output guards and empty/error cases. The full
local kernel/service build and all fourteen CTest checks pass. These are
numeric and source integration checks, not actual codec acoustics or Windows
format-negotiation acceptance. The normally offered endpoint channel count
matches the codec; this fix covers retained/mismatched channel layouts that
the existing service already handles, without claiming a failure in all
ordinary stereo playback.

ABI remains driver/service 11, bus 6, mapped ring 2. No candidate was installed
and no headset/device/player/volume/boot operation occurred. Real Settings
reconnect, sound/controls/formats and CPU/latency acceptance remain pending.

## 0.3.41: admitted PCM conversion and atomic diagnostics, source only

The 16/24-bit producer previously converted each chunk into a scratch array,
then called ring_write to discover how many frames fit. Rejected frames had
already been read/converted while the ring and position locks were held.
The new producer core entry point checks capacity first and converts only the
admitted prefix directly into ring slots. The driver scratch array and its
intermediate copy are removed (2 KiB per ring object). The conversion handles
little-endian signed values explicitly, including unaligned packed input,
wraparound and mono in fixed stereo storage. Raw 32-bit containers retain
their bit pattern; codec precision policy is unchanged.

Admission uses the same snapshot as the raw write path. Capacity released by
the consumer after that snapshot is available to a later write attempt; the
producer remains nonblocking. The existing 256-frame integer attempt boundary,
unread-audio protection, rejected-frame counting, format validation and
commit-before-data-ready notification are retained. The point where capacity
is sampled moves before conversion; this does not promise the identical drop
pattern under concurrent scheduling. No bulk producer-side read-position
advance, ring overwrite, format-layout change or relaxed barrier was added.

Overruns also used to mix a plain producer increment with an atomic service
read. Producer publication is now atomic, and service observations use the
core's matching snapshot helper. MSVC x64/ARM64 uses
[_InterlockedExchangeAdd64](https://learn.microsoft.com/en-us/cpp/intrinsics/interlockedexchangeadd-intrinsic-functions?view=msvc-170);
the x86 fallback uses existing atomic load/store with the sole-writer contract.
Other compilers use the same sequentially consistent atomic builtins as the
position helpers. The mapped header remains 80 bytes/ring v2; driver/service
ABI 11 and kernel bus ABI 6 are unchanged. This makes the diagnostic access
contract consistent; it is not a claim that the old count caused audible faults.

Core checks cover signed 16/24-bit boundaries, stereo/strided mono, destination
wrap, no source access when full, an exactly-sized source prefix when only one
frame fits, exact 32-bit payloads, invalid representations and unsigned counter
wrap. A real memory-only observer thread reads concurrent overflow updates
across the 32-bit carry boundary and checks the exact final total. Production
RingPump fixtures additionally exercise 16/24/32-bit source wrap, full-ring
drops, preserved unread samples and notification ordering. Full local
kernel/service build and all thirteen CTest checks pass. Sanitizer checks cover
source boundaries, not real Windows scheduling or a general race-freedom proof.

The removed scratch/copy/discarded conversion work is a source-level reduction;
no hardware CPU, spin-lock duration or acoustic latency gain has been measured.
No driver/service/device/player/volume/boot change was made on the host.
Settings reconnect and actual candidate audio/performance acceptance remain
pending.

## 0.3.40: coherent WaveRT packet progress, source only

GetPacketCount used to call UpdatePosition, advancing the play position and
pumping PCM, then return m_llPacketCounter that only TimerNotifyRT updated.
A query during timer delay therefore returned stale packet progress despite
already having copied the played samples. SetWritePacket also validated its
packet number against that old counter, rather than refreshing RUN progress.

UpdatePosition now commits frames, packet count and completion notifications
through one locked NotifyPackets helper. Position/presentation/count queries,
SetWritePacket and the timer use that path. The timer's duplicate count/event
logic was deleted. A query can signal progress before the delayed timer; the
later callback at the same position neither copies PCM again nor duplicates
the notification. SetWritePacket refreshes progress before classifying late
or overrun packets and rejects unknown flags and partial PCM-frame EOS lengths.

Event-mode checks in SetWritePacket, GetPacketCount and presentation-position
queries now share the position lock with FreeAudioBuffer. In particular, a
buffer release between SetWritePacket's former mode check and lock acquisition
could zero the notification count before its division. Stream state is also
read/committed under that lock; timer control and control-IRP reporting remain
outside it. The implementation retains Microsoft's [ordered SetState transitions](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavertstream-setstate).

The progress rules follow the [packet-count contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavertoutputstream-getpacketcount)
and [write-packet contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavertoutputstream-setwritepacket).
The existing cyclic-buffer catch-up bound, EOS sample limit and locked buffer
detach before unmap were inspected and retained. No timer-frequency, buffer
latency, codec, sample-rate or precision policy was tuned by this batch.

A new fixture compiles the actual clock/pump, position/packet/state methods,
timer callback and buffer-release bodies against fake OS/port APIs. Before
the fix it reproduced a 30 ms position advance with a stale zero packet count.
Corrected bodies pass delayed timer/query interleavings, one notification per
progress update, packet validation, release between check/acquisition, polled
mode, 44.1 kHz fractional frame accounting, 32-bit packet-number wrap, EOS pump
clipping, and ordered pause/resume/stop reset. The full local kernel/service
build and all thirteen CTest checks pass. Fake QPC/timer/port calls are not
real scheduling or audible playback evidence, and no performance measurement
or optimality claim follows. ABI remains service 11, kernel bus 6, ring 2.
No candidate was installed and no device/player/volume/boot setting changed.

## 0.3.39: completion after unlocking and waiter admission, source only

Policy callbacks were invoked under the parent's registration spin lock to
protect the child callback/context lifetime. Oa2dpApplyPolicy released its own
child lock, but then completed WAIT_FORMAT while the *parent* lock was still
held. The policy contract now transfers a removed, prepared IRP to the parent.
Connection, suspend, explicit-request fallback and audio registration callers
release their spin lock before completing that IRP. They do not access the
child context after releasing its registration protection. This changes the
kernel bus ABI from 5 to 6; both drivers must be updated together. Service ABI
11 and ring v2 are unchanged. No work thread, allocation or periodic poll was
added for policy delivery.

WAIT_FORMAT also used to insert into the cancel-safe queue under the device
spin lock. An already-cancelled insertion can synchronously invoke cancellation
completion. Insertion now occurs without an outer spin lock, followed by a
locked state/queue reconciliation. If a producer ran in that interval, a fresh
owned waiter receives its retained event; if stop or cleanup ran, a matching
waiter is failed. The code never accesses the original IRP after insertion,
because cancellation or a producer may already have completed it.

ControlCleanup closes admission by clearing the matching ControlFile under
the state lock before draining that file's waiters. Producers and post-insert
checks select waiters by file identity, so an old insertion cannot remain after
cleanup or consume a replacement service's event. A temporary file-object
reference preserves that identity through insertion, completion and the final
check. Cancellation with no owned waiter leaves a retained event available to
the next request. Completion stays outside state and registration spin locks.

These ownership rules follow Microsoft's [completion restriction](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-iocompleterequest),
[cancel-safe insertion](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-iocsqinsertirp)
and [removal contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-iocsqremovenextirp).
The production-body fixture now executes actual parent callbacks/fallback and
registration, child policy application, waiter snapshots/status, WAIT_FORMAT,
cancellation and cleanup against fake external APIs. It checks all four policy
delivery paths, unregister reentry on completion, immediate and queued cancel,
production before/after insertion, stop/cleanup in the gap, a replacement
control handle, current-file selection, preserved events and balanced file
references. Substituting the old child policy body (with only its signature
adapted to the new out parameter) or old WAIT_FORMAT independently reproduced
completion under a spin lock. Removing the post-insert check reproduced a
lost notification. Removing cleanup admission reconciliation or the producer's
file filter independently failed the respective regression. Corrected
production bodies pass.

The full local driver/service build and all twelve CTest checks pass. The
fixtures do not implement real Windows CSQ cancellation, OS scheduling or the
private profile manager, and do not prove the observed Settings failure's root
cause. No candidate was installed, no device/player/volume was changed, and no
reboot occurred. Actual audio, controls, reconnect and performance acceptance
remain pending.

## 0.3.38: connection-owned suspend cache, source only

The suspend cache stored only MpmIsSuspended. If the signalling link changed
during an external status update, the update's accepted boolean still came
from the old link. A later acknowledgement worker combined that old boolean
with the *new* signalling generation, allowing it to confirm the replacement
link's pending suspend/resume request before the service handled that link.
A later same-target callback could also schedule this premature confirmation.

The cache now stores MpmSuspendConnection together with MpmIsSuspended under
the transport lock, only after a successful external update. Both the policy
callback's already-at-target check and the acknowledgement worker validate
that cached epoch. The worker passes the cached epoch to PublishSuspended;
it never relabels cached state with the current link. A replacement request
stays pending until its own current service reply arrives. The disconnected
startup baseline uses epoch zero, which remains reusable while signalling is
closed and cannot satisfy an active link's request. Manager registration
explicitly resets this epoch. This is internal device-context state; the
driver/service, bus and mapped-ring contracts remain 11/5/2.

The existing production-body MPM fixture was extended first. Against the
unchanged 0.3.37 publishers it reproduced the incorrect replacement-request
completion. The corrected production bodies pass for both suspend and resume,
for later same-target callbacks, for a fresh current-epoch service reply and
for active-link versus disconnected startup baselines. The full local
driver/service build and all twelve CTest checks pass. No device operations,
deployment, playback or reboot occurred; Windows Settings and actual audio
remain hardware acceptance requirements. This does not make external manager
calls atomic or establish a measured performance improvement.

## 0.3.37: policy reentry handoff and worker epoch selection, source only

A status update can call back into ConnectionRequest or SuspendRequest before
the publisher commits its new cache. A new same-target request then sees the
old cache and omits its already-at-target acknowledgement. The old publisher
correctly refuses to retire the replacement serial, but formerly left that
request pending without scheduling the acknowledgement it now needs.

After a successful update, both publishers commit their cache, retire only
their captured serial, and check whether a still-pending request matches the
new cache and current connection. If so they queue the existing PublicationWork.
The worker revalidates and confirms the newer serial. Failed updates do not
advance the cache; opposite-target, unsettled-disconnect, obsolete-generation
and stopping cases do not gain a new confirmation from this reconciliation.
The MPM call's rundown reference now covers cache bookkeeping and the last
work enqueue, before stop can unregister and finish its final work-item flush.

There was a second handoff gap: PublicationWork read PublishedConnection before
acquiring PublicationLock. It could sample zero while the preceding publisher
was still committing a successful TRUE, then acquire the lock and reject its
own obsolete snapshot. The worker now acquires PublicationLock before choosing
the epoch and holds it through the common locked publication helper. The
public entry point uses that same helper; suspend acknowledgement retains its
existing separate acquisition. There is no additional thread or periodic poll.

The framework permits [requeueing a running work item](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfworkitem/nf-wdfworkitem-wdfworkitemenqueue)
on the project's KMDF version; PublicationLock serializes overlapping callbacks.
The rundown ordering follows [access rundown](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-exwaitforrundownprotectionrelease)
and [work-item flush](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfworkitem/nf-wdfworkitem-wdfworkitemflush)
contracts. No spin lock is held across an external MPM call.

The new production-body fixture compiles the actual manager callbacks,
connection/suspend publishers, suspend acknowledgement and connection worker
against fake OS/manager APIs. It deliberately injects same/opposite requests,
generation changes and stopping during calls, and a preceding commit while the
worker acquires its lock. It checks confirmation flags, pending-serial ownership,
failure handling, no premature Disconnect confirmation, no unnecessary requeue,
balanced rundown and enqueue-before-rundown-release. Substituting each original
0.3.36 connection publisher, suspend publisher and publication worker into the
build-only fixture produced the expected independent regression failures;
restoring the corrected production bodies passed. The local full driver/service
build and all twelve CTest checks pass.

These checks prove the selected local ordering rules under injected timing,
not the private MPM implementation or actual Windows Settings behavior. An
external update is still not atomic with an asynchronous policy callback;
this batch repairs missed follow-up work, not that cross-component atomicity.
No installed package, device, player or boot policy was changed. ABI remains
driver/service 11, bus 5, ring 2. Actual sound, Settings disconnect/reconnect,
existing-player recovery and performance remain unaccepted for this candidate.

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
