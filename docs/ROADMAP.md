# Roadmap and acceptance

The repository contains a buildable native prototype. The initial hardware target is WH-1000XM6 on Windows 10 x64. Updated 2026-11-11.

## Implemented and covered by build / focused checks

- KMDF A2DP transport and per-device WaveRT render endpoint.
- Capability negotiation with LDAC/AAC/SBC in the default build.
- Integer16/24/32, float32, valid-bit metadata and encoder-boundary conversion.
- LDAC HQ frame-size/rate checks at 44.1, 48, 88.2 and 96 kHz.
- Buffered connection events, persistent overlapped signalling, service/device cleanup and stale-PCM discard.
- INF-owned, device-triggered codec service with Driver Store payload and signed-catalog deployment gate.
- Ring producer rejects a stale sample rate under the format-change spin lock; endpoint format matching includes valid bits through the shared PCM comparator.
- MMCSS Audio scheduling, driver-to-service PCM data-ready events and cancellable stream waits; no Sleep(2) PCM polling.
- Media cancellation retains pending I/O ownership and uses one realtime drain budget; state-publication failures remain visible.
- Committed PCM epochs handle coalesced A -> B -> A formats. Opt-in ETW exposes PCM wait/buffering and media completion timing.
- Standalone inbox A2DP recovery, independent of native service registration and package integrity, with class-independent address-scoped selection.
- Public CMake/MSVC/WDK CI, pinned dependencies, license bundle and source/hash manifest.

## Required before native deployment is accepted

1. **A legitimate Microsoft-signed package.** The [free-signing inquiry](https://github.com/SignPath/fdn-website/issues/28) is open; no approval exists. Normal boot protection is retained.
2. **Real audio and controls.** Controlled local trials confirmed audible QQ Music through native LDAC HQ, PotPlayer video sound, manual play/pause/previous/next, wearing-detection pause/resume and bidirectional headset/Windows master volume. Sound was also confirmed after the 0.3.18 restart. Mute and automatic recovery for existing clients remain unaccepted. Packet counters or reopening a player do not satisfy this requirement.
3. **Format correctness.** Verify shared/exclusive streams, 32-bit integer/float and 24-in-32, supported sample-rate transitions and actual negotiated codec. The fixed LDAC 48 kHz/24-bit/HQ profile must produce its requested output or report failure.
4. **Lifecycle.** Version 0.3.51 is activated with verified actual images and service, without a computer restart. A settled DevicesFlowUI trace distinguished native49's false Connected from inbox's Disconnected; deployed50 and51 finally remained Disconnected with the headset unavailable. Version51 also fixes timeout classification, lost delay updates and silent RTP failure; all 21 local checks and source CI pass. Real per-device disconnect/reconnect, existing-client recovery, Chrome audio stability and sleep/resume remain unaccepted. See the [event-chain review](EVENT-CHAIN-REVIEW.md).
5. **Timing and driver behavior.** Check video presentation timing and run appropriate driver diagnostics on a controlled system.

## Future work, after the initial native path is validated

Improve measured latency and queue behavior, broaden tested headset/radio combinations, and evaluate additional Windows/architecture targets. LE Audio/LC3 requires a separate supported platform/transport path; an A2DP capability parser is not an LE Audio implementation.

The source-only 0.3.35 batch implements PCM data-ready notification, media-wait cancellation and opt-in timing probes; 0.3.36 removes the extra outgoing OPEN request and directly forwards caller cancellation. Version 0.3.37 repairs missed acknowledgement wakeups during policy reentry and selects the worker epoch under the publication lock; 0.3.38 also binds the suspend/resume cache to its original link. Production-body fake-API checks cover wakeup, cancellation and ownership ordering; they do not validate actual KMDF/Windows scheduling. Capture and compare real CPU use, buffering/overruns, wakeup and media-completion timing under load before making performance claims. Generic IOCTL/receive cancellation and final pending-I/O destruction still lack a universal hard deadline. The ring/epoch checks are not a concurrent hardware format-switch test.

Source-only 0.3.39 moves policy completion outside spin locks and reconciles cancel-safe waiter insertion with production, stop and control-handle cleanup. Kernel bus ABI 6 requires both drivers together. Source fixtures validate the selected ownership rules, not Windows CSQ internals or real reconnect behavior.

Source-only 0.3.40 unifies WaveRT position/count/notification progress, refreshes write-packet validation and locks buffer-mode/state checks. Delayed-timer, wrap, polled-mode and pause/resume fixtures do not replace actual Windows scheduling or audible playback tests.

Source-only 0.3.41 removes integer-PCM scratch conversion/copy, skips rejected-frame source access and makes overrun diagnostics atomic. Acceptance of CPU, lock-duration and latency gains still requires real measurements; source-memory/concurrency checks establish narrower contracts.

Source-only 0.3.42 fuses channel mapping and final quantization in the service, preserving mono averaging precision and float headroom and removing the separate float buffer. Pure numeric/integration regressions pass; actual Windows format negotiation, codec acoustics and performance remain acceptance work.

Source-only 0.3.43 retains failed suspend/resume publication and local-start intent, retrying from the existing main loop with the current policy serial/link. A remote START response no longer implies local admission succeeded. Production-body checks cover transient failure recovery, idle publication, cancellation, peer state and deadline wrap; they do not establish actual Windows reconnect or scheduling behavior.

Source-only 0.3.44 serializes driver-handle lifetime changes with stop-thread cancellation and preserves failed-open error codes across lock release. Real-thread/fake-handle-table regressions cover close/reuse/cancel ordering, opening publication and idempotent cleanup. Requests still require completion before their owner releases memory or closes the device; no universal shutdown deadline or actual Windows recovery behavior is established by these fixtures.

The 0.3.44 batch was subsequently deployed with explicit authorization; see the updated hardware record. Source-only 0.3.45 makes initial automatic codec selection prefer current endpoint PCM and preserves supported current formats in proposals. This is not per-file shared-mix adaptation and does not establish a Chrome/UI fix or measured performance improvement.

Source-only 0.3.46 fixes multi-frame RTP timestamp accounting. The actual service block and actual LDAC adapter reproduced a half-duration timestamp step at 96 kHz; regression checks cover all four supported LDAC rates, 16/24-bit input and MTUs 679/895/1024, plus portable batching, wrap and single-frame/fragment paths. This establishes packet-clock correctness for those paths, not actual headset jitter/stutter recovery.

Source-only 0.3.47 reconciles accepted peer START after endpoint readiness and prevents new local admission after stop. Production setup/admission fixtures cover reversed notification order, duplicate readiness, failed setup/publication and policy/state gates. Real Windows reconnect/UI and existing-client recovery remain unaccepted.

The 0.3.48 source review fixed a reproduced missed rate transition after peer configuration/START: unchanged running PCM is reconciled after capabilities and media readiness, including peer RECONFIGURE. Production service procedures and two real AVDTP cores cover supported rate pairs, policy/state changes and command delivery during capability discovery. Audio IOCTLs/encoder admission remain controlled fixtures; subsequent 0.3.49 activation does not resolve the UI/Chrome acceptance gaps.

Version 0.3.49 retains failed endpoint format/latency/connected publication for rate-limited retry using current state. Production procedures reproduce the old lost request and cover all three failure stages before/after rate changes, repeated failures, deadline wrap and policy/peer changes during backoff. It is now activated on the developer host, without new audible or lifecycle acceptance and without measured optimal performance.

The installation script now handles a reproduced up-to-date PnPUtil result 259 only after checking the present render device's exact selected INF and state. The real unchanged-binding check and fake failure/restart cases pass. Other commands and installation errors are not reclassified as success.

The inherited [SPEC.md](SPEC.md) is a protocol requirement catalogue, not proof of current hardware compatibility. The status table in the README and this acceptance list describe the current fork.
