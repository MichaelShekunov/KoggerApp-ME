.pragma library

function normalizedSettingsSide(value) {
    return value === "right" ? "right" : "left"
}

function normalizedPaneMode(value) {
    if (value === "3D")
        return "3D"
    if (value === "Video")
        return "Video"
    if (value === SCENE_MODE_RELIEF)
        return SCENE_MODE_RELIEF
    return "2D"
}

// "2/3D": relief inferred from side-scan brightness, kept out of the mosaic on
// purpose - an estimate and a survey must not share a surface.
var SCENE_MODE_RELIEF = "2/3D"

// Both of these drive the one Scene3DView the application owns, so they compete
// for the same slot and only one may exist at a time. Anything asking "is this
// pane a 3D pane" wants this, not a comparison against "3D".
function occupiesScene(mode) {
    return mode === "3D" || mode === SCENE_MODE_RELIEF
}

function normalizedPaneRotate3D(value) {
    return value === true
}

function normalizedPaneRotate2D(value) {
    return value === true
}

function paletteColor(index) {
    var colors = [
        "#60A5FA",
        "#34D399",
        "#FBBF24",
        "#A78BFA",
        "#F472B6",
        "#FB923C"
    ]
    return colors[index % colors.length]
}

function paneWithMode(paneObj, mode) {
    return {
        paneId: paneObj.paneId,
        title: paneObj.title,
        color: paneObj.color,
        mode: normalizedPaneMode(mode),
        rotate3DLogoOnSphere: normalizedPaneRotate3D(paneObj.rotate3DLogoOnSphere),
        rotate2DLogoHorizontal: normalizedPaneRotate2D(paneObj.rotate2DLogoHorizontal),
        contentId: paneObj.contentId || ""
    }
}

function makePane(paneNumber, mode) {
    var resolvedMode = mode === undefined ? "2D" : mode
    return {
        paneId: paneNumber,
        title: "Pane " + paneNumber,
        color: paletteColor(paneNumber - 1),
        mode: normalizedPaneMode(resolvedMode),
        rotate3DLogoOnSphere: false,
        rotate2DLogoHorizontal: false,
        contentId: ""
    }
}

function layoutHasAnyModeField(node) {
    if (!node)
        return false
    if (node.type === "leaf")
        return node.pane && node.pane.mode !== undefined
    return layoutHasAnyModeField(node.first) || layoutHasAnyModeField(node.second)
}

function isValidLayoutNode(node) {
    if (!node || typeof node !== "object")
        return false

    if (node.type === "leaf") {
        return typeof node.leafId === "number"
                && node.leafId > 0
                && node.pane
                && typeof node.pane.paneId === "number"
                && typeof node.pane.title === "string"
                && typeof node.pane.color === "string"
                && (node.pane.mode === undefined || typeof node.pane.mode === "string")
                && (node.pane.rotate3DLogoOnSphere === undefined || typeof node.pane.rotate3DLogoOnSphere === "boolean")
                && (node.pane.rotate2DLogoHorizontal === undefined || typeof node.pane.rotate2DLogoHorizontal === "boolean")
    }

    if (node.type === "split") {
        return typeof node.splitId === "number"
                && node.splitId > 0
                && (node.orientation === "vertical" || node.orientation === "horizontal")
                && typeof node.ratio === "number"
                && isValidLayoutNode(node.first)
                && isValidLayoutNode(node.second)
    }

    return false
}
