#pragma once

// What a shared map says about itself (docs/map-export.md): who made it, under which licence, the
// "not for navigation" notice, and how the data was made. The field names follow IHO B-12
// (Guidance to Crowdsourced Bathymetry, table 1) where it has one; the layout is this app's own.

#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

namespace mapexport {

/// The licences a user can put on a map. The ids are SPDX ones, "all-rights-reserved" aside.
struct LicenseInfo {
    QString id;
    QString name;
    QString url;
};
QList<LicenseInfo> licenses();
LicenseInfo licenseById(const QString& id);   // all-rights-reserved when unknown

struct MapMetadata {
    QString title;
    QString authorName;        // may be empty
    QString authorContact;     // may be empty
    QString licenseId = QStringLiteral("all-rights-reserved");
    QString software;          // e.g. "KoggerApp ME 1.0.3 (c4e1d3f4)"
    QString created;           // export time, ISO 8601 UTC
    bool hideTrackAndTime = true;
    QString surveyFrom;        // ISO 8601 UTC; hideTrackAndTime: the month only, "2026-10"
    QString surveyTo;
    bool trackIncluded = false;
    double draftM = 0.0;
    double west = 0.0, south = 0.0, east = 0.0, north = 0.0;

    bool hasMosaic = false;
    double mosaicResolutionM = 0.0;
    QString mosaicSource;      // Amplitude / SideScan / Tgc
    int mosaicTheme = 0;
    double mosaicLow = 0.0, mosaicHigh = 0.0;

    bool hasDepth = false;
    double depthCellM = 0.0;
    double edgeLimitM = 0.0;
    double extrapolationM = 0.0;   // the width of the disk each sounding's depth is extrapolated over
    double isobathStepM = 0.0;
    int depthTheme = 0;        // IsobathUtils::colorPalette theme the colours were made with
    int soundings = 0;
    double minDepthM = 0.0, maxDepthM = 0.0;

    QStringList files;         // the files written together, names only
    QString disclaimerLocal;   // the notice in the user's language, besides the English one
};

/// "NOT FOR NAVIGATION. ..." in English, with the draft when one was added.
QString disclaimerEn(double draftM);
/// "© author. <licence>" -- the line a Copyright tag or a caption carries.
QString copyrightLine(const MapMetadata& m);
QString verticalReference(const MapMetadata& m);   // "Transducer" or "Waterline"

QJsonObject toJson(const MapMetadata& m);
/// Flat items for GDAL_METADATA (QGIS shows them under the layer's properties).
QList<QPair<QString, QString>> toGdalItems(const MapMetadata& m);
/// The KML document's description (HTML, shown by Google Earth when the map is clicked).
QString toKmlDescription(const MapMetadata& m);
/// The README.txt that goes into the KMZ next to the map.
QString toReadme(const MapMetadata& m);

} // namespace mapexport
