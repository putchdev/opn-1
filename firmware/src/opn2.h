/* SPDX-License-Identifier: BSD-3-Clause */
/* YM2612 (OPN2) FM core: a C port of ymfm's YM2612 path, integer only. See opn2.c. */
#ifndef OPN2_H
#define OPN2_H
#include <stdint.h>

#define OPN2_RATE_NTSC 53267u                   /* 7670453 / 144: one opn2_clock() per sample at this rate */

typedef struct {
    uint32_t phase;                             /* 10.10 */
    uint32_t phase_step;                        /* cached; OPN2_STEP_DYNAMIC when LFO PM applies */
    uint32_t block_freq;
    int32_t detune;
    uint16_t env;                               /* attenuation, 10 bits */
    uint16_t sustain;
    uint16_t total_level;
    uint8_t state, ssg_inv, key, keyon_live;
    uint8_t multiple;                           /* x.1 */
    uint8_t rate[6];                            /* by envelope state */
    uint8_t am_en, ssg_en, ssg_mode;
    uint16_t offs;                              /* register offset of this operator */
} opn2_op_t;

typedef struct {
    opn2_op_t op[4];                            /* O1..O4, i.e. register slots 0, 8, 4, 12 */
    int16_t fb[2], fb_in;
    uint16_t offs;                              /* register offset of this channel */
} opn2_ch_t;

typedef struct {
    opn2_ch_t ch[6];
    uint8_t regs[0x200];
    uint32_t env_counter, lfo_counter, lfo_am;
    uint32_t prepare_count, modified, active;
    uint16_t address, dac_data;
    uint8_t dac_enable;
} opn2_t;

void opn2_reset(opn2_t *c);
void opn2_write(opn2_t *c, uint32_t port, uint8_t data);       /* port 0..3, as on the chip's bus */
void opn2_write_reg(opn2_t *c, uint32_t part, uint8_t reg, uint8_t data);
void opn2_render(opn2_t *c, int32_t *out, uint32_t n);         /* n samples at OPN2_RATE_NTSC, L R L R ... */
void opn2_clock(opn2_t *c, int32_t *l, int32_t *r);            /* one sample */

#endif
