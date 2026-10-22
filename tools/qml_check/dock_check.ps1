<#
.SYNOPSIS
  Offline check of the docks' layout math (qml/app/DockLayout.js; docs/widget-docking.ru.md).

.DESCRIPTION
  Runs the real file under qmltestrunner (offscreen, no window). Checks:
    1. normalize: anything stored comes back as four docks, an id in one dock at most, scales snapped, ids of
       deleted widgets dropped;
    2. withWidget / withoutWidget: a widget moves between docks, lands at the index asked, opens a collapsed dock;
    3. compute: side docks the window's height, top and bottom between them; widgets share a dock's length in
       proportion to theirs, edge to edge, to the pixel; a dock as thick as its thickest widget at its scale, a
       widget that does not scale at its own size;
    4. a collapsed dock takes no room, its widgets listed as collapsed; a hidden widget takes none either;
    5. fits and scaleSteps: the workspace keeps half the window each way; the steps the thickness handle offers;
    6. a window too small for its docks squeezes them, and the scale to draw at follows;
    7. nothing breaks on a window of 0, sizes not reported, ids nobody knows;
    8. the lines: a dock its edge line thicker than its widgets, the line on its inner side, a gap between two
       widgets; fits and the steps count the line, as compute does.
  -Mutants runs the tests again over the file broken on purpose, one way at a time: each has to be caught.
  Exit code 0 when all pass, 1 when a check fails, 2 when qmltestrunner is not found.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\qml_check\dock_check.ps1 [-QtBin D:\Qt\6.8.3\llvm-mingw_64\bin] [-Mutants]
#>
param(
    [string]$QtBin = $(if ($env:QT_BIN) { $env:QT_BIN } else { 'D:\Qt\6.8.3\llvm-mingw_64\bin' }),
    [switch]$Mutants
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$runner = Join-Path $QtBin 'qmltestrunner.exe'
if (-not (Test-Path $runner)) {
    Write-Host "dock_check: no qmltestrunner at $runner (pass -QtBin or set QT_BIN)"
    exit 2
}
$utf8 = New-Object System.Text.UTF8Encoding $false
function Write-Utf8([string]$path, [string]$text) { [System.IO.File]::WriteAllText($path, $text, $utf8) }

$test = @'
import QtQuick
import QtTest
import "DockLayout.js" as D

TestCase {
    name: "Dock"

    readonly property var sizes: ({
        a: { w: 200, h: 300, scalable: true },
        b: { w: 120, h: 100, scalable: true },
        c: { w: 400, h: 100, scalable: true },
        s: { w: 260, h: 180, scalable: false }
    })
    readonly property var all: ({ a: true, b: true, c: true, s: true })

    function docksWith(side, ids, scale, collapsed) {
        var d = D.emptyDocks()
        d[side].widgets = ids
        if (scale !== undefined) d[side].scale = scale
        if (collapsed !== undefined) d[side].collapsed = collapsed
        return d
    }

    // Each dock's widgets edge to edge over its span (lines.gap apart), as thick as the dock less its edge line,
    // the line on the dock's inner side; nothing outside the dock.
    function checkTiling(res, lines) {
        var edge = lines ? lines.edge : 0, gap = lines ? lines.gap : 0
        for (var i = 0; i < D.SIDES.length; ++i) {
            var side = D.SIDES[i], dock = res.docks[side], v = D.isVertical(side)
            var outer = side === "left" || side === "top"
            var at = dock.from
            for (var k = 0; k < dock.ids.length; ++k) {
                var r = res.widgets[dock.ids[k]]
                if (k > 0)
                    at += gap
                compare(v ? r.y : r.x, at, side + ": widget " + k + " starts a gap after the previous one")
                compare(v ? r.x : r.y, (v ? dock.x : dock.y) + (outer ? 0 : edge), side + ": widget " + k + " on the dock")
                compare(v ? r.w : r.h, dock.thickness - edge, side + ": widget " + k + " as thick as the dock less its line")
                at += v ? r.h : r.w
            }
            if (dock.ids.length > 0)
                compare(at, dock.to, side + ": the widgets fill the dock's length")
        }
    }

    function test_1_normalize() {
        var e = D.normalize(null)
        compare(Object.keys(e).sort().join(","), "bottom,left,right,top")
        compare(e.left.widgets.length, 0)
        compare(e.left.scale, 1.0)
        compare(e.left.collapsed, false)
        var n = D.normalize({ left: { widgets: ["a", "a", 7, "", "b"], scale: 1.4, collapsed: "yes" },
                              right: { widgets: ["b", "c"], scale: 9 },
                              top: "junk", bottom: { widgets: "c" } })
        compare(n.left.widgets.join(","), "a,b", "duplicates, non-strings and empty ids dropped")
        compare(n.right.widgets.join(","), "c", "an id already in a dock is not taken twice")
        compare(n.left.scale, 1.5, "1.4 snaps to 1.5")
        compare(n.right.scale, 2.5, "9 snaps to the largest step")
        compare(n.left.collapsed, false, "only true collapses")
        compare(n.top.widgets.length, 0)
        compare(n.bottom.widgets.length, 0)
        var v = D.normalize({ right: { widgets: ["a", "gone", "b"] } }, ["a", "b"])
        compare(v.right.widgets.join(","), "a,b", "a deleted widget's id dropped")
        compare(D.snapScale(NaN), 1.0)
        compare(D.snapScale(0.1), 0.75)
    }

    function test_2_move() {
        var d = D.withWidget(D.emptyDocks(), "a", "right", -1)
        d = D.withWidget(d, "b", "right", -1)
        d = D.withWidget(d, "c", "right", 0)
        compare(d.right.widgets.join(","), "c,a,b", "inserted at the index asked")
        d = D.withWidget(d, "a", "left", 5)
        compare(d.right.widgets.join(","), "c,b", "moved out of the dock it was in")
        compare(d.left.widgets.join(","), "a", "an index past the end puts it at the end")
        compare(D.sideOf(d, "a"), "left")
        compare(D.sideOf(d, "zz"), "")
        d = D.withCollapsed(d, "left", true)
        compare(d.left.collapsed, true)
        d = D.withWidget(d, "b", "left", 0)
        compare(d.left.collapsed, false, "a widget put into a collapsed dock opens it")
        compare(d.left.widgets.join(","), "b,a")
        d = D.withoutWidget(d, "b")
        compare(D.sideOf(d, "b"), "")
        compare(D.withWidget(D.emptyDocks(), "a", "middle", 0).left.widgets.length, 0, "no such side")
        var before = D.emptyDocks()
        D.withWidget(before, "a", "left", 0)
        compare(before.left.widgets.length, 0, "the docks given are not changed")
    }

    function test_3_compute() {
        var d = docksWith("right", ["a", "b"])
        d.left.widgets = ["c"]
        d.bottom.widgets = ["s"]
        var r = D.compute(d, sizes, all, 1920, 1080)
        compare(r.squeezed, false)
        compare(r.insets.right, 200, "a dock as thick as its thickest widget")
        compare(r.insets.left, 400)
        compare(r.insets.bottom, 180)
        compare(r.insets.top, 0)
        var a = r.widgets.a, b = r.widgets.b
        compare(a.x, 1720); compare(a.y, 0); compare(a.w, 200)
        compare(a.h, 810, "lengths 300:100 share 1080 as 810:270")
        compare(b.y, 810); compare(b.h, 270)
        compare(r.docks.right.edge, 1720, "the inner edge")
        compare(r.docks.bottom.from, 400, "the bottom dock starts after the left one")
        compare(r.docks.bottom.to, 1720, "and ends before the right one")
        compare(r.widgets.s.y, 900)
        compare(r.widgets.s.w, 1320, "a lone widget stretched over the dock's length")
        compare(r.docks.bottom.edge, 900)
        checkTiling(r)

        var t = docksWith("top", ["b", "c"], 1.5)
        var rt = D.compute(t, sizes, all, 1920, 1080)
        compare(rt.insets.top, 150, "100 high at 1.5")
        compare(rt.widgets.b.scale, 1.5)
        compare(rt.widgets.b.w + rt.widgets.c.w, 1920)
        compare(rt.widgets.b.w, Math.round(1920 * 180 / 780), "lengths 180:600 at 1.5")
        checkTiling(rt)

        var mixed = docksWith("left", ["b", "s"], 2.0)
        var rm = D.compute(mixed, sizes, all, 1920, 1080)
        compare(rm.insets.left, 260, "120 at 2.0 is 240, the settings widget's own 260 is thicker")
        compare(rm.widgets.s.scale, 1.0, "a widget that does not scale stays at 1")
        compare(rm.widgets.b.scale, 2.0)
        compare(rm.widgets.b.h, Math.round(1080 * 200 / 380), "lengths 200 (100 at 2.0) and 180 (not scaled)")
        checkTiling(rm)

        var odd = docksWith("right", ["a", "b", "c", "s"])
        checkTiling(D.compute(odd, sizes, all, 1917, 1079))
    }

    function test_4_collapsed_hidden() {
        var d = docksWith("right", ["a", "b"], 1.0, true)
        var r = D.compute(d, sizes, all, 1920, 1080)
        compare(r.insets.right, 0, "a collapsed dock takes no room")
        compare(r.docks.right.collapsed, true)
        compare(r.docks.right.edge, 1920, "its tab at the window's edge")
        verify(!!r.widgets.a, "its widgets are listed")
        compare(r.widgets.a.collapsed, true, "as collapsed, so they hide rather than float")

        var h = docksWith("right", ["a", "b"])
        var rh = D.compute(h, sizes, { b: true }, 1920, 1080)
        compare(rh.insets.right, 120, "a hidden widget takes no room")
        verify(!rh.widgets.a, "and is not laid out")
        compare(rh.widgets.b.h, 1080)

        var none = D.compute(h, sizes, {}, 1920, 1080)
        compare(none.insets.right, 0)
        compare(none.docks.right.ids.length, 0)
        compare(none.docks.right.collapsed, false, "no tab for a dock with nothing shown")
        compare(D.compute(docksWith("right", ["a"], 1.0, true), sizes, {}, 1920, 1080).docks.right.collapsed, false)
    }

    function test_5_fits_steps() {
        var d = docksWith("left", ["c"], 1.5)            // 600
        d.right.widgets = ["a"]                          // 200 -> 800 of 960
        verify(D.fits(d, sizes, all, 1920, 1080))
        d.right.scale = 2.0                              // 400 -> 1000
        verify(!D.fits(d, sizes, all, 1920, 1080), "the workspace keeps half the width")
        verify(D.fits(D.withCollapsed(d, "left", true), sizes, all, 1920, 1080), "a collapsed dock counts as 0")
        var steps = D.scaleSteps(d, sizes, all, 1920, 1080, "right")
        compare(steps.map(function(s) { return s.scale }).join(","), "0.75,1,1.25,1.5",
                "with 600 on the left the right dock goes up to 360 of 200 * k")
        compare(steps[0].thickness, 150)
        compare(steps[steps.length - 1].thickness, 300)
        var tall = docksWith("top", ["a"])               // 300 high
        tall.bottom.widgets = ["s"]                      // 180 -> 480 of 540
        verify(D.fits(tall, sizes, all, 1920, 1080))
        verify(!D.fits(D.withScale(tall, "top", 1.25), sizes, all, 1920, 1080), "and half the height")
        var flat = D.scaleSteps(docksWith("left", ["s"]), sizes, all, 1920, 1080, "left")
        compare(flat.length, 1, "a dock of widgets that do not scale has one thickness")
        compare(D.scaleSteps(d, sizes, all, 1920, 1080, "middle").length, 0)
    }

    function test_6_squeeze() {
        var d = docksWith("left", ["a"], 2.0)            // 400
        d.right.widgets = ["a2"]
        var sz = { a: sizes.a, a2: { w: 400, h: 300, scalable: true } }
        var r = D.compute(d, sz, { a: true, a2: true }, 1000, 800)
        compare(r.squeezed, true)
        compare(r.insets.left, 250, "400 + 400 squeezed into 500")
        compare(r.insets.right, 250)
        compare(r.widgets.a.scale, 2.0 * 0.625, "the scale to draw at follows")
        compare(r.widgets.a.w, 250)
        checkTiling(r)
        compare(D.compute(d, sz, { a: true, a2: true }, 1920, 1080).squeezed, false)
    }

    function test_7_degenerate() {
        var d = docksWith("right", ["a", "nosize", "b"])
        d.left.widgets = ["s"]
        var z = D.compute(d, sizes, all, 0, 0)
        compare(z.insets.right, 0, "no window, no room")
        verify(!!z.widgets.a, "the widgets are still docked, so they do not float for a frame")
        compare(z.widgets.a.h, 0)
        var r = D.compute(d, sizes, { a: true, b: true, nosize: true, s: true }, 1920, 1080)
        verify(!r.widgets.nosize, "a widget that has not reported its size waits")
        compare(r.widgets.a.h + r.widgets.b.h, 1080)
        var bad = D.compute(d, { a: { w: NaN, h: 5 }, b: { w: 10, h: -1 } }, all, 1920, 1080)
        compare(bad.insets.right, 0)
        var junk = D.compute("junk", null, null, NaN, undefined)
        compare(junk.insets.left, 0)
        compare(Object.keys(junk.widgets).length, 0)
    }

    function test_8_lines() {
        var L = { edge: 2, gap: 1 }
        var d = docksWith("right", ["a", "b"])
        d.left.widgets = ["c"]
        d.top.widgets = ["b2"]
        d.bottom.widgets = ["s", "c2"]
        var sz = { a: sizes.a, b: sizes.b, c: sizes.c, s: sizes.s, b2: sizes.b, c2: sizes.c }
        var sh = { a: true, b: true, c: true, s: true, b2: true, c2: true }
        var r = D.compute(d, sz, sh, 1920, 1080, L)
        compare(r.insets.right, 202, "the dock is its edge line thicker than its widgets")
        compare(r.widgets.a.w, 200, "the widgets keep their own thickness")
        compare(r.widgets.a.x, 1720, "after the line on the dock's inner side")
        compare(r.docks.right.edge, 1718)
        compare(r.widgets.c.x, 0, "a left dock's line on its right")
        compare(r.widgets.c.w, 400)
        compare(r.widgets.b.y, r.widgets.a.h + 1, "1 px between two widgets")
        checkTiling(r, L)
        checkTiling(D.compute(docksWith("right", ["a", "b", "c", "s"]), sizes, all, 1917, 1079, L), L)
        compare(D.compute(docksWith("right", ["a"], 1.0, true), sizes, all, 1920, 1080, L).insets.right, 0,
                "a collapsed dock has no line either")
        var e = docksWith("left", ["c"], 1.5)            // 600 + 2
        e.right.widgets = ["a"]
        e.right.scale = 1.5                              // 300 + 2
        verify(D.fits(e, sizes, all, 1920, 1080, L), "602 + 302 of 960")
        var steps = D.scaleSteps(e, sizes, all, 1920, 1080, "right", L)
        compare(steps.map(function(x) { return x.thickness }).join(","), "152,202,252,302",
                "a step's thickness counts the line; 2.0 (402) would leave the workspace 916 of 1920")
        var tight = docksWith("left", ["c"], 1.5)
        tight.right.widgets = ["a2"]
        var sz2 = { c: sizes.c, a2: { w: 358, h: 300, scalable: true } }
        verify(D.fits(tight, sz2, { c: true, a2: true }, 1920, 1080), "958 of 960 without lines")
        verify(!D.fits(tight, sz2, { c: true, a2: true }, 1920, 1080, L), "962 with them: what compute would squeeze")
        verify(D.compute(tight, sz2, { c: true, a2: true }, 1920, 1080, L).squeezed)
    }
}
'@

# -Mutants: the same tests over DockLayout.js broken on purpose, one way at a time; each has to fail.
$mutantList = @(
    @('MIN_WORKSPACE_SHARE = 0.5', 'MIN_WORKSPACE_SHARE = 0.4', 'the workspace''s share'),
    @('thick = Math.max(thick, across * k)', 'thick = Math.max(thick, across)', 'the scale left out of the thickness'),
    @('var k = s.scalable === false ? 1.0 : dock.scale', 'var k = dock.scale', 'a settings widget scaled'),
    @('(ids.length > 0 && !dock.collapsed) ? Math.round(thick) : 0', '(ids.length > 0) ? Math.round(thick) : 0', 'a collapsed dock keeps its room'),
    @('var to = vertical ? h : Math.max(from, w - t.right)', 'var to = vertical ? h : w', 'top and bottom under the right dock'),
    @('var a = from + k * gapLine + Math.round(span * acc / total)', 'var a = from + Math.round(span * acc / total)', 'no gap between widgets'),
    @('edge = w - th; across = edge + th - inner', 'edge = w - th; across = edge', 'the right dock''s line under its widgets'),
    @('    list.splice(at, 0, id)
    d[side].collapsed = false', '    list.splice(at, 0, id)', 'a collapsed dock stays shut'),
    @('id === "" || seen[id] === true', 'id === ""', 'an id in two docks'),
    @('(a + b > room && a + b > 0) ? room / (a + b) : 1.0', '1.0', 'no squeeze'),
    @('if (out.length === 0 || out[out.length - 1].thickness !== t)', 'if (true)', 'the same thickness offered twice'),
    @('if (Array.isArray(validIds) && validIds.indexOf(id) < 0)', 'if (false)', 'a deleted widget kept'),
    @('Math.round(lines.edge) : 0
    var t = {}', '0 : 0
    var t = {}', 'fits without the edge line')
)

function Invoke-Tests([string] $work, [string] $js) {
    Write-Utf8 (Join-Path $work 'DockLayout.js') $js
    $path = Join-Path $work 'tst_dock.qml'
    Write-Utf8 $path $test
    $results = Join-Path $work 'results.txt'
    Remove-Item -LiteralPath $results -ErrorAction SilentlyContinue
    $p = Start-Process -FilePath $runner -NoNewWindow -Wait -PassThru `
        -ArgumentList @('-platform', 'offscreen', '-input', "`"$path`"", '-o', "`"$results,txt`"") `
        -RedirectStandardOutput (Join-Path $work 'run.out') -RedirectStandardError (Join-Path $work 'run.err')
    $out = @()
    if (Test-Path $results) { $out += @(Get-Content $results -Encoding UTF8) }
    return [pscustomobject]@{ Passed = ($p.ExitCode -eq 0 -and (Test-Path $results)); Lines = $out }
}

$work = Join-Path ([System.IO.Path]::GetTempPath()) ('dock_check_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
try {
    $env:PATH = "$QtBin;$env:PATH"
    $js = [System.IO.File]::ReadAllText((Join-Path $repo 'qml\app\DockLayout.js'), $utf8).Replace("`r`n", "`n")
    $r = Invoke-Tests $work $js
    $r.Lines | Where-Object { $_ -match '^(PASS|FAIL|Totals|   Actual|   Expected|   Loc)' } | ForEach-Object { Write-Host $_ }
    if (-not $r.Passed) {
        @(Get-Content (Join-Path $work 'run.err') -Encoding UTF8) | Select-Object -Last 30 | ForEach-Object { Write-Host $_ }
        Write-Host 'dock_check: failed'
        $code = 1
    } else {
        Write-Host 'dock_check: all passed'
        $code = 0
    }
    if ($Mutants -and $code -eq 0) {
        $caught = 0
        foreach ($m in $mutantList) {
            $from = $m[0].Replace("`r`n", "`n"); $to = $m[1].Replace("`r`n", "`n")
            $n = ([regex]::Matches($js, [regex]::Escape($from))).Count
            if ($n -ne 1) { Write-Host "mutant '$($m[2])': its text found $n times, not once"; $code = 1; continue }
            $mr = Invoke-Tests $work $js.Replace($from, $to)
            if ($mr.Passed) { Write-Host "mutant '$($m[2])': NOT caught"; $code = 1 }
            else { $caught++; Write-Host ("mutant '{0}': caught by {1}" -f $m[2], (($mr.Lines | Where-Object { $_ -match '^FAIL' } | ForEach-Object { ($_ -split '::')[-1] -replace '\(\).*$', '' }) -join ', ')) }
        }
        Write-Host "dock_check: $caught of $($mutantList.Count) mutants caught"
    }
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
exit $code
