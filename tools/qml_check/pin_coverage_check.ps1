<#
.SYNOPSIS
  Static check that every value row of the settings page can be pinned as a widget: each control a widget can
  mirror carries a KPinTag around it, or is on this script's list of rows deliberately left out.

.DESCRIPTION
  "Settings pinned as widgets" (docs/HANDOFF.md) finds a row only by an explicit KPinTag; a row without one gets
  no pin and nothing says so. The merge of 2026-10-19 brought four such rows in (MosaicQualitySettings.qml) and no
  check noticed. This one reads the QML as text -- no Qt needed -- starting at qml/app/AppSettingsPage.qml and
  following every QML type of the repository it instantiates (the groups it embeds, MosaicQualitySettings.qml,
  PerformanceSettingsGroup.qml, ...) and every file a Loader on the page loads by URL (ConnectionViewer.qml),
  except the ones listed as not part of the page (a dialog).

  Reading: strings, comments and regex literals blanked; an object is "Type {", "Qualifier.Type {" or
  "Type on property {"; braces that do not balance, or two root objects, fail the file ("parse") rather than give
  a wrong tree. On the repository's QML this reads the same tree as Qt's own parser (qmldom --dump-ast,
  compared on all 203 files, 2026-10-19).

  A control is a KSwitch, KSpinBox, KCombo, KSlider or KTabBar -- the types with a controlKind PinRegistry
  mirrors -- or an instance of a repository type that holds one not covered inside its own file (KToggleRow).
  It is covered when it sits inside the block of a KPinTag and is not an object given to one of the block's
  properties (a SettingsGroup's headerActions). The block is found the way KPinTag.qml finds it at run time: the
  first KIslandRow among the tag's first three parents, else the tag's parent; the parents are counted as the
  app has them, so KIslandRow's content slot and KIsland's row column (a default property alias to an inner
  item) count as a level each.

  Fails on:
    uncovered    a control in no tag's block and not on the left-out list
    stale        a left-out entry no uncovered control matches, or a not-followed file no Loader loads
    duplicate    one literal key on two tags -- a type or file used twice counts its tags twice
    key          a literal key not of the form app:<group>.<name>
    empty        a tag whose block holds no control: its widget would be empty
    repeated     a literal key on a tag every item of a Repeater (or a view) makes: in the delegate, in a
                 Component used as a delegate, or in a type or file a delegate instantiates
    foreign      a value control with no controlKind in an application file of the page -- a Qt Quick Controls
                 one (Switch, CheckBox, SpinBox, ComboBox, Slider, ...) or one of the repository's own (ParamCard,
                 ParamCardGroup, SmallCheck, KCheckBox, CsvSpin): a widget cannot mirror it, tagged or not
    inline       a KPinTag or value control inside an inline component ("component Name: Base { }"), which this
                 script does not follow to its instances: give the row a file of its own
    parse        a file whose braces do not balance once strings, comments and regex literals are blanked
  A key is literal when its whole value is one string, in either quote; "app:x." + index is a key per item and
  is not checked. -List prints every control with the key that covers it. -Mutants runs the check on copies
  with one fault each and expects each to fail, and on copies that must still hold (a computed key in a
  Repeater). -Runtime checks the block model itself against KPinTag.qml: nine shapes of row in qmltestrunner
  (offscreen, the real kqml_types), each tag asked for its block, compared with what this script reads from the
  same text. Exit code 0 when all hold, 1 otherwise, 2 when -Runtime finds no qmltestrunner.

  Not checked: a key computed per instance (a property each instance sets) -- its presence, form and uniqueness.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\qml_check\pin_coverage_check.ps1 -Mutants
#>
param(
    [string]$Page = 'qml/app/AppSettingsPage.qml',
    [switch]$List,
    [switch]$Mutants,
    [switch]$Runtime,
    [string]$QtBin = $(if ($env:QT_BIN) { $env:QT_BIN } else { 'D:\Qt\6.8.3\llvm-mingw_64\bin' })
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$utf8 = New-Object System.Text.UTF8Encoding $false

# Rows left out on purpose, by file and the id of the control or of an object around it (a whole panel); each
# with why. HANDOFF's "Not pinnable" list names the kinds of row that are; a value row with no id anywhere around
# it cannot be listed here -- tag it, or give it or its panel an id first.
$OptOut = [ordered]@{
    'qml/app/AppSettingsPage.qml#tapTolerSlider'      = 'the Test group: a developer knob, shown only in MANUAL_TESTING builds'
    'qml/app/AppSettingsPage.qml#splitHitSlider'      = 'the Test group: a developer knob, shown only in MANUAL_TESTING builds'
    'qml/app/AppSettingsPage.qml#sidebarAnimSlider'   = 'the Test group: a developer knob, shown only in MANUAL_TESTING builds'
    'qml/app/AppSettingsPage.qml#workspaceAnimSlider' = 'the Test group: a developer knob, shown only in MANUAL_TESTING builds'
    'qml/app/AppSettingsPage.qml#klfFragmentFrom'    = 'Export, a KLF fragment''s first epoch: one file''s range for one export, not a setting (develop, 2026-10)'
    'qml/app/AppSettingsPage.qml#klfFragmentTo'      = 'Export, a KLF fragment''s last epoch: one file''s range for one export, not a setting (develop, 2026-10)'
    'qml/app/AppSettingsPage.qml#usblNodeSwitch'     = 'USBL, a node''s switch in a delegate: one per address heard, not one setting of the app (develop, 2026-10)'
    'qml/app/FactorySettingsGroup.qml#deviceCombo'    = 'the Factory group, factory mode only: the device to flash, an operator''s pick, not a setting'
    'qml/devices/ConnectionViewer.qml#baudrateCombo'  = 'a serial link''s rate, in the link list''s delegate: one per connection, not a setting of the app'
    'qml/devices/ConnectionViewer.qml#recFormatTab'   = 'Connections, the recording''s format: not pinnable yet (2026-10-19), for the owner to decide'
    'qml/devices/ConnectionViewer.qml#csvImportPanel' = 'Connections, the CSV import panel: shown only by importCheck, a button that is never visible; hand-made SmallCheck/CsvSpin and Qt ComboBoxes, none mirrorable'
}
# Files a Loader on the page loads by URL that are not part of the page, each with why.
$NotFollowed = [ordered]@{
    'qml/settings/HotkeysDialog.qml' = 'Key bindings: a dialog opened by a button, not on the page'
}

$ControlTypes = @('KSwitch', 'KSpinBox', 'KCombo', 'KSlider', 'KTabBar')
$DelegateHosts = @('Repeater', 'ListView', 'GridView', 'PathView', 'Instantiator', 'TableView')
# Value controls with no controlKind. Qt's own: kqml_types builds its K controls from them, so only application
# files are held to it. The repository's own: ParamCard and ParamCardGroup draw their toggle by hand (the page held
# twelve ParamCards until 2026-08-28), SmallCheck is an inline toggle (AppSettingsPage, ConnectionViewer),
# CsvSpin an inline spin (ConnectionViewer), KCheckBox a bare CheckBox.
$ForeignTypes = @('Switch', 'CheckBox', 'SpinBox', 'ComboBox', 'Slider', 'RangeSlider', 'TabBar', 'RadioButton', 'Dial', 'Tumbler',
                  'ParamCard', 'ParamCardGroup', 'SmallCheck', 'KCheckBox', 'CsvSpin')
$AppDirs = @('qml/app/', 'qml/devices/', 'qml/settings/', 'qml/scene2d/', 'qml/scene3d/')

# --- reading QML ---------------------------------------------------------------------------------------------

# Regex literals, comments and strings blanked (newlines kept, so offsets and line numbers hold). A regex literal
# is a "/" where an expression starts (after ( , = : [ ! & | ? { } ; return typeof) that is not "//" or "/*".
# A string ends on its line; a quote with no partner on its line stays an ordinary character.
$noiseRx = New-Object System.Text.RegularExpressions.Regex (
    '(?<=(?:[=(,:!&|?{};\[]|\breturn|\btypeof)[ \t]*)/(?![*/])(?:\\.|\[(?:\\.|[^\]\\\n])*\]|[^/\\\[\n])+/[A-Za-z]*' +
    '|//[^\n]*|/\*[\s\S]*?\*/|"(?:[^"\\\n]|\\.)*"|''(?:[^''\\\n]|\\.)*''|`(?:[^`\\]|\\.)*`')
$blank = [System.Text.RegularExpressions.MatchEvaluator] { param($m) [regex]::Replace($m.Value, '[^\n]', ' ') }
# An object opens with its type and a brace ("component Name: Type {" opens an inline component's root;
# "Qualifier.Type {" and "Type on property {" are objects too); a bare brace opens a script block; an id belongs
# to the innermost object open around it.
$tokenRx = New-Object System.Text.RegularExpressions.Regex (
    '(?<![A-Za-z0-9_.])(?:component\s+(?<inline>[A-Z][A-Za-z0-9_]*)\s*:\s*)?(?:[A-Za-z_][A-Za-z0-9_]*\.)*(?<type>[A-Z][A-Za-z0-9_]*)(?:\s+on\s+[A-Za-z_][A-Za-z0-9_.]*)?\s*\{' +
    '|(?<![A-Za-z0-9_.])id\s*:\s*(?<id>[A-Za-z_][A-Za-z0-9_]*)|\{|\}|\n')
$propRx = New-Object System.Text.RegularExpressions.Regex ('([A-Za-z_][A-Za-z0-9_.]*)\s*:\s*$')

function Test-Present([string]$rel, [hashtable]$over) {
    return ($over.ContainsKey($rel) -or (Test-Path -LiteralPath (Join-Path $repo $rel)))
}
function Get-Text([string]$rel, [hashtable]$over) {
    if ($over.ContainsKey($rel)) { return $over[$rel] }
    return [System.IO.File]::ReadAllText((Join-Path $repo $rel), $utf8)
}

function Read-Qml([string]$rel, [hashtable]$over) {
    $raw = Get-Text $rel $over
    $s = $noiseRx.Replace($raw, $blank)
    $objs = New-Object System.Collections.Generic.List[object]
    $stack = New-Object System.Collections.Generic.List[int]   # an object's index, or -1 for a script block
    $problems = @()
    $line = 1
    foreach ($m in $tokenRx.Matches($s)) {
        $v = $m.Value
        if ($v -eq "`n") { ++$line; continue }
        $inner = -1
        for ($i = $stack.Count - 1; $i -ge 0; --$i) { if ($stack[$i] -ge 0) { $inner = $stack[$i]; break } }
        if ($v -eq '}') {
            if ($stack.Count -eq 0) {
                $problems += "parse      ${rel}:${line}: a '}' with nothing open: the braces do not balance"
                continue
            }
            $k = $stack[$stack.Count - 1]; $stack.RemoveAt($stack.Count - 1)
            if ($k -ge 0) { $objs[$k].End = $m.Index }
        } elseif ($m.Groups['id'].Success) {
            if ($inner -ge 0 -and -not $objs[$inner].Id) { $objs[$inner].Id = $m.Groups['id'].Value }
        } elseif ($m.Groups['type'].Success) {
            # "name: Type {": the object is given to a property of the one around it, not declared in it.
            $lineStart = $s.LastIndexOf("`n", [Math]::Max(0, $m.Index - 1)) + 1
            $pm = $propRx.Match($s.Substring($lineStart, $m.Index - $lineStart))
            $objs.Add([pscustomobject]@{   # it starts at its type, as Qt's parser has it (after "component Name:")
                Type = $m.Groups['type'].Value; Start = $m.Groups['type'].Index; End = $s.Length; Parent = $inner; Line = $line; Id = ''
                Prop = $(if ($pm.Success) { $pm.Groups[1].Value } else { '' })
                Inline = $(if ($m.Groups['inline'].Success) { $m.Groups['inline'].Value } else { '' })
            })
            $stack.Add($objs.Count - 1)
            $line += ([regex]::Matches($v, "`n")).Count   # "Type`n{" spans a line break
        } else {
            $stack.Add(-1)
        }
    }
    if ($stack.Count -gt 0) { $problems += "parse      ${rel}: $($stack.Count) brace(s) left open at the end: the braces do not balance" }
    $roots = @($objs | Where-Object { $_.Parent -lt 0 })
    if ($roots.Count -gt 1) { $problems += "parse      ${rel}: $($roots.Count) root objects (lines $(($roots | ForEach-Object { $_.Line }) -join ', ')): the braces do not balance" }
    return [pscustomobject]@{ Rel = $rel; Raw = $raw; Bare = $s; Objs = $objs; Problems = $problems }
}

function Get-Ancestors($qml, $o) {
    $out = @(); $p = $o.Parent
    while ($p -ge 0) { $out += $qml.Objs[$p]; $p = $qml.Objs[$p].Parent }
    return $out
}

function Test-Within($inner, $outer) { return ($outer.Start -le $inner.Start -and $inner.End -le $outer.End) }

# The left-out entry for an object: its own id's, else the nearest id around it that is on the list; '' for none.
function Get-OptOutKey($qml, $o) {
    foreach ($x in @($o) + @(Get-Ancestors $qml $o)) {
        if ($x.Id -and $OptOut.Contains("$($qml.Rel)#$($x.Id)")) { return "$($qml.Rel)#$($x.Id)" }
    }
    return ''
}

# The literal key of a tag: its whole value one string, in either quote; else '' with Dynamic (a key per item).
function Get-TagKey($qml, $tag) {
    $bare = $qml.Bare.Substring($tag.Start, $tag.End - $tag.Start)
    $km = [regex]::Match($bare, '(?<![A-Za-z0-9_.])key\s*:')
    if (-not $km.Success) { return [pscustomobject]@{ Key = ''; Dynamic = $false; Missing = $true } }
    $from = $km.Index + $km.Length
    $to = $from
    while ($to -lt $bare.Length -and $bare[$to] -ne ';' -and $bare[$to] -ne "`n" -and $bare[$to] -ne '}') { ++$to }
    # The value runs to the binding's end in the blanked text (where strings are blank too); read from the raw
    # text, one string with at most a comment after it is a literal.
    $value = $qml.Raw.Substring($tag.Start + $from, $to - $from).Trim()
    $lm = [regex]::Match($value, '^(?:"((?:[^"\\]|\\.)*)"|''((?:[^''\\]|\\.)*)'')\s*(?:/\*.*?\*/|//.*)?$')
    if ($lm.Success) {
        $k = if ($lm.Groups[1].Success) { $lm.Groups[1].Value } else { $lm.Groups[2].Value }
        return [pscustomobject]@{ Key = $k; Dynamic = $false; Missing = $false }
    }
    return [pscustomobject]@{ Key = ''; Dynamic = $true; Missing = $false }
}

# --- the repository's QML types ------------------------------------------------------------------------------

$typeIndex = @{}
foreach ($f in Get-ChildItem (Join-Path $repo 'qml') -Recurse -Filter '*.qml' -File) {
    $rel = $f.FullName.Substring($repo.Length + 1).Replace('\', '/')
    if (-not $typeIndex.ContainsKey($f.BaseName)) { $typeIndex[$f.BaseName] = @() }
    $typeIndex[$f.BaseName] += $rel
}

function Resolve-Type([string]$type, [string]$fromRel) {
    if (-not $typeIndex.ContainsKey($type)) { return $null }
    $c = @($typeIndex[$type] | Where-Object { -not $_.StartsWith('qml/legacy_qml/') })   # in no module: never loaded
    if ($c.Count -eq 0) { return $null }
    if ($c.Count -eq 1) { return $c[0] }
    $dir = $fromRel.Substring(0, $fromRel.LastIndexOf('/'))
    $same = @($c | Where-Object { $_.StartsWith("$dir/") })
    if ($same.Count -ge 1) { return $same[0] }
    $lib = @($c | Where-Object { $_.StartsWith('qml/kqml_types/') })
    if ($lib.Count -ge 1) { return $lib[0] }
    return $c[0]
}

# A Loader's URL as a file of the repository: qrc:/qml/... is qml/...; a bare name is next to the file.
function Resolve-Source([string]$url, [string]$fromRel) {
    if ($url -match '^qrc:/(qml/.+\.qml)$') { return $Matches[1] }
    if ($url -match '^[A-Za-z0-9_./-]+\.qml$' -and $url -notmatch '^[a-z]+:') {
        return ($fromRel.Substring(0, $fromRel.LastIndexOf('/')) + '/' + $url)
    }
    return $null
}

$typeInfoCache = @{}
# How a type holds what is declared inside it: the levels from that content up to the instance (1 for a plain
# type; 2 for KIslandRow, whose default property puts its content into "slot"), and whether it is a KIslandRow.
function Get-TypeInfo([string]$type, [string]$fromRel, [hashtable]$over, [int]$depth = 0) {
    $rel = Resolve-Type $type $fromRel
    $key = "$type|$rel"
    if ($typeInfoCache.ContainsKey($key)) { return $typeInfoCache[$key] }
    $info = [pscustomobject]@{ Levels = 1; IsIslandRow = ($type -eq 'KIslandRow') }
    if ($rel -and $depth -lt 8) {
        $q = Read-Qml $rel $over
        if ($q.Objs.Count -gt 0) {
            $root = $q.Objs[0]
            $alias = [regex]::Match($q.Bare, '(?m)^\s*default\s+property\s+alias\s+\w+\s*:\s*(\w+)\.(?:data|children)\b')
            if ($alias.Success) {
                $target = $q.Objs | Where-Object { $_.Id -eq $alias.Groups[1].Value } | Select-Object -First 1
                if ($target) { $info.Levels = 1 + @(Get-Ancestors $q $target).Count }
            } elseif ($root.Type -ne $type) {
                $base = Get-TypeInfo $root.Type $rel $over ($depth + 1)
                $info.Levels = $base.Levels
                if ($base.IsIslandRow) { $info.IsIslandRow = $true }
            }
            if ($root.Type -ne $type -and -not $info.IsIslandRow) {
                if ((Get-TypeInfo $root.Type $rel $over ($depth + 1)).IsIslandRow) { $info.IsIslandRow = $true }
            }
        }
    }
    $typeInfoCache[$key] = $info
    return $info
}

# The object a tag marks, as KPinTag.qml's "block": the first KIslandRow among its first three parents as the
# app has them, else its parent. A declared parent of N levels stands for N parents; only its outermost is it.
function Get-TagBlock($qml, $tag, [hashtable]$over) {
    $declared = @(Get-Ancestors $qml $tag)
    if ($declared.Count -eq 0) { return $null }
    $seen = 0
    foreach ($d in $declared) {
        $ti = Get-TypeInfo $d.Type $qml.Rel $over
        $seen += $ti.Levels
        if ($seen -gt 3) { break }
        if ($ti.IsIslandRow) { return $d }
    }
    return $declared[0]
}

# Whether a tag's block mirrors a control: the control inside the block, and not inside an object given to one of
# the block's properties (a SettingsGroup's headerActions sits outside its content at run time; a KIslandRow's
# properties, "leading:", are its own parts and stay in).
function Test-Covers($qml, $tag, $ctl, [hashtable]$over) {
    $b = $tag.Block
    if (-not $b -or -not (Test-Within $ctl $b)) { return $false }
    if ((Get-TypeInfo $b.Type $qml.Rel $over).IsIslandRow) { return $true }
    if ($ctl.Start -eq $b.Start) { return $true }   # the block is the control itself
    $o = $ctl
    while ($o.Parent -ge 0 -and $qml.Objs[$o.Parent].Start -ne $b.Start) { $o = $qml.Objs[$o.Parent] }
    return -not ($o.Prop -and $o.Prop -ne 'data')
}

# --- the check -----------------------------------------------------------------------------------------------

$fileCache = @{}
function Get-FileResult([string]$rel, [hashtable]$over, [string[]]$path) {
    if ($fileCache.ContainsKey($rel)) { return $fileCache[$rel] }
    $q = Read-Qml $rel $over
    $res = [pscustomobject]@{
        Rel = $rel; Controls = @(); Tags = @(); Uncovered = @(); OptedOut = @(); NotFollowedUsed = @()
        Problems = @($q.Problems); Files = @($rel)
    }
    $fileCache[$rel] = $res   # a cycle sees an empty result rather than looping
    if ($q.Problems.Count -gt 0) { return $res }   # a tree that does not hold says nothing more

    $isApp = @($AppDirs | Where-Object { $rel.StartsWith($_) }).Count -gt 0
    $inlineRoots = @($q.Objs | Where-Object { $_.Inline })
    $inInline = {
        param($o)
        foreach ($r in $inlineRoots) { if (Test-Within $o $r) { return $r } }
        return $null
    }
    $merged = New-Object System.Collections.Generic.HashSet[string]
    $controls = @()

    # A followed file, from an instance of its type or a Loader's URL: its tags once per instance, its findings once.
    $follow = {
        param($o, [string]$tr)
        $sub = Get-FileResult $tr $over ($path + $rel)
        $res.Tags += $sub.Tags
        if (-not $merged.Contains($tr)) {
            [void]$merged.Add($tr)
            $res.Files += $sub.Files
            $res.Problems += $sub.Problems
            $res.OptedOut += $sub.OptedOut
            $res.NotFollowedUsed += $sub.NotFollowedUsed
            $res.Controls += $sub.Controls
        }
        if (@(Get-Ancestors $q $o) | Where-Object { $DelegateHosts -contains $_.Type }) {
            foreach ($t in $sub.Tags | Where-Object { -not $_.Dynamic -and $_.Key }) {
                $res.Problems += "repeated   $($t.Where): '$($t.Key)' is a literal key in $tr, which a delegate at ${rel}:$($o.Line) makes for every item"
            }
        }
        if ($sub.Uncovered.Count -gt 0) {
            $script:controlsOut += [pscustomobject]@{ Obj = $o; What = "$($o.Type) (holding $($sub.Uncovered.Count) uncovered: $(($sub.Uncovered | ForEach-Object { $_.Where }) -join '; '))"; Inner = $sub.Uncovered }
        }
    }

    foreach ($o in $q.Objs) {
        $inl = & $inInline $o
        if ($inl) {
            if ($ControlTypes -contains $o.Type -or $o.Type -eq 'KPinTag') {
                $res.Problems += "inline     ${rel}:$($o.Line): a $($o.Type) inside inline component $($inl.Inline) (line $($inl.Line)), which this check does not follow to its instances: give the row a file of its own"
            }
            continue
        }
        if ($ControlTypes -contains $o.Type) {
            $controls += [pscustomobject]@{ Obj = $o; What = $o.Type; Inner = @() }
            continue
        }
        if ($isApp -and $ForeignTypes -contains $o.Type) {
            $optKey = Get-OptOutKey $q $o
            if ($optKey) { $res.OptedOut += $optKey }
            else { $res.Problems += "foreign    ${rel}:$($o.Line): $($o.Type)$(if ($o.Id) { " id $($o.Id)" }) has no controlKind: a widget cannot mirror it, tagged or not; use the K control" }
            continue
        }
        if ($o.Type -eq 'KPinTag' -or $o.Type -eq 'Component') { continue }
        if ($o.Type -eq 'Loader') {
            $body = $q.Bare.Substring($o.Start, $o.End - $o.Start)
            $sm = [regex]::Match($body, '(?<![A-Za-z0-9_.])source\s*:')
            if ($sm.Success) {
                $vm = [regex]::Match($q.Raw.Substring($o.Start + $sm.Index + $sm.Length), '^\s*"([^"\n]*)"')
                if ($vm.Success) {
                    $src = Resolve-Source $vm.Groups[1].Value $rel
                    if ($src -and $NotFollowed.Contains($src)) { $res.NotFollowedUsed += $src }
                    elseif ($src -and (Test-Present $src $over) -and $path -notcontains $src -and $src -ne $rel) {
                        $script:controlsOut = @(); & $follow $o $src; $controls += $script:controlsOut
                    }
                }
            }
            continue
        }
        $tr = Resolve-Type $o.Type $rel
        if (-not $tr -or $tr -eq $rel -or $path -contains $tr) { continue }
        $script:controlsOut = @(); & $follow $o $tr; $controls += $script:controlsOut
    }

    $tags = @()
    foreach ($o in $q.Objs | Where-Object { $_.Type -eq 'KPinTag' -and -not (& $inInline $_) }) {
        $k = Get-TagKey $q $o
        $anc = @(Get-Ancestors $q $o)
        $tags += [pscustomobject]@{
            Obj = $o; Key = $k.Key; Dynamic = $k.Dynamic; Missing = $k.Missing
            Block = (Get-TagBlock $q $o $over); Where = "${rel}:$($o.Line)"
            InDelegate = [bool]($anc | Where-Object { $DelegateHosts -contains $_.Type })
            Component = ($anc | Where-Object { $_.Type -eq 'Component' } | Select-Object -First 1)
        }
    }
    $res.Tags += $tags

    foreach ($c in $controls) {
        $where = "${rel}:$($c.Obj.Line)"
        $cover = $tags | Where-Object { Test-Covers $q $_ $c.Obj $over } | Select-Object -First 1
        $optKey = if ($cover) { '' } else { Get-OptOutKey $q $c.Obj }
        $entry = [pscustomobject]@{ Where = $where; What = $c.What; Id = $c.Obj.Id; Key = $(if ($cover) { $cover.Key } else { '' }); LeftOut = $optKey }
        $res.Controls += $entry
        if ($cover) { continue }
        if ($optKey) { $res.OptedOut += $optKey; continue }
        $res.Uncovered += $entry
    }

    foreach ($t in $tags) {
        if ($t.Missing) { $res.Problems += "key        $($t.Where): a KPinTag with no key"; continue }
        if (-not $t.Dynamic -and $t.Key -notmatch '^app:[a-z0-9]+\.[A-Za-z0-9]+$') {
            $res.Problems += "key        $($t.Where): '$($t.Key)' is not app:<group>.<name>"
        }
        if (-not $t.Dynamic) {
            if ($t.InDelegate) {
                $res.Problems += "repeated   $($t.Where): '$($t.Key)' is a literal key inside a delegate: every item would carry it"
            } elseif ($t.Component) {
                # A Component is made once per Loader that loads it, once per item of a view it is the delegate of.
                $cmp = $t.Component
                $asDelegate = $cmp.Prop -eq 'delegate' -or ($cmp.Id -and $q.Bare -match "(?<![A-Za-z0-9_.])delegate\s*:\s*$($cmp.Id)\b")
                $loads = if ($cmp.Id) { ([regex]::Matches($q.Bare, "(?<![A-Za-z0-9_.])sourceComponent\s*:\s*$($cmp.Id)\b")).Count } else { 1 }
                if ($asDelegate -or $loads -gt 1) {
                    $res.Problems += "repeated   $($t.Where): '$($t.Key)' is a literal key in a Component made more than once ($(if ($asDelegate) { 'a delegate' } else { "$loads Loaders" }))"
                }
            }
        }
        $holds = @($controls | Where-Object { Test-Covers $q $t $_.Obj $over })
        if ($holds.Count -eq 0) {
            $res.Problems += "empty      $($t.Where): '$($t.Key)' marks a block with no control"
        }
    }
    return $res
}

function Invoke-Coverage([hashtable]$over) {
    $script:fileCache = @{}
    $script:typeInfoCache = @{}
    $r = Get-FileResult $Page $over @()
    $problems = @($r.Problems | Select-Object -Unique)
    foreach ($u in $r.Uncovered) {
        $idNote = if ($u.Id) { " id $($u.Id)" } elseif ($u.What -like '*holding*') { '' } else { ' (no id: tag it, or give it an id to leave it out)' }
        $problems += "uncovered  $($u.Where): $($u.What)$idNote"
    }
    foreach ($k in $OptOut.Keys) {
        if (@($r.OptedOut) -notcontains $k) { $problems += "stale      $k is on the left-out list but no uncovered control matches it" }
    }
    foreach ($k in $NotFollowed.Keys) {
        if (@($r.NotFollowedUsed) -notcontains $k) { $problems += "stale      $k is on the not-followed list but no Loader loads it" }
    }
    $keys = @($r.Tags | Where-Object { -not $_.Dynamic -and $_.Key } | ForEach-Object { $_.Key })
    foreach ($g in $keys | Group-Object | Where-Object { $_.Count -gt 1 }) {
        $at = ($r.Tags | Where-Object { $_.Key -eq $g.Name } | ForEach-Object { $_.Where }) -join ', '
        $problems += "duplicate  '$($g.Name)' on $($g.Count) tags: $at"
    }
    return [pscustomobject]@{
        Problems = $problems; Controls = @($r.Controls); Tags = @($r.Tags); OptedOut = @($r.OptedOut | Select-Object -Unique)
        Files = @($r.Files | Select-Object -Unique)
    }
}

# --- the block model against KPinTag.qml at run time -------------------------------------------------------

# Each tag and each object it could mark has an id (read here) and the same objectName (read at run time);
# the run time's block is named by its first named ancestor-or-self, as an inner item (KIslandRow's slot,
# KIsland's row column) belongs to the object declared around it.
$shapes = @'
import QtQuick
import QtQuick.Layouts
import QtTest
import kqml_types 1.0

Item {
    id: scene
    width: 600; height: 900
    Column {
        id: c0; objectName: "c0"
        width: 500
        KIslandRow { id: r1; objectName: "r1"; label: "One:"; KPinTag { id: t1; objectName: "t1"; key: "app:shape.one" } }
        KIslandRow { id: r2; objectName: "r2"; label: "Two:"
            KSwitch { id: s2; objectName: "s2"; KPinTag { id: t2; objectName: "t2"; key: "app:shape.two" } } }
        KIslandRow { id: r3; objectName: "r3"; label: "Three:"
            Row { id: w3; objectName: "w3"
                KSwitch { id: s3; objectName: "s3"; KPinTag { id: t3; objectName: "t3"; key: "app:shape.three" } } } }
        KIslandRow { id: r4; objectName: "r4"; label: "Four:"
            Item { id: i4; objectName: "i4"; width: 200; height: 30
                Row { id: w4; objectName: "w4"
                    KSpinBox { id: p4; objectName: "p4"; KPinTag { id: t4; objectName: "t4"; key: "app:shape.four" } } } } }
        RowLayout { id: l5; objectName: "l5"; width: 400
            KPinTag { id: t5; objectName: "t5"; key: "app:shape.five"; label: "Five:" }
            KCombo { id: k5; objectName: "k5"; model: ["a", "b"] } }
        KIsland { id: i6; objectName: "i6"
            KIslandRow { id: r6; objectName: "r6"; label: "Six:"; KPinTag { id: t6; objectName: "t6"; key: "app:shape.six" } } }
        KIsland { id: i7; objectName: "i7"
            KPinTag { id: t7; objectName: "t7"; key: "app:shape.seven"; label: "Seven" }
            KIslandRow { id: r7; objectName: "r7"; label: "Seven:" } }
        KIslandRow { id: r8; objectName: "r8"; label: "Eight:"
            RowLayout { id: l8; objectName: "l8"
                KPinTag { id: t8; objectName: "t8"; key: "app:shape.eight"; label: "Eight" }
                KSwitch { id: s8; objectName: "s8" } } }
        KIsland { id: i9; objectName: "i9"
            KIslandRow { id: r9; objectName: "r9"; label: "Nine:"
                RowLayout { id: l9; objectName: "l9"
                    KSlider { id: d9; objectName: "d9"; KPinTag { id: t9; objectName: "t9"; key: "app:shape.nine"; label: "Nine" } } } } }
    }
    TestCase {
        name: "Blocks"
        when: windowShown
        function named(it) { for (var p = it; p; p = p.parent) if (p.objectName) return p.objectName; return "?" }
        function test_blocks() {
            var tags = [t1, t2, t3, t4, t5, t6, t7, t8, t9]
            for (var i = 0; i < tags.length; ++i)
                console.log("BLOCK " + tags[i].objectName + " " + named(tags[i].block))
        }
    }
}
'@

function Invoke-RuntimeModel {
    $runner = Join-Path $QtBin 'qmltestrunner.exe'
    if (-not (Test-Path $runner)) {
        Write-Host "pin_coverage_check: no qmltestrunner at $runner (pass -QtBin or set QT_BIN)"
        exit 2
    }
    # What this script makes of the shapes: the shapes file read as if it sat next to the page.
    $rel = 'qml/app/__pin_shapes.qml'
    $script:typeInfoCache = @{}
    $q = Read-Qml $rel @{ $rel = $shapes }
    $static = @{}
    foreach ($t in $q.Objs | Where-Object { $_.Type -eq 'KPinTag' }) {
        $b = Get-TagBlock $q $t @{}
        while ($b -and -not $b.Id) { $b = if ($b.Parent -ge 0) { $q.Objs[$b.Parent] } else { $null } }
        $static[$t.Id] = $(if ($b) { $b.Id } else { '?' })
    }

    $work = Join-Path ([System.IO.Path]::GetTempPath()) ('pin_cover_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Force $work | Out-Null
    try {
        $mod = Join-Path $work 'imports\kqml_types'
        New-Item -ItemType Directory -Force $mod | Out-Null
        $lines = @('module kqml_types')
        foreach ($f in Get-ChildItem (Join-Path $repo 'qml\kqml_types') -File | Where-Object { $_.Extension -in '.qml', '.js' } | Sort-Object Name) {
            $text = [System.IO.File]::ReadAllText($f.FullName, $utf8)
            $single = $f.Extension -eq '.qml' -and $text -match '(?m)^\s*pragma\s+Singleton'
            if ($f.Name -in 'AppPalette.qml', 'Tokens.qml') {
                # the app's C++ context property "theme"; null here, so the files take their own fallbacks
                $text = ([regex]'QtObject \{').Replace($text, "QtObject {`n    readonly property var theme: null", 1)
            }
            [System.IO.File]::WriteAllText((Join-Path $mod $f.Name), $text, $utf8)
            $lines += ('{0}{1} 1.0 {2}' -f $(if ($single) { 'singleton ' } else { '' }), $f.BaseName, $f.Name)
        }
        [System.IO.File]::WriteAllText((Join-Path $mod 'qmldir'), (($lines -join "`n") + "`n"), $utf8)
        $fonts = Join-Path $work 'fonts'
        New-Item -ItemType Directory -Force $fonts | Out-Null
        Copy-Item (Join-Path $repo 'resources\fonts\arial.ttf') $fonts
        $env:QT_QPA_FONTDIR = $fonts
        $env:PATH = "$QtBin;$env:PATH"
        $path = Join-Path $work 'tst_pin_shapes.qml'
        [System.IO.File]::WriteAllText($path, $shapes, $utf8)
        $results = Join-Path $work 'results.txt'
        $p = Start-Process -FilePath $runner -NoNewWindow -Wait -PassThru `
            -ArgumentList @('-platform', 'offscreen', '-import', "`"$(Join-Path $work 'imports')`"", '-input', "`"$path`"",
                            '-o', "`"$results,txt`"") `
            -RedirectStandardOutput (Join-Path $work 'run.out') -RedirectStandardError (Join-Path $work 'run.err')
        $out = if (Test-Path $results) { @(Get-Content $results -Encoding UTF8) } else { @() }
        if ($p.ExitCode -ne 0 -or $out.Count -eq 0) {
            $out + @(Get-Content (Join-Path $work 'run.err') -Encoding UTF8) | Select-Object -Last 20 | ForEach-Object { Write-Host $_ }
            return @('runtime    qmltestrunner did not run the shapes')
        }
    }
    finally {
        Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
    }
    $problems = @()
    $got = @{}
    foreach ($l in $out) { if ($l -match 'BLOCK (\w+) (\S+)') { $got[$Matches[1]] = $Matches[2] } }
    foreach ($id in ($static.Keys | Sort-Object)) {
        $rt = if ($got.ContainsKey($id)) { $got[$id] } else { '(not reported)' }
        $ok = $rt -eq $static[$id]
        Write-Host ("{0} {1}: run time {2}, this script {3}" -f $(if ($ok) { 'ok  ' } else { 'FAIL' }), $id, $rt, $static[$id])
        if (-not $ok) { $problems += "runtime    the block of $id is $rt at run time, $($static[$id]) here" }
    }
    return $problems
}

$res = Invoke-Coverage @{}
if ($List) {
    foreach ($c in $res.Controls) {
        $how = if ($c.Key) { $c.Key } elseif ($c.LeftOut) { "(left out: $($c.LeftOut.Split('#')[1]))" } else { '(NONE)' }
        Write-Host ("{0,-44} {1,-10} {2,-24} {3}" -f $c.Where, $c.What, $c.Id, $how)
    }
}
Write-Host ("pin_coverage_check: {0} controls in {1} files, {2} tags, {3} left out on purpose" -f `
    $res.Controls.Count, $res.Files.Count, $res.Tags.Count, $res.OptedOut.Count)
$problems = @($res.Problems)
if ($Runtime) { $problems += @(Invoke-RuntimeModel) }
$problems | ForEach-Object { Write-Host "FAIL $_" }
if ($problems.Count -gt 0) {
    Write-Host 'pin_coverage_check: failed'
    exit 1
}
Write-Host 'pin_coverage_check: all hold'
if (-not $Mutants) { exit 0 }

# One fault each, in copies held in memory, each expected to fail with the named finding; then copies that must
# still hold. The text to replace must be there: a mutant that changes nothing would pass and look like a gap.
$mq = 'qml/app/MosaicQualitySettings.qml'
$ap = 'qml/app/AppSettingsPage.qml'
$angle = '    // The measured angle part'
$probe = 'qml/app/PinProbeRowX.qml'   # a file of the mutant's own, read from memory only
$faults = @(
    @{ Name = 'a quality row untagged'; Expect = 'uncovered'
       Edits = @(, @($mq, '        KPinTag { key: "app:mosaic.overlap"; label: qsTr("Overlaps:") }', '')) },
    @{ Name = 'a page row untagged'; Expect = 'uncovered'
       Edits = @(, @($ap, 'KPinTag { key: "app:mosaic.angle"; label: qsTr("Angle, °:") }', '')) },
    @{ Name = 'a key on two rows'; Expect = 'duplicate'
       Edits = @(, @($ap, 'KPinTag { key: "app:mosaic.angle";', 'KPinTag { key: "app:mosaic.traceLine";')) },
    @{ Name = 'a key on two rows, single-quoted'; Expect = 'duplicate'
       Edits = @(, @($ap, 'KPinTag { key: "app:mosaic.angle";', "KPinTag { key: 'app:mosaic.traceLine';")) },
    @{ Name = 'a left-out row tagged'; Expect = 'stale'
       Edits = @(, @($ap, "id: tapTolerSlider`r`n", "id: tapTolerSlider`r`n                KPinTag { key: `"app:test.tapTolerance`"; label: `"Double-tap`" }`r`n")) },
    @{ Name = 'a new untagged switch'; Expect = 'uncovered'
       Edits = @(, @($mq, $angle, "    KSwitch { id: newSwitch; text: `"New`" }`r`n$angle")) },
    @{ Name = 'a module-qualified untagged switch'; Expect = 'uncovered'
       Edits = @(@($mq, 'import kqml_types 1.0', "import kqml_types 1.0`r`nimport kqml_types 1.0 as K"),
                 @($mq, $angle, "    K.KSwitch { id: newSwitch; text: `"New`" }`r`n$angle")) },
    @{ Name = 'a literal key in a Repeater'; Expect = 'repeated'
       Edits = @(, @($mq, $angle, "    Repeater { model: 2; KSwitch { KPinTag { key: `"app:mosaic.each`" } } }`r`n$angle")) },
    @{ Name = 'a literal key in a Component used as a delegate'; Expect = 'repeated'
       Edits = @(, @($mq, $angle, "    Component { id: rowDelegate; KSwitch { KPinTag { key: `"app:mosaic.each`" } } }`r`n    Repeater { model: 2; delegate: rowDelegate }`r`n$angle")) },
    @{ Name = 'a literal key in a file a delegate makes'; Expect = 'repeated'
       Edits = @(@($probe, '', "import QtQuick`r`nimport kqml_types 1.0`r`nKSwitch {`r`n    KPinTag { key: `"app:probe.each`" }`r`n}`r`n"),
                 @($mq, $angle, "    Repeater { model: 2; delegate: PinProbeRowX { } }`r`n$angle")) },
    @{ Name = 'a tagged group used twice'; Expect = 'duplicate'
       Edits = @(, @($ap, '    PerformanceSettingsGroup {', "    PerformanceSettingsGroup { width: root.groupWidth; store: root.store }`r`n    PerformanceSettingsGroup {")) },
    @{ Name = 'a Qt switch on the page'; Expect = 'foreign'
       Edits = @(, @($mq, $angle, "    Switch { id: qtSwitch; text: `"Qt`" }`r`n$angle")) },
    @{ Name = 'a tagged ParamCard'; Expect = 'foreign'
       Edits = @(, @($mq, $angle, "    ParamCard { KPinTag { key: `"app:mosaic.card`" }; label: `"Card`"; KSpinBox { } }`r`n$angle")) },
    @{ Name = 'an untagged KCheckBox'; Expect = 'foreign'
       Edits = @(, @($mq, $angle, "    KCheckBox { id: check; text: `"Check`" }`r`n$angle")) },
    @{ Name = 'a switch in an inline component'; Expect = 'inline'
       Edits = @(, @($mq, $angle, "    component SwitchRow: RowLayout { KPinTag { key: `"app:mosaic.inlineRow`" } KSwitch { } }`r`n    SwitchRow { }`r`n$angle")) },
    @{ Name = 'a regex literal in a tagged row, the next row untagged'; Expect = 'uncovered'
       Edits = @(@($mq, '        KPinTag { key: "app:mosaic.farCrop"; label: qsTr("Crop the far edge, °:") }', ''),
                 @($mq, '        KPinTag { key: "app:mosaic.nadirCrop"; label: qsTr("Crop near the track, °:") }',
                   "        KPinTag { key: `"app:mosaic.nadirCrop`"; label: qsTr(`"Crop near the track, °:`") }`r`n        function shown(p) { return String(p).replace(/\//g, `"|`") }")) },
    @{ Name = 'a header switch beside a tag in the group'; Expect = 'uncovered'
       Edits = @(, @($ap, '    PerformanceSettingsGroup {', "    SettingsGroup {`r`n        title: `"Probe`"`r`n        stateKey: `"app.probe`"`r`n        headerActions: KSwitch { id: probeHeader }`r`n        KPinTag { key: `"app:probe.one`" }`r`n        KSwitch { id: probeOne }`r`n    }`r`n    PerformanceSettingsGroup {")) },
    @{ Name = 'braces that do not balance'; Expect = 'parse'
       Edits = @(, @($mq, $angle, "    Item { `r`n$angle")) },
    # The tag one level down, inside the first spin box: its row (a KIslandRow) is now four parents up, past the
    # three KPinTag looks at, so the tag marks the spin box alone and the other two controls lose their pin.
    @{ Name = 'a tag too deep for its row'; Expect = 'uncovered'
       Edits = @(@($ap, "                KPinTag { key: `"app:dataset.fbs`" }`r`n", ''),
                 @($ap, 'id: fixBlackStripesForwardStepsSpinBox', 'id: fixBlackStripesForwardStepsSpinBox; KPinTag { key: "app:dataset.fbs" }')) }
)
$holds = @(
    @{ Name = 'a computed key in a Repeater (a key per item)'
       Edits = @(, @($mq, $angle, "    Repeater { model: [`"a`", `"b`"]; KSwitch { required property string modelData; KPinTag { key: `"app:mosaic.`" + modelData } } }`r`n$angle")) },
    @{ Name = 'a comment after a key'
       Edits = @(, @($mq, 'KPinTag { key: "app:mosaic.overlap"; label', 'KPinTag { key: "app:mosaic.overlap" /* the combo */; label')) }
)

function Get-Mutated($m) {
    $over = @{}
    foreach ($e in $m.Edits) {
        $file = $e[0]; $from = $e[1]; $to = $e[2]
        if ($from -eq '') { $over[$file] = $to; continue }   # a file of the mutant's own
        $text = if ($over.ContainsKey($file)) { $over[$file] } else { Get-Text $file @{} }
        if (-not $text.Contains($from)) {   # a checkout with LF line ends
            $from = $from.Replace("`r`n", "`n"); $to = $to.Replace("`r`n", "`n")
            if (-not $text.Contains($from)) { throw "mutant '$($m.Name)': its text is not in $file" }
        }
        $over[$file] = $text.Replace($from, $to)
    }
    if ($over.ContainsKey($probe)) {   # known to the type index for the time of this mutant
        $script:typeIndex['PinProbeRowX'] = @($probe)
    }
    return $over
}

$missedFaults = 0
foreach ($m in $faults) {
    $rm = Invoke-Coverage (Get-Mutated $m)
    $script:typeIndex.Remove('PinProbeRowX')
    $hit = @($rm.Problems | Where-Object { $_.StartsWith($m.Expect) })
    if ($hit.Count -gt 0) {
        Write-Host ("mutant ({0}): caught, {1}" -f $m.Name, $hit[0])
    } else {
        Write-Host ("mutant ({0}): MISSED{1}" -f $m.Name, $(if ($rm.Problems.Count) { " (other: $($rm.Problems[0]))" } else { '' }))
        ++$missedFaults
    }
}
$failedHolds = 0
foreach ($m in $holds) {
    $rm = Invoke-Coverage (Get-Mutated $m)
    if ($rm.Problems.Count -eq 0) {
        Write-Host ("holds ({0}): all hold" -f $m.Name)
    } else {
        Write-Host ("holds ({0}): FAILED, {1}" -f $m.Name, $rm.Problems[0])
        ++$failedHolds
    }
}
Write-Host ("pin_coverage_check mutants: {0} of {1} caught; {2} of {3} that must hold hold" -f `
    ($faults.Count - $missedFaults), $faults.Count, ($holds.Count - $failedHolds), $holds.Count)
exit ([int](($missedFaults + $failedHolds) -gt 0))
