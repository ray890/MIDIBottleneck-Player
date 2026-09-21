# Release packaging

Never package the ignored `dist/` directory by enumeration. Local environments may contain unrelated test material even when a clean clone does not.

First build and test the release:

```powershell
.\build.ps1 -Test
.\package-release.ps1
```

The packaging command defaults to a dry run. It derives the release version from the compiled x64 executable and prints the explicit contents of two architecture packages.

To create the release files:

```powershell
.\package-release.ps1 -WritePackage -OutputDirectory .\release-package
```

The output is exactly:

- `MIDIBottleneck-Player-v<version>-x86.zip`
- `MIDIBottleneck-Player-v<version>-x64.zip`
- `SHA256SUMS.txt`

Each ZIP contains its matching executable and `.exe.config`, plus `README.md` and `LICENSE`. The x64 configuration is required by the current contiguous event store because .NET Framework must read `gcAllowVeryLargeObjects` before managed startup. That setting affects only 64-bit processes; the x86 config is retained for an explicit, symmetric package layout.

The script allowlists every input and never sweeps `dist/`. Inspect the ZIP entries and verify `SHA256SUMS.txt` before upload.

Historical releases may contain notes and source tags without binary assets. Do not rebuild an old tag with a newer toolchain and describe the result as its original binary.
