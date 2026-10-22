#requires -Version 5.1
<#
.SYNOPSIS
    Widgets docked to the window's edges, in the app (docs/widget-docking.ru.md): where they go, what the workspace
    gives up, the refusal, the tab's collapse, each layout's own docks, and all of it across a restart.

.DESCRIPTION
    Three runs of KoggerApp on the test desktop (no window, no focus taken), in a test profile of its own
    (KOGGER_PROFILE, an INI file under %APPDATA%\KOGGER written fresh before the first run: four widgets, two layouts,
    no docks), driven by the edit script's dock steps ("dock", "undock", "dockScale", "dockCollapse", "layout") and
    read back through its "docks" step, which logs the store's docks and what is on screen.

    Every snapshot is held to the same rules:
      - a docked widget's panel is drawn exactly in its slot; in a collapsed dock it is not drawn;
      - the workspace is the window less the docks;
      - a floating panel stays within the workspace's area;
      - the docks leave the workspace half the window each way.
    And the scenario:
      run 1  fresh: no docks. Two widgets to the right edge (the second put first), the settings widget to the bottom;
             the right dock to scale 1.5 (set or refused as its scale steps say); collapsed by the tab's toggle.
             Layout 0: no docks there; a widget docked in layout 1 to its top, that dock to 2.5, the 4x4 widget to
             the bottom: each docked or refused as the window and the widgets' sizes say (worked out here as
             DockLayout does), a refusal changing nothing, and at least one refusal. Back to layout 1: its docks kept.
      run 2  after a restart: both layouts' docks as run 1 left them. The right dock opened again, a widget undocked.
      run 3  after a restart: that too kept.
    Pictures of the window (the UI probe's grab) go to -Out for a look. Exit code 0 when every check holds.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\qml_check\dock_e2e.ps1 -Exe <build>\KoggerApp.exe
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [string] $PathPrefix = 'D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin',
    [string] $Profile = 'dockcheck',
    [string] $Out = "$env:TEMP\kogger_dock_e2e",
    [int]    $TimeoutS = 120
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\test_desktop.ps1')   # the app on a desktop of its own: no window, no focus taken
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$ini = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile.ini"
$log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"

# The profile: four widgets shown (a 1x4 and a 2x2 grid, a settings widget, a 4x4 grid), two layouts (a single
# echogram; 3D | echogram, the active one), no docks. Strings as QSettings writes them to an INI file.
function Ini-String([string] $s) { '"' + $s.Replace('\', '\\').Replace('"', '\"') + '"' }
$cell = { param($r, $c, $f) '{"row":' + $r + ',"col":' + $c + ',"field":"' + $f + '","rep":"value","big":false}' }
$fields = @('temp', 'sonarVoltage', 'apVoltage', 'linkAir', 'apHomeDist', 'sysOverview')
$big = @(); for ($r = 0; $r -lt 4; $r++) { for ($c = 0; $c -lt 4; $c++) { $big += & $cell $r $c $fields[($r * 4 + $c) % 6] } }
# Each element in parentheses: PowerShell's comma binds tighter than +, and would make an array of the pieces.
$defs = @()
$defs += ('{"id":"w_dock_a","kind":"grid","name":"","cols":1,"rows":4,"transparency":0,"cells":[' + ((0..3 | ForEach-Object { & $cell $_ 0 $fields[$_] }) -join ',') + ']}')
$defs += ('{"id":"w_dock_b","kind":"grid","name":"","cols":2,"rows":2,"transparency":0,"cells":[' + (@((& $cell 0 0 'temp'), (& $cell 0 1 'sonarVoltage'), (& $cell 1 0 'apHomeDist'), (& $cell 1 1 'sysOverview')) -join ',') + ']}')
$defs += '{"id":"w_dock_s","kind":"settings","name":"Settings 1","transparency":0,"items":[{"key":"dev:NanoSSS:trans.pulse","label":"Pulse count"},{"key":"dev:NanoSSS:trans.freq","label":"Frequency, kHz"}]}'
$defs += ('{"id":"w_dock_big","kind":"grid","name":"","cols":4,"rows":4,"transparency":0,"cells":[' + ($big -join ',') + ']}')
$widgets = '[' + ($defs -join ',') + ']'
$null = $widgets | ConvertFrom-Json   # a typo here would only show as an app with no widgets
$shown = '{"w_dock_a":true,"w_dock_b":true,"w_dock_s":true,"w_dock_big":true}'
$layouts = '[{"layout":{"type":"leaf","mode":"2D","paneId":1},"popupLinks":[],"echogramStates":{}},' +
           '{"layout":{"type":"split","orientation":"vertical","ratio":0.5,"first":{"type":"leaf","mode":"3D","paneId":1},"second":{"type":"leaf","mode":"2D","paneId":2}},"popupLinks":[],"echogramStates":{}}]'
$seed = @('[General]', 'schemaVersion=8', '', '[main]', 'ui\welcomeShown=true', 'workspace\activeLayoutIndexStored=1',
          ('workspace\layoutsJson=' + (Ini-String $layouts)), ('workspace\widgetShownJson=' + (Ini-String $shown)),
          ('workspace\widgetsJson=' + (Ini-String $widgets)), '')
if (Test-Path -LiteralPath $ini) { Remove-Item -LiteralPath $ini -Force }
[System.IO.File]::WriteAllText($ini, ($seed -join "`r`n"), (New-Object System.Text.UTF8Encoding $false))

# One run: the steps, then the log's "docks" snapshots and the steps' results, in order.
function Invoke-Run([string] $name, [string[]] $steps) {
    $scriptPath = Join-Path $Out "$name.json"
    $all = @('{"op": "wait", "ms": 4000}') + $steps + @('{"op": "quit"}')
    ('{"live": true, "steps": [' + ($all -join ",`n") + ']}') | Set-Content -LiteralPath $scriptPath -Encoding ASCII
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
    if ($PathPrefix) { $psi.EnvironmentVariables['PATH'] = $PathPrefix + ';' + $env:PATH }
    $psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
    $psi.EnvironmentVariables['KOGGER_SETTINGS_INI'] = '1'
    $psi.EnvironmentVariables['KOGGER_UI_PROBE'] = '1'
    $psi.EnvironmentVariables['KOGGER_ECHOEDIT_SCRIPT'] = $scriptPath
    $app = Start-TestDesktopProcess $psi
    if (-not $app.WaitForExit($TimeoutS * 1000)) { $app.Kill(); throw "run '$name': the app did not end in $TimeoutS s" }
    $lines = Get-Content -LiteralPath $log -Encoding UTF8
    $start = ($lines | Select-String -Pattern 'log opened' | Select-Object -Last 1).LineNumber
    $mine = @($lines[($start - 1)..($lines.Count - 1)])
    $mine | Set-Content -LiteralPath (Join-Path $Out "$name.log") -Encoding UTF8
    $snaps = @(); $results = @()
    foreach ($l in $mine) {
        if ($l -match 'echoedit script: docks (\{.*\})$') { $snaps += , ($Matches[1] | ConvertFrom-Json) }
        elseif ($l -match 'echoedit script: (dock .*|dockScale .*|undock .*)$') { $results += $Matches[1] }
        elseif ($l -match 'qrc:/qml/app/(DockLayout|WidgetDocks|BasePanePopup|DataWidgetPopup|SettingsWidgetPopup|WorkspaceStore|MainWindow)\.(js|qml):\d+.*(Error|error|undefined|Unable)') {
            $results += "QML: $l"
        }
    }
    return [pscustomobject]@{ Snaps = $snaps; Results = $results }
}

$failed = 0
function Expect([bool] $ok, [string] $what, [string] $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' })), $what, $detail
}
function Near($a, $b) { [math]::Abs([double]$a - [double]$b) -le 1.0 }
function Ids($snap, [string] $side) { @($snap.docks.$side.widgets) -join ',' }
function DocksText($snap) { (('left', 'right', 'top', 'bottom') | ForEach-Object { "${_}[" + (Ids $snap $_) + "]x$($snap.docks.$_.scale)$(if ($snap.docks.$_.collapsed) { ' collapsed' })" }) -join ' ' }
function Panel($snap, [string] $id) { @($snap.panels) | Where-Object { $_.id -eq $id } | Select-Object -First 1 }

# The rules every snapshot keeps.
function Check-Snap([string] $tag, $s) {
    $W = [double]$s.window.w; $H = [double]$s.window.h; $in = $s.insets
    $bad = @()
    foreach ($p in @($s.panels)) {
        $slot = $s.slots.($p.id)
        if ($p.docked) {
            if (-not $slot) { $bad += "$($p.id) docked without a slot"; continue }
            if ($slot.collapsed) { if ($p.visible) { $bad += "$($p.id) drawn in a collapsed dock" }; continue }
            if ($p.visible -and -not ((Near $p.panel.x $slot.x) -and (Near $p.panel.y $slot.y) -and (Near $p.panel.w $slot.w) -and (Near $p.panel.h $slot.h))) {
                $bad += "$($p.id) panel $($p.panel.x),$($p.panel.y) $($p.panel.w)x$($p.panel.h) not in its slot $($slot.x),$($slot.y) $($slot.w)x$($slot.h)"
            }
        } elseif ($slot) {
            $bad += "$($p.id) has a slot but is not docked"
        } elseif ($p.visible -and $p.panel) {
            $r = $p.panel
            if ($r.x -lt $in.left - 1 -or $r.y -lt $in.top - 1 -or $r.x + $r.w -gt $W - $in.right + 1 -or $r.y + $r.h -gt $H - $in.bottom + 1) {
                $bad += "$($p.id) floats over a dock: $($r.x),$($r.y) $($r.w)x$($r.h)"
            }
        }
    }
    $ws = $s.workspace
    if (-not ((Near $ws.x $in.left) -and (Near $ws.y $in.top) -and (Near ($ws.x + $ws.w) ($W - $in.right)) -and ($ws.y + $ws.h -le $H - $in.bottom + 1))) {
        $bad += "the workspace $($ws.x),$($ws.y) $($ws.w)x$($ws.h) is not the window $W x $H less the docks $($in.left)/$($in.top)/$($in.right)/$($in.bottom)"
    }
    if (-not $s.squeezed -and ($in.left + $in.right -gt $W / 2 + 1 -or $in.top + $in.bottom -gt $H / 2 + 1)) {
        $bad += "the docks take over half the window"
    }
    Expect ($bad.Count -eq 0) "$tag on screen" $(if ($bad.Count) { $bad -join '; ' } else { (DocksText $s) + " insets $($in.left)/$($in.top)/$($in.right)/$($in.bottom), window $W x $H" })
}

function Steps($snap, [string] $side) { @((@($snap.steps) | Where-Object { $_.side -eq $side }).steps | ForEach-Object { [double]$_.scale }) }

# What the store should answer, worked out here as DockLayout does it: a dock as thick as its thickest shown widget
# at its scale (one that does not scale at its own size) plus its edge line; the pairs at most half the window.
function Copy-Docks($docks) {
    $o = [ordered]@{}
    foreach ($side in 'left', 'right', 'top', 'bottom') {
        $d = $docks.$side
        $o[$side] = [pscustomobject]@{ widgets = @(@($d.widgets) | ForEach-Object { [string]$_ }); scale = [double]$d.scale; collapsed = [bool]$d.collapsed }
    }
    [pscustomobject]$o
}
function Thick($snap, $docks, [string] $side) {
    $d = $docks.$side
    if ($d.collapsed) { return 0 }
    $t = 0.0; $any = $false
    foreach ($id in @($d.widgets)) {
        $n = $snap.natural.$id
        if (-not $n) { continue }
        $k = $(if ($n.scalable) { [double]$d.scale } else { 1.0 })
        $t = [math]::Max($t, $(if ($side -in 'left', 'right') { [double]$n.w } else { [double]$n.h }) * $k)
        $any = $true
    }
    if (-not $any) { return 0 }
    [math]::Floor($t + 0.5) + [double]$snap.lines.edge
}
function Fits($snap, $docks) {
    ((Thick $snap $docks 'left') + (Thick $snap $docks 'right') -le $snap.window.w / 2) -and
    ((Thick $snap $docks 'top') + (Thick $snap $docks 'bottom') -le $snap.window.h / 2)
}
function With-Widget($snap, [string] $id, [string] $side) {
    $d = Copy-Docks $snap.docks
    foreach ($s in 'left', 'right', 'top', 'bottom') { $d.$s.widgets = @($d.$s.widgets | Where-Object { $_ -ne $id }) }
    $d.$side.widgets = @($d.$side.widgets) + $id
    $d.$side.collapsed = $false
    $d
}
function Said([string] $pattern) { @($r1.Results | Where-Object { $_ -match $pattern } | ForEach-Object { $Matches[1] }) -join '' }

# --- run 1 ------------------------------------------------------------------------------------------------------------
$g = { param($n) '{"op": "grab", "path": "' + (Join-Path $Out "$n.png").Replace('\', '\\') + '"}' }
$r1 = Invoke-Run 'run1' @(
    '{"op": "docks"}',
    '{"op": "dock", "widget": "w_dock_a", "side": "right"}',
    '{"op": "dock", "widget": "w_dock_b", "side": "right", "at": 0}',
    '{"op": "dock", "widget": "w_dock_s", "side": "bottom"}',
    '{"op": "wait", "ms": 800}', '{"op": "docks"}', (& $g 'docked'),
    '{"op": "dockScale", "side": "right", "scale": 1.5}',
    '{"op": "wait", "ms": 500}', '{"op": "docks"}',
    '{"op": "dockCollapse", "side": "right"}',
    '{"op": "wait", "ms": 500}', '{"op": "docks"}', (& $g 'collapsed'),
    '{"op": "layout", "index": 0}',
    '{"op": "wait", "ms": 800}', '{"op": "docks"}',
    '{"op": "dock", "widget": "w_dock_b", "side": "top"}',
    '{"op": "wait", "ms": 500}', '{"op": "docks"}', (& $g 'layout0'),
    '{"op": "dockScale", "side": "top", "scale": 2.5}',
    '{"op": "wait", "ms": 500}', '{"op": "docks"}',
    '{"op": "dock", "widget": "w_dock_big", "side": "bottom"}',
    '{"op": "wait", "ms": 500}', '{"op": "docks"}',
    '{"op": "layout", "index": 1}',
    '{"op": "wait", "ms": 800}', '{"op": "docks"}')
$S = $r1.Snaps
Expect ($S.Count -eq 9) 'run 1 snapshots' "$($S.Count) of 9; results: $($r1.Results -join ' | ')"
if ($S.Count -eq 9) {
    for ($i = 0; $i -lt 9; $i++) { Check-Snap "run 1 #$i" $S[$i] }
    $none = { param($s) ((('left', 'right', 'top', 'bottom') | ForEach-Object { @($s.docks.$_.widgets).Count }) | Measure-Object -Sum).Sum -eq 0 }
    Expect ((& $none $S[0]) -and $S[0].insets.right -eq 0) 'fresh profile' (DocksText $S[0])
    Expect ((Ids $S[1] 'right') -eq 'w_dock_b,w_dock_a' -and (Ids $S[1] 'bottom') -eq 'w_dock_s' -and $S[1].insets.right -gt 0 -and $S[1].insets.bottom -gt 0) `
        'docked right and bottom' (DocksText $S[1])
    Expect (@($r1.Results | Where-Object { $_ -match '^dock w_dock_(a|b|s) to (right|bottom) .*: docked$' }).Count -eq 3) 'three docked' ($r1.Results -join ' | ')
    $exp = Thick $S[1] (Copy-Docks $S[1].docks) 'right'
    Expect ($S[1].insets.right -eq $exp) 'as thick as the thicker widget' "right $($S[1].insets.right) px, worked out $exp (natural b $($S[1].natural.w_dock_b.w), a $($S[1].natural.w_dock_a.w))"
    $allowed = (Steps $S[1] 'right') -contains 1.5
    $said = Said '^dockScale right 1\.5: (set|refused)$'
    Expect ($said -eq $(if ($allowed) { 'set' } else { 'refused' }) -and ([double]$S[2].docks.right.scale -eq $(if ($allowed) { 1.5 } else { 1.0 }))) `
        'right dock to 1.5' "steps $((Steps $S[1] 'right') -join ' '); said $said; now x$($S[2].docks.right.scale), $($S[2].insets.right) px"
    Expect ($S[3].docks.right.collapsed -eq $true -and $S[3].insets.right -eq 0 -and -not (Panel $S[3] 'w_dock_a').visible -and -not (Panel $S[3] 'w_dock_b').visible) `
        'collapsed by the tab' "insets.right $($S[3].insets.right), a visible $((Panel $S[3] 'w_dock_a').visible)"
    Expect ($S[4].activeLayout -eq 0 -and (& $none $S[4]) -and -not (Panel $S[4] 'w_dock_a').docked -and (Panel $S[4] 'w_dock_a').visible) `
        'layout 0 has docks of its own: none' (DocksText $S[4])
    $fitB = Fits $S[4] (With-Widget $S[4] 'w_dock_b' 'top')
    Expect ((Said '^dock w_dock_b to top at -1: (docked|refused)$') -eq $(if ($fitB) { 'docked' } else { 'refused' }) -and ((Ids $S[5] 'top') -eq $(if ($fitB) { 'w_dock_b' } else { '' }))) `
        'a widget docked in layout 1 to the top of layout 0' "$(DocksText $S[5]); fits $fitB; window $($S[5].window.w) x $($S[5].window.h)"
    $top25 = (Steps $S[5] 'top') -contains 2.5
    $said = Said '^dockScale top 2\.5: (set|refused)$'
    Expect ($said -eq $(if ($top25) { 'set' } else { 'refused' }) -and (($top25) -or ([double]$S[6].docks.top.scale -eq [double]$S[5].docks.top.scale -and $S[6].insets.top -eq $S[5].insets.top))) `
        'the top dock to 2.5' "steps $((Steps $S[5] 'top') -join ' '); said $said; now x$($S[6].docks.top.scale), $($S[6].insets.top) px"
    $fitBig = Fits $S[6] (With-Widget $S[6] 'w_dock_big' 'bottom')
    $said = Said '^dock w_dock_big to bottom at -1: (docked|refused)$'
    Expect ($said -eq $(if ($fitBig) { 'docked' } else { 'refused' }) -and ($fitBig -or (DocksText $S[7]) -eq (DocksText $S[6]))) `
        'the 4x4 widget to the bottom' "fits $fitBig (top $($S[6].insets.top) + $(Thick $S[6] (With-Widget $S[6] 'w_dock_big' 'bottom') 'bottom') of $($S[6].window.h / 2)); said $said; $(DocksText $S[7])"
    Expect ((-not $top25) -or (-not $fitBig)) 'a refusal tried' 'the window must be small enough that 2.5 on top or the 4x4 at the bottom does not fit'
    Expect ($S[8].activeLayout -eq 1 -and (DocksText $S[8]) -eq (DocksText $S[3])) 'layout 1 kept its docks' "$(DocksText $S[8]) vs $(DocksText $S[3])"
}
$qml = @($r1.Results | Where-Object { $_ -like 'QML:*' })
Expect ($qml.Count -eq 0) 'run 1 QML errors' ($qml -join ' | ')

# --- run 2: after a restart ---------------------------------------------------------------------------------------------
$r2 = Invoke-Run 'run2' @(
    '{"op": "docks"}',
    '{"op": "layout", "index": 0}', '{"op": "wait", "ms": 800}', '{"op": "docks"}',
    '{"op": "layout", "index": 1}', '{"op": "wait", "ms": 800}',
    '{"op": "dockCollapse", "side": "right", "collapsed": false}',
    '{"op": "undock", "widget": "w_dock_a"}',
    '{"op": "wait", "ms": 800}', '{"op": "docks"}', (& $g 'reopened'))
$T = $r2.Snaps
Expect ($T.Count -eq 3) 'run 2 snapshots' "$($T.Count) of 3; results: $($r2.Results -join ' | ')"
if ($T.Count -eq 3 -and $S.Count -eq 9) {
    for ($i = 0; $i -lt 3; $i++) { Check-Snap "run 2 #$i" $T[$i] }
    Expect ($T[0].activeLayout -eq 1 -and (DocksText $T[0]) -eq (DocksText $S[8])) 'layout 1 after a restart' "$(DocksText $T[0]) vs $(DocksText $S[8])"
    Expect ((DocksText $T[1]) -eq (DocksText $S[7])) 'layout 0 after a restart' "$(DocksText $T[1]) vs $(DocksText $S[7])"
    Expect ((Ids $T[2] 'right') -eq 'w_dock_b' -and -not $T[2].docks.right.collapsed -and $T[2].insets.right -gt 0 -and (Panel $T[2] 'w_dock_b').visible -and -not (Panel $T[2] 'w_dock_a').docked -and (Panel $T[2] 'w_dock_a').visible) `
        'opened again, one undocked' (DocksText $T[2])
}
$qml = @($r2.Results | Where-Object { $_ -like 'QML:*' })
Expect ($qml.Count -eq 0) 'run 2 QML errors' ($qml -join ' | ')

# --- run 3: that kept ---------------------------------------------------------------------------------------------------
$r3 = Invoke-Run 'run3' @('{"op": "docks"}')
if ($r3.Snaps.Count -eq 1 -and $T.Count -eq 3) {
    Check-Snap 'run 3' $r3.Snaps[0]
    Expect ((DocksText $r3.Snaps[0]) -eq (DocksText $T[2])) 'kept after a restart' "$(DocksText $r3.Snaps[0]) vs $(DocksText $T[2])"
} else {
    Expect $false 'run 3 snapshot' "$($r3.Snaps.Count) of 1"
}

"dock e2e: {0} (pictures and logs in {1})" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" })), $Out
exit ([int]($failed -gt 0))
