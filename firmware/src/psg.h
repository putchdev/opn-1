/* SPDX-License-Identifier: BSD-3-Clause */
/* Sega PSG (the SN76489 variant in the Mega Drive's VDP): a C port of MAME's sn76496.cpp, integer only. See psg.c. */
#ifndef PSG_H
#define PSG_H
#include <stdint.h>

#define PSG_CLOCK 3579545u                     /* NTSC; the chip updates at PSG_CLOCK / 16 */

typedef struct {
    int32_t reg[8], last, volume[4], period[4], count[4], output[4];
    uint32_t rng;
    uint32_t frac;                              /* psg_render: ticks owed, Q16 */
    int32_t held;                               /* .. its last sample */
} psg_t;

void psg_reset(psg_t *p);
void psg_write(psg_t *p, uint8_t data);         /* a byte to the chip's port, as on the Mega Drive */
int32_t psg_tick(psg_t *p);                     /* one update; returns the output (MAME's sample, negated as the chip) */
void psg_render(psg_t *p, int32_t *out, uint32_t n, uint32_t step_q16);   /* adds n samples to out (L R L R),
                                                   each the mean of the step_q16 / 65536 ticks it spans */

#endif
