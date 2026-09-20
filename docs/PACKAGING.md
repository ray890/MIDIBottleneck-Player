# Release packaging

Never package the ignored `dist/` directory by enumeration. Local environments may contain unrelated or third-party material even though the public clean clone does not.

First build and test the release:

```powershell
.\build.ps1 -Test
.\package-release.ps1
```

The second command is a dry run. It prints the exact allowlisted files and hashes. To create a clean package directory:

```powershell
.\package-release.ps1 -WritePackage -OutputDirectory .\release-package
```

The allowlist contains the x86/x64 applications, matching configurations, `LICENSE`, and `README.md`. The script writes `SHA256SUMS.txt` from those exact inputs. Inspect the resulting directory before uploading it to a release.

Historical releases may contain notes and source tags without binary assets. Do not rebuild an old tag with a newer toolchain and describe the result as its original binary.
