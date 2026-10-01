#include "kec.h"
#include "kec_tab.h"

/* The device's echogram decoder, unchanged: the code below is the decoder half of the codec the
 * sonar runs. The encoder half is the sonar's and stays in the firmware tree (owner, 2026-10-01).
 * tools/mission_check decodes vectors made by that encoder, so a drift between the two shows there. */

/* ---------------------------------------------------------------- contexts */
#define NBAND        4
#define OFF_LAST     0
#define NCTX_LAST    32
#define OFF_SIG      (OFF_LAST + NCTX_LAST)
#define NCTX_SIG     (NBAND * 2)
#define OFF_MAG      (OFF_SIG + NCTX_SIG)
#define NCTX_MAG     (NBAND * 3)
#define OFF_DC       (OFF_MAG + NCTX_MAG)
#define NCTX_DC      4
#define NCTX         (OFF_DC + NCTX_DC)

#define PBITS 11
#define PMAX  (1u << PBITS)
#define ADAPT 5
#define TOP   (1u << 24)
#define EGCAP 20

static int band(int i) { int b = (i * NBAND) / KEC_BLK; return b < NBAND ? b : NBAND - 1; }

/* Quantiser step per frequency. Integer, round-half-up; `a4` is the slope in
 * quarters. Must stay bit-identical to the encoder's table. */
static void qtable(int q0, int a4, int32_t *Q)
{
    int f;
    for (f = 0; f < KEC_BLK; f++) {
        int32_t num = q0 * (4 * KEC_BLK + a4 * f) + 2 * KEC_BLK;
        int32_t v = num / (4 * KEC_BLK);
        Q[f] = v < 1 ? 1 : v;
    }
}

/* ------------------------------------------------------------------ decode */

#define QCLAMP 8191   /* a valid coefficient never gets near this; a corrupt stream can */

typedef struct {
    uint32_t range, code; const uint8_t *b; int nb, p;
    uint16_t pr[NCTX];
} dec_t;

static uint8_t d_byte(dec_t *d) { return (uint8_t)(d->p < d->nb ? d->b[d->p++] : (d->p++, 0)); }

static void d_init(dec_t *d, const uint8_t *b, int nb)
{
    int i;
    d->b = b; d->nb = nb; d->p = 1;          /* skip the leading cache byte */
    d->range = 0xFFFFFFFFu; d->code = 0;
    for (i = 0; i < 4; i++) d->code = (d->code << 8) | d_byte(d);
    for (i = 0; i < NCTX; i++) d->pr[i] = PMAX / 2;
}

static int d_bit(dec_t *d, int ctx)
{
    uint32_t bound = (d->range >> PBITS) * d->pr[ctx];
    int b;
    if (d->code < bound) { d->range = bound; d->pr[ctx] += (uint16_t)((PMAX - d->pr[ctx]) >> ADAPT); b = 0; }
    else { d->code -= bound; d->range -= bound; d->pr[ctx] -= d->pr[ctx] >> ADAPT; b = 1; }
    while (d->range < TOP) { d->range <<= 8; d->code = (d->code << 8) | d_byte(d); }
    return b;
}

static uint32_t d_raw(dec_t *d, int nb)
{
    uint32_t v = 0; int i;
    for (i = 0; i < nb; i++) {
        uint32_t t;
        d->range >>= 1;
        t = d->code >= d->range;
        if (t) d->code -= d->range;
        v = (v << 1) | t;
        while (d->range < TOP) { d->range <<= 8; d->code = (d->code << 8) | d_byte(d); }
    }
    return v;
}

static uint32_t eg_get(dec_t *d)
{
    int k = 0;
    while (k < EGCAP && d_raw(d, 1)) k++;
    return ((1u << k) | (k ? d_raw(d, k) : 0)) - 1;
}

static uint32_t mag_get(dec_t *d, int off)
{
    int j;
    for (j = 0; j < 3; j++) if (!d_bit(d, off + j)) return (uint32_t)j + 1;
    return eg_get(d) + 4;
}

int kec_decode_frame(const uint8_t *pay, int npay, int n, int q0, int a4, int R,
                     uint8_t *out)
{
    int32_t Q[KEC_BLK], qc[KEC_BLK], v[KEC_BLK];
    dec_t d; int bi, nb = n / KEC_BLK, dc_prev = 0;

    if (n % KEC_BLK) return -1;
    qtable(q0, a4, Q);
    d_init(&d, pay, npay);

    for (bi = 0; bi < nb; bi++) {
        int k, i, last = 0, prev = 0, u = 0;
        for (k = 0; k < KEC_BLK; k++) qc[k] = 0;

        if (d_bit(&d, OFF_DC)) {
            uint32_t z = mag_get(&d, OFF_DC + 1);
            u = (z & 1) ? -(int32_t)((z + 1) >> 1) : (int32_t)(z >> 1);
        }
        qc[0] = dc_prev + u; dc_prev = qc[0];

        for (k = 4; k >= 0; k--) {
            int node = (1 << (4 - k)) - 1 + (last >> (k + 1));
            last |= d_bit(&d, OFF_LAST + (node < 31 ? node : 31)) << k;
        }
        if (last >= KEC_BLK) last = KEC_BLK - 1;

        for (i = 1; i <= last; i++) {
            int s = d_bit(&d, OFF_SIG + band(i) * 2 + prev);
            prev = s;
            if (s) {
                int32_t m = (int32_t)mag_get(&d, OFF_MAG + band(i) * 3);
                qc[i] = d_raw(&d, 1) ? -m : m;
            }
        }
        for (k = 0; k < KEC_BLK; k++) {
            int32_t q = qc[k];
            if (q >  QCLAMP) q =  QCLAMP;
            if (q < -QCLAMP) q = -QCLAMP;
            {
                int32_t m = q < 0 ? -q : q;
                int32_t w = (m * 8 * Q[k] + (m > 0 ? (4 - R) * Q[k] : 0)) / 8;
                v[k] = q < 0 ? -w : w;
            }
        }
        for (i = 0; i < KEC_BLK; i++) {                        /* inverse DCT */
            int64_t s = 0; int32_t r;
            for (k = 0; k < KEC_BLK; k++) s += (int64_t)kec_dct[k * KEC_BLK + i] * v[k];
            r = (int32_t)((s + (1 << (KEC_DCT_SH - 1))) >> KEC_DCT_SH);
            out[bi * KEC_BLK + i] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
        }
    }
    return n;
}
/* ------------------------------------------------------------- container */

int kec_decode_ping(const uint8_t *buf, int nbuf, uint8_t *out, int outcap)
{
    uint8_t tmp[KEC_MAXFRAME];
    int o = 0, w = 0;
    while (o + KEC_HDR <= nbuf) {
        int nsamp = buf[o] | (buf[o+1] << 8);
        int q0 = buf[o+2], R = buf[o+3] & 7, a4 = buf[o+3] >> 3;
        int nb = buf[o+4] | (buf[o+5] << 8);
        int pad = (KEC_BLK - nsamp % KEC_BLK) % KEC_BLK, np = nsamp + pad;
        o += KEC_HDR;
        if (np > KEC_MAXFRAME || o + nb > nbuf || w + nsamp > outcap) return -1;
        if (kec_decode_frame(buf + o, nb, np, q0, a4, R, tmp) < 0) return -1;
        { int j; for (j = 0; j < nsamp; j++) out[w + j] = tmp[j]; }   /* no libc */
        w += nsamp; o += nb;
    }
    return w;
}
