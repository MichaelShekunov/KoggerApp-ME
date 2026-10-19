#pragma once

#include <QObject>
#include <QVector>
#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QPixmap>
#include <QPainter>
#include <QEvent>
#include <QPointer>


#include "plot2D_aim.h"
#include "plot2D_attitude.h"
#include "plot2D_bottom_processing.h"
#include "plot2D_contact.h"
#include "plot2D_defs.h"
#include "plot2D_dvl_beam_velocity.h"
#include "plot2D_dvl_solution.h"
#include "plot2D_echogram.h"
#include "plot2D_encoder.h"
#include "plot2D_gnss.h"
#include "plot2D_grid.h"
#include "plot2D_temperature.h"
#include "plot2D_quadrature.h"
#include "plot2D_rangefinder.h"
#include "plot2D_depth.h"
#include "plot2D_usbl_solution.h"
#include "dataset.h"
#include "data_processor.h"

class EchogramEditor;

class Plot2D
{
public:
    Plot2D();

    void setDataset(Dataset* dataset);
    void setDataProcessorPtr(DataProcessor* dataProcessorPtr);
    // Echogram edits: a stroke of a tool is one step of the editor's history (undo, the file of edits);
    // the pencil's magnet is the editor's switch.
    void setEchogramEditor(EchogramEditor* editor);

    float getDepthByMousePos(int mouseX, int mouseY, bool isHorizontal) const;
    float getSyncDepthByMousePos(int mouseX, int mouseY, bool isHorizontal, int* channelOut) const;
    int getEpochIndxByMousePos(int mouseX, int mouseY, bool isHorizontal) const;
    QPoint getMousePosByDepthAndEpochIndx(float depth, int epochIndx, bool isHorizontal) const;

    void addReRenderPlotIndxs(const QSet<int>& indxs);

    bool getPlotEnabled() const;
    void setPlotEnabled(bool state);

    bool plotEnabled() const;
    bool getLoupeVisible() const;
    void setLoupeVisible(bool state);
    int getLoupeSize() const;
    void setLoupeSize(int size);
    int getLoupeZoom() const;
    void setLoupeZoom(int zoom);

    bool isHorizontal();
    void setHorizontal(bool is_horizontal);

    void setAimEpochEventState(bool state);
    void setTimelinePosition(float position);
    void resetAim();

    void setTimelinePositionSec(float position);
    void setTimelinePositionByEpoch(int epochIndx);
    void setSyncCursor(int epoch, float depth, int channel);
    void clearSyncCursor();
    virtual void syncClearAim() {}
    bool hasSyncDepth() const { return syncDepthValid_; }
    float getSyncDepth() const { return syncDepth_; }
    int getSyncChannel() const { return syncChannel_; }
    void setAimFieldsMask(int mask) { aimFieldsMask_ = mask; }
    int getAimFieldsMask() const { return aimFieldsMask_; }

    float timelinePosition();
    /// Scrolls the history by `columns` screen columns (pings at the horizontal scale).
    void scrollPosition(float columns);

    /// Screen columns a ping takes: 2, 3, 4, 6 or 8 stretch the history, 1/2..1/16 compress it (a column
    /// shows every n-th ping, the newest of its group), 1 is a column a ping. Snapped to those steps.
    float horizontalScale() const { return horScale_; }
    void setHorizontalScale(float scale);
    /// One step finer (steps > 0) or coarser, keeping the ping under `anchorColumn` (-1: the newest) in
    /// place. Returns whether the scale changed.
    bool stepHorizontalScale(int steps, int anchorColumn = -1);
    /// The screen column showing `epochIndx`, or the column whose group holds it when compressed; -1.
    int columnOfEpoch(int epochIndx) const;
    /// The pings a screen column stands for: one, or its group's pings when compressed. false if none.
    bool epochSpanOfColumn(int column, int* first, int* last) const;

    void setDataChannel(bool fromGui, const ChannelId& channel, uint8_t subChannel1, const QString& portName1, const ChannelId& channel2 = channelNone(), uint8_t subChannel2 = 0, const QString& portName2 = QString());

    bool getIsContactChanged();

    QString getContactInfo();
    void    setContactInfo(const QString& str);
    bool    getContactVisible();
    void    setContactVisible(bool state);
    int     getContactPositionX();
    int     getContactPositionY();
    int     getContactIndx();
    double  getContactLat();
    double  getContactLon();
    double  getContactDepth();
    bool    getContactIsActive();

    bool getImage(int width, int height, QPainter* painter, bool is_horizontal);
    void draw(QPainter* painterPtr);
    bool drawEchogramZoomPreview(QPainter* painter, const QRect& targetRect, const QPoint& sourceCenter, int sourceSize, QPointF* focusPoint = nullptr);
    void copyVisualConfigTo(Plot2D& dst) const;
    bool drawEchogramZoomPreview(QPainter* painter, const QRect& targetRect, const QPoint& sourceCenter, int sourceWidth, int sourceHeight, QPointF* focusPoint = nullptr);

    float getCursorDistance() const;
    std::tuple<ChannelId, uint8_t, QString> getSelectedChannelId(float cursorDistance = 0.0f) const;

    float getEchogramLowLevel() const;
    float getEchogramHighLevel() const;
    int getThemeId() const;
    int getEchogramCompensation() const;
    void setEchogramLowLevel(float low);
    void setEchogramHightLevel(float high);
    void setEchogramVisible(bool visible);
    void setEchogramTheme(int theme_id);
    void setEchogramCompensation(int compensation_id);

    bool getEchogramDeconvEnabled() const;
    void setEchogramDeconvEnabled(bool state);
    float getEchogramDeconvAngularResolution() const;
    void setEchogramDeconvAngularResolution(float degrees);
    int getEchogramDeconvIterations() const;
    void setEchogramDeconvIterations(int iterations);
    bool getEchogramDeconvGeometry() const;
    void setEchogramDeconvGeometry(bool state);

    void setBottomTrackVisible(bool visible);
    void setBottomTrackTheme(int theme_id);
    void setBottomTrackDepthTextVisible(bool visible);
    bool getBottomTrackVisible() const;
    int getBottomTrackTheme() const;

    void setRangefinderVisible(bool visible);
    void setRangefinderTheme(int theme_id);
    void setRangefinderDepthTextVisible(bool visible);
    bool getRangefinderVisible() const;
    int getRangefinderTheme() const;
    void setAttitudeVisible(bool visible);
    void setTemperatureVisible(bool visible);
    bool hasTemperatureValue() const;
    bool hasRangefinderDepthTextValue() const;
    void setDopplerBeamVisible(bool visible, int beam_filter);
    void setDopplerInstrumentVisible(bool visible, int line_filter = -1);
    void setDVLLegendVisible(bool visible);
    void setDVLLegendPosition(int pos);

    void setGNSSVisible(bool visible, int flags);

    void setAcousticAngleVisible(bool visible);

    void setGridVetricalNumber(int grids);
    void setGridFillWidth(bool state);
    void setGridInvert(bool state);
    void setAngleVisibility(bool state);
    void setAngleRange(int angleRange);

    void setVelocityVisible(bool visible);
    void setVelocityRange(float velocity);
    void setDistanceAutoRange(int auto_range_type);

    bool getEchogramVisible() const { return echogram_.isVisible(); }
    bool getBottomTrackDepthTextVisible() const { return bottomProcessing_.isDepthTextVisible(); }
    bool getRangefinderDepthTextVisible() const { return rangefinder_.isDepthTextVisible(); }
    bool getAttitudeVisible() const { return attitude_.isVisible(); }
    bool getTemperatureVisible() const { return temperature_.isVisible(); }
    bool getAcousticAngleVisible() const { return usblSolution_.isVisible(); }
    bool getGNSSVisible() const { return gnss_.isVisible(); }
    bool getDopplerBeamVisible() const { return dvlBeamVelocity_.isVisible(); }
    int  getDopplerBeamFilter() const { return dvlBeamVelocity_.getBeamFilter(); }
    bool getDopplerInstrumentVisible() const { return dvlSolution_.isVisible(); }
    int  getDopplerInstrumentFilter() const { return dvlSolution_.getLineFilter(); }
    bool getDVLLegendVisible() const { return dvlLegendVisible_; }
    int  getDVLLegendPosition() const { return dvlLegendPosIndex_; }
    int  getGridVerticalNumber() const { return grid_.getVetricalNumber(); }
    bool getGridFillWidth() const { return grid_.isFillWidth(); }
    bool getGridInvert() const { return grid_.isInvert(); }
    bool getAngleVisibility() const { return grid_.getAngleVisibility(); }
    bool getVelocityVisible() const { return grid_.getVelocityVisible(); }
    int  getAngleRange() const { return std::isfinite(cursor_.attitude.to) ? static_cast<int>(std::lround(cursor_.attitude.to)) : 0; }
    float getVelocityRange() const { return std::isfinite(cursor_.velocity.to) ? cursor_.velocity.to : 0.0f; }
    int  getDistanceAutoRange() const { return static_cast<int>(cursor_.distance.mode); }

    void setDistance(float from, float to);
    void zoomDistance(float ratio);
    void scrollDistance(float ratio);

    void setMousePosition(int x, int y, bool isSync = false);
    void simpleSetMousePosition(int x, int y);
    void setMouseTool(MouseTool tool);
    bool setContact(int indx, const QString& text);
    bool setActiveContact(int indx);
    bool deleteContact(int indx);
    void updateContact();
    void onCursorMoved(int x, int y);

    Canvas& canvas();
    DatasetCursor& cursor();

    void resetCash();
    void releaseCache();
    Canvas image(int width, int height);
    void reindexingCursor();
    void reRangeDistance();

    virtual void plotUpdate();
    virtual void sendSyncEvent(int epoch_index, QEvent::Type eventType);

    void setMosaicLOffset(float val);
    void setMosaicROffset(float val);

protected:
    Canvas canvas_;
    DatasetCursor cursor_;

    Plot2DAim aim_;
    Plot2DAttitude attitude_;
    Plot2DBottomProcessing bottomProcessing_;
    Plot2DContact contacts_;
    Plot2DDVLBeamVelocity dvlBeamVelocity_;
    Plot2DDVLSolution dvlSolution_;
    Plot2DEchogram echogram_;
    Plot2DEncoder encoder_;
    Plot2DGNSS gnss_;
    Plot2DGrid grid_;
    Plot2DTemperature temperature_;
    Plot2DQuadrature quadrature_;
    Plot2DRangefinder rangefinder_;
    Plot2DDepth depth_;
    Plot2DUSBLSolution usblSolution_;
    Dataset* datasetPtr_;
    DataProcessor* dataProcessorPtr_;
    std::function<void()> pendingBtpLambda_;
    bool isHorizontal_;
    bool dvlLegendVisible_ = true;
    int  dvlLegendPosIndex_ = 0;
    bool syncDepthValid_ = false;
    float syncDepth_ = 0.0f;          // absolute physical depth (>=0 from surface)
    int syncChannel_ = 1;             // which channel the synced depth belongs to (1/2)
    int aimFieldsMask_ = 0xFF;
    QPointer<EchogramEditor> editor_;
    bool strokeOpen_ = false;         // a press of an edit tool is under way (setMousePosition)
    float horScale_ = 1.0f;           // screen columns a ping takes (horizontalScale)

    // The pencil's magnet: the bottom track's bottom looked for within a few pixels of dist, else dist.
    float magnetDistance(int epochIndex, float dist, float metersPerPixel) const;
    // Pings marked bad, and trimmed off, shaded over the echogram.
    void drawEditShading();

private:
    bool isEnabled_;
    bool isLoupeVisible_;
    int loupeSize_;
    int loupeZoom_;
    float lAngleOffsetDeg_;
    float rAngleOffsetDeg_;
};

class MiniPreviewPlot2D final : public Plot2D
{
public:
    MiniPreviewPlot2D();

    bool render(QPainter* painter,
                Dataset* dataset,
                const Plot2D* configSource,
                const DatasetCursor& parentCursor,
                int parentCanvasWidth,
                int sourceLeft,
                int sourceWidth,
                int previewWidth,
                int previewHeight,
                float zoomFrom,
                float zoomTo,
                int themeId,
                float lowLevel,
                float highLevel,
                int compensationId);

private:
    void updateEchogramSettings(int themeId, float lowLevel, float highLevel, int compensationId);

    int cachedThemeId_ = -1;
    int cachedCompensationId_ = -1;
    float cachedLowLevel_ = NAN;
    float cachedHighLevel_ = NAN;
};
