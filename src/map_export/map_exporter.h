#pragma once

// "Map for sharing" (docs/map-export.md): saves the survey's side-scan mosaic and depth map as a
// KMZ for Google Earth and GeoTIFFs for GIS, with the author, the licence the user chose, the "not
// for navigation" notice and how the data was made written into every file. Exposed to QML as
// `mapExport`; the settings are kept between runs.

#include <atomic>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QThread>
#include <QVariantList>

class Dataset;
class DataProcessor;
class GraphicsScene3dView;

class MapExporter : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString title READ title WRITE setTitle NOTIFY settingsChanged)
    Q_PROPERTY(QString authorName READ authorName WRITE setAuthorName NOTIFY settingsChanged)
    Q_PROPERTY(QString authorContact READ authorContact WRITE setAuthorContact NOTIFY settingsChanged)
    Q_PROPERTY(QString licenseId READ licenseId WRITE setLicenseId NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList licenseOptions READ licenseOptions CONSTANT)
    Q_PROPERTY(int detailZoom READ detailZoom WRITE setDetailZoom NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList detailOptions READ detailOptions CONSTANT)
    Q_PROPERTY(double draftM READ draftM WRITE setDraftM NOTIFY settingsChanged)
    Q_PROPERTY(double fillGapsM READ fillGapsM WRITE setFillGapsM NOTIFY settingsChanged)
    Q_PROPERTY(double isobathStepM READ isobathStepM WRITE setIsobathStepM NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList isobathStepOptions READ isobathStepOptions CONSTANT)
    Q_PROPERTY(bool writeKmap READ writeKmap WRITE setWriteKmap NOTIFY settingsChanged)
    Q_PROPERTY(bool writeKmz READ writeKmz WRITE setWriteKmz NOTIFY settingsChanged)
    Q_PROPERTY(bool writeGeoTiff READ writeGeoTiff WRITE setWriteGeoTiff NOTIFY settingsChanged)
    Q_PROPERTY(bool includeMosaic READ includeMosaic WRITE setIncludeMosaic NOTIFY settingsChanged)
    Q_PROPERTY(bool includeDepth READ includeDepth WRITE setIncludeDepth NOTIFY settingsChanged)
    Q_PROPERTY(bool hideTrackAndTime READ hideTrackAndTime WRITE setHideTrackAndTime NOTIFY settingsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString stage READ stage NOTIFY progressChanged)
    Q_PROPERTY(bool resultOk READ resultOk NOTIFY resultChanged)
    Q_PROPERTY(QString resultText READ resultText NOTIFY resultChanged)
    Q_PROPERTY(QStringList resultFiles READ resultFiles NOTIFY resultChanged)

public:
    explicit MapExporter(QObject* parent = nullptr);
    ~MapExporter() override;

    void setSources(Dataset* dataset, DataProcessor* dataProcessor, GraphicsScene3dView* scene);

    /// Starts an export into `folder` (created when missing). Does nothing while one runs.
    Q_INVOKABLE void start(const QString& folder);
    Q_INVOKABLE void cancel();
    /// Cancels a running export and waits for its thread (before the dataset is cleared, at exit).
    void stopAndWait();

    QString title() const { return title_; }
    void setTitle(const QString& v);
    QString authorName() const { return authorName_; }
    void setAuthorName(const QString& v);
    QString authorContact() const { return authorContact_; }
    void setAuthorContact(const QString& v);
    QString licenseId() const { return licenseId_; }
    void setLicenseId(const QString& v);
    QVariantList licenseOptions() const;
    int detailZoom() const { return detailZoom_; }
    void setDetailZoom(int v);
    QVariantList detailOptions() const;
    double draftM() const { return draftM_; }
    void setDraftM(double v);
    /// The longest triangle edge the depth surface spans, m: wider gaps between soundings stay empty.
    /// The export's own (the view's edge limit is for the screen; a shared map wants the lines joined).
    double fillGapsM() const { return fillGapsM_; }
    void setFillGapsM(double v);
    /// Metres between isobaths and between depth colours; 0 = chosen from the depths (autoIsobathStep).
    double isobathStepM() const { return isobathStepM_; }
    void setIsobathStepM(double v);
    QVariantList isobathStepOptions() const;
    bool writeKmap() const { return writeKmap_; }
    void setWriteKmap(bool v);
    bool writeKmz() const { return writeKmz_; }
    void setWriteKmz(bool v);
    bool writeGeoTiff() const { return writeGeoTiff_; }
    void setWriteGeoTiff(bool v);
    bool includeMosaic() const { return includeMosaic_; }
    void setIncludeMosaic(bool v);
    bool includeDepth() const { return includeDepth_; }
    void setIncludeDepth(bool v);
    bool hideTrackAndTime() const { return hideTrackAndTime_; }
    void setHideTrackAndTime(bool v);

    bool busy() const { return busy_; }
    double progress() const { return progress_; }
    QString stage() const { return stage_; }
    bool resultOk() const { return resultOk_; }
    QString resultText() const { return resultText_; }
    QStringList resultFiles() const { return resultFiles_; }

signals:
    void settingsChanged();
    void busyChanged();
    void progressChanged();
    void resultChanged();

private:
    void save() const;
    void setProgress(double p, const QString& stage);
    void finish(bool ok, const QString& text, const QStringList& files);

    QPointer<Dataset> dataset_;
    QPointer<DataProcessor> dataProcessor_;
    QPointer<GraphicsScene3dView> scene_;

    QString title_;
    QString authorName_;
    QString authorContact_;
    QString licenseId_;
    int detailZoom_ = 5;
    double draftM_ = 0.0;
    double fillGapsM_ = 50.0;
    double isobathStepM_ = 0.0;
    bool writeKmap_ = true;     // the KOGGER map, for KoggerApp: the main file
    bool writeKmz_ = false;    // for other programs, off unless asked
    bool writeGeoTiff_ = false;
    bool includeMosaic_ = true;
    bool includeDepth_ = true;
    bool hideTrackAndTime_ = true;

    bool busy_ = false;
    double progress_ = 0.0;
    QString stage_;
    bool resultOk_ = false;
    QString resultText_;
    QStringList resultFiles_;

    QThread* thread_ = nullptr;
    std::atomic_bool cancel_{ false };
};
