/* SPDX-License-Identifier: BSD-3-Clause
 *
 * YM2612 (OPN2) FM core: a C port of the YM2612 path of ymfm (https://github.com/aaronsgiles/ymfm),
 * integer only, for a CPU with no FPU and no C++ runtime. It computes one sample per call, like ymfm, rather
 * than every internal clock like Nuked-OPN2 (too slow for the FM-1). Behaviour follows ymfm as closely as
 * possible so that it can be checked against it sample for sample (tests/opn2_compare.cpp).
 * Not ported: the timers, and so CSM key-on; the status/busy reads.
 *
 * Copyright (c) 2021, Aaron Giles (ymfm). All rights reserved.
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
#include "opn2.h"

/* A firmware build can place the per-sample code and the tables in RAM (flash execution is slow on the FM-1) */
#ifndef OPN2_HOT
#define OPN2_HOT
#endif
#ifndef OPN2_RODATA
#define OPN2_RODATA const
#endif

enum { EG_ATTACK = 1, EG_DECAY, EG_SUSTAIN, EG_RELEASE };
#define EG_QUIET 0x380u
#define STEP_DYNAMIC 1u                         /* as ymfm: a phase step that is recomputed each sample */
#define ALL_CH 0x3Fu
#define BF(v, s, n) (((uint32_t)(v) >> (s)) & ((1u << (n)) - 1u))

/* |sin| for a quarter wave, as 4.8 attenuation (from the die, via ymfm) */
static OPN2_RODATA uint16_t SIN_ATT[256] = {
    0x859,0x6c3,0x607,0x58b,0x52e,0x4e4,0x4a6,0x471,0x443,0x41a,0x3f5,0x3d3,0x3b5,0x398,0x37e,0x365,
    0x34e,0x339,0x324,0x311,0x2ff,0x2ed,0x2dc,0x2cd,0x2bd,0x2af,0x2a0,0x293,0x286,0x279,0x26d,0x261,
    0x256,0x24b,0x240,0x236,0x22c,0x222,0x218,0x20f,0x206,0x1fd,0x1f5,0x1ec,0x1e4,0x1dc,0x1d4,0x1cd,
    0x1c5,0x1be,0x1b7,0x1b0,0x1a9,0x1a2,0x19b,0x195,0x18f,0x188,0x182,0x17c,0x177,0x171,0x16b,0x166,
    0x160,0x15b,0x155,0x150,0x14b,0x146,0x141,0x13c,0x137,0x133,0x12e,0x129,0x125,0x121,0x11c,0x118,
    0x114,0x10f,0x10b,0x107,0x103,0x0ff,0x0fb,0x0f8,0x0f4,0x0f0,0x0ec,0x0e9,0x0e5,0x0e2,0x0de,0x0db,
    0x0d7,0x0d4,0x0d1,0x0cd,0x0ca,0x0c7,0x0c4,0x0c1,0x0be,0x0bb,0x0b8,0x0b5,0x0b2,0x0af,0x0ac,0x0a9,
    0x0a7,0x0a4,0x0a1,0x09f,0x09c,0x099,0x097,0x094,0x092,0x08f,0x08d,0x08a,0x088,0x086,0x083,0x081,
    0x07f,0x07d,0x07a,0x078,0x076,0x074,0x072,0x070,0x06e,0x06c,0x06a,0x068,0x066,0x064,0x062,0x060,
    0x05e,0x05c,0x05b,0x059,0x057,0x055,0x053,0x052,0x050,0x04e,0x04d,0x04b,0x04a,0x048,0x046,0x045,
    0x043,0x042,0x040,0x03f,0x03e,0x03c,0x03b,0x039,0x038,0x037,0x035,0x034,0x033,0x031,0x030,0x02f,
    0x02e,0x02d,0x02b,0x02a,0x029,0x028,0x027,0x026,0x025,0x024,0x023,0x022,0x021,0x020,0x01f,0x01e,
    0x01d,0x01c,0x01b,0x01a,0x019,0x018,0x017,0x017,0x016,0x015,0x014,0x014,0x013,0x012,0x011,0x011,
    0x010,0x00f,0x00f,0x00e,0x00d,0x00d,0x00c,0x00c,0x00b,0x00a,0x00a,0x009,0x009,0x008,0x008,0x007,
    0x007,0x007,0x006,0x006,0x005,0x005,0x005,0x004,0x004,0x004,0x003,0x003,0x003,0x002,0x002,0x002,
    0x002,0x001,0x001,0x001,0x001,0x001,0x001,0x001,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000};

/* attenuation -> linear: 10-bit mantissas with the implied 0x400, shifted up 2, reversed (ymfm's layout) */
#define X(a) (((a) | 0x400) << 2)
static OPN2_RODATA uint16_t POW[256] = {
    X(0x3fa),X(0x3f5),X(0x3ef),X(0x3ea),X(0x3e4),X(0x3df),X(0x3da),X(0x3d4),X(0x3cf),X(0x3c9),X(0x3c4),X(0x3bf),
    X(0x3b9),X(0x3b4),X(0x3ae),X(0x3a9),X(0x3a4),X(0x39f),X(0x399),X(0x394),X(0x38f),X(0x38a),X(0x384),X(0x37f),
    X(0x37a),X(0x375),X(0x370),X(0x36a),X(0x365),X(0x360),X(0x35b),X(0x356),X(0x351),X(0x34c),X(0x347),X(0x342),
    X(0x33d),X(0x338),X(0x333),X(0x32e),X(0x329),X(0x324),X(0x31f),X(0x31a),X(0x315),X(0x310),X(0x30b),X(0x306),
    X(0x302),X(0x2fd),X(0x2f8),X(0x2f3),X(0x2ee),X(0x2e9),X(0x2e5),X(0x2e0),X(0x2db),X(0x2d6),X(0x2d2),X(0x2cd),
    X(0x2c8),X(0x2c4),X(0x2bf),X(0x2ba),X(0x2b5),X(0x2b1),X(0x2ac),X(0x2a8),X(0x2a3),X(0x29e),X(0x29a),X(0x295),
    X(0x291),X(0x28c),X(0x288),X(0x283),X(0x27f),X(0x27a),X(0x276),X(0x271),X(0x26d),X(0x268),X(0x264),X(0x25f),
    X(0x25b),X(0x257),X(0x252),X(0x24e),X(0x249),X(0x245),X(0x241),X(0x23c),X(0x238),X(0x234),X(0x230),X(0x22b),
    X(0x227),X(0x223),X(0x21e),X(0x21a),X(0x216),X(0x212),X(0x20e),X(0x209),X(0x205),X(0x201),X(0x1fd),X(0x1f9),
    X(0x1f5),X(0x1f0),X(0x1ec),X(0x1e8),X(0x1e4),X(0x1e0),X(0x1dc),X(0x1d8),X(0x1d4),X(0x1d0),X(0x1cc),X(0x1c8),
    X(0x1c4),X(0x1c0),X(0x1bc),X(0x1b8),X(0x1b4),X(0x1b0),X(0x1ac),X(0x1a8),X(0x1a4),X(0x1a0),X(0x19c),X(0x199),
    X(0x195),X(0x191),X(0x18d),X(0x189),X(0x185),X(0x181),X(0x17e),X(0x17a),X(0x176),X(0x172),X(0x16f),X(0x16b),
    X(0x167),X(0x163),X(0x160),X(0x15c),X(0x158),X(0x154),X(0x151),X(0x14d),X(0x149),X(0x146),X(0x142),X(0x13e),
    X(0x13b),X(0x137),X(0x134),X(0x130),X(0x12c),X(0x129),X(0x125),X(0x122),X(0x11e),X(0x11b),X(0x117),X(0x114),
    X(0x110),X(0x10c),X(0x109),X(0x106),X(0x102),X(0x0ff),X(0x0fb),X(0x0f8),X(0x0f4),X(0x0f1),X(0x0ed),X(0x0ea),
    X(0x0e7),X(0x0e3),X(0x0e0),X(0x0dc),X(0x0d9),X(0x0d6),X(0x0d2),X(0x0cf),X(0x0cc),X(0x0c8),X(0x0c5),X(0x0c2),
    X(0x0be),X(0x0bb),X(0x0b8),X(0x0b5),X(0x0b1),X(0x0ae),X(0x0ab),X(0x0a8),X(0x0a4),X(0x0a1),X(0x09e),X(0x09b),
    X(0x098),X(0x094),X(0x091),X(0x08e),X(0x08b),X(0x088),X(0x085),X(0x082),X(0x07e),X(0x07b),X(0x078),X(0x075),
    X(0x072),X(0x06f),X(0x06c),X(0x069),X(0x066),X(0x063),X(0x060),X(0x05d),X(0x05a),X(0x057),X(0x054),X(0x051),
    X(0x04e),X(0x04b),X(0x048),X(0x045),X(0x042),X(0x03f),X(0x03c),X(0x039),X(0x036),X(0x033),X(0x030),X(0x02d),
    X(0x02a),X(0x028),X(0x025),X(0x022),X(0x01f),X(0x01c),X(0x019),X(0x016),X(0x014),X(0x011),X(0x00e),X(0x00b),
    X(0x008),X(0x006),X(0x003),X(0x000)};
#undef X

/* envelope increments: 8 steps of 4 bits for each rate */
static OPN2_RODATA uint32_t EG_INC[64] = {
    0x00000000, 0x00000000, 0x10101010, 0x10101010, 0x10101010, 0x10101010, 0x11101110, 0x11101110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x11111111, 0x21112111, 0x21212121, 0x22212221, 0x22222222, 0x42224222, 0x42424242, 0x44424442,
    0x44444444, 0x84448444, 0x84848484, 0x88848884, 0x88888888, 0x88888888, 0x88888888, 0x88888888};

static OPN2_RODATA uint8_t DETUNE[32][4] = {
    {0, 0, 1, 2}, {0, 0, 1, 2}, {0, 0, 1, 2}, {0, 0, 1, 2}, {0, 1, 2, 2}, {0, 1, 2, 3}, {0, 1, 2, 3}, {0, 1, 2, 3},
    {0, 1, 2, 4}, {0, 1, 3, 4}, {0, 1, 3, 4}, {0, 1, 3, 5}, {0, 2, 4, 5}, {0, 2, 4, 6}, {0, 2, 4, 6}, {0, 2, 5, 7},
    {0, 2, 5, 8}, {0, 3, 6, 8}, {0, 3, 6, 9}, {0, 3, 7, 10}, {0, 4, 8, 11}, {0, 4, 8, 12}, {0, 4, 9, 13}, {0, 5, 10, 14},
    {0, 5, 11, 16}, {0, 6, 12, 17}, {0, 6, 13, 19}, {0, 7, 14, 20}, {0, 8, 16, 22}, {0, 8, 16, 22}, {0, 8, 16, 22}, {0, 8, 16, 22}};

static OPN2_RODATA uint8_t PM_SHIFTS[8][8] = {
    {0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77}, {0x77, 0x77, 0x77, 0x77, 0x72, 0x72, 0x72, 0x72},
    {0x77, 0x77, 0x77, 0x72, 0x72, 0x72, 0x17, 0x17}, {0x77, 0x77, 0x72, 0x72, 0x17, 0x17, 0x12, 0x12},
    {0x77, 0x77, 0x72, 0x17, 0x17, 0x17, 0x12, 0x07}, {0x77, 0x77, 0x17, 0x12, 0x07, 0x07, 0x02, 0x01},
    {0x77, 0x77, 0x17, 0x12, 0x07, 0x07, 0x02, 0x01}, {0x77, 0x77, 0x17, 0x12, 0x07, 0x07, 0x02, 0x01}};

static OPN2_RODATA uint8_t LFO_MAX[8] = {109, 78, 72, 68, 63, 45, 9, 6};

static uint16_t wave[1024];                     /* full sine as attenuation, bit 15 = negative half */

OPN2_HOT static uint32_t eff_rate(uint32_t raw, uint32_t ksr) { return raw == 0 ? 0 : raw + ksr < 63u ? raw + ksr : 63u; }
OPN2_HOT static int32_t clamp9(int32_t v) { return v < -257 ? -257 : v > 256 ? 256 : v; }
OPN2_HOT static int32_t ladder(int32_t v) { return v < 0 ? v - 3 : v + 4; }     /* the YM2612's DAC discontinuity */

OPN2_HOT static int32_t pm_adjust(uint32_t fnum_bits, uint32_t pms, int32_t raw_pm)
{
    uint32_t a = (uint32_t)(raw_pm < 0 ? -raw_pm : raw_pm), sh = PM_SHIFTS[pms][a & 7u];
    int32_t adj = (int32_t)((fnum_bits >> (sh & 15u)) + (fnum_bits >> (sh >> 4)));
    if (pms > 5u) adj <<= pms - 5u;
    adj >>= 2;
    return raw_pm < 0 ? -adj : adj;
}

OPN2_HOT static uint32_t phase_step(const opn2_t *c, uint32_t choffs, const opn2_op_t *op, int32_t raw_pm)
{
    uint32_t bf = op->block_freq, fnum = BF(bf, 0, 11) << 1, pms = BF(c->regs[0xB4 + choffs], 0, 3), step;
    if (pms) {
        fnum += (uint32_t)pm_adjust(BF(bf, 4, 7), pms, raw_pm);
        fnum &= 0xFFFu;
    }
    step = (fnum << BF(bf, 11, 3)) >> 2;
    step += (uint32_t)op->detune;
    step &= 0x1FFFFu;
    return (step * op->multiple) >> 1;
}

OPN2_HOT static void cache_op(const opn2_t *c, uint32_t choffs, opn2_op_t *op)
{
    const uint8_t *r = c->regs;
    uint32_t o = op->offs, bf, keycode, dt, sl, ksr;
    bf = (uint32_t)(r[0xA4 + choffs] & 0x3Fu) << 8 | r[0xA0 + choffs];
    if ((r[0x27] & 0xC0u) && choffs == 2u) {   /* channel 3 special mode: O1..O3 have their own frequency */
        uint32_t n = o == 2u ? 1u : o == 10u ? 2u : o == 6u ? 0u : 3u;
        if (n < 3u) bf = (uint32_t)(r[0xAC + n] & 0x3Fu) << 8 | r[0xA8 + n];
    }
    op->block_freq = bf;
    keycode = BF(bf, 10, 4) << 1 | BF(0xFE80u, BF(bf, 7, 4), 1);
    dt = BF(r[0x30 + o], 4, 3);
    op->detune = (dt & 4u) ? -(int32_t)DETUNE[keycode][dt & 3u] : (int32_t)DETUNE[keycode][dt & 3u];
    op->multiple = (uint8_t)(BF(r[0x30 + o], 0, 4) * 2u);
    if (!op->multiple) op->multiple = 1;
    op->phase_step = (!(r[0x22] & 8u) || !BF(r[0xB4 + choffs], 0, 3)) ? phase_step(c, choffs, op, 0) : STEP_DYNAMIC;
    op->total_level = (uint16_t)(BF(r[0x40 + o], 0, 7) << 3);
    sl = BF(r[0x80 + o], 4, 4);
    sl |= (sl + 1u) & 0x10u;
    op->sustain = (uint16_t)(sl << 5);
    ksr = keycode >> (BF(r[0x50 + o], 6, 2) ^ 3u);
    op->rate[EG_ATTACK] = (uint8_t)eff_rate(BF(r[0x50 + o], 0, 5) * 2u, ksr);
    op->rate[EG_DECAY] = (uint8_t)eff_rate(BF(r[0x60 + o], 0, 5) * 2u, ksr);
    op->rate[EG_SUSTAIN] = (uint8_t)eff_rate(BF(r[0x70 + o], 0, 5) * 2u, ksr);
    op->rate[EG_RELEASE] = (uint8_t)eff_rate(BF(r[0x80 + o], 0, 4) * 4u + 2u, ksr);
    op->am_en = (uint8_t)BF(r[0x60 + o], 7, 1);
    op->ssg_en = (uint8_t)BF(r[0x90 + o], 3, 1);
    op->ssg_mode = (uint8_t)BF(r[0x90 + o], 0, 3);
}

OPN2_HOT static void start_attack(opn2_op_t *op, int restart)
{
    if (op->state == EG_ATTACK) return;
    op->state = EG_ATTACK;
    if (!restart) {
        op->ssg_inv = (uint8_t)(op->ssg_en & BF(op->ssg_mode, 2, 1));
        op->phase = 0;
    }
    if (op->rate[EG_ATTACK] >= 62u) op->env = 0;
}

OPN2_HOT static void start_release(opn2_op_t *op)
{
    if (op->state >= EG_RELEASE) return;
    op->state = EG_RELEASE;
    if (op->ssg_inv) {
        op->env = (uint16_t)((0x200u - op->env) & 0x3FFu);
        op->ssg_inv = 0;
    }
}

OPN2_HOT static int prepare_op(const opn2_t *c, uint32_t choffs, opn2_op_t *op)
{
    uint8_t ks = op->keyon_live != 0;
    cache_op(c, choffs, op);
    if (ks != op->key) {
        op->key = ks;
        if (ks) start_attack(op, 0);
        else start_release(op);
    }
    op->keyon_live &= (uint8_t)~4u;            /* the CSM key-on lasts one sample */
    return op->state != EG_RELEASE || op->env < EG_QUIET;
}

OPN2_HOT static void clock_ssg(opn2_op_t *op)
{
    uint32_t mode = op->ssg_mode;
    if (!(op->env & 0x200u)) return;
    if (mode & 1u) {                            /* hold */
        op->ssg_inv = (uint8_t)(BF(mode, 2, 1) ^ BF(mode, 1, 1));
        if (op->state != EG_ATTACK) op->env = op->ssg_inv ? 0x200u : 0x3FFu;
    } else {                                    /* repeat */
        op->ssg_inv ^= (uint8_t)BF(mode, 1, 1);
        if (op->state == EG_DECAY || op->state == EG_SUSTAIN) start_attack(op, 1);
        if (!BF(mode, 1, 1)) op->phase = 0;
    }
    if (op->state == EG_RELEASE) op->env = 0x3FFu;
}

OPN2_HOT static void clock_env(opn2_op_t *op, uint32_t cnt)
{
    uint32_t rate, shift, inc;
    if (op->state == EG_ATTACK && op->env == 0) op->state = EG_DECAY;
    if (op->state == EG_DECAY && op->env >= op->sustain) op->state = EG_SUSTAIN;
    rate = op->rate[op->state];
    shift = rate >> 2;
    cnt <<= shift;
    if (cnt & 0x7FFu) return;
    inc = BF(EG_INC[rate], 4u * BF(cnt, shift <= 11u ? 11u : shift, 3), 4);
    if (op->state == EG_ATTACK) {
        if (rate < 62u)                         /* (the unsigned wrap of ~env is ymfm's arithmetic, kept exactly) */
            op->env = (uint16_t)(op->env + ((~(uint32_t)op->env * inc) >> 4));
    } else {
        uint32_t e = op->env;
        if (!op->ssg_en) e += inc;
        else if (e < 0x200u) e += 4u * inc;
        op->env = (uint16_t)(e >= 0x400u ? 0x3FFu : e);
    }
}

OPN2_HOT static void clock_lfo(opn2_t *c, int32_t *pm)
{
    uint32_t sub, n, p;
    if (!(c->regs[0x22] & 8u)) {
        c->lfo_counter = 0;
        c->lfo_am = 0x3F;                       /* LFO off still leaves the AM offset of position 0 */
        *pm = 0;
        return;
    }
    sub = c->lfo_counter++ & 0xFFu;
    if (sub >= LFO_MAX[c->regs[0x22] & 7u]) c->lfo_counter += 0x101u - sub;
    n = c->lfo_counter;
    c->lfo_am = BF(n, 8, 6) ^ (BF(n, 14, 1) ? 0u : 0x3Fu);
    p = BF(n, 10, 3);
    if (BF(n, 13, 1)) p ^= 7u;
    *pm = BF(n, 14, 1) ? -(int32_t)p : (int32_t)p;
}

static void keyonoff(opn2_ch_t *ch, uint32_t mask)
{
    uint32_t i;
    for (i = 0; i < 4u; i++)
        ch->op[i].keyon_live = (uint8_t)((ch->op[i].keyon_live & ~1u) | BF(mask, i, 1));
}

static void fm_write(opn2_t *c, uint32_t idx, uint8_t data)
{
    c->modified = ALL_CH;
    if ((idx & 0xF0u) == 0xA0u) {               /* frequencies: the high byte latches until the low byte */
        uint32_t latch = 0xB8u | BF(idx, 3, 1);
        if ((idx & 3u) == 3u) return;
        if (idx & 4u) c->regs[latch] = data & 0x3Fu;
        else {
            c->regs[idx] = data;
            c->regs[idx | 4u] = c->regs[latch];
        }
        return;
    }
    if ((idx & 0xF8u) == 0xB8u) return;         /* the latches live here */
    c->regs[idx] = data;
    if (idx == 0x28u) {
        uint32_t n = BF(data, 0, 2);
        if (n == 3u) return;
        keyonoff(&c->ch[n + BF(data, 2, 1) * 3u], BF(data, 4, 4));
    }
}

void opn2_reset(opn2_t *c)
{
    static const uint8_t SLOT[4] = {0, 8, 4, 12};
    uint32_t i, j;
    uint8_t *p = (uint8_t *)c;
    for (i = 0; i < sizeof *c; i++) p[i] = 0;
    for (i = 0; i < 1024u; i++)
        wave[i] = (uint16_t)(SIN_ATT[((i & 0x100u) ? ~i : i) & 0xFFu] | (i & 0x200u) << 6);
    for (i = 0; i < 6u; i++) {
        opn2_ch_t *ch = &c->ch[i];
        ch->offs = (uint16_t)(i % 3u + 0x100u * (i / 3u));
        for (j = 0; j < 4u; j++) {
            ch->op[j].offs = (uint16_t)(ch->offs + SLOT[j]);
            ch->op[j].env = 0x3FF;
            ch->op[j].state = EG_RELEASE;
        }
    }
    c->regs[0xB4] = c->regs[0xB5] = c->regs[0xB6] = 0xC0;     /* both outputs on */
    c->regs[0x1B4] = c->regs[0x1B5] = c->regs[0x1B6] = 0xC0;
    c->modified = c->active = ALL_CH;
}

void opn2_write(opn2_t *c, uint32_t port, uint8_t data)
{
    switch (port & 3u) {
    case 0: c->address = data; break;
    case 2: c->address = (uint16_t)(0x100u | data); break;
    case 1:
        if (c->address & 0x100u) break;
        if (c->address == 0x2Au) c->dac_data = (uint16_t)((c->dac_data & ~0x1FEu) | (uint32_t)(data ^ 0x80u) << 1);
        else if (c->address == 0x2Bu) c->dac_enable = (uint8_t)BF(data, 7, 1);
        else if (c->address == 0x2Cu) c->dac_data = (uint16_t)((c->dac_data & ~1u) | BF(data, 3, 1));
        else fm_write(c, c->address, data);
        break;
    case 3:
        if (c->address & 0x100u) fm_write(c, c->address, data);
        break;
    }
}

void opn2_write_reg(opn2_t *c, uint32_t part, uint8_t reg, uint8_t data)
{
    opn2_write(c, part ? 2u : 0u, reg);
    opn2_write(c, part ? 3u : 1u, data);
}

/* ---- rendering, a block at a time. Each operator runs through the block with its state in registers, then
 * each channel combines its four operators sample by sample. Within a channel the operators meet only at the
 * combine, so this is sample for sample ymfm's one-sample-at-a-time order (tests/opn2_compare.cpp checks).
 * Loads cost the FM-1 three to four cycles, so the per-sample loops keep them few: an operator's phase and
 * attenuation share a word, and so do the mix's left and right. */
#define BLK 32u
#define SILENT 0x1000u                          /* an attenuation x4 whose volume is 0: ymfm's early out */
#define NO_ATT 0xFFFFu                          /* att_base() of an operator past EG_QUIET */

static int8_t b_pm[BLK];                        /* per sample of the block: the raw LFO PM */
static uint8_t b_am[BLK];                       /* .. the raw LFO AM */
static uint32_t b_op[4][BLK];                   /* .. each operator: phase (low 16 bits of 10.10 >> 10) | attenuation x4 << 16 */
static int32_t b_mix[BLK];                      /* .. the channels' sum: left + right * 65536 */

OPN2_HOT static void prepare_all(opn2_t *c)
{
    uint32_t i, j;
    c->active = 0;
    for (i = 0; i < 6u; i++) {
        uint32_t on = 0;
        for (j = 0; j < 4u; j++) on |= (uint32_t)prepare_op(c, c->ch[i].offs, &c->ch[i].op[j]);
        if (on) c->active |= 1u << i;
    }
    c->modified = c->prepare_count = 0;
}

OPN2_HOT static uint32_t att_base(const opn2_op_t *op)    /* env_att() without the AM and the clamp */
{
    uint32_t a = op->env;
    if (a > EG_QUIET) return NO_ATT;
    if (op->ssg_inv) a = (0x200u - a) & 0x3FFu;
    return a + op->total_level;
}

/* tick0: the first sample of the block on which the envelope runs (then every third), tcnt its counter >> 2 */
OPN2_HOT static void run_ops(const opn2_t *c, opn2_ch_t *ch, uint32_t n, uint32_t tick0, uint32_t tcnt)
{
    uint32_t amsh = (1u << (BF(c->regs[0xB4 + ch->offs], 4, 2) ^ 3u)) - 1u, j, s;
    for (j = 0; j < 4u; j++) {
        opn2_op_t *op = &ch->op[j];
        uint32_t *o = b_op[j];
        uint32_t phase = op->phase, step = op->phase_step, dyn = step == STEP_DYNAMIC, ssg = op->ssg_en;
        uint32_t am_en = op->am_en, nt = tick0, cnt = tcnt, base;
        int32_t last_pm = 0x7FFF;
        if (!ssg) {
            op->ssg_inv = 0;
            if (op->state == EG_RELEASE && op->env == 0x3FFu) {   /* silent until a key-on, which resets the phase */
                for (s = 0; s < n; s++) o[s] = SILENT << 16;
                continue;
            }
        }
        base = att_base(op);
        for (s = 0; s < n; s++) {
            uint32_t a;
            if (ssg) {
                op->phase = phase;
                clock_ssg(op);
                phase = op->phase;
                base = att_base(op);
            }
            if (s == nt) {
                clock_env(op, cnt);
                nt += 3u;
                cnt++;
                base = att_base(op);
            }
            if (dyn && b_pm[s] != last_pm) {    /* the PM moves slowly: a new step only when it changes */
                last_pm = b_pm[s];
                step = phase_step(c, ch->offs, op, last_pm);
            }
            phase += step;
            if (base == NO_ATT) a = SILENT;
            else {
                a = base;
                if (am_en) a += ((uint32_t)b_am[s] << 1) >> amsh;
                a = (a < 0x3FFu ? a : 0x3FFu) << 2;
            }
            o[s] = ((phase >> 10) & 0xFFFFu) | a << 16;
        }
        op->phase = phase;
    }
}

OPN2_HOT static int32_t vol(uint32_t op, int32_t mod)     /* op: a b_op word */
{
    uint32_t s = wave[(op + (uint32_t)mod) & 0x3FFu], a = (s & 0x7FFFu) + (op >> 16);
    int32_t v = (int32_t)(POW[a & 0xFFu] >> (a >> 8));
    return (s & 0x8000u) ? -v : v;
}

/* A computed channel: adds left + right * 65536 into b_mix. One loop per algorithm, chosen once a block: a switch
 * inside the loop costs the registers that keep the loop's pointers and constants. OUT computes r from o1 (and
 * O2..O4, in ymfm's order: >> 1 on an input, >> 5 out). */
#define CH_LOOP(OUT)                                                                        \
    for (s = 0; s < n; s++) {                                                               \
        int32_t o1, o2, o3, r, mod = 0;                                                     \
        fb0 = fb1;                                                                          \
        fb1 = fbin;                                                                         \
        if (fbk) mod = (fb0 + fb1) >> (10u - fbk);                                          \
        fbin = o1 = vol(q0[s], mod);                                                        \
        OUT;                                                                                \
        r = ladder(r);                          /* a side that is off gets ladder(0) = 4 */ \
        b_mix[s] += (lm ? r : 4) + (rm ? r * 65536 : 4 * 65536);                            \
    }

OPN2_HOT static void run_channel(const opn2_t *c, opn2_ch_t *ch, uint32_t n)
{
    const uint32_t *q0 = b_op[0], *q1 = b_op[1], *q2 = b_op[2], *q3 = b_op[3];
    uint32_t b4 = c->regs[0xB4 + ch->offs], b0 = c->regs[0xB0 + ch->offs], fbk = BF(b0, 3, 3), s;
    int32_t fb0 = ch->fb[0], fb1 = ch->fb[1], fbin = ch->fb_in;
    int32_t lm = (b4 & 0x80u) ? 1 : 0, rm = (b4 & 0x40u) ? 1 : 0;
    if (!(b4 & 0xC0u)) {                        /* both sides off: only operator 1 runs, for its feedback */
        for (s = 0; s < n; s++) {
            int32_t mod = 0;
            fb0 = fb1;
            fb1 = fbin;
            if (fbk) mod = (fb0 + fb1) >> (10u - fbk);
            fbin = vol(q0[s], mod);
            b_mix[s] += 4 + 4 * 65536;
        }
    } else switch (BF(b0, 0, 3)) {
    case 0:
        CH_LOOP(o2 = vol(q1[s], o1 >> 1); o3 = vol(q2[s], o2 >> 1); r = vol(q3[s], o3 >> 1) >> 5)
        break;
    case 1:
        CH_LOOP(o2 = vol(q1[s], 0); o3 = vol(q2[s], (o1 + o2) >> 1); r = vol(q3[s], o3 >> 1) >> 5)
        break;
    case 2:
        CH_LOOP(o2 = vol(q1[s], 0); o3 = vol(q2[s], o2 >> 1); r = vol(q3[s], (o1 + o3) >> 1) >> 5)
        break;
    case 3:
        CH_LOOP(o2 = vol(q1[s], o1 >> 1); o3 = vol(q2[s], 0); r = vol(q3[s], (o2 + o3) >> 1) >> 5)
        break;
    case 4:
        CH_LOOP(o2 = vol(q1[s], o1 >> 1); o3 = vol(q2[s], 0); r = vol(q3[s], o3 >> 1) >> 5;
                r = clamp9(r + (o2 >> 5)))
        break;
    case 5:
        CH_LOOP(o2 = vol(q1[s], o1 >> 1); o3 = vol(q2[s], o1 >> 1); r = vol(q3[s], o1 >> 1) >> 5;
                r = clamp9(clamp9(r + (o2 >> 5)) + (o3 >> 5)))
        break;
    case 6:
        CH_LOOP(o2 = vol(q1[s], o1 >> 1); o3 = vol(q2[s], 0); r = vol(q3[s], 0) >> 5;
                r = clamp9(clamp9(r + (o2 >> 5)) + (o3 >> 5)))
        break;
    default:
        CH_LOOP(o2 = vol(q1[s], 0); o3 = vol(q2[s], 0); r = vol(q3[s], 0) >> 5;
                r = clamp9(clamp9(clamp9(r + (o1 >> 5)) + (o2 >> 5)) + (o3 >> 5)))
        break;
    }
    ch->fb[0] = (int16_t)fb0;
    ch->fb[1] = (int16_t)fb1;
    ch->fb_in = (int16_t)fbin;
}
#undef CH_LOOP

OPN2_HOT static void idle_channel(opn2_ch_t *ch, uint32_t n)    /* the feedback shifts on, nothing new comes in */
{
    if (n >= 2u) ch->fb[0] = ch->fb[1] = ch->fb_in;
    else { ch->fb[0] = ch->fb[1]; ch->fb[1] = ch->fb_in; }
}

OPN2_HOT static int32_t scale(int32_t v)       /* v * 8192 / 390 as C divides (ymfm's average); exact for |v| < 2048 */
{
    uint32_t a = (uint32_t)(v < 0 ? -v : v), q = (a * 10754u + ((a * 321u) >> 9)) >> 9;
    return v < 0 ? -(int32_t)q : (int32_t)q;
}

OPN2_HOT void opn2_render(opn2_t *c, int32_t *out, uint32_t n)
{
    while (n) {
        uint32_t len, s, i, last, tick0 = BLK, tcnt = 0;
        int32_t dl = 0, dr = 0;
        if (c->modified || c->prepare_count >= 4096u) prepare_all(c);   /* new registers, or every 4096 samples */
        else c->prepare_count++;
        len = 1u + (4096u - c->prepare_count);  /* the block ends where the next periodic prepare is due */
        if (len > n) len = n;
        if (len > BLK) len = BLK;
        c->prepare_count += len - 1u;
        for (s = 0; s < len; s++) {
            int32_t pm;
            if ((++c->env_counter & 3u) == 3u) c->env_counter++;   /* the envelope runs every third sample */
            if (tick0 == BLK && !(c->env_counter & 3u)) {
                tick0 = s;
                tcnt = c->env_counter >> 2;
            }
            clock_lfo(c, &pm);
            b_pm[s] = (int8_t)pm;
            b_am[s] = (uint8_t)c->lfo_am;
            b_mix[s] = 0;
        }
        last = c->dac_enable ? 5u : 6u;         /* the DAC replaces channel 6 */
        for (i = 0; i < 6u; i++) {
            run_ops(c, &c->ch[i], len, tick0, tcnt);
            if (i < last && (c->active >> i & 1u)) run_channel(c, &c->ch[i], len);
            else {
                idle_channel(&c->ch[i], len);
                if (i < last) { dl += 4; dr += 4; }                  /* ladder(0) */
            }
        }
        if (c->dac_enable) {
            int32_t d = ladder((int16_t)(c->dac_data << 7) >> 7);
            dl += (c->regs[0x1B6] & 0x80u) ? d : 4;
            dr += (c->regs[0x1B6] & 0x40u) ? d : 4;
        }
        for (s = 0; s < len; s++) {
            int32_t m = b_mix[s], l = (int16_t)(m & 0xFFFF);
            out[2u * s] = scale(l + dl);
            out[2u * s + 1u] = scale(((m - l) >> 16) + dr);
        }
        out += 2u * len;
        n -= len;
    }
}

/* opn2_render may live in RAM (OPN2_HOT), out of a direct call's reach from flash */
static void (*volatile clock_render)(opn2_t *, int32_t *, uint32_t) = opn2_render;

void opn2_clock(opn2_t *c, int32_t *l, int32_t *r)
{
    int32_t o[2];
    clock_render(c, o, 1);
    *l = o[0];
    *r = o[1];
}
