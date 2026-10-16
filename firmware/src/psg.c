/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Sega PSG: a C port of the Sega VDP PSG path of MAME's sn76496.cpp (license:BSD-3-Clause,
 * copyright-holders:Nicola Salmoria, with contributions by others; https://github.com/mamedev/mame).
 * Integer only. tests/psg_compare.cpp checks it tick for tick against a transcription of MAME's code.
 *
 * Copyright (c) Nicola Salmoria and the MAME contributors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided
 * that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the
 *    following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and
 *    the following disclaimer in the documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or
 *    promote products derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include "psg.h"

#ifndef OPN2_HOT
#define OPN2_HOT
#endif

#define FEEDBACK 0x8000u                        /* Sega: a 16-bit LFSR, taps on bits 0 and 3 */
#define TAP1 0x01u
#define TAP2 0x08u

/* 2 dB a step from 0x7fff / 4, as MAME computes it in doubles (truncated) */
static const int32_t VOL[16] = {8191, 6506, 5168, 4105, 3260, 2590, 2057, 1634, 1298, 1031, 819, 650, 516, 410, 326, 0};

void psg_reset(psg_t *p)
{
    uint32_t i;
    p->last = 3;                                /* the Sega PSG starts with channel 2's volume selected */
    for (i = 0; i < 8u; i += 2) {
        p->reg[i] = 0;
        p->reg[i + 1] = 0xF;
    }
    for (i = 0; i < 4u; i++) {
        p->output[i] = p->count[i] = p->period[i] = 0;
        p->volume[i] = VOL[p->reg[i * 2 + 1]];
    }
    p->rng = FEEDBACK;
    p->output[3] = (int32_t)(p->rng & 1u);
    p->frac = 0;
    p->held = 0;
}

void psg_write(psg_t *p, uint8_t data)
{
    int32_t r, c, n;
    if (data & 0x80u) {
        r = (data & 0x70) >> 4;
        p->last = r;
        p->reg[r] = (p->reg[r] & 0x3F0) | (data & 0x0F);
    } else
        r = p->last;
    c = r >> 1;
    switch (r) {
    case 0: case 2: case 4:                     /* tone: frequency (a Sega PSG takes 0 as 0) */
        if (!(data & 0x80u)) p->reg[r] = (p->reg[r] & 0x0F) | ((data & 0x3F) << 4);
        p->period[c] = p->reg[r];
        if (r == 4 && (p->reg[6] & 3) == 3) p->period[3] = p->period[2] << 1;
        break;
    case 1: case 3: case 5: case 7:             /* volume */
        p->volume[c] = VOL[data & 0x0F];
        if (!(data & 0x80u)) p->reg[r] = (p->reg[r] & 0x3F0) | (data & 0x0F);
        break;
    case 6:                                     /* noise: N/512, N/1024, N/2048 or tone 3's; white or periodic */
        if (!(data & 0x80u)) p->reg[r] = (p->reg[r] & 0x3F0) | (data & 0x0F);
        n = p->reg[6];
        p->period[3] = (n & 3) == 3 ? p->period[2] << 1 : 1 << (5 + (n & 3));
        p->rng = FEEDBACK;
        break;
    }
}

OPN2_HOT int32_t psg_tick(psg_t *p)
{
    uint32_t i;
    for (i = 0; i < 3u; i++)
        if (--p->count[i] <= 0) {
            p->output[i] ^= 1;
            p->count[i] = p->period[i];
        }
    if (--p->count[3] <= 0) {
        uint32_t white = (p->reg[6] >> 2) & 1u;
        if (((p->rng & TAP1) != 0) != (((p->rng & TAP2) != 0) && white)) p->rng = (p->rng >> 1) | FEEDBACK;
        else p->rng >>= 1;
        p->output[3] = (int32_t)(p->rng & 1u);
        p->count[3] = p->period[3];
    }
    return -((p->output[0] ? p->volume[0] : 0) + (p->output[1] ? p->volume[1] : 0) +
             (p->output[2] ? p->volume[2] : 0) + (p->output[3] ? p->volume[3] : 0));
}

OPN2_HOT void psg_render(psg_t *p, int32_t *out, uint32_t n, uint32_t step_q16)
{
    uint32_t s;
    for (s = 0; s < n; s++) {                   /* a box filter: the mean of the ticks in each sample */
        int32_t sum = 0, k;
        p->frac += step_q16;
        k = (int32_t)(p->frac >> 16);
        p->frac &= 0xFFFFu;
        if (k > 0) {                            /* (no tick in this sample: hold the last) */
            int32_t t = k;
            while (t--) sum += psg_tick(p);
            p->held = sum / k;
        }
        out[2u * s] += p->held;
        out[2u * s + 1u] += p->held;
    }
}
