# Native hardware bring-up

The controlled developer trial targets WH-1000XM6 on Windows 10 x64, build 19045. It uses a local development signature under explicitly authorized test signing. This is separate from the production installer, which still requires Microsoft-signed catalogs.

## Observed on hardware

- The KMDF transport and PortCls/WaveRT child load and enumerate through the Windows Bluetooth stack. Windows retains the radio, pairing and AVRCP profiles.
- The headset reports SBC, AAC and LDAC endpoints. Native signalling and media channels have opened, with LDAC configured at 48 kHz stereo. This proves negotiation, not audible playback or sustained bitrate.
- Exclusive-mode format queries accepted integer16/24/32 and float32; 24-in-32 was explicitly added to the service's format list. These queries do not prove successful rendering.
- Shared-mode initialization has returned `AUDCLNT_E_UNSUPPORTED_FORMAT`. Native audible playback, endpoint volume, media controls and lifecycle acceptance are still outstanding.
- Actual WASAPI exclusive-mode initialization also failed; successful format queries were insufficient. A direct KS probe initially omitted the required nonzero priority subclass and returned `ERROR_INVALID_PARAMETER`; this was a probe defect. Corrected `KsCreatePin` requests actually opened float32 and integer16/24/32 streams, and a float32 stream accepted notification support and actual buffer-allocation requests. This does not prove WASAPI initialization or audible playback. Version 0.3.7 adds an opt-in kernel ETW provider (`XM6Audio.WaveRT`, `{ad2dba1f-78a1-4d25-a66e-d3195b9b1f72}`), extended in 0.3.8 with format-property stages. It logs scalar status/phase information only, without file names, device addresses or audio content.
- A paging timeout was observed in Bluetooth ETW. The headset responded after a power cycle. The service no longer requires an existing ACL link before initiating its own connection; complete reconnection acceptance remains outstanding.

## Offline format-contract correction in 0.3.9

The proposed-format handler previously copied a signal-processing attribute list after checking only its outer length. It could return a preferred format for modes absent from `GetModes`, or echo malformed attribute records. The handler now validates the complete list, record bounds, count, padding and supported mode before returning a format. DEFAULT and RAW remain supported; unsupported modes are rejected rather than reported as supported.

The exact allocation-free parser used by the miniport is exercised in a user-mode contract test. These tests open no audio devices and cover supported modes, required attributes, truncated buffers, invalid sizes/counts/flags, unknown modes/attributes and duplicate modes. Full driver builds still require the offline PE/PDB descriptor check. This correction has not been deployed or tested on the headset, and is not established as the cause of the WASAPI failure.

Reference: [Microsoft's proposed-format and attribute handling sample](https://github.com/microsoft/Windows-driver-samples/blob/main/audio/simpleaudiosample/Source/Main/minwavert.cpp).

## Offline recovery lifecycle correction

The standalone recovery source also coordinates codec-service shutdown before rebinding the selected headset. Offline tests exercise the actual script with fake service/PnP/install APIs, including stop failures/timeouts, binding failure, service absence, restart failure and pending reboot. This reduces the recovery-time mapping teardown race; it does not establish safety for unexpected PnP removal during playback, which remains a lifecycle acceptance item.

## Offline format-transition correction in 0.3.10

The service previously allocated input buffers from one channel count, then copied PCM using a mutable ring header. A mono-to-stereo change could therefore size a later copy differently from its destination. Previously queued samples could also be interpreted with the new sample rate or representation.

Ring protocol v2 uses fixed storage slots, atomic format generations, a first-frame boundary and bounded complete-block reads. The service encodes only a block matching one stable snapshot. Deterministic regressions cover obsolete mono buffers, representation/rate changes, wrapped storage, old-format backlog and incomplete encoder blocks. A separate two-thread memory-only test alternates 10,000 formats and checks generation-tagged samples and destination boundaries. Linux CI applies AddressSanitizer/UndefinedBehaviorSanitizer to both C and C++ tests. The kernel target explicitly selects the kernel-only ring code path, excluding user-mode floating-point conversion helpers. This source has not been deployed or exercised on the headset.

## Do not load candidate 0.3.5

A local crash dump identified a descriptor ABI error introduced in 0.3.5: its stream data-range pointer table used a serialized `KSMULTIPLE_ITEM` where PortCls expects a pointer-based `KSATTRIBUTE_LIST`. The kernel interpreted a length as a pointer while enumerating the attributes. The developer trial was rolled back to the Windows inbox driver.

The 0.3.6 source uses the WDK's actual `KSATTRIBUTE_LIST` type and a separate array of attribute pointers. A new offline PE/PDB metadata verifier traverses the linked stream table without loading kernel code. It rejects the crashing 0.3.5 image before local signing/deployment and runs against new driver builds in CI. This check covers descriptor structure; it is not a general driver-safety or audio-stability guarantee.

Crash dumps, raw ETW, device addresses, local certificates and signing keys remain private. Source changes, build artifacts and the distinction between build checks and hardware acceptance remain reviewable in the repository.
