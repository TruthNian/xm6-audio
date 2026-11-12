# Short-clip startup correction (0.4.1)

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

The patch requires both the endpoint driver and service. Source checks do not
replace exact-package deployment and real short-clip acceptance. Those results
will be recorded separately; this report does not claim the hearing result.
