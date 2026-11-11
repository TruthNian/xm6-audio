# Codec capability and fallback review

Reviewed 2026-11-11 against source 0.3.53. The headset was unavailable. The
checks below use production protocol/selection/advertisement code and the
actual installed Windows AAC MFT, without opening an audio device or Bluetooth
connection. They do not replace connected playback acceptance.

## Corrected contracts

**AAC target selection must agree with the encoder.** The bundled Microsoft
AAC backend accepts CBR targets of 96, 128, 160 and 192 kbit/s for mono/stereo,
with 16-bit PCM at 44.1 or 48 kHz. These are the documented
[Microsoft AAC encoder limits](https://learn.microsoft.com/en-us/windows/win32/medfound/aac-encoder).
The old adapter initialized its target to 96 kbit/s even when the negotiated
cap was lower. A real-MFT regression restored the old production method and
observed a 64 kbit/s limit incorrectly accepted at a 96 kbit/s output type.

One shared selection function now chooses the highest supported target within
the peer and user limits. An unspecified limit permits the backend's highest
supported target. No target below the minimum is invented. Outgoing negotiation,
incoming configuration validation, user policy and the actual MFT media-type
setup use this policy. A 170 kbit/s cap selects 160 kbit/s; a 64 kbit/s cap is
unsupported. Automatic mode may try the next permitted codec; AAC-only does
not silently become SBC. Nominal codec targets exclude LATM/RTP/transport
overhead and do not describe instantaneous packet throughput.

**A linked adapter is not proof of an available OS backend.** AAC availability
now checks COM/MF startup and activation before publishing its SEP. An absent
or unusable backend is omitted, so it cannot repeatedly win negotiation merely
because an adapter was compiled. Startup/cleanup is shared with encoder
construction. Every owned COM/MF reference is balanced, including S_FALSE,
startup/activation failure and an already-initialized different apartment.
There is no permanent negative cache or new worker. A future resource/allocation
failure after a successful probe remains a streaming failure; this probe does
not promise that every later encoder creation will succeed.

**Explicit policy also constrains the candidate list.** The generic best-codec
helper previously returned a preferred configuration without enforcing fixed
rate/precision requirements. Service advertisement could likewise publish
AAC/SBC despite a fixed 96 kHz or 24-bit requirement that neither could satisfy.
The helper now checks the explicit policy, and advertisement excludes such
codecs before probing their backends. Automatic endpoint-rate preference remains
distinct from a fixed user requirement. The normal LDAC HQ preference and
Windows shared-mix behavior are unchanged.

## Evidence and boundaries

- Portable protocol tests cover supported-rate boundaries, unspecified limits,
  peer/user intersections, sub-minimum rejection, permitted SBC fallback,
  AAC-only failure and fixed-rate/precision constraints. Restoring the old
  selectors fails the new assertions.
- Production COM/MF startup, cleanup, availability dispatch and service SEP
  advertisement execute against controlled OS boundaries. Cases include missing
  activation, startup failure, healthy recovery, balanced reference lifetimes,
  no disallowed-codec fallback and no initialization of a policy-excluded backend.
  The old compiled-is-available dispatch fails these checks.
- The production media-type setup executes against the installed Windows MFT:
  36 valid type/limit combinations and 8 rejected limits across mono/stereo and
  44.1/48 kHz. Current output attributes are read from the actual transform.
  Constructor checks reject peer/user limits below the minimum. Additional
  availability probes while a real AAC encoder is alive must not break encoding.
  A genuinely absent OS backend is reported as a CTest skip, not an executed
  success; the developer host's real MFT checks executed successfully.
- Actual encoder smoke checks and the existing transport/format/lifecycle
  checks remain required. Missing-MF behavior uses controlled failures; no
  Windows feature pack or system component was removed for testing.

No audible AAC/SBC fallback, real codec transition, RF stability, Chrome-stutter
resolution or measured performance gain is established by these checks.

The subsequent [0.3.54 output-lifetime review](PERFORMANCE-REVIEW.md) covers
events-only output, NO_SAMPLE and cleanup independently of capability probing.

## Local activation

Source `42f0670618b9771208b0626fe53a554bfc3ca885` passed all 23 local checks
and [source CI](https://github.com/TruthNian/xm6-audio/actions/runs/42988148471).
The locally built version53 package was signed with the existing development
certificate and activated through a radio cycle. Both actual kernel images
and the running service matched its signed payload hashes. No computer restart
was performed or pending. Paired/disconnected state remained consistent, with
all 305 display events decoded and zero trace loss. The expected Paired resource
text remains distinct from observing a rendered Settings window. Existing auto
PCM, LDAC HQ, ABR-off and permitted LDAC/AAC/SBC policy were verified unchanged.
