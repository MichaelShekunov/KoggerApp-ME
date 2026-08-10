#include "plot2D_echogram.h"
#include "plot2D.h"
#include "echogram_deconvolution.h"

#include <cmath>


Plot2DEchogram::Plot2DEchogram()
{
    setThemeId(ClassicTheme);
    setLevels(10, 100);
}

Plot2DEchogram::~Plot2DEchogram()
{
    releaseCache();
    delete miniPreviewPlot_;
}

void Plot2DEchogram::setLowLevel(float low)
{
    setLevels(low, _levels.high);
}

void Plot2DEchogram::setHightLevel(float high)
{
    setLevels(_levels.low, high);
}

void Plot2DEchogram::setLevels(float low, float high)
{
    _levels.low = low;
    _levels.high = high;
    updateColors();
}

void Plot2DEchogram::setColorScheme(QVector<QColor> coloros, QVector<int> levels)
{
    if(coloros.length() != levels.length()) { return; }

    _colorTable.resize(256);
    _colorLevels.resize(256);

    int nbr_levels = coloros.length() - 1;
    int i_level = 0;

    for(int i = 0; i < nbr_levels; i++) {
        while(levels[i + 1] >= i_level) {
            float b_koef = (float)(i_level - levels[i]) / (float)(levels[i + 1] - levels[i]);
            float a_koef = 1.0f - b_koef;

            int red = qRound(coloros[i].red()*a_koef + coloros[i + 1].red()*b_koef);
            int green = qRound(coloros[i].green()*a_koef + coloros[i + 1].green()*b_koef);
            int blue = qRound(coloros[i].blue()*a_koef + coloros[i + 1].blue()*b_koef);
            _colorHashMap[i_level] = ((red / 8) << 10) | ((green / 8) << 5) | ((blue / 8));

            _colorTable[i_level] = qRgb(red, green, blue);
            i_level++;
        }
    }

    updateColors();
}

int Plot2DEchogram::getThemeId() const
{
    return static_cast<int>(themeId_);
}

void Plot2DEchogram::setThemeId(int theme_id) {
    if (theme_id >= ClassicTheme && theme_id <= MidnightTheme) {
        themeId_ = static_cast<ThemeId>(theme_id);
    }
    else {
        themeId_ = ClassicTheme;
    }

    QVector<QColor> coloros;
    QVector<int> levels;
    colormapFor(static_cast<int>(themeId_), coloros, levels);
    setColorScheme(coloros, levels);
}

void Plot2DEchogram::colormapFor(int theme_id, QVector<QColor>& coloros, QVector<int>& levels) {
    coloros.clear();
    levels.clear();

    if(theme_id == SepiaTheme) {
        coloros = { QColor::fromRgb(0,   0,   0),
                    QColor::fromRgb(50,  50,  10),
                    QColor::fromRgb(230, 200, 100),
                    QColor::fromRgb(255, 255, 220) };
        levels = {0, 30, 130, 255};
    } else if(theme_id == SepiaNewTheme) {
        coloros = {
            QColor::fromRgb(  0,   0,   0),
            QColor::fromRgb( 28,  10,   0),
            QColor::fromRgb( 55,  18,   0),
            QColor::fromRgb( 95,  35,   0),
            QColor::fromRgb(150,  70,  10),
            QColor::fromRgb(210, 105,  15),
            QColor::fromRgb(245, 175,  70),
            QColor::fromRgb(255, 232, 160)
        };
        levels = { 0, 12, 26, 52, 92, 140, 200, 255 };
    }else if(theme_id == WRGBDTheme) {
        coloros = {
            QColor::fromRgb(0, 0, 0),
            QColor::fromRgb(40, 0, 80),
            QColor::fromRgb(0, 30, 150),
            QColor::fromRgb(20, 230, 30),
            QColor::fromRgb(255, 50, 20),
            QColor::fromRgb(255, 255, 255),
        };

        levels = {0, 30, 80, 120, 150, 255};
    } else if(theme_id == WBTheme) {
        coloros = { QColor::fromRgb(0, 0, 0), QColor::fromRgb(190, 200, 200), QColor::fromRgb(230, 255, 255)};
        levels = {0, 150, 255};
    } else if(theme_id == BWTheme) {
        coloros = {QColor::fromRgb(230, 255, 255), QColor::fromRgb(70, 70, 70), QColor::fromRgb(0, 0, 0)};
        levels = {0, 150, 255};
    } else if(theme_id == DeepBlueTheme) {
        coloros = { QColor::fromRgb(0,   0,   40),
                    QColor::fromRgb(20,  0,   120),
                    QColor::fromRgb(40,  0,   200),
                    QColor::fromRgb(0,   90,  255),
                    QColor::fromRgb(0,   200, 255),
                    QColor::fromRgb(0,   255, 200),
                    QColor::fromRgb(0,   255, 80),
                    QColor::fromRgb(220, 255, 0),
                    QColor::fromRgb(255, 120, 0),
                    QColor::fromRgb(255, 40,  40),
                    QColor::fromRgb(255, 255, 255) };
        levels = {0, 18, 36, 60, 90, 120, 150, 185, 210, 235, 255};
    } else if(theme_id == IceTheme) {
        coloros = { QColor::fromRgb(0,   0,   70),
                    QColor::fromRgb(0,   40,  150),
                    QColor::fromRgb(0,   100, 230),
                    QColor::fromRgb(0,   180, 255),
                    QColor::fromRgb(0,   240, 255),
                    QColor::fromRgb(80,  255, 230),
                    QColor::fromRgb(170, 255, 255),
                    QColor::fromRgb(230, 255, 255),
                    QColor::fromRgb(255, 255, 255) };
        levels = {0, 30, 60, 95, 130, 170, 210, 235, 255};
    } else if(theme_id == GreenTheme) {
        coloros = { QColor::fromRgb(0,   0,   0),
                    QColor::fromRgb(0,   50,  0),
                    QColor::fromRgb(0,   100, 0),
                    QColor::fromRgb(0,   160, 0),
                    QColor::fromRgb(0,   220, 0),
                    QColor::fromRgb(0,   255, 0),
                    QColor::fromRgb(120, 255, 0),
                    QColor::fromRgb(200, 255, 0),
                    QColor::fromRgb(255, 255, 80),
                    QColor::fromRgb(255, 255, 255) };
        levels = {0, 20, 45, 70, 100, 130, 160, 190, 220, 255};
    } else if(theme_id == MidnightTheme) {
        coloros = { QColor::fromRgb(51,  128, 255),
                    QColor::fromRgb(51,  102, 230),
                    QColor::fromRgb(77,  77,  204),
                    QColor::fromRgb(102, 51,  179),
                    QColor::fromRgb(128, 51,  153),
                    QColor::fromRgb(153, 77,  128),
                    QColor::fromRgb(179, 102, 102),
                    QColor::fromRgb(204, 128, 77),
                    QColor::fromRgb(230, 153, 51),
                    QColor::fromRgb(255, 179, 26) };
        levels = {0, 28, 56, 84, 112, 140, 168, 196, 224, 255};
    } else {
        coloros = { QColor::fromRgb(0, 0, 0), QColor::fromRgb(20, 5, 80), QColor::fromRgb(50, 180, 230), QColor::fromRgb(190, 240, 250), QColor::fromRgb(255, 255, 255)};
        levels = {0, 30, 130, 220, 255};
    }
}

void Plot2DEchogram::setCompensation(int compensation_id)
{
    _compensation_id = compensation_id;
}

int Plot2DEchogram::getCompensation() const
{
    return _compensation_id;
}

void Plot2DEchogram::setWrapEnabled(bool state)
{
    if (wrapEnabled_ == state) {
        return;
    }

    wrapEnabled_ = state;
    resetCash();
}

void Plot2DEchogram::updateColors()
{
    float low = _levels.low;
    float high = _levels.high;

    int level_range = high - low;
    int index_offset = (int)((float)low*2.5f);
    float index_map_scale = 0;
    if(level_range > 0) {
        index_map_scale = (float)(256 - 1)/((float)(high - low)*2.55f);
    } else {
        index_map_scale = 10000;
    }

    for(int i = 0; i < _colorTable.size(); i++) {
        int index_map = ((float)(i - index_offset)*index_map_scale);
        if(index_map < 0) { index_map = 0; }
        else if(index_map > 255) { index_map = 255; }
        _colorLevels[i] = _colorTable[index_map];
    }

    _flagColorChanged = true;
    _image.setColorTable(_colorLevels);
}

void Plot2DEchogram::resetCash()
{
    _cashFlags.resetCash = true;
}

void Plot2DEchogram::releaseCache()
{
    _cash.clear();
    _cash.squeeze();
    _image = QImage();
    _pixmap = QPixmap();
    _lastCursor = DatasetCursor();
    _lastWidth = -1;
    _lastHeight = -1;
    _flagColorChanged = true;
    _cashFlags.resetCash = true;
    reRenderPlotIndxs_.clear();

    if (miniPreviewPlot_ != nullptr) {
        miniPreviewPlot_->releaseCache();
    }
}

void Plot2DEchogram::addReRenderPlotIndxs(const QSet<int> &indxs)
{
    reRenderPlotIndxs_.unite(indxs);
}

bool Plot2DEchogram::getDeconvEnabled() const { return deconvEnabled_; }

void Plot2DEchogram::setDeconvEnabled(bool state)
{
    if (deconvEnabled_ == state) { return; }
    deconvEnabled_ = state;
    resetCash();
}

float Plot2DEchogram::getDeconvAngularResolution() const { return deconvAngularResolutionDeg_; }

void Plot2DEchogram::setDeconvAngularResolution(float degrees)
{
    const float clamped = qBound(0.1f, degrees, 30.0f);
    if (qFuzzyCompare(deconvAngularResolutionDeg_, clamped)) { return; }
    deconvAngularResolutionDeg_ = clamped;
    if (deconvEnabled_) { resetCash(); }
}

int Plot2DEchogram::getDeconvIterations() const { return deconvIterations_; }

void Plot2DEchogram::setDeconvIterations(int iterations)
{
    const int clamped = qBound(1, iterations, 20);
    if (deconvIterations_ == clamped) { return; }
    deconvIterations_ = clamped;
    if (deconvEnabled_) { resetCash(); }
}

bool Plot2DEchogram::applyDeconvolution(Plot2D* parent, Dataset* dataset, int width, int height, int wrapStartPos)
{
    if (!deconvEnabled_ || !parent || !dataset || width < 3 || height <= 0) {
        return false;
    }

    auto& cursor = parent->cursor();
    const float from = cursor.distance.from;
    const float to   = cursor.distance.to;
    if (!(to > from)) {
        return false;
    }

    // Row -> slant range. The dual-channel column is laid out
    // [far ch1 .. nadir][nadir .. far ch2], so its two halves run in opposite
    // directions and a single linear ramp would be wrong for one of them.
    deconvRange_.resize(height);
    if (cursor.channel2 == channelNone()) {
        for (int row = 0; row < height; ++row) {
            const float d = from + (to - from) * (float(row) + 0.5f) / float(height);
            deconvRange_[row] = std::fabs(d);
        }
    }
    else {
        const float fullrange = to - from;
        const float range1 = (to >= 0.0f) ? (0.0f - from) : (to - from);
        int part1 = int(float(height) * (range1 / fullrange));
        part1 = qBound(0, part1, height);

        const float near1 = 0.0f;
        const float far1  = -from;
        const float near2 = (from >= 0.0f) ? from : 0.0f;
        const float far2  = to;

        for (int row = 0; row < part1; ++row) {
            // Written with reverse: row 0 is the far edge of channel 1.
            const float t = (float(row) + 0.5f) / float(part1);
            deconvRange_[row] = std::fabs(far1 + (near1 - far1) * t);
        }
        const int part2 = height - part1;
        for (int row = part1; row < height; ++row) {
            const float t = (float(row - part1) + 0.5f) / float(part2 > 0 ? part2 : 1);
            deconvRange_[row] = std::fabs(near2 + (far2 - near2) * t);
        }
    }

    // Columns in along-track order, i.e. with the display wrap undone, plus the
    // navigation spacing between consecutive pings.
    deconvScratch_.resize(width * height);
    deconvColumns_.resize(width);
    deconvSpacing_.resize(width);

    auto nedOf = [](Epoch* epoch, double& n, double& e) -> bool {
        if (!epoch) { return false; }
        const Position sonar = epoch->getSonarPosition();
        if (std::isfinite(sonar.ned.n) && std::isfinite(sonar.ned.e)) {
            n = sonar.ned.n; e = sonar.ned.e; return true;
        }
        const Position gnss = epoch->getPositionGNSS();
        if (std::isfinite(gnss.ned.n) && std::isfinite(gnss.ned.e)) {
            n = gnss.ned.n; e = gnss.ned.e; return true;
        }
        return false;
    };

    bool havePrev = false;
    double prevN = 0.0, prevE = 0.0;
    int navCount = 0;

    for (int i = 0; i < width; ++i) {
        int column = i + wrapStartPos;
        if (column >= width) { column -= width; }

        int16_t* dst = deconvScratch_.data() + size_t(i) * size_t(height);
        const auto& src = _cash[column].data;
        if (src.size() == height) {
            std::copy(src.constBegin(), src.constEnd(), dst);
        }
        else {
            std::fill(dst, dst + height, int16_t(0));
        }
        deconvColumns_[i] = dst;

        deconvSpacing_[i] = 0.0f;
        double n = 0.0, e = 0.0;
        const int poolIndex = _cash[column].poolIndex;
        Epoch* epoch = poolIndex >= 0 ? dataset->fromIndex(poolIndex) : nullptr;
        if (nedOf(epoch, n, e)) {
            if (havePrev) {
                const double dn = n - prevN;
                const double de = e - prevE;
                const float step = float(std::sqrt(dn * dn + de * de));
                if (std::isfinite(step) && step > 0.0f) {
                    deconvSpacing_[i] = step;
                    ++navCount;
                }
            }
            prevN = n; prevE = e; havePrev = true;
        }
        else {
            havePrev = false;
        }
    }

    echo_deconv::Params params;
    params.angularResolutionDeg = deconvAngularResolutionDeg_;
    params.iterations = deconvIterations_;

    // With no navigation the kernel would have no metric to be sized against.
    // One metre per ping keeps the control usable on recordings that carry no
    // position - the angle then reads as "footprint in pings", not in metres.
    const float fallbackSpacing = 1.0f;
    const float* spacing = (navCount * 2 >= width) ? deconvSpacing_.constData() : nullptr;

    const echo_deconv::Stats stats = echo_deconv::alongTrack(
        deconvColumns_.data(), width, height,
        deconvRange_.constData(), spacing, fallbackSpacing, params);

    if (stats.rowsProcessed <= 0) {
        return false;
    }

    // Whole image, not just the columns that changed: a deconvolved column
    // depends on its neighbours, so any new ping invalidates the ones near it.
    uint8_t* image_data = _image.bits();
    const int b_scanline = _image.bytesPerLine();
    for (int i = 0; i < width; ++i) {
        int column = i + wrapStartPos;
        if (column >= width) { column -= width; }

        const int16_t* srcCol = deconvColumns_[i];
        uint8_t* img_data = image_data + column;
        for (int row = 0; row < height; ++row) {
            *img_data = uint8_t(srcCol[row]);
            img_data += b_scanline;
        }
    }

    return true;
}

int Plot2DEchogram::updateCash(Plot2D* parent, Dataset* dataset, int width, int height)
{
    auto& cursor = parent->cursor();

    if (_cash.size() != width) {
        _cash.resize(width);
        resetCash();
    }

    uint8_t* image_data = _image.bits();
    const int b_scanline = _image.bytesPerLine();

    bool is_cash_notvalid = getTriggerCashReset();
    is_cash_notvalid |= !_lastCursor.isChannelsEqual(cursor);
    is_cash_notvalid |= !_lastCursor.isDistanceEqual(cursor);
    is_cash_notvalid |=  _lastWidth != width;
    is_cash_notvalid |=  _lastHeight != height;


    float from = cursor.distance.from;
    float to = cursor.distance.to;
    float fullrange = to - from;

    float range1 = 0;
    float from1 = 0;
    float to1 = 0;

    float from2 = 0;
    float to2 = 0;

    if (to >= 0) {
        range1 = 0 - from;
        from1 = 0;
        to1 = -from;

        if (from >= 0) {
            from2 = from;
        }
        else {
            from2 = 0;
        }
        to2 = to;
    }
    else {
        range1 = to - from;
        from1 = -to;
        to1 = -from;
    }

    int wrap_start_pos = 0;
    if (wrapEnabled_) {
        wrap_start_pos = qAbs(cursor.getIndex(0) % width);

        for (unsigned int i = 0; i < cursor.indexes.size(); i++) {
            if (cursor.indexes[i] > 0) {
                wrap_start_pos = qAbs((cursor.indexes[i] + (width - i)) % width);
                break;
            }
        }
    }


//    _cashPosition = wrap_start_pos;
    for(int column = 0; column < width; column++) {
        if(_cash[column].data.size() != height) {
//            _cash[column].stateColor = CashLine::CashState::CashStateNotValid;
            _cash[column].state = CashLine::CashState::CashStateNotValid;
            _cash[column].data.resize(height);
            _cash[column].data.fill(0);
            _cash[column].poolIndex = -1;
            _cash[column].state = CashLine::CashState::CashStateEraced;
            _cash[column].isNeedUpdate = true;

            int16_t cash_data_size = _cash[column].data.size();
            int16_t* cash_data = _cash[column].data.data();
            uint8_t * img_data = image_data + column;
            for (int image_row = 0; image_row < cash_data_size; image_row++) {
                *img_data = *cash_data;
                img_data += b_scanline;
                cash_data++;
            }
        }

        int cursor_pos = column - wrap_start_pos;
        if(column < wrap_start_pos) {
            cursor_pos += width;
        }

        int pool_index = cursor.getIndex(cursor_pos);
        int pool_index_safe = dataset->validIndex(pool_index);
        if(pool_index_safe >= 0) {

            bool wasValidlyRendered = true;
            if (reRenderPlotIndxs_.contains(pool_index_safe)) {
                reRenderPlotIndxs_.remove(pool_index_safe);
                wasValidlyRendered = false;
            }

            auto* datasource = dataset->fromIndex(pool_index_safe);
            const int cash_index = _cash[column].poolIndex;

            if (is_cash_notvalid || pool_index_safe != cash_index || !wasValidlyRendered) {
                _cash[column].poolIndex = pool_index_safe;

                if(datasource != nullptr) {
                    _cash[column].state = CashLine::CashState::CashStateNotValid;
                    int16_t* cash_data = _cash[column].data.data();
                    int16_t cash_data_size = _cash[column].data.size();

                    if (cursor.channel2 == channelNone()) {
                        datasource->chartTo(cursor.channel1, cursor.subChannel1, from, to, cash_data, cash_data_size, _compensation_id);
                    }
                    else {
                        int cash_data_size_part1 = cash_data_size*(range1/fullrange);

                        if(cash_data_size_part1 > 0) {
                            datasource->chartTo(cursor.channel1, cursor.subChannel1, from1, to1, cash_data, cash_data_size_part1, _compensation_id, true);
                        }

                        if(cash_data_size_part1 < 0) {
                            cash_data_size_part1 = 0;
                        }

                        const int cash_data_size_part2 = cash_data_size - cash_data_size_part1;
                        if(cash_data_size_part2 > 0) {
                            datasource->chartTo(cursor.channel2, cursor.subChannel2, from2, to2, &cash_data[cash_data_size_part1], cash_data_size_part2, _compensation_id, false);
                        }
                    }

                    _cash[column].state = CashLine::CashState::CashStateValid;
                    _cash[column].isNeedUpdate = true;
                    uint8_t * img_data = image_data + column;
                    for (int image_row = 0; image_row < cash_data_size; image_row++) {
                        *img_data = *cash_data;
                        img_data += b_scanline;
                        cash_data++;
                    }
//                    _cash[column].stateColor = CashLine::CashState::CashStateNotValid;
                } else {
                    if(is_cash_notvalid || _cash[column].state != CashLine::CashState::CashStateEraced) {
//                        _cash[column].stateColor = CashLine::CashState::CashStateNotValid;
                        _cash[column].state = CashLine::CashState::CashStateNotValid;
                        _cash[column].data.fill(0);
                        _cash[column].poolIndex = -1;
                        _cash[column].state = CashLine::CashState::CashStateEraced;
                        _cash[column].isNeedUpdate = true;

                        int16_t cash_data_size = _cash[column].data.size();
                        int16_t* cash_data = _cash[column].data.data();
                        uint8_t * img_data = image_data + column;
                        for (int image_row = 0; image_row < cash_data_size; image_row++) {
                            *img_data = *cash_data;
                            img_data += b_scanline;
                            cash_data++;
                        }
                    }
                }

            }
        } else {
            if(is_cash_notvalid || _cash[column].state != CashLine::CashState::CashStateEraced) {
//                _cash[column].stateColor = CashLine::CashState::CashStateNotValid;
                _cash[column].state = CashLine::CashState::CashStateNotValid;
                _cash[column].data.fill(0);
                _cash[column].poolIndex = -1;
                _cash[column].state = CashLine::CashState::CashStateEraced;
                _cash[column].isNeedUpdate = true;

                int16_t* cash_data = _cash[column].data.data();
                int16_t cash_data_size = _cash[column].data.size();
                uint8_t * img_data = image_data + column;
                for (int image_row = 0; image_row < cash_data_size; image_row++) {
                    *img_data = *cash_data;
                    img_data += b_scanline;
                    cash_data++;
                }
            }

        }
    }

    //qInfo("Cash validate %u", cash_validate);

    applyDeconvolution(parent, dataset, width, height, wrap_start_pos);

    _lastCursor = cursor;
    _lastWidth = width;
    _lastHeight = height;

    return wrap_start_pos;
}

bool Plot2DEchogram::draw(Plot2D* parent, Dataset* dataset)
{
    auto& canvas = parent->canvas();
    auto& cursor = parent->cursor();

    if (isVisible() && dataset != nullptr && cursor.distance.isValid()) {
        const int image_width = canvas.width();
        const int image_height = canvas.height();

        if(_image.width() != image_width || _image.height() != image_height) {
            _image = QImage(image_width, image_height, QImage::Format_Indexed8);
            _image.setColorTable(_colorLevels);
            _image.fill(0);
            _pixmap = QPixmap(image_width, image_height);
            const QRgb bg = _colorLevels.isEmpty() ? qRgb(0, 0, 0) : _colorLevels.first();
            _pixmap.fill(QColor::fromRgb(bg));
            resetCash();
        }

        const int cash_width = canvas.width();

        const int cash_position = updateCash(parent, dataset, cash_width, image_height);

        QPainter p(&_pixmap);

        int cash_col = 0;
        while(cash_col < cash_width) {
            int cash_col_1 = cash_col;
            while(cash_col < cash_width && (_cash[cash_col].isNeedUpdate || _flagColorChanged)) {
                _cash[cash_col].isNeedUpdate = false;
                cash_col++;
            }

            int cash_update_width = cash_col - cash_col_1;

            if(cash_update_width > 0) {
                 p.drawImage(cash_col_1, 0, _image, cash_col_1, 0 , cash_update_width, 0, Qt::ThresholdDither); // Qt::NoOpaqueDetection |
            } else {
                cash_col++;
            }
        }

        _flagColorChanged = false;

        canvas.painter()->drawPixmap(0, 0, _pixmap, cash_position, 0, cash_width - cash_position, 0);
        canvas.painter()->drawPixmap(cash_width - cash_position, 0, _pixmap, 0, 0, cash_position, 0);
    } else {
        if (dataset == nullptr || !cursor.distance.isValid()) {
            releaseCache();
        }
    }

    return true;
}

bool Plot2DEchogram::drawZoomPreview(Plot2D* parent,
                                     Dataset* dataset,
                                     QPainter* painter,
                                     const QRect& targetRect,
                                     const QPoint& sourceCenter,
                                     int sourceSize,
                                     QPointF* focusPoint)
{
    return drawZoomPreview(parent, dataset, painter, targetRect, sourceCenter, sourceSize, sourceSize, focusPoint);
}

bool Plot2DEchogram::drawZoomPreview(Plot2D* parent,
                                     Dataset* dataset,
                                     QPainter* painter,
                                     const QRect& targetRect,
                                     const QPoint& sourceCenter,
                                     int sourceWidth,
                                     int sourceHeight,
                                     QPointF* focusPoint)
{
    if (focusPoint) {
        *focusPoint = QPointF(0.5, 0.5);
    }

    if (!parent || !dataset || !painter || targetRect.width() <= 0 || targetRect.height() <= 0) {
        return false;
    }

    auto& cursor = parent->cursor();
    auto& canvas = parent->canvas();

    if (!cursor.distance.isValid() || canvas.width() <= 0 || canvas.height() <= 0) {
        return false;
    }

    const int previewWidth = targetRect.width();
    const int previewHeight = targetRect.height();
    if (previewWidth <= 0 || previewHeight <= 0) {
        return false;
    }

    const int srcWidth = qBound(4, sourceWidth, canvas.width());
    const int srcHeight = qBound(4, sourceHeight, canvas.height());

    const int clampedCenterX = qBound(0, sourceCenter.x(), canvas.width() - 1);
    const int clampedCenterY = qBound(0, sourceCenter.y(), canvas.height() - 1);

    const float cursorFrom = cursor.distance.from;
    const float cursorTo = cursor.distance.to;
    const float cursorRange = cursorTo - cursorFrom;
    if (qFuzzyIsNull(cursorRange)) {
        return false;
    }

    const float centerScale = static_cast<float>(clampedCenterY) / static_cast<float>(canvas.height());
    const float centerDistance = cursorFrom + centerScale * cursorRange;
    float distanceSpan = std::abs(cursorRange) * (static_cast<float>(srcHeight) / static_cast<float>(canvas.height()));
    if (distanceSpan < 0.01f) {
        distanceSpan = 0.01f;
    }

    const float minDistance = qMin(cursorFrom, cursorTo);
    const float maxDistance = qMax(cursorFrom, cursorTo);

    float low = centerDistance - distanceSpan * 0.5f;
    float high = centerDistance + distanceSpan * 0.5f;

    if (low < minDistance) {
        const float delta = minDistance - low;
        low += delta;
        high += delta;
    }
    if (high > maxDistance) {
        const float delta = high - maxDistance;
        low -= delta;
        high -= delta;
    }

    low = qBound(minDistance, low, maxDistance);
    high = qBound(minDistance, high, maxDistance);
    if (high <= low) {
        high = qMin(maxDistance, low + 0.01f);
    }

    const bool isAscending = cursorTo >= cursorFrom;
    const float zoomFrom = isAscending ? low : high;
    const float zoomTo = isAscending ? high : low;
    if (focusPoint) {
        const float zoomSpan = zoomTo - zoomFrom;
        float focusY = 0.5f;
        if (std::isfinite(zoomSpan) && std::abs(zoomSpan) > 1e-6f) {
            focusY = (centerDistance - zoomFrom) / zoomSpan;
        }
        focusY = qBound(0.0f, focusY, 1.0f);
        focusPoint->setX(0.5);
        focusPoint->setY(focusY);
    }

    const int sourceLeft = clampedCenterX - srcWidth / 2;
    if (miniPreviewPlot_ == nullptr) {
        miniPreviewPlot_ = new MiniPreviewPlot2D();
    }

    painter->save();
    painter->setClipRect(targetRect);
    painter->translate(targetRect.left(), targetRect.top());

    const bool rendered = miniPreviewPlot_->render(painter,
                                                   dataset,
                                                   parent,
                                                   cursor,
                                                   canvas.width(),
                                                   sourceLeft,
                                                   srcWidth,
                                                   previewWidth,
                                                   previewHeight,
                                                   zoomFrom,
                                                   zoomTo,
                                                   getThemeId(),
                                                   getLowLevel(),
                                                   getHighLevel(),
                                                   _compensation_id);
    painter->restore();

    return rendered;
}

float Plot2DEchogram::getLowLevel() const
{
    return _levels.low;
}

float Plot2DEchogram::getHighLevel() const
{
    return _levels.high;
}
