#include "kec.h"
#include "kec_tab.h"

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
 * quarters. Must stay bit-identical to ref/codec.py:quant_table(). */
static void qtable(int q0, int a4, int32_t *Q)
{
    int f;
    for (f = 0; f < KEC_BLK; f++) {
        int32_t num = q0 * (4 * KEC_BLK + a4 * f) + 2 * KEC_BLK;
        int32_t v = num / (4 * KEC_BLK);
        Q[f] = v < 1 ? 1 : v;
    }
}

/* ------------------------------------------------------------ range coder */
typedef struct {
    uint64_t low; uint32_t range; uint8_t cache; uint32_t cnt;
    uint8_t *buf; int cap, n; int ovf;
    uint16_t p[NCTX];
} enc_t;

static void e_init(enc_t *e, uint8_t *buf, int cap)
{
    int i;
    e->low = 0; e->range = 0xFFFFFFFFu; e->cache = 0; e->cnt = 1;
    e->buf = buf; e->cap = cap; e->n = 0; e->ovf = 0;
    for (i = 0; i < NCTX; i++) e->p[i] = PMAX / 2;
}

static void e_byte(enc_t *e, uint8_t b)
{
    if (e->n < e->cap) e->buf[e->n++] = b; else { e->ovf = 1; e->n++; }
}

static void e_shift(enc_t *e)
{
    if (e->low < 0xFF000000u || e->low > 0xFFFFFFFFu) {
        uint32_t carry = (uint32_t)(e->low >> 32);
        uint8_t t = e->cache;
        do { e_byte(e, (uint8_t)(t + carry)); t = 0xFF; } while (--e->cnt);
        e->cache = (uint8_t)(e->low >> 24);
    }
    e->cnt++;
    e->low = (e->low << 8) & 0xFFFFFFFFu;
}

static void e_bit(enc_t *e, int ctx, int b)
{
    uint32_t bound = (e->range >> PBITS) * e->p[ctx];
    if (b) { e->low += bound; e->range -= bound; e->p[ctx] -= e->p[ctx] >> ADAPT; }
    else   { e->range = bound; e->p[ctx] += (uint16_t)((PMAX - e->p[ctx]) >> ADAPT); }
    while (e->range < TOP) { e->range <<= 8; e_shift(e); }
}

static void e_raw(enc_t *e, uint32_t v, int nb)          /* bypass, equiprobable */
{
    int k;
    for (k = nb - 1; k >= 0; k--) {
        e->range >>= 1;
        if ((v >> k) & 1) e->low += e->range;
        while (e->range < TOP) { e->range <<= 8; e_shift(e); }
    }
}

static int e_done(enc_t *e) { int i; for (i = 0; i < 5; i++) e_shift(e); return e->ovf ? -1 : e->n; }

/* -------------------------------------------------------------- binarisers */
static void eg_put(enc_t *e, uint32_t v)      /* Exp-Golomb order 0, bypass */
{
    uint32_t w = v + 1; int k = 0;
    while (w >> (k + 1)) k++;
    if (k) e_raw(e, (1u << k) - 1, k);
    e_raw(e, 0, 1);
    if (k) e_raw(e, w & ((1u << k) - 1), k);
}

static void mag_put(enc_t *e, int off, uint32_t m)        /* m >= 1 */
{
    uint32_t v = m - 1; int j;
    for (j = 0; j < 3; j++) { int b = v > (uint32_t)j; e_bit(e, off + j, b); if (!b) return; }
    eg_put(e, v - 3);
}

/* ------------------------------------------------------------------ encode */
/* One block of the forward transform, folded on the symmetry of the basis: row k is
 * symmetric about the centre for even k and antisymmetric for odd k, so 32 products
 * per row become 16 over the sums or the differences. Everything up to the final
 * shift is exact integer arithmetic, so this is not an approximation of the plain
 * matrix product -- it is the same number, for half the multiplies.
 *
 * A frame need not be a whole number of blocks: only the tail block is copied out,
 * into 32 bytes of stack, so nothing ever buffers the frame. */
static void dct_block(const uint8_t *x, int n, int bi, int nb, int32_t *c)
{
    uint8_t tail[KEC_BLK];
    const uint8_t *b = x + bi * KEC_BLK;
    int32_t sum[KEC_BLK / 2], dif[KEC_BLK / 2];
    int k, i;

    if (bi == nb - 1 && n % KEC_BLK) {
        int m = n % KEC_BLK;
        for (k = 0; k < m; k++) tail[k] = b[k];
        for (; k < KEC_BLK; k++) tail[k] = 0;
        b = tail;
    }
    for (i = 0; i < KEC_BLK / 2; i++) {
        sum[i] = (int32_t)b[i] + b[KEC_BLK - 1 - i];
        dif[i] = (int32_t)b[i] - b[KEC_BLK - 1 - i];
    }
    for (k = 0; k < KEC_BLK; k++) {
        const int16_t *r = kec_dct + k * KEC_BLK;
        const int32_t *v = (k & 1) ? dif : sum;
        int32_t acc = 0;
        for (i = 0; i < KEC_BLK / 2; i++) acc += (int32_t)r[i] * v[i];
        c[k] = (acc + (1 << (KEC_DCT_SH - 1))) >> KEC_DCT_SH;
    }
}

void kec_dct_frame(const uint8_t *x, int n, int32_t *c)
{
    int nb = (n + KEC_BLK - 1) / KEC_BLK, bi;
    for (bi = 0; bi < nb; bi++) dct_block(x, n, bi, nb, c + bi * KEC_BLK);
}

/* The single coding path. Coefficients come either from `cache` (the rate-control
 * search, which must not transform twice) or, when cache is NULL, from transforming
 * `x` a block at a time. Fusing the transform into this loop keeps the working set at
 * 32 coefficients and measured ~15% faster on an STM32F302R8 than reading a whole
 * cached frame back -- so the common single-attempt case stays fused, and the cache
 * is built only once a retry is actually needed. */
static int code_frame(const uint8_t *x, const int32_t *cache, int n, int nblk,
                      int q0, int a4, int R, uint8_t *out, int outcap)
{
    int32_t Q[KEC_BLK], qc[KEC_BLK], local[KEC_BLK];
    enc_t e; int bi, dc_prev = 0;

    if (nblk <= 0) return -1;
    qtable(q0, a4, Q);
    e_init(&e, out, outcap);

    for (bi = 0; bi < nblk; bi++) {
        const int32_t *cb;
        int k, i, last = 0, prev = 0, u, z;

        if (cache) cb = cache + bi * KEC_BLK;
        else { dct_block(x, n, bi, nblk, local); cb = local; }

        for (k = 0; k < KEC_BLK; k++) {
            int32_t v = cb[k], av = v < 0 ? -v : v;
            int32_t m = (av * 8 + R * Q[k]) / (8 * Q[k]);
            qc[k] = v < 0 ? -m : m;
        }

        u = qc[0] - dc_prev; dc_prev = qc[0];
        z = u >= 0 ? 2 * u : -2 * u - 1;
        if (!z) e_bit(&e, OFF_DC, 0);
        else { e_bit(&e, OFF_DC, 1); mag_put(&e, OFF_DC + 1, (uint32_t)z); }

        for (k = KEC_BLK - 1; k > 0; k--) if (qc[k]) { last = k; break; }
        for (k = 4; k >= 0; k--) {
            int node = (1 << (4 - k)) - 1 + (last >> (k + 1));
            e_bit(&e, OFF_LAST + (node < 31 ? node : 31), (last >> k) & 1);
        }
        for (i = 1; i <= last; i++) {
            int s = qc[i] != 0;
            e_bit(&e, OFF_SIG + band(i) * 2 + prev, s);
            prev = s;
            if (s) {
                int32_t m = qc[i] < 0 ? -qc[i] : qc[i];
                mag_put(&e, OFF_MAG + band(i) * 3, (uint32_t)m);
                e_raw(&e, qc[i] < 0, 1);
            }
        }
        if (e.ovf) return -1;                                 /* budget blown: stop early */
    }
    return e_done(&e);
}

int kec_code_coeffs(const int32_t *c, int nblk, int q0, int a4, int R,
                    uint8_t *out, int outcap)
{
    return code_frame(0, c, nblk * KEC_BLK, nblk, q0, a4, R, out, outcap);
}

int kec_encode_frame(const uint8_t *x, int n, int q0, int a4, int R,
                     uint8_t *out, int outcap)
{
    if (n <= 0) return -1;
    return code_frame(x, 0, n, (n + KEC_BLK - 1) / KEC_BLK, q0, a4, R, out, outcap);
}

/* ------------------------------------------------------------------ decode */
#ifndef KEC_NO_DECODER

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
#endif /* KEC_NO_DECODER */

/* ------------------------------------------------- container + rate control */

#define KEC_FRAME 512
#define KEC_A4      16      /* a = 4.0, the slope of the quantiser table  */
#define KEC_R        2      /* dead zone, eighths of a step               */
#define NLADDER     22

/* Coarse on purpose: every rung is a real step in cost, and the MCU must not spend
 * its budget searching. Starting where the previous ping settled makes the search
 * one pass in the common case. */
static const uint8_t QLADDER[NLADDER] = {
    3, 4, 5, 6, 7, 8, 9, 11, 13, 15, 18, 21, 25, 30, 35, 42, 50, 59, 70, 83, 99, 118
};
#define QI_DEFAULT 2        /* step 5 */

static void put_hdr(uint8_t *h, int nsamp, int q0, int a4, int R, int nbytes)
{
    h[0] = (uint8_t)nsamp;  h[1] = (uint8_t)(nsamp >> 8);
    h[2] = (uint8_t)q0;     h[3] = (uint8_t)((R & 7) | ((a4 & 31) << 3));
    h[4] = (uint8_t)nbytes; h[5] = (uint8_t)(nbytes >> 8);
}

int kec_encode_ping(const uint8_t *x, int n, int budget,
                    uint8_t *out, int outcap, kec_rc_state *st)
{
    int32_t c[KEC_MAXFRAME];
    const int32_t *cache;
    int nfr, per, i, o = 0, s;

    if (n <= 0) return 0;
    nfr = (n + KEC_FRAME - 1) / KEC_FRAME;
    per = budget / nfr - KEC_HDR;
    if (per < 24) per = 24;
    i = st ? st->qi : QI_DEFAULT;
    if (i >= NLADDER) i = NLADDER - 1;

    for (s = 0; s < n; s += KEC_FRAME) {
        int nsamp = n - s > KEC_FRAME ? KEC_FRAME : n - s;
        int nblk = (nsamp + KEC_BLK - 1) / KEC_BLK;
        int len, room;

        cache = 0;                 /* first attempt is fused; see code_frame() */
        for (;;) {
            room = outcap - o - KEC_HDR;
            if (room <= 0) return -1;
            /* Below the top rung the buffer is capped at the budget, so a frame that
             * will not fit stops early instead of being coded out in full. */
            len = code_frame(x + s, cache, nsamp, nblk, QLADDER[i], KEC_A4, KEC_R,
                             out + o + KEC_HDR,
                             (i < NLADDER - 1 && per < room) ? per : room);
            if (len >= 0 && len <= per) break;
            if (i == NLADDER - 1) { if (len < 0) return -1; break; }
            /* A retry is happening, so the transform is about to be repeated: pay for
             * it once here and read it back on every rung from now on. */
            if (!cache) { kec_dct_frame(x + s, nsamp, c); cache = c; }
            i++;
        }
        put_hdr(out + o, nsamp, QLADDER[i], KEC_A4, KEC_R, len);
        o += KEC_HDR + len;
        if (len * 4 < per * 3 && i > 0) i--;      /* room to spare -> finer next frame */
    }
    if (st) st->qi = (uint8_t)i;
    return o;
}

#ifndef KEC_NO_DECODER
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
#endif
