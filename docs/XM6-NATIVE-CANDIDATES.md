# Native Windows backend investigation

Checked on 2026-11-08. Candidate README claims are not acceptance evidence. None of the candidates below was installed on the user's machine.

## The boundary to preserve

Windows must continue owning `BTHUSB` / `BTHPORT`, pairing, Bluetooth state and the other Bluetooth profiles. Replace or extend the headset's A2DP audio profile, not the USB radio. Expose a real Windows render endpoint and its volume/lifecycle interfaces. Encode PCM once and negotiate fixed LDAC 990 kbps at 48 kHz. Preserve working native AVRCP where feasible.

Alternative A2DP's installed MEDIA-class `BTHENUM` INF demonstrates this boundary; it registers KS/WDM render and topology interfaces. Its existing binary does not make its implementation open source, and this repository does not copy it or bypass licensing.

## Candidates and verified limitations

| Candidate | Architectural relevance | State established from the project |
|---|---|---|
| [RafiMota/open-a2dp](https://github.com/RafiMota/open-a2dp) | Windows 10/11 A2DP profile, audio endpoint, shared PCM ring and encoder service; intends to leave AVRCP/HFP with Windows | Pre-alpha. Its README's old 'not WDK-built' wording is superseded by CI run 38632901913, which built both drivers and generated catalogs. All downloaded SYS/CAT files have local Authenticode status `NotSigned`. No real-headset verification established. Pinned as a source prototype under `native/open-a2dp`; not installed. |
| [yariktop133/ldac-a2dp-windows](https://github.com/yariktop133/ldac-a2dp-windows) | KMDF A2DP/AVRCP implementation, real ACX render endpoint and encoder service | Reports real JBL/Intel use on Windows 11; provides a release, but explicitly requires a test-signed driver and Windows 11/ACX. It does not provide a normal signed Windows 10 package for this machine. |
| [sourajitk/ldac](https://github.com/sourajitk/ldac) | KMDF filter at the native Bluetooth BRB boundary | Contains source and a signing workflow, but no released driver package found. Signing documentation requires project approval/credentials and later Microsoft submission; merely having a workflow is not a Microsoft-signed loadable package. No real-XM6 acceptance established. |
| [will-ch-h/l2cap-windowsdriver](https://github.com/will-ch-h/l2cap-windowsdriver) via [LibrePods bridge](https://github.com/will-ch-h/librepods-windowsbridge) | A user-mode L2CAP interface backed by a kernel profile driver | The bridge's own README explicitly says its bundled driver is test-signed and requires test mode. 'Signed' there does not mean normal production-loadable. It is also not an audio endpoint. |

The Microsoft [Bluetooth Echo L2CAP profile sample](https://github.com/microsoft/Windows-driver-samples/tree/main/bluetooth/bthecho) and [SysVAD audio sample](https://github.com/microsoft/Windows-driver-samples/tree/main/audio/sysvad) define useful supported interfaces. They are source starting points, not finished Sony audio drivers.

## A real remaining deployment condition

A new kernel driver must satisfy the machine's normal Windows code-integrity policy. Microsoft documents the [kernel signing requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/kernel-mode-code-signing-requirements--windows-vista-and-later-) and [attestation signing](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/driver-signing-offerings). An ordinary application signature or a project's own test certificate is not equivalent to a production-loadable driver package.

No test mode, Secure Boot change, fake Bluetooth-on indicator or renamed unrelated output device is used to claim success. The next native milestone is a buildable Windows 10 profile/endpoint implementation with a valid deployment path. Until then, Bluetooth-on and the standard XM6 volume endpoint remain explicitly unmet.
