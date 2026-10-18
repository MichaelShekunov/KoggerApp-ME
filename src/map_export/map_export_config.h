#pragma once

// The view's mosaic and surface settings as the data processor last passed them on, for a map
// export to paint and colour the same way (DataProcessor::mapExportConfig, docs/map-export.md).

#include <cstdint>
#include <vector>
#include <QMetaType>
#include "dataset_defs.h"

struct MapExportConfig {
    ChannelId ch1;
    uint8_t sub1 = 0;
    ChannelId ch2;
    uint8_t sub2 = 0;
    int mosaicSource = 1;                    // MosaicProcessor::Source; its default is SideScan
    float lAngleOffset = 0.0f;
    float rAngleOffset = 0.0f;
    int mosaicTheme = 0;
    float mosaicLow = 0.0f;
    float mosaicHigh = 0.0f;
    std::vector<uint8_t> mosaicColorTable;   // 256 x RGBA, the table the view's shader looks up
    int surfaceTheme = 0;                    // the depth colours (SurfaceProcessor's default until set)
};
Q_DECLARE_METATYPE(MapExportConfig)
