# Kernel signing and free channels

Research date: 2026-11-08. No application, email, external issue or binary submission has been sent. No new paid service is authorized.

The listening machine currently uses the Microsoft inbox A2DP driver, with normal Bluetooth and audible XM6 sound restored. That recovery does not establish LDAC support. The native candidate must not replace it until the two drivers have a valid Microsoft kernel-policy signature and a controlled hardware acceptance path.

## Verified primary sources

| Channel | Verified facts | Consequence for this project |
|---|---|---|
| [Microsoft requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-reqs) | Hardware dashboard submissions require an account associated with a valid EV certificate. | The user has neither a signing account nor certificate. A free build does not supply this prerequisite. |
| [Microsoft attestation](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-attestation) | An eligible submitter can submit driver packages without HLK test logs for attestation. Attestation does not establish hardware quality or retail Windows Update distribution. | Requires an authorized signing organization; not an anonymous upload/signing endpoint. |
| [OSSign](https://ossign.org/) | Publicly offers free OSS signing and EV/attestation for selected projects. Applications currently suspended; minimum six months activity applies to account/organization/project, including forks without prior upstream involvement. | A real candidate in principle, unavailable immediately and this new project does not meet published history requirements. Do not claim an application or exception exists. |
| [SignPath Foundation](https://signpath.org/terms) | Free OSS signing subject to review, provenance, project reputation and rules for modified upstream sources. Its public terms require upstream signed releases for certain fork-signing cases. | Public material does not confirm a free Microsoft kernel-attestation path for these two drivers. Eligibility and technical coverage need confirmation; no acceptance is implied. |

No immediately available, confirmed free route has been found. A signing sponsor collaborating with the upstream project is a possible route to investigate, not an obtained signature or an installation workaround. A reviewable [inquiry draft](SIGNING-INQUIRY.md) is provided; sending it requires explicit user authorization.

## A vendor signature cannot be transferred

Microsoft's [catalog documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/catalog-files) describes hashes of the exact package members. Changing even one byte invalidates the package signature. Alternative A2DP may be used unmodified subject to its license; its SYS/CAT signature cannot authenticate new XM6 Audio code. Local-only use does not change cryptographic validation.

## Verify a returned signed package

```powershell
./scripts/verify-package.ps1 -PackageDirectory path/to/signed-native
```

This read-only gate requires a valid Microsoft catalog and SignTool `/kp` verification of each SYS/INF against its catalog. Do not reuse old unsigned catalogs or modify any signed INF/SYS. A signature proves trust/integrity; actual audio and driver reliability still require testing.

After review, in an administrator shell, select the paired device's address locally (never put it in an issue or commit):

```powershell
./scripts/install.ps1 -PackageDirectory path/to/signed-native -DeviceAddress <local-address> -CheckOnly
./scripts/install.ps1 -PackageDirectory path/to/signed-native -DeviceAddress <local-address>
```

Recovery is scoped to the same selected headset:

```powershell
./scripts/install.ps1 -DeviceAddress <local-address> -RestoreWindows
```

Recovery switches the selected headset back to Microsoft A2DP and makes the native codec service demand-start. It retains pairing, radio ownership, package files and local backup for diagnosis. No automatic reboot or broad `restore all` is performed.

## Still required before calling native complete

- Legitimate loadable signatures for both drivers, catalog membership and consistent package version.
- Hardware validation of native endpoint name, volume/mute, retained AVRCP play/pause/next/previous, actual sound and source-rate transitions.
- Headset off/on, disconnect state notification, sleep/resume and service restart without stale PCM or silent-success status.
- Shared/exclusive audio and video timing; no claim to infer file format from Windows shared-mode PCM.

Known prototype gap: the inherited WaveRT topology does not yet report jack connected/disconnected state. Retaining Windows AVRCP services is an architectural prerequisite, not evidence that headset buttons already control this new endpoint on hardware.
