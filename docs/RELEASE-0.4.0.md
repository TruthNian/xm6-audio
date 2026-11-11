# XM6 Audio 0.4.0

This version unifies the native transport driver, WaveRT endpoint driver and
codec service. On 2026-11-12, the project owner designated this exact package
as the stable baseline for the validated WH-1000XM6 / Windows 10 x64 host
configuration and paused feature expansion and additional device/OS adaptation.
This changes release status only: source tag, binary artifacts, hashes and the
installed package remain unchanged. Public packages remain unsigned; the host
retains its existing local test signature. Normal protected Windows installation
still requires Microsoft production signing. Stability classification does not
extend the recorded hardware qualification or assert that unknown bugs cannot exist.

## Change

Installing an already-selected package could leave Windows displaying
`Headphones (XM6 Audio Topology)` despite correct driver interface names and a
computer reboot. The installer now performs a selected-device installation
with the verified render INF, so Windows rebuilds its endpoint metadata and
migrates settings. Both INF placeholder names are removed: NOCLOBBER protected
existing values but still seeded a wrong name when an upgrade created a fresh
interface key. The driver already waits for the real name before exposing its
filters, so the static fallback served no purpose. Name publication remains
ordered before filter registration.
The 0.3.65 concurrent stop/rate-selection fix and actual encoder-input reporting
are included in the same version as both drivers.

Both catalog-covered INFs identify the exact source commit. This prevents an
older candidate with identical version numbers and INF text from being silently
reused by Driver Store while a newer candidate is being validated. Deployment
still verifies the actual payload hashes; a version string alone is insufficient.
The transport selector also restricts its compatible driver list to the INF
beside the verified packaged tool. Selecting by provider, date and version
alone could choose an older 0.4.0 candidate on a tie. Microsoft inbox recovery
clears that restriction and remains independent of the native package.

## Acceptance scope

The final package from source `c3287bf56cbe989c7f2f51bfa4b736d9c5a47b28`
is installed on the Windows 10 development host. Both actual kernel images and
the running service match the same 0.4.0.0 package. After upgrade and a Bluetooth
radio cycle, Core Audio, PnP and the underlying public interface name all identify
WH-1000XM6. Default roles, mix format, volume and mute were preserved. A short
controlled signal reached the endpoint and LDAC transport. The native build,
32 local tests, installation/recovery fixtures and exact-source Linux/Windows CI
passed. See the [hardware record](HARDWARE-BRINGUP.md) for evidence and scope.
No computer restart, new certificate trust or boot-policy change was performed.

The historical 0.3.61 Settings connection-state acceptance remains valid as
historical evidence. This naming change does not establish new listening,
sleep/resume, every format transition or every radio/headset acceptance.
Shared-mode PCM still reflects Windows' mixed endpoint stream, not original
file metadata; per-file automatic shared-mix selection is not implemented.
The two older audio interruptions are closed for investigation at the user's
request, without an invented diagnosis or claim of resolution.
