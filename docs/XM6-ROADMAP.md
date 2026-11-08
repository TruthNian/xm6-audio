# Improvement decisions

The baseline is useful only if audible output remains reproducible. Each item starts with a concrete missing behavior and a way to tell whether it is fixed. No item is marked verified merely because its code exists.

| Priority | Observed gap or open question | Smallest next step | Acceptance |
|---|---|---|---|
| 1 | CLI requires manual startup and QQ-specific capture | Use the existing resident core, system source and saved profile; restore it inside the core | Native process restart restores fixed LDAC and audible system playback without a terminal |
| 1 | Headphones may be off when Windows signs in | Bounded initial retries in the existing sampler | Powering headphones on later connects; explicit Disconnect still stops |
| 1 | Pre-muted Windows output can escape mute when volume changes | Separate mute restoration ownership from the existing key watcher | Volume keys leave speakers silent, change headphone level, preserve prior mute on exit |
| 1 | Warm operation does not establish cold-start firmware readiness | Prepare the exact controller-family firmware and retain its source/license; observe next real cold start | Cold start succeeds without first loading the Intel Windows driver |
| 2 | Sleep, USB power transitions and audio-device removal have not been verified here | Observe existing invalidation/reconnect paths before adding mechanisms | Resume/default-device change restores 48 kHz / fixed 990 and audible playback |
| 2 | Video uses an external audio pipeline with headphone buffering | Measure audio/video offset; inspect whether an endpoint with accurate latency reporting is needed | A documented synchronization bound on named players, rather than a universal claim |
| 3 | Shared-mode capture may omit protected, exclusive or explicitly rerouted playback | Establish which case fails and why; evaluate a standard Windows render endpoint only for a proven gap | Normal app output selection and volume behavior work without duplicated PCM paths |
| 3 | A process crash differs from a Bluetooth link drop | Examine native Windows restart/recovery facilities before adding another process | Recovery restores the saved profile; an intentional exit remains an exit |
| Ongoing | Fixed 990 kbps cannot defeat insufficient radio capacity | Measure airtime, interference and queue age; remove avoidable contention | Report impairment honestly; do not relabel an automatic downgrade as 990 |

Before implementation, record the requirement being questioned and evidence that an existing path cannot meet it. After implementation, delete the workaround it replaces. Optimization follows a demonstrated bottleneck, not a desire to add settings.
