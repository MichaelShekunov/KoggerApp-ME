#requires -Version 5.1
<#
.SYNOPSIS
    Plays a recording to KoggerApp as a live device over UDP, zooms with the wheel as the stream stops,
    and reports whether the zoom levels were painted in the background while data streamed: with the
    precompute and without it (KOGGER_MOSAIC_PRECOMPUTE=0), the tiles after the zoom compared tile by
    tile.

.DESCRIPTION
    The app runs with KOGGER_PROFILE=<Profile> and KOGGER_AUTOTEST=UDP:<Port> (a hidden UDP link, never
    saved). The recording's SBP frames go to 127.0.0.1:<Port>, whole frames per datagram, at
    -PingsPerSecond (a ping of the 2026-10-17 test file is 6 102 bytes on average: -BytesPerPing), for
    -StreamSeconds. Then -Wheel notches are posted at the 3D view's middle and the stream stops; the app
    logs a digest of the tiles on screen once they stop coming (KOGGER_MOSAIC_DIGEST, dumped with
    KOGGER_MOSAIC_DUMP), grabs its window -Settle seconds later and quits.

    Printed per run: the precompute's runs while data streamed (their lines say "streaming: epochs
    before N"), the ones dropped, the zoom and when the last tile of the new level came after it (the
    digest's time less its 1.5 s wait), the live latency line. Then mosaic_dump_compare.ps1 on the two
    dumps. A live run is not deterministic -- which pings go into which job depends on timing -- so two
    runs may differ more than two file runs; the verdict is mosaic_dump_compare.ps1 -Judge's, at looser
    shares than for a file: two runs without the precompute, 60 s of 00014 at 45 pings a second
    (2026-10-17), differed by 0.044 to 0.067 % painted in one only and 0.02 to 1.7 % another value.

    The 3D camera is the profile's saved one: it has to look at the recording's area (it does after a
    file run of open_bench.ps1 or precompute_check.ps1 in the same profile). Live, only the tiles in
    view are painted: once the boat leaves the view -- the wheel acting otherwise in another run, or the
    position jumping as the boat's navigation becomes ready -- pings stop being painted, in any build
    (2026-10-18). Keep the mouse off the window.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\perf\live_zoom_check.ps1 -Exe <build>\KoggerApp.exe -File D:\Temp\perf\00014_03.05.2025.KLF -PathPrefix 'D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin'
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [Parameter(Mandatory = $true)] [string] $File,
    [string] $PathPrefix,
    [string] $Profile = 'perf',
    [int]    $Port = 23470,
    [double] $PingsPerSecond = 15,
    [int]    $BytesPerPing = 6102,
    [double] $StartDelay = 8,       # the app's start before the stream (a slow PC needs ~3 s)
    [double] $StreamSeconds = 120,
    # Where in the file the stream starts, bytes (the player finds the next frame). The test recordings
    # of 2025 open with some 2 700 to 2 900 pings of the boat out of the water (no bottom, a bogus
    # altitude, the navigation not ready): 00014 is in the water from about 18 MB (2026-10-18).
    [long]   $FromOffset = 0,
    [double] $ZoomOutAt = 5,        # seconds into the stream: -Wheel notches out, a wide view while it streams
    [int]    $Wheel = 6,
    [double] $Settle = 20,
    [string[]] $Modes = @('on', 'off'),   # on*: with the precompute, else without; the first two compared
    [string] $Out = "$env:TEMP\kogger_live_zoom_check",
    [double] $MaxCoverage = 0.002,
    [double] $MaxOther = 0.03,
    [double] $MaxHeights = 0.1
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$compare = Join-Path $here 'mosaic_dump_compare.ps1'
New-Item -ItemType Directory -Force -Path $Out | Out-Null

Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.IO;
using System.Net.Sockets;
using System.Threading;

public static class KlfLivePlayer
{
    // The file's frames to 127.0.0.1:port as the device sent them: KP1 (BB 55, route, mode, id, a byte
    // of length, payload, Fletcher-16) and KP2 (CC 55, then the frame's whole length in two bytes, as the
    // recordings of 2025 hold them). Whole frames per datagram, about 1400 bytes (a longer frame alone),
    // from file offset `offset`, at bytesPerSecond, for at most seconds.
    // Returns "<frames> <bytes> <seconds> <bytes skipped> <the offset to go on from>".
    public static string Play(string path, long offset, int port, double bytesPerSecond, double seconds)
    {
        const int kRefill = 70000;   // more than the longest frame (65535)
        byte[] buf = new byte[1 << 21];
        int len = 0, pos = 0;
        byte[] dgram = new byte[65536];
        int dlen = 0;
        long sent = 0, frames = 0, skipped = 0, consumed = 0;
        var clock = Stopwatch.StartNew();
        using (var udp = new UdpClient())
        using (var fs = File.OpenRead(path))
        {
            udp.Connect("127.0.0.1", port);
            fs.Seek(offset, SeekOrigin.Begin);
            bool eof = false;
            Action flush = () =>
            {
                udp.Send(dgram, dlen);
                sent += dlen;
                dlen = 0;
                double due = sent / bytesPerSecond - clock.Elapsed.TotalSeconds;
                if (due > 0.002) { Thread.Sleep((int)(due * 1000.0)); }
            };
            while (clock.Elapsed.TotalSeconds < seconds)
            {
                if (len - pos < kRefill && !eof)
                {
                    Buffer.BlockCopy(buf, pos, buf, 0, len - pos);
                    len -= pos;
                    pos = 0;
                    int n = fs.Read(buf, len, buf.Length - len);
                    if (n <= 0) { eof = true; } else { len += n; }
                }
                if (len - pos < 6) { break; }
                int flen = 0;
                if (buf[pos] == 0xCC && buf[pos + 1] == 0x55) { flen = buf[pos + 2] | (buf[pos + 3] << 8); }
                else if (buf[pos] == 0xBB && buf[pos + 1] == 0x55) { flen = 6 + buf[pos + 5] + 2; }
                if (flen < 6) { pos++; skipped++; consumed++; continue; }
                if (pos + flen > len) { if (eof) { break; } continue; }
                if (dlen > 0 && dlen + flen > 1400) { flush(); }
                Buffer.BlockCopy(buf, pos, dgram, dlen, flen);
                dlen += flen;
                pos += flen;
                consumed += flen;
                frames++;
                if (dlen >= 1400) { flush(); }
            }
            if (dlen > 0) { flush(); }
        }
        return frames + " " + sent + " " + clock.Elapsed.TotalSeconds.ToString("F1", System.Globalization.CultureInfo.InvariantCulture) + " " + skipped + " " + (offset + consumed);
    }
}
'@
Add-Type -Namespace KoggerLive -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool PostMessage(System.IntPtr h, uint msg, System.IntPtr w, System.IntPtr l);
[DllImport("user32.dll")] public static extern bool ClientToScreen(System.IntPtr h, ref POINT p);
public struct POINT { public int X, Y; }
'@

function Send-Wheel([System.IntPtr] $h, [int] $notches) {
    $pt = New-Object KoggerLive.Win+POINT
    $pt.X = 360; $pt.Y = 360                       # the 3D view is the window's left half
    [void][KoggerLive.Win]::ClientToScreen($h, [ref]$pt)
    $l = [System.IntPtr](($pt.Y -shl 16) -bor ($pt.X -band 0xFFFF))
    $notch = if ($notches -gt 0) { 120 } else { -120 }
    for ($n = 0; $n -lt [Math]::Abs($notches); $n++) {
        [void][KoggerLive.Win]::PostMessage($h, 0x020A, [System.IntPtr](($notch -band 0xFFFF) -shl 16), $l)
        Start-Sleep -Milliseconds 150
    }
}

$log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"
$keep = @{}
foreach ($name in 'KOGGER_MOSAIC_DIGEST', 'KOGGER_MOSAIC_PRECOMPUTE', 'KOGGER_MOSAIC_DUMP') {
    $keep[$name] = [Environment]::GetEnvironmentVariable($name)
}
$dumps = @{}
$failed = $false
try {
    foreach ($mode in $Modes) {
        "== precompute $mode"
        $dir = Join-Path $Out $mode
        if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
        if ($PathPrefix) { $psi.EnvironmentVariables['PATH'] = $PathPrefix + ';' + $env:PATH }
        $psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
        $psi.EnvironmentVariables['KOGGER_AUTOTEST'] = "UDP:$Port"
        $psi.EnvironmentVariables['KOGGER_UI_PROBE'] = $dir
        $psi.EnvironmentVariables['KOGGER_UI_PROBE_DELAY_MS'] = [string][int](($StartDelay + $StreamSeconds + $Settle) * 1000)
        $psi.EnvironmentVariables['KOGGER_UI_PROBE_QUIT'] = '1'
        $psi.EnvironmentVariables['KOGGER_UI_PROBE_PNG'] = '1'
        $psi.EnvironmentVariables['KOGGER_MOSAIC_DIGEST'] = '1'
        $psi.EnvironmentVariables['KOGGER_MOSAIC_DUMP'] = (Join-Path $dir 'dump')
        $psi.EnvironmentVariables['KOGGER_MOSAIC_PRECOMPUTE'] = $(if ($mode -like 'on*') { '1' } else { '0' })
        $app = [System.Diagnostics.Process]::Start($psi)
        Start-Sleep -Milliseconds ([int]($StartDelay * 1000))

        # Out by -Wheel notches early in the stream, in by as many as it stops: the camera ends where it
        # began, so runs one after the other start alike.
        $rate = $PingsPerSecond * $BytesPerPing
        $first = [KlfLivePlayer]::Play($File, $FromOffset, $Port, $rate, $ZoomOutAt).Split(' ')
        Send-Wheel $app.MainWindowHandle (-$Wheel)
        $rest = [KlfLivePlayer]::Play($File, [long]$first[4], $Port, $rate, $StreamSeconds - $ZoomOutAt).Split(' ')
        Send-Wheel $app.MainWindowHandle $Wheel
        "   streamed {0:N0} frames, {1:N1} MB in {2:N1} s ({3} bytes skipped); wheel -{4} notches {5} s in, +{4} as the stream stopped" -f `
            ([long]$first[0] + [long]$rest[0]), (([long]$first[1] + [long]$rest[1]) / 1MB), ([double]$first[2] + [double]$rest[2]), `
            ([long]$first[3] + [long]$rest[3]), $Wheel, $ZoomOutAt

        if (-not $app.WaitForExit([int](($Settle + 120) * 1000))) { throw 'the app did not quit after its window grab' }

        $lines = Get-Content -LiteralPath $log -Encoding UTF8
        $start = ($lines | Select-String -Pattern 'log opened' | Select-Object -Last 1).LineNumber
        $t0 = $null; $tZoom = $null; $tDigest = $null; $runs = 0; $streamRuns = 0; $drops = 0; $zoomLine = ''
        foreach ($line in $lines[($start - 1)..($lines.Count - 1)]) {
            if ($line -notmatch '^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3}) ') { continue }
            $t = [datetime]::ParseExact($Matches[1], 'yyyy-MM-dd HH:mm:ss.fff', $null)
            if (-not $t0) { $t0 = $t }
            if ($line -match 'mosaic precompute: zoom') { $runs++; if ($line -match 'streaming:') { $streamRuns++ } }
            if ($line -match 'mosaic precompute: dropped') { $drops++; "   {0,7:N2} s  {1}" -f ($t - $t0).TotalSeconds, ($line -replace '^\S+ \S+ ', '') }
            if ($line -match 'mosaic zoom: (\d+) -> (\d+)') { $tZoom = $t; $zoomLine = $Matches[0] }
            if ($tZoom -and $line -match 'mosaic digest:') { $tDigest = $t; $digestLine = ($line -replace '^.*mosaic digest: ', '') }
            if ($line -match 'mosaic zoom fallback: ') { "   {0,7:N2} s  {1}" -f ($t - $t0).TotalSeconds, ($line -replace '^\S+ \S+ ', '') }
            if ($line -match 'mosaic latency: ') { "   {0,7:N2} s  {1}" -f ($t - $t0).TotalSeconds, ($line -replace '^\S+ \S+ ', '') }
        }
        "   precompute runs {0} (while data streamed {1}), dropped {2}" -f $runs, $streamRuns, $drops
        if (-not $tZoom -or -not $tDigest) {
            "   no zoom or no digest after it"; $failed = $true; continue
        }
        "   {0}, the last tile {1:N2} s after it; digest {2}" -f $zoomLine, (($tDigest - $tZoom).TotalSeconds - 1.5), $digestLine
        $last = Get-ChildItem -LiteralPath (Join-Path $dir 'dump') -Filter 'digest_*.bin' -ErrorAction SilentlyContinue |
                Sort-Object { [int]($_.BaseName -replace '\D', '') } | Select-Object -Last 1
        if ($last) { $dumps[$mode] = $last.FullName }
    }
    if ($Modes.Count -ge 2 -and $dumps.ContainsKey($Modes[0]) -and $dumps.ContainsKey($Modes[1])) {
        "== tiles, $($Modes[0]) (A) against $($Modes[1]) (B)"
        & powershell -NoProfile -ExecutionPolicy Bypass -File $compare -A $dumps[$Modes[0]] -B $dumps[$Modes[1]] -Picture (Join-Path $Out 'diff.png') `
            -Judge -MaxCoverage $MaxCoverage -MaxOther $MaxOther -MaxHeights $MaxHeights 2>&1 | ForEach-Object { "$_" }
        if ($LASTEXITCODE -ne 0) { $failed = $true }
    }
} finally {
    foreach ($name in $keep.Keys) { [Environment]::SetEnvironmentVariable($name, $keep[$name]) }
}
if ($failed) { exit 1 }
