#requires -Version 5.1
<#
.SYNOPSIS
    The circular mosaic under a zeroed bottom track with the far edge cropped: painted or not.

.DESCRIPTION
    The owner's bench of 2026-11-04: Pos zeroing and Bottom track zeroing on, "Crop the far edge" at 82 deg,
    a sonar turned on the spot -- and no mosaic. The zeroed depth (1 cm) put every sample at the horizon and
    the crop took it. Fixed by mosaic_beam::sampleGain (no angle under kNoAngleAltitudeM).

    A profile of its own, KoggerApp-<Profile>, is made from KoggerApp-perf.reg each run, with those settings,
    the mosaic shown, brightness evening on, the mosaic palette fixed and the camera over the sonar. ring_bench.ps1 plays the turning
    sonar to the app over UDP and grabs its window; the pixels of the mosaic's palette in the 3D view (the
    left half of the grab) are then counted. PASS at -MinPixels or more. The build before the fix (3dc063ad)
    paints none: run it with that -Exe as the control. The user's settings are not touched.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\mosaic_bench\zeroed_crop_check.ps1 -Exe <build>\KoggerApp.exe
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [string] $Out = "$env:TEMP\kogger_zeroed_crop",
    [string] $Profile = 'zerocrop',
    [string] $PerfReg = 'D:\YandexDisk\DelphiProjects\KoggerApp_myEdition_transfer\KoggerApp-perf.reg',
    [int]    $FarCrop = 82,
    [int]    $MinPixels = 50000,
    [double] $Seconds = 12,
    [string] $PathPrefix
)

$ErrorActionPreference = 'Stop'

$key = "HKCU\Software\KOGGER\KoggerApp-$Profile"
cmd.exe /c "reg delete `"$key`" /f >nul 2>&1"
$reg = Join-Path $env:TEMP "KoggerApp-$Profile.reg"
(Get-Content -LiteralPath $PerfReg -Encoding Unicode) -replace 'KoggerApp-perf(?=[\\\]])', "KoggerApp-$Profile" |
    Set-Content -LiteralPath $reg -Encoding Unicode
cmd.exe /c "reg import `"$reg`" >nul 2>&1"
if ($LASTEXITCODE -ne 0) { throw "reg import $reg failed" }
$set = @(
    @('main\dataset',   'zeroingPosButtonCheched',         'REG_SZ',    'true'),
    @('main\dataset',   'zeroingBottomTrackButtonChecked', 'REG_SZ',    'true'),
    @('scene3d\view',   'mosaicViewCheckButton',           'REG_SZ',    'true'),
    @('scene3d\mosaic', 'mosaicBeamCorrection',            'REG_SZ',    'true'),
    @('scene3d\mosaic', 'mosaicFarCrop',                   'REG_DWORD', [string]$FarCrop),
    @('scene3d\mosaic', 'mosaicTheme',                     'REG_DWORD', '1'),
    # the camera over the sonar, the 15 m ring filling the view (the perf profile looks from 750 m)
    @('scene3d\cameraView', 'distance', 'REG_SZ', '41'),
    @('scene3d\cameraView', 'lookAtN',  'REG_SZ', '0'),
    @('scene3d\cameraView', 'lookAtE',  'REG_SZ', '0')
)
foreach ($s in $set) {
    cmd.exe /c "reg add `"$key\$($s[0])`" /v $($s[1]) /t $($s[2]) /d $($s[3]) /f >nul 2>&1"
    if ($LASTEXITCODE -ne 0) { throw "reg add $($s[0]) $($s[1]) failed" }
}

$bench = Join-Path $PSScriptRoot 'ring_bench.ps1'
$benchArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $bench, '-Exe', $Exe, '-Profile', $Profile,
               '-Out', $Out, '-Seconds', [string]$Seconds)
if ($PathPrefix) { $benchArgs += @('-PathPrefix', $PathPrefix) }
& powershell @benchArgs | ForEach-Object { "  $_" }

$png = Join-Path $Out 'ui-auto.png'
if (-not (Test-Path -LiteralPath $png)) { Write-Output "FAIL  no window grab at $png"; exit 1 }

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class MosaicPixels {
    // Pixels of the yellow palette (theme 1) in the left half: the ring bench's echo from dark olive to
    // pale yellow. Not the background (navy), the trace line (cyan), the sonar arrow (red) or the grey
    // buttons.
    public static int Count(string path) {
        using (var bmp = new Bitmap(path)) {
            var rect = new Rectangle(0, 0, bmp.Width / 2, bmp.Height);
            var data = bmp.LockBits(rect, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            var buf = new byte[data.Stride * data.Height];
            Marshal.Copy(data.Scan0, buf, 0, buf.Length);
            bmp.UnlockBits(data);
            int n = 0;
            for (int y = 0; y < rect.Height; y++) {
                for (int x = 0; x < rect.Width; x++) {
                    int i = y * data.Stride + 4 * x;
                    int b = buf[i], g = buf[i + 1], r = buf[i + 2];
                    if (r > 60 && g > 40 && b < r - 25 && Math.Abs(r - g) < 70) n++;
                }
            }
            return n;
        }
    }
}
'@
$n = [MosaicPixels]::Count($png)
cmd.exe /c "reg delete `"$key`" /f >nul 2>&1"
if ($n -ge $MinPixels) {
    Write-Output "PASS  the circular mosaic under a zeroed bottom, far crop $FarCrop deg: $n px painted (at least $MinPixels)"
    exit 0
}
Write-Output "FAIL  the circular mosaic under a zeroed bottom, far crop $FarCrop deg: $n px painted (at least $MinPixels)"
exit 1
