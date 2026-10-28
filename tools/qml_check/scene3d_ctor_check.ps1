#requires -Version 5.1
<#
.SYNOPSIS
    The 3D view's camera must not call back into the view while the view constructs it: the view's m_camera is not
    built yet. Run under cdb with that slot poisoned, a build that calls back crashes at once.

.DESCRIPTION
    2026-10-28: one start crashed (0xc0000005) in GraphicsScene3dView::refreshSceneContentVisible, called from
    Camera::Camera via reset() / updateCameraParams() inside the view's initializer list (m_camera(make_shared<Camera>
    (this))). It read the view's m_camera before it was built; whatever the heap held there decided whether it crashed.
    This makes it deterministic: cdb breaks at GraphicsScene3dView's constructor, writes 0x17 into the m_camera slot
    (its offset read from the build's refreshSceneContentVisible), and watches access violations. The app runs on the
    test desktop with a live edit script that waits and quits.
    Checked:
      poisoned  the constructor was reached and the slot written
      callback  no access violation in refreshSceneContentVisible
      ran       the app logged its normal end
    Exit code 0 when every check holds, 1 when one fails, 2 when cdb or the LLVM tools are missing.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\qml_check\scene3d_ctor_check.ps1 -Exe <build>\KoggerApp.exe -PathPrefix "D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [string] $PathPrefix,
    [string] $Profile = 'perf',
    [string] $LlvmBin = 'D:\Qt\Tools\llvm-mingw1706_64\bin',
    [string] $Cdb = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe',
    [string] $Out = "$env:TEMP\kogger_scene3d_ctor_check",
    [string] $PerfReg = 'D:\YandexDisk\DelphiProjects\KoggerApp_myEdition_transfer\KoggerApp-perf.reg'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\test_desktop.ps1')   # the app on a desktop of its own: no window, no focus taken
foreach ($tool in $Cdb, (Join-Path $LlvmBin 'llvm-nm.exe'), (Join-Path $LlvmBin 'llvm-objdump.exe')) {
    if (-not (Test-Path -LiteralPath $tool)) { "not run: $tool not found"; exit 2 }
}
New-Item -ItemType Directory -Force -Path $Out | Out-Null
if (Test-Path -LiteralPath $PerfReg) {
    # reg.exe reports success on stderr; under 'Stop' PowerShell 5.1 would take that for a failure.
    cmd.exe /c "reg import `"$PerfReg`" >nul 2>&1"
}

# Where the constructor and refreshSceneContentVisible are, from this build's symbols (a PE of llvm-mingw has its
# image base at 0x140000000), and the m_camera slot's offset: the function's first load from rcx.
$ctorSym = '_ZN19GraphicsScene3dViewC1Ev'
$refreshSym = '_ZN19GraphicsScene3dView26refreshSceneContentVisibleEv'
$base = [UInt64]0x140000000
$nm = & (Join-Path $LlvmBin 'llvm-nm.exe') $Exe 2>$null
function OffsetOf([string] $name) {
    $line = $nm | Where-Object { $_ -match ('\s' + [regex]::Escape($name) + '$') } | Select-Object -First 1
    if (-not $line) { throw "symbol $name not in $Exe" }
    return [Convert]::ToUInt64(($line -split '\s+')[0], 16) - $base
}
$ctor = OffsetOf $ctorSym
$refresh = OffsetOf $refreshSym
$dis = & (Join-Path $LlvmBin 'llvm-objdump.exe') -d -M intel --no-show-raw-insn "--disassemble-symbols=$refreshSym" $Exe 2>$null
# COFF symbols carry no size: the function ends after its last instruction in the disassembly.
$lastInsn = $dis | Where-Object { $_ -match '^\s*([0-9a-f]+):' } | ForEach-Object { [Convert]::ToUInt64($matches[1], 16) } |
    Measure-Object -Maximum
$refreshEnd = [UInt64]$lastInsn.Maximum - $base + 16
$load = $dis | Where-Object { $_ -match 'mov\s+rax, qword ptr \[rcx \+ 0x[0-9a-f]+\]' } | Select-Object -First 1
if (-not $load) { throw 'no load of m_camera from rcx in refreshSceneContentVisible' }
[void]($load -match '\[rcx \+ (0x[0-9a-f]+)\]')
$slot = $matches[1]

# cdb: each first-chance access violation is logged as KCHECK_AV and its rip less the module base, then passed on;
# a second chance or the process's end quits.
$cmdFile = Join-Path $Out 'cdb_commands.txt'
@(
    'sxe -c ".echo KCHECK_AV; ? @rip-KoggerApp; gn" -c2 ".echo KCHECK_AV2; q" av'
    'sxe -c "q" epr'
    ('bp KoggerApp+0x{0:x} ".echo KCHECK_POISON; eq @rcx+{1} 0x17; gc"' -f $ctor, $slot)
    'g'
) | Set-Content -LiteralPath $cmdFile -Encoding ASCII

$scriptPath = Join-Path $Out 'script.json'
'{"live": true, "steps": [{"op": "wait", "ms": 3000}, {"op": "quit"}]}' | Set-Content -LiteralPath $scriptPath -Encoding ASCII
$log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"
$before = if (Test-Path -LiteralPath $log) { (Get-Item -LiteralPath $log).Length } else { 0 }
$cdbOut = Join-Path $Out 'cdb.txt'
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Cdb
$psi.Arguments = ('-G -cf "{0}" "{1}"' -f $cmdFile, $Exe)
$psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
if ($PathPrefix) { $psi.EnvironmentVariables['PATH'] = $PathPrefix + ';' + $env:PATH }
$psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
$psi.EnvironmentVariables['KOGGER_ECHOEDIT_SCRIPT'] = $scriptPath
$proc = Start-TestDesktopProcess $psi -StdOut $cdbOut
if (-not $proc.WaitForExit(90000)) {
    $proc.Kill()
    Get-Process KoggerApp -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $Exe } | Stop-Process -Force
    'the run did not end in 90 s, killed'
}
Start-Sleep -Milliseconds 300
$cdbText = if (Test-Path -LiteralPath $cdbOut) { Get-Content -LiteralPath $cdbOut -Raw } else { '' }
$fs = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite'); [void]$fs.Seek($before, 'Begin')
$appText = (New-Object System.IO.StreamReader($fs, [Text.Encoding]::UTF8)).ReadToEnd(); $fs.Close()
$appText | Set-Content -LiteralPath (Join-Path $Out 'kogger.log') -Encoding UTF8

$failed = 0
function Expect([bool] $ok, [string] $what, [string] $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' })), $what, $detail
}
$avs = @([regex]::Matches($cdbText, 'KCHECK_AV\s+Evaluate expression: -?\d+ = ([0-9a-f`]+)') | ForEach-Object {
    [Convert]::ToUInt64(($_.Groups[1].Value -replace '`', ''), 16) })
$inRefresh = @($avs | Where-Object { $_ -ge $refresh -and $_ -lt $refreshEnd })
$ended = $appText -match 'session: --- log closed ---'
Expect ($cdbText -match 'KCHECK_POISON') 'poisoned' ("constructor at KoggerApp+0x{0:x}, m_camera at this+{1}" -f $ctor, $slot)
Expect ($inRefresh.Count -eq 0) 'callback' ("{0} access violations in refreshSceneContentVisible (KoggerApp+0x{1:x}..0x{2:x}), {3} anywhere" -f $inRefresh.Count, $refresh, $refreshEnd, $avs.Count)
Expect $ended 'ran' ($(if ($ended) { 'the app ended normally' } else { 'no normal end in the app log' }))
"scene3d ctor check: {0}" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" }))
"logs: {0}" -f $Out
exit $(if ($failed -eq 0) { 0 } else { 1 })
