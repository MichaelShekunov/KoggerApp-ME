#requires -Version 5.1
<#
.SYNOPSIS
    The map export of an edited recording: pings marked bad or trimmed on the echogram are out of the KMZ's track
    and the survey's times, as they are out of its mosaic and depths. docs/map-export.md, docs/echogram-editing.md.

.DESCRIPTION
    Two windows of the app, each exporting without a click and closing itself (about a minute each on a 350 MB
    recording; KOGGER_MAP_EXPORT, KOGGER_MAP_EXPORT_QUIT): the recording as it is, then with a file of edits
    next to it that trims the first -TrimStart pings and marks -BadFrom up to (not including) -BadTo bad. Both export a KMZ with the
    track (writeKmz and hideTrackAndTime off in a profile copied from the test profile "perf").

    Checked:
      left out   the edited run's log says how many pings the edits left out: -TrimStart plus the bad range
      track      the edited run's track has fewer points than the plain run's and starts elsewhere
    A build that takes the track from every ping fails "track" (on 00014: 1665 points both ways, the same first
    point). The recording's own file of edits, if any, is put aside and back; a file put aside by a run that did
    not finish stops the check, to be put back by hand. Exit code 0 when both pass, 1 otherwise.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\perf\mapexport_edit_check.ps1 -Exe <build>\KoggerApp.exe -File D:\Temp\perf\00014_03.05.2025.KLF -Epochs 57079 -PathPrefix D:\Qt\6.8.3\llvm-mingw_64\bin
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [Parameter(Mandatory = $true)] [string] $File,
    [Parameter(Mandatory = $true)] [int] $Epochs,   # the recording's epochs as KoggerApp reads it: 57079 for 00014
    [int] $TrimStart = 2924,                         # 00014: the boat out of the water before it
    [int] $BadFrom = 22832,
    [int] $BadTo = 23973,
    [string] $Profile = 'mapexport_edit',
    [string] $Out = "$env:TEMP\kogger_mapexport_edit",
    [string] $PathPrefix,
    [int] $TimeoutS = 300
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\test_desktop.ps1')   # the app on a desktop of its own: no window, no focus taken
Add-Type -AssemblyName System.IO.Compression.FileSystem
$File = (Resolve-Path -LiteralPath $File).Path
$kedit = "$File.kedit"
$kept = "$File.kedit.mapexportcheck-kept"
$key = "HKCU\Software\KOGGER\KoggerApp-$Profile"
$log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"
if (Test-Path -LiteralPath $Out) { Remove-Item -LiteralPath $Out -Recurse -Force }
New-Item -ItemType Directory -Force -Path $Out | Out-Null

$script:fail = 0
function Check([bool] $ok, [string] $what, [string] $detail = '') {
    if ($ok) { Write-Host "PASS $what $(if ($detail) { "($detail)" })" }
    else { $script:fail++; Write-Host "FAIL $what $(if ($detail) { ": $detail" })" }
}

# One export; the track's points (lon,lat,alt each) and the log's "left out" count.
function Export([string] $tag) {
    $eap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'   # reg.exe's messages are not errors here
    & reg.exe delete $key /f *> $null
    & reg.exe copy 'HKCU\Software\KOGGER\KoggerApp-perf' $key /s /f *> $null
    $rc = $LASTEXITCODE
    & reg.exe add "$key\mapExport" /v writeKmz /t REG_SZ /d true /f *> $null
    & reg.exe add "$key\mapExport" /v hideTrackAndTime /t REG_SZ /d false /f *> $null
    $ErrorActionPreference = $eap
    if ($rc -ne 0) { throw "could not copy the test profile perf to $Profile" }
    $dir = Join-Path $Out $tag
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $before = if (Test-Path -LiteralPath $log) { (Get-Item -LiteralPath $log).Length } else { 0 }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
    $psi.Arguments = '"' + $File + '"'
    if ($PathPrefix) { $psi.EnvironmentVariables['PATH'] = $PathPrefix + ';' + $env:PATH }
    $psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
    $psi.EnvironmentVariables['KOGGER_MAP_EXPORT'] = $dir
    $psi.EnvironmentVariables['KOGGER_MAP_EXPORT_QUIT'] = '1'
    $app = Start-TestDesktopProcess $psi
    if (-not $app.WaitForExit($TimeoutS * 1000)) { $app.Kill(); throw "run $tag did not end in $TimeoutS s" }
    $fs = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite'); [void]$fs.Seek($before, 'Begin')
    $text = (New-Object System.IO.StreamReader($fs)).ReadToEnd(); $fs.Close()
    $line = @($text -split "`n" | Where-Object { $_ -match 'map export: \d+ epochs' } | Select-Object -Last 1)
    $left = if ($line.Count -and $line[0] -match '\((\d+) left out by echogram edits\)') { [int]$Matches[1] } else { -1 }
    $kmz = Get-ChildItem -LiteralPath $dir -Filter *.kmz -Recurse | Select-Object -First 1
    $pts = @()
    if ($kmz) {
        $zip = [System.IO.Compression.ZipFile]::OpenRead($kmz.FullName)
        try {
            $entry = $zip.Entries | Where-Object { $_.Name -like '*.kml' } | Select-Object -First 1
            $kml = (New-Object System.IO.StreamReader($entry.Open())).ReadToEnd()
        } finally { $zip.Dispose() }
        $m = [regex]::Match($kml, '(?s)<Placemark><name>Track</name>.*?<coordinates>(.*?)</coordinates>')
        if ($m.Success) { $pts = @($m.Groups[1].Value.Trim() -split '\s+' | Where-Object { $_ }) }
    }
    Write-Host ("run {0}: {1}; KMZ {2}, track points {3}{4}" -f $tag, $(if ($line.Count) { ($line[0] -replace '^.*?map export: ', '').Trim() } else { 'no export line' }),
        $(if ($kmz) { $kmz.Name } else { 'none' }), $pts.Count, $(if ($pts.Count) { ", first $($pts[0])" } else { '' }))
    return [pscustomobject]@{ Left = $left; Track = $pts }
}

if (Test-Path -LiteralPath $kept) {
    throw "a run that did not finish left $kept -- the recording's own edits, or this check's; put it back as $kedit by hand (or remove it), then run again"
}
if (Test-Path -LiteralPath $kedit) { Move-Item -LiteralPath $kedit -Destination $kept; Write-Host "kept aside: $kedit" }
$hadBak = Test-Path -LiteralPath "$kedit.bak"
try {
    $plain = Export 'plain'
    $bytes = (Get-Item -LiteralPath $File).Length
    # Written by hand: PowerShell 5.1's ConvertTo-Json can wrap a nested array as {"value": [...], "Count": n}.
    $name = [System.IO.Path]::GetFileName($File).Replace('\', '\\').Replace('"', '\"')
    $json = ('{{"kind": "KoggerApp echogram edits", "version": 1, "record": "{0}", "recordBytes": {1}, "epochs": {2}, ' +
             '"trim": {{"start": {3}, "end": null}}, "bad": [[{4}, {5}]]}}') -f $name, $bytes, $Epochs, $TrimStart, $BadFrom, $BadTo
    [System.IO.File]::WriteAllText($kedit, $json, (New-Object System.Text.UTF8Encoding $false))
    $edited = Export 'edited'
    $expect = $TrimStart + ($BadTo - $BadFrom)   # a range of edits ends before its "to": 2924 + 1141 on 00014
    Check ($plain.Left -eq 0 -and $edited.Left -eq $expect) 'left out' "plain $($plain.Left), edited $($edited.Left), expected $expect"
    Check ($plain.Track.Count -gt 0 -and $edited.Track.Count -gt 0 -and $edited.Track.Count -lt $plain.Track.Count -and $edited.Track[0] -ne $plain.Track[0]) `
        'track' "points $($edited.Track.Count) against $($plain.Track.Count), the first point $(if ($edited.Track.Count -and $plain.Track.Count -and $edited.Track[0] -ne $plain.Track[0]) { 'moved' } else { 'the same' })"
}
finally {
    if (Test-Path -LiteralPath $kedit) { Remove-Item -LiteralPath $kedit -Force }   # this check's
    if (-not $hadBak -and (Test-Path -LiteralPath "$kedit.bak")) { Remove-Item -LiteralPath "$kedit.bak" -Force }
    if (Test-Path -LiteralPath $kept) { Move-Item -LiteralPath $kept -Destination $kedit; Write-Host "put back: $kedit" }
}
Write-Host ("mapexport_edit_check: {0}" -f $(if ($script:fail) { "$($script:fail) failed" } else { 'all passed' }))
exit ([int]($script:fail -gt 0))
