# Development rules

The goal is audible, stable system playback through WH-1000XM6 at fixed LDAC 990 kbps, 48 kHz, 24 bit, with ordinary Windows startup, volume and connection behavior.

Complete headphone behavior is mandatory: Play, Pause, Next, Previous, system-wide audio and Windows volume. Validate transport events, Windows dispatch and player response as separate boundaries. Never call a userspace capture bridge a native Windows Bluetooth render endpoint.

The target keeps Windows in control of Bluetooth and exposes a real, independently controllable XM6 audio endpoint. A Bluetooth-off radio or missing XM6 volume endpoint is an unmet requirement. Preserve WinUSB only as an explicitly labeled transport experiment; adding tray integration or autostart does not meet the native-device requirement.

For every change: question the requirement, delete unnecessary steps, simplify the remaining path, then optimize the measured bottleneck. Prefer the existing core lifecycle to new wrappers, launchers, watchers, services or dependencies. A setup script may install or configure the product; it must not become a runtime dependency.

State the trigger, observed failure, necessary mechanism, and acceptance criterion before adding a mechanism. Keep changes reversible and preserve a known-audible baseline. Bitrate counters and Windows meters alone do not establish audible success. Never silently substitute a lower codec or rate and label it 990 kbps.

Run focused checks for changed failure paths. Do not add broad benchmarks or repeated listening tests without a concrete unresolved question. Do not claim untested reboot, resume, video synchronization, radio-interference or hardware compatibility cases as passed.

Keep Bluetooth addresses, pairing/link keys, Windows user identifiers, local paths, firmware dumps, license material and machine-specific configuration out of git. Keep all upstream attribution and third-party licenses. The bundled BTstack dependency has its own noncommercial-use terms; this repository's MIT/Apache source licenses do not supersede them.
