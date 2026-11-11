# Endpoint name and identity review

Reviewed 2026-11-11. Naming changes activated in 0.3.64.

## Live outcome and remaining migration problem

Both loaded kernel images and the running service match source
`ebe5c0739caeac37c35c9537f509b76bb1537095`. Five current native KS interface
properties and their legacy `FriendlyName` values contain `WH-1000XM6`.
Nevertheless Core Audio reports interface name `XM6 Audio Topology` and endpoint
name `耳机 (XM6 Audio Topology)`. This was observed both offline and after the
headset reconnected. Thus the fixes below do **not** establish that existing
endpoint metadata has migrated correctly.

An administrator can open this endpoint's property store read/write, but
`SetValue` for both name keys returns `E_ACCESSDENIED`; even setting the endpoint
name to its existing value is rejected. The previously used Windows audio policy
COM path also rejected the interface-name write. No property-store update was
committed. A bounded change of the driver's topology interface property type
from STRING to STRING_INDIRECT retained the same headset name and did not refresh
Core Audio; the original type was restored. Therefore changing property type is
not an established remedy. No ACL, private MMDevice cache, endpoint identity,
user name, default or format was overwritten.

A proposed single targeted PnP refresh was stopped by its active-session guard
before stopping any service or device, because playback had resumed. The existing
endpoint migration/refresh path remains unresolved; new-install source fixtures
do not simulate AudioEndpointBuilder's persistence or prove fresh-host behavior.

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

## Offline publication after failed connection attempts

A subsequent one-shot log inspection found repeated suspend-publication error
21 and `state-update-pending` after the headset went offline. A failed outgoing
OPEN can advance the driver's connection generation without returning that new
generation to the service. The service retained the last successful link's
generation and kept using it for suspension reports, which the driver's
generation guard correctly rejected.

A controlled regression combines the actual service publication body with the
actual kernel `SuspendStatusCurrent` gate. Last successful generation 1 followed
by a closed, failed generation 7 reproduces the rejection. When the service has
no open signalling link, it now sends the already-defined no-link token (zero).
The unchanged kernel gate accepts that token only while its channel is Closed.
An incoming replacement in Opening or Open still rejects it, until the service
receives and uses that replacement's real generation. This fixes the stale
publication loop without weakening connection-epoch checks, adding a timer,
changing the IOCTL or suppressing errors from a real live replacement.

This regression does not establish the cause of the earlier audible PCM gap.
The prior 0.3.64 candidate was not installed; both repairs are consolidated into
the same deployment.

## Historical 0.3.64 scope

This change addresses the two reproduced name-publication defects. It does not
claim all bugs are eliminated, audible fidelity has improved, or every Windows
cache/lifecycle path has been accepted. The earlier unexplained PCM supply gap,
hardware format transitions, sleep/resume and Microsoft production signing
retain their separate evidence and status.

## Legacy endpoint migration verified before 0.4.0 (2026-11-12)

The first 0.4.0 upgrade candidate exposed a further gap: installing a **new**
package reintroduced the INF placeholder, despite the successful same-package
repair and unload/reload checks below. It was not accepted as the final release.
NOCLOBBER only protects an existing registry value; it still creates a
placeholder in a fresh interface key. Both static FriendlyName entries and
their unused strings have now been removed from the render INF. Deferred
filter creation already ensures the driver publishes the real headset name
first. The production-INF regression fails on the old fresh-key behavior and
passes on the new sections, preserving existing names and KS/endpoint settings.
Final package-upgrade acceptance is recorded in the hardware record.

Upgrade validation also rejected two package-identity defects before final
acceptance. Identical INF text let Driver Store reuse an earlier candidate;
both catalog-covered INFs now include the source commit. The CLI's provider,
date and version ranking could then select an older same-version transport
package. Native selection now enumerates only the INF adjacent to the verified
tool, while inbox recovery explicitly clears that scope. A fixture compiles
the production selector and covers tied packages, recovery and failed path/API
boundaries. Deployment checks the selected package, service path and loaded
image hashes rather than treating matching version numbers as sufficient.

A user reboot still left the Core Audio adapter label at `XM6 Audio Topology`.
The current render PDO and all five KS interfaces already published
`WH-1000XM6`. The topology jack container matched the PnP container. A readonly
inspection of the exact Windows binaries with matching Microsoft public PDBs
showed that the generated Core Audio label preferred a retained interface-name
field over the updated device name. These observations distinguish correct
driver metadata from successful migration of an existing Windows endpoint.

An ordinary device restart, a completed disable/enable cycle and removal of the
software endpoint followed by restart all retained the old name. A temporary
driver-owned EP property probe did not migrate it and was removed. Public
property writes were rejected; no MMDevice cache field, ACL or system binary
was modified. These unsuccessful approaches are not shipped.

Reinstalling the **same verified render INF** through the documented
`DiInstallDevice` API corrected both public Core Audio name properties and the
PnP endpoint label. This was an installation-lifecycle repair, not a user-name
override. The successful operation requested no computer restart. Windows
created a new ordinary endpoint ID, migrated all three default render roles,
retained capture defaults, float32/96 kHz stereo mix, volume and mute, and the
existing player's process produced nonzero signal through native LDAC again.

The installer now uses a compatible driver list restricted to the verified
render INF and calls `DiInstallDevice` for the selected `OA2DP\Render` child.
It checks the instance pattern, hardware ID, provider, selected-INF hash and
started/pending-restart state. Merely staging an already-selected package is
no longer reported as successful endpoint installation. There is no periodic
rename, private-cache edit, hard-coded headset name or runtime reinstall.

The fixture executes the production installer function and compiles its real
native boundary. It covers same-package reinstall, unrelated/missing devices,
wrong selected package, unsafe published-INF path, native install failure and
pending restart. Fixtures do not emulate the Windows endpoint cache; the live
before/after evidence above supplies that separate acceptance.

A subsequent child-disable request required deferred completion; after a
Bluetooth radio cycle unloaded both kernel images, the selected child was
explicitly re-enabled and its problem/configuration flags returned to zero.
The correctly named endpoint returned, with unchanged format/volume/mute and
96 kHz LDAC transmission. This verifies re-enumeration persistence on this
host, **not a new post-fix computer reboot or every Windows version**.

The two older audio-interruption investigations are closed at the user's
request. Their causes remain unassigned; this naming repair is not evidence
that either historical interruption was fixed.

References: [per-device installation](https://learn.microsoft.com/en-us/windows/win32/api/newdev/nf-newdev-diinstalldevice),
[interface metadata before registration](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/setting-properties-and-registry-values),
[Core Audio device properties](https://learn.microsoft.com/en-us/windows/win32/coreaudio/device-properties).
