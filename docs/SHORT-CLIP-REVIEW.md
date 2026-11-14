# Short-clip pipeline review (0.4.1–0.4.6)

On 2026-11-12, the owner reported missing beginnings of pronunciation clips
in the Shici Windows application on the 0.4.0 native driver. This concrete bug
reopens development within the existing headset/host scope; platform expansion
remains paused.

## Reproduced defect

Windows can write the beginning of a clip to the PCM ring while the service
is completing AVDTP START and creating the encoder. `Session::stream_loop`
then unconditionally called `oa2dp_ring_discard`, moving the consumer past
all those valid samples. A fixture compiling the real admission block queues
50 ms of a new clip: on 0.4.0 the first requested 128 frames returns zero.
This is a deterministic data-loss defect, not a speculative RF explanation.

A 94-second passive Shici trace on 0.4.0 contained 16 WaveRT stream creations,
6,750 completed media sends, no failed media completion and no ring overruns
in the emitted PCM windows. That does not prove audible completeness: samples
discarded before encoding never appear as failed Bluetooth sends.

## Correction

Encoder admission preserves queued current-epoch PCM. New WaveRT streams now
establish a producer epoch even when their format is unchanged. Reusing a
STOPped stream establishes its next epoch before the first RUN sample is
pumped; PAUSE/resume retains the current epoch. The existing checked ring read
excludes the preceding clip without clearing the new one. Stream positions
remain monotonic and the radio/codec policy is unchanged.

Focused regressions cover the queued prefix and unread suffix for all ring
representations, identical-format clip replacement, old-epoch rejection,
STOP/reuse and PAUSE/resume. Opt-in EncoderStart/FirstPcm timing events record
only epochs, frame positions and counts, never audio content.

## 0.4.1 result: incomplete correction

The exact 0.4.1 package was locally test-signed and activated with both kernel
images and the service verified. The owner still reported a missing beginning
or another playback problem. It is **not** an accepted fix or a stable release.

In the deployment trace, nine subsequent starts preserved their first checked
PCM block. Two started with 32,768 and 32,750 queued frames (the ring capacity
is 32,768). In the first 100 ms of those nine starts, completed LDAC HQ packets
represented approximately 123–155 ms of 96 kHz audio. These are host transport
completion times and represented sample counts, not proof of receiver playback
or a measurement of exactly which audible samples were lost. They expose
startup backlog and catch-up bursts that the first patch did not address.

The earlier zero-overrun-window result was also too narrow: its baseline was
taken after encoder creation, so startup losses and subsecond streams were not
fully represented. No PCM content or private device identifiers are published.

## 0.4.2: combined pipeline correction

The owner requested a complete source review and a single combined deployment,
rather than incremental installation/listening experiments. The review covers
the WaveRT clock, source lifetime, ring admission, codec input, packet pacing,
end-of-stream and cancellation paths.

| Stage | Defect or gap | Correction and offline coverage |
| --- | --- | --- |
| Virtual hardware clock | RUN immediately advanced the OS cyclic buffer while AVDTP START and encoder creation were still pending. A finite ring could overflow before the first reader, despite the removed discard. | The encoder admits its exact producer epoch only after transport/codec readiness. The initial WaveRT clock waits without consuming packet zero. The actual clock body is tested with a 900 ms delayed consumer, plus paused-stream restart. |
| Stream ownership | A pin open/probe changed the producer epoch before RUN. Format equality also did not distinguish a late callback from an older identical-format stream. | Establish the epoch at RUN. Pump, EOS, ready checks and state publication carry the producer sequence. Tests reject old identical-format PCM, STOP and EOS without disturbing the new stream. |
| Repeated short clips | Every new producer sequence forced SUSPEND/START, recreated LDAC and reset RTP identity even when full PCM format was unchanged. | The policy thread authorizes only a full-format match (including valid bits); the existing encoder adopts that epoch. Actual format, HFP and disconnect changes still retire the stream. Production-handler tests exercise these distinctions. |
| PCM tail / codec lookahead | The reader required a complete block. Partial EOS data remained queued; an empty ring did not supply the subsequent samples needed to emit codec lookahead. | Preserve the partial tail and zero-pad only after PAUSE/STOP or committed EOS. Paced silence during the existing three-second idle grace drains codec output and maintains the receiver stream between short clips. Active partial blocks still wait for real PCM. Peer START without a source also gets a bounded idle deadline. |
| Media timing | Completed/queued Bluetooth sends limited throughput but did not enforce the represented audio time; queued startup PCM could be sent as a burst. | Sample-count packet deadlines, fractional clock carry and cancellable waitable timers. A long scheduling stall cannot cause an unbounded catch-up burst. RTP fragmentation advances the deadline only once per represented frame group. |
| Observability | First-sample data loss was invisible to successful-send counts; startup overruns and short stream tails could escape one-second summaries. | EncoderStart/PcmEpoch/FirstPcm/MediaSubmit/EncoderEnd record epoch positions, source/padding counts, media-frame counts and total overruns. ETW remains opt-in and contains no audio samples. |

Ring protocol version 3 occupies an existing reserved header word for consumer
admission and rejects mixed old/new mappings. The endpoint driver and service
must be deployed together; no registry sound tweak, per-application workaround,
radio ownership change or background keepalive application is introduced.

The complete production `stream_loop` is compiled into an offline fixture with
scripted PCM/time/OS waits and the real LDAC encoder. At 44.1/48/88.2/96 kHz it
checks that two short clips retain every one of 4,226 source frames across two
producer epochs with one encoder, emit continuous RTP timestamps, obey media
deadlines, flush through silence and cancel cleanly. Separate ring fixtures
cover mono/stereo, all ring representations, 1/127/128/129/255/511/2401-frame
clips, stale epochs and unapproved format changes. This validates input
preservation and host packet scheduling, not acoustic output from a headset.

The fixture also exercises the real SBC and Media Foundation AAC encoders at
44.1/48 kHz, plus LDAC at all four rates, with mono/stereo input and integer
16/16, 24/24, 24/32, 32/32 and float32 formats: **80 codec/rate/channel/PCM
combinations**. Exactly representable signed source values are normalized at
the encoder input boundary, so expected precision conversion is distinguished
from dropped or repeated frames. All 80 retain both clips' complete input;
RTP timing includes AAC fragmentation. These additional checks change only
the test fixture and documentation, not the deployed 0.4.2 production code.

The pacing distinction is also consistent with
[PipeWire's media sender](https://github.com/PipeWire/pipewire/blob/master/spa/plugins/bluez5/media-sink.c),
which limits queued-media flushing by represented sample time. That comparison
does not establish the cause of a particular receiver's audible behavior.

Exact-package activation and a final real short-clip acceptance remain separate
gates. Do not label the 0.4.2 candidate an accepted or stable fix until recorded.

## 2026-11-13: 0.4.2 failed acceptance; 0.4.3 timing correction

The owner reports that actual Shici playback is still defective on 0.4.2.
The previous offline successes do not establish a complete fix. In particular,
the scripted producer in the stream-loop fixture waits for admission itself;
it is not Windows Audio Engine or Shici's Media Foundation renderer.

A standalone, very low-amplitude 500 ms WAV diagnostic using the same Windows
Media Engine family found a 2,141 ms first Play-to-Ended interval, including
repeated stalls in the reported media clock. Immediate replay took 515 ms;
replay after a 4.5-second idle took 594 ms. This establishes an actual timing
anomaly. It does **not** measure which samples were audible at the headset.
No Shici application code or content was changed.

The source review found two timing-contract problems:

1. 0.4.2 acknowledged KS RUN before transport/encoder readiness and then froze
   the running WaveRT clock. The correction under review completes preparation
   within the state transition, without holding the position lock or running
   the timer; successful RUN then advances at the nominal rate. An explicit
   consumer-ready IOCTL wakes the transition. The wait has one five-second
   budget and wakes on mapping loss, replaced epoch and device stop. This is
   ring protocol version 4; mixed old/new packages are rejected.
2. PAUSE froze the presentation position immediately, even though buffered
   audio continued through the idle drain. The correction advances the paused
   presentation estimate only to the submitted-frame boundary. No new source
   PCM is consumed while paused, and STOP resets the timeline.

Consumer readiness follows allocation of the encoder and packet timer. A late
readiness reply for a replaced source epoch cannot tear down its successor.
Production-body checks cover delayed preparation, preparation failure,
timeout/removal wakeups and paused presentation drain. These remain software
checks, not renewed hardware acceptance.

The combined 0.4.3 candidate was subsequently deployed to the local test host.
Both loaded kernel images and the running service matched the same signed
candidate; activation required one Bluetooth radio cycle and no computer reboot.
Endpoint defaults, mix format, volume, mute and capture defaults were preserved.
The local deployment does not constitute a stable release or Shici acceptance.

A bounded trace then included Windows Audio and Media Foundation events, not
just the driver's input ring and Bluetooth sends. For three 500 ms diagnostic
clips, the Media Foundation render events started at source timestamp zero and
covered contiguous intervals to approximately 500 ms. All 18 render-position
computations reported zero bytes to stall. The trace recorded no native send
failures or ring overruns. These events do not establish byte identity through
Windows mixing/encoding or what was audible at the headset.

Startup latency was initially an unresolved observation: Media Foundation's first
`StartLocalGraph` interval took approximately 2,065 ms, immediate replay 0.37 ms,
and replay after idle 75 ms. The first interval ended immediately after native
encoder preparation. This localizes the wait to the startup path but does not
identify the slow call or prove that the wait loses source samples. Media clock
plateaus also remain observable; they must not alone be called sample loss.
The diagnostic process's COM cleanup order was corrected separately and its
last run exited successfully. No application code or further driver deployment
was performed during this investigation. Shici's reported audible defect remains
unaccepted; do not claim that every cause has been found or resolved.

A later trace explicitly required the native stream to be suspended before
starting the diagnostic and included Bluetooth HCI-boundary events. The initial
AVDTP START command reached that boundary at 338.838 ms; the matching ACCEPT
arrived at 1842.000 ms, a 1,503.162 ms interval. EncoderStart followed at
1842.220 ms, and the Windows graph-start call returned at 1842.780 ms. A later
START on the same connection was accepted in 60.25 ms. A separate trace with an
already-running encoder had no new START transaction and no long graph-start
wait. These are different initial states, not interchangeable test results.

This locates the dominant cold-start wait between Bluetooth command transmission
and reception of the peer's response, rather than after response processing in
the encoder. HCI-boundary timestamps cannot separate controller scheduling,
over-air link power state and headset firmware. In particular, a sniff-mode
explanation is still unproven. No link-policy tweak, arbitrary pre-roll, further
driver deployment or claim of Shici acceptance follows from this trace.

## 0.4.4: preparation cancellation, failed acceptance

Review of the new preparation wait found a separate cancellation defect. PnP
stop and mapping loss woke the waiter, but a user disconnect or HFP suspension
only notified the service. A pending RUN could therefore wait for its full
five-second budget after policy already forbade playback. A regression fixture
calling the actual `Oa2dpApplyPolicy` and `Oa2dpRingWaitConsumer` bodies reproduced
that timeout before the correction. This is not evidence of the cause of Shici's
reported missing beginnings.

Connection state and profile policy now update preparation availability under
the device-to-ring lock order. An unavailable endpoint wakes waiting RUN calls;
a cancellation serial prevents a rapid re-enable from erasing a cancellation
already issued to an existing waiter. Late RUN requests cannot reopen admission
while disconnected, suspended or stopped. A policy change does not retire an
otherwise live encoder: the service still owns actual encoder retirement, so a
coalesced suspend/resume does not strand its ready marker. Physical jack removal
also closes preparation admission, independently of a pending service callback.
Consumer-ready notification remains distinct from permission to RUN: an
otherwise valid notification during policy cancellation succeeds, while the
waiter still fails. This prevents a normal cancellation from being misreported
to the service as an encoder/transport failure requiring link teardown.

Production-body checks cover disconnect, HFP suspension, fast re-enable, a late
RUN, retained encoder readiness and a new wait after recovery. The exact 0.4.4
package was subsequently test-signed and deployed on 2026-11-14; both loaded
kernel images and the running service matched it. Activation used one Bluetooth
radio cycle, without a computer reboot or new certificate trust. The owner then
reported incomplete Windows notification sounds. This is failed acceptance,
not a stable correction.

## 0.4.5: prepare before the render deadline

A subsequent trace included the Windows Audio provider and real Shici process,
in addition to Media Foundation and native transport events. The initial KS RUN
call occupied approximately 109 ms. Immediately after it returned, the Windows
audio engine recorded a missed pump deadline and two buffer glitch reports at
stream byte positions 15,360 and 23,040. The native encoder recorded no ring
overruns or failed media completions in that trace. These are distinct stages:
clean native transport counters cannot rule out upstream engine data loss.
They do not identify the exact audible syllable, nor prove every reported
notification failure has the same cause.

The remaining design error was putting a remote Bluetooth transaction inside
RUN. Finishing that transaction before advancing the driver's own clock was
insufficient: Windows had already committed its render scheduling deadline.
The candidate prepares the consumer during the transition into PAUSE. RUN only
checks current availability and commits the clock; it cannot wait for START.
A production-body regression with a 900 ms handshake failed on the previous
implementation because PAUSE did not prepare anything. The revised path spends
that budget before RUN and leaves packet zero untouched until actual playback.

Prepared transport ownership is now separate from actual rendering. The service
retains the encoder and paced silence while Windows keeps the pin in PAUSE,
instead of retiring the consumer after three seconds and forcing the next RUN
to wait again. ACQUIRE/STOP, buffer release and stream destruction relinquish
the request. The existing tail-drain grace then applies. This trades radio work
while a client deliberately retains a paused pin for immediate, complete resume;
it does not keep transport running after the pin releases its resources.
HFP and disconnect still override availability and cancel pending preparation.
PAUSE can relinquish actual RUN during that cancellation without falsely
reporting playback. A retained request can be reconciled after profile recovery.

The ready check includes mapping and cancellation state. Buffer release resets
the source timeline, so reuse cannot inherit a stale DMA offset. Protocol 5 and
the expanded format-state reply reject mixed driver/service packages. Opt-in
state-request, preparation, state-commit and unavailable-RUN events cover the
previous observability gap without recording audio content.

The candidate builds locally and the 36-check suite passes, including actual
production state/ring/service bodies, codec input preservation, pacing, profile
cancellation, source formats and native metadata. These are source and host-side
checks. The exact candidate was subsequently deployed, with both loaded kernel
images and the service verified at 0.4.5. An automated capture covered two
standard Windows notification sounds and three 500 ms Media Engine diagnostic
clips, including replay after a 4.5-second pause. It recorded no Windows buffer
glitch or missed-pump-deadline events, no failed media sends, no ring overruns,
and no lost ETW events. The diagnostic graph starts took approximately
1.1/0.4/0.5 ms and source render intervals were contiguous from timestamp zero
through approximately 500 ms. This was a diagnostic process, not another real
Shici listening acceptance. Cold Bluetooth preparation still took about two
seconds before the first notification. No stable tag, main merge or
hardware-completeness claim follows.

## 0.4.6: require a playback buffer and honor allocation sizes

The activation trace also exposed 34 zero-source encoder instances during
Windows endpoint format discovery. This prompted a review of PAUSE preparation.
A production-body fixture independently reproduces unnecessary preparation for
PAUSE without a DMA allocation: 0.4.5 changes the source epoch and waits for
Bluetooth despite having no buffer. That fixture does **not** establish that
the 34 observed instances were bufferless; the deployed trace below establishes
that those real probes did allocate buffers.

Preparation now requires both PAUSE and an allocated audio buffer. The second
operation performs the same preparation helper, supporting either ordering and
both event-driven and polled allocation APIs. Bufferless probes do not change
the source epoch or request a codec; RUN still never waits for a remote START.
If preparation fails after allocation, the buffer is unmapped and freed before
the failed allocation returns. Tests execute the real allocation and state
bodies for both orderings, failure cleanup, reuse and immediate RUN.

The allocation review also found that both methods rounded buffer sizes down.
The WaveRT contract requires an actual size at least as large as the requested
size. Both now round up to full frames (and complete notification packets),
with overflow rejection. Tests exercise nonaligned sizes and maximum-size
overflow. Notification count is limited to the documented one or two per cycle.
These source corrections are included in the deployed package described below.
See Microsoft's [AllocateAudioBuffer contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavertstream-allocateaudiobuffer)
and [AllocateBufferWithNotification contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavertstreamnotification-allocatebufferwithnotification).

### Deployed result and remaining limits

Commit `71bf0725fe0c2f311b29d86940896c3c67f471c4` passed the complete local native
build and all 36 checks. Its exact [CI run](https://github.com/TruthNian/xm6-audio/actions/runs/43551801687)
also passed. On 2026-11-14 the package was signed with the existing local test
certificate and activated. Both loaded kernel images and the running service
were verified against that package at 0.4.6. Activation used one Bluetooth radio
cycle, no computer reboot, new certificate trust or boot-policy change. Endpoint
name, default outputs, capture defaults, mix format, volume and mute were retained.

A bounded three-minute trace covered endpoint activation, two standard Windows
notification sounds and three 500 ms Media Engine diagnostic clips, including
replay after a 4.5-second pause. Windows reported no buffer glitches or missed
pump deadlines; native events reported no unavailable RUN, failed media send,
ring overrun or ETW loss. The diagnostic graph-start spans were 0–1 ms at the
trace decoder's timing precision. Source render intervals started at timestamp
zero and were contiguous through approximately 500 ms on each clip.

Metadata accounting also matched: WaveRT pumped 731,656 frames across the three
source epochs, and the encoder consumed 731,656. Each first read began at its
epoch's first frame; all consumed source frames reached media submission, and
no unread ring tail remained when the encoder ended. This checks counts and
positions, not sample values, headset decoding or audible completeness. The
diagnostic process is not a repeat of real Shici listening acceptance.

The first cold preparation still occupied approximately 2,001 ms before RUN;
the following new-epoch preparations took approximately 1.4 and 2.4 ms. The
trace also retained 34 zero-source encoder instances during endpoint discovery.
Their state-request events contain nonzero buffer sizes: excluding bufferless
pins does not remove these allocated probes. A PAUSE with a buffer can be a
legitimate playback reservation, so treating it as disposable solely because
RUN has not yet happened would reintroduce the startup readiness problem.
No process-name exception or claim that this overhead is solved is made.

The deployed source fixes and these automatic checks do not close acoustic
acceptance. The user has not accepted 0.4.6 listening behavior, and no further
listening request was sent. No stable tag or main-branch merge follows.

The Windows contracts require a nominal packet clock and distinguish DMA
transfer position from downstream presentation position:
[SetWritePacket](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavertoutputstream-setwritepacket),
[SetState](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavertstream-setstate),
[GetOutputStreamPresentationPosition](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavertoutputstream-getoutputstreampresentationposition).
