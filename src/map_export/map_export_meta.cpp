#include "map_export_meta.h"

#include <QJsonArray>

namespace mapexport {

QList<LicenseInfo> licenses()
{
    return {
        { QStringLiteral("all-rights-reserved"), QStringLiteral("All rights reserved"), QString() },
        { QStringLiteral("CC-BY-4.0"), QStringLiteral("Creative Commons Attribution 4.0 International (CC BY 4.0)"),
          QStringLiteral("https://creativecommons.org/licenses/by/4.0/") },
        { QStringLiteral("CC0-1.0"), QStringLiteral("CC0 1.0 Universal (public domain dedication)"),
          QStringLiteral("https://creativecommons.org/publicdomain/zero/1.0/") },
    };
}

LicenseInfo licenseById(const QString& id)
{
    const auto all = licenses();
    for (const auto& l : all) {
        if (l.id == id) {
            return l;
        }
    }
    return all.first();
}

QString disclaimerEn(double draftM)
{
    const QString from = draftM != 0.0
        ? QStringLiteral("from the waterline (the transducer depth plus a draft of %1 m)").arg(draftM, 0, 'f', 2)
        : QStringLiteral("from the transducer");
    return QStringLiteral(
        "NOT FOR NAVIGATION. A survey made with a KOGGER sonar and KoggerApp. Depths are measured %1, "
        "are not reduced to a chart datum or to the water level, and are not corrected for sound speed; "
        "positions are as GNSS gave them. The data may contain errors and gaps. Use it at your own risk.").arg(from);
}

QString verticalReference(const MapMetadata& m)
{
    return m.draftM != 0.0 ? QStringLiteral("Waterline") : QStringLiteral("Transducer");
}

QString copyrightLine(const MapMetadata& m)
{
    const LicenseInfo l = licenseById(m.licenseId);
    const QString who = m.authorName.trimmed();
    QString s;
    if (l.id == QLatin1String("CC0-1.0")) {
        s = who.isEmpty() ? QStringLiteral("Dedicated to the public domain")
                          : QStringLiteral("Dedicated to the public domain by %1").arg(who);
    }
    else {
        s = who.isEmpty() ? QStringLiteral("© the author") : QStringLiteral("© %1").arg(who);
    }
    s += QStringLiteral(". ") + l.name;
    if (!l.url.isEmpty()) {
        s += QStringLiteral(", ") + l.url;
    }
    return s;
}

QJsonObject toJson(const MapMetadata& m)
{
    const LicenseInfo l = licenseById(m.licenseId);
    QJsonObject provider;
    if (!m.authorName.isEmpty()) {
        provider[QStringLiteral("name")] = m.authorName;
    }
    if (!m.authorContact.isEmpty()) {
        provider[QStringLiteral("contact")] = m.authorContact;
    }

    QJsonObject survey;
    if (!m.surveyFrom.isEmpty()) {
        survey[QStringLiteral("timeFrom")] = m.surveyFrom;
    }
    if (!m.surveyTo.isEmpty()) {
        survey[QStringLiteral("timeTo")] = m.surveyTo;
    }
    survey[QStringLiteral("trackIncluded")] = m.trackIncluded;
    survey[QStringLiteral("trackAndTimeHidden")] = m.hideTrackAndTime;

    QJsonObject products;
    if (m.hasMosaic) {
        products[QStringLiteral("sidescanMosaic")] = QJsonObject{
            { QStringLiteral("resolution_m"), m.mosaicResolutionM },
            { QStringLiteral("source"), m.mosaicSource },
            { QStringLiteral("paletteTheme"), m.mosaicTheme },
            { QStringLiteral("levels"), QJsonArray{ m.mosaicLow, m.mosaicHigh } },
            { QStringLiteral("values"), QStringLiteral("8-bit echo intensity index, 0 = no data") },
        };
    }
    if (m.hasDepth) {
        products[QStringLiteral("bathymetry")] = QJsonObject{
            { QStringLiteral("cell_m"), m.depthCellM },
            { QStringLiteral("interpolation"), QStringLiteral("Delaunay triangulation of the bottom track, linear") },
            { QStringLiteral("edgeLimit_m"), m.edgeLimitM },
            { QStringLiteral("soundings"), m.soundings },
            { QStringLiteral("minDepth_m"), m.minDepthM },
            { QStringLiteral("maxDepth_m"), m.maxDepthM },
            { QStringLiteral("isobathStep_m"), m.isobathStepM },
            { QStringLiteral("paletteTheme"), m.depthTheme },
            { QStringLiteral("depthPositive"), QStringLiteral("down") },
            { QStringLiteral("noData"), -9999 },
        };
    }

    QJsonObject o;
    o[QStringLiteral("format")] = QStringLiteral("KoggerApp map export");
    o[QStringLiteral("formatVersion")] = 1;
    o[QStringLiteral("title")] = m.title;
    if (!provider.isEmpty()) {
        o[QStringLiteral("provider")] = provider;
    }
    o[QStringLiteral("license")] = QJsonObject{
        { QStringLiteral("id"), l.id }, { QStringLiteral("name"), l.name }, { QStringLiteral("url"), l.url },
        { QStringLiteral("notice"), copyrightLine(m) },
    };
    o[QStringLiteral("disclaimer")] = disclaimerEn(m.draftM);
    if (!m.disclaimerLocal.isEmpty()) {
        o[QStringLiteral("disclaimerLocal")] = m.disclaimerLocal;
    }
    o[QStringLiteral("logger")] = m.software;
    o[QStringLiteral("coordinateSystem")] = QStringLiteral("EPSG:4326");
    o[QStringLiteral("verticalReference")] = verticalReference(m);
    o[QStringLiteral("sensor")] = QJsonObject{
        { QStringLiteral("type"), QStringLiteral("Sounder") },
        { QStringLiteral("make"), QStringLiteral("KOGGER") },
        { QStringLiteral("draft_m"), m.draftM },
    };
    o[QStringLiteral("processing")] = QJsonObject{
        { QStringLiteral("draftApplied"), m.draftM != 0.0 },
        { QStringLiteral("verticalReduction"), false },
        { QStringLiteral("soundSpeedCorrection"), false },
    };
    o[QStringLiteral("survey")] = survey;
    o[QStringLiteral("created")] = m.created;
    o[QStringLiteral("bounds")] = QJsonObject{
        { QStringLiteral("west"), m.west }, { QStringLiteral("south"), m.south },
        { QStringLiteral("east"), m.east }, { QStringLiteral("north"), m.north },
    };
    o[QStringLiteral("products")] = products;
    if (!m.files.isEmpty()) {
        o[QStringLiteral("files")] = QJsonArray::fromStringList(m.files);
    }
    return o;
}

QList<QPair<QString, QString>> toGdalItems(const MapMetadata& m)
{
    const LicenseInfo l = licenseById(m.licenseId);
    QList<QPair<QString, QString>> it;
    it << qMakePair(QStringLiteral("TITLE"), m.title);
    if (!m.authorName.isEmpty()) {
        it << qMakePair(QStringLiteral("AUTHOR"), m.authorName);
    }
    if (!m.authorContact.isEmpty()) {
        it << qMakePair(QStringLiteral("CONTACT"), m.authorContact);
    }
    it << qMakePair(QStringLiteral("LICENSE"), l.name)
       << qMakePair(QStringLiteral("LICENSE_ID"), l.id);
    if (!l.url.isEmpty()) {
        it << qMakePair(QStringLiteral("LICENSE_URL"), l.url);
    }
    it << qMakePair(QStringLiteral("COPYRIGHT"), copyrightLine(m))
       << qMakePair(QStringLiteral("DISCLAIMER"), disclaimerEn(m.draftM));
    if (!m.disclaimerLocal.isEmpty()) {
        it << qMakePair(QStringLiteral("DISCLAIMER_LOCAL"), m.disclaimerLocal);
    }
    it << qMakePair(QStringLiteral("LOGGER"), m.software)
       << qMakePair(QStringLiteral("VERTICAL_REFERENCE"), verticalReference(m))
       << qMakePair(QStringLiteral("DRAFT_M"), QString::number(m.draftM, 'f', 2))
       << qMakePair(QStringLiteral("CREATED"), m.created);
    if (!m.surveyFrom.isEmpty()) {
        it << qMakePair(QStringLiteral("SURVEY_FROM"), m.surveyFrom);
    }
    if (!m.surveyTo.isEmpty()) {
        it << qMakePair(QStringLiteral("SURVEY_TO"), m.surveyTo);
    }
    return it;
}

namespace {
QString esc(const QString& s) { return s.toHtmlEscaped(); }
}

QString toKmlDescription(const MapMetadata& m)
{
    const LicenseInfo l = licenseById(m.licenseId);
    QString h;
    h += QStringLiteral("<p><b>%1</b></p>").arg(esc(disclaimerEn(m.draftM)));
    if (!m.disclaimerLocal.isEmpty()) {
        h += QStringLiteral("<p><b>%1</b></p>").arg(esc(m.disclaimerLocal));
    }
    h += QStringLiteral("<p>%1").arg(esc(copyrightLine(m)));
    if (!l.url.isEmpty()) {
        h += QStringLiteral(" &mdash; <a href=\"%1\">%1</a>").arg(esc(l.url));
    }
    h += QStringLiteral("</p><table>");
    auto row = [&](const QString& k, const QString& v) {
        if (!v.isEmpty()) {
            h += QStringLiteral("<tr><td>%1</td><td>%2</td></tr>").arg(esc(k), esc(v));
        }
    };
    row(QStringLiteral("Author"), m.authorName);
    row(QStringLiteral("Contact"), m.authorContact);
    row(QStringLiteral("Survey"), m.surveyFrom == m.surveyTo ? m.surveyFrom : m.surveyFrom + QStringLiteral(" – ") + m.surveyTo);
    row(QStringLiteral("Depth reference"), verticalReference(m));
    if (m.hasDepth) {
        row(QStringLiteral("Depths"), QStringLiteral("%1 – %2 m, grid %3 m, isobaths every %4 m")
            .arg(m.minDepthM, 0, 'f', 1).arg(m.maxDepthM, 0, 'f', 1).arg(m.depthCellM, 0, 'f', 2).arg(m.isobathStepM, 0, 'g', 3));
    }
    if (m.hasMosaic) {
        row(QStringLiteral("Side-scan mosaic"), QStringLiteral("%1 cm per pixel").arg(m.mosaicResolutionM * 100.0, 0, 'f', 0));
    }
    row(QStringLiteral("Made with"), m.software);
    row(QStringLiteral("Exported"), m.created);
    h += QStringLiteral("</table>");
    return h;
}

QString toReadme(const MapMetadata& m)
{
    const LicenseInfo l = licenseById(m.licenseId);
    QString t;
    t += m.title + QStringLiteral("\n\n");
    t += disclaimerEn(m.draftM) + QStringLiteral("\n");
    if (!m.disclaimerLocal.isEmpty()) {
        t += m.disclaimerLocal + QStringLiteral("\n");
    }
    t += QStringLiteral("\n") + copyrightLine(m) + QStringLiteral("\n");
    if (!m.authorContact.isEmpty()) {
        t += QStringLiteral("Contact: ") + m.authorContact + QStringLiteral("\n");
    }
    if (!l.url.isEmpty()) {
        t += QStringLiteral("Licence text: ") + l.url + QStringLiteral("\n");
    }
    t += QStringLiteral("\nOpen doc.kml (this .kmz) in Google Earth. metadata.json describes how the map was made.\n");
    t += QStringLiteral("Made with ") + m.software + QStringLiteral(".\n");
    return t;
}

} // namespace mapexport
