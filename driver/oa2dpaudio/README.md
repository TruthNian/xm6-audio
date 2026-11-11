# oa2dpaudio.sys — virtual render endpoint

**Status: builds (WDK 10.0.26100, CI) and its catalog passes Inf2Cat.
Version 0.3.12 has audible LDAC playback and endpoint-volume evidence in
one controlled, test-signed WH-1000XM6 trial.** See the
[hardware record](../../docs/HARDWARE-BRINGUP.md) for acceptance limits.

Each headset served by `oa2dp.sys` gets a child device `OA2DP\Render\<address>`.
This driver loads on it and shows Windows a normal "Headphones" render
endpoint. Audio the engine renders is copied into a shared
[`oa2dp_pcm_ring`](../../core/include/oa2dp/pcm_ring.h) that `oa2dpsvc.exe`
maps, encodes and sends. The consumer side is `AudioEndpoint` in
[`service/src/driver_io.cpp`](../../service/src/driver_io.cpp); the IOCTL
contract is [`oa2dp_ioctl.h`](../common/oa2dp_ioctl.h).

## Files

| File | Origin | What it does |
|------|--------|--------------|
| `adapter.cpp` | SYSVAD `adapter.cpp` + install code of `common.cpp` | DriverEntry, AddDevice, StartDevice, PnP; installs/removes wave + topology subdevices on actual device lifecycle; routes control IRPs |
| `minwavert.cpp/.h` | SYSVAD `minwavert.*` | WaveRT miniport: formats from the service, data intersection, `KSPROPERTY_PIN_PROPOSEDATAFORMAT(2)`, `KSEVENT_PINCAPS_FORMATCHANGE` |
| `minwavertstream.cpp/.h` | SYSVAD `minwavertstream.*` | Stream: buffer allocation, 1 ms position timer that pumps the ring, positions, packet notifications, presentation position, latency |
| `mintopo.cpp` | SYSVAD `mintopo.cpp`/`basetopo.cpp` | Two-pin topology filter (bridge → headphones) |
| `wavtable.h`, `toptable.h` | SYSVAD `bthhfpspeakerwavtable.h`, `speakertoptable.h` | Filter descriptors |
| `newdelete.cpp/.h` | SYSVAD `NewDelete.*` | Kernel `new`/`delete` |
| `control.cpp` | OpenA2DP | Control interface (`GUID_DEVINTERFACE_OA2DP_AUDIO`), IOCTLs, persisted settings |
| `ring.c`, `oa2dpaudio.h` | OpenA2DP | Ring producer and user mapping; shared state |
| `oa2dpaudio.inf` | OpenA2DP (layout as in SYSVAD's INF) | Matches `OA2DP\Render` |

SYSVAD files keep Microsoft's header plus a note on what changed
(Windows-driver-samples commit `2dc3fd3a`, MS-PL, text in
[`LICENSE.MS-PL`](LICENSE.MS-PL)). Removed from SYSVAD: every other endpoint
(mic arrays, HDMI, S/PDIF, USB/Bluetooth sideband, keyword spotter),
offload, loopback, audio modules, APO/effects, DRM, PoFx, ETW, tone
generator, file saving, KMDF.

## How the plan was implemented

1. **Simple render endpoint.** Wave filter: host pin (one instance) → DAC
   node → bridge pin; topology filter: bridge in → `KSNODETYPE_HEADPHONES`.
   The volume/mute nodes share the native parent's AVRCP volume cache through
   a referenced kernel interface. Hardware-volume acceptance is recorded
   separately in the hardware bring-up log.
2. **Formats** come from `IOCTL_OA2DP_AUDIO_SET_FORMATS`. The KS data
   ranges are only an envelope (8–192 kHz, 1–2 channels, float32 or 16/24/32-bit
   PCM); `IsFormatSupported`, `DataRangeIntersection`,
   `PROPOSEDATAFORMAT` (get = preferred, set = check) and
   `PROPOSEDATAFORMAT2` answer from the list. Float32 is offered first. A
   different set (not just a different preferred index) raises
   `KSEVENT_PINCAPS_FORMATCHANGE`, which makes the engine re-pick its format.
   The last list is stored in the device key (`Oa2dpFormats`) so the
   endpoint looks right before the service connects; the default is
   48 / 44.1 kHz stereo float.
3. **Pump.** The stream keeps positions in frames from the elapsed RUN time
   (no drift, never half a frame). A 1 ms high-resolution timer (SYSVAD's
   hardware emulation, here also in polled mode) calls `Oa2dpRingPump()`
   with the frames played since the last tick. Float32 is copied as is;
   16/24-bit PCM becomes 24-bit values in int32 (`OA2DP_RING_FMT_S32`).
   `OA2DP_RING_FLAG_RUNNING` follows accepted `SetState(RUN)` / leaving RUN
   reports, after checking the complete producer format.
   ABI 11's referenced data-ready event wakes the service at a complete
   encoder-block threshold, including already-buffered data at registration.
   Format/RUN changes also wake it; unmap/cleanup release the event reference.
   The service has independent stop events and no Sleep(2) PCM polling loop.
4. **Format change.** The ring is allocated once for 2 channels and never
   moves. Ring protocol v2 keeps the two-channel storage stride fixed, even
   for mono PCM. A format transaction updates rate/channels/representation,
   an atomic sequence and the first position of the new format. Source
   container/valid-bit changes also start an epoch even if this normalized
   storage layout is unchanged. Producer admission and state reports compare
   the complete source tuple. Counters
   keep growing; the consumer discards preceding backlog instead of treating
   it as the new format. A bounded complete-block read uses one snapshot and
   rejects a sequence change before publishing the block to the encoder.
   A format update also sets
   `OA2DP_RING_FLAG_FORMAT_CHANGED` and completes a pending
   `IOCTL_OA2DP_AUDIO_WAIT_FORMAT`. WAIT_FORMAT also completes on RUN
   (`Running = 1`) and when leaving RUN (`Running = 0`), which the service
   uses to connect on play and to suspend when idle. The latest state wins
   if the service has not picked up the previous one; a new control handle
   starts with the current state if a stream is running.
5. **Latency.** The local virtual WaveRT path has no hardware FIFO, chipset
   or DAC delay, so `KSRTAUDIO_HWLATENCY` reports zero for those fields.
   Windows validates that local delay against its cyclic buffer size;
   including a remote headset's delay can reject an otherwise valid buffer.
   Downstream `TransportUs + DeviceUs`, supplied by
   `IOCTL_OA2DP_AUDIO_SET_LATENCY`, is instead subtracted from transferred
   frames to estimate `KSPROPERTY_RTAUDIO_PRESENTATION_POSITION`. The committed
   estimate never decreases within a stream epoch, freezes at PAUSE and resets
   on STOP; updated delay cannot undo an already reported position. The KS
   timestamp retains raw QPC ticks. End-to-end video timing still needs hardware
   testing because this estimate does not observe the receiver's actual output.
6. **Control interface.** Deviation from the earlier plan (a KMDF
   miniport-mode control device): a control device object cannot carry a
   per-PDO device interface, and the service picks the endpoint of a headset
   by finding `#<address>#` in the interface path, which only a PDO
   interface has; in KMDF miniport mode the framework also does not dispatch
   I/O. So `GUID_DEVINTERFACE_OA2DP_AUDIO` is registered on our PDO with the
   reference string `Oa2dpControl`, and `adapter.cpp` hooks
   `IRP_MJ_CREATE/CLEANUP/CLOSE/DEVICE_CONTROL`: a create with that
   reference string, and later IRPs on the FILE_OBJECT we tagged, go to
   `control.cpp`; everything else goes to PortCls unchanged. The driver
   needs no KMDF.
7. **Friendly name.** `IOCTL_OA2DP_AUDIO_SET_NAME` stores the name
   (`Oa2dpName`), sets `DEVPKEY_Device_FriendlyName` on the PDO and publishes
   the name to all five audio interface/reference pairs in the INF. Cached
   names are published before filter registration; a same-name service request
   retries interface publication. Naming never re-registers filters or restarts
   audio clients. In the 0.3.62 host observation all these properties matched
   the headset name, while Core Audio still returned the old display name.
   Do not treat successful property writes as visible-name acceptance; see
   [current diagnostics](../../docs/DIAGNOSTICS.md#endpoint-name-publication-0362-partially-effective).

`oa2dp.sys` now reports `UniqueID` for the child PDO; without it the PnP
manager would prefix the instance ID and the service would not find the
endpoint.

## Security

The device object must keep the standard audio ACL (the audio engine runs
as LOCAL SERVICE and opens the KS filters), so the control path checks the
caller itself: only SYSTEM or an elevated administrator may open it, and only
one handle at a time. The ring is mapped only into the process that sent
`MAP_RING`; it is unmapped on `UNMAP_RING`, on handle cleanup, and on device
removal (attaching to that process if needed) before the pages are freed.
The service validates the header against the mapping size
(`oa2dp_ring_validate`) before reading.

## Open points

- Driver Verifier has not been run; it requires a controlled test system.
  The authorized local test-signed headset trial and its acceptance limits
  are recorded in [hardware bring-up](../../docs/HARDWARE-BRINGUP.md).
- PDO and interface names have been verified on the host, but Core Audio's
  visible-name refresh remains unresolved. A publication failure is logged
  and does not prevent the other name properties from being attempted.
- The service now discards accumulated PCM when a Bluetooth stream restarts,
  rather than replaying the pre-reconnection backlog. Hardware timing
  still needs validation.
- KS jack presence descriptions and change events are implemented. Link
  teardown, media failure and service-handle cleanup mark the endpoint
  disconnected; the negotiated endpoint marks it connected. AudioEndpointBuilder
  behavior and default-device policy still need hardware validation.
