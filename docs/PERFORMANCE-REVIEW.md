# Audio clock, PCM computation and output-lifetime review

Reviewed 2026-11-11 against source 0.3.54 and 0.3.59–0.3.60. The headset was unavailable.
Computational measurements below exclude Windows rendering, scheduling under
audio load, Bluetooth transmission and headphone playback.

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

The local native x64 Release build and all 28 checks pass. Exact-source CI
and runtime activation are recorded separately after verification.

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
