# Audio clock, PCM computation and output-lifetime review

Reviewed 2026-11-11 against source 0.3.54 and 0.3.59–0.3.62. The 0.3.61/0.3.62
observations below include a connected headset. Earlier computational reviews
were performed without the headset and exclude Windows rendering, scheduling
under audio load, Bluetooth transmission and headphone playback.

## 0.3.63 candidate: precision-only changes also delimit PCM epochs

A controlled regression on the preceding production body reproduced packed
16 -> packed 24 PCM at the same rate/channel count without a ring epoch change:
`changed=0, epoch=0->0, old read=0 frames=2`. Both source formats normalize to
the same S24-in-int32 storage. Valid-bit changes within an int32 container also
share one storage representation. The old consumer could therefore accept new
source samples before the asynchronous format notification retired its encoder,
potentially applying the previous precision policy. This is a reproduced format
boundary defect, not an attribution of an observed headset artifact.

The kernel now keeps the complete producer PCM tuple under its existing ring
lock and begins an epoch for every change to that tuple. A precision/container
change advances the sequence and first-frame position even if storage shape
stays the same. The old consumer rejects the new generation; obsolete producer
metadata and old-format state reports are also rejected. Running publication
has moved into the accepted format report, removing the separate unqualified
RUN/STOP flag update. The device-to-ring lock order is unchanged, and external
state callbacks still execute after unlocking. No mapped-header/IOCTL layout,
additional timer, worker or resampler is introduced.

The production-body fixture covers six precision/container transitions,
unchanged-format no-ops, A -> B -> A, stale consumer/producer rejection, an
uninitialized source descriptor, late old-format STOP, and accepted RUN/STOP
notification. OS event and state-callback boundaries are controlled. The real
producer/consumer thread fixture includes 10,000 epoch changes, half retaining
the same normalized storage, with generation-tagged sample and buffer-boundary
checks. The complete 0.3.63 native Release build and all 29 local checks passed,
with CTest started only after the build completed. This source is not yet
installed; loaded 0.3.62 and hardware acceptance remain separate.
It does not explain the earlier 2.178-second PCM supply gap
or implement automatic per-file shared-mix selection.

## 0.3.63 candidate: remove fixed S24 quantization before native LDAC input

The common adapter contract previously rounded full int32/float32 input onto
a fixed 24-bit integer grid, then shifted those samples into libldac's S32
container. The pinned Sony library also accepts S32 and F32 directly: its
`setpcm_ldac.c` scales those samples into its internal scalar representation.
Its advertised wireless precision is not a requirement to quantize all PCM
onto the same fixed 24-bit grid before invoking that API.

Format-aware encoder creation now selects native S32/F32 input for 32-bit
endpoint containers in automatic mode, including 24-in-32. Explicit 16/24-bit
settings retain their conversion boundary; automatic input with at most 16
valid integer bits keeps the S16 route. Other codecs retain the existing
adapter contract. Source/codec sample-rate mismatches are rejected. No new
resampler, gain adjustment, EQ, codec negotiation or radio behavior is added.

Native preparation preserves representation while mapping one/two channels.
Float sanitization/clipping follows channel mixing; integer downmix uses a
wide sum and rounds once to full int32. Equal-channel int32 input already in
the service buffer requires no conversion loop or copy. The LDAC adapter no
longer allocates/converts an intermediate PCM buffer for these native inputs.
This is an input-fidelity and work-reduction change, not a measured audible
improvement, end-to-end CPU speedup, or 32-bit/lossless Bluetooth claim.

The production preparation fixture checks full integer extremes, signed
half-sample downmix, low-level float preservation, same-buffer mono expansion,
unaligned input, bounds, NaN/infinities and mixing before clipping. It also
shows that the previous S24 boundary maps +/-2^-26 float samples to zero,
whereas native preparation retains them. Such levels do not establish an
audible benefit. The actual LDAC adapter matches direct pinned-libldac output
byte for byte over 1,536 calls, covering four supported rates, mono/stereo,
full S32/F32/24-in-32, input immutability, HQ frame sizes and explicit precision
selection. The native Release build and all 29 local checks pass. A preliminary
check run overlapped compilation and could not launch a locked executable;
the reported complete run was repeated only after the build exited successfully.

This source has **not been installed or heard on the headset**. The currently
loaded source remains `292a0c98b07b545c8c8b377eeef307056ab0b5fb` (0.3.62).
At the quality inspection, Windows used a 96 kHz shared mix and supplied
24 valid integer bits in a 32-bit device container. Native input cannot recover
detail already removed upstream. Media/Movie/GameMedia effects enumeration
returned no advertised effect, the active endpoint had no FxProperties key,
and Equalizer APO's configuration contained only a bypass comment. This does
not inspect player-specific processing or the headset's stored DSP settings.
The shared-mix rate and EQ were left unchanged; automatic per-file rate
selection is still absent, as described in the main format-policy section.

## 0.3.62: short passive observation after activation

Both actual kernel images and the running service match signed source
`292a0c98b07b545c8c8b377eeef307056ab0b5fb`. A 20.392-second ETW capture ran
from 20:32:22 to 20:32:43 local time after activation. It made no endpoint,
player, radio or format changes and did not overlap a local build.

| Observed metric | Result |
|---|---:|
| Decoded ETW events / lost events / lost buffers | 3,846 / 0 / 0 |
| Media completions / failed completions | 3,823 / 0 |
| Completion latency p50 / p95 / p99 / maximum | 0.800 / 1.482 / 5.638 / 11.264 ms |
| Completion interval p99 / maximum | 8.54 / 13.4 ms |
| Peak buffered PCM / maximum data-event wait | 391 frames (4.07 ms) / 3.528 ms |
| Ring overrun frames, excluding first PCM window | 0 |
| Media rate including protocol headers | 1,009,479 bit/s |
| Service CPU over 20.459 seconds, tracing included | 78.125 ms (0.382% of one logical CPU) |

There was one PCM stream generation at 96 kHz. No application snapshots were
collected during this short trace, so it does not independently attribute the
PCM to PotPlayer. Separate post-activation snapshots found the existing
PotPlayer session on the native endpoint; a later sample contained nonzero
signal. Lower-driver completion is not headphone rendering, and this short
interval does not establish audible quality, lip sync, a relative performance
gain, or resolution of the earlier interruption. Collection stopped normally.
See the [activation limitations](HARDWARE-BRINGUP.md).

### Video playback-speed observation, with application-reported rates

The user subsequently began video-speed testing. Core Audio snapshots identified
Chrome as the active source; PotPlayer and QQ Music were paused. No radio,
service, endpoint, codec or player setting was changed by the observer.

| Metric | Chrome, 20:48:33–20:53:39 | Chrome, 20:54:27–20:56:09 |
|---|---:|---:|
| Trace duration | 306.363 s | 102.473 s |
| Decoded events / lost events / lost buffers | 57,750 / 0 / 0 | 19,317 / 0 / 0 |
| Media completions / failures | 57,441 / 0 | 19,213 / 0 |
| Completion latency p99 / maximum | 7.831 / 28.277 ms | 7.321 / 18.774 ms |
| Approximate completion interval p99 / maximum | 9 / 32 ms | 9.2 / 23.7 ms |
| Peak buffered PCM / maximum data-event wait | 940 frames / 9.042 ms | 1,026 frames / 10.392 ms |
| Ring overrun frames, excluding first PCM window | 0 | 0 |
| Media rate including protocol headers | 1,009,497 bit/s | 1,009,506 bit/s |
| Service CPU, one logical CPU, tracing included | 0.602% | 0.548% |
| Read-only audio snapshots | 60 | 20 |

Both windows retained one PCM stream generation at 96 kHz, native render defaults
for all three roles and Chrome with nonzero signal at every audio snapshot.
No new service error or connection rebuild was observed. No local build ran
during collection. Completion interval estimates use the decoder's displayed
timestamps; microsecond completion latency comes from the event field itself.

A separate read-only Windows GSMTC observer queried
[PlaybackRate](https://learn.microsoft.com/en-us/uwp/api/windows.media.control.globalsystemmediatransportcontrolssessionplaybackinfo.playbackrate)
without sending playback-control requests. Forty-five samples overlap the
second trace, from 20:54:28 to 20:55:56, all reporting Chrome Playing at **1.5x**.
The original five-minute trace did not collect rates, so its clean result is
not attributed retrospectively to 1.5x. These are application-reported rates
and sampled state, not verification of rendered video speed or audible lip sync.
The second trace does not establish other speeds or the act of switching speed.

An earlier 87.412-second segment at 20:45:31–20:46:58 had 16,389 successful media
completions and zero trace loss/overrun, but its collector stopped on a sample-file
sharing error. A subsequent collector startup stopped on a summary-formatting
error. Neither stopped the audio service. Both partial captures are preserved;
the first lacks a final CPU measurement. The observer now appends live snapshots
to a shared-read JSONL stream and writes the final array after closing it.
These disjoint captures must not be reported as uninterrupted monitoring.
Further rate-change observation continues separately from these completed results.

### Continuous speed-change trace and an unresolved PCM supply gap

A 612.796-second capture on the same 0.3.62 runtime ran from 20:56:53 to
21:07:06. It included 120 paired Core Audio/GSMTC snapshots, approximately
five seconds apart. Chrome was active with nonzero signal at every snapshot;
QQ Music and PotPlayer were paused. The native endpoint ID and all three
render defaults remained unchanged. Reported rates and sampled coverage were:

| Chrome rate | Samples | Longest consecutive first-to-last sample span |
|---|---:|---:|
| 0.25x | 32 | 157.736 s |
| 0.3x | 1 | Single observation |
| 0.5x | 7 | 30.576 s |
| 1x | 2 | 5.097 s |
| 1.5x | 11 | 51.000 s |
| 1.75x | 1 | Single observation |
| 2x | 66 | 199.708 s |

These spans are bounded by snapshots; they are not precise transition times or
equal-duration acceptance tests. There were 115,105 decoded events, no trace
loss, 114,491 successful media completions and no failed completion. Completion
latency p99 was 8.235 ms and maximum 25.786 ms. Ring overrun was zero after the
first window, with 932 peak buffered frames. Service CPU was 0.699% of one
logical CPU with tracing included. The stream remained at 96 kHz LDAC HQ,
24 useful input bits, one stream generation and one connection generation.

**One approximately 2.178-second media-completion gap must not be hidden by
the successful-send count.** It occurred around 20:59:54.099–20:59:56.277.
The corresponding PCM window recorded a 2.171557-second data-event wait, zero
overrun and 321 peak buffered frames. The neighboring windows had roughly
3 ms maximum waits. The preceding GSMTC sample reported Playing at 2x;
the next, about five seconds later, reported Playing at 1x. No service error,
encoder restart or connection rebuild accompanied the gap.

This localizes the stall to waiting for PCM availability rather than a slow or
failed media-send completion. The evidence does not separate a deliberate
pause/seek/video change from an upstream supply or notification problem.
Five-second state snapshots cannot rule out a short pause between samples.
The next observation samples playback state/rate every second while keeping
Core Audio snapshots at five-second intervals; it also includes Windows audio
GlitchDetection events. No driver or playback configuration was changed.

A separate 120.290-second Windows audio trace at 21:03:48–21:05:48 enabled the
installed Microsoft-Windows-Audio provider's GlitchDetection keyword
`0x0400000000000000` at level 5. The provider was registered in audiodg and
Chrome; the running session's provider/mask were verified. The installed
manifest maps this keyword to engine starvation/overread and KS position/glitch
events. The trace contained only its two headers, with zero lost events/buffers:
no event from that selected keyword was recorded. It began after the earlier
PCM gap and cannot explain it. This is a bounded observation, not proof that
every detector path ran or that the headset rendered uninterrupted sound.

### One-second state sampling separates a later pause from transmission failure

A subsequent capture ran from 21:08:43 to 21:19:01 local time for 618.063 seconds.
It combined the native stream/transport providers with the same Windows audio
GlitchDetection keyword, 600 playback-state/rate snapshots and 120 Core Audio
snapshots. No playback settings, runtime binaries or radio state changed, and
no local build ran. Chrome was the only sampled application with audio signal;
QQ Music and PotPlayer reported Paused. Sampled Playing rates included
0.25x, 0.5x, 0.75x, 1x, 1.25x, 1.5x and 2x, plus short 0.35x/0.45x transitions.
These unequal sampled spans do not establish separate acceptance at every rate.

| Observed metric | Result |
|---|---:|
| Decoded events / lost events / lost buffers | 112,648 / 0 / 0 |
| Media completions / failed completions | 112,046 / 0 |
| Completion latency p99 / maximum | 7.267 / 29.090 ms |
| Approximate completion interval p99 | 9 ms |
| Largest interval outside the pause-associated gap | 32 ms |
| Peak buffered PCM / maximum data-event wait, excluding first windows | 415 frames / 3.996 ms |
| Ring overrun frames, excluding first window per stream | 0 |
| Service CPU, one logical CPU, tracing included | 0.576% |

The sole completion gap of at least 100 ms lasted approximately 20.475 seconds,
from 21:09:18.501 to 21:09:38.976. Twenty one-second samples inside it reported
Chrome Paused. The native status subsequently reported Suspended and returned
to Streaming; PCM stream generation advanced from 3 to 4 while connection
generation stayed at 1. The surrounding samples show Playing before and after
the gap. This is consistent with a playback pause/resume, without a connection
rebuild or failed send; it does not identify who initiated the pause or measure
exact resume latency. The whole-window average of 976,061 bit/s includes that
pause and is not evidence of LDAC quality reduction. The endpoint stayed at
96 kHz under the unchanged LDAC HQ policy.

The native endpoint ID and all three default render roles were unchanged at
every Core Audio snapshot. No event from the selected Windows audio keyword
was recorded, with no trace loss. The session stopped normally. This later
pause correlation does **not** explain or close the earlier 20:59:54 PCM wait:
that separate interval still lacks sufficiently frequent playback-state data.
No newly unexplained long supply gap was observed in this capture; neither
successful sends nor absent selected glitch events establish audible rendering
or physical lip sync.

A separate follow-up ran from 21:19:57 to 21:25:08 for 310.293 seconds with
300 state/rate samples and 60 Core Audio snapshots. The same observer and
unchanged runtime recorded a longest consecutive 2x sample span of 279.748
seconds, with short 0.5x and 1x observations. All 57,237 media completions
succeeded, with no lost trace events/buffers or ring overrun after excluding
each stream's first window. Completion latency p99 was 7.536 ms and maximum
35.216 ms; peak PCM buffering was 424 frames and maximum recorded data-event
wait was 4.514 ms. Service CPU was 0.836% of one logical CPU, including tracing.

Its only completion gap of at least 100 ms was approximately 5.027 seconds
at 21:24:41.035–21:24:46.062. Five samples inside it reported Chrome Paused;
stream generation advanced from 4 to 5 while connection generation remained 1.
All other completion intervals were at most approximately 32 ms. The native
default endpoint and 96 kHz LDAC policy were unchanged; no selected Windows
audio glitch event was recorded. The service process stayed the same and the
trace stopped normally. These are two separately bounded captures, not one
continuous 15-minute observation, and neither resolves the earlier PCM gap.

## 0.3.61: passive playback observations and remaining failure visibility

Installed source `b378045332f704af05f07abc12c44772f9f985a1` was observed without
changing radio, endpoint, player, service or codec settings. The user reported
no renewed Chrome/YouTube audio stutter so far and continued testing scenarios.
This is a current user observation, not full long-term audio acceptance.

A 20-second ETW capture had 3812 successful media completions, no lost trace
events/buffers and no ring overrun after excluding its first PCM window.
Completion latency p99 was 5.779 ms and maximum 9.927 ms. A subsequent session
snapshot identified QQ Music as active; this short run was not a Chrome test.
After that capture, at 19:42:57 local time, the service recorded a media-send
failure and tore the session down. Following connection timeouts, streaming
resumed at 19:43:36 without restarting the service. This interruption occurred
outside that trace window; its cause is unresolved. The generic 0.3.61 error
does not distinguish a native completion failure from slot-wait timeout, and
there is no evidence here separating physical/link interruption from software
failure. The preceding clean trace must not be used to erase this observation.

A second, 303.314-second capture ran from 19:54:51 to 19:59:54 local time.
Twenty read-only application snapshots, approximately 15 seconds apart, showed
Chrome active with nonzero signal, followed by QQ Music with nonzero signal.
No simultaneous audible-app case was captured. The native endpoint remained
default for all three render roles at every snapshot. The stream stayed at
96 kHz stereo, LDAC HQ policy, 24 useful bits in a 32-bit integer input container.
This describes the endpoint/encoder stream, not either player's original file
metadata. No format or connection generation changed during this interval.

| Observed metric | Five-minute result |
|---|---:|
| Decoded ETW events / lost events / lost buffers | 57,165 / 0 / 0 |
| Media-send completions / failed completions | 56,859 / 0 |
| Completion latency p50 / p95 / p99 / maximum | 0.793 / 2.938 / 7.790 / 29.649 ms |
| Completions taking at least 20 ms / 100 ms | 16 / 0 |
| Completion interval p99 / maximum | 9.3 / 28.3 ms |
| Peak buffered PCM / maximum data-event wait | 383 frames (3.99 ms) / 3.963 ms |
| Ring overrun frames, excluding first PCM window | 0 |
| Observed media submission rate including protocol headers | 1,009,503 bit/s |
| Service CPU over 303.314 seconds, tracing included | 1.719 seconds (0.567% of one logical CPU) |

The service process remained unchanged and added no error or teardown entries
during the second capture. That interval overlapped a local native build and
tests; it is not a controlled benchmark against other versions. Numeric ETW
events contain timing/counters, not audio content. Both trace sessions stopped
after collection. The first PCM window is excluded because collection starts
midstream. Lower-driver completion is not headphone rendering, and completion
intervals alone do not prove audible underrun or lip-sync quality.

The subsequent 0.3.62 source retains the failing media stage and original Win32
error rather than conflating all failures. Actual production wait/send/drain
bodies are covered by the expanded regression; the full native Release build
and all 28 local checks pass. It changes neither the wait budget nor slot/buffer
ownership, and adds no successful-packet logging. See
[failure attribution](DIAGNOSTICS.md#media-send-failure-attribution-deployed-in-0362).
It was not installed during these tests and is not presented as a fix for the
earlier interruption. Sleep/resume, simultaneous applications, physical
synchronization and supported-format transitions still need separate evidence.

### PotPlayer video scenario and observed source/mix mismatch

The user then switched to PotPlayer and supplied its playback-information
screen. It reports FLAC input, 48,000 -> 48,000 Hz, 24 -> 24 bits, two -> two
channels, and DirectSound Audio Renderer. In the same scenario, read-only
endpoint/session observations show PotPlayerMini64 active with nonzero signal,
a 96 kHz Windows shared mix, and 96 kHz LDAC HQ transmission with 24 useful
bits in a 32-bit integer container. Thus this source undergoes 48 -> 96 kHz
conversion before the driver; the automatic policy follows the endpoint PCM,
not the original FLAC file. Upsampling does not add source detail. A matching
48 kHz shared format would avoid this rate conversion for this source, but
this observation does not measure an audible advantage. No setting was changed
while the user was comparing playback scenarios. See Microsoft's
[format-conversion and shared-mix contract](https://learn.microsoft.com/en-us/windows/win32/coreaudio/device-formats).

A separate passive capture ran for 302.608 seconds, 20:04:30–20:09:32 local
time, on the same unchanged 0.3.61 runtime. All 20 application snapshots found
PotPlayer as the active audio application and the native endpoint default for
all three render roles. Results:

| Observed metric | PotPlayer result |
|---|---:|
| Decoded ETW events / lost events / lost buffers | 57,033 / 0 / 0 |
| Media-send completions / failed completions | 56,726 / 0 |
| Completion latency p50 / p95 / p99 / maximum | 0.796 / 2.297 / 6.941 / 23.908 ms |
| Completion interval p99 / maximum | 9.0 / 28.2 ms |
| Peak buffered PCM / maximum data-event wait | 423 frames (4.41 ms) / 4.155 ms |
| Ring overrun frames, excluding first PCM window | 0 |
| Media rate including protocol headers | 1,009,501 bit/s |
| Service CPU, tracing included | 1.547 seconds (0.511% of one logical CPU) |

The two captured KS property calls also completed successfully. There was one
PCM stream generation, no new service error or teardown, and no service restart.
The capture stopped normally. Part of it overlapped an independent local build;
it is not a controlled performance comparison. The transport result is evidence
for this interval, not an audible lip-sync assessment, a measured zero-underrun
result at the headset, or simultaneous-player acceptance.

### Read-only format queries are not streaming acceptance

Before deployment, IsFormatSupported queries on 0.3.61 covered 36 stereo
variants at 44.1, 48, 88.2 and 96 kHz: integer16/24/32 and float32 with
WAVEFORMATEX, plus those representations and 24 valid bits in a 32-bit integer
container with WAVEFORMATEXTENSIBLE. All 36 exclusive-mode queries returned
S_OK. Shared-mode queries returned S_OK for the nine 96 kHz variants and
S_FALSE with a closest format for the other 27. The mix remained 96 kHz.
The probe called neither Initialize nor Start; it establishes format-query
behavior, not successful exclusive playback, rate switching or audible output.
Shared clients still feed one Windows mix; capability queries do not provide
per-file rate adaptation or recover original metadata from mixed PCM.

## 0.3.60: keep the presentation clock monotonic across delay updates

The driver estimated presentation position by subtracting the latest transport
and headset delay from the transferred frame count. Increasing the delay
could therefore move the reported position backward. The old production body
reproduces 2640 -> 960 frames at 48 kHz when a 45 ms delay becomes 90 ms while
RUN time advances from 100 to 110 ms: a 35 ms backward clock step.
While paused, changing the delay also changed the reported position even
though the transferred position was frozen.

Commit the presentation estimate under the existing position lock and retain
the largest value within the stream epoch. When delay increases, the position
waits for the new estimate to catch up. Commit at RUN -> PAUSE, including time
since the last query, then freeze while paused. STOP clears the committed
position; RUN resumes normal advancement. Read device latency before acquiring
the position lock, so this change introduces no nested device/position locks.
DMA position, packet counts, notification cadence and PCM samples are unchanged.

This follows the upper-layer
[audio clock contract](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclock-getposition)
that successive device positions do not decrease and a stopped client clock
is frozen until restart/reset. The WaveRT
[presentation position](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavertoutputstream-getoutputstreampresentationposition)
is used by upper layers for synchronization. Keep the kernel structure's QPC
field in [raw counter ticks](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ksmedia/ns-ksmedia-ksaudio_presentation_position);
its units differ from the converted user-mode IAudioClock timestamp.

The existing production WaveRT fixture reproduces the old backward step and
checks delay growth/catch-up at 44.1/48/88.2/96 kHz, pause-time commit, changes
while paused, resume, STOP reset and raw timestamps at three QPC frequencies.
OS timing, latency reports and buffer transfers are controlled. This establishes
the clock arithmetic and state contract, not accurate remote playback position,
physical lip sync or resolution of the reported Chrome audio stutter. The
estimate still depends on reported headset delay and does not measure actual
radio, encoder and receiver queue latency.

The local native x64 Release build, all 28 checks and
[exact-source CI](https://github.com/TruthNian/xm6-audio/actions/runs/43008980426)
pass. Version60 is activated without a computer restart; both actual kernel
images and the running service match the signed local package. The
[runtime record](HARDWARE-BRINGUP.md) keeps connected audio and synchronization
acceptance separate from the controlled clock regression and offline activation.

## 0.3.59: send complete batched frames before waiting for PCM

SBC accumulates complete encoded frames in the RTP packet buffer. The service
previously flushed that buffer only when another encoded output filled the
packet. If the PCM ring then lacked one full encoder input block, the sender
waited indefinitely for new PCM while the complete tail stayed unsent. With
no later input, eventual stream retirement discarded it. This is a distinct
buffering omission, not an observed explanation of Chrome's LDAC stutter.

After a successful ring read reports insufficient input, flush any already
complete RTP frames before waiting. Keep aggregation while PCM is available.
An unresolved BUSY format snapshot does not authorize a flush; failed sends
use the existing recovery path and leave before entering the data wait.
No new timer, worker, PCM padding, sample conversion or codec change is added.
Sparse SBC input can produce smaller packets; this exchanges batching
efficiency for timely delivery of already complete audio. Radio throughput
and audible latency under this policy have not been measured.

The existing service RTP fixture now executes the actual PCM-wait branch with
the real RTP core and controlled input availability, send result and OS wait.
The previous production body fails the single-SBC-frame submission assertion.
The corrected body sends that 132-byte packet (119 encoded bytes plus 13 bytes
of headers) before waiting, preserves its timestamp and advances the next one
by 128 samples. Coverage includes a three-frame MTU-sized tail, empty wakeups,
subsequent input, BUSY state, send failure and wait failure. Actual compiled
LDAC checks still pass at four sample rates, two precision settings and three
MTUs; its existing per-output packet flushing is unchanged. These checks do
not establish whole-stream timing, RF reception, or headset rendering.

The local native x64 Release build, all 28 checks and
[exact-source CI](https://github.com/TruthNian/xm6-audio/actions/runs/43007583981)
pass. Version59 is activated without a computer restart; both actual kernel
images and the running service match the signed local package. The
[runtime record](HARDWARE-BRINGUP.md) retains the separate hardware-acceptance
limits; no connected SBC playback or latency measurement was performed.

## Integer preparation: preserve the samples, remove repeated division

The release x64 assembly contained signed division inside the integer sample
loops. Precision reduction and stereo averaging only use powers-of-two
divisors. Replace those divisions with unsigned magnitude rounding/shifts,
then clamp before narrowing. Preserve ties away from zero, saturation,
unaligned reads and identical-buffer expansion. Mixing still precedes the
single precision reduction. Float conversion and format policy are unchanged.

An independent quotient/remainder oracle matches 2,424,844 output samples,
including all int16 values represented in S24/full-int32, positive/negative
rounding boundaries, extrema, channel conversion, unaligned input and in-place
buffers. Existing PCM/encoder/ring checks also pass. The optimized release
assembly removes the per-sample signed divisions.

The standalone [benchmark](../tools/pcm-benchmark) compares exact baseline
source `b63ed7e816cab8a6329b050490bcdb39d4b6d09a` with the new implementation.
It uses 128-frame blocks, fresh synthetic input copied into the working buffer
before each in-place conversion, 2,000 warmups and six 25,000-call batches.
Both C implementations are compiled in one process with prefixed API names;
batch order alternates which runs first. A normal-priority benchmark thread
is pinned to one allowed logical CPU and restores its affinity afterward.
The compiler was MSVC 19.29, Release x64. Timing includes the fresh-buffer copy.

| PCM path, stereo -> stereo | Before, ns/block | After, ns/block | Ratio |
|---|---:|---:|---:|
| Full integer32 -> 16 useful bits | 1890.896 | 363.968 | 5.20x |
| Full integer32 -> 24 useful bits | 1847.536 | 355.972 | 5.19x |
| Integer-in-S24 -> 16 useful bits | 1927.984 | 366.040 | 5.27x |

All 24 format/channel/precision checksums agree. The
[complete paired CSV](perf/pcm-054.csv) includes float and already-S24 controls.
An earlier separate-process trial showed a float slowdown; that did not
reproduce in the alternating single-process comparison. No float-path speedup
is attributed to this change. These finite timings describe this computational
step, not whole-stream CPU improvement, reduced jitter, audible quality or
resolution of the reported Chrome stutter.

### Reproduce on Windows

From the repository root, export a compatible baseline and configure the
standalone benchmark. Use the installed Visual Studio generator:

```powershell
New-Item -ItemType Directory -Force build/pcm-baseline | Out-Null
python -c "import pathlib,subprocess; pathlib.Path('build/pcm-baseline/pcm-before.c').write_bytes(subprocess.check_output(['git','show','b63ed7e816cab8a6329b050490bcdb39d4b6d09a:core/src/pcm_ring.c']))"
$baselineSource = (Resolve-Path build/pcm-baseline/pcm-before.c).Path
cmake -S tools/pcm-benchmark -B build/pcm-perf -G "Visual Studio 16 2019" -A x64 "-DXM6_PCM_BASELINE=$baselineSource"
cmake --build build/pcm-perf --config Release
./build/pcm-perf/Release/pcm_benchmark.exe
```

Compare matching rows and checksums. A source/header ABI mismatch is not a
valid performance comparison. Single-implementation timing is also supported
without `XM6_PCM_BASELINE`; use paired timing for old/new conclusions. No audio
endpoint, radio, pairing or player is opened by this tool.

## AAC output is not the same as an output-sample pointer

With caller-allocated samples, `pSample` remains non-null when ProcessOutput
returns events but marks NO_SAMPLE. The old drain queued a zero-byte AAC access
unit in that valid events-only case. A production-body fixture reproduces it.
Version54 respects NO_SAMPLE, rejects unmarked empty access units and joins
common object cleanup before returning input-needed. A returned empty event
collection must also be released; the old early-return branch omitted it.
These follow the documented [ProcessOutput ownership and output contracts](https://learn.microsoft.com/en-us/windows/win32/api/mftransform/nf-mftransform-imftransform-processoutput).

Controlled COM boundaries cover caller/MFT-allocated output, events without
media, input-needed with an owned empty collection, allocation/add-buffer
failure, output failure, contiguous-buffer/lock failure and empty data. Actual
Windows MFT checks encode mono/stereo at 44.1/48 kHz, including additional
availability probes while an encoder is alive. No host memory-growth incident
or actual events-only output from the installed AAC MFT was observed; these
are reproduced API-edge corrections, not a claimed explanation of user audio
stutter. The changes add no runtime worker, watchdog or polling loop.
