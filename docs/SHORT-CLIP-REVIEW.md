# Short-clip pipeline review (0.4.1–0.4.3)

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
checks, not renewed hardware acceptance. This work has not been deployed.

The Windows contracts require a nominal packet clock and distinguish DMA
transfer position from downstream presentation position:
[SetWritePacket](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavertoutputstream-setwritepacket),
[SetState](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavertstream-setstate),
[GetOutputStreamPresentationPosition](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavertoutputstream-getoutputstreampresentationposition).
