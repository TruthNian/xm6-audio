# Building XM6 Audio

Clone with `--recurse-submodules`, or run:

```sh
git submodule update --init --recursive
```

The gitlinks pin Sony/AOSP libldac, BlueZ libsbc and FindWDK. The default Windows build provides LDAC, SBC and Windows Media Foundation AAC. aptX dependencies are not downloaded (XM6 does not support aptX). The optional developer GUI is off by default.

## Windows user mode

Visual Studio 2019/2022 with C++ and Windows SDK:

```powershell
cmake -S . -B build/native -G "Visual Studio 17 2022" -A x64 -DXM6_BUILD_GUI=OFF
cmake --build build/native --config Release --parallel 2
ctest --test-dir build/native -C Release --output-on-failure
```

For VS2019 use `-G "Visual Studio 16 2019"`. The pinned libldac has two MSVC portability adaptations outside the submodule: an unused-parameter annotation and an otherwise unused POSIX include. No codec algorithm is changed.

Outputs include `oa2dpsvc.exe`, `oa2dpctl.exe`, `ldac.dll` and `sbc.dll`. Keep both replaceable codec DLLs alongside the service. User-mode CMake builds with MSVC; the older optional Zig script is retained for upstream development but is not the release pipeline.

## Kernel drivers and unsigned candidate

Install the matching WDK on a development machine, or use the Windows CI job:

```powershell
cmake -S . -B build/native -G "Visual Studio 17 2022" -A x64 -DOA2DP_BUILD_DRIVERS=ON
cmake --build build/native --config Release --parallel 2
ctest --test-dir build/native -C Release --output-on-failure
./scripts/package-native.ps1 -BuildDirectory build/native -OutputDirectory out/native
```

The packager requires InfVerif and Inf2Cat to pass; includes both SYS/INF/CAT/PDB sets, user-mode files and licenses; and records source/dependency commits and SHA-256 hashes. Its CAT files are **unsigned**. The manifest proves integrity relative to that build, not Microsoft trust or hardware compatibility. Symbols are included for the signing provider and crash analysis.

For a workspace-local WDK layout, set the process's `WDKContentRoot` before
CMake configuration and keep matching SDK shared headers with the WDK kernel
headers and libraries. The packager accepts `-WdkToolsRoot <kit-root>` to locate
`bin/<version>/x86/Inf2Cat.exe` and `Tools/<version>/x64/infverif.exe` there;
without this option it uses the installed Windows Kits directory. This also
supports build files extracted from Microsoft's [WDK NuGet distribution](https://learn.microsoft.com/en-us/windows-hardware/drivers/install-the-wdk-using-nuget),
without running a system-wide WDK installer. Tool selection does not sign or
deploy the package.

In the 0.3 package layout, `drivers/oa2dp/` contains the transport SYS/INF/CAT plus `oa2dpsvc.exe`, `oa2dpctl.exe`, `ldac.dll` and `sbc.dll`. All loadable transport/service files are included in that INF's CopyFiles/catalog payload. There is no separate `user-mode/` installation directory. The INF registers a demand-start Win32 service with a device-interface trigger, running from Driver Store. User-mode targets link their MSVC runtime statically; no separate VC runtime installer is required.

The two drivers and service must come from the same build. The shared IOCTL
and kernel-bus contracts carry explicit versions and reject mismatches. PCM
metadata distinguishes integer/float, container width and valid bits,
including integer 32 and 24-in-32 PCM. Kernel compile success does not establish
hardware correctness.

## Deployment and recovery

Follow [docs/SIGNING.md](docs/SIGNING.md). A Microsoft-signed returned package is a prerequisite. `scripts/install.ps1` checks the signature before making changes and binds only the selected XM6 A2DP service; it does not change the radio driver or pairing. It is a deployment prototype and has not yet been exercised on hardware.

The previous development installer that created trusted test certificates and requested relaxed boot protection has been removed. Never run a test-signed package on the listening machine.

## Portable protocol checks

```sh
cmake -S . -B build/core
cmake --build build/core
ctest --test-dir build/core --output-on-failure
```

The Windows encoder check runs the actual pinned library and verifies represented frame durations and HQ frame sizes at 44.1, 48, 88.2 and 96 kHz. At 88.2/96 kHz an LDAC codec frame represents 256 samples although an API encode call still consumes 128; RTP uses codec-frame duration. Initial encoder buffering is excluded from the bitrate calculation. This check neither transmits Bluetooth audio nor proves the headset is audible.
