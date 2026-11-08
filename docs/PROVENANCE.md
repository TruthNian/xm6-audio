# Source provenance

XM6 Audio is a derivative project, not an independently invented Bluetooth stack. Original author and third-party notices remain in the source.

## Native source baseline

- Upstream: [RafiMota/open-a2dp](https://github.com/RafiMota/open-a2dp).
- Pinned upstream commit: `167ebc0e2c89fec570eec535fefdade72bd9e416`.
- Initial native import in this repository: `20744fae`.
- Both baseline trees: `5cd2c0155f4df142705c477b18731be42d85518a`.

The matching Git tree IDs establish that the initial native import preserved the upstream tree exactly. The import is linked to this repository's earlier history; GitHub does not display it as a fork of the native upstream. Subsequent changes are visible as commits after the import. This distinction matters when a signing provider evaluates fork eligibility.

## Dependencies

| Dependency | Pinned commit | License |
|---|---|---|
| Sony/AOSP libldac | `eeee1a3f5f8df1282e3a6d297085885fd886737b` | Apache-2.0 |
| BlueZ libsbc | `b3deb8a5dcfb42d8c10ba1f2f1ac9bd7bf7271cc` | LGPL-2.1-or-later |
| FindWDK | `068618358d27e9e7ea528cdca3091d7c180be6e9` | BSD-3-Clause |

The WaveRT-derived files retain their SYSVAD attribution and separate MS-PL license. Media Foundation AAC is supplied by Windows, not redistributed. [THIRD_PARTY.md](../THIRD_PARTY.md) documents optional inherited adapters and distribution obligations.

## Build traceability

The repository's CI builds checked-out source with the recorded submodule revisions. Packaging records source/dependency commits and SHA-256 hashes, includes symbols and license notices, and fails on INF/catalog-generation errors. CI artifacts remain unsigned until a legitimate signing process is available; the manifest alone is not publisher authentication.

The historical radio-takeover bridge remains available at `v0.1.0-bridge` and is excluded from the current native audio path. See [clean-room rules](CLEAN_ROOM.md) for accepted implementation sources.
