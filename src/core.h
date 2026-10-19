#pragma once

#include <QObject>
#include <QUrl>
#include <QQmlApplicationEngine>
#include <QStandardItemModel>
#include <QQmlContext>
#include <QThread>
#include <QVariantList>
#include <QHash>
#ifdef FLASHER
#include "flasher/deviceflasher.h"
#endif
#include "data_processor.h"
#include "qPlot2D.h"
#include "logger.h"
#include "console.h"
#include "converter_xtf.h"
#include "scene3d_view.h"
#include "boat_track_control_menu_controller.h"
#include "navigation_arrow_control_menu_controller.h"
#include "bottom_track_control_menu_controller.h"
#include "isobaths_view_control_menu_controller.h"
#include "mosaic_view_control_menu_controller.h"
#include "image_view_control_menu_controller.h"
#include "map_view_control_menu_controller.h"
#include "point_group_control_menu_controller.h"
#include "polygon_group_control_menu_controller.h"
#include "mpc_filter_control_menu_controller.h"
#include "npd_filter_control_menu_controller.h"
#include "scene3d_toolbar_controller.h"
#include "scene3d_control_menu_controller.h"
#include "hotkeys_controller.h"
#include "device_manager_wrapper.h"
#include <QElapsedTimer>
#include <QTimer>

#include "link_manager_wrapper.h"
#include "device_topology_model.h"
#include "tile_manager.h"
#include "internet_manager.h"
#include "data_horizon.h"
#include "echogram_editor.h"
#include "mosaic_index_provider.h"
#include "ui_keepalive.h"
#include "usb_tap.h"

class MapExporter;
class MapLibrary;
class MapLayer;

class Core : public QObject
{
    Q_OBJECT

public:
    Core();
    ~Core() override;

    Q_PROPERTY(bool              isGPSAlive                   READ getIsGPSAlive                   NOTIFY isGPSAliveChanged)
    Q_PROPERTY(bool              isFactoryMode                READ isFactoryMode                   CONSTANT)
    Q_PROPERTY(ConsoleListModel* consoleList                  READ consoleList                     CONSTANT)
    Q_PROPERTY(ConsoleListModel* consoleListApp               READ consoleListApp                  CONSTANT)
    Q_PROPERTY(ConsoleListModel* consoleListProto             READ consoleListProto                CONSTANT)
    Q_PROPERTY(bool              loggingKlf                   READ getKlfLogging                   WRITE setKlfLogging                   NOTIFY loggingKlfChanged)
    Q_PROPERTY(bool              isKlfLogging                 READ getKlfLogging                   NOTIFY loggingKlfChanged)
    Q_PROPERTY(bool              loggingCsv                   READ getCsvLogging                   WRITE setCsvLogging                   NOTIFY loggingCsvChanged)
    Q_PROPERTY(bool              useGPS                       READ getUseGPS                       WRITE setUseGPS                       NOTIFY useGPSChanged)
    Q_PROPERTY(bool              bringWindowToFrontEnabled    READ getBringWindowToFrontEnabled    WRITE setBringWindowToFrontEnabled    NOTIFY bringWindowToFrontEnabledChanged)
    Q_PROPERTY(bool              fixBlackStripesState         READ getFixBlackStripesState         WRITE setFixBlackStripesState         NOTIFY fixBlackStripesStateChanged)
    Q_PROPERTY(int               fixBlackStripesForwardSteps  READ getFixBlackStripesForwardSteps  WRITE setFixBlackStripesForwardSteps  NOTIFY fixBlackStripesForwardStepsChanged)
    Q_PROPERTY(int               fixBlackStripesBackwardSteps READ getFixBlackStripesBackwardSteps WRITE setFixBlackStripesBackwardSteps NOTIFY fixBlackStripesBackwardStepsChanged)
    Q_PROPERTY(QString           filePath                     READ getFilePath                     NOTIFY filePathChanged)
    Q_PROPERTY(QString           openedFilePath               READ getOpenedFilePath               NOTIFY openedFilePathChanged)
    Q_PROPERTY(bool              isAppendMode                 READ getIsAppendMode                 NOTIFY isAppendModeChanged)
    Q_PROPERTY(QString           fileTitle                    READ getFileTitle                    NOTIFY fileTitleChanged)
    Q_PROPERTY(bool              isFileOpening                READ getIsFileOpening                NOTIFY sendIsFileOpening)
    Q_PROPERTY(bool              trackReprocessing            READ getTrackReprocessing            NOTIFY trackReprocessingChanged)
    Q_PROPERTY(bool              isSeparateReading            READ getIsSeparateReading            CONSTANT)
    Q_PROPERTY(QString           ch1Name                      READ getChannel1Name                 NOTIFY channelListUpdated FINAL)
    Q_PROPERTY(QString           ch2Name                      READ getChannel2Name                 NOTIFY channelListUpdated FINAL)
    Q_PROPERTY(int               dataProcessorState           READ getDataProcessorState           NOTIFY dataProcessorStateChanged)
    Q_PROPERTY(int               mapTileProviderId            READ getMapTileProviderId            NOTIFY mapTileProviderChanged)
    Q_PROPERTY(QString           mapTileProviderName          READ getMapTileProviderName          NOTIFY mapTileProviderChanged)
    Q_PROPERTY(QVariantList      mapTileProviders             READ getMapTileProviders             CONSTANT)
    Q_PROPERTY(bool              internetAvailable            READ getInternetAvailable            NOTIFY internetAvailableChanged)
    Q_PROPERTY(bool              metered                      READ getMetered                      NOTIFY meteredChanged)
    Q_PROPERTY(bool              mapTileLoadingEnabled        READ getMapTileLoadingEnabled        WRITE setMapTileLoadingEnabled NOTIFY mapTileLoadingEnabledChanged)
    Q_PROPERTY(bool              needForceZooming             READ getNeedForceZooming             WRITE setNeedForceZooming NOTIFY needForceZoomingChanged)
    Q_PROPERTY(bool              posZeroing                   READ getPosZeroing                   NOTIFY posZeroingChanged)
    Q_PROPERTY(int               bottomTrackEditTool          READ getBottomTrackEditTool          WRITE setBottomTrackEditTool          NOTIFY bottomTrackEditToolChanged)

    MosaicIndexProvider* getMosaicIndexProviderPtr();
    void setEngine(QQmlApplicationEngine *engine);
    Console* getConsolePtr();
    Dataset* getDatasetPtr();
    // Echogram edits (QML "echoEdit"): undo, redo, the trim, the magnet, the file of edits.
    EchogramEditor* getEchogramEditorPtr() { return echogramEditor_.get(); }
    UsbTap* getUsbTapPtr() { return &usbTap_; }
    DataProcessor* getDataProcessorPtr() const;
    DeviceManagerWrapper* getDeviceManagerWrapperPtr() const;
    LinkManagerWrapper* getLinkManagerWrapperPtr() const;
    DeviceTopologyModel* getDeviceTopologyModelPtr() const;
#ifdef SEPARATE_READING
    QString getTryOpenedfilePath() const;
    void stopDeviceManagerThread() const;
#endif
    void setConsoleOutputEnabled(bool enabled);
    bool isConsoleOutputEnabled() const { return consoleOutputEnabled_; }
    Q_INVOKABLE void consoleInfo(QString msg);
    Q_INVOKABLE void consoleWarning(QString msg);
    void consoleStreamInfo(const QString& msg);
    void consoleNotification(const QString& msg, bool isWarning);
    void consoleProtoText(const QString& msg);
    void consoleProto(FrameParser& parser, bool isIn = true);
    Q_INVOKABLE void setConsoleMaxRows(int rows);
    void saveLLARefToSettings();
    Q_INVOKABLE void saveCameraViewToSettings();

    // Side-scan relief point cloud (backlog item 9). Building walks every ping,
    // so it happens when the layer is switched on, not as data arrives.
    Q_INVOKABLE void setReliefCloudVisible(bool state);
    // Settings, Performance (WorkspaceStore's performancePrefs, applied at start and on change).
    Q_INVOKABLE void setMosaicPrecomputeEnabled(bool on);
    Q_INVOKABLE void setMosaicMultiThreaded(bool on);
    Q_INVOKABLE void setZoomFallbackEnabled(bool on);
    void removeLinkManagerConnections();
#ifdef SEPARATE_READING
    void removeDeviceManagerConnections();
#endif
    QHash<QUuid, QString> getLinkNames() const;
    void shutdownBackgroundWorkers();
    bool getIsGPSAlive() const { return isGPSAlive_; };
    bool getKlfLogging() const;
    bool getFixBlackStripesState() const;
    int  getFixBlackStripesForwardSteps() const;
    int  getFixBlackStripesBackwardSteps() const;
    bool getCsvLogging() const;
    Q_INVOKABLE QString klfLogFilePath() const;
    Q_INVOKABLE void    revealInFolder(const QString& path);
    /// Android: the system share sheet for a file (a .kmap to a messenger). False elsewhere, or when
    /// it cannot be offered (a warning then says so).
    Q_INVOKABLE bool    shareFile(const QString& path, const QString& chooserTitle);
    Q_INVOKABLE void    copyToClipboard(const QString& text);
    Q_INVOKABLE QString csvLogFilePath() const;
    Q_INVOKABLE qint64  activeLogSizeBytes() const;
    Q_INVOKABLE int     activeLogDurationSecs() const;
    Q_INVOKABLE void    setLogDirectory(const QString& dir);
    Q_INVOKABLE QString logDirectory() const;
    Q_INVOKABLE QString logDirectoryUrl() const;
    Q_INVOKABLE bool    prepareLogDirectory(const QString& dir);
    Q_INVOKABLE QString appLogDirectory() const;
    Q_INVOKABLE bool    promoteAppLogStorage();
    Q_INVOKABLE QString appLogFilePath() const;
    Q_INVOKABLE void    revealAppLogFolder();
    Q_INVOKABLE void    powerOffSystem();
    bool getUseGPS() const;
    bool getNeedForceZooming() const { return needForceZooming_; }

    void deferStartupFileOpen(const QString& filePath);
    Q_INVOKABLE void notifyUiSettingsApplied();

public slots:
    void setIsGPSAlive(bool state) { qDebug() << "Core::setIsGPSAlive" << state; isGPSAlive_ = state; emit isGPSAliveChanged(); }

#ifdef SEPARATE_READING
    void openLogFile(const QString& filePath, bool isAppend = false, bool onCustomEvent = false);
    bool closeLogFile(bool onOpen = false);
    void onFileStartOpening();
    void onFileReadEnough();
    void onFileOpenBreaked(bool onOpen);
#else
    void openLogFile(const QString& filePath, bool isAppend = false, bool onCustomEvent = false);
    bool closeLogFile();
#endif
    void onRequestClearing();

    void onFileOpened();
    bool openXTF(const QByteArray& data);
    bool openCSV(QString name, int separatorType, int row = -1, int colTime = -1, bool isUtcTime = true, int colLat = -1, int colLon = -1, int colAltitude = -1, int colNorth = -1, int colEast = -1, int colUp = -1);
    bool openProxy(const QString& address, const int port, bool isTcp);
    bool closeProxy();
    bool upgradeFW(const QString& name, QObject* dev);
    /// Every ring board on the line of dev, the last one first (docs/RING_UPDATE.md).
    bool upgradeFWAll(const QString& name, QObject* dev);
    void upgradeChanged(int progressStatus);
    void setKlfLogging(bool isLogging);
    void setFixBlackStripesState(bool state);
    void setFixBlackStripesForwardSteps(int val);
    void setFixBlackStripesBackwardSteps(int val);
    void setBottomTrackRealtimeFromSettings(bool state);
    void setCsvLogging(bool isLogging);
    void setNeedForceZooming(bool state);
    void setUseGPS(bool state);
    bool exportComplexToCSV(QString filePath);
    bool exportUSBLToCSV(QString filePath);
    bool exportPlotAsCVS(QString filePath, const ChannelId& channelId, float decimation = 0);
    bool exportPlotAsXTF(QString filePath);
    Q_INVOKABLE bool csvExportFieldEnabled(const QString& key) const;
    Q_INVOKABLE void setCsvExportField(const QString& key, bool enabled);
    Q_INVOKABLE void resetCsvExportFields();
    Q_INVOKABLE QString defaultExportDirectory() const;
    void refreshMosaicProcessing();
    void setPlotStartLevel(int level);
    void setPlotStopLevel(int level);
    void setTimelinePosition(double position);
    void resetAim();
    void UILoad(QObject* object, const QUrl& url);
    void setMosaicChannels(const QString& firstChStr, const QString& secondChStr);
    void onChannelsUpdated();
    void onRedrawEpochs(const QSet<int>& indxs);
    void initAfterApp();
    void initStreamList();

#ifdef FLASHER
    void connectOpenedLinkAsFlasher(QString pn);
    void setFlasherData(QString data);
    void releaseFlasherLink();
    void refreshFlasherProducts();          // refresh the factory device list
#endif

    Q_INVOKABLE void setPosZeroing(bool state);
    Q_INVOKABLE void setBottomTrackZeroing(bool state);
    Q_INVOKABLE void setTgcGainNear(float val);
    Q_INVOKABLE void setTgcGainFar(float val);
    Q_INVOKABLE void setTgcCompensate(bool state);
    Q_INVOKABLE void setMosaicSource(int source);
    Q_INVOKABLE void setMosaicFakeCoordsLastN(int n);
    Q_INVOKABLE void setMosaicFakeCoordsClearOldData(bool state);
    /**
     * @brief Track smoothing from the mosaic settings (TrackSmoother, docs/track-interpolation.md).
     * @param enabled   off: pings straight between neighbouring fixes and drawn at once, as before.
     * @param mode      0 responsive, 1 normal, 2 smooth: the accuracy aimed at, 0.12 / 0.08 / 0.06 m.
     * @param reprocess an open recording is read again, the view kept, so that all of it is drawn
     *                  the new way; live data takes the change from the next epochs on.
     */
    Q_INVOKABLE void setTrackSmoothing(bool enabled, int mode, bool reprocess);
    bool getTrackReprocessing() const { return trackReprocessing_; }
    bool getPosZeroing() const { return isActiveZeroing_; }
    int  getBottomTrackEditTool() const { return bottomTrackEditTool_; }
    Q_INVOKABLE void setBottomTrackEditTool(int tool);
    Q_INVOKABLE bool getIsFileOpening() const;
    Q_INVOKABLE bool getIsAppendMode() const;
    Q_INVOKABLE QString getFileTitle() const;
    Q_INVOKABLE bool getIsSeparateReading() const;
    Q_INVOKABLE int getDataProcessorState() const;
    Q_INVOKABLE QString getChannel1Name() const;
    Q_INVOKABLE QString getChannel2Name() const;
    Q_INVOKABLE void registerPlot2D(QObject* plotObj);
    Q_INVOKABLE void setEchogramSyncCursor(bool state);
    Q_INVOKABLE void setEchogramSyncView(bool state);
    Q_INVOKABLE void setAimFieldsMask(int mask);
    bool echogramSyncCursor() const { return echogramSyncCursor_; }
    bool echogramSyncView() const { return echogramSyncView_; }
    void broadcastEpochCursor(qPlot2D* source, int epoch, float depth, int channel);
    void broadcastCursorClear(qPlot2D* source);
    /**
     * @brief Clears every echogram's synced cursor (another echogram's pointer, a pick on the 3D track).
     *
     * Only its source clears a synced cursor, and a source taken out of the layout never does; WorkspaceView calls
     * this when the panes change, and a live source sends its cursor again on its next move.
     */
    Q_INVOKABLE void clearEchogramSyncCursors();
    Q_INVOKABLE void broadcastEchogramTime(QObject* source, double timelinePos);        // time/scroll → gated by echogramSyncCursor_
    Q_INVOKABLE void broadcastEchogramVertical(QObject* source, double from, double to); // vertical zoom+offset → gated by echogramSyncView_
    Q_INVOKABLE void registerSyncLoupePlot(QObject* plotObj);
    Q_INVOKABLE QVariant getConvertedMousePos(int indx, int mouseX, int mouseY);

    Q_INVOKABLE void setIsAttitudeExpected(bool state);
    Q_INVOKABLE void setMapTileProvider(int providerId);
    Q_INVOKABLE void toggleMapTileProvider();
    Q_INVOKABLE int getMapTileProviderId() const;
    Q_INVOKABLE QString getMapTileProviderName() const;
    Q_INVOKABLE QVariantList getMapTileProviders() const;
    Q_INVOKABLE QVariantMap getMapTileDbInfo(int providerId) const;
    Q_INVOKABLE bool getInternetAvailable() const;
    Q_INVOKABLE bool getMetered() const;
    Q_INVOKABLE void setDeferTilesOnMetered(bool defer);
    Q_INVOKABLE bool getMapTileLoadingEnabled() const;
    Q_INVOKABLE void setMapTileLoadingEnabled(bool enabled);
    Q_INVOKABLE bool getBringWindowToFrontEnabled() const;
    Q_INVOKABLE void setBringWindowToFrontEnabled(bool enabled);
    Q_INVOKABLE void moveAppToBackground();
    Q_INVOKABLE void requestDismissTransientUi();
    Q_INVOKABLE void setActiveTransientUi(QObject* who);

signals:
    void appLogPathChanged();
    void bringWindowToFrontRequested();
    void bringWindowToFrontEnabledChanged();
    void activeTransientUiChanged(QObject* who);
    void csvExportFieldsReset();   // emitted by resetCsvExportFields() so UI can rebuild
    void connectionChanged(bool duplex = false);
    void filePathChanged();
    void openedFilePathChanged();
    void isAppendModeChanged();
    void fileTitleChanged();
    void fileOpenFailed(const QString& path);
    void receivedMapOpened();   // another app handed a KOGGER map over and it is in My maps
    void sendIsFileOpening();
    void trackReprocessingChanged();
    void channelListUpdated();
    void dataProcessorStateChanged();
    void needForceZoomingChanged();
    void isGPSAliveChanged();
    void loggingKlfChanged();
    void loggingCsvChanged();
    void useGPSChanged();
    void fixBlackStripesStateChanged();
    void fixBlackStripesForwardStepsChanged();
    void fixBlackStripesBackwardStepsChanged();
    void mapTileProviderChanged();
    void internetAvailableChanged();
    void meteredChanged();
    void mapTileLoadingEnabledChanged();
    void posZeroingChanged();
    void bottomTrackEditToolChanged();
    void languageChanged();

#ifdef SEPARATE_READING
    void sendCloseLogFile(bool onOpen = false);
#endif

private slots:
    void onFileStopsOpening();
    void onSendMapTextureIdByTileIndx(const map::TileIndex& tileIndx, GLuint textureId); // TODO: maybe store map texture id in mapView
    void onDataProcesstorStateChanged(const DataProcessorType& state);
    void onSendFrameInputToLogger(QUuid uuid, Link* link, const Parsers::FrameParser& frame);

private:
    /*methods*/
    void bringWindowToFront();
    // A running map export (src/map_export) reads the dataset: stopped before anything clears it.
    void stopMapExport();
    // A .kmap given to openLogFile is opened into My maps instead; true when it was one.
    bool openMapFileInstead(const QString& path);
    void loadCsvExportFields();
    void saveCsvExportFields();
    QHash<QString, bool> csvExportFields_;   // key -> enabled (lazy-loaded from QSettings)
    bool csvExportFieldsLoaded_ = false;
    void createMapTileManagerConnections();
    void createDatasetConnections();
    void createInternetManager();
    void destroyInternetManager();
    void updateTileDownloadGate();
    void createDataProcessor();
    void destroyDataProcessor();
    void createScene3dConnections();
    void bindPlot2D(qPlot2D* plot);

    void createDataHorizonConnections();
    void destroyDataHorizonConnections();

    void setDataProcessorConnections();
    void resetDataProcessorConnections();

    ConsoleListModel* consoleList();
    ConsoleListModel* consoleListApp();
    ConsoleListModel* consoleListProto();
    void createControllers();
    void createDeviceManagerConnections();
    void createLinkManagerConnections();
    bool isOpenedFile() const;
    bool isFactoryMode() const;

    QString getFilePath() const;
    QString getOpenedFilePath() const;
    void fixFilePathString(QString& filePath) const;
    void notifyFileOpened(const QString& filePath);
    void loadLLARefFromSettings();
    void loadCameraViewFromSettings();
    void onTgcParamsChanged();
    int loadSavedMapTileProviderId() const;
    void resetRealtimeSessionState();
    void restoreRealtimeProcessingFlags();
    void releasePlotCaches();
    void flushStartupFileOpen();
    void installAppLogStoragePromotion();
    void installOpenFileHandler();
    void openReceivedFile(const QString& uri);
    QString resolveExportBasePath(const QString& basePath) const;
    QString buildExportFileStem(const QString& openedFilePath) const;
#ifdef Q_OS_ANDROID
    QString resolveAndroidUriToPath(const QString& uriString) const;
#endif

    /*data*/
    Console* consolePtr_;
    // 3d scene controllers
    std::shared_ptr<BoatTrackControlMenuController> boatTrackControlMenuController_;
    std::shared_ptr<NavigationArrowControlMenuController> navigationArrowControlMenuController_;
    std::shared_ptr<BottomTrackControlMenuController> bottomTrackControlMenuController_;
    std::shared_ptr<MpcFilterControlMenuController> mpcFilterControlMenuController_;
    std::shared_ptr<NpdFilterControlMenuController> npdFilterControlMenuController_;
    std::shared_ptr<IsobathsViewControlMenuController> isobathsViewControlMenuController_;
    std::shared_ptr<MosaicViewControlMenuController> mosaicViewControlMenuController_;
    std::shared_ptr<ImageViewControlMenuController> imageViewControlMenuController_;
    std::shared_ptr<MapViewControlMenuController> mapViewControlMenuController_;
    std::shared_ptr<PointGroupControlMenuController> pointGroupControlMenuController_;
    std::shared_ptr<PolygonGroupControlMenuController> polygonGroupControlMenuController_;
    std::shared_ptr<Scene3DControlMenuController> scene3dControlMenuController_;
    std::shared_ptr<Scene3dToolBarController> scene3dToolBarController_;
    std::unique_ptr<HotkeysController> hotkeysController_;
    std::unique_ptr<DeviceManagerWrapper> deviceManagerWrapperPtr_;
    std::unique_ptr<LinkManagerWrapper> linkManagerWrapperPtr_;
    std::unique_ptr<DeviceTopologyModel> deviceTopologyModelPtr_;
    InternetManager* internetManager_;
    QThread* internetThread_;
    std::unique_ptr<map::TileManager> tileManager_;
    // data processor
    DataProcessor* dataProcessor_;
    QThread* dataProcThread_;
    std::unique_ptr<DataHorizon> dataHorizon_; // this thread
    std::unique_ptr<EchogramEditor> echogramEditor_;   // this thread; the epochs it edits are datasetPtr_'s
    std::unique_ptr<MapExporter> mapExporter_; // QML "mapExport"
    std::unique_ptr<MapLibrary> mapLibrary_;   // QML "mapLibrary": My maps
    std::unique_ptr<MapLayer> mapLayer_;       // My maps in the 3D scene
    bool mapLayerConnected_ = false;   // UILoad's lambdas for My maps, made once

#ifdef SEPARATE_READING
    QString tryOpenedfilePath_;
    bool fileIsCompleteOpened_ = false;
    QList<QMetaObject::Connection> deviceManagerWrapperConnections_;
#endif

    QQmlApplicationEngine* qmlAppEnginePtr_;
    Dataset* datasetPtr_;
    QPointer<GraphicsScene3dView> scene3dViewPtr_;
    // the relief-cloud layer as the store last asked for it: the store restores
    // before UILoad finds the 3D view, so UILoad applies it again
    bool reliefCloudVisible_ = false;
    bool zoomFallbackEnabled_ = true;   // as reliefCloudVisible_: UILoad applies it to the 3D view found then
    ConverterXTF converterXtf_;
    Logger logger_;
    UsbTap usbTap_;   // IQ / RAW stream from the sonar's second core, see usb_tap.h
    QList<qPlot2D*> plot2dList_;
    bool echogramSyncCursor_ = true;  // default ON  (cursor/click + time/scroll sync)
    bool echogramSyncView_ = false;   // default OFF (vertical zoom + offset sync)
    int aimFieldsMask_ = 0xFF;
    QPointer<qPlot2D> syncLoupePlot3dPtr_;
    QList<QMetaObject::Connection> linkManagerWrapperConnections_;
    QString openedfilePath_;
    bool isLoggingKlf_;
    bool isLoggingCsv_;
    bool consoleOutputEnabled_ = true;
    QString filePath_;
    QString fChName_;
    QString sChName_;
    QString startupFilePath_;
    bool uiSettingsApplied_ = false;

    bool isFileOpening_;
    bool isAppendMode_ = false;
    QStringList appendedFiles_;
    // Reading the open recording again for the track smoothing (setTrackSmoothing), the view kept:
    // the place being compared stays on screen.
    void reopenFilesKeepingView();
    void afterFileOpened(bool reprocess);
    void setTrackReprocessing(bool state);
    QStringList reopenQueue_;           // the files appended after the one being read again
    bool nextOpenIsReprocess_ = false;  // taken by the next openLogFile; any other open ends the chain
    bool reprocessPending_ = false;     // asked while a file was opening: read again once it is in
    bool trackReprocessing_ = false;
    QList<QUuid> openLinkOrder_;

    /**
     * Measured stream shown in the window title.
     *
     * Recomputed when a ping arrives, never on a timer of its own, so the
     * number is always an average over whole pings rather than over a window
     * that cuts one in half. A ping every few milliseconds would make it
     * unreadable, so the average runs over at least kRateWindowMs and the
     * update still lands on a ping boundary; with a slower device that is
     * every single ping. The one timer here only notices that the pings
     * stopped, because then no ping can come to zero the number.
     */
    void onEpochForRate();   // зовётся по каждому приходу данных от прибора
    void updateRateIdle();

    static constexpr qint64 kRateWindowMs = 500;   // не короче: иначе цифра пляшет
    static constexpr qint64 kRateIdleMs   = 2000;  // столько без посылок — поток нулевой

    QElapsedTimer rateClock_;
    QTimer        rateIdleTimer_;
    quint64       rateBytesMark_ = 0;
    qint64        rateWindowStartMs_ = 0;
    qint64        rateLastEpochMs_ = 0;
    double        rateKbps_ = 0.0;
    bool          rateShown_ = false;
    QSet<QUuid> receivingLinks_; // links currently receiving data; window is raised on the empty→non-empty edge

    bool isGPSAlive_;
    bool isUseGPS_;
    bool internetAvailable_ = false;
    bool internetStateKnown_ = false;
    bool metered_ = false;
    bool deferTilesOnMetered_ = true;
    bool mapTileLoadingEnabled_ = true;
    bool bringWindowToFrontEnabled_ = true;
    bool needForceZooming_ = false; // debug

    bool fixBlackStripesState_;
    int  fixBlackStripesForwardSteps_;
    int  fixBlackStripesBackwardSteps_;

    bool isActiveZeroing_;
    bool isBottomTrackZeroing_;
    int  bottomTrackEditTool_ = 0;

#ifdef FLASHER
    Q_PROPERTY(QString flasherTextInfo READ flasherTextInfo NOTIFY dev_flasher_changed)
    Q_PROPERTY(int flasherIdInfo READ flasherIdInfo NOTIFY dev_flasher_changed)
    Q_PROPERTY(QVariantList flasherProducts READ flasherProducts NOTIFY flasherProductsChanged)
    Q_PROPERTY(bool flasherHasToken READ flasherHasToken NOTIFY flasherHasTokenChanged)
private:
    DeviceFlasher dev_flasher_;
    int dev_flasher_msg_id_ = 0;
    QString dev_flasher_msg_;

    QString flasherTextInfo() { return dev_flasher_msg_; }
    int flasherIdInfo() { return dev_flasher_msg_id_; }
    QVariantList flasherProducts() { return dev_flasher_.products(); }
    bool flasherHasToken() { return dev_flasher_.hasToken(); }
private slots:
    void dev_flasher_rcv(QString msg, int num);
signals:
    void dev_flasher_changed();
    void flasherProductsChanged();
    void flasherHasTokenChanged();
#endif

private:   // reset access after the (signals-terminated) FLASHER block — else these leak into signals: under -DFLASHER
    QVector<QMetaObject::Connection> dataProcessorConnections_;
    QVector<QMetaObject::Connection> dataHorizonConnections_;

    DataProcessorType dataProcessorState_ = DataProcessorType::kUndefined;

    ChannelId lastCh1_;
    uint8_t   lastSub1_;
    ChannelId lastCh2_;
    uint8_t   lastSub2_;

    MosaicIndexProvider mosaicIndexProvider_;

    UiKeepalive uiKeepalive_;
};
