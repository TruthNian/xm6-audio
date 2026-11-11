# Endpoint name and identity review

Reviewed 2026-11-11. Installed baseline: 0.3.63; source candidate: 0.3.64.

## Two reproduced name-publication defects

The render INF unconditionally writes `FriendlyName` in both wave and topology
interface sections. Executing those exact sections through Windows SetupAPI in
an isolated HKCU registry key overwrote `WH-1000XM6` with `XM6 Audio Wave`.
The corresponding topology section has the same overwrite policy. This happens
during installation, outside the driver's later attempt to restore metadata.
The INF now uses `FLG_ADDREG_NOCLOBBER`: clean installations retain a fallback,
while updates preserve an existing headset-specific interface name. The test
executes both production sections for a clean key and two existing names,
without installing a device or writing a machine audio property.

First installation has a separate ordering defect. There is no persisted
`Oa2dpName`, but the old `StartDevice` still registered its topology and wave
filters. The service could provide the Bluetooth name only after their control
interface became available. Thus the initial endpoint was exposed with the
INF placeholder. A fixture executing the old production `StartDevice` body
failed the assertion that an unnamed initial device must not expose filters.

The new startup exposes the control interface and initialized PCM ring while
waiting for the first name. `SET_NAME` publishes metadata and creates filters
only if they do not exist, under the existing filter lock. Cached-name startup
still creates the endpoint immediately, including when the headset is offline.
Initial metadata publication failures prevent placeholder registration and
reach the service for retry. Stopped/removed devices reject deferred creation.
Later name updates never unregister working filters. The service now checks
`SET_NAME` instead of silently ignoring its failure; an unavailable Bluetooth
name uses a truthful generic fallback, not a hard-coded model name.

The focused checks execute the production publication, startup, deferred
creation and service-admission bodies with controlled OS boundaries. They cover
cached startup, first creation, failed creation/retry, stop/remove admission,
unchanged-name retries, property failure isolation, Unicode and invalid names.
They do not simulate AudioEndpointBuilder's cache or establish current UI
acceptance. The complete 0.3.64 native Release build and all 31 local checks
passed. Live name verification remains separate from these reproductions.

Microsoft requires interface properties to be prepared before PortCls exposes
the interfaces; see [setting properties and registry values](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/setting-properties-and-registry-values).

## Ordinary endpoint IDs are not guaranteed upgrade identities

Private before/after snapshots for 0.3.62 -> 0.3.63 show that all five KS interface
paths, the underlying render device instance and its container ID are unchanged.
The ordinary MMDevice ID changed; all three default render roles still select
native XM6, and mix, volume, mute and capture defaults were preserved.

The earlier report treated MMDevice ID equality as an unconditional upgrade
requirement. That interpretation was too strong. Microsoft explicitly documents
that an operating-system or audio-driver update can change the ordinary ID from
`IMMDevice::GetId`. The newer `PKEY_AudioEndpoint_StableId` is optional and requires
Windows 11 24H2/Server 2025, so it is not a solution on this Windows 10 host.
See [stable endpoint identity](https://learn.microsoft.com/en-us/windows/win32/coreaudio/pkey-audioendpoint-stableid).

The observed ID change is real, but alone does not establish a driver defect.
Windows' internal migration decision has not been independently reconstructed.
The driver/service use the headset's persistent PnP/KS identity, not a stored
MMDevice GUID. Do not manufacture a GUID, overwrite private endpoint caches,
or remove/recreate a working endpoint to satisfy an unsupported assertion.
Upgrade checks must distinguish underlying identity, default/settings migration,
and actual application recovery. Third-party applications that retain an old
ordinary ID must handle normal endpoint notifications; driver source cannot
prove their recovery without evidence from those clients.

## Remaining scope

This change addresses the two reproduced name-publication defects. It does not
claim all bugs are eliminated, audible fidelity has improved, or every Windows
cache/lifecycle path has been accepted. The earlier unexplained PCM supply gap,
hardware format transitions, sleep/resume and Microsoft production signing
retain their separate evidence and status.
