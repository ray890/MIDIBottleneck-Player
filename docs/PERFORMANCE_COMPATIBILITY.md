# Performance and compatibility notes

## Supported environment

The verified environment is Windows 10/11 with .NET Framework 4.8 or a compatible installed 4.x runtime/compiler. Explicit x86 and x64 builds are provided. Native wrappers/providers must match the executable architecture.

Windows 7 SP1 is untested. Windows XP and non-Windows platforms are not currently supported.

## Large MIDI files and memory

Packed inline short messages removed one `byte[]` allocation from ordinary short events, and the indexed event-store boundary prepares the codebase for later storage changes. The final store nevertheless still contains one object per event and one contiguous reference array. Extremely large files can therefore require tens of gigabytes, long load times, heavy paging, and occasional UI pauses.

The x64 `.exe.config` enables the supported [.NET Framework `gcAllowVeryLargeObjects` runtime setting](https://learn.microsoft.com/en-us/dotnet/framework/configure-apps/file-schema/runtime/gcallowverylargeobjects-element). This permits arrays larger than 2 GB on 64-bit processes but does not remove element-count limits or guarantee enough memory. The CLR must read this setting before managed startup, so the adjacent x64 configuration file is functionally required by the current contiguous-store design. The same setting has no effect in x86; the x86 package retains its matching config for a consistent, explicit distribution layout.

The planned compact segmented store would remove the single greater-than-2-GB reference-array requirement and is the preferred route to eventually dropping that runtime dependency. It is not implemented yet.

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
