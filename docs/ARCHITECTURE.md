# Native Windows audio architecture

The native implementation adds two device-scoped drivers and a codec service. Windows retains radio control, pairing and its AVRCP/HFP profile drivers.

| Component | Responsibility |
|---|---|
| `driver/oa2dp` — KMDF | Binds to the selected BTHENUM Audio Sink service; opens/accepts L2CAP channels using Bluetooth Request Blocks; transfers SDUs; creates a per-headset audio child. |
| `driver/oa2dpaudio` — PortCls/WaveRT | Exposes the standard render endpoint, PCM ring, native format proposals, latency and connected-state properties. Derived from Microsoft SYSVAD under MS-PL. |
| `service/oa2dpsvc` | Negotiates AVDTP/A2DP, chooses an available encoder, follows the actual endpoint format and sends encoded audio through the profile driver. |
| `core/` | Portable C99 protocol state, capability negotiation, RTP/LATM framing, PCM metadata/conversion, ring buffers and settings. |
| `codecs/` | User-mode adapters for the pinned codec libraries and Windows Media Foundation AAC. The default build includes LDAC/AAC/SBC. |
| `tools/oa2dpctl` | Device selection, per-device settings, status and restoration to the Microsoft driver. |

## Native installation and ownership

The transport INF includes the codec service, command-line tool and replaceable codec DLLs in its Driver Store payload. Its non-associated Win32 service uses demand start with a device-interface-arrival trigger. The associated PnP function driver remains `oa2dp.sys`.

Windows registers and starts the service from the INF. There is no HKCU Run entry, login launcher, task-scheduler watchdog or separate Program Files payload. The deployment script stages signed packages and selects one headset's A2DP service; the radio driver and pairing stay with Windows.

The catalog covers the kernel file, INF, service/tool executables and codec DLLs. The deployment gate verifies kernel policy for SYS files and catalog membership for the rest before using the protected Driver Store tool. A Microsoft signature and successful hardware acceptance remain separate requirements.

Reference: Microsoft's [Win32 services interacting with devices](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/best-practices-win32services-interacting-with-devices) and [run from Driver Store](https://learn.microsoft.com/en-us/windows-hardware/drivers/develop/run-from-driver-store).

## PCM and clock boundaries

The format contract distinguishes sample rate, channels, container width, valid precision and integer/float representation. Integer32 remains integer32 in the shared ring; 24-in-32 follows WAVEFORMATEXTENSIBLE's left-aligned precision convention. Conversion to 16/24 useful bits occurs at the selected encoder boundary.

Ring protocol v2 fixes the allocated frame stride, independent of the active channel count. A producer sequence and first-frame position separate format generations. The consumer copies only a complete bounded block for its encoder's snapshot, checks the generation after copying, and releases no block on a format race. Old-format backlog is skipped, not reinterpreted. Partial codec input blocks remain queued until complete. Driver and service must both use v2; a v1/v2 mismatch is rejected during mapping. These contracts have offline concurrent coverage, not hardware acceptance.

Windows shared mode supplies a mixed stream. The driver cannot infer the original format of each contributing file. Exclusive mode can supply a supported player-selected stream format. Unsupported rate changes do not become a falsely labeled fixed-rate success.

Driver/service ABI 11 registers a referenced PCM data-ready event at the actual encoder block threshold. The consumer rechecks the ring before waiting and can be woken by PCM, format changes or stream/service cancellation. Committed format epochs survive coalesced A -> B -> A changes. The encoder no longer polls with Sleep(2); the virtual WaveRT position timer remains a separate software hardware-emulation mechanism.

Media completion waits preserve pending OVERLAPPED/buffer ownership on cancellation, and retirement has one shared realtime drain budget. Outgoing OPEN forwards the caller's original request, removing a separate request allocation and cancellation bridge. Final destruction still waits for the kernel to release outstanding I/O. Registry status publication runs on the existing session thread, outside the encoder's lock and realtime work. Optional ETW probes and their measurement limits are described in the [lifecycle review](LIFECYCLE-REVIEW.md).

Manager status publishers reconcile requests arriving during an external call after committing their cache, using the existing publication work item. That worker chooses its connection epoch inside PublicationLock, and rundown covers the final enqueue before unregister/flush. This preserves follow-up acknowledgement work; it does not make private MPM calls atomic with asynchronous policy changes.

The accepted suspend/resume cache retains its original connection epoch. Already-at-target callbacks and worker acknowledgements validate that cached epoch; a new link needs its own service reply. Epoch zero represents the disconnected startup baseline and cannot acknowledge an active link.

Kernel bus ABI 6 transfers prepared policy-wait IRPs to the parent for completion after unlocking; child callback lifetime remains protected during preparation. WAIT_FORMAT inserts without an outer spin lock and reconciles state afterwards. File-bound admission, cleanup and producer selection prevent old control handles from retaining waits or consuming a new service's events. These paths add no notification thread or periodic poll.

WaveRT position advancement commits PCM, packet count and completion notification in one locked path shared by the timer and queries. A query before a delayed timer does not leave stale packet numbers or require duplicate notification accounting. Buffer-mode checks and state changes use the same position lock as buffer detach; external stream reports run after unlocking. The 1 ms software timer and existing PCM catch-up/EOS bounds remain.

Integer PCM admission precedes conversion: 16/24-bit accepted samples are written directly into ring slots, with no scratch pass; rejected source frames are not accessed. Raw 32-bit containers remain unchanged. Overrun increments and observations use atomic core helpers. This removes work and intermediate storage without changing the mapped ring layout or codec precision policy; measured hardware performance is still pending.

Consumer preparation maps channels before final codec-precision clipping/quantization. The same int32 word buffer holds raw ring payloads (including float bits loaded via memcpy) and prepared S24 adapter values. Mono expansion proceeds backwards in place; equal-channel S24 at 24-bit precision needs no transform. Bounds and layout are checked before mutation. This replaces the separate float vector and layout pass without resampling or changing gain/codec policy.

LDAC encode calls consume 128 samples per channel. At 88.2/96 kHz, a codec frame represents 256 samples; RTP uses the codec-frame duration, not the API-call block size. AVDTP headset delay reports affect the WaveRT presentation position. The virtual endpoint reports zero local hardware FIFO/chipset/codec delay; remote Bluetooth delay must not be compared against the local cyclic buffer's size. Hardware video synchronization is still unvalidated.

## Connection and service lifecycle

- The kernel buffers channel notifications across service startup and gaps between waits. Overflow requests a state reset; it does not silently discard a transition and assume the link is healthy.
- Local channel closes are not echoed as remote disconnects, preventing an old close notification from tearing down a replacement connection.
- Signalling receive uses one persistent overlapped request on the protocol thread. Its buffer survives timeouts and is canceled/completed before closing a channel; the extra receive thread and PDU relay queue have been removed.
- Stream errors stop the native path and report failure. Reconnection discards stale PCM.
- KS jack properties and jack-change events expose the negotiated endpoint's connection state. Link teardown and service-handle cleanup mark it disconnected.
- Transport file cleanup releases orphaned channels after service exit. Removed-device cleanup is joined before SCM reports the service stopped.

These mechanisms have build/protocol evidence. Their combined behavior with the actual headset, Windows audio policy and Bluetooth radio remains a hardware acceptance item.
