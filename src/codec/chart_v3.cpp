#include "chart_v3.h"

#include <algorithm>

#include "chart_codec_host.h"

bool ChartV3Assembler::feed(const uint8_t* p, int len, std::vector<Ping>& out)
{
    if (!p || len < CCH_BLOCK_HDR)
        return false;
    Key k;
    k.mode = p[0];
    k.resol = p[1];
    k.count = uint16_t(p[2] | (p[3] << 8));
    k.seq = p[4];
    const int blk = p[5];

    if (havePrev_ && k == prevKey_ && !(active_ && k == key_))
        return true;                          // a late block of a ping already published
    if (active_ && !(k == key_))
        finish(out);                          // another ping started: this one is as whole as it gets

    if (!active_) {
        cch_band b[CCH_MAXBANDS];
        uint16_t n = 0;
        const int nb = cch_bands(k.mode, k.resol, k.count, b, CCH_MAXBANDS, &n);
        if (nb <= 0)
            return false;
        bands_.clear();
        for (int i = 0; i < nb; ++i)
            bands_.push_back({ b[i].resol_mm, b[i].count, b[i].start_mm });
        bins_.assign(n, 0);
        have_.assign((n + CCH_BLOCK_BINS - 1) / CCH_BLOCK_BINS, false);
        key_ = k;
        active_ = true;
    }

    const int start = blk * CCH_BLOCK_BINS;
    const int blen = std::min<int>(CCH_BLOCK_BINS, int(bins_.size()) - start);
    if (blk >= int(have_.size()) || blen <= 0)
        return false;                         // a block past the ping's end
    if (cch_decode_block(p + CCH_BLOCK_HDR, len - CCH_BLOCK_HDR, uint16_t(blen), bins_.data() + start) < 0) {
        std::fill(bins_.begin() + start, bins_.begin() + start + blen, uint8_t(0));
        have_[blk] = false;
        return false;
    }
    have_[blk] = true;
    if (std::all_of(have_.begin(), have_.end(), [](bool h) { return h; }))
        finish(out);
    return true;
}

void ChartV3Assembler::flush(std::vector<Ping>& out)
{
    if (active_)
        finish(out);
}

void ChartV3Assembler::finish(std::vector<Ping>& out)
{
    Ping ping;
    ping.mode = key_.mode;
    ping.seq = key_.seq;
    ping.resolMm = key_.resol;
    ping.blocks = int(have_.size());
    const bool fill = havePrev_ && key_.sameGeometry(prevKey_) && prevBins_.size() == bins_.size();
    for (int b = 0; b < int(have_.size()); ++b) {
        if (have_[b])
            continue;
        ++ping.missingBlocks;
        if (fill) {
            const int s = b * CCH_BLOCK_BINS;
            const int e = std::min<int>(s + CCH_BLOCK_BINS, int(bins_.size()));
            std::copy(prevBins_.begin() + s, prevBins_.begin() + e, bins_.begin() + s);
        }
    }
    ping.bins = bins_;
    ping.cells = toNative(bins_);
    out.push_back(std::move(ping));

    havePrev_ = true;
    prevKey_ = key_;
    prevBins_ = bins_;
    active_ = false;
}

std::vector<uint8_t> ChartV3Assembler::toNative(const std::vector<uint8_t>& bins) const
{
    if (bands_.size() <= 1)
        return bins;                          // uniform: the bins are the native cells
    std::vector<uint8_t> cells(key_.count, 0);
    size_t band = 0, first = 0;              // first: index in bins of the band's first bin
    for (uint32_t i = 0; i < key_.count; ++i) {
        const uint32_t mid = i * key_.resol + key_.resol / 2;
        while (band + 1 < bands_.size() && mid >= bands_[band + 1].startMm) {
            first += bands_[band].count;
            ++band;
        }
        const Band& b = bands_[band];
        uint32_t j = mid >= b.startMm ? (mid - b.startMm) / b.resol : 0;
        if (j >= b.count)
            j = b.count - 1u;
        cells[i] = bins[first + j];
    }
    return cells;
}
