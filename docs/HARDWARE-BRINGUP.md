# Native hardware bring-up

The controlled developer trial targets WH-1000XM6 on Windows 10 x64, build 19045. It uses a local development signature under explicitly authorized test signing. This is separate from the production installer, which still requires Microsoft-signed catalogs.

Current runtime is **0.3.63**, with the loaded kernel images and running service
verified against the local signed package. Native LDAC input and complete PCM
format epochs are deployed; Core Audio's displayed endpoint name is unresolved.
The user's acceptance of **Paired while disconnected** and correct Settings
connect/disconnect display belongs to 0.3.61 and remains recorded below.
Passive Chrome/QQ Music and PotPlayer observations were also collected on
0.3.61; a short post-activation trace was collected on 0.3.62. An earlier
interruption remains unexplained. See the
[timing evidence and limits](PERFORMANCE-REVIEW.md).
Earlier runtime observations apply to 0.3.44. After its authorized
deployment and a user restart, both actual loaded kernel images and the running
service matched the verified local test-signed candidate. The service process
started approximately twelve seconds after boot without a manual start in the
post-boot session. This establishes automatic service startup, not complete
automatic audio/reconnect acceptance.

## 0.3.63 activation: PCM precision fixes (2026-11-11)

Source `d3c62280751864b8b0375546cddad68cf36b24d7` passed all 29 local checks
after the complete native Release build, and passed
[Linux sanitizer and Windows native CI](https://github.com/TruthNian/xm6-audio/actions/runs/43035576755).
The CI artifact digest, manifest and every payload hash were verified before
signing with the existing local development certificate. The two loaded kernel
images and running service match this signed package. Activation required one
Bluetooth off/on cycle; no PC restart, boot-policy or certificate-trust change
was made. Current transport/render restart flags are clear.

The service selected native int32 LDAC input at 96 kHz stereo. The endpoint
supplies 24 valid integer bits in a 32-bit container; this is not evidence of
32-bit wireless precision or added source detail. Automatic PCM/codec policy,
LDAC HQ and ABR-off were read back unchanged. The later state snapshot was
suspended, with existing applications paused. No test audio or sustained trace
was started; this activation has no new audible acceptance or quality claim.

All three default render roles still select native XM6. The shared mix, master
volume, mute and capture defaults were preserved. **The MMDevice endpoint ID
changed again**, so identity continuity remains a failure; its cause is not yet
isolated. The Core Audio name remains `XM6 Audio Topology`. The earlier 0.3.61
Settings display acceptance is retained as historical user evidence, not a
new 0.3.63 UI test. These limitations and the actual loaded identities are
recorded in the private activation receipt. See the
[precision changes and validation limits](PERFORMANCE-REVIEW.md).

## 0.3.62 activation, endpoint migration and partial acceptance (2026-11-11)

Source `292a0c98b07b545c8c8b377eeef307056ab0b5fb` passed
[exact-source Linux/Windows CI](https://github.com/TruthNian/xm6-audio/actions/runs/43018884143).
The same-code local native Release build and all 29 checks passed; after the
version-only bump, the package was rebuilt and native metadata checked again.
Two SYS and two CAT signatures and all eight catalog members verified using
the existing development certificate. Deployment began during a sampled idle
interval. One Bluetooth off/on cycle activated both kernel images; their real
loaded paths/hashes and the running service match the protected signed package.
Current transport/render restart flags are clear. No PC reboot, boot-policy or
certificate-trust change was made. The prior installation restart request is
retained as historical evidence, separate from these activation observations.

Automatic rate/precision, LDAC/AAC/SBC, LDAC HQ and ABR-off policy was read back.
Signalling and media connected automatically after activation, with 96 kHz LDAC.
All three render defaults still resolve to native XM6, and volume, mute and
96 kHz shared mix were preserved. Capture defaults remained unchanged.
However, the MMDevice endpoint ID changed. The original continuity assertion
failed with `Original endpoint identity is absent`; this is not a continuity
pass. The before/after IDs and failure are preserved privately. Installation
migration versus name-publication causation has not been isolated.

The render PDO and all five interface properties now report `WH-1000XM6`;
the five legacy interface FriendlyName values agree. Core Audio nevertheless
returns `XM6 Audio Topology` as its interface name and the old localized
endpoint display name. **Display-name acceptance remains false.** No private
MMDevice property overwrite or endpoint recreation was used to hide it.
See [name diagnostics](DIAGNOSTICS.md#endpoint-name-publication-0362-partially-effective).

The existing PotPlayer process acquired a session on the new native endpoint
without a player restart. A later snapshot contained nonzero session/endpoint
signal. A 20.392-second trace recorded 3,823 successful media completions, zero
trace loss and zero overrun after the first PCM window. That trace did not
sample application identity concurrently and does not establish audible output.
No new service error was observed between activation and these checks.

The protected activation receipt records these successes and failures without
marking complete hardware acceptance. Audible acceptance of this package,
simultaneous applications, supported rate/codec streaming transitions,
sleep/resume and physical lip sync still need separate evidence. The earlier
0.3.61 Settings acceptance is not relabeled as a fresh 0.3.62 UI test.

## 0.3.61 activation and connected state acceptance (2026-11-11)

Source `b378045332f704af05f07abc12c44772f9f985a1` passed all 28 local checks
and [Linux/Windows source CI](https://github.com/TruthNian/xm6-audio/actions/runs/43011913970).
The local WDK/MSVC package was signed with the existing test certificate and
activated through a Bluetooth radio cycle. Both actual loaded kernel images
and the running service match the protected signed package; current restart
flags are clear. No computer reboot, boot-policy or certificate-trust changes
were made. Automatic PCM, LDAC/AAC/SBC, LDAC HQ and ABR-off policy was read back.

The planned offline check was not applicable: the headset had become reachable
and the service was actually submitting 96 kHz LDAC HQ audio. Its rejected
offline assertion was preserved, rather than reported as an offline regression
or silently counted as a successful absent-headset test.

One standard Bluetooth-audio KS disconnect followed by the address-scoped
Windows whole-device disconnect produced Paired=True/Connected=False,
container Connected=False and native user-disconnected/zero-bitrate state.
The KS call succeeded in 78.627 ms; this is one call's elapsed time, not a
universal physical-disconnect deadline. Reconnect was accepted, and media
submission resumed at 96 kHz, stereo, 24 useful bits in a 32-bit integer input
container, with LDAC HQ policy. Reported submission rate was about 1.01 Mbit/s
including protocol overhead, not a separate LDAC quality level. No player or
service restart occurred during this control cycle.

All 38 display-model trace events decoded with zero loss. The target changed
from Connected(7) to Disconnected(3), then back to Connected(7), agreeing with
the sampled AEP/container/service states. The user independently confirmed
that the current Windows UI shows **Paired when unconnected**, and correctly
updates after connection and disconnection. This Settings-state requirement
is now accepted on this host. Computer Use accessibility returned null and
window capture timed out twice; the visual acceptance source is the user.

This run does not establish a worst-case cancellation time, audible playback
after reconnect, long-term Chrome stability, sleep/resume, all PCM transitions
or physical audio/video synchronization. The control-wait defect was proven
with production-body fixtures, but not established as the sole cause of the
prior UI error. A preceding unconnected 0.3.60 control call already succeeded
in about 30 ms, so these separate calls do not demonstrate a latency speedup.

## 0.3.60 presentation-clock correction activation (2026-11-11)

Source `a3abe1f8b6e18db1e755704389d283fa27fd8271` passed all 28 local checks
and [Linux/Windows source CI](https://github.com/TruthNian/xm6-audio/actions/runs/43008980426).
The actual WaveRT presentation body reproduced a 2640 -> 960 frame backward
step at 48 kHz under controlled delay updates. The corrected body covers four
sample rates, pause-time commit/freeze, resume/reset and raw QPC units at three
frequencies. This is not a measured backward step in a player on this host.

The locally built candidate was signed with the existing test certificate and
activated through one Bluetooth radio cycle. Both actual loaded kernel images
and the running service match the protected signed package. No computer restart,
boot/trust or privacy changes were made. Current transport/render install flags
request no restart. Automatic PCM rate/precision, LDAC/AAC/SBC, LDAC HQ and ABR
off were read back from the running installation.

All 731 offline display-model events decoded with zero event/buffer loss.
The last target state was Disconnected(3) at approximately 5529 ms, followed
by another 5005 ms of observation. AEP Paired=True/Connected=False, container
Connected=False and native disconnected/zero-bitrate state agreed. The expected
English label remains **Paired**, with rendered text/buttons unobserved.
Physical audio/video synchronization, real headset reconnect, playback and
Chrome stability remain unaccepted; estimated presentation position is not
an observation of receiver output.

## 0.3.59 complete RTP tail correction activation (2026-11-11)

Source `7dda97d4b9d247b7aa03cd830518b78fdd68993c` passed all 28 local checks
and [Linux/Windows source CI](https://github.com/TruthNian/xm6-audio/actions/runs/43007583981).
The previous service PCM-wait branch withheld a complete short SBC tail until
another encoder output arrived. The expanded production-body fixture now
verifies submission before waiting, packet clocks, empty wakeups, unresolved
format state and send/wait failure. Actual LDAC encoder/RTP checks remain green.

The locally built candidate was signed using the existing test certificate
and activated through one Bluetooth radio cycle. Both actual loaded kernel
images and the running service match the protected signed payload. No computer
restart, boot/trust or privacy change was made. Current transport/render flags
request no restart. Automatic PCM rate/precision, LDAC/AAC/SBC, LDAC HQ and ABR
off were read back from the running installation.

All 623 offline display-model events decoded with zero event/buffer loss.
The last target state was Disconnected(3) at approximately 5842 ms, followed
by another 4995 ms of observation. AEP Paired=True/Connected=False, container
Connected=False and native disconnected/zero-bitrate state agreed. The expected
English label is **Paired**; rendered text/buttons remain unobserved. No
connected SBC tail, radio packet efficiency, headset rendering or Chrome
stability was measured. Hardware acceptance remains incomplete.

## 0.3.58 failed power-query recovery activation (2026-11-11)

Source `d08fb0f163c7f5a9e4bc0ba68e7222358960eae3` passed all 28 local checks
and [Linux/Windows source CI](https://github.com/TruthNian/xm6-audio/actions/runs/43006059101).
The previous production refresh body loses retry intent on a failed GET_INFO;
the new fixture verifies latest-state recovery without another notification
or a ready audio endpoint. Queries and time are controlled, so this is not a
physical sleep/resume result or a measured host recovery deadline.

The locally built candidate was signed with the existing test certificate and
activated through one Bluetooth radio cycle. Both actual kernel images and
the running service match the protected signed package. No computer restart,
boot/trust or privacy changes were made. Current transport/render install flags
request no restart. Automatic PCM rate/precision, LDAC/AAC/SBC, LDAC HQ and ABR
off were read back from the running installation.

All 570 offline display-model events decoded with zero event/buffer loss.
The last target state was Disconnected(3) at approximately 5859 ms, with
another 5007 ms of observation. AEP Paired=True/Connected=False, container
Connected=False and native disconnected/zero-bitrate state agreed. The expected
English label remains **Paired**. Rendered text/buttons, physical sleep/resume,
headset recovery/playback and Chrome stability remain unaccepted. The offline
activation does not establish the cause or resolution of those symptoms.

## 0.3.57 startup discovery correction activation (2026-11-11)

Source `1d036f987462178368477ec6797a4851432cb179` passed all 27 local checks
and [Linux/Windows source CI](https://github.com/TruthNian/xm6-audio/actions/runs/43005001589).
The actual DeviceManager fixture reproduces stale startup enumeration
recreating a removed interface and verifies the corrected event orderings.
This is not evidence that it caused the earlier rendered Settings symptom.

The candidate was signed using the existing local test certificate and
activated through one Bluetooth radio cycle. Both actual loaded kernel images
and the running service match the protected signed payload. No computer
restart, boot/trust or privacy change was made. Current transport/render
install flags request no restart. Automatic PCM rate/precision, LDAC/AAC/SBC,
LDAC HQ and ABR off were read back from the running installation.

All 557 offline display-model events decoded with zero event/buffer loss.
The last target state was Disconnected(3) at approximately 5659 ms, followed
by another 4993 ms of observation. AEP Paired=True/Connected=False, container
Connected=False and native disconnected/zero-bitrate state agreed. Installed
Windows resources map this disconnected state to the English label **Paired**.
An approximately one-second transient Connected remains; the retained inbox
driver trace has the same transient, so it alone does not establish a native
driver fault. The final state must still settle to paired, not connected.

The Settings window could be listed but accessibility text was empty and
window capture timed out twice after reselecting it. No rendered label or
button was observed and no Settings click was performed. Rendered UI,
connected headset recovery, playback and Chrome stability remain unaccepted.

## 0.3.56 control-I/O stop correction activation (2026-11-11)

Source `51d9b814346255dce0b08bb722b2845fde7e12f5` passed all 26 local checks
and [Linux/Windows source CI](https://github.com/TruthNian/xm6-audio/actions/runs/43003121997).
The production call-chain fixture reproduces the old stop-before-submit gap
and verifies stop during submission, completion ownership and normal output.
Its simulated timeout is not a measurement of actual host shutdown speed.

The locally built candidate was signed with the existing test certificate
and activated through one Bluetooth radio cycle, without a computer restart
or boot/trust/privacy change. Both actual loaded kernel images and the running
service match the protected signed package. Current install flags requested
no restart. Auto PCM format, LDAC/AAC/SBC, LDAC HQ and ABR off were read back.

All 464 offline display events decoded with zero event/buffer loss. The last
target state was Disconnected(3) at approximately 5471 ms, followed by another
4986 ms of observation. AEP Paired=True/Connected=False, container Connected=False
and native disconnected/zero-bitrate state agreed. The corresponding installed
English resource is **Paired**. Rendered text/buttons, real connected recovery,
headset playback, shutdown timing under load and Chrome stability remain
unaccepted; no audio-performance result follows from this offline activation.

## 0.3.55 incoming-OPEN failure recovery activation (2026-11-11)

Source `3f99f4897acab6f498db2130e2dae6a9766d8fc8` passed all 25 local checks
and [Linux/Windows source CI](https://github.com/TruthNian/xm6-audio/actions/runs/43000936449).
Controlled production bodies reproduce the old missing attempt binding,
failure notification and service event-consumption paths. Coverage includes
replacement generations and a failed remaining-channel CLOSE followed by the
existing teardown retry. No connected headset was used for these checks.

The locally built package was signed using the existing development certificate
and activated through one Bluetooth radio cycle. Both actual loaded kernel
images and the running service executable match the protected signed payload.
No computer restart, boot/trust or privacy change was made. Current transport
and render install flags requested no restart. The actual settings retain
automatic PCM sample rate/precision, LDAC/AAC/SBC, LDAC HQ and ABR off.

The offline trace decoded all 411 events with zero event/buffer loss. Target
state was transiently Connected(7) around 4463 ms, then Disconnected(3) around
5458/5473 ms, followed by another 4994 ms of settled observation. The retained
Windows inbox trace also has a roughly one-second transient Connected interval;
this alone is not attributed to the native driver. AEP Paired=True/Connected=False,
container Connected=False and native disconnected/zero-bitrate state agreed.
The corresponding installed English label is **Paired**, but rendered text and
buttons were not observed. Real per-device reconnect, existing-client recovery,
Chrome audio stability and current-version headset sound remain unaccepted.

## 0.3.54 PCM/output correction activation (2026-11-11)

Source `a582b7d39e88b39ab176b1b474726ffba9657303` passed all 25 local checks
and [source CI](https://github.com/TruthNian/xm6-audio/actions/runs/42996976885).
The locally built package was signed with the existing development certificate.
A Bluetooth radio cycle activated both actual kernel images and the running
service, verified against the protected signed package hashes. No computer
restart or boot/trust/privacy change was made. Both current install flags
requested no restart. Auto sample rate/precision, LDAC HQ, ABR off and the
LDAC/AAC/SBC preference were read back unchanged.

All 359 display-trace events decoded, with zero event/buffer loss. The last
target transition was Disconnected(3) at approximately 5392 ms, followed by
approximately 4997 ms of settled observation. AEP Paired=True/Connected=False,
container Connected=False and native disconnected/zero-bitrate state agreed.
The installed English resource for this state is **Paired**; the enum is not
the literal screen label. Rendered Settings text/buttons were not observed.

The same trace records transient Connected(7) at approximately 4383/4398 ms
before settling to Disconnected(3) at approximately 5392 ms. This roughly
one-second startup transition remains an open observation; this trace does
not attribute it to a particular driver/Windows component or establish its
visible duration. Do not describe the entire display lifecycle as fixed.
Connected per-device reconnect, existing-client recovery, Chrome audio
stability and actual headset playback remain unaccepted for version54.

## 0.3.53 codec/backend correction activation (2026-11-11)

Source `42f0670618b9771208b0626fe53a554bfc3ca885` passed all 23 local checks,
including 36 actual Windows AAC media types, 8 rejected limits and encoding
while additional backend probes run. Restoring the old production setup
accepted a 64 kbit/s cap at an actual 96 kbit/s output type. Controlled OS
failures separately cover unavailable backends and balanced resource cleanup.
[Source CI](https://github.com/TruthNian/xm6-audio/actions/runs/42988148471) passed.

The locally built candidate was signed using the existing certificate and
activated through a Bluetooth radio cycle, without rebooting the computer or
changing boot/trust/privacy policy. Both actual loaded kernel images and the
running service matched the signed version53 payload hashes. Current install
flags requested no restart. Auto sample rate/precision, LDAC HQ, ABR off and
the LDAC/AAC/SBC preference were read from the real current settings.

All 305 display-trace events decoded with zero event/buffer loss. The final
target state was Disconnected(3), followed by approximately 5007 ms of settled
observation. AEP Paired=True/Connected=False, container Connected=False and
native disconnected state agreed. The expected English label is Paired;
rendered UI/buttons, real reconnect and audio stability remain unaccepted.

## 0.3.52 activation and label-resource clarification (2026-11-11)

Source `ca3761525ff28a6403b2afbb41918b6a63377dd3` adds failed-rate-change
recovery. All 21 local checks and [source CI](https://github.com/TruthNian/xm6-audio/actions/runs/42982362029)
passed. The locally built package was signed using the existing certificate
and activated through a radio cycle; no computer reboot, boot/trust or privacy
change was made. Both actual kernel images and the running service matched
the signed version52 payload hashes. Current transport/render install flags
requested no restart.

All 267 display-trace events decoded with zero event/buffer loss. The final
target transition was Disconnected(3) at approximately 5658 ms, with another
5001 ms of observation afterward. AEP Paired=True/Connected=False, container
Connected=False and the disconnected native session agreed. This enum is not
literal UI text: the expected English label is **Paired**. SDK MakePRI read
the installed Windows EN-US resources for both
`SystemSettings_Device_Bluetooth_Disconnected` and `DisconnectedAudio`; both
have value `Paired`. This verifies resource text, not the rendered window or
button behavior. No new connected headset acceptance is claimed.

## 0.3.50/0.3.51 offline-state acceptance (2026-11-11)

The actual Windows DevicesFlowUI trace supplied a reproducible disconnected
case without a headset: native49's final state was Connected(7), inbox's final
state was Disconnected(3), and native49 restoration reproduced Connected(7).
The common transient Connected during enumeration did not distinguish them.

Version50's PnP wireless-state fix was built, locally test-signed with the
existing certificate and activated by cycling Bluetooth. Both actual kernel
images matched the signed package; the running service was version50. Its
final display-model transition was Disconnected(3), at approximately 5429 ms.
Trace EventsLost and BuffersLost were zero.

Source `44286f759444c54155383cb5c845da18b57d3822` adds the remaining version51
event/transport fixes. All 21 local checks, metadata/INF/catalog validation and
[Linux/Windows source CI](https://github.com/TruthNian/xm6-audio/actions/runs/42979844718)
passed. The locally built package was signed with the same existing test
certificate, staged and activated through a radio cycle, without rebooting
the computer or changing boot policy, certificate trust or radio privacy.
The actual loaded transport/audio images and running service executable
matched the signed version51 package hashes. Current transport/render install
flags requested no restart.

The version51 trace decoded all 305 events and lost no events/buffers. Its final
target transitions at approximately 5166 ms were Disconnected(3). A subsequent
read-only report agreed: Bluetooth On, paired AEP Connected=False, container
Connected=False, native session disconnected with zero bitrate, no query
errors. Protected activation state records this success without marking
headset acceptance complete. No connected playback, per-device reconnect,
Chrome-stability or performance conclusion follows from this offline test.

## 0.3.49 activation and controlled state comparison (2026-11-11)

Source `541917cbed05d8fc706888bf685d5c049a3dbbb1` was locally built and
test-signed with the existing development certificate. All 19 local checks,
INF validation, catalog-member and package-hash checks passed. Both driver
images and the service match that protected package. Initial render update
requested a restart; a Bluetooth radio cycle unloaded the old image and
activated both 0.3.49 drivers without a computer restart. The service started
through its existing device-interface trigger. Current node/install checks
showed no pending restart. No boot policy, certificate trust or radio privacy
setting was changed. This is a local test deployment, not Microsoft signing.

A valid Bluetooth API record reported the headset disconnected. A controlled
native -> Microsoft inbox A2DP -> native comparison retained pairing and found
AEP Connected=False, DeviceContainer Connected=True in every phase. The
restored native image hashes were checked. Container state alone therefore
does not identify a custom-driver defect on its own. A subsequent bounded
DevicesFlowUI ETW comparison resolved the settling order: native49 finally
reported Connected(7), inbox finally remained Disconnected(3), and restoring
native49 reproduced Connected(7), all while the Bluetooth API reported no
connection. No events or buffers were lost. This independently reproduces the
offline display defect without a headset. See the 0.3.50 source correction in
[the lifecycle review](LIFECYCLE-REVIEW.md). Actual per-device reconnect remains
unaccepted; the trace observes Windows' display model, not a rendered screenshot.

PnPUtil also returned 259 for an up-to-date render binding. The updated
installer procedure was exercised against that actual unchanged binding:
259 was accepted only after checking the present device, exact published INF
hash and started/pending state; no restart was required. Fake-API cases cover
missing/wrong/unsafe INF, failed start and genuine install errors. This fixes
an erroneous installation failure path, not the Bluetooth label problem.

There is no new audible, control, format-transition or Chrome-stability
acceptance for 0.3.49. Earlier user confirmations remain scoped to their
recorded versions.

## 0.3.44 post-restart user acceptance (2026-11-11)

Runtime source: `f9e919799effe6d61d17ced0bbf74be8370a58a8`, with successful
[CI](https://github.com/TruthNian/xm6-audio/actions/runs/42851798203).
The deployed binaries were locally built from that source and locally
test-signed; they are not claimed to be the CI artifact or production-signed.

- The user confirmed QQ Music, PotPlayer, system notification sounds and
  Bluetooth controls without observed problems in this round.
- YouTube in Chrome exhibited intermittent audio stutter even with the video
  buffered. This is unresolved; video buffering alone does not locate an
  audio-track, decoder, Windows-render or Bluetooth scheduling fault.
- Windows Settings still falsely displays Connected before a real connection
  and reports a failed disconnect while the physical link has disconnected.
  Per-device disconnect/reconnect remains failed acceptance.
- A read-only snapshot while QQ Music was active found the native endpoint
  default for all three render roles, with a 96 kHz stereo 32-bit float mix.
  The service reported LDAC HQ, 24-bit encoder input and approximately
  1.006 Mbit/s media transport including overhead. These are endpoint/transport
  observations, not the song's original format or codec payload bitrate.
- A later bounded timing trace contained no active PCM/media events while the
  service reported suspended. It supplies no active-playback CPU/jitter result
  and does not reproduce or resolve the Chrome fault.

The user subsequently requested source-only investigation and had no time for
further physical testing. Later authorization enabled the 0.3.49 deployment
recorded above; the source-review results alone did not establish acceptance.

Earlier activation history: versions
0.3.23 through 0.3.30 were not deployed. A controlled 0.3.31 warm-binding trial
failed during initialization and was rolled back without a system reboot.
The corrected 0.3.32 then activated without rebooting; the earlier
0.3.21 staging state is historical.
Do not treat source, staging, CI or backend commands as completed hardware/UI
acceptance. The observations below retain their original tested versions.

## 0.3.32 activation without a system reboot (2026-11-10)

Both loaded kernel image paths/hashes and the running codec service's image
path matched a protected, locally test-signed 0.3.32 package. Parent and render
child were started without a pending-restart node flag. The standard endpoint
is named `耳机 (WH-1000XM6)`. The Bluetooth identity properties were independently
read back from the child's devnode with the expected types and values.

This followed correction of an actual 0.3.31 initialization failure, not another
reboot attempt. The [lifecycle review](LIFECYCLE-REVIEW.md) explains the premature
WDF property assignment and the enumeration-boundary fix. The same source passed
[full CI](https://github.com/TruthNian/xm6-audio/actions/runs/42679246224), all six
local CTest checks, INF validation and catalog generation. The installed binaries
were locally built; they are not claimed to be the CI artifact or a Microsoft
production-signed release.

At the latest runtime check the endpoint was unplugged, all XM6 endpoints were
inactive and the codec service reported repeated signalling connection timeouts.
The standard one-shot reconnect request was accepted, which is not proof of a
successful connection. Current headset availability, actual sound, controls,
Settings disconnect/connect labels and existing-client recovery still require
acceptance. No player was restarted or individually rerouted in this trial.

A later bounded controller trace narrowed the current connection failure.
The submitted L2CAP OPEN used AVDTP PSM 0x19 and the selected peer address.
The controller accepted Create Connection (opcode 0x0405, Command Status 0),
but two Connection Complete events for that peer reported 0x04, defined as
PAGE_TIMEOUT by the Windows SDK. The failure therefore occurs before ACL
establishment, authentication and AVDTP codec negotiation. It does not prove
whether the headset is powered or available for a connection. The trace was
stopped after capture; raw packets, addresses and controller data remain in
the protected local diagnostic directory. Headset availability confirmation
is pending, and no further driver or codec change was made from this result.

## Current acceptance after the 0.3.18 restart (2026-11-09)

The installed audio package is source commit `2cb9079edb146e6b41b25144a2c0cd4f9c104050`, built by [CI run 42635173749](https://github.com/TruthNian/xm6-audio/actions/runs/42635173749). After the authorized restart, actual kernel-module paths and on-disk image hashes matched both locally signed candidate drivers. The audio topology returned the new 88-byte support description: two channels with MULTICHANNEL and UNIFORM flags. This distinguishes the loaded update from a staged package or a service ImagePath setting.

The native codec service started during boot without a manual launch. The initial availability check found the headset disconnected and outgoing signalling attempts timed out. The user subsequently confirmed audible music/video with normal quality after this restart. Windows Settings per-device disconnect/connect still reported an error or an incorrect button state. Service startup alone did not establish sound; the subsequent audible confirmation does not establish correct reconnection.

Before that restart, the user explicitly confirmed bidirectional headset/Windows master volume. That supersedes the earlier volume failure recorded below. The application volume remains independently scoped. PotPlayer had also become audible after a one-time reopen; this does not establish automatic recovery for existing players. Reconnect acceptance requires retaining the endpoint identity and application processes without individual repair.

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

## Native multi-profile coordination candidate in 0.3.19

The user confirmed normal audible output after the 0.3.18 restart. A subsequent
connection rebuild retained both QQ Music and PotPlayer processes and the same
native endpoint, with active audio after recovery. Windows settings still
reported `Couldn't disconnect`. Direct inspection reproduced that failure,
including stale connected state after the Classic link was already down.
The public address-scoped `IOCTL_BTH_DISCONNECT_DEVICE` successfully closed the
link but did not correct the settings UI; it is not a completed UI fix and is
not added as a substitute disconnect path.

The candidate registers A2DP with Windows' existing multi-profile manager.
One-shot connection properties request native coordination, actual channel
state is reported back, and suspend requests travel through the existing audio
waiter. The service stops and drains encoded media before acknowledging suspend;
Windows retains HFP/AVRCP ownership. The driver/service ABI is version 5 and the
kernel bus interface is version 2. Opaque registration users and callbacks have
separate rundown protection during unregister. The device-info flag and service
log distinguish live registration from a binary merely importing the APIs.

These entry points are observed Windows 10 system compatibility contracts, not
documented public cross-version interfaces. See [provenance](CLEAN_ROOM.md).
Builds do not establish correct settings UI, SCO interaction, callback removal,
or reconnect. Those remain hardware acceptance requirements for this candidate.

## Bluetooth-audio interface state correction in 0.3.20

Both 0.3.19 drivers loaded without another reboot, and the service's live
device-info flag confirmed manager registration. The same QQ Music and
PotPlayer processes remained open and native LDAC resumed. A settings-initiated
disconnect reached the manager callback and closed the real Classic link, but
the settings page still displayed Connected with a Disconnect button. This is
not successful UI acceptance.

The connection property was attached to the devnode rather than the audio
device interface. The inbox implementation publishes it with
`IoSetDeviceInterfacePropertyData`. It also exposes a separate Bluetooth-audio
interface category, absent from the earlier control-only interface. The new
candidate registers both categories with the existing control reference,
publishes real AVDTP state on their interfaces, and removes the misplaced
devnode value. Registration does not add another audio endpoint or new process.
The Windows 10 category/key remain observed compatibility contracts. Actual
settings disconnect/reconnect and retained-client playback must be revalidated.

## Audio-child Bluetooth identity correction in 0.3.21

Direct inspection of Windows Settings confirmed that 0.3.20 still leaves the
per-device Disconnect button inconsistent with the actual Classic link. The
transport is 0.3.20; the active audio module is 0.3.19, whose executable sections
match the staged 0.3.20 audio module. Both modules being staged is not evidence
that both are active.

The render PDO lacked the Bluetooth address and service GUID carried by the
inbox HFP audio PDO. Settings' provider resolves endpoint profiles through the
endpoint's devnode. Sharing the physical headset container alone does not
identify its A2DP profile. A reversible local metadata correction made Settings
recognize native LDAC as "Connected voice, music" and its Disconnect request
reached the service and closed the link. The UI still remained Disconnecting;
this is a partial result, not completed disconnect/reconnect acceptance.

The transport now publishes the render PDO's real remote address and Audio Sink
service GUID before enumeration. It also supplies Bluetooth bus information for
its children, matching the bus identity of the inbox HFP audio child. No connection
status is fabricated, no Microsoft devnode policy is cleared, and no helper process
is added. Bus identity and complete Settings behavior still require deployment
and hardware validation.

References: the SDK's Bluetooth device property keys and the observed local
Microsoft endpoint association; documented
[WdfDeviceSetBusInformationForChildren](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfdevice/nf-wdfdevice-wdfdevicesetbusinformationforchildren).

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
