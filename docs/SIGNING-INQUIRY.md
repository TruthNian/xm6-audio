# Free-signing feasibility inquiry — DRAFT, NOT SENT

Proposed channel: a public issue in the Foundation's own [SignPath/fdn-website](https://github.com/SignPath/fdn-website) repository, where driver eligibility questions are already being raised. Publishing requires the user's explicit authorization. This is a feasibility question, not an application or binary submission. No private hardware information, account credentials, licenses or signing material is included.

Subject: Free Microsoft kernel-attestation route for an open-source Windows Bluetooth audio project?

Hello,

We are developing XM6 Audio, an open-source native Windows Bluetooth audio implementation: https://github.com/TruthNian/xm6-audio . It uses a KMDF A2DP profile driver and a PortCls/WaveRT audio driver, preserving Microsoft's BTHUSB/BTHPORT radio stack. User-mode LDAC encoding uses Sony/AOSP libldac. The source, pinned dependencies and public CI are available for review.

Our code is based on the MIT-licensed RafiMota/open-a2dp project. The audio sample retains Microsoft's MS-PL attribution; libldac is Apache-2.0 and the replaceable BlueZ libsbc DLL is LGPL-2.1-or-later. This is a new project based on an upstream that does not currently publish production-signed drivers. We understand that your reputation and fork/upstream-signing conditions may prevent eligibility. We have not claimed approval or advertised your organization as our signing provider.

Does any free Foundation-supported process cover Microsoft Hardware Dev Center attestation for these two kernel driver packages, rather than only ordinary Authenticode signing? If not, is there an established free route through collaboration with the upstream maintainers that you can point us to?

The end user has no EV certificate or Hardware Developer account and is not authorizing new paid services. We intend to retain Secure Boot and normal driver signature enforcement. The current binaries are unsigned development candidates and have not been installed on that protected host; hardware acceptance is still pending.

We are asking about feasibility and the official prerequisites before applying or uploading any artifact. Thank you.
