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

Windows shared mode supplies a mixed stream. The driver cannot infer the original format of each contributing file. Exclusive mode can supply a supported player-selected stream format. Unsupported rate changes do not become a falsely labeled fixed-rate success.

LDAC encode calls consume 128 samples per channel. At 88.2/96 kHz, a codec frame represents 256 samples; RTP uses the codec-frame duration, not the API-call block size. AVDTP headset delay reports are forwarded to the WaveRT latency/presentation interfaces. Hardware synchronization is still unvalidated.

## Connection and service lifecycle

- The kernel buffers channel notifications across service startup and gaps between waits. Overflow requests a state reset; it does not silently discard a transition and assume the link is healthy.
- Local channel closes are not echoed as remote disconnects, preventing an old close notification from tearing down a replacement connection.
- Signalling receive uses one persistent overlapped request on the protocol thread. Its buffer survives timeouts and is canceled/completed before closing a channel; the extra receive thread and PDU relay queue have been removed.
- Stream errors stop the native path and report failure. Reconnection discards stale PCM.
- KS jack properties and jack-change events expose the negotiated endpoint's connection state. Link teardown and service-handle cleanup mark it disconnected.
- Transport file cleanup releases orphaned channels after service exit. Removed-device cleanup is joined before SCM reports the service stopped.

These mechanisms have build/protocol evidence. Their combined behavior with the actual headset, Windows audio policy and Bluetooth radio remains a hardware acceptance item.
