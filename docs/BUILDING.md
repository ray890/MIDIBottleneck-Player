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

Each has a matching `.exe.config` copied from `app.config`. The icon is embedded as both a native executable icon and a managed resource.

`-MidiIntegration` is optional and intentionally excluded from the clean-clone gate because it opens installed native MIDI outputs. It requires user-supplied compatible providers and must be externally bounded:

```powershell
.\build.ps1 -Test -MidiIntegration
```
