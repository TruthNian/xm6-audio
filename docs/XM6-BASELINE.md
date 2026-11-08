# XM6 integration baseline

## Objective

Ordinary Windows playback through WH-1000XM6: 48 kHz, stereo, 24-bit input to LDAC, fixed 990 kbps, ABR off. Startup, connection recovery and volume control belong to the existing BlueCaster core. No QQ-specific executable, runtime PowerShell loop, extra watchdog or terminal window.

## Evidence and scope

The initial baseline is upstream `v3.0.0-beta.3`, commit `e4d254d4a867b84071b91c9478846087d58c9428`, on Windows 10 build 19045 with an Intel 8087:0AAA adapter and WH-1000XM6. The user confirmed audible, stable QQ Music using process capture. Encoder negotiation reported 48 kHz / 24 bit / LDAC 990 kbps with ABR disabled. Short observed intervals recorded no send errors or media drops. This establishes feasibility, not universal stability.

THX endpoint loopback previously produced silence despite active application meters and packet counters. Its cause has not been established. It is not the default integration source.

## Bridge changes

1. Restore the saved active profile from the resident core's existing sampler. Retry initial failures with bounded backoff while the device is unavailable; hand a successful stream to existing reconnect behavior. Respect Disconnect, a changed/removed profile and disabled auto-reconnect.
2. Honor `output_mute` in ordinary mode, without requiring developer mode or the developer volume mixer. Keep the existing mute-key watcher attached when the output was already muted at startup, while preserving that original mute state on exit. This prevents a Windows volume key from exposing speakers in a session that started with the PC muted. Existing output-mute, mute-key and volume-sync code remains responsible for interaction with Windows.
3. Configure system loopback with a real 48 kHz render endpoint, a fixed LDAC profile and the core's own Windows startup registration. Setup is one-time; the installed core is the only runtime.
4. Capture all other Windows application sessions through process-loopback exclusion of the core, rather than only the default endpoint, where supported. Route classic AVRCP through the current Windows media session, with a legacy key fallback only if no session accepts the command.
5. Preserve the complete AVRCP pass-through operation octet in replies, including the release bit. The bundled BTstack target masked the bit off when replying, so a controller could fail to complete a key transaction. Keep the operation event's semantic ID separate from the echoed wire octet. Windows Play/Pause uses actual playback state and explicit Play/Pause requests when known.

After changes 4 and 5 were deployed, the user confirmed music was normal and then confirmed Play/Pause/Next/Previous had all recovered; volume was already working. Log evidence includes both pressed/released operation events and Windows media-session acceptance. This is bridge acceptance only: Windows Bluetooth ownership and an independent XM6 render endpoint are still not provided.

## Acceptance status

| Case | Evidence required |
|---|---|
| QQ application capture at fixed 48 kHz / LDAC 990 | User confirmed audible and stable on the baseline |
| Whole-system capture | Audible shared-mode audio through the native system source; not inferred from packets |
| Native startup restore | Restart core, inspect profile/codec state, confirm audible playback |
| Headphones off at sign-in | Initial failures retry, later power-on connects |
| User Disconnect | No unwanted reconnect caused by startup recovery |
| Sleep/resume, cold boot | Hardware verification pending until actually performed |
| Video synchronization | Measure/observe separately; do not equate high bitrate with low delay |

## Constraints that remain real

Intel cold-start firmware is part of the path, not a startup-script problem. This controller answered with legacy variant `0x13`, hardware revision `0`, firmware revision `0`. The loader deliberately re-reads legacy fields for variants `0x11..=0x14`; its cold-start stem is derived from those fields. The tested warm session already had operational firmware in RAM. Local preparation uses the official linux-firmware common `ibt-19-0-4.sfi` / `.ddc`, also under the observed `ibt-19-0-0` names. Intel's [upstream alias change](https://kernel.googlesource.com/pub/scm/linux/kernel/git/firmware/linux-firmware/+/949dba8936abfc1d17ff6f9a69b96488b0cd4c4a) documents that these variants share that image. SHA-256 of that pinned image: `74c7df0302a4cb0917411b959771b9148e87e4518e37215daddab57b397c99e1`; DDC: `fe272982577efdc289cfe3e8bedafca607c0b7d6c9df1aab1880e22ef930d077`. Keep the Intel firmware license beside downloaded files. Preparing files does not establish a passed cold-boot test.

Fixed 990 kbps requires sufficient usable radio airtime. No software can guarantee it at arbitrary distance or interference. DRM-protected or exclusive hardware paths may not be available to WASAPI loopback. Application-specific output selection can bypass the default system endpoint. Video has sink buffering and transport delay; universal automatic lip-sync is not established by the audible music result.

This implementation uses WinUSB and a userspace Bluetooth stack. Windows does not own that radio while it is assigned. It does not install a signed native Windows A2DP codec driver or create a Windows headphone render endpoint. Those would be distinct integration work with driver/signing and latency-reporting requirements.

## Improvement order

Question every requirement and proposed mechanism. Delete the QQ-only capture dependency and runtime launcher. Reuse the native core. Stabilize whole-system capture and startup first. Then measure video delay, sleep/resume and cold-start firmware behavior. Add a driver, virtual endpoint or extra process only after demonstrating a gap that cannot be solved in the current path.
