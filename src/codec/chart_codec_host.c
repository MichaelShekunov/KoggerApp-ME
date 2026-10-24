#include "chart_codec_host.h"
#include "chart_codec.h"

/* Built with -ffp-contract=off (CMakeLists.txt): the decoder is float, and a fused multiply-add where
 * the firmware's reference build has none would round some cells one count apart. */

_Static_assert(CCH_BLOCK_HDR == CHART_BLOCK_HDR, "block header size");
_Static_assert(CCH_BLOCK_BINS == CHART_BLOCK_BINS, "bins per block");
_Static_assert(CCH_MAXBANDS == CC_MAXBANDS, "band table size");
_Static_assert(CCH_MAXN == CC_MAXN, "longest chart");

int cch_bands(uint8_t mode, uint16_t resol_mm, uint16_t count, cch_band* bands, int max, uint16_t* n)
{
    cc_band_t b[CC_MAXBANDS];
    uint16_t total = 0;
    if (mode > CHART_MODE_PROGRESSIVE || max <= 0)
        return 0;
    const uint8_t nb = chart_codec_bands((chart_codec_mode_t)mode, resol_mm, count, b, CC_MAXBANDS, &total);
    if (nb == 0 || nb > max || total == 0 || total > CC_MAXN)
        return 0;
    /* Where each band starts: the ladder's own edges (chart_bands_for_mode), not the running sum of
     * count * resol, which falls short by up to a cell per band where an edge is not a whole cell. */
    uint32_t lo = 0;
    const uint32_t total_mm = (uint32_t)resol_mm * count;
    for (int i = 0, k = 0; i < CHART_LADDER_BANDS && k < nb; ++i) {
        if (mode == CHART_MODE_UNIFORM) {
            bands[0].start_mm = 0;
            break;
        }
        const uint32_t edge = (i == CHART_LADDER_BANDS - 1) ? total_mm : (uint32_t)CHART_LADDER_EDGE_MM[i];
        const uint32_t hi = edge < total_mm ? edge : total_mm;
        if (hi <= lo)
            continue;
        bands[k++].start_mm = lo;
        lo = edge;
    }
    for (int k = 0; k < nb; ++k) {
        bands[k].resol_mm = b[k].resol_mm;
        bands[k].count = b[k].count;
    }
    if (n)
        *n = total;
    return nb;
}

int cch_decode_block(const uint8_t* bits, int nbytes, uint16_t blen, uint8_t* out)
{
    float scratch[(CHART_BLOCK_BINS + CC_HOP - 1) / CC_HOP * CC_HOP + CC_BLK];
    if (blen == 0 || blen > CHART_BLOCK_BINS || nbytes < 0)
        return -1;
    const chart_codec_cfg_t cfg = chart_codec_default_cfg();
    return chart_decode(bits, nbytes, blen, &cfg, out, scratch) < 0 ? -1 : 0;
}
