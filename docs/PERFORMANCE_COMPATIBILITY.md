# Performance and compatibility notes

## Supported environment

The verified environment is Windows 10/11 with .NET Framework 4.8 or a compatible installed 4.x runtime/compiler. Explicit x86 and x64 builds are provided. Native wrappers/providers must match the executable architecture.

Windows 7 SP1 is untested. Windows XP and non-Windows platforms are not currently supported.

## Large MIDI files and memory

Build 28 parses tracks through a 64 KiB bounded reader into compact provisional segments and merges directly into immutable 40-byte final records. Record segments contain 2,048 values (80 KiB, below the .NET Framework large-object threshold); long payloads remain in 1 MiB segments. Tempo histories and source-value indexes are segmented as well.

The production loading audit found no intentionally scalable single array: track reads use 64 KiB, records/tempo/source histories/payloads are segmented, merge heaps scale only with the at-most-65,535 track count, and Analysis buckets are explicitly capped. Both packages therefore run without `gcAllowVeryLargeObjects` or a configuration sidecar. The event store retains an explicit 2,147,483,647-event indexed-identity limit.

On repeated bounded one-million-short-event runs, Build 28 retained about 41.3 MB managed memory on both architectures. Retained private delta was about 42.7 MB on x86 and 40.5 MB on x64; observed peak managed delta was about 60–62 MB on x86 and 61–66 MB on x64. The post-merge conversion is gone. Loading was about 363–398 ms on x86 and 267–316 ms on x64. Analysis and immediate None playback recorded zero Gen0 collections; Analysis was about 18–24 ms, while playback was about 78–82 ms on x86 and 46–54 ms on x64. Measurements are synthetic indicators, not machine/file guarantees.

The historical Build 26 x86 playback result was about 62–70 ms. Same-session Build 27 warm runs ranged about 77–94 ms; Build 28's contained sequential compact-reader path narrowed this to roughly 78–82 ms without harming x64. The remaining percentage difference versus the older historical run cannot be isolated from session/power variation confidently enough to justify a larger hot-path redesign.

Build 29 adds an allocation-light count-only scan only above conservative file-size gates: 15 MiB on x86 and 63 MiB on x64. A generated 250,000-event/750,027-byte dense fixture scanned in about 10.1 ms on x86 and 15.7 ms on x64, changed managed memory by roughly 74 KiB, and caused no Gen0 collection. These are bounded measurements on one machine, not universal throughput promises.

Warnings use exact SMF counts and the current segmented layouts. The x86 warning boundary is 1 GiB because practical 32-bit address-space pressure begins well before an x64-scale load; x64 uses the original 4 GiB product threshold. The dialog calls these figures estimated memory while open and estimated highest memory while loading. They remain projections rather than allocation guarantees.

Build 30 pauses the displayed loading timer while that warning waits for a decision. The pause is subtracted after Continue or Cancel, so elapsed loading time describes application work rather than reading time.

A Build 31 bounded 200,000-event x64 run measured the count-only preflight separately at 4.8–6.4 ms and production parsing at 45.4–57.2 ms. The preflight produced no Gen0 collection; parsing produced two or three. Warning-sized files intentionally pay both costs, while ordinary files remain one-pass. These figures describe one generated fixture and are not giant-file predictions.

One user-reported external validation loaded a file containing 484,582,688 dispatchable events in the x64 build, using roughly 50 GB. This was not reproduced by the public deterministic test gate and must not be read as a general memory or load-time guarantee.

## Performance measurements

Deterministic tests use synthetic files, fake outputs, and the None output to isolate application behavior. Null-output throughput does not establish real-provider throughput, audio timing, or synthesizer capacity.

Earlier real-provider observations and incomplete native probes are retained only as qualified development evidence. Provider, architecture, wrapper, settings, workload interval, system power state, and instrumentation must match before results are compared.

The current success paths avoid per-event diagnostic string construction, module lookup, short-message arrays, and empty SysEx-reclamation work. Ordered checkpoints keep queue/statistics publication responsive without intentionally pacing output.

In Per-note mode, Effective speed uses a monotonic resolved-source frontier that advances only after each interval frame's output batch returns. It advances through empty frames in sparse/filtered passages and stalls behind a blocked output. Individual event lag remains tied to each event's original timestamp, so it can vary within one frame by design.

Build 31 repeated injected short-message boundary measurements without opening a provider. Across five 500,000-send runs, WinMM took 13.1–14.5 ms and KDMAPI took 12.6–14.6 ms, with zero Gen0 collections. Neither boundary had a stable advantage large enough to justify changing the tested adapter path. These numbers measure application/delegate overhead only and say nothing about a synthesizer's internal processing. Warm immediate None playback of the generated 200,000-event fixture was about 6.9–7.2 ms after a 21.3 ms first run, also with zero Gen0 collections.

## Provider boundaries

- WinMM behavior depends on the selected Windows MIDI device or user-supplied wrapper.
- KDMAPI behavior depends on a compatible provider exposing the expected exports and ABI.
- None is the deterministic scheduler-only reference and produces no sound.
- The project does not distribute or install providers, synthesizers, soundfonts, or MIDI files.

Native calls can block for provider-dependent time. The player prevents concurrent scheduler/control calls, but an in-process native call that never returns cannot be forcibly interrupted safely.

## Analysis boundaries

Analysis predicts simulator service, queue occupancy, overflow, accepted-output completion, and source-end overrun. It does not predict synthesizer voice stealing or release tails, provider-internal buffering, audio-device latency, live Channel Monitor mutes/overrides, or the audible result of starting without prior channel-state reconstruction.

See [Known limitations](KNOWN_LIMITATIONS.md) for the concise compatibility list and [Technical reference](TECHNICAL_REFERENCE.md) for implementation details.
