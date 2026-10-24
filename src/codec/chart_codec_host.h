#pragma once

/* The host side of echocodec (chart_codec.h, GitLab kogger/research/echocodec @ e6a02dd): the range
 * blocks SonarN sends in ID_CHART V3 (docs/ECHO_CODECS.md). chart_codec.h is plain C with static
 * inline functions; this wrapper compiles it once, as C, and gives C++ a small fixed API. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CCH_BLOCK_HDR   6     /* [mode U1][resol_mm U1][count U2][seq U1][blk U1] */
#define CCH_BLOCK_BINS  252   /* bins per block; the last block of a ping may be shorter */
#define CCH_MAXBANDS    6
#define CCH_MAXN        12288 /* longest chart the codec takes */

/** One range band of a chart: its bins are @p count cells of @p resol_mm each, the first at @p start_mm. */
typedef struct {
    uint16_t resol_mm;
    uint16_t count;
    uint32_t start_mm;
} cch_band;

/**
 * The band table of a block header's geometry, as the firmware built it: uniform mode is one band of
 * the native geometry, progressive mode the codec's ladder sized to the native range.
 * @return the number of bands (at most @p max), 0 when the geometry is not valid; @p n gets the chart
 *         length, the sum of the band counts.
 */
int cch_bands(uint8_t mode, uint16_t resol_mm, uint16_t count, cch_band* bands, int max, uint16_t* n);

/**
 * Decode one block's bitstream (the payload after its 6-byte header) into @p out[0 .. blen).
 * @return 0, or -1 when the stream is malformed.
 */
int cch_decode_block(const uint8_t* bits, int nbytes, uint16_t blen, uint8_t* out);

#ifdef __cplusplus
}
#endif
