/* chart_codec.h - echogram chart codec (MDCT-12, faithful-floor + sparse features + Rice).
 *
 * Compresses the uint8 log-amplitude chart (0.4 dB/count, post-resampler) for the
 * bandwidth-limited radio link. Design (see Research/EchoCompr/docs/codec_findings.md):
 *
 *   chart -> MDCT-12 (2N=24 block, 50% overlap, sine window, DCT-IV, TDAC) per frame
 *     TWO RESOLUTIONS, selected per frame by a RELATIVE detector:
 *       FLOOR frame   : keep coeffs 0..m-1      (m=6  -> faithful DC + low/mid, drop the rest)
 *       FEATURE frame : keep coeffs 0..m_feat-1 (m_feat=12 -> full bandwidth, uniform quant)
 *     coeff 0 (DC)         : delta vs reconstructed prev frame (finer step on feature frames)
 *     coeffs 1..mc-1       : uniform scalar quant (NO dead-zone); mc = m or m_feat by frame mode
 *     feature flag         : run-length header (Rice runs), not 1 bit/frame -> long floor runs
 *     entropy              : Rice/Golomb (limited-length, with escape), per-class k
 *
 * Detector = TWO-BOX (running-sum) "wall": feature iff any sample's prominence
 *   mean(target box) - mean(base box) exceeds box_thr.  RELATIVE -> offset-invariant, so it is
 *   GAIN-ROBUST in the log-amplitude domain (a constant gain shift cancels in the difference);
 *   O(1)/sample (two incremental sums, no sort/histogram). Base box is LARGE (water-floor ref),
 *   target box small (the peak). Compared with integer cross-multiply -> exact, libm-free.
 *
 * Why this design (see codec_findings.md): the floor is incompressible white noise -> keep its
 * low/mid faithfully and drop only its high texture; features get full bandwidth. Uniform coding
 * BEATS the old dead-zone high band (mask+sign overhead > plain Rice). An ABSOLUTE amplitude
 * classifier was dropped: it collapses under gain changes; the relative two-box is invariant.
 *
 * Float math (FPU) with const tables -> NO libm at runtime (libm is linker-discarded on
 * target). Encoder is allocation-free and MCU-light (~52 cyc/bin, <3% CPU worst case).
 * The decoder runs host/app-side and needs a float scratch accumulator.
 *
 * Pure C, no MCU/Config deps -> shared verbatim by the firmware and the host harness, so the
 * Python sandbox (echo_sandbox.py) and this implementation can be cross-checked bit-for-bit.
 *
 * Bitstream = [flag RLE header][per-frame DC + AC coeffs]. The header needs the whole chart's
 * classification first (one cheap no-MDCT pass), then frame data is sequential. Frame 0's DC is
 * absolute (per-packet keyframe), so each encoded chart is independently decodable (UART/radio
 * loss resync). One chart == one ping, so the header-first pass is not a streaming cost.
 *
 *   chart_codec_cfg_t cfg = chart_codec_default_cfg();
 *   int nbytes = chart_encode(chart, n, &cfg, out, out_cap);          // MCU
 *   chart_decode(out, nbytes, n, &cfg, chart_rec, scratch);  // scratch = chart_codec_scratch_floats(n)
 */
#ifndef CHART_CODEC_H_
#define CHART_CODEC_H_
#include <stdint.h>

#define CC_N    12          /* MDCT length / coeffs per frame                 */
#define CC_H    6           /* N/2                                            */
#define CC_BLK  24          /* 2N block                                       */
#define CC_HOP  12          /* hop = N (50% overlap)                          */
#define CC_RICE_ESC 24      /* limited-length Rice: >=ESC unary -> 32-bit raw */
#define CC_MAXN     12288   /* max chart length (SonarN ping <=4096; higher for multi-ping test) */
#define CC_MAXFRAMES ((CC_MAXN / CC_HOP) + 2)   /* encoder feature-flag scratch, ~1 KB stack    */

/* ---- libm-free helpers (map to VABS/VCVT on M4F, no library calls) ---- */
#define CC_ROUND(x)  ((int32_t)((x) >= 0.0f ? (x) + 0.5f : (x) - 0.5f))
#define CC_ABSF(x)   ((x) < 0.0f ? -(x) : (x))
#define CC_ABSI(x)   ((x) < 0   ? -(x) : (x))

/* =========================== config =========================== */
typedef struct {
    uint8_t m;           /* FLOOR frames keep coeffs 0..m-1            (faithful DC + low/mid)  */
    uint8_t m_feat;      /* FEATURE frames keep coeffs 0..m_feat-1     (full bandwidth, uniform)*/
    float   step_dc,   step_lm;     /* floor-frame DC + AC quant steps                         */
    float   step_dc_f, step_lm_f;   /* feature-frame DC + AC quant steps                       */
    uint16_t box_t, box_b;          /* two-box detector window sizes (target, base) in samples */
    int16_t  box_thr;               /* feature prominence threshold (counts)                   */
    uint8_t  k_dc, k_lm, k_dc_f, k_lm_f, k_flag;  /* Rice parameters per class                 */
} chart_codec_cfg_t;

/* Converged operating point (see codec_findings.md, progressive_resolution.md): floor m=6 +
 * feature m_feat=12 (full band, uniform), two-box RELATIVE detector. On the PROGRESSIVE chart
 * (~1067 bins/ping, 10/15/20/30 mm ladder): ~5.2x, feat ~1.1 dB, floorDC ~1.1 dB, gain-robust.
 * box_b is the water-floor reference window in BINS; scale it ~n/11 with the chart length
 * (96 suits ~1000-bin progressive pings; the old 10980-bin native validation used 256). */
static inline chart_codec_cfg_t chart_codec_default_cfg(void) {
    chart_codec_cfg_t c;
    c.m = 6; c.m_feat = 12;
    c.step_dc = 22.0f; c.step_lm = 24.0f;
    c.step_dc_f = 12.0f; c.step_lm_f = 24.0f;
    c.box_t = 3; c.box_b = 96; c.box_thr = 28;
    c.k_dc = 2; c.k_lm = 0; c.k_dc_f = 5; c.k_lm_f = 1; c.k_flag = 3;
    return c;
}

/* ===================== progressive band table ===================== */
/* The chart is a concatenation of range bands at increasing mm/bin (range-dependent
 * resolution; see Research/EchoCompr/docs/progressive_resolution.md). The codec is
 * resolution-AGNOSTIC (it compresses bins); this small table travels in the packet so the
 * host maps each reconstructed bin back to physical range. Packet = [band table][codec
 * bitstream]. Sum of band counts == n passed to chart_encode/decode. The resampler
 * (resample.h rsProg / resampleLogBands) produces the bands; default ladder below. */
#define CC_MAXBANDS 6
typedef struct { uint16_t resol_mm; uint16_t count; } cc_band_t;

/* Range-scaling mode. The codec (encode/decode) is resolution-agnostic; the mode selects how the
 * chart is RESAMPLED before encoding and is captured entirely by the band table in the packet:
 *   UNIFORM     -> 1 band at native resolution (best fidelity, most bins/bandwidth).
 *   PROGRESSIVE -> the CHART_LADDER_* ladder (coarser with range -> strong bandwidth lever).
 * The decoder needs only the band table (no mode byte), so uniform and progressive share one path. */
typedef enum { CHART_MODE_UNIFORM = 0, CHART_MODE_PROGRESSIVE = 1 } chart_codec_mode_t;

/* Progressive ladder: band b spans up to CHART_LADDER_EDGE_MM[b] (one-way range, mm) at
 * CHART_LADDER_RESOL_MM[b] mm/bin. Last edge is the sentinel "to end of chart". Retune here. */
#define CHART_LADDER_BANDS 4
static const uint16_t CHART_LADDER_EDGE_MM[CHART_LADDER_BANDS]  = { 3000, 6000, 12000, 65535 };
static const uint16_t CHART_LADDER_RESOL_MM[CHART_LADDER_BANDS] = {   10,   15,    20,    30 };

/* Default 10/15/20/30 mm ladder over 0-3/3-6/6-12/12-20 m -> 1067 bins (legacy fixed-20 m table;
 * prefer chart_bands_for_mode() which sizes the ladder to the actual chart). */
static inline uint8_t chart_default_bands(cc_band_t *b) {
    b[0].resol_mm = 10; b[0].count = 300;   /* 0-3 m   */
    b[1].resol_mm = 15; b[1].count = 200;   /* 3-6 m   */
    b[2].resol_mm = 20; b[2].count = 300;   /* 6-12 m  */
    b[3].resol_mm = 30; b[3].count = 267;   /* 12-20 m */
    return 4;
}

/* Serialize: [nbands u8][resol_mm u8, count u16 BE] * nbands. Returns bytes, -1 on overflow. */
static inline int cc_bands_write(const cc_band_t *bands, uint8_t nbands, uint8_t *out, int out_cap) {
    int need = 1 + 3 * (int)nbands, b;
    if (nbands > CC_MAXBANDS || need > out_cap) return -1;
    out[0] = nbands;
    for (b = 0; b < nbands; ++b) {
        out[1 + 3 * b] = (uint8_t)bands[b].resol_mm;
        out[2 + 3 * b] = (uint8_t)(bands[b].count >> 8);
        out[3 + 3 * b] = (uint8_t)(bands[b].count & 0xFF);
    }
    return need;
}
/* Deserialize. Returns bytes consumed, -1 on truncation/overflow. Sets *nbands, *n_total. */
static inline int cc_bands_read(const uint8_t *buf, int nbytes, cc_band_t *bands, uint8_t max,
                                uint8_t *nbands, uint16_t *n_total) {
    int b; uint16_t tot = 0; uint8_t nb;
    if (nbytes < 1) return -1;
    nb = buf[0];
    if (nb > max || 1 + 3 * (int)nb > nbytes) return -1;
    for (b = 0; b < nb; ++b) {
        bands[b].resol_mm = buf[1 + 3 * b];
        bands[b].count = ((uint16_t)buf[2 + 3 * b] << 8) | buf[3 + 3 * b];
        tot += bands[b].count;
    }
    *nbands = nb; if (n_total) *n_total = tot;
    return 1 + 3 * (int)nb;
}

/* Build the band table for a mode, given the NATIVE chart geometry (native_resol_mm mm/bin,
 * native_count bins). UNIFORM -> one native band. PROGRESSIVE -> the ladder, sized to the chart's
 * actual range and never finer than native (a coarse native chart clamps the fine bands up).
 * Returns nbands (<= max, <= CHART_LADDER_BANDS); sum of band counts = the resampled length that
 * the resampler must produce and chart_encode receives. 0 on bad input. */
static inline uint8_t chart_bands_for_mode(chart_codec_mode_t mode, cc_band_t *bands, uint8_t max,
                                           uint16_t native_resol_mm, uint16_t native_count) {
    if (native_resol_mm == 0u || native_count == 0u || max == 0u) return 0;
    if (mode == CHART_MODE_UNIFORM) {
        bands[0].resol_mm = native_resol_mm;
        bands[0].count = native_count;
        return 1;
    }
    uint32_t total_mm = (uint32_t)native_resol_mm * native_count;
    uint8_t nb = 0;
    uint32_t lo = 0;
    for (int i = 0; i < CHART_LADDER_BANDS && nb < max && lo < total_mm; ++i) {
        uint32_t edge = (i == CHART_LADDER_BANDS - 1) ? total_mm : (uint32_t)CHART_LADDER_EDGE_MM[i];
        uint32_t hi = (edge < total_mm) ? edge : total_mm;
        if (hi <= lo) continue;                     /* band range already beyond the chart */
        uint16_t resol = CHART_LADDER_RESOL_MM[i] < native_resol_mm ? native_resol_mm : CHART_LADDER_RESOL_MM[i];
        uint16_t count = (uint16_t)((hi - lo) / resol);
        if (count == 0u) count = 1u;
        bands[nb].resol_mm = resol;
        bands[nb].count = count;
        ++nb;
        lo = edge;
    }
    return nb;
}

/* ===================== radio-durable block transport ===================== */
/* For the lossy radio link the chart is split into independently-decodable range BLOCKS of
 * CHART_BLOCK_BINS bins. Each block is a self-contained chart_encode() of its bin-slice
 * (absolute DC, own flag-RLE, byte-aligned) and ships in ONE radio frame:
 *     payload = [mode:U1][resol_mm:U1][count:U2][seq:U1][blk:U1][ block bitstream ]  (<= RADIO_PAYLOAD_MAX)
 * The header is SELF-DESCRIBING: mode (uniform/progressive) + the FLEXIBLE native geometry
 * (resol_mm, count) let the host derive the band table (chart_bands_for_mode) and block layout
 * with no cached config -> a lost config/first frame never desyncs geometry. The band-table
 * SHAPE (the ladder) is predefined (CHART_LADDER_*); only the mode is a few-bit selector, the
 * distance stays a free value. A lost frame loses only its block's range (host fills from the
 * previous ping). 21 frames (252 bins) keeps even a noise block's FLOOR-fallback under 115 B. */
#define CHART_BLOCK_BINS        252   /* 21 MDCT frames; noise floor-fallback ~104 B + 6 hdr = 110 B <=115 */
#define CHART_RADIO_PAYLOAD_MAX 115   /* max KP payload on the radio link (bytes) */
#define CHART_BLOCK_HDR         6     /* [mode U1][resol_mm U1][count U2][seq U1][blk U1] */

static inline uint16_t chart_block_count(uint16_t n) {
    return (uint16_t)((n + CHART_BLOCK_BINS - 1u) / CHART_BLOCK_BINS);
}

/* Band table + concatenated length for (mode, native geometry). Returns nbands (0 on bad input).
 * The single source of truth for both firmware encode and host decode. */
static inline uint8_t chart_codec_bands(chart_codec_mode_t mode, uint16_t resol_mm, uint16_t count,
                                        cc_band_t *bands, uint8_t max, uint16_t *n_out) {
    uint8_t nb = chart_bands_for_mode(mode, bands, max, resol_mm, count);
    uint16_t n = 0; for (uint8_t b = 0; b < nb; ++b) n += bands[b].count;
    if (n_out) *n_out = n;
    return nb;
}

/* Generous output-buffer bound; encode() still returns -1 if it would overflow. */
static inline int chart_codec_max_bytes(uint16_t n) { return (int)n + (n >> 1) + 32; }

/* Decoder scratch size in FLOATS. The OLA accumulator spans F*CC_HOP + CC_BLK, which is
 * LARGER than n+2*CC_N whenever n is not a multiple of CC_HOP (e.g. a 4096 or 1067 ping).
 * Always size the scratch with this, never n+2*CC_N. */
static inline int chart_codec_scratch_floats(uint16_t n) {
    int F = (n + CC_HOP - 1) / CC_HOP;
    return F * CC_HOP + CC_BLK;
}

/* ===================== const transform tables ===================== */
/* CC_W[n]   = sin(pi/(2N) * (n+0.5))                  (sine window, 2N)            */
/* CC_A[k][n]= cos(pi/N * (k+0.5)(n+0.5))              (DCT-IV kernel, symmetric)   */
static const float CC_W[24] = {
    +6.540312923e-02f, +1.950903220e-01f, +3.214394653e-01f, +4.422886902e-01f, +5.555702330e-01f, +6.593458151e-01f,
    +7.518398075e-01f, +8.314696123e-01f, +8.968727415e-01f, +9.469301295e-01f, +9.807852804e-01f, +9.978589232e-01f,
    +9.978589232e-01f, +9.807852804e-01f, +9.469301295e-01f, +8.968727415e-01f, +8.314696123e-01f, +7.518398075e-01f,
    +6.593458151e-01f, +5.555702330e-01f, +4.422886902e-01f, +3.214394653e-01f, +1.950903220e-01f, +6.540312923e-02f,
};
static const float CC_A[12][12] = {
    {+9.978589232e-01f, +9.807852804e-01f, +9.469301295e-01f, +8.968727415e-01f, +8.314696123e-01f, +7.518398075e-01f, +6.593458151e-01f, +5.555702330e-01f, +4.422886902e-01f, +3.214394653e-01f, +1.950903220e-01f, +6.540312923e-02f},
    {+9.807852804e-01f, +8.314696123e-01f, +5.555702330e-01f, +1.950903220e-01f, -1.950903220e-01f, -5.555702330e-01f, -8.314696123e-01f, -9.807852804e-01f, -9.807852804e-01f, -8.314696123e-01f, -5.555702330e-01f, -1.950903220e-01f},
    {+9.469301295e-01f, +5.555702330e-01f, -6.540312923e-02f, -6.593458151e-01f, -9.807852804e-01f, -8.968727415e-01f, -4.422886902e-01f, +1.950903220e-01f, +7.518398075e-01f, +9.978589232e-01f, +8.314696123e-01f, +3.214394653e-01f},
    {+8.968727415e-01f, +1.950903220e-01f, -6.593458151e-01f, -9.978589232e-01f, -5.555702330e-01f, +3.214394653e-01f, +9.469301295e-01f, +8.314696123e-01f, +6.540312923e-02f, -7.518398075e-01f, -9.807852804e-01f, -4.422886902e-01f},
    {+8.314696123e-01f, -1.950903220e-01f, -9.807852804e-01f, -5.555702330e-01f, +5.555702330e-01f, +9.807852804e-01f, +1.950903220e-01f, -8.314696123e-01f, -8.314696123e-01f, +1.950903220e-01f, +9.807852804e-01f, +5.555702330e-01f},
    {+7.518398075e-01f, -5.555702330e-01f, -8.968727415e-01f, +3.214394653e-01f, +9.807852804e-01f, -6.540312923e-02f, -9.978589232e-01f, -1.950903220e-01f, +9.469301295e-01f, +4.422886902e-01f, -8.314696123e-01f, -6.593458151e-01f},
    {+6.593458151e-01f, -8.314696123e-01f, -4.422886902e-01f, +9.469301295e-01f, +1.950903220e-01f, -9.978589232e-01f, +6.540312923e-02f, +9.807852804e-01f, -3.214394653e-01f, -8.968727415e-01f, +5.555702330e-01f, +7.518398075e-01f},
    {+5.555702330e-01f, -9.807852804e-01f, +1.950903220e-01f, +8.314696123e-01f, -8.314696123e-01f, -1.950903220e-01f, +9.807852804e-01f, -5.555702330e-01f, -5.555702330e-01f, +9.807852804e-01f, -1.950903220e-01f, -8.314696123e-01f},
    {+4.422886902e-01f, -9.807852804e-01f, +7.518398075e-01f, +6.540312923e-02f, -8.314696123e-01f, +9.469301295e-01f, -3.214394653e-01f, -5.555702330e-01f, +9.978589232e-01f, -6.593458151e-01f, -1.950903220e-01f, +8.968727415e-01f},
    {+3.214394653e-01f, -8.314696123e-01f, +9.978589232e-01f, -7.518398075e-01f, +1.950903220e-01f, +4.422886902e-01f, -8.968727415e-01f, +9.807852804e-01f, -6.593458151e-01f, +6.540312923e-02f, +5.555702330e-01f, -9.469301295e-01f},
    {+1.950903220e-01f, -5.555702330e-01f, +8.314696123e-01f, -9.807852804e-01f, +9.807852804e-01f, -8.314696123e-01f, +5.555702330e-01f, -1.950903220e-01f, -1.950903220e-01f, +5.555702330e-01f, -8.314696123e-01f, +9.807852804e-01f},
    {+6.540312923e-02f, -1.950903220e-01f, +3.214394653e-01f, -4.422886902e-01f, +5.555702330e-01f, -6.593458151e-01f, +7.518398075e-01f, -8.314696123e-01f, +8.968727415e-01f, -9.469301295e-01f, +9.807852804e-01f, -9.978589232e-01f},
};

/* ===================== MDCT-12 (analysis / synthesis) ===================== */
/* Forward: window -> TDAC fold -> DCT-IV.  block[24] centered samples -> X[12]. */
static inline void cc_mdct(const float *block, float *X) {
    float u[CC_BLK], y[CC_N];
    int i, k, nn;
    for (i = 0; i < CC_BLK; ++i) u[i] = block[i] * CC_W[i];
    for (i = 0; i < CC_H; ++i) y[i]        = -u[CC_N + (CC_H - 1 - i)] - u[CC_N + CC_H + i];
    for (i = 0; i < CC_H; ++i) y[CC_H + i] =  u[i]                     - u[CC_H + (CC_H - 1 - i)];
    for (k = 0; k < CC_N; ++k) {
        float acc = 0.0f;
        const float *Ak = CC_A[k];
        for (nn = 0; nn < CC_N; ++nn) acc += Ak[nn] * y[nn];
        X[k] = acc;
    }
}

/* Inverse: IDCT-IV (2/N * A^T, A symmetric) -> slice map -> window.  X[12] -> v[24]. */
static inline void cc_imdct(const float *X, float *v) {
    float yv[CC_N];
    int i, nn, k;
    for (nn = 0; nn < CC_N; ++nn) {
        float acc = 0.0f;
        const float *An = CC_A[nn];            /* A symmetric -> A^T row nn == A row nn */
        for (k = 0; k < CC_N; ++k) acc += An[k] * X[k];
        yv[nn] = (2.0f / (float)CC_N) * acc;
    }
    for (i = 0; i < CC_H; ++i) v[i]            =  yv[CC_H + i];
    for (i = 0; i < CC_H; ++i) v[CC_H + i]     = -yv[CC_N - 1 - i];
    for (i = 0; i < CC_H; ++i) v[CC_N + i]     = -yv[CC_H - 1 - i];
    for (i = 0; i < CC_H; ++i) v[CC_N + CC_H + i] = -yv[i];
    for (i = 0; i < CC_BLK; ++i) v[i] *= CC_W[i];
}

/* ===================== bit I/O (MSB-first, no pre-zeroing) ===================== */
typedef struct { uint8_t *buf; int cap; int byte; uint32_t acc; int nbits; int err; } cc_bw;
typedef struct { const uint8_t *buf; int nbytes; int byte; uint32_t acc; int nbits; int err; } cc_br;

static inline void cc_bw_bit(cc_bw *w, int b) {
    w->acc = (w->acc << 1) | (uint32_t)(b & 1); w->nbits++;
    if (w->nbits == 8) {
        if (w->byte < w->cap) w->buf[w->byte++] = (uint8_t)w->acc; else w->err = 1;
        w->acc = 0; w->nbits = 0;
    }
}
static inline void cc_bw_bits(cc_bw *w, uint32_t v, int n) { int i; for (i = n - 1; i >= 0; --i) cc_bw_bit(w, (v >> i) & 1); }
static inline int  cc_bw_finish(cc_bw *w) {
    if (w->nbits > 0) {
        w->acc <<= (8 - w->nbits);
        if (w->byte < w->cap) w->buf[w->byte++] = (uint8_t)w->acc; else w->err = 1;
        w->acc = 0; w->nbits = 0;
    }
    return w->err ? -1 : w->byte;
}
static inline int cc_br_bit(cc_br *r) {
    if (r->nbits == 0) {
        if (r->byte < r->nbytes) { r->acc = r->buf[r->byte++]; r->nbits = 8; }
        else { r->err = 1; return 0; }
    }
    r->nbits--; return (int)((r->acc >> r->nbits) & 1u);
}
static inline uint32_t cc_br_bits(cc_br *r, int n) { uint32_t v = 0; int i; for (i = 0; i < n; ++i) v = (v << 1) | (uint32_t)cc_br_bit(r); return v; }

/* limited-length Rice (unsigned), escape to 32-bit raw on long runs */
static inline void cc_rice_put_u(cc_bw *w, uint32_t u, int k) {
    uint32_t q = u >> k, i;
    if (q < (uint32_t)CC_RICE_ESC) {
        for (i = 0; i < q; ++i) cc_bw_bit(w, 1);
        cc_bw_bit(w, 0);
        if (k) cc_bw_bits(w, u & ((1u << k) - 1u), k);
    } else {
        for (i = 0; i < (uint32_t)CC_RICE_ESC; ++i) cc_bw_bit(w, 1);   /* no terminating 0 */
        cc_bw_bits(w, u, 32);
    }
}
static inline uint32_t cc_rice_get_u(cc_br *r, int k) {
    uint32_t q = 0;
    while (q < (uint32_t)CC_RICE_ESC && cc_br_bit(r)) q++;
    if (q < (uint32_t)CC_RICE_ESC) { uint32_t rem = k ? cc_br_bits(r, k) : 0u; return (q << k) | rem; }
    return cc_br_bits(r, 32);
}
static inline uint32_t cc_zz(int32_t v)  { return ((uint32_t)v << 1) ^ (uint32_t)(v >> 31); }
static inline int32_t  cc_unzz(uint32_t u){ return (int32_t)(u >> 1) ^ -(int32_t)(u & 1u); }
static inline void    cc_rice_put_s(cc_bw *w, int32_t v, int k) { cc_rice_put_u(w, cc_zz(v), k); }
static inline int32_t cc_rice_get_s(cc_br *r, int k) { return cc_unzz(cc_rice_get_u(r, k)); }

/* TWO-BOX (running-sum) feature classifier -> active[0..F).  RELATIVE / gain-robust / O(n).
 * Frame f is a feature frame iff any sample i in its 2N block satisfies
 *   mean(target box) - mean(base box) > box_thr,  compared by integer cross-multiply:
 *   tsum*bcnt - bsum*tcnt > box_thr*tcnt*bcnt   (exact, libm-free; matches echo_sandbox).
 * Both boxes are centred at i and clamped to [0,n); base is large (water floor), target small. */
static inline void cc_classify(const uint8_t *chart, int n,
                               const chart_codec_cfg_t *cfg, uint8_t *active, int F) {
    int rb = cfg->box_b / 2, rt = cfg->box_t / 2, i, f;
    int bL = 0, bR = (rb < n) ? rb : n - 1;
    int tL = 0, tR = (rt < n) ? rt : n - 1;
    int32_t bsum = 0, tsum = 0;
    for (f = 0; f < F; ++f) active[f] = 0;
    for (i = 0; i <= bR; ++i) bsum += chart[i];
    for (i = 0; i <= tR; ++i) tsum += chart[i];
    for (i = 0; i < n; ++i) {
        int nbL = i - rb; if (nbL < 0) nbL = 0;
        int nbR = i + rb; if (nbR > n - 1) nbR = n - 1;
        while (bL < nbL) bsum -= chart[bL++];
        while (bR < nbR) bsum += chart[++bR];
        int ntL = i - rt; if (ntL < 0) ntL = 0;
        int ntR = i + rt; if (ntR > n - 1) ntR = n - 1;
        while (tL < ntL) tsum -= chart[tL++];
        while (tR < ntR) tsum += chart[++tR];
        int bcnt = bR - bL + 1, tcnt = tR - tL + 1;
        if (tsum * bcnt - bsum * tcnt > (int32_t)cfg->box_thr * tcnt * bcnt) {
            int f0 = i / CC_HOP;                  /* sample i lies in frames f0 and f0-1 */
            if (f0 < F) active[f0] = 1;
            if (f0 - 1 >= 0) active[f0 - 1] = 1;
        }
    }
}

/* ============================= encode (MCU) ============================= */
/* chart[0..n) -> out[0..out_cap). Returns bytes written, or -1 on overflow / n>CC_MAXN.
 * Allocation-free; a 24-float block + a F-byte flag array on the stack.
 * Layout: [flag RLE header] [frame 0 data] [frame 1 data] ...  (no per-frame flag bit).
 * force_floor=1 -> classify is skipped and ALL frames are coded in floor mode (noise/
 * overflow fallback): never exceeds the floor-only size (~6x real, ~4x worst-case noise). */
static inline int cc_encode_impl(const uint8_t *chart, uint16_t n,
                                 const chart_codec_cfg_t *cfg, uint8_t *out, int out_cap,
                                 int force_floor) {
    cc_bw w; w.buf = out; w.cap = out_cap; w.byte = 0; w.acc = 0; w.nbits = 0; w.err = 0;
    int F = (n + CC_HOP - 1) / CC_HOP;
    int f, i, c;
    uint8_t active[CC_MAXFRAMES];
    if (F > CC_MAXFRAMES) return -1;
    if (force_floor) { for (f = 0; f < F; ++f) active[f] = 0; }
    else cc_classify(chart, (int)n, cfg, active, F);

    /* --- pass 1: run-length-code the feature flag (first bit + Rice run lengths) --- */
    int cur = active[0];
    cc_bw_bit(&w, cur);
    int run = 0;
    for (f = 0; f < F; ++f) {
        if (active[f] == cur) { run++; }
        else { cc_rice_put_u(&w, (uint32_t)run, cfg->k_flag); cur = active[f]; run = 1; }
    }
    cc_rice_put_u(&w, (uint32_t)run, cfg->k_flag);              /* final run */

    /* --- pass 2: per-frame DC + AC coeffs (mc = m or m_feat by mode; uniform, no dead-zone) --- */
    float prev_dc = 0.0f;
    for (f = 0; f < F; ++f) {
        float blk[CC_BLK], X[CC_N];
        int feat = active[f];
        for (i = 0; i < CC_BLK; ++i) {
            int idx = f * CC_HOP + i;
            uint8_t s = (idx < (int)n) ? chart[idx] : 127u;     /* pad with center (amp 127) */
            blk[i] = (float)s - 127.0f;
        }
        cc_mdct(blk, X);
        float sdc = feat ? cfg->step_dc_f : cfg->step_dc;
        float slm = feat ? cfg->step_lm_f : cfg->step_lm;
        int   kdc = feat ? cfg->k_dc_f   : cfg->k_dc;
        int   klm = feat ? cfg->k_lm_f   : cfg->k_lm;
        int   mc  = feat ? cfg->m_feat   : cfg->m;

        int qdc = CC_ROUND((X[0] - prev_dc) / sdc);
        cc_rice_put_s(&w, qdc, kdc);
        prev_dc += (float)qdc * sdc;                            /* reconstructed predictor */

        for (c = 1; c < mc; ++c) {
            int q = CC_ROUND(X[c] / slm);
            cc_rice_put_s(&w, q, klm);
        }
    }
    return cc_bw_finish(&w);
}

/* Normal encode (two-box feature classifier). Returns bytes, -1 on overflow. */
static inline int chart_encode(const uint8_t *chart, uint16_t n,
                               const chart_codec_cfg_t *cfg, uint8_t *out, int out_cap) {
    return cc_encode_impl(chart, n, cfg, out, out_cap, 0);
}
/* Floor-only encode: forces every frame to floor mode (no feature frames). Overflow-proof
 * fallback for noise pings. Caller pattern:
 *   int nb = chart_encode(chart, n, &cfg, out, cap);
 *   if (nb < 0) nb = chart_encode_floor(chart, n, &cfg, out, cap);  // send raw only if this <0 too
 * Decodes with the normal chart_decode (the flag header is just one long floor run). */
static inline int chart_encode_floor(const uint8_t *chart, uint16_t n,
                                     const chart_codec_cfg_t *cfg, uint8_t *out, int out_cap) {
    return cc_encode_impl(chart, n, cfg, out, out_cap, 1);
}

/* ============================= decode (host/app) ============================= */
/* buf[0..nbytes) -> chart[0..n).  `scratch` is a float OLA accumulator, size >=
 * chart_codec_scratch_floats(n) (= F*CC_HOP + CC_BLK; exceeds n+2*CC_N when n%CC_HOP != 0).
 * Returns bins written (n), or -1 on bitstream underrun. */
static inline int chart_decode(const uint8_t *buf, int nbytes, uint16_t n,
                               const chart_codec_cfg_t *cfg, uint8_t *chart, float *scratch) {
    int F = (n + CC_HOP - 1) / CC_HOP;
    int ylen = F * CC_HOP + CC_BLK;
    int i, f, c;
    for (i = 0; i < ylen; ++i) scratch[i] = 0.0f;
    cc_br r; r.buf = buf; r.nbytes = nbytes; r.byte = 0; r.acc = 0; r.nbits = 0; r.err = 0;
    float prev_dc = 0.0f;
    /* flag RLE header is written CONTIGUOUSLY before any frame data, so expand it fully now.
     * Stash per-frame flags in chart[0..F): read in the frame loop, overwritten by samples
     * only in the final pass (F <= n always) -> no extra buffer. */
    {
        int feat_cur = cc_br_bit(&r);
        uint32_t run_left = cc_rice_get_u(&r, cfg->k_flag);
        for (f = 0; f < F; ++f) {
            if (run_left == 0) { feat_cur ^= 1; run_left = cc_rice_get_u(&r, cfg->k_flag); }
            chart[f] = (uint8_t)feat_cur;
            run_left--;
        }
    }
    for (f = 0; f < F; ++f) {
        float X[CC_N], v[CC_BLK];
        for (c = 0; c < CC_N; ++c) X[c] = 0.0f;
        int feat = chart[f];
        float sdc = feat ? cfg->step_dc_f : cfg->step_dc;
        float slm = feat ? cfg->step_lm_f : cfg->step_lm;
        int   kdc = feat ? cfg->k_dc_f   : cfg->k_dc;
        int   klm = feat ? cfg->k_lm_f   : cfg->k_lm;
        int   mc  = feat ? cfg->m_feat   : cfg->m;

        int qdc = cc_rice_get_s(&r, kdc);
        prev_dc += (float)qdc * sdc;
        X[0] = prev_dc;
        for (c = 1; c < mc; ++c) X[c] = (float)cc_rice_get_s(&r, klm) * slm;
        cc_imdct(X, v);
        int st = f * CC_HOP;
        for (i = 0; i < CC_BLK; ++i) scratch[st + i] += v[i];
    }
    for (i = 0; i < (int)n; ++i) {
        float a = scratch[i] + 127.0f;
        if (a < 0.0f) a = 0.0f; else if (a > 255.0f) a = 255.0f;
        chart[i] = (uint8_t)CC_ROUND(a);
    }
    return r.err ? -1 : (int)n;
}

/* ============================= packet framing (mode switch) ============================= */
/* A self-describing packet = [band table][codec bitstream]. The band table (uniform or
 * progressive, from chart_bands_for_mode) makes the packet decode the same way regardless of
 * mode, so the mode switch lives entirely in how the CHART was resampled before encoding:
 *   uniform:     chart = native;                  bands = chart_bands_for_mode(UNIFORM,...)
 *   progressive: chart = rsProg(amp, bands);      bands = chart_bands_for_mode(PROGRESSIVE,...)
 * In both cases sum(bands.count) MUST equal n. Returns total packet bytes, or -1 on overflow. */
static inline int cc_pkt_encode_impl(const uint8_t *chart, uint16_t n, const cc_band_t *bands,
                                     uint8_t nbands, const chart_codec_cfg_t *cfg,
                                     uint8_t *out, int out_cap, int force_floor) {
    int hb = cc_bands_write(bands, nbands, out, out_cap);
    if (hb < 0) return -1;
    int bb = cc_encode_impl(chart, n, cfg, out + hb, out_cap - hb, force_floor);
    if (bb < 0) return -1;
    return hb + bb;
}
static inline int chart_pkt_encode(const uint8_t *chart, uint16_t n, const cc_band_t *bands,
                                   uint8_t nbands, const chart_codec_cfg_t *cfg, uint8_t *out, int out_cap) {
    return cc_pkt_encode_impl(chart, n, bands, nbands, cfg, out, out_cap, 0);
}
/* Floor-only fallback (noise/overflow): forces every frame to floor mode. Same packet layout. */
static inline int chart_pkt_encode_floor(const uint8_t *chart, uint16_t n, const cc_band_t *bands,
                                         uint8_t nbands, const chart_codec_cfg_t *cfg, uint8_t *out, int out_cap) {
    return cc_pkt_encode_impl(chart, n, bands, nbands, cfg, out, out_cap, 1);
}

/* Decode a [band table][bitstream] packet. Fills bands_out (<= max_bands), *nbands_out, and the
 * chart (>= sum of band counts bins); `scratch` >= chart_codec_scratch_floats(that sum). Returns
 * bins written (= chart length = sum of counts), or -1 on truncation/underrun. The caller maps
 * each bin to physical range from bands_out (uniform -> one native band; progressive -> ladder). */
static inline int chart_pkt_decode(const uint8_t *buf, int nbytes, const chart_codec_cfg_t *cfg,
                                   uint8_t *chart, float *scratch,
                                   cc_band_t *bands_out, uint8_t max_bands, uint8_t *nbands_out) {
    uint16_t n = 0;
    int hb = cc_bands_read(buf, nbytes, bands_out, max_bands, nbands_out, &n);
    if (hb < 0) return -1;
    return chart_decode(buf + hb, nbytes - hb, n, cfg, chart, scratch);
}

#endif /* CHART_CODEC_H_ */
