import QtQuick 2.15

// The QML side of the supply voltage, off the application: both catalogs loaded from the source tree as
// plain components (the app links its QML modules in statically), the widget field and the device switch
// exercised against stand-in objects. Run (prints a line a check, exit 0 = pass, 1 = fail):
//   set QT_FORCE_STDERR_LOGGING=1
//   D:\Qt\6.8.3\llvm-mingw_64\bin\qml.exe -platform offscreen tools\voltage_check\catalog_check.qml
Item {
    Component.onCompleted: {
        var bad = 0
        function check(what, good, detail) {
            console.log((good ? "ok   " : "FAIL ") + what + (detail !== undefined ? "  [" + detail + "]" : ""))
            if (!good) bad++
        }
        var fc = Qt.createComponent(Qt.resolvedUrl("../../qml/kqml_types/DataFieldCatalog.qml"))
        check("DataFieldCatalog loads", fc.status === Component.Ready, fc.errorString())
        var f = fc.status === Component.Ready ? fc.createObject(null) : null
        if (f) {
            var entry = null
            for (var i = 0; i < f.fields.length; ++i) if (f.fields[i].key === "sonarVoltage") entry = f.fields[i]
            check("sonarVoltage is in the palette", entry !== null)
            check("its unit is V", entry && entry.unit === "V", entry ? entry.unit : "")
            var off = { isLastSupplyVoltageValid: false, lastSupplyVoltage: NaN }
            var on  = { isLastSupplyVoltageValid: true,  lastSupplyVoltage: 12.5 }
            check("no reading: not valid, a dash", !f.isValid("sonarVoltage", off) && f.formatValue("sonarVoltage", off) === "\u2014")
            check("a reading: valid, two decimals", f.isValid("sonarVoltage", on) && f.formatValue("sonarVoltage", on) === "12.50 V",
                  f.formatValue("sonarVoltage", on))
            check("a sample for the palette", f.sampleValue("sonarVoltage", null) === "12.10 V", f.sampleValue("sonarVoltage", null))
            check("temperature unchanged", f.formatValue("temp", { isLastTempValid: true, lastTemp: 28.1 }) === "28.1 \u00B0C")
        }
        var pc = Qt.createComponent(Qt.resolvedUrl("../../qml/kqml_types/DeviceParamCatalog.qml"))
        check("DeviceParamCatalog loads", pc.status === Component.Ready, pc.errorString())
        var p = pc.status === Component.Ready ? pc.createObject(null) : null
        if (p) {
            var m = p.meta("dataset.voltage")
            check("dataset.voltage is a switch on datasetVoltage, mask 1",
                  m && m.kind === "switch" && m.prop === "datasetVoltage" && m.mask === 1,
                  m ? (m.kind + " " + m.prop + " " + m.mask) : "missing")
            var dev = { isDatasetSupport: true, datasetVoltage: 0 }
            check("shown where the dataset is", p.supported(dev, "dataset.voltage"))
            check("reads off", p.read(dev, "dataset.voltage") === false, String(p.read(dev, "dataset.voltage")))
            p.write(dev, "dataset.voltage", true)
            check("switching on writes 1 (channel 1)", dev.datasetVoltage === 1, String(dev.datasetVoltage))
            check("reads on", p.read(dev, "dataset.voltage") === true, String(p.read(dev, "dataset.voltage")))
            p.write(dev, "dataset.voltage", false)
            check("switching off writes 0", dev.datasetVoltage === 0, String(dev.datasetVoltage))
        }
        console.log(bad ? "FAILED" : "PASS")
        Qt.exit(bad ? 1 : 0)
    }
}
