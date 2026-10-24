/* Kogger echogram codec -- lossy compression of one sonar ping, on the MCU.
 *
 * Integer only: no float, no division in the inner loops, no libc at all.
 * The encoder needs no inverse transform (DC prediction runs on quantised values),
 * so the MCU pays for the forward DCT and the entropy coder only.
 *
 * The transform runs ONCE per frame; the rate-control search re-quantises and re-codes
 * the cached coefficients instead of transforming again. Measured on an STM32F302R8,
 * that is the difference between 207 and 3883 cycles per sample on incompressible data.
 *
 * Working set for a 512-sample frame: 1 KB of cached coefficients + 96 bytes of
 * probability state + the output buffer. Flash: 2 KB of DCT table plus code.
 *
 * The format is defined by docs/FORMAT.md; this header is the API only.
 */
#ifndef KEC_H
#define KEC_H
#include <stdint.h>

#define KEC_BLK      32          /* transform length along range                */
#define KEC_HDR       6          /* frame header, bytes                         */
#define KEC_MAXFRAME  512        /* samples per frame; equals the container's frame size */

/* Encoder rate-control state. Zero-initialise once; carry it across pings so each
 * frame of a ping starts at the step the same frame of the previous ping settled on.
 * One step per frame position, not one per ping: the near field and the far field of
 * a ping need different steps, and a single shared step missed and was re-coded on
 * most frames (2.2 coding passes per frame on the 2026-10-24 test set; 1.2 per-frame).
 * Frames past the last slot share it. A slot holds rung + 1; 0 means the frame has not been
 * coded yet and starts where the previous frame of the same ping ended (measured on the
 * board 2026-10-24: without this the first ping after power-up re-coded every frame from
 * the finest rung, +49% cycles on that ping). */
#define KEC_RC_FRAMES 24         /* 24 x 512 samples = 12288, the longest ping seen */
typedef struct { uint8_t qi[KEC_RC_FRAMES]; } kec_rc_state;

/* Compress `n` samples into at most `budget` bytes.
 * Returns bytes written, or -1 if `out` is too small (need budget + KEC_HDR slack).
 * `st` may be NULL, in which case the search restarts from the default step. */
int kec_encode_ping(const uint8_t *x, int n, int budget,
                    uint8_t *out, int outcap, kec_rc_state *st);

/* Decode a ping. Returns samples written, or -1 on a malformed stream. */
int kec_decode_ping(const uint8_t *buf, int nbuf, uint8_t *out, int outcap);

/* Transform one frame into int16 coefficients, and code already-transformed
 * coefficients. Split so the rate-control search never transforms twice. */
void kec_dct_frame(const uint8_t *x, int n, int32_t *c);
int  kec_code_coeffs(const int32_t *c, int nblk, int q0, int a4, int R,
                     uint8_t *out, int outcap);

/* Single frame, for tests and for callers doing their own framing. */
int kec_encode_frame(const uint8_t *x, int n, int q0, int a4, int R,
                     uint8_t *out, int outcap);
int kec_decode_frame(const uint8_t *pay, int npay, int n, int q0, int a4, int R,
                     uint8_t *out);

#endif
