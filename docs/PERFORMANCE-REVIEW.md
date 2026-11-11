# PCM computation and output-lifetime review

Reviewed 2026-11-11 against source 0.3.54. The headset was unavailable.
Computational measurements below exclude Windows rendering, scheduling under
audio load, Bluetooth transmission and headphone playback.

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
