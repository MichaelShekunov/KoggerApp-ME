#pragma once

#include <cstdint>
#include <vector>

/**
 * @brief Pings of echocodec (ID_CHART V3, as SonarN sends them) put back together from their range
 * blocks (docs/ECHO_CODECS.md).
 *
 * A frame is one independently decodable block of a ping:
 * [mode U1][resol_mm U1][count U2][seq U1][blk U1][bitstream]. The blocks of one ping share mode,
 * resolution, count and seq, and come in any order. A ping is published when all its blocks are in,
 * or when a frame of another ping comes first; then each missing block is taken from the previous
 * published ping if that one had the same geometry (SonarN's design), else left at 0.
 *
 * A published ping is on the native grid -- count cells of resol_mm from range 0 -- so nothing
 * downstream sees the codec's bands: a progressive ping (10/15/20/30 mm bands) is mapped back cell by
 * cell, each cell taking the band bin its middle falls in.
 */
class ChartV3Assembler
{
public:
    struct Ping {
        std::vector<uint8_t> bins;    ///< as decoded: the bands end to end (uniform: the native cells)
        std::vector<uint8_t> cells;   ///< on the native grid: count cells of resolMm
        uint16_t resolMm = 0;
        uint8_t mode = 0;             ///< 0 uniform, 1 progressive
        uint8_t seq = 0;
        int blocks = 0;               ///< blocks in the ping
        int missingBlocks = 0;        ///< not received: from the previous ping, or 0
    };

    /** One frame's payload. Appends the pings it ends to @p out (0, 1 or 2). False: the frame is not
     *  a valid block (bad geometry, block outside the ping, a stream that does not decode). */
    bool feed(const uint8_t* payload, int len, std::vector<Ping>& out);

    /** Publish the ping being collected, if any (its missing blocks filled as above). */
    void flush(std::vector<Ping>& out);

private:
    struct Key {
        uint8_t mode = 0, resol = 0, seq = 0;
        uint16_t count = 0;
        bool sameGeometry(const Key& o) const { return mode == o.mode && resol == o.resol && count == o.count; }
        bool operator==(const Key& o) const { return sameGeometry(o) && seq == o.seq; }
    };
    struct Band { uint16_t resol = 0, count = 0; uint32_t startMm = 0; };

    void finish(std::vector<Ping>& out);
    std::vector<uint8_t> toNative(const std::vector<uint8_t>& bins) const;

    bool active_ = false;
    Key key_;
    std::vector<Band> bands_;
    std::vector<uint8_t> bins_;
    std::vector<bool> have_;

    bool havePrev_ = false;
    Key prevKey_;                     ///< the last published ping: its geometry fills, its seq is done
    std::vector<uint8_t> prevBins_;
};
