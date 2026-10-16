/* SPDX-License-Identifier: GPL-3.0-only */
/* VGM player (format: https://vgmrips.net/wiki/VGM_Specification). Time runs in chip samples: a command due at VGM
 * sample T (44.1 kHz) is applied before chip sample floor(T * 53267.03 / 44100), and the chips render between
 * commands, so a write lands on its sample however close the next one is. tests/vgm_compare.cpp checks this
 * against ymfm and MAME's PSG run one sample at a time. */
#include "vgm.h"

#define CHIP_PER_VGM_Q16 79159u                 /* chip samples per VGM sample, Q16 */
#define PSG_STEP_Q16 275251u                    /* PSG ticks per chip sample, Q16 */
#define HZ_TO_INC 80631u                        /* stream Hz -> Q16 per chip sample, >> 16 (65536 * 65536 / 53267) */
#define STREAM_HZ_MAX 50000u                    /* keeps freq * HZ_TO_INC in 32 bits */

/* opn2_render and psg_render may live in RAM, out of a direct call's reach from flash */
static void (*volatile render_fm)(opn2_t *, int32_t *, uint32_t) = opn2_render;
static void (*volatile render_psg)(psg_t *, int32_t *, uint32_t, uint32_t) = psg_render;

static uint32_t rd32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* the length of the command at p (0: unknown), from the specification's table */
static uint32_t cmd_len(const uint8_t *p, uint32_t left)
{
    uint32_t c = p[0];
    if (c == 0x67u) return left >= 7u ? 7u + rd32(p + 3) : 0;
    if (c >= 0x30u && c <= 0x3Fu) return 2;
    if (c == 0x4Fu || c == 0x50u) return 2;
    if (c >= 0x40u && c <= 0x5Fu) return 3;
    if (c >= 0x61u && c <= 0x63u) return c == 0x61u ? 3u : 1u;
    if (c == 0x66u) return 1;
    if (c == 0x68u) return 12;
    if (c >= 0x70u && c <= 0x8Fu) return 1;
    if (c == 0x90u || c == 0x91u) return 5;
    if (c == 0x92u) return 6;
    if (c == 0x93u) return 11;
    if (c == 0x94u) return 2;
    if (c == 0x95u) return 5;
    if (c >= 0xA0u && c <= 0xBFu) return 3;
    if (c >= 0xC0u && c <= 0xDFu) return 4;
    if (c >= 0xE0u) return 5;
    return 0;
}

int vgm_open(vgm_t *v, const uint8_t *buf, uint32_t len)
{
    uint32_t ver, p, n;
    uint8_t *z = (uint8_t *)v;
    for (n = 0; n < sizeof *v; n++) z[n] = 0;
    if (len < 0x40u || buf[0] != 'V' || buf[1] != 'g' || buf[2] != 'm' || buf[3] != ' ') return VGM_ERR_MAGIC;
    ver = rd32(buf + 8);
    v->buf = buf;
    v->len = len;
    v->end = rd32(buf + 4) + 4u;
    if (v->end > len || v->end < 0x40u) v->end = len;
    v->start = ver >= 0x150u && rd32(buf + 0x34) ? 0x34u + rd32(buf + 0x34) : 0x40u;
    v->loop = rd32(buf + 0x1C) ? 0x1Cu + rd32(buf + 0x1C) : 0;
    if (v->start >= v->end || v->loop >= v->end) return VGM_ERR_SIZE;
    for (p = v->start; p < v->end;) {           /* walk the commands once: they must all be known; collect the PCM */
        uint32_t k = cmd_len(buf + p, v->end - p);
        if (!k || p + k > v->end) return VGM_ERR_CMD;
        if (buf[p] == 0x67u && buf[p + 2] == 0x00u) {   /* a YM2612 PCM block: the bank is their concatenation */
            if (v->nblk >= VGM_BLOCKS) return VGM_ERR_BLOCKS;
            v->blk_off[v->nblk] = p + 7u;
            v->blk_len[v->nblk] = k - 7u;
            v->blk_bank[v->nblk] = v->bank_len;
            v->bank_len += k - 7u;
            v->nblk++;
        }
            if (buf[p] == 0x66u) break;
        p += k;
    }
    v->mask = 0xFFFFFFFFu;
    return VGM_OK;
}

int vgm_open_stream(vgm_t *v, const uint8_t *image, uint32_t image_len, const uint8_t *ring, uint32_t mask,
                    const volatile uint32_t *fill)
{
    uint32_t n, i, at;
    uint8_t *z = (uint8_t *)v;
    for (i = 0; i < sizeof *v; i++) z[i] = 0;
    if (image_len < 4u) return VGM_ERR_SIZE;
    n = rd32(image);
    if (n > VGM_BLOCKS) return VGM_ERR_BLOCKS;
    if (4u + 4u * n > image_len) return VGM_ERR_SIZE;
    at = 0;
    for (i = 0; i < n; i++) {
        v->blk_len[i] = rd32(image + 4u + 4u * i);
        v->blk_bank[i] = at;
        at += v->blk_len[i];
        if (at > image_len) return VGM_ERR_SIZE;
    }
    v->nblk = n;
    v->bank = image + 4u + 4u * n;
    v->bank_len = at;
    if (4u + 4u * n + at > image_len) return VGM_ERR_SIZE;
    v->buf = ring;
    v->mask = mask;
    v->fill = fill;
    return VGM_OK;
}

static uint32_t bank_byte(const vgm_t *v, uint32_t at)
{
    uint32_t i;
    if (v->bank) return at < v->bank_len ? v->bank[at] : 0x80u;
    for (i = 0; i < v->nblk; i++)               /* (usually one block, or a few) */
        if (at - v->blk_bank[i] < v->blk_len[i]) return v->buf[v->blk_off[i] + at - v->blk_bank[i]];
    return 0x80;                                /* past the data: the DAC's middle */
}

void vgm_play(vgm_t *v, opn2_t *c, psg_t *p)
{
    uint32_t i;
    opn2_reset(c);
    psg_reset(p);
    v->pos = v->start;
    v->wait = v->acc = v->bank_pos = v->played = 0;
    v->starved = 0;
    for (i = 0; i < VGM_STREAMS; i++) v->st[i].used = v->st[i].on = 0;
    v->playing = v->buf != 0;
}

void vgm_stop(vgm_t *v, opn2_t *c, psg_t *p)
{
    static const uint8_t KEY[6] = {0, 1, 2, 4, 5, 6};
    uint32_t i;
    v->playing = 0;
    for (i = 0; i <= VGM_STREAMS; i++) v->st[i].on = 0;
    for (i = 0; i < 6u; i++) {                  /* every key up, the DAC off, the PSG quiet */
        opn2_write(c, 0, 0x28);
        opn2_write(c, 1, KEY[i]);
    }
    opn2_write(c, 0, 0x2B);
    opn2_write(c, 1, 0);
    for (i = 0; i < 4u; i++) psg_write(p, (uint8_t)(0x9F | i << 5));
}

static vgm_stream_t *stream(vgm_t *v, uint32_t id, int make)
{
    uint32_t i;
    for (i = 0; i < VGM_STREAMS; i++)
        if (v->st[i].used && v->st[i].id == id) return &v->st[i];
    if (!make) return 0;
    for (i = 0; i < VGM_STREAMS; i++)
        if (!v->st[i].used) {
            vgm_stream_t *s = &v->st[i];
            uint8_t *z = (uint8_t *)s;
            uint32_t k;
            for (k = 0; k < sizeof *s; k++) z[k] = 0;
            s->used = 1;
            s->id = (uint8_t)id;
            s->step_size = 1;
            return s;
        }
    return 0;
}

static void stream_start(vgm_stream_t *s, uint32_t start, uint32_t count, int loop)
{
    s->start = s->pos = start;
    s->len = s->left = count;
    s->loop = (uint8_t)loop;
    s->phase = 0;
    s->on = count != 0;
}

static void ym_write(opn2_t *c, uint32_t part, uint32_t reg, uint32_t val)
{
    opn2_write(c, part ? 2u : 0u, (uint8_t)reg);
    opn2_write(c, part ? 3u : 1u, (uint8_t)val);
}

/* apply every command due now (less than one chip sample away) */
static void run_commands(vgm_t *v, opn2_t *c, psg_t *ps)
{
    uint8_t t[VGM_RING_CMD];
    while (v->playing && v->wait == 0 && v->acc < 65536u) {
        const uint8_t *p;
        uint32_t cmd, k, n, left;
        if (v->fill) {                          /* a stream: the command out of the ring, if it has all arrived */
            left = *v->fill - v->pos;
            n = left < VGM_RING_CMD ? left : VGM_RING_CMD;
            for (k = 0; k < n; k++) t[k] = v->buf[(v->pos + k) & v->mask];
            if (!n || (k = cmd_len(t, n)) > n) {
                if (!v->starved) v->starve_count++;
                v->starved = 1;
                break;
            }
            if (!k || t[0] == 0x67u) { v->playing = 0; break; }
            p = t;
        } else {
            left = v->end - v->pos;
            p = v->buf + v->pos;
            k = cmd_len(p, left);
            if (!k || k > left) { v->playing = 0; break; }
        }
        v->starved = 0;
        cmd = p[0];
        v->pos += k;
        switch (cmd) {
        case 0x50: psg_write(ps, p[1]); break;
        case 0x52: ym_write(c, 0, p[1], p[2]); break;
        case 0x53: ym_write(c, 1, p[1], p[2]); break;
        case 0x61: v->wait += p[1] | (uint32_t)p[2] << 8; break;
        case 0x62: v->wait += 735u; break;
        case 0x63: v->wait += 882u; break;
        case 0x66:                              /* (a stream's sender unrolls the loops) */
            if (v->loops && v->loop && !v->fill) v->pos = v->loop;
            else v->playing = 0;
            break;
        case 0xE0: v->bank_pos = rd32(p + 1); break;
        case 0x90: {                            /* stream setup: which chip and register */
            vgm_stream_t *s = stream(v, p[1], 1);
            if (s) { s->port = p[3]; s->reg = p[4]; }
            if (s && (p[2] & 0x7Fu) != 0x02u) s->used = 0;   /* not the YM2612: not ours */
        } break;
        case 0x91: {                            /* its data: bank (YM2612 PCM only), step size and base */
            vgm_stream_t *s = stream(v, p[1], 0);
            if (s) { s->step_size = p[3] ? p[3] : 1u; s->step_base = p[4]; }
        } break;
        case 0x92: {
            vgm_stream_t *s = stream(v, p[1], 0);
            if (s) {
                s->freq = rd32(p + 2);
                if (s->freq > STREAM_HZ_MAX) s->freq = STREAM_HZ_MAX;
                s->inc = (s->freq * HZ_TO_INC) >> 16;
            }
        } break;
        case 0x93: {                            /* start at an offset; length in bytes, ms or to the end */
            vgm_stream_t *s = stream(v, p[1], 0);
            uint32_t off = rd32(p + 2), mode = p[6], len = rd32(p + 7), at;
            if (!s) break;
            at = off == 0xFFFFFFFFu ? s->start : off + s->step_base;   /* (all ones: where it was) */
            switch (mode & 3u) {
            case 1: n = len; break;
            case 2: n = s->freq / 10u * len / 100u; break;
            case 3: n = at < v->bank_len ? (v->bank_len - at) / s->step_size : 0; break;
            default: n = s->len; break;
            }
            stream_start(s, at, n, (mode & 0x80u) != 0);
        } break;
        case 0x94:
            for (n = 0; n < VGM_STREAMS; n++)
                if (v->st[n].used && (p[1] == 0xFFu || v->st[n].id == p[1])) v->st[n].on = 0;
            break;
        case 0x95: {                            /* start block bbbb of the bank */
            vgm_stream_t *s = stream(v, p[1], 0);
            uint32_t id = p[2] | (uint32_t)p[3] << 8;
            if (s && id < v->nblk)
                stream_start(s, v->blk_bank[id] + s->step_base, v->blk_len[id] / s->step_size, p[4] & 1u);
        } break;
        default:
            if (cmd >= 0x70u && cmd <= 0x7Fu) v->wait += (cmd & 15u) + 1u;
            else if (cmd >= 0x80u && cmd <= 0x8Fu) {   /* a DAC byte from the bank, then wait n */
                ym_write(c, 0, 0x2A, bank_byte(v, v->bank_pos++));
                v->wait += cmd & 15u;
            }
            break;                              /* (other chips' commands, data blocks: nothing to do now) */
        }
        if (v->wait) {
            v->played += v->wait;
            while (v->wait && v->acc < 65536u) {    /* VGM samples -> chip samples, a chunk at a time (32 bits) */
                uint32_t w = v->wait < 16384u ? v->wait : 16384u;
                v->acc += w * CHIP_PER_VGM_Q16;
                v->wait -= w;
            }
        }
    }
}

static void run_streams(vgm_t *v, opn2_t *c)
{
    uint32_t i;
    for (i = 0; i <= VGM_STREAMS; i++) {
        vgm_stream_t *s = &v->st[i];
        while (s->on && s->phase >= 65536u) {
            s->phase -= 65536u;
            if (s->data) {                      /* a drum: its sample, at its level */
                int32_t b = ((int32_t)s->data[s->pos] - 128) * s->gain / 256 + 128;
                ym_write(c, s->port, s->reg, (uint32_t)(b < 0 ? 0 : b > 255 ? 255 : b));
            } else
                ym_write(c, s->port, s->reg, bank_byte(v, s->pos));
            s->pos += s->step_size;
            if (--s->left == 0) {
                if (s->loop) { s->pos = s->start; s->left = s->len; }
                else s->on = 0;
            }
        }
    }
}

/* chip samples until something is due, at most max (>= 1) */
static uint32_t until_next(const vgm_t *v, uint32_t max)
{
    uint32_t i, k = max;
    if (v->playing && !v->starved && (v->acc >> 16) < k) k = v->acc >> 16;   /* (acc, not wait: refills on elapse) */
    for (i = 0; i <= VGM_STREAMS; i++) {
        const vgm_stream_t *s = &v->st[i];
        if (s->on && s->inc) {
            uint32_t a = (65536u - s->phase + s->inc - 1u) / s->inc;
            if (a < k) k = a;
        }
    }
    return k ? k : 1u;
}

static void elapse(vgm_t *v, uint32_t k)
{
    uint32_t i;
    if (v->starved) return;                     /* the song's time stands still until bytes come */
    if (v->playing) {
        v->acc -= k << 16;
        while (v->wait && v->acc < 65536u) {
            uint32_t w = v->wait < 16384u ? v->wait : 16384u;
            v->acc += w * CHIP_PER_VGM_Q16;
            v->wait -= w;
        }
    }
    for (i = 0; i <= VGM_STREAMS; i++)
        if (v->st[i].on) v->st[i].phase += k * v->st[i].inc;
}

void vgm_drum(vgm_t *v, opn2_t *c, const uint8_t *data, uint32_t len, uint32_t rate, int32_t gain)
{
    vgm_stream_t *s = &v->st[VGM_STREAMS];
    if (rate > STREAM_HZ_MAX) rate = STREAM_HZ_MAX;
    s->used = 1;
    s->port = 0;
    s->reg = 0x2A;
    s->data = data;
    s->gain = gain;
    s->step_size = 1;
    s->freq = rate;
    s->inc = (rate * HZ_TO_INC) >> 16;
    s->phase = 65536u;                          /* its first byte now, not a period from now */
    ym_write(c, 0, 0x2B, 0x80);                 /* the DAC on: it replaces FM channel 6 */
    s->start = s->pos = 0;
    s->len = s->left = len;
    s->loop = 0;
    s->on = len != 0;
}

void vgm_render(vgm_t *v, opn2_t *c, psg_t *p, int32_t *out, uint32_t n, int32_t fm_gain, int32_t psg_gain)
{
    static int32_t pbuf[2u * 64u];
    uint32_t t = 0, i;
    while (t < n) {
        uint32_t k = n - t;
        if (k > 64u) k = 64u;
        run_commands(v, c, p);
        run_streams(v, c);
        k = until_next(v, k);
        render_fm(c, out + 2u * t, k);
        for (i = 0; i < 2u * k; i++) pbuf[i] = 0;
        render_psg(p, pbuf, k, PSG_STEP_Q16);
        for (i = 0; i < 2u * k; i++)
            out[2u * t + i] = (out[2u * t + i] * fm_gain + pbuf[i] * psg_gain) >> 8;
        elapse(v, k);
        t += k;
    }
}
