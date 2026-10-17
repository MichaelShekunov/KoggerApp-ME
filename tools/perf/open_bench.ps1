#requires -Version 5.1
<#
.SYNOPSIS
    Opens a log file in KoggerApp the way a double click does (the path as the first argument) and
    prints, from kogger.log, when each stage of the open happened, in seconds from the app's start.

.DESCRIPTION
    The app runs with KOGGER_PROFILE=<Profile> (settings and a log of their own, beside the user's open
    app; a profile has no pinned links, so no serial port is opened), grabs its window
    (KOGGER_UI_PROBE_PNG) after -Seconds and quits. The lines printed are the session's: the open, the
    "perf:" stage lines, the stall and health lines. Copy a file out of a synced folder first if the
    app may write beside it.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\perf\open_bench.ps1 -Exe <build>\KoggerApp.exe -File D:\Temp\perf\a.KLF -Profile perf
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [Parameter(Mandatory = $true)] [string] $File,
    [string] $Profile = 'perf',
    [double] $Seconds = 60,
    [string] $Out = "$env:TEMP\kogger_open_bench",
    [string] $PathPrefix,          # a build tree's exe needs the Qt and toolchain bin folders on PATH
    [string] $Pattern = 'log opened|File opened|file opened|perf:|stall|health|mosaic view|failed|error',
    [switch] $Threads,             # every second, the threads that used the CPU (ids in hex, as kogger.log tags them)
    [switch] $Memory,              # every 5 s the process's private bytes and working set; the peak at the end
    [string] $WheelAt = '0',       # this many seconds in, -Wheel notches of the mouse wheel at the 3D view's middle
    [int]    $Wheel = 0            # (posted to the window; the cursor stays): positive zooms in. Several times
                                   # apart, "30,40,50": a few notches move the camera by a level at most
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
$psi.Arguments = '"' + $File + '"'
if ($PathPrefix) { $psi.EnvironmentVariables['PATH'] = $PathPrefix + ';' + $env:PATH }
$psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
$psi.EnvironmentVariables['KOGGER_UI_PROBE'] = $Out
$psi.EnvironmentVariables['KOGGER_UI_PROBE_DELAY_MS'] = [string][int]($Seconds * 1000)
$psi.EnvironmentVariables['KOGGER_UI_PROBE_QUIT'] = '1'
$psi.EnvironmentVariables['KOGGER_UI_PROBE_PNG'] = '1'
$clock = [System.Diagnostics.Stopwatch]::StartNew()
$app = [System.Diagnostics.Process]::Start($psi)
if ($Wheel -ne 0) {
    Add-Type -Namespace KoggerPerf -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool PostMessage(System.IntPtr h, uint msg, System.IntPtr w, System.IntPtr l);
[DllImport("user32.dll")] public static extern bool ClientToScreen(System.IntPtr h, ref POINT p);
public struct POINT { public int X, Y; }
'@
}
$wheelTimes = @($WheelAt -split ',' | ForEach-Object { [double]::Parse($_.Trim(), [Globalization.CultureInfo]::InvariantCulture) })
$wheelNext = 0
$wheeled = ($Wheel -eq 0)
[long]$memPeak = 0; [long]$memLast = 0; [long]$wsPeak = 0
if ($Threads -or $Memory -or -not $wheeled) {
    $prev = @{}
    while (-not $app.HasExited) {
        Start-Sleep -Milliseconds 1000
        try { $app.Refresh(); $ths = $app.Threads } catch { break }
        $busy = foreach ($th in $ths) {
            try { $cpu = $th.TotalProcessorTime.TotalMilliseconds } catch { continue }
            $was = if ($prev.ContainsKey($th.Id)) { $prev[$th.Id] } else { 0 }
            $prev[$th.Id] = $cpu
            if ($cpu - $was -ge 100) { '{0:x}:{1:N0}' -f $th.Id, ($cpu - $was) }
        }
        if ($Threads -and $busy) { "{0,6:N1} s  cpu ms by thread  {1}" -f $clock.Elapsed.TotalSeconds, ($busy -join '  ') }
        if ($Memory) {
            try { $memLast = $app.PrivateMemorySize64; $ws = $app.WorkingSet64 } catch { $memLast = 0; $ws = 0 }
            $memPeak = [Math]::Max($memPeak, $memLast); $wsPeak = [Math]::Max($wsPeak, $ws)
            if ([int]$clock.Elapsed.TotalSeconds % 5 -eq 0) {
                "{0,6:N1} s  memory: private {1:N0} MB, working set {2:N0} MB" -f $clock.Elapsed.TotalSeconds, ($memLast / 1MB), ($ws / 1MB)
            }
        }
        if (-not $wheeled -and $clock.Elapsed.TotalSeconds -ge $wheelTimes[$wheelNext]) {
            $wheelNext++
            $wheeled = ($wheelNext -ge $wheelTimes.Count)
            $h = $app.MainWindowHandle
            $pt = New-Object KoggerPerf.Win+POINT
            $pt.X = 360; $pt.Y = 360                       # the 3D view is the window's left half
            [void][KoggerPerf.Win]::ClientToScreen($h, [ref]$pt)
            $l = [System.IntPtr](($pt.Y -shl 16) -bor ($pt.X -band 0xFFFF))
            $notch = if ($Wheel -gt 0) { 120 } else { -120 }
            for ($n = 0; $n -lt [Math]::Abs($Wheel); $n++) {
                [void][KoggerPerf.Win]::PostMessage($h, 0x020A, [System.IntPtr](($notch -band 0xFFFF) -shl 16), $l)
                Start-Sleep -Milliseconds 150
            }
            "{0,6:N1} s  wheel {1} notches posted" -f $clock.Elapsed.TotalSeconds, $Wheel
        }
    }
}
if (-not $app.WaitForExit([int](($Seconds + 120) * 1000))) { throw 'the app did not quit after its window grab' }
"app ran {0:N1} s, exit {1}" -f $clock.Elapsed.TotalSeconds, $app.ExitCode
if ($Memory) { "memory: private peak {0:N0} MB, last {1:N0} MB; working set peak {2:N0} MB" -f ($memPeak / 1MB), ($memLast / 1MB), ($wsPeak / 1MB) }

$log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"
$lines = Get-Content -LiteralPath $log -Encoding UTF8
$start = ($lines | Select-String -Pattern 'log opened' | Select-Object -Last 1).LineNumber
$t0 = $null
foreach ($line in $lines[($start - 1)..($lines.Count - 1)]) {
    if ($line -notmatch $Pattern) { continue }
    if ($line -match '^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3}) ') {
        $t = [datetime]::ParseExact($Matches[1], 'yyyy-MM-dd HH:mm:ss.fff', $null)
        if (-not $t0) { $t0 = $t }
        "{0,7:N2} s  {1}" -f ($t - $t0).TotalSeconds, ($line -replace '^\S+ \S+ ', '')
    }
}
"window: {0}" -f (Join-Path $Out 'ui-auto.png')
