/* SPDX-License-Identifier: GPL-3.0-only */
/* VGM player for the Mega Drive's chips: YM2612 + PSG, with the YM2612 PCM data blocks, the 0x8n DAC writes
 * and the DAC streams (0x90-0x95). Commands for other chips are skipped. See vgm.c.
 * Two sources: a whole file in memory (vgm_open), or a stream (vgm_open_stream): the PCM bank given whole,
 * the commands arriving in a ring buffer while it plays, loops unrolled by the sender. */
#ifndef VGM_H
#define VGM_H
#include <stdint.h>
#include "opn2.h"
#include "psg.h"

#define VGM_BLOCKS 64u                          /* YM2612 PCM data blocks */
#define VGM_STREAMS 4u

enum { VGM_OK, VGM_ERR_MAGIC, VGM_ERR_SIZE, VGM_ERR_CMD, VGM_ERR_BLOCKS };
#define VGM_RING_CMD 12u                        /* the longest command a stream may carry (not 0x67: blocks go in the bank) */

typedef struct {
    uint8_t id, used, on, loop, port, reg;
    uint32_t step_size, step_base, freq, start, len, pos, left, phase, inc;   /* inc: Q16 per chip sample */
    const uint8_t *data;                        /* a drum: its sample (else the song's bank) */
    int32_t gain;                               /* .. its level, /256 around the DAC's middle */
} vgm_stream_t;

typedef struct {
    const uint8_t *buf;                         /* the commands: a whole file, or a ring (mask) */
    uint32_t mask;
    const volatile uint32_t *fill;              /* a stream: how many bytes have arrived (0: a whole file) */
    const uint8_t *bank;                        /* a stream: the PCM bank, whole */
    uint32_t len, start, loop, end, pos;        /* (a stream: pos counts the bytes taken, start, loop, end unused) */
    uint32_t blk_off[VGM_BLOCKS], blk_len[VGM_BLOCKS], blk_bank[VGM_BLOCKS], nblk, bank_len;
    uint32_t bank_pos;                          /* 0xE0's pointer into the PCM bank */
    uint32_t wait;                              /* VGM samples (44.1 kHz) not yet moved into acc */
    uint32_t acc;                               /* chip samples until the next command, Q16 */
    uint32_t played;                            /* VGM samples played, for the time shown */
    uint8_t playing, loops;                     /* loops: 1 = go back to the loop point at the end (a file) */
    uint8_t starved;                            /* a stream ran dry: time waits for the next bytes */
    uint32_t starve_count;
    vgm_stream_t st[VGM_STREAMS + 1];          /* the song's, and the drum (the last) */
} vgm_t;

int vgm_open(vgm_t *v, const uint8_t *buf, uint32_t len);   /* checks the file and finds the PCM blocks */
/* image: [u32 n][u32 length of block 1 .. n][the blocks' bytes] (little-endian); ring: size mask + 1, a power of
 * two; fill: the bytes written to it so far, ever (the writer only adds) */
int vgm_open_stream(vgm_t *v, const uint8_t *image, uint32_t image_len, const uint8_t *ring, uint32_t mask,
                    const volatile uint32_t *fill);
void vgm_play(vgm_t *v, opn2_t *c, psg_t *p);   /* from the start, the chips reset */
void vgm_stop(vgm_t *v, opn2_t *c, psg_t *p);   /* and silence: keys up, DAC off, PSG quiet */
/* a drum: 8-bit unsigned samples at rate Hz through the DAC (on from now), level gain / 256; replaces the last */
void vgm_drum(vgm_t *v, opn2_t *c, const uint8_t *data, uint32_t len, uint32_t rate, int32_t gain);
/* n chip samples (OPN2_RATE_NTSC) into out (L R), each (fm * fm_gain + psg * psg_gain) >> 8, the song's commands
 * applied at their samples; with no song playing, just the chips */
void vgm_render(vgm_t *v, opn2_t *c, psg_t *p, int32_t *out, uint32_t n, int32_t fm_gain, int32_t psg_gain);

#endif
