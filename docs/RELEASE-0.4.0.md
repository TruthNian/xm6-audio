# XM6 Audio 0.4.0

This version unifies the native transport driver, WaveRT endpoint driver and
codec service. It is a developer release with a controlled, locally test-signed
deployment. Public CI packages remain unsigned; Microsoft production signing
and broad hardware qualification are not implied by the version number.

## Change

Installing an already-selected package could leave Windows displaying
`Headphones (XM6 Audio Topology)` despite correct driver interface names and a
computer reboot. The installer now performs a selected-device installation
with the verified render INF, so Windows rebuilds its endpoint metadata and
migrates settings. The existing name-publication ordering/NOCLOBBER fixes remain.
The 0.3.65 concurrent stop/rate-selection fix and actual encoder-input reporting
are included in the same version as both drivers.

## Acceptance scope

The legacy name was corrected on the Windows 10 development host before the
version bump. Core Audio and PnP both returned the headset name; defaults,
format, volume and mute migrated, and an existing player resumed producing
signal. Driver unload/reload retained the correct name. Exact 0.4.0 package and
loaded-image verification belongs in the [hardware record](HARDWARE-BRINGUP.md).

The historical 0.3.61 Settings connection-state acceptance remains valid as
historical evidence. This naming change does not establish new listening,
sleep/resume, every format transition or every radio/headset acceptance.
Shared-mode PCM still reflects Windows' mixed endpoint stream, not original
file metadata; per-file automatic shared-mix selection is not implemented.
The two older audio interruptions are closed for investigation at the user's
request, without an invented diagnosis or claim of resolution.
