#requires -Version 5.1
<#
.SYNOPSIS
    The supply-voltage path through the running application: ID_VOLTAGE frames in over a UDP link,
    the reading in the Dataset (the widget's "Sonar supply" field reads it from there).

.DESCRIPTION
    Two runs of the app, each with KOGGER_AUTOTEST=UDP:<Port> (a hidden UDP link, never saved) in the
    profile -Profile (its own settings and log). Frames are ID_VOLTAGE v0 CONTENT as the nanoSSS
    firmware sends them every ping: u8 channel, s32 value.
      control: only the diagnostic channels 1..4 -- the Dataset must log no "sonar supply" line;
      reading: the same plus channel 0 = -Millivolts -- the Dataset must log
               "sonar supply: first reading <Millivolts/1000> V" (Dataset::addSupplyVoltage).
    The app quits by itself (KOGGER_UI_PROBE_QUIT). Exit 0 = both hold, 1 = not, 2 = could not run.
    Keep the mouse off the test window; the user's own app may stay open (another profile).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\voltage_check\voltage_live.ps1 -Exe D:\Temp\kogger_merge_test\build\Desktop_Qt_6_8_3_llvm_mingw_64_bit-Release\KoggerApp.exe -PathPrefix 'D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin'
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [string] $PathPrefix,
    [string] $Profile = 'perf',
    [int]    $Port = 23461,
    [int]    $Millivolts = 12345,
    [double] $Seconds = 5,
    [string] $Out = "$env:TEMP\kogger_voltage_live"
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\test_desktop.ps1')   # the app on a desktop of its own: no window, no focus taken

function Get-Frame([int] $channel, [int] $value) {
    $payload = New-Object byte[] 5
    $payload[0] = [byte]$channel
    [Array]::Copy([BitConverter]::GetBytes([int32]$value), 0, $payload, 1, 4)
    $body = New-Object System.Collections.Generic.List[byte]
    $body.Add(0); $body.Add(0x01); $body.Add(0x31); $body.Add([byte]$payload.Length)   # route, CONTENT v0, ID_VOLTAGE, length
    $body.AddRange($payload)
    $c1 = 0; $c2 = 0
    foreach ($b in $body) { $c1 = ($c1 + $b) -band 0xFF; $c2 = ($c2 + $c1) -band 0xFF }
    $frame = New-Object System.Collections.Generic.List[byte]
    $frame.Add(0xBB); $frame.Add(0x55); $frame.AddRange($body); $frame.Add([byte]$c1); $frame.Add([byte]$c2)
    return ,($frame.ToArray())
}

# One run of the app; returns its session's lines of kogger.log.
function Invoke-Run([bool] $withSupply) {
    New-Item -ItemType Directory -Force -Path $Out | Out-Null
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
    if ($PathPrefix) { $psi.EnvironmentVariables['PATH'] = $PathPrefix + ';' + $env:PATH }
    $psi.EnvironmentVariables['KOGGER_AUTOTEST'] = "UDP:$Port"
    $psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
    $psi.EnvironmentVariables['KOGGER_UI_PROBE'] = $Out
    $psi.EnvironmentVariables['KOGGER_UI_PROBE_DELAY_MS'] = [string][int](($Seconds + 8) * 1000)
    $psi.EnvironmentVariables['KOGGER_UI_PROBE_QUIT'] = '1'
    $app = Start-TestDesktopProcess $psi
    $udp = New-Object System.Net.Sockets.UdpClient
    try {
        Start-Sleep -Seconds 4                                    # the link opens about a second after start
        $clock = [System.Diagnostics.Stopwatch]::StartNew()
        $k = 0
        while ($clock.Elapsed.TotalSeconds -lt $Seconds) {        # ten "pings" a second
            $frames = @((Get-Frame 1 0x47), (Get-Frame 2 0x30), (Get-Frame 3 4025), (Get-Frame 4 700))
            if ($withSupply) { $frames = @(Get-Frame 0 ($Millivolts + ($k % 3))) + $frames }
            foreach ($f in $frames) { [void]$udp.Send($f, $f.Length, '127.0.0.1', $Port) }
            $k++
            Start-Sleep -Milliseconds 100
        }
    }
    finally { $udp.Close() }
    if (-not $app.WaitForExit(60000)) { $app.Kill(); throw 'the app did not quit' }
    $log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"
    $lines = Get-Content -LiteralPath $log -Encoding UTF8
    $start = ($lines | Select-String -Pattern 'log opened' | Select-Object -Last 1).LineNumber
    return ,($lines[($start - 1)..($lines.Count - 1)])
}

try {
    $control = Invoke-Run $false
    $reading = Invoke-Run $true
}
catch { "could not run: $($_.Exception.Message)"; exit 2 }

$bad = 0
$c = @($control | Where-Object { $_ -match 'sonar supply:' })
"control (channels 1..4 only): {0} 'sonar supply' line(s) -- {1}" -f $c.Count, $(if ($c.Count -eq 0) { 'ok' } else { $bad++; 'FAIL' })
$want = ('{0:0.000}' -f ($Millivolts / 1000.0)).Replace(',', '.')
$r = @($reading | Where-Object { $_ -match 'sonar supply: first reading ([\d.]+) V' })
$got = if ($r.Count -gt 0 -and $r[0] -match 'first reading ([\d.]+) V') { $Matches[1] } else { '' }
"reading (channel 0 = $Millivolts mV): '{0}' -- {1}" -f $(if ($r.Count) { $r[0] -replace '^\S+ \S+ ', '' } else { 'no line' }), $(if ($got -eq $want) { 'ok' } else { $bad++; "FAIL, want $want V" })
if ($bad) { 'FAILED'; exit 1 }
'PASS'
exit 0
