# Roadmap and acceptance

The repository contains a buildable native prototype. The initial hardware target is WH-1000XM6 on Windows 10 x64. Updated 2026-11-09.

## Implemented and covered by build / focused checks

- KMDF A2DP transport and per-device WaveRT render endpoint.
- Capability negotiation with LDAC/AAC/SBC in the default build.
- Integer16/24/32, float32, valid-bit metadata and encoder-boundary conversion.
- LDAC HQ frame-size/rate checks at 44.1, 48, 88.2 and 96 kHz.
- Buffered connection events, persistent overlapped signalling, service/device cleanup and stale-PCM discard.
- INF-owned, device-triggered codec service with Driver Store payload and signed-catalog deployment gate.
- Ring producer rejects a stale sample rate under the format-change spin lock; endpoint format matching includes valid bits through the shared PCM comparator.
- MMCSS Audio scheduling with thread-scoped cleanup. Ring consumption still polls; this is not a measured jitter guarantee.
- Standalone inbox A2DP recovery, independent of native service registration and package integrity, with class-independent address-scoped selection.
- Public CMake/MSVC/WDK CI, pinned dependencies, license bundle and source/hash manifest.

## Required before native deployment is accepted

1. **A legitimate Microsoft-signed package.** The [free-signing inquiry](https://github.com/SignPath/fdn-website/issues/28) is open; no approval exists. Normal boot protection is retained.
2. **Real audio and controls.** Controlled local trials confirmed audible QQ Music through native LDAC HQ, manual play/pause/previous/next, wearing-detection pause/resume and subsequently bidirectional headset/Windows master volume. PotPlayer video sound was restored once. Post-restart playback, mute and automatic recovery for existing clients remain unaccepted. Packet counters or reopening a player do not satisfy this requirement.
3. **Format correctness.** Verify shared/exclusive streams, 32-bit integer/float and 24-in-32, supported sample-rate transitions and actual negotiated codec. The fixed LDAC 48 kHz/24-bit/HQ profile must produce its requested output or report failure.
4. **Lifecycle.** After the authorized restart, both loaded 0.3.18 driver hashes matched the local signed candidate and the codec service started with the native device. This establishes module activation and service startup only. Verify post-startup sound, installation/recovery, headset off/on, Windows per-device disconnect/reconnect, existing-client recovery, service restart and sleep/resume without stale audio or silent-success status.
5. **Timing and driver behavior.** Check video presentation timing and run appropriate driver diagnostics on a controlled system.

## Future work, after the initial native path is validated

Improve measured latency and queue behavior, broaden tested headset/radio combinations, and evaluate additional Windows/architecture targets. LE Audio/LC3 requires a separate supported platform/transport path; an A2DP capability parser is not an LE Audio implementation.

Replace PCM polling with driver-to-service data-ready notification only after validating wakeup, cancellation and teardown ordering. Use ETW to correlate ring occupancy/overruns, wakeup delay and L2CAP completion latency under load. Neither event-driven consumption nor this timing evidence is implemented yet. The ring's stale-rate guard and portable format checks do not constitute a concurrent hardware format-switch test.

The inherited [SPEC.md](SPEC.md) is a protocol requirement catalogue, not proof of current hardware compatibility. The status table in the README and this acceptance list describe the current fork.
