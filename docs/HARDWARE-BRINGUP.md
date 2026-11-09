# Native hardware bring-up

The controlled developer trial targets WH-1000XM6 on Windows 10 x64, build 19045. It uses a local development signature under explicitly authorized test signing. This is separate from the production installer, which still requires Microsoft-signed catalogs.

## Observed with 0.3.12 (2026-11-09)

The local trial uses source commit `8c8a57ea91bdbb0d72a50b955851c92c0ded9a75`, built by [CI run 42612878702](https://github.com/TruthNian/xm6-audio/actions/runs/42612878702). Both CI jobs passed, including linked-driver metadata verification. The existing local development certificate was used; this is not a Microsoft-signed release.

- `IAudioClient::GetMixFormat` and endpoint-volume activation succeed after the local hardware-latency correction. The shared engine supplies 48 kHz stereo float32; this is not evidence of the music file's original format.
- QQ Music's active audio session was verified on the native XM6 endpoint, with nonzero audio peaks; its previous Hands-Free session is inactive. The per-app output override was reset to follow the Windows default. The user confirmed audible music with clearly restored quality.
- The service actually streams LDAC HQ, with 24-bit encoder input and approximately 1.01 Mbit/s measured media transport including packet overhead. This transport count is distinct from the nominal 990 kbit/s codec payload. ABR remains disabled.
- Windows endpoint volume is available and was set to 30%. The user confirmed automatic pause on removal and resume on wearing. The observed QQ media-session states changed from Playing to Paused and back to Playing during that test. No wearing-event emulator or media-key substitute was added.
- The installed SCM service has the native transport's device-interface arrival start trigger. Its demand-start setting is intentional; querying this configuration does not establish successful cold-boot startup.

The user subsequently confirmed native play/pause, previous/next and headset volume adjustment. **Volume integration is incomplete:** headset gestures change audible volume without changing the Windows endpoint slider; this is not bidirectional absolute-volume acceptance. QQ Music's per-app volume is independently scoped and need not mirror the master slider.

Turning the Bluetooth radio off and on restored native playback with the same endpoint ID and retained 30% Windows volume. However, the user reported that disconnecting only XM6 from Windows Bluetooth settings does not work properly. This radio-reset observation must not be called successful per-device disconnect/reconnect. Startup, sleep/resume, format transitions and video timing also remain outstanding.

## Endpoint identity correction in 0.3.13

The native render PDO already has the physical headset's Bluetooth container ID, but its MMDEVAPI endpoint had a different synthetic container. The topology now provides the documented `KSPROPERTY_JACK_CONTAINERID` from the PDO's `DEVPKEY_Device_ContainerId`. The property returns a GUID, validates the requested jack pin and output size, and fails if a reliable container ID is unavailable. This is a device-association correction; it does not by itself establish an absolute-volume implementation. The Bluetooth one-shot connection/disconnection property contract is still absent and remains required work.

Reference: [Microsoft's audio endpoint container ID contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/audio-endpoint-container-id).

The local 0.3.13 deployment corrected the MMDEVAPI endpoint's container ID to match the physical Bluetooth device. Windows now names that endpoint after WH-1000XM6, and QQ Music still has an active, nonzero native LDAC stream. The user explicitly confirmed that this identity fix **did not** synchronize headset volume with Windows. Windows AVRCP's A2DP volume interface is a separate integration requirement, not something provided by retaining its transport driver alone.

## Windows one-shot connection control in 0.3.14

Both native KS filters now expose `KSPROPSETID_BtAudio` one-shot reconnect/disconnect as GET commands without a property value. A disconnect disables outbound retries and rejects late connected=true updates; a reconnect enables attempts immediately. Stream notifications carry the current connection policy and command serial through the existing waiter. The service reads that state before initiating a connection, including after a service restart, rather than overriding an explicit disconnect with its retry loop. This extends the driver/service IOCTL contract to version 4; mismatched transport and service versions are rejected.

This source requires direct KS command and Windows Bluetooth UI acceptance. It does not claim complete Windows multi-profile-manager integration or absolute-volume synchronization.

References: [one-shot disconnect](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/ksproperty-oneshot-disconnect), [one-shot reconnect](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/ksproperty-oneshot-reconnect).

Direct KS support queries and actual one-shot disconnect/reconnect succeeded on 0.3.14. Disconnect suppressed retries until a reconnect command, after which nonzero QQ Music audio returned over LDAC. **The Windows Bluetooth UI still failed:** it reported `Couldn't disconnect` after audio stopped and retained the Disconnect button, preventing reconnection there. This remains a failed acceptance item; a working diagnostic command is insufficient.

## Native volume integration work in 0.3.15

Windows' AVRCP volume controller discovers the A2DP Bluetooth service devnode, then its audio control interface. The PortCls child alone does not satisfy that discovery path. The transport now provides a control-only KS volume-cache interface on the actual Bluetooth service node. A referenced, versioned kernel bus interface shares the cache with standard PortCls volume/mute nodes and node-change events. Host writes notify Windows AVRCP; remote writes notify the Windows audio engine without reflecting the write back to AVRCP. The audio engine can therefore use hardware volume instead of adding software gain before the headset's own gain.

This retains Microsoft's AVRCP protocol driver and service. The Windows 10 volume-cache property/event GUID and Bluetooth connection property are compatibility details observed in the local Windows implementation and its public Microsoft symbols; they are not a documented cross-version public ABI. No vendor binary, signature or private key is copied. This path requires hardware validation on the target Windows build before it can be described as working.

The control-only reference admits LOCAL SERVICE under the device ACL. Transport control still requires SYSTEM/admin and one owner; volume clients cannot issue transport IOCTLs. METHOD_NEITHER KS requests copy bounded structures in the caller's context, probe user addresses, reference event objects and release them on cleanup. File cleanup and topology-notification removal synchronize with volume changes under the parent lock. These are implementation properties, not a general driver-safety guarantee.

The same native interface accepts one-shot connection commands. The driver publishes its actual AVDTP readiness on the Bluetooth service node; complete Windows multi-profile state/UI acceptance is still required.

The local 0.3.15 trial loaded both new driver images. The native AVRCP cache received non-default, changing headset volume values through Microsoft's controller. Direct topology requests reported two control nodes and readable volume. Bounded KS checks rejected a short output, an invalid user output address and an unknown property with Win32 errors 122, 998 and 50, respectively. However, `IAudioEndpointVolume::QueryHardwareSupport` still returned zero: Windows continued to use software master volume. This is not successful volume synchronization.

## Endpoint lifetime and channel-contract correction in 0.3.16

PotPlayer was playing video without an active Windows render session after the endpoint updates. Resetting its per-app output to the system default and reopening the same video at the preserved playback position restored an active native session with nonzero audio; the user confirmed video sound. Reopening an application is recovery evidence, not acceptance of normal reconnect behavior.

The driver no longer unregisters and recreates its KS filters to refresh a friendly name. The naming work item, stream counter used only for that rebuild, and teardown wait were deleted. Friendly-name changes update metadata and cannot invalidate clients' render handles. Actual PnP removal still unregisters filters normally.

The volume/mute basic-support descriptions now expose two channels, both required MULTICHANNEL and UNIFORM flags, and complete per-channel volume/Boolean ranges. Version 0.3.15 incorrectly returned a single range without these flags, including no range for mute. Windows Vista and later require the multichannel flag on channel properties. Both nodes also expose their non-host gain implementation through CPU_RESOURCES. The parent KS one-shot handler now returns full support descriptors when requested and accepts GET commands with unused output capacity.

These changes require hardware-volume, Bluetooth UI and existing-client reconnect acceptance. They are not a claim that all lifecycle cases are complete. Reference: [Microsoft's multichannel-node contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/exposing-multichannel-nodes).

The user subsequently confirmed bidirectional headset/Windows master-volume synchronization. QQ Music's application volume remains independent. The Bluetooth settings disconnect still fails or leaves the wrong button state, so connection integration remains unaccepted.

The transport loaded the 0.3.16 image, but a query of the actual loaded audio module and its KS range description still identified the earlier audio image. An installed INF, service ImagePath, successful build, or working volume cache does not establish that a pending kernel update has taken effect. The endpoint-lifetime changes must be tested with the new audio module actually loaded.

## Disconnect completion ordering in 0.3.17

The service previously published disconnected before closing the media and signalling channels, and the one-shot KS request returned as soon as its command was queued. Disconnect now publishes after the synchronous close operations. Both the parent's control reference and the PortCls filters wait on notification events for that publication, with a bounded ten-second timeout reported as an error. The parent releases its registration spin lock before waiting; its topology callback remains nonblocking.

The existing PortCls filters, audio handle and PCM ring remain in place during Bluetooth-only reconnects. This does not install application hooks, restart players, capture their output, or add a recovery watchdog. Acceptance requires Windows settings to complete disconnect/reconnect correctly and existing application processes to produce audio through the same endpoint afterward. Build results and direct diagnostic reconnects alone do not meet that acceptance condition.

## Build identification in 0.3.18

The earlier resource file retained a manual 0.3.16 version while the 0.3.17 project and INF had advanced. Binary resources now derive from the configured CMake project version. Packaging rejects a project/INF/binary-version mismatch. During local acceptance, compare the actual loaded module's DriverStore path and hash with the signed candidate; neither the configured service path nor the version label alone establishes which image is running. No playback behavior is changed by this build-identification correction.

The CLI and installer also propagate Windows' restart-required result (3010). A pending driver selection no longer prints that the headset already uses the replacement. The installer reports activation pending when either transport selection or audio-driver installation requests a restart; it does not reboot the machine itself. This reporting correction does not establish that a driver is active or passes hardware acceptance.

## Earlier investigation, before 0.3.12

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

The standalone recovery source also coordinates codec-service shutdown before rebinding the selected headset. Offline tests exercise the actual script with fake service/PnP/install APIs, including stop failures/timeouts, binding failure, service absence, restart failure and pending reboot. This coordinates handle/mapping release before rebinding; it does not establish safety for unexpected PnP removal during playback, which remains a lifecycle acceptance item.

## Offline format-transition correction in 0.3.10

The service previously allocated input buffers from one channel count, then copied PCM using a mutable ring header. A mono-to-stereo change could therefore size a later copy differently from its destination. Previously queued samples could also be interpreted with the new sample rate or representation.

Ring protocol v2 uses fixed storage slots, atomic format generations, a first-frame boundary and bounded complete-block reads. The service encodes only a block matching one stable snapshot. Deterministic regressions cover obsolete mono buffers, representation/rate changes, wrapped storage, old-format backlog and incomplete encoder blocks. A separate two-thread memory-only test alternates 10,000 formats and checks generation-tagged samples and destination boundaries. Linux CI applies AddressSanitizer/UndefinedBehaviorSanitizer to both C and C++ tests. The kernel target explicitly selects the kernel-only ring code path, excluding user-mode floating-point conversion helpers. This source has not been deployed or exercised on the headset.

## Stop/wait lifecycle correction in 0.3.11

Review against [Windows' remove-IRP ordering](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/understanding-when-remove-irps-are-issued) does not establish a normal surprise-remove mapping-revocation race: final removal follows handle closure. The source preserves the mapping during stop/surprise-stop. No exception-swallowing guard or new mapping lifetime was added.

A separate source defect was confirmed: an existing control handle could queue `WAIT_FORMAT` after stop's final drain, because only final removal was checked. Readiness changes and wait insertion now use the same spin lock. Stopped devices reject new control operations, while unmap/cleanup remain available; a late connection update cannot mark a stopped endpoint connected. Startup makes control readiness visible before announcing its interface.

On format-wait failure, the service now preserves the Win32 error, stops the encoder, releases the old mapping/handle, and retries through its existing connection state machine. The waiter stays quiescent until that release completes, and actual PCM metadata is retained. Build checks cover compilation and linked descriptors; they do not exercise Windows PnP stop/restart timing or establish successful reconnection. Those hardware checks remain deferred.

## Local WaveRT latency correction in 0.3.12

Hardware investigation isolated a buffer-initialization rejection after successful KS pin creation and allocation. The miniport reported the remote Bluetooth/headset delay (about 200 ms in that test) as local WaveRT hardware latency. Windows rejected a normal 10–20 ms cyclic buffer because the reported local latency exceeded its size. This also prevented default mix-format discovery and endpoint-volume activation; successful AVDTP negotiation did not mean that music was using this endpoint.

The virtual endpoint now reports zero local FIFO/chipset/codec delay, while retaining the downstream Bluetooth delay in its presentation-position calculation. This separates local buffer consumption from remote presentation without inflating the cyclic buffer. The fix requires actual WASAPI initialization, audible LDAC playback and video-timing checks before acceptance. Manual controls, wearing detection and reconnection remain separate hardware acceptance items.

Reference: [Microsoft's WaveRT hardware-latency property](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/ksproperty-rtaudio-hwlatency).

## Do not load candidate 0.3.5

A local crash dump identified a descriptor ABI error introduced in 0.3.5: its stream data-range pointer table used a serialized `KSMULTIPLE_ITEM` where PortCls expects a pointer-based `KSATTRIBUTE_LIST`. The kernel interpreted a length as a pointer while enumerating the attributes. The developer trial was rolled back to the Windows inbox driver.

The 0.3.6 source uses the WDK's actual `KSATTRIBUTE_LIST` type and a separate array of attribute pointers. A new offline PE/PDB metadata verifier traverses the linked stream table without loading kernel code. It rejects the crashing 0.3.5 image before local signing/deployment and runs against new driver builds in CI. This check covers descriptor structure; it is not a general driver-safety or audio-stability guarantee.

Crash dumps, raw ETW, device addresses, local certificates and signing keys remain private. Source changes, build artifacts and the distinction between build checks and hardware acceptance remain reviewable in the repository.
