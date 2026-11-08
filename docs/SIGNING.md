# Kernel signing and free channels

## Code signing policy

Project owner and release approver: [@TruthNian](https://github.com/TruthNian). Signing has not been approved or sponsored by any provider. A future request must identify the reviewed source commit, pinned dependencies and corresponding public CI artifact. Release/signing approval is manual; a green CI run alone does not authorize a submission or establish hardware compatibility.

The kernel SYS files require Microsoft kernel-policy trust. INF, service/tool executables and codec DLLs are checked as catalog members of the returned package. Runtime privacy is documented in [PRIVACY.md](PRIVACY.md), and source ownership/attribution in [PROVENANCE.md](PROVENANCE.md). Provider-specific account, MFA, role and eligibility requirements must be satisfied before the first signing request. This project does not advertise a provider's sponsorship before approval.

## Current status

Research date: 2026-11-08. A public feasibility inquiry has been published as [SignPath/fdn-website#28](https://github.com/SignPath/fdn-website/issues/28). The project is exploring free OSS signing channels. No signing application or binary submission has been made, and a reply/approval is still pending.

Unsigned native candidates must not replace a working inbox A2DP path on a normally protected system. The native package needs valid Microsoft kernel-policy signatures and a controlled hardware acceptance path before deployment can be accepted.

## Verified primary sources

| Channel | Verified facts | Consequence for this project |
|---|---|---|
| [Microsoft requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-reqs) | Hardware dashboard submissions require an account associated with a valid EV certificate. | No project signing account/certificate is configured. Building an open-source package does not supply this prerequisite. |
| [Microsoft attestation](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-attestation) | An eligible submitter can submit driver packages without HLK test logs for attestation. Attestation does not establish hardware quality or retail Windows Update distribution. | Requires an authorized signing organization; not an anonymous upload/signing endpoint. |
| [OSSign](https://ossign.org/) | Publicly offers free OSS signing and EV/attestation for selected projects. Applications currently suspended; minimum six months activity applies to account/organization/project, including forks without prior upstream involvement. | A real candidate in principle, unavailable immediately and this new project does not meet published history requirements. Do not claim an application or exception exists. |
| [SignPath Foundation](https://signpath.org/terms) | Free OSS signing subject to review, provenance, project reputation and rules for modified upstream sources. Its public terms require upstream signed releases for certain fork-signing cases. | Public material does not confirm a free Microsoft kernel-attestation path for these two drivers. Eligibility and technical coverage need confirmation; no acceptance is implied. |

No immediately available, confirmed free route has been found. A signing sponsor collaborating with the upstream project is a possible route to investigate, not an obtained signature or an installation workaround. The [approved inquiry text](SIGNING-INQUIRY.md) is retained for traceability; publication is not approval or a signed package.

## A vendor signature cannot be transferred

Microsoft's [catalog documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/catalog-files) describes hashes of the exact package members. Changing even one byte invalidates the package signature. Alternative A2DP may be used unmodified subject to its license; its SYS/CAT signature cannot authenticate new XM6 Audio code. Local-only use does not change cryptographic validation.

## Verify a returned signed package

```powershell
./scripts/verify-package.ps1 -PackageDirectory path/to/signed-native
```

This read-only gate requires a valid Microsoft catalog, SignTool `/kp` verification of each SYS, and `/pa /c` membership verification for the INF and user-mode payload. Do not reuse old unsigned catalogs or modify signed package members. A signature proves trust/integrity; actual audio and driver reliability still require testing.

After review, in an administrator shell, select the paired device's address locally (never put it in an issue or commit):

```powershell
./scripts/install.ps1 -PackageDirectory path/to/signed-native -DeviceAddress <local-address> -CheckOnly
./scripts/install.ps1 -PackageDirectory path/to/signed-native -DeviceAddress <local-address>
```

Recovery is scoped to the same selected headset:

```powershell
./scripts/install.ps1 -DeviceAddress <local-address> -RestoreWindows
# Standalone fallback if the codec service or native package is missing/damaged:
./scripts/restore-windows.ps1 -DeviceAddress <local-address> -CheckOnly
./scripts/restore-windows.ps1 -DeviceAddress <local-address>
```

The INF installs the codec service as demand-start with a device-interface trigger and runs it from Driver Store. Recovery uses SetupAPI and Windows' inbox A2DP INF directly, without executing any native-package binary or requiring its catalog, service registration or local backup. It selects one present A2DP service by address across device classes (the Microsoft driver uses MEDIA), and refuses absent or ambiguous targets.

Recovery first checks the compatible Microsoft driver. If the codec service exists and is active, recovery waits for it to stop before rebinding, so its PCM mapping is released before the audio child is removed. Failure to confirm service shutdown aborts the rebind. A previously running service is resumed afterward, including after a failed bind, to serve other native headsets; when replacement requires a reboot it stays stopped until that reboot. Missing service registration does not block recovery. `-CheckOnly` does not inspect or change the service. Pairing, radio ownership, cached package files and the local backup are retained. No automatic reboot or broad `restore all` is performed.

## Still required before calling native complete

- Legitimate loadable signatures for both drivers, catalog membership and consistent package version.
- Hardware validation of native endpoint name, volume/mute, retained AVRCP play/pause/next/previous, actual sound and source-rate transitions.
- Headset off/on, disconnect state notification, sleep/resume and service restart without stale PCM or silent-success status.
- Shared/exclusive audio and video timing; no claim to infer file format from Windows shared-mode PCM.
- Integer/float32 and 24-in-32 endpoint acceptance; visible input/container/encoder precision; honest LDAC/AAC/SBC negotiation and fixed-profile rejection.

The WaveRT topology now implements KS jack presence properties and standard jack-change events. The service marks the endpoint disconnected on link failure; control-handle cleanup also handles a crashed service. These paths have not yet been exercised on hardware. Retaining Windows AVRCP services is an architectural prerequisite, not evidence that headset buttons already control this new endpoint on hardware.
