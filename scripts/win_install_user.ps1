#requires -Version 5.1
<#
.SYNOPSIS
    Installs a packaged build of the fork for the current user and checks it.

.DESCRIPTION
    The per-user install, %LOCALAPPDATA%\Programs\KoggerApp ME (Inno Setup /CURRENTUSER, no UAC; until
    2026-10-18 "KoggerApp myEdition", which the setup removes).
    Checks that dist\stage is build <Tag> and not dirty, waits for KoggerApp of this install to exit
    (up to -WaitMinutes), runs the newest dist\KoggerApp-ME-*-setup*.exe silently, and
    compares the installed exe with dist\stage. With -Relaunch starts it again. The log's last line is
    UPDATE_DONE, or FAILED: <why> (exit 1).

    Before it: what is installed now is build_info.txt in the install folder. The packaging is
    scripts\win_package_release.ps1 in a clean checkout outside the synced folder (docs/HANDOFF.md).
    Was D:\Temp\kogger_install\update_kogger_user.ps1 on the first PC.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\win_install_user.ps1 -Tag 273964bc -Dist D:\Temp\kogger_merge_test\dist
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Tag,
    [string] $Dist = 'D:\Temp\kogger_merge_test\dist',
    [int]    $WaitMinutes = 180,
    [string] $Log = (Join-Path $env:TEMP 'kogger_update_user.log'),
    [switch] $Relaunch
)

$ErrorActionPreference = 'Stop'
function L([string] $m) { Add-Content -LiteralPath $Log -Value ("{0}  {1}" -f (Get-Date -Format 'HH:mm:ss'), $m); $m }
Set-Content -LiteralPath $Log -Value ("start " + (Get-Date) + ", tag $Tag")
$stage = "$Dist\stage"
$target = Join-Path $env:LOCALAPPDATA 'Programs\KoggerApp ME'
$oldTarget = Join-Path $env:LOCALAPPDATA 'Programs\KoggerApp myEdition'   # removed by the setup
try {
    if (-not (Select-String -LiteralPath "$stage\build_info.txt" -Pattern "^commit:\s+$Tag" -Quiet)) { throw "the stage is not build $Tag" }
    if (-not (Select-String -LiteralPath "$stage\build_info.txt" -Pattern '^dirty:\s+no' -Quiet)) { throw "the stage is a dirty build" }
    $setup = Get-ChildItem -LiteralPath $Dist -Filter "KoggerApp-ME-*-setup*.exe" |
             Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $setup) { throw "no installer for $Tag in $Dist" }
    $deadline = (Get-Date).AddMinutes($WaitMinutes)
    $waiting = $false
    # Only an instance of this install holds its files; a test build elsewhere does not matter.
    while (Get-Process KoggerApp -ErrorAction SilentlyContinue | Where-Object { $_.Path -and ($_.Path.StartsWith($target, [System.StringComparison]::OrdinalIgnoreCase) -or $_.Path.StartsWith($oldTarget, [System.StringComparison]::OrdinalIgnoreCase)) }) {
        if (-not $waiting) { L "waiting for KoggerApp to exit"; $waiting = $true }
        if ((Get-Date) -gt $deadline) { throw "KoggerApp still running after $WaitMinutes min" }
        Start-Sleep -Seconds 2
    }
    if ($waiting) { L "KoggerApp exited" }
    Start-Sleep -Seconds 2   # let it release its files
    $p = Start-Process -FilePath $setup.FullName -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/CURRENTUSER' -Wait -PassThru
    L "setup $($setup.Name) exit $($p.ExitCode)"
    $h1 = (Get-FileHash -LiteralPath "$stage\KoggerApp.exe").Hash
    $h2 = (Get-FileHash -LiteralPath "$target\KoggerApp.exe").Hash
    L ("exe " + $(if ($h1 -eq $h2) { 'MATCH' } else { 'MISMATCH' }) + " " + $h2.Substring(0, 16))
    if ($h1 -ne $h2) { throw "the installed exe differs from the stage" }
    if ($Relaunch) {
        Start-Process -FilePath "$target\KoggerApp.exe" -WorkingDirectory $target
        L "started again"
    }
    L "UPDATE_DONE"
} catch {
    L ("FAILED: " + $_.Exception.Message)
    exit 1
}
