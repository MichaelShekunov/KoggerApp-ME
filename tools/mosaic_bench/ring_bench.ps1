#requires -Version 5.1
<#
.SYNOPSIS
    A synthetic two-sided sonar turning on the spot, for the circular mosaic: as a KLF file, or played
    to KoggerApp live over UDP with the mosaic latency read back from kogger.log.

.DESCRIPTION
    Each ping is what a nanoSSS sends: ID_ATTITUDE v0 (yaw, pitch, roll in hundredths of a degree; the
    pitch is 0.1 deg because the app drops an all-zero attitude), then the echogram as ID_CHART v1
    parts (both sides interleaved), then an empty part at offset 0, which completes the chart at once
    as the one-frame ID_CHART_Z does. The echo is a bright ring every metre of range on a faint ramp,
    every other ring brighter on starboard, so a polygon or a wrong side shows at once.

    -Klf writes the stream to a file (open it in the app; Pos zeroing on draws it).
    -Exe starts that app with KOGGER_AUTOTEST=UDP:<Port> (a hidden UDP link, never saved),
    KOGGER_UI_PROBE=<Out> and KOGGER_UI_PROBE_PNG=1, plays the stream in real time, resizes the window
    by a pixel five seconds in (headless, nothing else moves the camera, and the mosaic paints only
    visible tiles; -NoNudge leaves it, as a user who does not touch the view), and once the stream
    has stopped the app grabs its window to <Out>\ui-auto.png and quits. The app reads the user's own settings: Pos zeroing must be on, the mosaic shown. Back them
    up first (reg export HKCU\Software\KOGGER\KoggerApp) if the run must leave them as they were.
    Stop the stream before the app quits: quitting with a live serial link open has hung the app.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\mosaic_bench\ring_bench.ps1 -Klf D:\Temp\rings_12deg.klf -Step 12 -Seconds 2

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\mosaic_bench\ring_bench.ps1 -Exe <build>\KoggerApp.exe -Out D:\Temp\bench -Step 6
#>
[CmdletBinding()]
param(
    [string] $Klf,
    [string] $Exe,
    [string] $Out = "$env:TEMP\kogger_ring_bench",
    [double] $Step = 6,          # degrees a ping
    [double] $Rate = 30,         # pings a second
    [double] $Seconds = 28,      # length of the stream
    [int]    $Port = 23460,
    [int]    $Samples = 500,     # a side
    [int]    $ResolutionMm = 30,
    [string] $PathPrefix,        # e.g. D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin for a build tree's exe
    [string] $Profile,           # KOGGER_PROFILE: settings and a log of their own, beside the user's running app
    [switch] $NoNudge            # no window nudge: as a user who connects and does not touch the view
)

$ErrorActionPreference = 'Stop'

function Get-Fletcher([byte[]] $data) {
    $c1 = 0; $c2 = 0
    foreach ($b in $data) { $c1 = ($c1 + $b) -band 0xFF; $c2 = ($c2 + $c1) -band 0xFF }
    return ,([byte[]]@($c1, $c2))
}

function New-Frame([int] $id, [int] $ver, [byte[]] $payload) {
    $body = New-Object System.Collections.Generic.List[byte]
    $body.Add(0)                                   # route
    $body.Add([byte](1 -bor (($ver -band 7) -shl 3)))   # CONTENT, version
    $body.Add([byte]$id)
    $body.Add([byte]$payload.Length)
    $body.AddRange($payload)
    $frame = New-Object System.Collections.Generic.List[byte]
    $frame.Add(0xBB); $frame.Add(0x55)
    $frame.AddRange($body)
    $frame.AddRange([byte[]](Get-Fletcher $body.ToArray()))
    return ,($frame.ToArray())
}

function Get-U16([int] $v) { return ,([BitConverter]::GetBytes([uint16]$v)) }
function Get-S16([int] $v) { return ,([BitConverter]::GetBytes([int16]$v)) }

# The echogram of one ping, both sides interleaved, then its ID_CHART v1 parts.
$inter = New-Object byte[] (2 * $Samples)
for ($s = 0; $s -lt $Samples; $s++) {
    $mm = $s * $ResolutionMm
    $level = 30 + [int][Math]::Floor($s * 40 / $Samples)
    $left = $level; $right = $level
    if (($mm % 1000) -lt 2 * $ResolutionMm) { $left = 230; $right = 230 }
    if (($mm % 2000) -lt 2 * $ResolutionMm) { $right = 255 }
    $inter[2 * $s] = [byte]$left
    $inter[2 * $s + 1] = [byte]$right
}
$chart = New-Object System.Collections.Generic.List[byte]
for ($off = 0; $off -lt $inter.Length; $off += 200) {
    $len = [Math]::Min(200, $inter.Length - $off)
    $part = New-Object byte[] $len
    [Array]::Copy($inter, $off, $part, 0, $len)
    $chart.AddRange([byte[]](New-Frame 0x03 1 ([byte[]]((Get-U16 $off) + (Get-U16 $ResolutionMm) + (Get-U16 0) + $part))))
}
$chart.AddRange([byte[]](New-Frame 0x03 1 ([byte[]]((Get-U16 0) + (Get-U16 $ResolutionMm) + (Get-U16 0)))))
$chartBytes = $chart.ToArray()

function Get-Ping([int] $k) {
    $yaw = (($k * $Step + 180.0) % 360.0) - 180.0
    $att = New-Frame 0x04 0 ([byte[]]((Get-S16 ([int][Math]::Round($yaw * 100))) + (Get-S16 10) + (Get-S16 0)))
    return ,([byte[]]($att + $chartBytes))
}

$pings = [int][Math]::Round($Seconds * $Rate)

if ($Klf) {
    $stream = New-Object System.IO.MemoryStream
    for ($k = 0; $k -le $pings; $k++) { $b = Get-Ping $k; $stream.Write($b, 0, $b.Length) }
    [System.IO.File]::WriteAllBytes($Klf, $stream.ToArray())
    "{0}: {1} pings, {2} deg each, {3} samples a side at {4} mm" -f $Klf, ($pings + 1), $Step, $Samples, $ResolutionMm
    return
}

if (-not $Exe) { throw 'give -Klf <file> or -Exe <KoggerApp.exe>' }

Add-Type -Namespace KoggerBench -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool GetWindowRect(System.IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool SetWindowPos(System.IntPtr h, System.IntPtr after, int x, int y, int cx, int cy, uint flags);
public struct RECT { public int Left, Top, Right, Bottom; }
'@

New-Item -ItemType Directory -Force -Path $Out | Out-Null
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
if ($PathPrefix) { $psi.EnvironmentVariables['PATH'] = $PathPrefix + ';' + $env:PATH }
$psi.EnvironmentVariables['KOGGER_AUTOTEST'] = "UDP:$Port"
if ($Profile) { $psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile }
$psi.EnvironmentVariables['KOGGER_UI_PROBE'] = $Out
$psi.EnvironmentVariables['KOGGER_UI_PROBE_DELAY_MS'] = [string][int](($Seconds + 10) * 1000)
$psi.EnvironmentVariables['KOGGER_UI_PROBE_QUIT'] = '1'
$psi.EnvironmentVariables['KOGGER_UI_PROBE_PNG'] = '1'
$app = [System.Diagnostics.Process]::Start($psi)

$udp = New-Object System.Net.Sockets.UdpClient
Start-Sleep -Seconds 4                              # the link opens about a second after start
$clock = [System.Diagnostics.Stopwatch]::StartNew()
$resized = [bool]$NoNudge
for ($k = 0; $k -lt $pings; $k++) {
    $due = $k * 1000.0 / $Rate
    while ($clock.Elapsed.TotalMilliseconds -lt $due) { Start-Sleep -Milliseconds 1 }
    $b = Get-Ping $k
    [void]$udp.Send($b, $b.Length, '127.0.0.1', $Port)
    if (-not $resized -and $clock.Elapsed.TotalSeconds -ge 5) {
        $resized = $true
        $app.Refresh()
        $h = $app.MainWindowHandle
        if ($h -ne [System.IntPtr]::Zero) {
            $r = New-Object KoggerBench.Win+RECT
            [void][KoggerBench.Win]::GetWindowRect($h, [ref]$r)
            [void][KoggerBench.Win]::SetWindowPos($h, [System.IntPtr]::Zero, $r.Left, $r.Top, ($r.Right - $r.Left) + 1, ($r.Bottom - $r.Top), 0x0014)
        }
    }
}
$udp.Close()
"sent {0} pings in {1:N1} s, {2} deg each" -f $pings, $clock.Elapsed.TotalSeconds, $Step
if (-not $app.WaitForExit(60000)) { throw 'the app did not quit after its window grab' }

$appName = if ($Profile) { "KoggerApp-$Profile" } else { 'KoggerApp' }
$log = Join-Path $env:APPDATA "KOGGER\$appName\AppLogs\kogger.log"
$lines = Get-Content -LiteralPath $log -Encoding UTF8
$start = ($lines | Select-String -Pattern 'log opened' | Select-Object -Last 1).LineNumber
$lines[($start - 1)..($lines.Count - 1)] | Select-String -Pattern 'mosaic latency' | ForEach-Object { $_.Line -replace '^.*default: ', '' }
$png = Join-Path $Out 'ui-auto.png'
"window: {0}" -f $png

# How much of the circle the mosaic covers, from the grab: around the sonar's red arrow (looked for in
# the middle of the 3D view, the compass's red needle is in its corner), an angle is covered when most
# of the radii 100-250 px are not the dark background. With "Clear old data" and N
# pings a side it is about 2 (N + 2) Step degrees (two sides, the three pairs before the N), else 360.
Add-Type -AssemblyName System.Drawing
$bmp = [System.Drawing.Bitmap]::FromFile($png)
try {
    $rect = New-Object System.Drawing.Rectangle 0, 0, $bmp.Width, $bmp.Height
    $bits = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $px = New-Object byte[] ($bits.Stride * $bmp.Height)
    [System.Runtime.InteropServices.Marshal]::Copy($bits.Scan0, $px, 0, $px.Length)
    $stride = $bits.Stride
    $bmp.UnlockBits($bits)
} finally { $bmp.Dispose() }
$w = $rect.Width; $h = $rect.Height
$sx = 0.0; $sy = 0.0; $sn = 0
for ($y = [int]($h / 4); $y -lt [int]($h * 3 / 4); $y++) {
    for ($x = [int]($w / 8); $x -lt [int]($w * 3 / 8); $x++) {
        $i = $y * $stride + 4 * $x
        if ($px[$i + 2] -gt 180 -and $px[$i + 1] -lt 90 -and $px[$i] -lt 90) { $sx += $x; $sy += $y; $sn++ }
    }
}
if ($sn -lt 20) { 'mosaic: the sonar arrow is not in the grab'; return }
$cx = $sx / $sn; $cy = $sy / $sn
$covered = 0
for ($a = 0; $a -lt 360; $a++) {
    $t = $a * [Math]::PI / 180; $lit = 0; $all = 0
    for ($r = 100; $r -le 250; $r += 10) {
        $x = [int]($cx + $r * [Math]::Cos($t)); $y = [int]($cy - $r * [Math]::Sin($t))
        if ($x -lt 0 -or $y -lt 0 -or $x -ge $w -or $y -ge $h) { continue }
        $i = $y * $stride + 4 * $x
        $all++
        if ([Math]::Max([Math]::Max($px[$i], $px[$i + 1]), $px[$i + 2]) -gt 60) { $lit++ }
    }
    if ($all -gt 0 -and $lit * 2 -gt $all) { $covered++ }
}
"mosaic: covers {0} of 360 deg at 100-250 px around the sonar ({1:N0}, {2:N0})" -f $covered, $cx, $cy
