#requires -Version 5.1
<#
.SYNOPSIS
    No check under tools\ starts a program where it can show a window to whoever works at the machine.

.DESCRIPTION
    The owner, 2026-10-22: the checks' app windows kept coming up over his work and taking the focus.
    The app goes through test_desktop.ps1 (a desktop of its own); qmltestrunner runs with -platform
    offscreen. Flagged in every .ps1 under tools\ (test_desktop.ps1 itself aside):
      - [System.Diagnostics.Process]::Start(   use Start-TestDesktopProcess
      - .MainWindowHandle                       sees the caller's desktop only: Get-TestDesktopWindow
      - Start-Process without -platform offscreen in that call (its line and the continuation lines)
    Exit code 0 when nothing is flagged.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\common\visible_runs_lint.ps1
#>
[CmdletBinding()]
param([string] $Root)   # tools\ by default

$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = Join-Path $PSScriptRoot '..' }   # PowerShell 5.1 has no $PSScriptRoot in param defaults
$flagged = 0
$files = Get-ChildItem -Path $Root -Filter *.ps1 -Recurse | Where-Object { $_.Name -ne 'test_desktop.ps1' -and $_.FullName -ne $PSCommandPath }
foreach ($f in $files) {
    $lines = @(Get-Content -LiteralPath $f.FullName -Encoding UTF8)
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $l = $lines[$i]
        if ($l -match '^\s*#') { continue }
        $why = $null
        if ($l -match '\[System\.Diagnostics\.Process\]::Start\(') { $why = 'Process.Start: use Start-TestDesktopProcess' }
        elseif ($l -match '\.MainWindowHandle') { $why = 'MainWindowHandle: use Get-TestDesktopWindow' }
        elseif ($l -match '\bStart-Process\b') {
            $call = $l; $j = $i
            while ($lines[$j] -match '`\s*$' -and $j + 1 -lt $lines.Count) { $j++; $call += ' ' + $lines[$j] }
            if ($call -notmatch "'-platform',\s*'offscreen'|-platform offscreen") { $why = 'Start-Process without -platform offscreen' }
        }
        if ($why) {
            $flagged++
            "{0}:{1}: {2}" -f $f.FullName.Substring((Resolve-Path $Root).Path.Length).TrimStart('\'), ($i + 1), $why
        }
    }
}
"visible runs lint: {0} file(s), {1}" -f $files.Count, $(if ($flagged -eq 0) { 'PASS' } else { "$flagged flagged" })
exit ([int]($flagged -gt 0))
