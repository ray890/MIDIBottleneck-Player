# Release packaging

Never package the ignored `dist/` directory by enumeration. Local environments may contain unrelated test material even when a clean clone does not.

First build and test the release:

```powershell
.\build.ps1 -Test
.\package-release.ps1
```

The packaging command defaults to a dry run. It derives the release version from the compiled x64 executable and prints the two explicitly selected first-party executables.

To create the release files:

```powershell
.\package-release.ps1 -WritePackage -OutputDirectory .\release-package
```

The output is exactly:

- `MIDIBottleneck-Player-v<version>-x86.exe`
- `MIDIBottleneck-Player-v<version>-x64.exe`
- `SHA256SUMS.txt`

The release executables are direct downloads. Production parsing, preflight, tempo data, provisional/final records, payloads, and source-value indexes are bounded or segmented, so neither architecture needs a runtime configuration sidecar. The tagged repository supplies the matching README, GPL license, and source.

The script allowlists each executable and never sweeps `dist/`. Verify the copied files and `SHA256SUMS.txt` before upload.

Historical releases may contain notes and source tags without binary assets. Do not rebuild an old tag with a newer toolchain and describe the result as its original binary.
