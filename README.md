# XM6 Audio

**Native Windows Bluetooth audio, with capability-aware codec negotiation and a standard headphone endpoint.**

[![Native build](https://github.com/TruthNian/xm6-audio/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/TruthNian/xm6-audio/actions/workflows/ci.yml)
[![Core license: MIT](https://img.shields.io/badge/core_license-MIT-blue.svg)](LICENSE)
[![Status: prototype](https://img.shields.io/badge/status-prototype-yellow.svg)](docs/ROADMAP.md)

English · [简体中文](README.zh-CN.md)

XM6 Audio adds an A2DP profile driver and a WaveRT render endpoint while Windows continues to own the Bluetooth radio, pairing, volume UI and AVRCP media-control stack. The WH-1000XM6 is the initial hardware acceptance target.

> **Development status:** the native drivers, service and codec checks build in CI. Microsoft kernel signing and native headset acceptance are still pending. CI artifacts are unsigned development candidates, not installable releases for a normally protected Windows system.

The [controlled developer trial](docs/HARDWARE-BRINGUP.md) established native QQ Music/PotPlayer playback, media controls, wearing detection and bidirectional master volume. The last verified runtime is 0.3.32. **Its Windows connection labels and per-device disconnect/connect still fail acceptance.**

Source-only 0.3.36 also forwards the original outgoing connection request to the Bluetooth stack, removing an extra request allocation and letting caller cancellation reach the lower OPEN. Earlier fixes include event-driven PCM consumption, cancellable media waits, format-epoch tracking and opt-in timing probes. It has not been installed or hardware-accepted, and no measured performance gain is claimed. See the [lifecycle review](docs/LIFECYCLE-REVIEW.md). **Do not load candidate 0.3.5**; its descriptor ABI error caused a developer-machine bugcheck and is corrected in 0.3.6.

**[Architecture](docs/ARCHITECTURE.md) · [Build from source](BUILDING.md) · [Signing status](docs/SIGNING.md) · [Roadmap](docs/ROADMAP.md)**

## What the project implements

- A **standard Windows headphone endpoint** for applications using Windows Audio/WASAPI.
- **LDAC, AAC and SBC** negotiation from the intersection of local encoders, headset capabilities and per-device preferences.
- **Integer 16/24/32-bit and float32 PCM**, with container width and valid precision tracked separately, including 24-in-32.
- Sample-rate following for formats the peer supports, with explicit fixed-format profiles available for diagnostics.
- Device-interface notifications, buffered Bluetooth connection events, disconnected endpoint state and cleanup of stale PCM during reconnection.
- A codec service **installed by the driver INF and started by a Windows device-interface trigger**, with binaries loaded from Driver Store.

Native playback, manual play/pause/previous/next, wearing-detection pause/resume and bidirectional headset/Windows master volume have been confirmed in the controlled developer trial. Per-device disconnect and existing-client recovery remain unaccepted. See the hardware record for acceptance limits.

## Audio path

```mermaid
flowchart LR
    Apps["Music, video and system audio"] --> WinAudio["Windows Audio / WASAPI"]
    WinAudio --> Endpoint["WaveRT headphone endpoint"]
    Endpoint --> Codec["Codec service<br/>LDAC · AAC · SBC"]
    Codec --> Profile["KMDF A2DP profile"]
    Profile --> WindowsBT["Windows BTHPORT / BTHUSB"]
    WindowsBT --> Headset["Paired headphones"]
    Headset <--> AVRCP["Windows AVRCP"]
    AVRCP <--> Media["System media controls"]
```

The profile binds to the selected headset's A2DP Audio Sink service. Radio ownership and other Bluetooth profiles remain with Windows. The audio path uses the native endpoint's PCM buffer; it does not capture a player's output or loop back a different system endpoint.

See [component boundaries and lifecycle](docs/ARCHITECTURE.md).

## Format and quality policy

| Layer | Policy |
|---|---|
| Source files | The player/Windows decodes MP3, FLAC, ALAC, AAC, Opus, WAV and other formats it supports into PCM. File decoders do not belong in the Bluetooth driver. |
| Endpoint PCM | Integer 16/24/32 and float32; valid precision is distinct from container width. |
| Sample rate | Follow the actual Windows stream where supported. Shared-mode audio exposes the Windows mix format, not each file's original metadata. |
| LDAC quality | HQ by default: nominal 909 kbps at 44.1/88.2 kHz and 990 kbps at 48/96 kHz. ABR is off by default. |
| Precision boundary | Keep full integer32 PCM until conversion to the encoder's supported input precision. LDAC accepts at most 24 useful bits; a 32-bit container is not a claim of 32-bit Bluetooth transmission. |
| Negotiation | Prefer LDAC, then AAC/SBC. Report the actual codec and format. An explicitly fixed LDAC profile must not report a fallback as success. |

The XM6 supports SBC/AAC/LDAC over Classic Audio and LC3 over LE Audio. LC3 is not part of this Windows 10 A2DP implementation. Sources: [Sony codec support](https://helpguide.sony.net/mdr/2984/v1/en/contents/TP1001856790.html), [Sony libldac API](https://android.googlesource.com/platform/external/libldac/+/eeee1a3f5f8df1282e3a6d297085885fd886737b/inc/ldacBT.h), [Windows device formats](https://learn.microsoft.com/en-us/windows/win32/coreaudio/device-formats).

## Evidence and current limits

| Area | Evidence / status |
|---|---|
| Portable protocol and PCM policy | CMake checks, including integer/float distinction, valid bits, 24-in-32 and precision conversion; sanitizer CI on Linux. |
| Encoders | Actual SBC, Media Foundation AAC and pinned libldac encoding checks on Windows. LDAC checks cover 44.1/16, 48/24, 88.2/24 and 96/24. |
| Native binaries | MSVC/WDK build of both kernel drivers, codec service and command-line tool. |
| Packaging | INF validation, catalog generation, pinned dependency commits, symbols, licenses and SHA-256 manifest. |
| Microsoft kernel signing | **Pending.** [Public feasibility inquiry](https://github.com/SignPath/fdn-website/issues/28); no provider approval or production signature obtained. |
| Native headset acceptance | **Partial, one test-signed system.** Earlier trials confirmed audible QQ Music/PotPlayer, media controls, wearing detection and bidirectional master volume. Version 0.3.32 activated without rebooting; both actual kernel images and the codec process match the verified package, and child identity readback succeeds. The headset is currently disconnected. Fresh sound/control/GUI acceptance, existing-client recovery, format transitions, sleep/resume and video timing remain pending. |

Build success, encoder bitrate and packet counters are not evidence of audible headset playback. The target baseline is Windows 10 2004+ x64; broader runtime compatibility also requires hardware validation.

## Build and review

```powershell
git clone --recurse-submodules https://github.com/TruthNian/xm6-audio.git
cd xm6-audio
cmake -S . -B build/native -G "Visual Studio 17 2022" -A x64 -DOA2DP_BUILD_DRIVERS=ON
cmake --build build/native --config Release --parallel 2
ctest --test-dir build/native -C Release --output-on-failure
```

The full build needs Visual Studio C++ tools, Windows SDK and WDK. [BUILDING.md](BUILDING.md) also covers user-mode-only and portable builds. The public [CI workflow](.github/workflows/ci.yml) produces an **unsigned** review candidate; deployment requires the signed-package checks in [SIGNING.md](docs/SIGNING.md).

## Provenance, licensing and signing

The native implementation is a documented derivative of [RafiMota/open-a2dp](https://github.com/RafiMota/open-a2dp), pinned at `167ebc0e2c89fec570eec535fefdade72bd9e416`. The initial imported tree is byte-for-byte traceable to that revision; subsequent changes are in this repository's history. See [PROVENANCE.md](docs/PROVENANCE.md).

Core code retains the **MIT** license. WaveRT-derived files retain Microsoft's **MS-PL** notices; libldac is **Apache-2.0**, libsbc is **LGPL-2.1-or-later**, and FindWDK is **BSD-3-Clause**. Component licenses and build pins are documented in [THIRD_PARTY.md](THIRD_PARTY.md) and included in packages.

**Code signing policy:** no signing provider is approved yet. Any future signing request must correspond to a reviewed source revision and public CI artifact, with explicit release approval. See [signing policy and status](docs/SIGNING.md) and [privacy policy](docs/PRIVACY.md).

Maintainer: [@TruthNian](https://github.com/TruthNian). Contributions are welcome through [CONTRIBUTING.md](CONTRIBUTING.md) and the [clean-room rules](docs/CLEAN_ROOM.md). This project is independent of Sony, Microsoft and Luculent Systems.

The earlier experimental radio-takeover bridge is preserved under [`v0.1.0-bridge`](https://github.com/TruthNian/xm6-audio/tree/v0.1.0-bridge) for provenance. It is not the native implementation on the default branch.
