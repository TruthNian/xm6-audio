# Windows integration: distinguish an audio transport from a device

The user's requirement is ordinary Windows headphone behavior: system-wide audio, headphone Play/Pause/Next/Previous, volume keys, startup and reconnection. Audible LDAC output establishes the transport only. It is not the full acceptance criterion.

## What Alternative A2DP teaches us

The installed vendor INF binds a MEDIA-class kernel driver to the Bluetooth A2DP service (`BTHENUM` service UUID `0000110b-0000-1000-8000-00805f9b34fb`), registers WDM/KS audio and render interfaces and installs its own audio topology. It does not replace the USB radio with WinUSB. Windows continues to own the radio and the other Bluetooth profile machinery. This is why that architecture can preserve standard Windows device behavior. The [vendor's installation instructions](https://www.bluetoothgoodies.com/a2dp/) describe switching the audio device driver.

The available vendor binary is not open implementation source. Its INF and observable behavior inform the integration boundary; no vendor binary, license, activation data or proprietary code is copied into this repository.

## What the current source changes do

- The existing native core restores its saved profile and owns initial retry and normal reconnect behavior. No new runtime supervisor or launcher.
- System capture uses Windows process loopback to exclude the core's own process tree and include the other Windows application sessions. This avoids restricting capture to QQ Music or to the default render endpoint. On a Windows build that rejects process loopback, a logged fallback uses the existing endpoint loopback; it does not silently lower the codec or bitrate.
- Classic AVRCP commands go first to the current Windows media session, using the same Windows.Media.Control facilities already present for LE media control. A legacy keyboard-media-key fallback remains only when no session accepts the command. An accepted toggle is never also injected as a keyboard toggle.
- The existing volume/mute synchronization handles ordinary mode and preserves speaker mute across volume changes, including an output already muted before startup.

## What is still a distinct integration gap

WinUSB gives this core ownership of the Bluetooth controller. It therefore does not provide a normal Windows Bluetooth audio render endpoint, and Windows Bluetooth settings do not own the connection. Capturing all applications does not, by itself, create such an endpoint or provide audio/video latency compensation to every player.

A true standard render-device path requires an actual endpoint implementation and accurate lifecycle/latency behavior. Microsoft's [SysVAD sample](https://github.com/Microsoft/Windows-driver-samples/tree/main/audio/sysvad) provides an open audio-driver starting point. A locally loadable driver must satisfy Windows' [driver-signing policy](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/driver-signing). Disabling platform protections is not part of this implementation. These facts determine engineering work; they do not justify describing the current user-mode transport as a completed native device.

Before adding a driver or another audio component, prove the gap in a concrete scenario, check whether an existing properly signed endpoint can meet it, and avoid redundant capture and encoding paths. Video synchronization, protected audio, explicit alternate endpoints and recovery from process crashes each need explicit acceptance evidence.
