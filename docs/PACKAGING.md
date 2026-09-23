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

Each architecture ZIP contains only its corresponding executable, `README.md`, and `LICENSE`. Production parsing, preflight, tempo data, provisional/final records, payloads, and source-value indexes are bounded or segmented, so neither architecture has a runtime configuration sidecar.

The script allowlists every input and never sweeps `dist/`. Inspect the ZIP entries and verify `SHA256SUMS.txt` before upload.

Historical releases may contain notes and source tags without binary assets. Do not rebuild an old tag with a newer toolchain and describe the result as its original binary.
