# Read-only Bluetooth state inspection

Run from the repository in **Windows PowerShell 5.1**. No Settings window,
headset operation or administrator prompt is required:

```powershell
powershell.exe -NoProfile -File .\scripts\inspect-bluetooth-state.ps1
```

Use `-DeviceName 'Headset name'` for another paired Classic Bluetooth device.
Add `-IncludeOtherPaired` to compare connection flags; other device names are
redacted. The JSON contains no Bluetooth addresses, pairing keys or device IDs.
The script reads state only: it does not change radio power, pair/unpair,
connect/disconnect, restart processes/services, install drivers or start traces.

The report keeps these independent observations separate:

| Observation | Meaning |
|---|---|
| `BluetoothRadios[].State` | Windows radio power state. |
| `AssociationEndpoints[]` | Paired Classic Bluetooth AEP: enabled, paired, connected and present flags. |
| `DeviceContainers[]` | Local device-container enabled/connected state. |
| `NativeAudio` | Installed codec-service version/state, matching native status records and currently present audio endpoints. A running service alone is not an active headset session. |
| `Errors[]` | Failed/incomplete queries; missing values must not be interpreted as `False`. |

`NativeAudio.StatusLookup` distinguishes a completed status lookup from an
unresolved target/address. Errors include an operation, exception type and
HRESULT rather than messages that could disclose device IDs. Queries are
sequential observations, not an atomic snapshot; transitions need correlation
with the relevant Windows/driver events before assigning a cause.

Microsoft defines [AEP IsConnected](https://learn.microsoft.com/en-us/windows/win32/properties/props-system-devices-aep-isconnected)
as current connection state. [Device-information properties](https://learn.microsoft.com/en-us/windows/uwp/devices-sensors/device-information-properties)
also distinguish pairing, presence and device containers. Merely finding a
cached paired AEP does not establish presence or usable audio.

A local investigation on 2026-11-11 found Bluetooth Off, XM6 not present,
its device container disconnected, but its AEP Connected flag still True.
Three other paired Classic endpoints reported False. This is evidence of
inconsistent Windows state, not proof of the responsible publisher or a fix.
It was observed on runtime 0.3.44. Version 0.3.49 was subsequently activated
and its loaded images verified. Audio, controls, reconnect and performance
acceptance are still separate.

With radio On and a valid disconnected Bluetooth device record, a controlled
native -> Microsoft inbox A2DP -> native comparison on that system reported
AEP Connected=False but DeviceContainer Connected=True in all three phases.
The native package was restored and its real loaded images checked. Therefore
container Connected=True alone does not establish a custom-driver fault or
justify overwriting that property. The actual Settings label and its failed
per-device operation remain unresolved.

A render-package update also reproduced PnPUtil exit 259 while reporting that
the device already uses the up-to-date driver. The installer accepts this
specific result only after finding the present target child, comparing its
published OEM INF hash with the requested INF and classifying its device state.
Missing devices, another selected package and genuine install errors still
fail. This does not assert that a selected INF is the loaded kernel image.

Windows PowerShell may expose WinRT properties as a COM map. The script
enumerates key/value entries rather than treating failed dictionary projection
as an empty set of properties. It reports the queried values without attempting
to overwrite cached connection flags or guess original music-file formats.
