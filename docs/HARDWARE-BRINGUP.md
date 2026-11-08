# Native hardware bring-up

The controlled developer trial targets WH-1000XM6 on Windows 10 x64, build 19045. It uses a local development signature under explicitly authorized test signing. This is separate from the production installer, which still requires Microsoft-signed catalogs.

## Observed on hardware

- The KMDF transport and PortCls/WaveRT child load and enumerate through the Windows Bluetooth stack. Windows retains the radio, pairing and AVRCP profiles.
- The headset reports SBC, AAC and LDAC endpoints. Native signalling and media channels have opened, with LDAC configured at 48 kHz stereo. This proves negotiation, not audible playback or sustained bitrate.
- Exclusive-mode format queries accepted integer16/24/32 and float32; 24-in-32 was explicitly added to the service's format list. These queries do not prove successful rendering.
- Shared-mode initialization has returned `AUDCLNT_E_UNSUPPORTED_FORMAT`. Native audible playback, endpoint volume, media controls and lifecycle acceptance are still outstanding.
- Actual exclusive-mode initialization also failed; successful format queries were insufficient. Direct `KsCreatePin` calls returned `ERROR_INVALID_PARAMETER` for the driver's preferred format and integer PCM formats. Version 0.3.7 adds an opt-in kernel ETW provider (`XM6Audio.WaveRT`, `{ad2dba1f-78a1-4d25-a66e-d3195b9b1f72}`) to locate the failing stream-creation stage. It logs scalar status/phase information only, without file names, device addresses or audio content.
- A paging timeout was observed in Bluetooth ETW. The headset responded after a power cycle. The service no longer requires an existing ACL link before initiating its own connection; complete reconnection acceptance remains outstanding.

## Do not load candidate 0.3.5

A local crash dump identified a descriptor ABI error introduced in 0.3.5: its stream data-range pointer table used a serialized `KSMULTIPLE_ITEM` where PortCls expects a pointer-based `KSATTRIBUTE_LIST`. The kernel interpreted a length as a pointer while enumerating the attributes. The developer trial was rolled back to the Windows inbox driver.

The 0.3.6 source uses the WDK's actual `KSATTRIBUTE_LIST` type and a separate array of attribute pointers. A new offline PE/PDB metadata verifier traverses the linked stream table without loading kernel code. It rejects the crashing 0.3.5 image before local signing/deployment and runs against new driver builds in CI. This check covers descriptor structure; it is not a general driver-safety or audio-stability guarantee.

Crash dumps, raw ETW, device addresses, local certificates and signing keys remain private. Source changes, build artifacts and the distinction between build checks and hardware acceptance remain reviewable in the repository.
