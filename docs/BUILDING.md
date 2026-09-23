# Building and testing

MIDIBottleneck Player is a Windows WinForms application targeting .NET Framework 4.x. The verified public build environment is Windows 10/11 with .NET Framework 4.8 or a compatible installed 4.x runtime/compiler and Windows PowerShell.

From the repository root:

```powershell
.\build.ps1 -Test
```

The script uses the 64-bit .NET Framework compiler at `C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe`, builds explicit x86 and x64 applications, verifies the COFF machine field, and runs the deterministic suite in both processes. It requires no NuGet packages or network access.

Outputs are written to ignored `build/` and `dist/` directories. The expected applications are:

- `dist\MIDIBottleneck Player x86.exe`
- `dist\MIDIBottleneck Player x64.exe`

Both architectures build and test without an adjacent `.exe.config`. Production parsing and retained-song storage use bounded buffers/segments, so no supported path intentionally requests a greater-than-2-GB CLR array. The icon is embedded as both a native executable icon and a managed resource.

Build-focused groups can be run from an already compiled test harness (for example, `--test-build29` for preflight or `--test-build30` for the warning/statistics/package follow-up). The complete `build.ps1 -Test` run remains the release gate.

Local deterministic testing remains the authoritative release gate. A bounded hosted-Windows experiment showed that display-independent tests run correctly, while realized WinForms checks can see different working-area, wrapping, and font metrics on a hosted 1024-pixel desktop. Future CI should separate those test classes or provide a controlled interactive desktop rather than weakening valid layout assertions. Native-provider integration is deliberately separate.

`-MidiIntegration` is optional and intentionally excluded from the clean-clone gate because it opens installed native MIDI outputs. It requires user-supplied compatible providers and must be externally bounded:

```powershell
.\build.ps1 -Test -MidiIntegration
```
