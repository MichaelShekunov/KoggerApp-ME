.pragma library

function severity(dev) {
    if (!dev || !dev.recorderStatusValid)
        return "idle"
    var st = dev.recorderRecordingState
    if (st === 3 || dev.recorderCriticalFlags)
        return "crit"
    if (st === 4)
        return "crit"
    if (st === 2) {
        var active = dev.recorderStatusFlags || 0
        var silent = (dev.recorderDegradedFlags || 0) & 0x7
        if (dev.recorderSecondsSinceLastWrite > 10 || (silent && !active))
            return "warn"
        if (silent & ~active)
            return "warn"
        return "good"
    }
    return "idle"
}

function sourceState(recorder, port) {
    if (!recorder || !recorder.recorderStatusValid || port < 1 || port > 3)
        return ""
    var bit = 1 << (port - 1)
    if ((recorder.recorderStatusFlags   || 0) & bit) return "good"
    if ((recorder.recorderDegradedFlags || 0) & bit) return "crit"
    return ""
}

function linkState(dev) {
    if (!dev) return ""
    if (dev.linkConnected)    return dev.linkReceivesData ? "ok" : "warn"
    if (dev.linkNotAvailable) return "down"
    return ""
}

function pillState(dev, master, port) {
    // Silent on a live link (DevDriver dataStale): the link may still carry other devices' frames.
    if (dev && dev.linkConnected && dev.dataStale)
        return "warn"
    if (dev && dev.isRecorder && dev.recorderStatusValid) {
        var s = severity(dev)
        return s === "good" ? "ok" : s === "warn" ? "warn" : s === "crit" ? "down" : ""
    }
    // A recorder that is not recording (no storage, a failed recording, recording off: severity "crit") reports its
    // ports as lost; that is its own trouble, shown on it. A device behind it is shown by its own frames then
    // (owner 2026-09-30: the logger without its flash drive turned the working sonar red).
    var src = (master && severity(master) === "crit") ? "" : sourceState(master, port)
    if (src === "good") return "ok"
    if (src === "crit") return "down"
    return linkState(dev)
}

function fmtSize(b) {
    if (!b || b <= 0) return "0 B"
    if (b < 1024) return b + " B"
    if (b < 1048576) return (b / 1024).toFixed(1) + " KB"
    if (b < 1073741824) return (b / 1048576).toFixed(1) + " MB"
    return (b / 1073741824).toFixed(2) + " GB"
}

function elapsed(s) {
    if (!s || s <= 0) return "0s"
    if (s < 60) return s + "s"
    if (s < 3600) return Math.floor(s / 60) + "m"
    return Math.floor(s / 3600) + "h"
}

function fmtSizeShort(b) {
    if (!b || b <= 0) return "0"
    if (b < 1048576)    return Math.round(b / 1024) + "K"
    if (b < 1073741824) return Math.round(b / 1048576) + "M"
    if (b < 1099511627776) {
        var g = b / 1073741824
        return (g < 9.95 ? g.toFixed(1) : Math.round(g)) + "G"
    }
    var t = b / 1099511627776
    return (t < 9.95 ? t.toFixed(1) : Math.round(t)) + "T"
}
