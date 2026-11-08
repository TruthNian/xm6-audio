# Native Windows A2DP direction

This is a source/developer preview, not an installed or hardware-validated driver.

The target path is:

Windows XM6 render endpoint -> LDAC encoder -> Windows BTHPORT/L2CAP -> XM6

Windows keeps the Bluetooth radio, pairing and other Bluetooth profiles. A standard named XM6 audio endpoint exposes Windows volume and lifecycle state. System audio enters that endpoint directly, instead of being copied from unrelated output devices. Native AVRCP/HFP should be reused rather than rebuilt in an adapter-owning userspace stack.

## Pinned prototype

`open-a2dp` is the MIT-licensed [RafiMota/open-a2dp](https://github.com/RafiMota/open-a2dp) at `167ebc0e2c89fec570eec535fefdade72bd9e416`. The audio-driver sample also preserves its separate MS-PL notice. It contains a BTHENUM A2DP profile driver, a WDM/WaveRT render driver, a shared PCM ring and an encoder service. It is an upstream prototype, not original code claimed by this repository.

The README's old 'not WDK-built' wording is superseded by its successful [CI run 38632901913](https://github.com/RafiMota/open-a2dp/actions/runs/38632901913): both `oa2dp.sys` and `oa2dpaudio.sys` built, and Inf2Cat packaging succeeded. User-mode service/tool artifacts also exist. This establishes buildability only. Its own detailed driver documentation still says not loaded/tested on real hardware.

The downloaded driver binaries and catalogs were inspected locally: all four have Authenticode status `NotSigned`. Nothing from this preview was installed, no certificate was trusted and no boot-security setting was changed. A valid normal-policy kernel deployment path is required before host installation. Do not run the upstream `install-dev.ps1` on the user's host as part of an ordinary install.

## Acceptance gates

1. Windows continues reporting/managing Bluetooth normally.
2. XM6 has a real independently controllable render/volume endpoint.
3. All intended music/video applications can use it through standard audio APIs.
4. Headphone controls and Windows volume work through the native control boundary.
5. Actual audible output is fixed LDAC 990 kbps, 48 kHz, stereo, 24 bit, ABR off; no silent codec downgrade.
6. Audio-device lifecycle, reconnect, sign-in and sleep/resume work without a manual launcher.
7. Headset delay is propagated into endpoint latency before video synchronization is claimed.

No item above is marked passed for this prototype. The bridge's audible/controls result does not pass native-driver gates.
