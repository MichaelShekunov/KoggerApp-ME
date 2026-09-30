<#
.SYNOPSIS
  Static check of the device-switch rule on qml/app/DeviceSettingsPage.qml.

.DESCRIPTION
  One page serves every device; only dev changes on a switch. On 2026-09-29 "Apply and save" took the fields left
  from one Wi-Fi module's page as edits for the other one (the boat's module got the head unit's address). The rule:
    1. every pending condition pendingSteps() collects is gated by an edit flag the user sets here (dirty, netDirty,
       apDirty, ownAddrEdited), or is read anew on the switch (the port speed pick, the USB network fields);
    2. every island's edit flag is reset in _forgetEdits (a Repeater delegate's is made anew with the model), and
       every own-address field is cleared there;
    3. onDevChanged calls _forgetEdits, which stops a running "Apply and save".
  Exit code 0 when all hold, 1 otherwise.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\qml_check\devswitch_check.ps1
#>
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$utf8 = New-Object System.Text.UTF8Encoding $false
$text = [System.IO.File]::ReadAllText((Join-Path $repo 'qml\app\DeviceSettingsPage.qml'), $utf8)
$lines = $text -split "`r?`n"
$bad = 0

# The text from the first line matching $pattern through its closing brace.
function Get-Body([string]$pattern) {
    for ($i = 0; $i -lt $lines.Length; $i++) {
        if ($lines[$i] -match $pattern) {
            $depth = 0
            for ($j = $i; $j -lt $lines.Length; $j++) {
                $s = $lines[$j] -replace '"[^"]*"', '""'
                $c = $s.IndexOf('//')
                if ($c -ge 0) { $s = $s.Substring(0, $c) }
                $depth += ($s.ToCharArray() | Where-Object { $_ -eq '{' }).Count
                $depth -= ($s.ToCharArray() | Where-Object { $_ -eq '}' }).Count
                if ($depth -le 0) { return ($lines[$i..$j] -join "`n") }
            }
        }
    }
    return ''
}

function Report([bool]$ok, [string]$what) {
    Write-Host ('{0,-4} {1}' -f $(if ($ok) { 'ok' } else { 'BAD' }), $what)
    if (-not $ok) { $script:bad++ }
}

$forget = Get-Body 'function _forgetEdits\(\)'
$reread = Get-Body 'function _readNewDevice\(\)'
$devch = Get-Body '^\s*onDevChanged: \{'

# the islands by the line they start on, to name where a property lives
$starts = @()
for ($i = 0; $i -lt $lines.Length; $i++) {
    if ($lines[$i] -match '^\s*(DevIsland|KIsland|DeviceSettingsGroup)\s*\{') {
        for ($k = $i + 1; $k -lt [Math]::Min($i + 6, $lines.Length); $k++) {
            if ($lines[$k] -match '^\s*id:\s*(\w+)') { $starts += , @($i, $Matches[1]); break }
        }
    }
}
function Get-Owner([int]$lineNo) {
    $cur = ''
    foreach ($s in $starts) { if ($s[0] -le $lineNo) { $cur = $s[1] } }
    return $cur
}

# 1. every pending condition gated by an edit, or read anew on the switch
for ($i = 0; $i -lt $lines.Length; $i++) {
    if ($lines[$i] -notmatch '^\s*readonly property bool (pendingApply|netPending|ratePending|rolePending|apPending|ownAddrPending):(.*)$') { continue }
    $name = $Matches[1]
    $expr = $Matches[2]
    for ($j = $i + 1; $j -lt $lines.Length -and $lines[$j].Trim().StartsWith('&&'); $j++) { $expr += $lines[$j] }
    $isl = Get-Owner $i
    $gated = $expr -match '\b(dirty|netDirty|apDirty|ownAddrEdited)\b'
    if ($isl -eq 'portIsland' -and $name -eq 'pendingApply') { $gated = $forget.Contains('portIsland.sel = 0') }
    if ($isl -eq 'devNetGroup') { $gated = $reread.Contains('devNetGroup._sync(true)') -and $forget.Contains('_readNewDevice') }
    Report $gated ('{0,-14} {1,-15} {2}' -f $isl, $name, $(if ($gated) { 'gated' } else { 'NOT gated: ' + $expr.Trim() }))
}

# 2. edit flags reset, own-address fields cleared
for ($i = 0; $i -lt $lines.Length; $i++) {
    if ($lines[$i] -notmatch '^(\s*)property bool (dirty|apDirty|ownAddrEdited)\s*:') { continue }
    $indent = $Matches[1].Length
    $flag = $Matches[2]
    $isl = Get-Owner $i
    if (-not $isl) { continue }
    if ($indent -gt 8) {
        Report $true ('{0,-14} {1,-15} in a delegate (made anew with the model)' -f $isl, $flag)
        continue
    }
    $ok = $forget.Contains("$isl.$flag = false")
    Report $ok ('{0,-14} {1,-15} {2}' -f $isl, $flag, $(if ($ok) { 'reset' } else { 'not reset in _forgetEdits' }))
}
foreach ($m in [regex]::Matches($text, 'parseInt\((\w+)\.text, 10\) !== dev\.wifiNetOwnAddr')) {
    $field = $m.Groups[1].Value
    $ok = $forget.Contains("$field.text = """"")
    Report $ok ('{0,-14} {1,-15} {2}' -f $field, 'text', $(if ($ok) { 'cleared' } else { 'not cleared in _forgetEdits' }))
}

# 3. the switch calls it; a running "Apply and save" stops
Report $devch.Contains('_forgetEdits()') 'onDevChanged calls _forgetEdits'
Report ($forget.Contains('applyStepTimer.stop()') -and $forget.Contains('applying = false')) '_forgetEdits stops a running Apply and save'

if ($bad -eq 0) { Write-Host 'devswitch_check: all hold'; exit 0 }
Write-Host "devswitch_check: $bad BAD"
exit 1
