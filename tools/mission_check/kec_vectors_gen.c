/* kec_vectors_gen.c - makes kec_vectors.h: pings encoded by the device's echogram encoder, with what the
 * device's decoder returns for each. The encoder is the sonar's and is not in this repository, so this
 * builds only against the device's codec (kec.c and kec.h of the firmware tree):
 *
 *     cc -std=c99 -O2 kec_vectors_gen.c <firmware>/kec.c -I<firmware> -o gen && ./gen > kec_vectors.h
 *
 * Built against the app's src/kec (decoder only) it fails to compile, which is the point. Run it again
 * only when the codec's format changes; mission_check then holds the app's decoder to the new vectors. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "kec.h"

static uint32_t rng = 0x12345678u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static uint32_t fnv(const uint8_t *p, int n) { uint32_t h = 2166136261u; int i; for (i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; } return h; }

static int clamp(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* kind: 0 ramp+noise, 1 echogram (bottom echo at 60 %), 2 white noise, 3 zeros, 4 saturated, 5 sparse spikes */
static void make(uint8_t *x, int n, int kind)
{
    int i;
    for (i = 0; i < n; i++) {
        int v = 0, noise = (int)(rnd() % 9) - 4;
        switch (kind) {
        case 0: v = (i * 200) / (n > 1 ? n - 1 : 1) + noise; break;
        case 1: {
            int b = (n * 6) / 10;
            if (i < 8) v = 230 - i * 20;                         /* the transmit pulse */
            else if (i < b) v = 18 + noise + (int)(rnd() % 6);   /* water column */
            else if (i < b + 6) v = 250 - (i - b) * 3;           /* the bottom */
            else v = 160 * 64 / (64 + (i - b)) + noise;          /* decaying tail */
            break; }
        case 2: v = (int)(rnd() & 255); break;
        case 3: v = 0; break;
        case 4: v = 255; break;
        case 5: v = (rnd() % 37 == 0) ? 255 : 3; break;
        }
        x[i] = (uint8_t)clamp(v);
    }
}

#define MAXN 3000
static uint8_t x[MAXN], enc[8192], dec[MAXN];
static int nvec = 0;
static uint32_t hashes[64]; static int counts[64], lens[64];

static void emit(int n, int kind, int budget, kec_rc_state *st)
{
    int len, got, i;
    make(x, n, kind);
    len = kec_encode_ping(x, n, budget, enc, (int)sizeof enc, st);
    if (len < 0) { fprintf(stderr, "encode failed n=%d kind=%d budget=%d\n", n, kind, budget); return; }
    got = kec_decode_ping(enc, len, dec, MAXN);
    if (got != n) { fprintf(stderr, "decode returned %d for n=%d\n", got, n); return; }
    printf("static const uint8_t kec_vec%d[] = {  /* n=%d kind=%d budget=%d -> %d bytes */\n", nvec, n, kind, budget, len);
    for (i = 0; i < len; i++) printf("%s0x%02X,%s", (i % 16) ? "" : "    ", enc[i], (i % 16 == 15 || i == len - 1) ? "\n" : " ");
    printf("};\n");
    hashes[nvec] = fnv(dec, got); counts[nvec] = got; lens[nvec] = len;
    nvec++;
}

int main(void)
{
    kec_rc_state st; int i;
    printf("/* GENERATED 2026-10-01 by kec_vectors_gen.c with the device's echogram codec (firmware tree) -- do not edit.\n");
    printf(" * Each vector: a ping encoded by the sonar's encoder; samples = what the device's decoder returns;\n");
    printf(" * fnv = FNV-1a-32 of those samples. The app's decoder must return the same, byte for byte. */\n");
    printf("#include <stdint.h>\n\n");
    emit(1, 0, 64, 0);
    emit(31, 1, 64, 0);
    emit(32, 5, 64, 0);
    emit(512, 0, 400, 0);
    emit(700, 1, 300, 0);
    emit(1500, 2, 600, 0);
    emit(1024, 3, 100, 0);
    emit(1024, 4, 100, 0);
    emit(2000, 1, 900, 0);
    emit(2000, 5, 2400, 0);
    memset(&st, 0, sizeof st);
    for (i = 0; i < 6; i++) emit(1200, i % 2 ? 1 : 2, 260 + 40 * i, &st);   /* rate control carried across pings */
    printf("\nstruct KecVector { const uint8_t *enc; int nenc; int samples; uint32_t fnv; };\n");
    printf("static const struct KecVector kec_vectors[] = {\n");
    for (i = 0; i < nvec; i++) printf("    { kec_vec%d, %d, %d, 0x%08Xu },\n", i, lens[i], counts[i], hashes[i]);
    printf("};\nstatic const int kec_nvectors = %d;\n", nvec);
    fprintf(stderr, "%d vectors\n", nvec);
    return 0;
}
