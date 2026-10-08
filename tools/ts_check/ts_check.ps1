#requires -Version 5.1
<#
Mechanical check of the Qt Linguist files in translations/.

    powershell -File tools\ts_check\ts_check.ps1                      every translations\translation_*.ts
    powershell -File tools\ts_check\ts_check.ps1 ru zh                only these languages
    powershell -File tools\ts_check\ts_check.ps1 -Path some.ts        one file anywhere
    powershell -File tools\ts_check\ts_check.ps1 -UnfinishedOk ru

For every translated message: the same placeholders as the source (%1..%99, %L1, %n, %%), the same
HTML tags, the same number of line breaks and the same leading/trailing whitespace -- the things that
break a string at run time or on screen. An unfinished message is a failure unless -UnfinishedOk:
lrelease puts its text (often lupdate's guess from another context) into the .qm all the same.

It cannot tell a good translation from a bad one; that takes a reader of the language.
Exit code 0 = nothing found, 1 = problems listed, 2 = bad arguments or unreadable file.
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]] $Languages = @(),
    [string[]] $Path = @(),
    [switch]   $UnfinishedOk
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$ph   = '%(?:L?\d{1,2}|n|%)'
$tag  = '</?([a-zA-Z][a-zA-Z0-9]*)(?:\s[^<>]*)?/?>'

function Get-Sorted([string] $Text, [string] $Pattern, [switch] $Group1) {
    $found = [regex]::Matches($Text, $Pattern) | ForEach-Object {
        if ($Group1) { $_.Groups[1].Value.ToLowerInvariant() } else { $_.Value }
    }
    return (@($found) | Sort-Object -CaseSensitive) -join ' '
}

function Get-Edges([string] $Text) {
    return ([regex]::Match($Text, '^\s*').Value) + '|' + ([regex]::Match($Text, '\s*$').Value)
}

function Get-Problems([string] $Src, [string] $Tr) {
    $out = @()
    if ((Get-Sorted $Src $ph) -cne (Get-Sorted $Tr $ph)) {
        $out += "placeholders [$(Get-Sorted $Src $ph)] -> [$(Get-Sorted $Tr $ph)]"
    }
    if ((Get-Sorted $Src $tag -Group1) -cne (Get-Sorted $Tr $tag -Group1)) { $out += 'HTML tags differ' }
    $ns = [regex]::Matches($Src, "`n").Count
    $nt = [regex]::Matches($Tr, "`n").Count
    if ($ns -ne $nt) { $out += "line breaks $ns -> $nt" }
    if ((Get-Edges $Src) -cne (Get-Edges $Tr)) { $out += 'edge whitespace differs' }
    return $out
}

function Test-TsFile([string] $File) {
    $settings = New-Object System.Xml.XmlReaderSettings
    $settings.DtdProcessing = [System.Xml.DtdProcessing]::Ignore      # <!DOCTYPE TS> has nothing to load
    $doc = New-Object System.Xml.XmlDocument
    $doc.PreserveWhitespace = $true                                      # edge whitespace is checked
    try {
        $reader = [System.Xml.XmlReader]::Create($File, $settings)
        try { $doc.Load($reader) } finally { $reader.Close() }
    } catch {
        Write-Output "${File}: cannot read: $($_.Exception.Message)"
        exit 2
    }
    $lang = $doc.DocumentElement.GetAttribute('language')
    $total = 0; $unfinished = 0; $bad = 0
    foreach ($ctx in $doc.SelectNodes('/TS/context')) {
        $name = $ctx.SelectSingleNode('name').InnerText
        foreach ($m in $ctx.SelectNodes('message')) {
            $tr   = $m.SelectSingleNode('translation')
            $kind = if ($tr) { $tr.GetAttribute('type') } else { 'missing' }
            if ($kind -eq 'vanished' -or $kind -eq 'obsolete') { continue }   # not compiled into the .qm
            $total++
            $src  = $m.SelectSingleNode('source').InnerText
            $show = if ($src.Length -gt 60) { $src.Substring(0, 60) } else { $src }
            $show = $show.Replace("`n", '\n')
            if ($m.GetAttribute('numerus') -eq 'yes') {
                $texts = @($tr.SelectNodes('numerusform') | ForEach-Object { $_.InnerText })
            } elseif ($tr) {
                $texts = @($tr.InnerText)
            } else {
                $texts = @()
            }
            if ($kind -eq 'unfinished' -or $kind -eq 'missing') {
                $unfinished++
                if (-not $UnfinishedOk) { $bad++; Write-Output "  $name | '$show': unfinished" }
            }
            # An empty translation is left out of the .qm and the source shows; a guess that ships is checked
            foreach ($text in ($texts | Where-Object { $_ -ne '' })) {
                foreach ($p in (Get-Problems $src $text)) { $bad++; Write-Output "  $name | '$show': $p" }
            }
        }
    }
    $shown = $File
    if ($File.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { $shown = $File.Substring($root.Length + 1) }
    Write-Output "$shown ($lang): $total messages, $unfinished unfinished, $bad problems"
    return $bad
}

$files = @()
if ($Path.Count) {
    foreach ($p in $Path) { if (-not (Test-Path -LiteralPath $p)) { Write-Output "${p}: no such file"; exit 2 } }
    $files = $Path | ForEach-Object { (Resolve-Path -LiteralPath $_).Path }
} elseif ($Languages.Count) {
    $files = $Languages | ForEach-Object { Join-Path $root "translations\translation_$_.ts" }
} else {
    $files = @(Get-ChildItem (Join-Path $root 'translations') -Filter 'translation_*.ts' | Sort-Object Name |
               ForEach-Object { $_.FullName })
}
if (-not $files.Count) { Write-Output 'no translation files'; exit 2 }

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$problems = 0
foreach ($f in $files) {
    if (-not (Test-Path -LiteralPath $f)) { Write-Output "${f}: no such file"; exit 2 }
    $result = @(Test-TsFile $f)
    $problems += [int]$result[-1]
    $result[0..($result.Count - 2)] | Where-Object { $_ -ne $null } | ForEach-Object { Write-Output $_ }
}
if ($problems) { exit 1 }
exit 0
