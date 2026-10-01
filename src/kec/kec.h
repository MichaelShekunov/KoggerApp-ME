/* Kogger echogram codec, decoder side: what the app needs to show a compressed echogram (ID_CHART_Z).
 *
 * The encoder runs on the sonar and stays in the firmware tree (owner, 2026-10-01). kec.c is the
 * device's decoder, unchanged, so it decodes exactly what the device's encoder produces;
 * tools/mission_check decodes vectors made by that encoder and fails if the two drift apart.
 *
 * Integer only: no float, no libc.
 */
#ifndef KEC_H
#define KEC_H
#include <stdint.h>

#define KEC_BLK      32          /* transform length along range                */
#define KEC_HDR       6          /* frame header, bytes                         */
#define KEC_MAXFRAME  512        /* samples per frame; equals the container's frame size */

/* Decode a ping. Returns samples written, or -1 on a malformed stream. */
int kec_decode_ping(const uint8_t *buf, int nbuf, uint8_t *out, int outcap);

/* Decode one frame of `n` samples (a whole number of blocks) coded with step `q0`, slope `a4`
 * (quarters) and dead zone `R` (eighths of a step). Returns `n`, or -1 if `n` is not a whole
 * number of blocks. For callers doing their own framing. */
int kec_decode_frame(const uint8_t *pay, int npay, int n, int q0, int a4, int R,
                     uint8_t *out);

#endif
