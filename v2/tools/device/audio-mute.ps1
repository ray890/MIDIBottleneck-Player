# Mutes / restores the Windows playback endpoint(s) that real-device tests would be heard on.
#
#   audio-mute.ps1 -Action Status                    (read only: lists render endpoints and their mute flags)
#   audio-mute.ps1 -Action Mute    -StateFile <json>  (records the ORIGINAL mute flags, mutes, reads back)
#   audio-mute.ps1 -Action Restore -StateFile <json>  (puts every original flag back, reads back)
#
# Only the endpoint MUTE FLAG is ever written. Volume levels are only read (for the log), never set.
# It uses the Windows Core Audio COM interfaces (IMMDeviceEnumerator / IAudioEndpointVolume) through a small
# inline C# class: Microsoft API only, no third-party code.
#
# Which endpoints are muted: the default playback device for each of the three roles (console, multimedia,
# communications) AND every other active playback endpoint (belt and braces: OmniMIDI with the WASAPI engine and a
# WASAPIOutput value that names no real device renders to the default device, but a muted spare costs nothing).
# See v2/docs/DEVICE_MEASUREMENTS.md "Mute handling".
#
# Safe against a crashed run: if the state file already says "muted" (a previous run died before Restore), the
# ORIGINAL flags in it are kept (they are not re-read as "muted"), the mute is re-applied, and the later
# Restore puts the true originals back.
#
# Exit code 0 = the requested state was read back and confirmed; 1 = it could not be confirmed.
param(
    [Parameter(Mandatory = $true)][ValidateSet('Status', 'Mute', 'Restore')][string]$Action,
    [string]$StateFile
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace CoreAudioMute
{
    [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IMMDeviceEnumerator
    {
        [PreserveSig] int EnumAudioEndpoints(int dataFlow, uint stateMask, out IMMDeviceCollection devices);
        [PreserveSig] int GetDefaultAudioEndpoint(int dataFlow, int role, out IMMDevice device);
        [PreserveSig] int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string id, out IMMDevice device);
        [PreserveSig] int RegisterEndpointNotificationCallback(IntPtr client);
        [PreserveSig] int UnregisterEndpointNotificationCallback(IntPtr client);
    }

    [ComImport, Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IMMDeviceCollection
    {
        [PreserveSig] int GetCount(out uint count);
        [PreserveSig] int Item(uint index, out IMMDevice device);
    }

    [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IMMDevice
    {
        [PreserveSig] int Activate(ref Guid iid, uint clsCtx, IntPtr activationParams, [MarshalAs(UnmanagedType.IUnknown)] out object endpoint);
        [PreserveSig] int OpenPropertyStore(uint access, out IntPtr store);
        [PreserveSig] int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
        [PreserveSig] int GetState(out uint state);
    }

    [ComImport, Guid("5CDF2C82-841E-4546-9722-0CF74078229A"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IAudioEndpointVolume
    {
        [PreserveSig] int RegisterControlChangeNotify(IntPtr notify);
        [PreserveSig] int UnregisterControlChangeNotify(IntPtr notify);
        [PreserveSig] int GetChannelCount(out uint count);
        [PreserveSig] int SetMasterVolumeLevel(float level, ref Guid context);
        [PreserveSig] int SetMasterVolumeLevelScalar(float level, ref Guid context);
        [PreserveSig] int GetMasterVolumeLevel(out float level);
        [PreserveSig] int GetMasterVolumeLevelScalar(out float level);
        [PreserveSig] int SetChannelVolumeLevel(uint channel, float level, ref Guid context);
        [PreserveSig] int SetChannelVolumeLevelScalar(uint channel, float level, ref Guid context);
        [PreserveSig] int GetChannelVolumeLevel(uint channel, out float level);
        [PreserveSig] int GetChannelVolumeLevelScalar(uint channel, out float level);
        [PreserveSig] int SetMute([MarshalAs(UnmanagedType.Bool)] bool mute, ref Guid context);
        [PreserveSig] int GetMute([MarshalAs(UnmanagedType.Bool)] out bool mute);
        [PreserveSig] int GetVolumeStepInfo(out uint step, out uint stepCount);
        [PreserveSig] int VolumeStepUp(ref Guid context);
        [PreserveSig] int VolumeStepDown(ref Guid context);
        [PreserveSig] int QueryHardwareSupport(out uint mask);
        [PreserveSig] int GetVolumeRange(out float min, out float max, out float increment);
    }

    [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
    internal class MMDeviceEnumeratorClass { }

    public sealed class Endpoint
    {
        public string Id;
        public uint State;
        public bool Muted;
        public float VolumeScalar;
        public string Roles = "";
    }

    public static class Audio
    {
        private const int ERender = 0;
        private static readonly string[] RoleNames = { "console", "multimedia", "communications" };
        private static Guid _noContext = Guid.Empty;

        private static void Check(int hr, string what)
        {
            if (hr < 0) throw new InvalidOperationException(what + " failed, HRESULT 0x" + hr.ToString("X8"));
        }

        private static IMMDeviceEnumerator NewEnumerator()
        {
            return (IMMDeviceEnumerator)new MMDeviceEnumeratorClass();
        }

        private static IAudioEndpointVolume Volume(IMMDevice device)
        {
            Guid iid = typeof(IAudioEndpointVolume).GUID;
            object o;
            Check(device.Activate(ref iid, 23 /* CLSCTX_ALL */, IntPtr.Zero, out o), "Activate(IAudioEndpointVolume)");
            return (IAudioEndpointVolume)o;
        }

        private static Endpoint Describe(IMMDevice device)
        {
            Endpoint e = new Endpoint();
            Check(device.GetId(out e.Id), "GetId");
            Check(device.GetState(out e.State), "GetState");
            IAudioEndpointVolume v = Volume(device);
            bool muted;
            Check(v.GetMute(out muted), "GetMute");
            e.Muted = muted;
            float scalar;
            Check(v.GetMasterVolumeLevelScalar(out scalar), "GetMasterVolumeLevelScalar");
            e.VolumeScalar = scalar;
            return e;
        }

        // Default render endpoints of the three roles, de-duplicated (Roles lists which roles point at each).
        public static List<Endpoint> DefaultEndpoints()
        {
            IMMDeviceEnumerator en = NewEnumerator();
            List<Endpoint> list = new List<Endpoint>();
            for (int role = 0; role < 3; role++)
            {
                IMMDevice device;
                int hr = en.GetDefaultAudioEndpoint(ERender, role, out device);
                if (hr < 0) continue;   // no default for this role
                Endpoint e = Describe(device);
                Endpoint existing = list.Find(delegate (Endpoint x) { return x.Id == e.Id; });
                if (existing == null) { e.Roles = RoleNames[role]; list.Add(e); }
                else existing.Roles += "," + RoleNames[role];
            }
            return list;
        }

        // Every active render endpoint (state 1 = ACTIVE), for the record.
        public static List<Endpoint> ActiveEndpoints()
        {
            IMMDeviceEnumerator en = NewEnumerator();
            IMMDeviceCollection c;
            Check(en.EnumAudioEndpoints(ERender, 1, out c), "EnumAudioEndpoints");
            uint n;
            Check(c.GetCount(out n), "GetCount");
            List<Endpoint> list = new List<Endpoint>();
            for (uint i = 0; i < n; i++)
            {
                IMMDevice d;
                Check(c.Item(i, out d), "Item");
                list.Add(Describe(d));
            }
            return list;
        }

        public static bool GetMute(string id)
        {
            IMMDevice d;
            Check(NewEnumerator().GetDevice(id, out d), "GetDevice");
            bool m;
            Check(Volume(d).GetMute(out m), "GetMute");
            return m;
        }

        public static void SetMute(string id, bool mute)
        {
            IMMDevice d;
            Check(NewEnumerator().GetDevice(id, out d), "GetDevice");
            Check(Volume(d).SetMute(mute, ref _noContext), "SetMute");
        }
    }
}
'@

function Get-EndpointName([string]$Id) {
    # Best effort friendly name from the MMDevices registry key (the endpoint GUID is the last {...} of the id).
    if ($Id -match '\{([0-9a-fA-F-]{36})\}$') {
        $key = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\{$($Matches[1])}\Properties"
        try {
            $p = Get-ItemProperty -Path $key -ErrorAction Stop
            $desc = $p.'{a45c254e-df1c-4efd-8020-67d146a850e0},2'
            $name = $p.'{b3f8fa53-0004-438e-9003-51a46e139bfc},6'
            if ($desc -and $name) { return "$desc ($name)" }
            if ($desc) { return [string]$desc }
        } catch { }
    }
    return '(name unavailable)'
}

function Write-State($state, [string]$Path) {
    $dir = Split-Path -Parent $Path
    if ($dir) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    $state | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Path -Encoding UTF8
}

function Show([string]$text) { Write-Host ("audio-mute: " + $text) }

if ($Action -eq 'Status') {
    foreach ($e in [CoreAudioMute.Audio]::ActiveEndpoints()) {
        Show ("active render endpoint {0} muted={1} volume={2:N2} {3}" -f $e.Id, $e.Muted, $e.VolumeScalar, (Get-EndpointName $e.Id))
    }
    foreach ($e in [CoreAudioMute.Audio]::DefaultEndpoints()) {
        Show ("default for [{0}] = {1} muted={2}" -f $e.Roles, $e.Id, $e.Muted)
    }
    exit 0
}

if (-not $StateFile) { throw '-StateFile is required for Mute and Restore.' }

if ($Action -eq 'Mute') {
    $existing = $null
    if (Test-Path -LiteralPath $StateFile) {
        $existing = Get-Content -LiteralPath $StateFile -Raw | ConvertFrom-Json
        if ($existing.status -ne 'muted') { $existing = $null }   # an old, already-restored state: start fresh
    }
    $defaults = [CoreAudioMute.Audio]::DefaultEndpoints()
    if ($defaults.Count -eq 0) { Show 'no default playback device found; cannot confirm anything is muted'; exit 1 }
    $targets = @($defaults)
    foreach ($a in [CoreAudioMute.Audio]::ActiveEndpoints()) {
        if (-not (@($targets | Where-Object { $_.Id -eq $a.Id }).Count)) { $a.Roles = 'non-default'; $targets += $a }
    }
    $entries = @()
    foreach ($e in $targets) {
        $original = $e.Muted
        if ($existing) {
            $prior = @($existing.endpoints | Where-Object { $_.id -eq $e.Id })
            if ($prior.Count -gt 0) { $original = [bool]$prior[0].originalMuted }   # keep the true original
        }
        $entries += [pscustomobject]@{
            id = $e.Id; name = (Get-EndpointName $e.Id); roles = $e.Roles
            originalMuted = $original; volumeScalarAtStart = [math]::Round($e.VolumeScalar, 4)
        }
    }
    if ($existing) {   # keep any endpoint from the earlier state that is no longer a default
        foreach ($p in @($existing.endpoints)) {
            if (-not (@($entries | Where-Object { $_.id -eq $p.id }).Count)) { $entries += $p }
        }
    }
    # Persist the originals BEFORE changing anything.
    $state = [pscustomobject]@{
        status = 'muted'; startedLocal = (Get-Date).ToString('s'); endpoints = $entries
    }
    Write-State $state $StateFile
    $ok = $true
    foreach ($entry in $entries) {
        [CoreAudioMute.Audio]::SetMute($entry.id, $true)
        $now = [CoreAudioMute.Audio]::GetMute($entry.id)
        Show ("endpoint {0} [{1}] {2}: original muted={3}; now muted={4}" -f $entry.id, $entry.roles, $entry.name, $entry.originalMuted, $now)
        if (-not $now) { $ok = $false }
    }
    if (-not $ok) { Show 'MUTE NOT CONFIRMED'; exit 1 }
    Show 'MUTE CONFIRMED (read back)'
    exit 0
}

if ($Action -eq 'Restore') {
    if (-not (Test-Path -LiteralPath $StateFile)) { Show 'no state file; nothing to restore'; exit 0 }
    $state = Get-Content -LiteralPath $StateFile -Raw | ConvertFrom-Json
    $ok = $true
    foreach ($entry in @($state.endpoints)) {
        try {
            [CoreAudioMute.Audio]::SetMute($entry.id, [bool]$entry.originalMuted)
            $now = [CoreAudioMute.Audio]::GetMute($entry.id)
            Show ("endpoint {0} [{1}]: restored to muted={2}; read back muted={3}" -f $entry.id, $entry.roles, $entry.originalMuted, $now)
            if ($now -ne [bool]$entry.originalMuted) { $ok = $false }
        } catch {
            Show ("endpoint {0}: restore FAILED: {1}" -f $entry.id, $_.Exception.Message)
            $ok = $false
        }
    }
    if ($ok) {
        $state.status = 'restored'
        $state | Add-Member -NotePropertyName restoredLocal -NotePropertyValue ((Get-Date).ToString('s')) -Force
        Write-State $state $StateFile
        Show 'RESTORE CONFIRMED (read back)'
        exit 0
    }
    Show 'RESTORE NOT CONFIRMED (state file kept as "muted")'
    exit 1
}
