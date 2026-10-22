#requires -Version 5.1
<#
.SYNOPSIS
    My maps at start (docs/map-export.md): with maps kept, the 3D view starts over the newest map shown, not where the
    camera was left; a survey opened then still takes the view to its data.

.DESCRIPTION
    Two KOGGER maps at two places: A, the map export of a recording (-File, exported here by the app itself,
    KOGGER_MAP_EXPORT), and B, the synthetic map of tools\mapexport_check (built and run with -Keep). Each run starts
    KoggerApp on the test desktop (no window, no focus taken) in a test profile of its own, written fresh (an INI file
    and the registry key QSettings("KOGGER", <app>) reads whatever the format): the camera saved over Moscow, the maps copied into the profile's maps folder with the times asked. The edit
    script's "viewCenter" logs where the middle of the 3D view looks; the expected centre is the middle of the map's
    passport bounds (map.json), read here, not from the app.
      newest     A older, B newer                    -> the view over B
      flipped    B older, A newer                    -> over A
      hidden     B newer but hidden in My maps       -> over A
      layer off  both, the My maps layer hidden      -> the saved camera (Moscow)
      no maps    none                                -> the saved camera
      survey     B newer, the recording opened too   -> the recording's data (inside A's bounds), not B
    Exit code 0 when every run holds.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\qml_check\mymaps_start_check.ps1 -Exe <build>\KoggerApp.exe -File D:\Temp\perf\00014_03.05.2025.KLF
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [Parameter(Mandatory = $true)] [string] $File,
    [string] $MapA = '',           # a map to use instead of exporting -File
    [string] $MapB = '',           # a map to use instead of the synthetic one
    [string] $PathPrefix = 'D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin',
    [string] $Profile = 'mapstart',
    [string] $Out = "$env:TEMP\kogger_mymaps_start",
    [int]    $TimeoutS = 180,
    [int]    $ExportTimeoutS = 900   # the export paints the whole mosaic: minutes on a loaded PC
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\test_desktop.ps1')   # the app on a desktop of its own: no window, no focus taken
Add-Type -AssemblyName System.IO.Compression.FileSystem
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$kogger = Join-Path $env:APPDATA 'KOGGER'
$utf8 = New-Object System.Text.UTF8Encoding $false

# Moscow: far from both maps.
$camLat = 55.7558; $camLon = 37.6173

# The profile, written fresh: the INI file (what QSettings() and QML's Settings read under KOGGER_SETTINGS_INI) and the
# registry key of the same name -- QSettings("KOGGER", <app>) is the registry whatever the default format: the saved
# camera, My maps' switches. $reg: @( @('mapLayer', 'visible', 'false'), ... ), each a group, a key and its value.
function Write-Profile([string] $name, [object[]] $reg) {
    $ini = Join-Path $kogger "KoggerApp-$name.ini"
    if (Test-Path -LiteralPath $ini) { Remove-Item -LiteralPath $ini -Force }
    [System.IO.File]::WriteAllText($ini, ((@('[General]', 'schemaVersion=8', '', '[main]', 'ui\welcomeShown=true') -join "`r`n") + "`r`n"), $utf8)
    $lat = $camLat * [math]::PI / 180; $lon = $camLon * [math]::PI / 180
    $ci = [Globalization.CultureInfo]::InvariantCulture
    $f = { param($v) ([double]$v).ToString('R', $ci) }
    $cam = @(@('refLatSin', (& $f ([math]::Sin($lat)))), @('refLatCos', (& $f ([math]::Cos($lat)))), @('refLatRad', (& $f $lat)),
             @('refLonRad', (& $f $lon)), @('refLlaLatitude', (& $f $camLat)), @('refLlaLongitude', (& $f $camLon)),
             @('refLlaAltitude', '0'), @('isInit', 'true'), @('lookAtN', '0'), @('lookAtE', '0'), @('distance', '800'),
             @('yaw', '0'), @('pitch', '0'))
    $values = @()
    foreach ($c in $cam) { $values += , @('scene3d\cameraView', $c[0], $c[1]) }
    foreach ($r in $reg) { $values += , $r }
    $key = "HKCU:\Software\KOGGER\KoggerApp-$name"
    if (Test-Path -LiteralPath $key) { Remove-Item -LiteralPath $key -Recurse -Force }
    foreach ($v in $values) {
        $k = Join-Path $key $v[0]
        if (-not (Test-Path -LiteralPath $k)) { New-Item -Path $k -Force | Out-Null }
        New-ItemProperty -LiteralPath $k -Name $v[1] -Value $v[2] -PropertyType String -Force | Out-Null
    }
    $maps = Join-Path $kogger "KoggerApp-$name\maps"
    if (Test-Path -LiteralPath $maps) { Remove-Item -LiteralPath $maps -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $maps | Out-Null
    return $maps
}

function Start-App([string] $name, [string[]] $steps, [string] $arguments, [hashtable] $envExtra, [string] $profileName = $Profile, [bool] $ini = $true,
                   [int] $timeout = $TimeoutS) {
    $scriptPath = Join-Path $Out "$name.json"
    ('{"live": true, "steps": [' + ($steps -join ",`n") + ']}') | Set-Content -LiteralPath $scriptPath -Encoding ASCII
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
    if ($arguments) { $psi.Arguments = $arguments }
    if ($PathPrefix) { $psi.EnvironmentVariables['PATH'] = $PathPrefix + ';' + $env:PATH }
    $psi.EnvironmentVariables['KOGGER_PROFILE'] = $profileName
    if ($ini) { $psi.EnvironmentVariables['KOGGER_SETTINGS_INI'] = '1' }
    $psi.EnvironmentVariables['KOGGER_ECHOEDIT_SCRIPT'] = $scriptPath
    if ($envExtra) { foreach ($k in $envExtra.Keys) { $psi.EnvironmentVariables[$k] = $envExtra[$k] } }
    $app = Start-TestDesktopProcess $psi
    if (-not $app.WaitForExit($timeout * 1000)) { $app.Kill(); throw "run '$name': the app did not end in $timeout s" }
    $log = Join-Path $kogger "KoggerApp-$profileName\AppLogs\kogger.log"
    $lines = Get-Content -LiteralPath $log -Encoding UTF8
    $start = ($lines | Select-String -Pattern 'log opened' | Select-Object -Last 1).LineNumber
    $mine = @($lines[($start - 1)..($lines.Count - 1)])
    $mine | Set-Content -LiteralPath (Join-Path $Out "$name.log") -Encoding UTF8
    return $mine
}

function Read-Bounds([string] $kmap) {
    $z = [IO.Compression.ZipFile]::OpenRead($kmap)
    try {
        $e = $z.GetEntry('map.json')
        $r = New-Object IO.StreamReader($e.Open(), $utf8)
        $j = $r.ReadToEnd() | ConvertFrom-Json
        $r.Dispose()
    } finally { $z.Dispose() }
    $b = $j.bounds
    return [pscustomobject]@{ W = [double]$b.west; S = [double]$b.south; E = [double]$b.east; N = [double]$b.north
                              Lat = ([double]$b.south + [double]$b.north) / 2; Lon = ([double]$b.west + [double]$b.east) / 2 }
}

function Metres($lat1, $lon1, $lat2, $lon2) {
    $k = [math]::PI / 180
    $x = ($lon2 - $lon1) * $k * [math]::Cos(($lat1 + $lat2) / 2 * $k) * 6371000
    $y = ($lat2 - $lat1) * $k * 6371000
    [math]::Sqrt($x * $x + $y * $y)
}

$failed = 0
function Expect([bool] $ok, [string] $what, [string] $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' })), $what, $detail
}

# --- the maps ----------------------------------------------------------------------------------------------------------
if (-not $MapA) {
    # In a profile copied from the test profile "perf" (the registry, as tools\perf\mapexport_edit_check.ps1 does): in a
    # fresh profile the recording's bottom track is not made, and an export without depths or mosaic writes nothing.
    $exportDir = Join-Path $Out 'export'
    if (Test-Path -LiteralPath $exportDir) { Remove-Item -LiteralPath $exportDir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $exportDir | Out-Null
    $exportProfile = $Profile + '_export'
    $key = "HKCU\Software\KOGGER\KoggerApp-$exportProfile"
    cmd.exe /c "reg delete `"$key`" /f >nul 2>&1"
    cmd.exe /c "reg copy `"HKCU\Software\KOGGER\KoggerApp-perf`" `"$key`" /s /f >nul 2>&1"
    if ($LASTEXITCODE -ne 0) { throw "no test profile perf to copy: reg import the transfer folder's KoggerApp-perf.reg first" }
    try {
        $null = Start-App 'export' @('{"op": "wait", "ms": 600000}') ('"' + $File + '"') @{
            KOGGER_MAP_EXPORT = $exportDir; KOGGER_MAP_EXPORT_DELAY_MS = '40000'; KOGGER_MAP_EXPORT_ZOOM = '6'
            KOGGER_MAP_EXPORT_QUIT = '1' } $exportProfile $false $ExportTimeoutS
    }
    finally {
        cmd.exe /c "reg delete `"$key`" /f >nul 2>&1"
    }
    $MapA = @(Get-ChildItem -LiteralPath $exportDir -Filter '*.kmap' | Select-Object -First 1 -ExpandProperty FullName)[0]
    if (-not $MapA) { throw "the export of $File wrote no .kmap (log: $(Join-Path $Out 'export.log'))" }
}
if (-not $MapB) {
    $keep = Join-Path $Out 'synthetic'
    New-Item -ItemType Directory -Force -Path $keep | Out-Null
    $null = & powershell -ExecutionPolicy Bypass -File (Join-Path $repo 'tools\mapexport_check\run.ps1') -Keep $keep
    $MapB = Join-Path $keep 'open.kmap'
    if (-not (Test-Path -LiteralPath $MapB)) { throw "tools\mapexport_check wrote no open.kmap into $keep" }
}
$A = Read-Bounds $MapA; $B = Read-Bounds $MapB
$apart = Metres $A.Lat $A.Lon $B.Lat $B.Lon
Expect ($apart -gt 500) 'two places' ("A {0:F5}, {1:F5}; B {2:F5}, {3:F5}; {4:F0} m apart" -f $A.Lat, $A.Lon, $B.Lat, $B.Lon, $apart)

function Run-Case([string] $name, [string[]] $mapsOrder, [object[]] $reg, [string] $arguments, [int] $waitMs) {
    $maps = Write-Profile $Profile $reg
    $t = (Get-Date).AddHours(-2)
    foreach ($m in $mapsOrder) {   # oldest first
        $src = $(if ($m -eq 'A') { $MapA } else { $MapB })
        $dst = Join-Path $maps "map$m.kmap"
        Copy-Item -LiteralPath $src -Destination $dst
        (Get-Item -LiteralPath $dst).LastWriteTime = $t
        $t = $t.AddMinutes(10)
    }
    $lines = Start-App $name @(('{"op": "wait", "ms": ' + $waitMs + '}'), '{"op": "viewCenter"}', '{"op": "quit"}') $arguments $null
    $c = $lines | Select-String -Pattern 'echoedit script: viewCenter lat (-?[\d.]+) lon (-?[\d.]+) valid (\d)' | Select-Object -Last 1
    $lib = ($lines | Select-String -Pattern 'map library: (\d+) maps in' | Select-Object -Last 1)
    $to = ($lines | Select-String -Pattern 'map library: the view to (\S+),' | Select-Object -Last 1)
    return [pscustomobject]@{
        Valid = [bool]($c -and $c.Matches[0].Groups[3].Value -eq '1')
        Lat = $(if ($c) { [double]::Parse($c.Matches[0].Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture) } else { [double]::NaN })
        Lon = $(if ($c) { [double]::Parse($c.Matches[0].Groups[2].Value, [Globalization.CultureInfo]::InvariantCulture) } else { [double]::NaN })
        Maps = $(if ($lib) { [int]$lib.Matches[0].Groups[1].Value } else { -1 })
        ViewTo = $(if ($to) { $to.Matches[0].Groups[1].Value } else { '' })
    }
}

function Near($r, $lat, $lon, [double] $tol) {
    $r.Valid -and (Metres $r.Lat $r.Lon $lat $lon) -le $tol
}
function Show($r) { "view {0:F5}, {1:F5} (valid {2}), {3} maps loaded, the view to '{4}'" -f $r.Lat, $r.Lon, $r.Valid, $r.Maps, $r.ViewTo }
$tolA = [math]::Max(5.0, 0.02 * (Metres $A.S $A.W $A.N $A.E))
$tolB = [math]::Max(5.0, 0.02 * (Metres $B.S $B.W $B.N $B.E))

$r = Run-Case 'newest' @('A', 'B') @() '' 3000
Expect ((Near $r $B.Lat $B.Lon $tolB) -and $r.Maps -eq 2 -and $r.ViewTo -eq 'mapB.kmap') 'newest: over B' (Show $r)
$r = Run-Case 'flipped' @('B', 'A') @() '' 3000
Expect ((Near $r $A.Lat $A.Lon $tolA) -and $r.ViewTo -eq 'mapA.kmap') 'flipped: over A' (Show $r)
$r = Run-Case 'hidden' @('A', 'B') @(, @('mapLibrary', 'hidden', 'mapB.kmap')) '' 3000
Expect ((Near $r $A.Lat $A.Lon $tolA) -and $r.ViewTo -eq 'mapA.kmap') 'hidden newest: over A' (Show $r)
$r = Run-Case 'layeroff' @('A', 'B') @(, @('mapLayer', 'visible', 'false')) '' 3000
Expect ((Near $r $camLat $camLon 5.0) -and $r.ViewTo -eq '') 'layer off: the saved camera' (Show $r)
$r = Run-Case 'nomaps' @() @() '' 3000
Expect ((Near $r $camLat $camLon 5.0) -and $r.Maps -eq 0 -and $r.ViewTo -eq '') 'no maps: the saved camera' (Show $r)
$r = Run-Case 'survey' @('A', 'B') @() ('"' + $File + '"') 25000
$inA = $r.Valid -and $r.Lat -ge $A.S - 0.002 -and $r.Lat -le $A.N + 0.002 -and $r.Lon -ge $A.W - 0.002 -and $r.Lon -le $A.E + 0.002
Expect ($inA -and $r.ViewTo -eq 'mapB.kmap') 'a survey opened: its data, not B' ((Show $r) + ("; A's bounds {0:F5}..{1:F5}, {2:F5}..{3:F5}" -f $A.S, $A.N, $A.W, $A.E))

# The test profile goes: its registry key, its INI file (the logs stay with the profile's data folder).
Remove-Item -LiteralPath "HKCU:\Software\KOGGER\KoggerApp-$Profile" -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path $kogger "KoggerApp-$Profile.ini") -Force -ErrorAction SilentlyContinue

"mymaps start: {0} (logs in {1})" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" })), $Out
exit ([int]($failed -gt 0))
