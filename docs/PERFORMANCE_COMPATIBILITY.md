# Performance and compatibility notes

## Supported environment

The verified environment is Windows 10/11 with .NET Framework 4.8 or a compatible installed 4.x runtime/compiler. Explicit x86 and x64 builds are provided. Native wrappers/providers must match the executable architecture.

Windows 7 SP1 is untested. Windows XP and non-Windows platforms are not currently supported.

## Large MIDI files and memory

Build 27 stores final events as immutable 40-byte values in 65,536-record segments. Ordinary short messages are inline, and long payloads use 1 MiB byte segments. This removes the final one-object/one-reference-slot cost and any multi-gigabyte final record allocation.

The x64 `.exe.config` enables the supported [.NET Framework `gcAllowVeryLargeObjects` runtime setting](https://learn.microsoft.com/en-us/dotnet/framework/configure-apps/file-schema/runtime/gcallowverylargeobjects-element). The final store is segmented, but parsing still creates legacy per-track objects and a contiguous merged reference list before conversion. The sidecar therefore remains required until direct parser construction removes every relevant greater-than-2-GB contiguous allocation. The setting has no effect in x86, whose package has no sidecar.

On the bounded one-million-short-event fixture, x64 retained managed memory fell from about 81.3 MB to 41.3 MB and retained private memory from about 77.3 MB to 48.9 MB. Compact conversion took about 65 ms. Analysis took about 20 ms and immediate None playback about 52 ms, both with zero Gen0 collections in the measured phase. The temporary conversion peak was about 116.9 MB managed/119.6 MB private because the legacy parser graph still exists during conversion. Measurements are indicative synthetic results, not promises for every machine or file.

One user-reported external validation loaded a file containing 484,582,688 dispatchable events in the x64 build, using roughly 50 GB. This was not reproduced by the public deterministic test gate and must not be read as a general memory or load-time guarantee.

## Performance measurements

Deterministic tests use synthetic files, fake outputs, and the None output to isolate application behavior. Null-output throughput does not establish real-provider throughput, audio timing, or synthesizer capacity.

Earlier real-provider observations and incomplete native probes are retained only as qualified development evidence. Provider, architecture, wrapper, settings, workload interval, system power state, and instrumentation must match before results are compared.

The current success paths avoid per-event diagnostic string construction, module lookup, short-message arrays, and empty SysEx-reclamation work. Ordered checkpoints keep queue/statistics publication responsive without intentionally pacing output.

## Provider boundaries

- WinMM behavior depends on the selected Windows MIDI device or user-supplied wrapper.
- KDMAPI behavior depends on a compatible provider exposing the expected exports and ABI.
- None is the deterministic scheduler-only reference and produces no sound.
- The project does not distribute or install providers, synthesizers, soundfonts, or MIDI files.

Native calls can block for provider-dependent time. The player prevents concurrent scheduler/control calls, but an in-process native call that never returns cannot be forcibly interrupted safely.

## Analysis boundaries

Analysis predicts simulator service, queue occupancy, overflow, accepted-output completion, and source-end overrun. It does not predict synthesizer voice stealing or release tails, provider-internal buffering, audio-device latency, live Channel Monitor mutes/overrides, or the audible result of starting without prior channel-state reconstruction.

See [Known limitations](KNOWN_LIMITATIONS.md) for the concise compatibility list and [Technical reference](TECHNICAL_REFERENCE.md) for implementation details.
