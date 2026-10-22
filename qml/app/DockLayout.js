.pragma library

// Widgets docked to the window's edges (WorkspaceStore.docks, WidgetDocks.qml, docs/widget-docking.ru.md): a
// layout's four docks, and where they put the widgets and the workspace. No QML types, so dock_check tests it as
// it is.
//
// A dock is a strip along one edge. The side ones run the window's whole height, the top and bottom ones between
// them. Its widgets lie along it one after another, each given a share of its length in proportion to its own
// length, and each as thick as the dock. The dock is as thick as its thickest widget at the dock's scale; a widget
// that does not scale (a settings widget) counts at its own size. A collapsed dock takes no room, only its tab
// shows. The workspace keeps at least MIN_WORKSPACE_SHARE of the window across and down: a dock that would take
// more is refused (fits), and when the window shrinks under docks already there they are squeezed to fit.
//
// docks:  { left|right|top|bottom: { widgets: [id, ...], scale, collapsed } }
// sizes:  { id: { w, h, scalable } }, a widget's size at scale 1 as it reports it
// shown:  { id: true } for the widgets on screen; the others keep their place in the dock and take no room

var SIDES = ["left", "right", "top", "bottom"]
var SCALES = [0.75, 1.0, 1.25, 1.5, 2.0, 2.5]
var MIN_WORKSPACE_SHARE = 0.5

function isSide(side) { return SIDES.indexOf(side) >= 0 }

// Runs along the window's height.
function isVertical(side) { return side === "left" || side === "right" }

function snapScale(scale) {
    if (typeof scale !== "number" || !isFinite(scale))
        return 1.0
    var best = SCALES[0]
    for (var i = 1; i < SCALES.length; ++i)
        if (Math.abs(SCALES[i] - scale) < Math.abs(best - scale))
            best = SCALES[i]
    return best
}

function emptyDock() { return { widgets: [], scale: 1.0, collapsed: false } }

function emptyDocks() {
    return { left: emptyDock(), right: emptyDock(), top: emptyDock(), bottom: emptyDock() }
}

// A copy that can be trusted: every side a dock, an id in one dock at most, the scale one of SCALES. validIds,
// when given, drops the ids of widgets that are gone.
function normalize(raw, validIds) {
    var out = emptyDocks()
    if (!raw || typeof raw !== "object")
        return out
    var seen = {}
    for (var i = 0; i < SIDES.length; ++i) {
        var side = SIDES[i]
        var d = raw[side]
        if (!d || typeof d !== "object")
            continue
        var ids = Array.isArray(d.widgets) ? d.widgets : []
        for (var j = 0; j < ids.length; ++j) {
            var id = ids[j]
            if (typeof id !== "string" || id === "" || seen[id] === true)
                continue
            if (Array.isArray(validIds) && validIds.indexOf(id) < 0)
                continue
            seen[id] = true
            out[side].widgets.push(id)
        }
        out[side].scale = snapScale(d.scale)
        out[side].collapsed = d.collapsed === true
    }
    return out
}

function sideOf(docks, id) {
    var d = normalize(docks)
    for (var i = 0; i < SIDES.length; ++i)
        if (d[SIDES[i]].widgets.indexOf(id) >= 0)
            return SIDES[i]
    return ""
}

function withoutWidget(docks, id) {
    var d = normalize(docks)
    for (var i = 0; i < SIDES.length; ++i) {
        var list = d[SIDES[i]].widgets
        var at = list.indexOf(id)
        if (at >= 0)
            list.splice(at, 1)
    }
    return d
}

// The widget into the dock at a side, at index among the widgets already there (out of range: at the end), and
// out of any other. A collapsed dock opens: the widget was put there to be seen.
function withWidget(docks, id, side, index) {
    var d = withoutWidget(docks, id)
    if (!isSide(side) || typeof id !== "string" || id === "")
        return d
    var list = d[side].widgets
    var at = (typeof index === "number" && index >= 0 && index <= list.length) ? Math.floor(index) : list.length
    list.splice(at, 0, id)
    d[side].collapsed = false
    return d
}

function withScale(docks, side, scale) {
    var d = normalize(docks)
    if (isSide(side))
        d[side].scale = snapScale(scale)
    return d
}

function withCollapsed(docks, side, collapsed) {
    var d = normalize(docks)
    if (isSide(side))
        d[side].collapsed = collapsed === true
    return d
}

// Each dock's shown widgets with their lengths along it, and its thickness, at the dock's scale, before any
// squeeze; a collapsed dock is 0 thick.
function _measure(d, sizes, shown) {
    var m = {}
    for (var i = 0; i < SIDES.length; ++i) {
        var side = SIDES[i]
        var dock = d[side]
        var vertical = isVertical(side)
        var ids = [], lens = [], scales = [], thick = 0
        for (var j = 0; j < dock.widgets.length; ++j) {
            var id = dock.widgets[j]
            var s = sizes ? sizes[id] : null
            if (!s || !(shown && shown[id] === true))
                continue
            var across = vertical ? s.w : s.h
            var along = vertical ? s.h : s.w
            if (!(across > 0) || !(along > 0))
                continue
            var k = s.scalable === false ? 1.0 : dock.scale
            ids.push(id)
            lens.push(along * k)
            scales.push(k)
            thick = Math.max(thick, across * k)
        }
        m[side] = { ids: ids, lens: lens, scales: scales,
                    thickness: (ids.length > 0 && !dock.collapsed) ? Math.round(thick) : 0 }
    }
    return m
}

function _pairFits(a, b, size) { return a + b <= size * (1 - MIN_WORKSPACE_SHARE) }

// Each dock's thickness with the line along its inner edge (see compute's lines).
function _full(m, lines) {
    var edgeLine = (lines && lines.edge > 0) ? Math.round(lines.edge) : 0
    var t = {}
    for (var i = 0; i < SIDES.length; ++i)
        t[SIDES[i]] = m[SIDES[i]].thickness > 0 ? m[SIDES[i]].thickness + edgeLine : 0
    return t
}

// Whether the docks leave the workspace its share of a window W by H without a squeeze.
function fits(docks, sizes, shown, W, H, lines) {
    var t = _full(_measure(normalize(docks), sizes, shown), lines)
    return _pairFits(t.left, t.right, W) && _pairFits(t.top, t.bottom, H)
}

// The scales the dock at a side can take with the other docks as they are: [{ scale, thickness }], thinnest first,
// the thickness with its edge line. The thickness handle (WidgetDocks.qml) snaps to these.
function scaleSteps(docks, sizes, shown, W, H, side, lines) {
    var out = []
    if (!isSide(side))
        return out
    for (var i = 0; i < SCALES.length; ++i) {
        var d = withScale(docks, side, SCALES[i])
        d[side].collapsed = false
        if (!fits(d, sizes, shown, W, H, lines))
            continue
        var t = _full(_measure(d, sizes, shown), lines)[side]
        if (out.length === 0 || out[out.length - 1].thickness !== t)
            out.push({ scale: SCALES[i], thickness: t })
    }
    return out
}

function _squeeze(a, b, size) {
    var room = Math.max(0, size * (1 - MIN_WORKSPACE_SHARE))
    return (a + b > room && a + b > 0) ? room / (a + b) : 1.0
}

// Where everything goes in a window W by H. lines: { edge, gap } in pixels, the line along a dock's inner edge (the
// dock is that much thicker than its widgets) and the gap between two widgets in it; none when not given.
//   insets:  { left, right, top, bottom }, what the workspace gives up at each edge
//   docks:   { side: { ids, x, y, w, h, thickness, edge, from, to, collapsed } } -- edge: the dock's inner edge
//            (x for a side dock, y for the others; at the window's edge when collapsed), from..to: its span along
//   widgets: { id: { side, x, y, w, h, scale, collapsed } } -- scale: the one to draw at; a collapsed dock's
//            widgets are listed, so they hide rather than float
//   squeezed: whether the window was too small for the docks as they are
function compute(docks, sizes, shown, W, H, lines) {
    var d = normalize(docks)
    var m = _measure(d, sizes, shown)
    var w = Math.max(0, W || 0), h = Math.max(0, H || 0)
    var edgeLine = (lines && lines.edge > 0) ? Math.round(lines.edge) : 0
    var gapLine = (lines && lines.gap > 0) ? Math.round(lines.gap) : 0
    var full = _full(m, lines)
    var fx = _squeeze(full.left, full.right, w)
    var fy = _squeeze(full.top, full.bottom, h)
    var t = {
        left: Math.floor(full.left * fx), right: Math.floor(full.right * fx),
        top: Math.floor(full.top * fy), bottom: Math.floor(full.bottom * fy)
    }
    var out = { insets: t, docks: {}, widgets: {}, squeezed: fx < 1 || fy < 1 }
    for (var i = 0; i < SIDES.length; ++i) {
        var side = SIDES[i]
        var vertical = isVertical(side)
        var f = vertical ? fx : fy
        var th = t[side]
        var inner = Math.max(0, th - edgeLine)   // the widgets' thickness
        var from = vertical ? 0 : t.left
        var to = vertical ? h : Math.max(from, w - t.right)
        var x, y, dw, dh, edge, across
        if (side === "left")       { x = 0;      y = 0;      dw = th;        dh = h;  edge = th;     across = 0 }
        else if (side === "right") { x = w - th; y = 0;      dw = th;        dh = h;  edge = w - th; across = edge + th - inner }
        else if (side === "top")   { x = from;   y = 0;      dw = to - from; dh = th; edge = th;     across = 0 }
        else                       { x = from;   y = h - th; dw = to - from; dh = th; edge = h - th; across = edge + th - inner }
        var mm = m[side]
        out.docks[side] = { ids: mm.ids.slice(0), x: x, y: y, w: dw, h: dh, thickness: th, edge: edge,
                            from: from, to: to, collapsed: d[side].collapsed && mm.ids.length > 0 }
        var n = mm.ids.length
        var span = Math.max(0, to - from - gapLine * Math.max(0, n - 1))
        var total = 0
        for (var j = 0; j < n; ++j)
            total += mm.lens[j]
        var acc = 0
        for (var k = 0; k < n; ++k) {
            var a = from + k * gapLine + Math.round(span * acc / total)
            acc += mm.lens[k]
            var b = from + k * gapLine + Math.round(span * acc / total)
            out.widgets[mm.ids[k]] = vertical
                ? { side: side, x: across, y: a, w: inner, h: b - a, scale: mm.scales[k] * f, collapsed: d[side].collapsed }
                : { side: side, x: a, y: across, w: b - a, h: inner, scale: mm.scales[k] * f, collapsed: d[side].collapsed }
        }
    }
    return out
}
