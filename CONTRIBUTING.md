# Contributing

Thanks for helping! A few rules keep the project legally clean and easy to
review.

## Clean room

Read [docs/CLEAN_ROOM.md](docs/CLEAN_ROOM.md). In short: never disassemble or
unpack proprietary A2DP drivers or their installers, and cite the public
source (spec section, Microsoft doc, open-source file) for protocol or codec
behaviour you implement. PRs without a source for new protocol behaviour will
be asked for one.

## Workflow

1. Open an issue for anything bigger than a bug fix, referencing the SPEC ID
   (e.g. "F4.3").
2. Keep `core/` portable: no Windows headers, no allocation, C99.
3. Add focused regression checks for changed protocol/PCM behavior and run the
   relevant CMake checks from [BUILDING.md](BUILDING.md). Documentation changes
   need link/content checks, not repeated encoder or hardware tests.
4. Kernel code: describe build/analysis results and separate them from actual
   headset testing. State explicitly when hardware was not tested. Run driver
   diagnostics only on an appropriately configured development system.
5. Update [docs/ROADMAP.md](docs/ROADMAP.md) when acceptance evidence changes.
   The inherited SPEC is a reference, not a current hardware support claim.

## Style

Match the surrounding code: 4-space indent, `oa2dp_` prefix in C, Windows
naming (`Oa2dp...`) in drivers, `snake_case` in the service. Format with
`clang-format` defaults close to the existing files.

## Testing with headsets

Report results in a GitHub issue: model, codecs
offered (`oa2dpsvc --console` logs them), codec chosen, any dropouts, and the
`oa2dpctl status` output.

Review logs before posting: remove device addresses, pairing data, licenses
and unrelated personal information. See [privacy policy](docs/PRIVACY.md).
