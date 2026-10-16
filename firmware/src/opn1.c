/* SPDX-License-Identifier: GPL-3.0-only */
/* OPN-1: the Mega Drive's sound chips (YM2612 + PSG) as an instrument on the FM-1.
 *
 *   MIDI (USB)   GenMDM v100's layout: channels 1-6 the FM channels, 7-9 the PSG tones, 10 the PSG noise
 *                (the note picks the noise: see NOISE_MODE). FM CCs as GenMDM: 14 algorithm, 15 feedback,
 *                16-19 total level (127 = loudest), 20-23 multiple, 24-27 detune, 39-42 rate scaling,
 *                43-46 attack, 47-50 decay 1, 51-54 decay 2, 55-58 sustain level, 59-62 release,
 *                70-73 AM on, 75 PMS, 76 AMS, 77 stereo, 1 LFO speed, 74 LFO on, 81 bend range, 9 preset.
 *                Program change picks a preset too.
 *   keys         play the panel channel (SELECT turns it): FM 1-6, PSG 1-3, NOISE, DRUMS (the kit, key F3 its first
 *                sound) or POLY (six FM voices, five with a kit)
 *   drums        a kit from the page (web/index.html, kept in flash): MIDI channel 10 plays it (note 36 = C2
 *                the first sound) instead of the PSG noise, which MIDI channel 11 always plays as well. One sound at a time through the YM2612's DAC, which
 *                then replaces FM channel 6, as on the console. Velocity sets its level.
 *   PRESETS      the instrument of the panel's FM channel (POLY: all six), from the sound set sent from the page
 *   OCT- OCT+    the octave
 *   PLAY         stops the song the page is playing
 *
 * Songs: web/index.html streams a VGM while it plays (nothing is kept). First the song's PCM bank as an image
 * (vgm.h), checked by CRC; then the commands into a 128 KB ring, which the page keeps topped up. All on MIDI
 * channels 15 and 16 (SysEx belongs to the updater), in chunks the FM-1 confirms, as Linux may drop messages:
 *   chunk         CC 6A 6B 6C where it goes (bytes from the bank's or the stream's start, mod 2^21, 7 bits each,
 *                 low first), up to 128 data messages, CC 6D 6E 6F its CRC-16 (7, 7, 2 bits; the last ends it)
 *                 over the offset's three bytes (little-endian) then the data.
 *                 Each data message carries 16 bits: poly aftertouch or pitch bend on channel 15 or 16 gives the
 *                 top two (AE, EE, AF, EF = 0..3), the data bytes the low 14; the 16 bits are two bytes, low first.
 *                 The FM-1 keeps a chunk only if it starts where its data ends and its CRC holds, and answers
 *                 every chunk with how many bytes it holds (CC 65 66 67, mod 2^21): the page sends on from there.
 *   page -> FM-1  CC 70 the bank begins (answer CC 6E 70), 78-7C its CRC32 in 7-bit groups (low first), 7D-7F
 *                 its length, its chunks, 72 the bank ends (answer CC 73: 0 ok, 1 length, 2 CRC, 3 too big, 10+
 *                 vgm.h's error); CC 60 the commands begin (answer CC 6E 60), their chunks; 75 play (1) / stop
 *                 (0) (answer CC 74). Every one of these may come more than once: the page repeats until answered.
 *                 CC 61 a drum kit begins (answer 6E 61), then as the bank: CRC, length, chunks, 63 the end (answer 73).
 *                 CC 4E keep the sound set (instruments, PSG shapes, kit) in flash for the next boot (answers 6E 4E
 *                 at once, then 4D: 0 kept, 3 too big, 4 erase, 5 write).
 *   FM-1 -> page  CC 77 each KB of commands played (mod 128), 7B a dry spell (count mod 128), 74 playing or not.
 *
 * The chips run in the audio interrupt: the YM2612 at its own 53267 Hz in blocks (opn2.c), resampled to 44.1 kHz,
 * the PSG straight at 44.1 kHz (psg.c). The main loop never touches them: it queues register writes (wq_*),
 * which the interrupt applies before it renders. */
#define APP_NAME "OPN-1"                      /* the rescue and the crash screen say this */
#include "platform.c"
#include "opn2.h"
#include "psg.h"
#include "vgm.h"

#ifdef DINGHY_HOST
#define FM1_TICKS_PER_US 24u
static uint32_t fm1_ticks(void) { return fm1_ms * 24000u; }   /* host: the same 24 MHz units, simulated time */
static uint32_t crc32(const uint8_t *p, uint32_t n)    /* host: as platform.c's */
{
    uint32_t c = 0xFFFFFFFFu, k;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8u; k++) c = c & 1u ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return ~c;
}
#endif

/* ---- presets: operator order O1..O4 (registers +0, +8, +4, +12) */
typedef struct { uint8_t dt, mul, tl, ks, ar, am, d1r, d2r, sl, rr, ssg; } op_t;
typedef struct {
    char name[12];
    uint8_t alg, fb, ams, pms;
    op_t op[4];
    uint8_t lfo;                                /* 0x80 | register 0x22 its song had (it uses the LFO); 0: leave it */
    uint8_t oct;                                /* 64 + the panel octave its song played it in; 0: leave it */
    uint8_t vib_depth, vib_rate, vib_delay;     /* its driver's vibrato: cents, 0.1 Hz, 10 ms (depth 0: none) */
} patch_t;

static const uint8_t OPOFF[4] = {0, 8, 4, 12};  /* operator O1..O4 -> register offset */
static const uint8_t CARRIERS[8] = {0x8, 0x8, 0x8, 0x8, 0xA, 0xE, 0xE, 0xF};   /* bit k: Ok+1 is a carrier */

/* ---- the register write queue: the main loop writes, the audio interrupt applies (one writer each). Each write
 * carries the chip sample it is due at, and the interrupt applies it at that place in the next buffer: a note is
 * always about one buffer (5.8 ms) late, rather than anywhere from 0 to 5.8 ms by where in the buffer it came */
#define WQ 1024u
enum { DEV_OPN0, DEV_OPN1, DEV_OPN2, DEV_OPN3, DEV_PSG, DEV_DRUM_VEL, DEV_DRUM, DEV_TICK };   /* DEV_DRUM: a kit sound, 0xFF stops */
static uint32_t wq[WQ];                         /* the due sample (mod 2^16) << 16 | dev << 8 | data */
static volatile uint32_t wq_w, wq_r;
static volatile uint32_t chip_pos;              /* chip samples rendered (the interrupt's) */
static volatile uint32_t isr_pos, isr_t, isr_seq;   /* the last buffer: where it ended, when it began; odd seq: changing */

static uint32_t due_now(void)                   /* the chip sample a write made now is due at */
{
    uint32_t sq, pos, t, us;
    do { sq = isr_seq; pos = isr_pos; t = isr_t; } while ((sq & 1u) || sq != isr_seq);
    us = (fm1_ticks() - t) / FM1_TICKS_PER_US;
    if (us > 20000u) us = 20000u;               /* (no interrupt for that long: soon, not in the far future) */
    return pos + us * 53267u / 1000000u;
}

static vgm_t vgm;

/* a write, or a pair the interrupt must apply together (an address and its data: a song's writes between the two
 * would move the address). While a song plays the chips are its alone: the panel's writes are dropped, and
 * restore_panel puts its sounds back after */
static void q2(uint32_t dev, uint32_t data, uint32_t dev2, uint32_t data2, uint32_t n)
{
    uint32_t w = wq_w, due;
    if (vgm.playing) return;
    while (w + n - wq_r > WQ) platform_poll();  /* full: the interrupt empties it every 5.8 ms */
    due = due_now() << 16;
    wq[w % WQ] = due | dev << 8 | (data & 0xFFu);
    if (n > 1u) wq[(w + 1u) % WQ] = due | dev2 << 8 | (data2 & 0xFFu);
    wq_w = w + n;
}
static void q(uint32_t dev, uint32_t data) { q2(dev, data, 0, 0, 1); }
static void fm(uint32_t part, uint32_t reg, uint32_t val) { q2(part * 2u, reg, part * 2u + 1u, val, 2); }

/* ---- the sound (audio interrupt) */
#define CBLK 32u
#define CHIP_STEP_Q16 79159u                    /* chip samples per output frame: 53267.03 / 44100, Q16 */
#define FM_GAIN 192                             /* /256; the FM : PSG balance is set by ear, not measured */
#define PSG_GAIN 96
#define BANK_MAX (128u * 1024u)                 /* the song's PCM (its drums) */
#define KIT_MAX (48u * 1024u)                   /* the drum kit from the page */
#define KIT_SOUNDS 32u
#define RING_SIZE (128u * 1024u)                /* its commands, streamed while it plays */
#define RING_MASK (RING_SIZE - 1u)

static opn2_t chip;
static psg_t psg;
static uint8_t bank_buf[BANK_MAX] __attribute__((section(".pool")));
static uint8_t ring_buf[RING_SIZE] __attribute__((section(".pool")));
static uint8_t kit_buf[KIT_MAX] __attribute__((section(".pool")));
static uint8_t kit_n;                           /* sounds in the kit (0: none; MIDI channel 10 is then the PSG noise) */
static uint32_t kit_off[KIT_SOUNDS], kit_len[KIT_SOUNDS], kit_rate[KIT_SOUNDS];
static uint32_t kit_bytes;                      /* the kit image's length (for the store) */
static uint8_t drum_vel = 127;                  /* (the interrupt's: set by DEV_DRUM_VEL before each DEV_DRUM) */
static volatile uint32_t ring_fill;             /* bytes written to the ring, ever (the main loop the only writer) */
enum { SONG_NONE, SONG_START, SONG_STOP };
static volatile uint8_t song_req;               /* main loop -> interrupt; the interrupt clears it when done */
static int32_t cbuf[2u * CBLK];
static uint32_t cpos = CBLK, rfrac;
static int32_t s0l, s0r, s1l, s1r;              /* the two chip samples the output falls between */
static volatile uint32_t load_ticks;            /* the interrupt's time, the worst since the main loop read it */

static void wq_apply(uint32_t upto)             /* the queued writes due by chip sample upto */
{
    while (wq_r != wq_w) {
        uint32_t e = wq[wq_r % WQ];
        if ((int16_t)(uint16_t)((e >> 16) - upto) > 0) break;   /* (in order: the rest are later) */
        e &= 0xFFFFu;
        if ((e >> 8) == DEV_PSG) psg_write(&psg, (uint8_t)e);
        else if ((e >> 8) == DEV_DRUM_VEL) drum_vel = (uint8_t)e;
        else if ((e >> 8) == DEV_DRUM) {
            uint32_t k = e & 0xFFu;
            if (k < kit_n) vgm_drum(&vgm, &chip, kit_buf + kit_off[k], kit_len[k], kit_rate[k], (int32_t)(drum_vel * 258u >> 7));
            else vgm.st[VGM_STREAMS].on = 0;
        }
        else if ((e >> 8) == DEV_TICK) { int32_t l, r; opn2_clock(&chip, &l, &r); }
        else opn2_write(&chip, e >> 8, (uint8_t)e);
        wq_r++;
    }
}

static void app_audio(int32_t *out, uint32_t n)
{
    uint32_t i, t0 = fm1_ticks(), dt;
    if (song_req == SONG_STOP) vgm_stop(&vgm, &chip, &psg);
    else if (song_req == SONG_START) vgm_play(&vgm, &chip, &psg);
    song_req = SONG_NONE;
    for (i = 0; i < n; i++) {
        int32_t l, r;
        rfrac += CHIP_STEP_Q16;
        while (rfrac >= 65536u) {               /* linear interpolation between chip samples */
            rfrac -= 65536u;
            if (cpos >= CBLK) {                 /* the chips, and the song's writes at their samples */
                wq_apply(chip_pos + CBLK / 2u);
                vgm_render(&vgm, &chip, &psg, cbuf, CBLK, FM_GAIN, PSG_GAIN);
                chip_pos += CBLK;
                cpos = 0;
            }
            s0l = s1l; s0r = s1r;
            s1l = cbuf[2u * cpos];
            s1r = cbuf[2u * cpos + 1u];
            cpos++;
        }
        l = s0l + (((s1l - s0l) * (int32_t)(rfrac >> 2)) >> 14);
        r = s0r + (((s1r - s0r) * (int32_t)(rfrac >> 2)) >> 14);
        out[2u * i] += l;
        out[2u * i + 1u] += r;
    }
    isr_seq++;                                  /* (for due_now: where this buffer ended, when it began) */
    isr_pos = chip_pos;
    isr_t = t0;
    isr_seq++;
    dt = fm1_ticks() - t0;
    if (dt > load_ticks) load_ticks = dt;
}

/* ---- the voices (main loop) */
static const uint16_t FNUM[13] = {644, 682, 723, 766, 811, 859, 910, 965, 1022, 1083, 1147, 1215, 1288};   /* C4..C5 at block 4 */
static const uint32_t PSG_PER[13] = {218911, 206624, 195028, 184081, 173750, 163998, 154793, 146106,
                                     137905, 130165, 122860, 115964, 109456};   /* MIDI 0..12, x16 */
static const uint8_t NOISE_MODE[12] = {0, 0, 1, 1, 2, 4, 5, 6, 6, 3, 3, 7};   /* GenMDM: the note's name picks it */

typedef struct {
    patch_t p;
    uint8_t preset, note, vel, on, bend_range, pan;   /* pan: register 0xB4's bits 7-6 */
    int16_t bend;                               /* -8192..8191 */
    int16_t vib;                                /* the vibrato's offset now, cents */
    int8_t det;                                 /* cents: POLY's doubling sets the pair apart */
    uint32_t t_on;                              /* fm1_ms at its note-on */
} fmch_t;
static fmch_t fmc[6];
static struct {
    uint8_t note, vel, on, bend_range;
    int16_t bend;
    uint8_t inst, att;                          /* inst: PSG instrument + 1 (0: plain); att: the attenuation now */
    uint32_t t_on;
} psc[4];

/* PSG instruments from the page (until power-off): a volume shape, the attenuation each 60 Hz frame from the note's
 * start (0 = its loudest, which velocity sets), its last value held; for noise, the noise type */
#define PSG_SET_MAX 16u
#define PSG_ENV_MAX 64u
typedef struct { char name[12]; uint8_t len, noise; uint8_t env[PSG_ENV_MAX]; } psgi_t;   /* noise: 0x80 | type */
static psgi_t psg_set[PSG_SET_MAX];
static uint8_t psg_set_n;
static uint8_t lfo_reg = 0;                     /* register 0x22 */
static int8_t tune;                             /* cents, every voice (GLOBAL page) */

static void fm_patch(uint32_t ch)               /* the whole patch to the chip, the carriers by velocity */
{
    const patch_t *p = &fmc[ch].p;
    uint32_t part = ch / 3u, c = ch % 3u, k;
    fm(part, 0xB0 + c, (uint32_t)(p->fb & 7u) << 3 | (p->alg & 7u));
    fm(part, 0xB4 + c, (uint32_t)fmc[ch].pan << 6 | (uint32_t)(p->ams & 3u) << 4 | (p->pms & 7u));
    for (k = 0; k < 4u; k++) {
        const op_t *o = &p->op[k];
        uint32_t r = OPOFF[k] + c, tl = o->tl;
        if (CARRIERS[p->alg & 7u] >> k & 1u) {
            tl += (127u - fmc[ch].vel) >> 2;
            if (tl > 127u) tl = 127u;
        }
        fm(part, 0x30 + r, (uint32_t)(o->dt & 7u) << 4 | (o->mul & 15u));
        fm(part, 0x40 + r, tl);
        fm(part, 0x50 + r, (uint32_t)(o->ks & 3u) << 6 | (o->ar & 31u));
        fm(part, 0x60 + r, (uint32_t)(o->am & 1u) << 7 | (o->d1r & 31u));
        fm(part, 0x70 + r, o->d2r & 31u);
        fm(part, 0x80 + r, (uint32_t)(o->sl & 15u) << 4 | (o->rr & 15u));
        fm(part, 0x90 + r, o->ssg & 15u);
    }
}

static void fm_level(uint32_t ch)               /* the carriers' levels only (velocity, a TL change) */
{
    const patch_t *p = &fmc[ch].p;
    uint32_t k;
    for (k = 0; k < 4u; k++) {
        uint32_t tl = p->op[k].tl;
        if (CARRIERS[p->alg & 7u] >> k & 1u) {
            tl += (127u - fmc[ch].vel) >> 2;
            if (tl > 127u) tl = 127u;
        }
        fm(ch / 3u, 0x40 + OPOFF[k] + ch % 3u, tl);
    }
}

/* the pitch of note + bend (in 1/8192 of the bend range) as an F-number and block */
static void fm_pitch(uint32_t ch)
{
    fmch_t *v = &fmc[ch];
    int32_t cents = (int32_t)v->note * 100 + (int32_t)v->bend * v->bend_range * 100 / 8192 + v->vib + v->det + tune;
    int32_t n, semi, oct, frac;
    uint32_t f, block;
    if (cents < 0) cents = 0;
    n = cents / 100;
    frac = cents % 100;
    semi = n % 12;
    oct = n / 12 - 1;                           /* MIDI 60 = C4 = block 4 */
    f = FNUM[semi] + (uint32_t)((FNUM[semi + 1] - FNUM[semi]) * frac / 100);
    if (oct < 0) { f >>= -oct; block = 0; }
    else if (oct > 7) { f <<= oct - 7; block = 7; }
    else block = (uint32_t)oct;
    if (f > 2047u) f = 2047u;
    fm(ch / 3u, 0xA4 + ch % 3u, block << 3 | f >> 8);
    fm(ch / 3u, 0xA0 + ch % 3u, f & 0xFFu);
}

static void fm_key(uint32_t ch, uint32_t on) { fm(0, 0x28, (on ? 0xF0u : 0u) | (ch % 3u) | (ch / 3u) << 2); }

static void fm_note_on(uint32_t ch, uint32_t note, uint32_t vel, int32_t det)
{
    fmch_t *v = &fmc[ch];
    fm_key(ch, 0);                              /* mono: a new note restarts the envelopes */
    q(DEV_TICK, 0);                             /* (the chip sees a key change only between its samples) */
    v->note = (uint8_t)note;
    v->det = (int8_t)det;
    v->vib = 0;                                 /* (before the pitch: not the last note's vibrato) */
    if (vel != v->vel) { v->vel = (uint8_t)vel; fm_level(ch); }
    fm_pitch(ch);
    fm_key(ch, 1);
    v->on = 1;
    v->t_on = fm1_ms;
}
static void fm_note_off(uint32_t ch, uint32_t note)
{
    if (fmc[ch].on && fmc[ch].note == note) { fm_key(ch, 0); fmc[ch].on = 0; }
}

static void psg_pitch(uint32_t ch)              /* tones 0..2 */
{
    int32_t cents = (int32_t)psc[ch].note * 100 + (int32_t)psc[ch].bend * psc[ch].bend_range * 100 / 8192 + tune;
    int32_t n, semi, oct, frac;
    uint32_t per;
    if (cents < 0) cents = 0;
    n = cents / 100; frac = cents % 100; semi = n % 12; oct = n / 12;
    per = PSG_PER[semi] - (uint32_t)((int32_t)(PSG_PER[semi] - PSG_PER[semi + 1]) * frac / 100);
    per = (per + (8u << oct)) >> (oct + 4);     /* x16 -> the period, rounded */
    if (per > 1023u) per = 1023u;
    if (per < 1u) per = 1u;
    q2(DEV_PSG, 0x80 | ch << 5 | (per & 15u), DEV_PSG, per >> 4, 2);
}
static void psg_vol(uint32_t ch, uint32_t att) { q(DEV_PSG, 0x90 | ch << 5 | (att & 15u)); }

static uint32_t psg_att(uint32_t ch, uint32_t frame)   /* the note's attenuation at a frame of its shape */
{
    uint32_t a = 15u - (psc[ch].vel >> 3u);
    if (psc[ch].inst) {
        const psgi_t *i = &psg_set[psc[ch].inst - 1u];
        a += i->env[frame < i->len ? frame : i->len - 1u];
    }
    return a < 15u ? a : 15u;
}

static void psg_note_on(uint32_t ch, uint32_t note, uint32_t vel)   /* ch 0..2 tones, 3 noise */
{
    const psgi_t *i = psc[ch].inst ? &psg_set[psc[ch].inst - 1u] : 0;
    psc[ch].note = (uint8_t)note;
    psc[ch].vel = (uint8_t)vel;
    psc[ch].on = 1;
    psc[ch].t_on = fm1_ms;
    if (ch < 3u) psg_pitch(ch);
    else {
        uint32_t mode = i && (i->noise & 0x80u) ? i->noise & 7u : NOISE_MODE[note % 12u];
        if (i && (mode & 3u) == 3u) {           /* noise at tone 3's rate: tone 3 silent, at the note's pitch, as games do */
            psc[2].on = 0;
            psc[2].note = (uint8_t)note;
            psc[2].bend = 0;
            psg_vol(2, 15);
            psg_pitch(2);
        }
        q(DEV_PSG, 0xE0 | mode);
    }
    psc[ch].att = (uint8_t)psg_att(ch, 0);
    psg_vol(ch, psc[ch].att);
}

static int is_noise_inst(uint32_t inst) { return inst && (psg_set[inst - 1u].noise & 0x80u); }
static void noise_default(void)                 /* the noise part: the set's first noise instrument, unless it has one */
{
    uint32_t k;
    if (is_noise_inst(psc[3].inst) && psc[3].inst <= psg_set_n) return;
    psc[3].inst = 0;
    for (k = 1; k <= psg_set_n; k++)
        if (is_noise_inst(k)) { psc[3].inst = (uint8_t)k; return; }
}

static void psg_shapes(void)                    /* the notes' volume shapes, a frame (1/60 s) at a time */
{
    uint32_t ch;
    for (ch = 0; ch < 4u; ch++)
        if (psc[ch].on && psc[ch].inst) {
            uint32_t a = psg_att(ch, (fm1_ms - psc[ch].t_on) * 60u / 1000u);
            if (a != psc[ch].att) { psc[ch].att = (uint8_t)a; psg_vol(ch, a); }
        }
}
static void psg_note_off(uint32_t ch, uint32_t note)
{
    if (psc[ch].on && psc[ch].note == note) { psg_vol(ch, 15); psc[ch].on = 0; }
}

/* the instruments: the sound set from the page (kept in flash, the last one sent); none until one is sent */
#define SET_MAX 32u
static patch_t set_bank[SET_MAX];
static uint8_t set_n;
static uint32_t n_instruments(void) { return set_n; }

static uint32_t inst_changed;                   /* bit ch: channel ch has a new instrument (the panel follows its octave) */

static void new_instrument(uint32_t ch)         /* fmc[ch].p replaced: to the chip, with its song's LFO if it needs it */
{
    if (fmc[ch].p.lfo & 0x80u) {
        lfo_reg = fmc[ch].p.lfo & 0x0Fu;
        fm(0, 0x22, lfo_reg);
    }
    fm_patch(ch);
    inst_changed |= 1u << ch;
}

static void load_preset(uint32_t ch, uint32_t i)
{
    if (!set_n) return;                         /* (no sound set yet: nothing to choose from) */
    i %= set_n;
    fmc[ch].preset = (uint8_t)i;
    fmc[ch].p = set_bank[i];
    new_instrument(ch);
}

/* a GenMDM CC on an FM channel; the value scaled to the parameter's range by its top bits */
static void fm_cc(uint32_t ch, uint32_t cc, uint32_t v)
{
    patch_t *p = &fmc[ch].p;
    if (cc >= 16u && cc <= 62u) {
        static const uint8_t BASE[] = {16, 20, 24, 39, 43, 47, 51, 55, 59};
        uint32_t k, b;
        for (k = 0; k < sizeof BASE; k++) {
            b = BASE[k];
            if (cc >= b && cc < b + 4u) break;
        }
        if (k == sizeof BASE) return;
        {
            op_t *o = &p->op[cc - b];
            switch (k) {
            case 0: o->tl = (uint8_t)(127u - v); fm_level(ch); return;
            case 1: o->mul = (uint8_t)(v >> 3); break;
            case 2: o->dt = (uint8_t)(v >> 4); break;
            case 3: o->ks = (uint8_t)(v >> 5); break;
            case 4: o->ar = (uint8_t)(v >> 2); break;
            case 5: o->d1r = (uint8_t)(v >> 2); break;
            case 6: o->d2r = (uint8_t)(v >> 3); break;
            case 7: o->sl = (uint8_t)(v >> 3); break;
            default: o->rr = (uint8_t)(v >> 3); break;
            }
        }
        fm_patch(ch);
        return;
    }
    switch (cc) {
    case 1: lfo_reg = (uint8_t)((lfo_reg & 8u) | v >> 4); fm(0, 0x22, lfo_reg); return;
    case 74: lfo_reg = (uint8_t)((lfo_reg & 7u) | (v >> 6) << 3); fm(0, 0x22, lfo_reg); return;
    case 9: load_preset(ch, v >> 3); return;
    case 14: p->alg = (uint8_t)(v >> 4); break;
    case 15: p->fb = (uint8_t)(v >> 4); break;
    case 70: case 71: case 72: case 73: p->op[cc - 70u].am = (uint8_t)(v >> 6); break;
    case 75: p->pms = (uint8_t)(v >> 4); break;
    case 76: p->ams = (uint8_t)(v >> 5); break;
    case 77: fmc[ch].pan = (uint8_t)(v >> 5); break;       /* (as the register's bits: 1 R, 2 L, 3 both) */
    case 81: fmc[ch].bend_range = (uint8_t)(v * 18u >> 7); return;
    default: return;
    }
    fm_patch(ch);
}

/* the screen: console black, red stripes, chrome lettering (the look of the era; no one's logo) */
#define C_ACC RGB(232, 36, 44)                  /* red */
#define C_RED_DK RGB(120, 14, 20)
#define C_OFF RGB(140, 144, 156)                /* steel grey */
#define C_SOFT RGB(214, 216, 224)
#define C_MARK RGB(255, 0, 255)                 /* (only drawn to be recoloured) */

/* ---- the song: upload (MIDI channels 15, 16) and transport */
enum { UP_NONE, UP_BANK, UP_STREAM, UP_KIT };
#define CHUNK 256u                              /* the most bytes a chunk carries */
static struct {
    uint32_t len, want_len, want_crc, sent_kb, sent_starve;
    uint8_t on, err, loaded;
    uint8_t c_on, c_bad;                        /* the chunk arriving: */
    uint32_t c_off, c_len, c_crc;               /* where it goes (bytes from the start, mod 2^21), size, CRC */
    uint8_t c_buf[CHUNK];
} upl;
/* (crc32: platform.c's) */

static uint32_t crc16(uint32_t c, const uint8_t *p, uint32_t n)    /* CRC-16/CCITT-FALSE, from c (0xFFFF to begin) */
{
    uint32_t k;
    while (n--) {
        c ^= (uint32_t)*p++ << 8;
        for (k = 0; k < 8u; k++) c = (c & 0x8000u ? c << 1 ^ 0x1021u : c << 1) & 0xFFFFu;
    }
    return c;
}

static int chunk_commit(void)                   /* the chunk into the bank, the kit or the ring; 0: no room */
{
    uint32_t i, n = upl.c_len;
    if (upl.on == UP_KIT) {
        if (upl.len + n > KIT_MAX) { upl.err = 3; return 0; }
        for (i = 0; i < n; i++) kit_buf[upl.len + i] = upl.c_buf[i];
        upl.len += n;
        return 1;
    }
    if (upl.on == UP_BANK) {
        if (upl.len + n > BANK_MAX) { upl.err = 3; return 0; }
        for (i = 0; i < n; i++) bank_buf[upl.len + i] = upl.c_buf[i];
        upl.len += n;
        return 1;
    }
    if (upl.on == UP_STREAM) {
        uint32_t f = ring_fill;
        if (f + n - vgm.pos > RING_SIZE) return 0;   /* (the page counts the room too: rare) */
        for (i = 0; i < n; i++) ring_buf[(f + i) & RING_MASK] = upl.c_buf[i];
        ring_fill = f + n;                      /* (after the bytes: the interrupt reads up to it) */
        return 1;
    }
    return 0;
}

/* the kit's image: [u32 n] then n x [u32 length, u32 rate in Hz], then the sounds' bytes one after another
 * (8-bit unsigned, as the DAC takes them). 0: kept; 12: the table does not fit the bytes */
static uint32_t kit_open(uint32_t len)
{
    uint32_t n = kit_buf[0] | (uint32_t)kit_buf[1] << 8, k, at;
    if (n > KIT_SOUNDS || 4u + 8u * n > len) return 12;
    at = 4u + 8u * n;
    for (k = 0; k < n; k++) {
        const uint8_t *e = kit_buf + 4u + 8u * k;
        kit_len[k] = e[0] | (uint32_t)e[1] << 8 | (uint32_t)e[2] << 16;
        kit_rate[k] = e[4] | (uint32_t)e[5] << 8 | (uint32_t)e[6] << 16;
        kit_off[k] = at;
        at += kit_len[k];
        if (at > len || kit_rate[k] < 1000u || kit_rate[k] > 50000u) return 12;
    }
    kit_n = (uint8_t)n;
    kit_bytes = len;
    return 0;
}

static void drum(uint32_t k, uint32_t vel)      /* kit sound k, now */
{
    if (k >= kit_n) return;
    q(DEV_DRUM_VEL, vel);
    q(DEV_DRUM, k);
}

static void song_request(uint32_t r)            /* and wait until the interrupt has done it */
{
    song_req = (uint8_t)r;
    while (song_req != SONG_NONE) platform_poll();
}

/* ---- the sound set in flash: the last one sent, read back at boot. 64 KB of the data area (0x97000, platform.c
 * lets the driver erase it); [header][set_bank][psg_set][kit image], the header written last, so a save cut short
 * reads as none. The header's ver changes with the layout: an old store is then ignored, not misread. */
#ifndef STORE_AT                                /* (platform.c has them; the host build does not) */
#define STORE_AT 0x00097000u
#define STORE_LEN (64u * 1024u)
#endif
#define STORE_MAGIC 0x314E504Fu                 /* "OPN1" */
#define STORE_VER ((uint32_t)sizeof(patch_t) | (uint32_t)sizeof(psgi_t) << 8 | 1u << 24)
typedef struct { uint32_t magic, ver, len, crc; uint8_t set_n, psg_n, kit_n, pad; uint32_t kit_len; } store_head_t;

#ifdef DINGHY_HOST                              /* host: the flash as a RAM array (one run) */
static uint8_t host_flash[STORE_LEN];
static int store_erase(uint32_t off) { uint32_t i; for (i = 0; i < 0x1000u; i++) host_flash[off - STORE_AT + i] = 0xFF; return 0; }
static int store_write(uint32_t off, const void *p, uint32_t n) { const uint8_t *b = p; while (n--) host_flash[off++ - STORE_AT] &= *b++; return 0; }
static int store_read(uint32_t off, void *p, uint32_t n) { uint8_t *b = p; while (n--) *b++ = host_flash[off++ - STORE_AT]; return 0; }
#else
static int store_erase(uint32_t off) { uint32_t took; return fl_erase4k(off, &took); }
static int store_write(uint32_t off, const void *p, uint32_t n) { return fl_write(off, p, n); }
static int store_read(uint32_t off, void *p, uint32_t n) { return st_read(off, p, n); }
#endif

static uint32_t crc32_more(uint32_t c, const void *p, uint32_t n)   /* CRC-32 over pieces: c from 0xFFFFFFFF, ~ at the end */
{
    const uint8_t *b = p;
    uint32_t k;
    while (n--) {
        c ^= *b++;
        for (k = 0; k < 8u; k++) c = c & 1u ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return c;
}

static uint32_t store_save(void)                /* 0 kept; 3 too big; 4 erase failed; 5 write failed */
{
    const uint32_t a = SET_MAX * sizeof(patch_t), b = PSG_SET_MAX * sizeof(psgi_t);
    store_head_t h;
    uint32_t off, c, at = STORE_AT + sizeof h;
    h.set_n = set_n;
    h.psg_n = psg_set_n;
    h.kit_n = kit_n;
    h.pad = 0;
    h.kit_len = kit_n ? kit_bytes : 0;
    h.len = a + b + h.kit_len;
    if (sizeof h + h.len > STORE_LEN) return 3;
    c = crc32_more(0xFFFFFFFFu, &h.set_n, 8);
    c = crc32_more(c, set_bank, a);
    c = crc32_more(c, psg_set, b);
    c = crc32_more(c, kit_buf, h.kit_len);
    h.magic = STORE_MAGIC;
    h.ver = STORE_VER;
    h.crc = ~c;
    for (off = 0; off < sizeof h + h.len; off += 0x1000u) {
        platform_poll();                        /* (USB and the watchdog between the erases) */
        if (store_erase(STORE_AT + off)) return 4;
    }
    if (store_write(at, set_bank, a) || store_write(at + a, psg_set, b) ||
        (h.kit_len && store_write(at + a + b, kit_buf, h.kit_len)))
        return 5;
    return store_write(STORE_AT, &h, sizeof h) ? 5u : 0u;   /* the header last */
}

static void store_load(void)                    /* at boot: the kept set, if it is whole */
{
    const uint32_t a = SET_MAX * sizeof(patch_t), b = PSG_SET_MAX * sizeof(psgi_t);
    store_head_t h;
    uint32_t c, at = STORE_AT + sizeof h;
    if (store_read(STORE_AT, &h, sizeof h) || h.magic != STORE_MAGIC || h.ver != STORE_VER ||
        h.set_n > SET_MAX || h.psg_n > PSG_SET_MAX || h.kit_len > KIT_MAX || h.len != a + b + h.kit_len)
        return;
    if (store_read(at, set_bank, a) || store_read(at + a, psg_set, b) || store_read(at + a + b, kit_buf, h.kit_len))
        return;
    c = crc32_more(0xFFFFFFFFu, &h.set_n, 8);
    c = crc32_more(c, set_bank, a);
    c = crc32_more(c, psg_set, b);
    c = crc32_more(c, kit_buf, h.kit_len);
    if (~c != h.crc) return;                    /* (damaged: start empty) */
    set_n = h.set_n;
    psg_set_n = h.psg_n;
    if (h.kit_len && kit_open(h.kit_len)) kit_n = 0;
}

/* ---- a whole patch, exactly (from web/index.html, e.g. one found in a song). Channel 16 CCs: 50 n begins it
 * (n: FM channel 0-5, 6 for all six, or 16 + k for the sound set's instrument k), CC 59 c its name a character at a
 * time (up to 11, optional), then 30 register values, each as CC 52 (low 7 bits) and CC 53 (the top bit): B0, B4,
 * then for O1..O4 (registers +0, +8, +4, +12) 30 40 50 60 70 80 90, optionally 2 or 5 more (patch_t's lfo, oct,
 * vib_depth, vib_rate, vib_delay: what its song had; 0 for none); then CC 54 55 the 14-bit sum of the name's
 * characters and the values. Kept only whole, with the sum right; the FM-1 answers CC 56 n (taken) or 57 (not).
 * CC 5A n: the sound set has n instruments (0: none); they are programs 0..n-1, the built-in presets after them. */
static struct { uint8_t on, ch, n, lo, nlen; uint8_t v[35]; char name[12]; uint32_t sum, want; } pf;
static uint8_t panel_stale;                     /* the panel shows a patch name that changed */

static void regs_to_patch(patch_t *p, const uint8_t *v, const char *name)
{
    uint32_t k;
    for (k = 0; k < 11u && name[k]; k++) p->name[k] = name[k];
    p->name[k] = 0;
    p->alg = v[0] & 7u;
    p->fb = (v[0] >> 3) & 7u;
    p->ams = (v[1] >> 4) & 3u;
    p->pms = v[1] & 7u;
    for (k = 0; k < 4u; k++) {
        const uint8_t *r = v + 2u + 7u * k;
        op_t *o = &p->op[k];
        o->dt = (r[0] >> 4) & 7u; o->mul = r[0] & 15u; o->tl = r[1] & 127u;
        o->ks = r[2] >> 6; o->ar = r[2] & 31u; o->am = r[3] >> 7; o->d1r = r[3] & 31u;
        o->d2r = r[4] & 31u; o->sl = r[5] >> 4; o->rr = r[5] & 15u; o->ssg = r[6] & 15u;
    }
    p->lfo = v[30];                             /* (32 values: the song's LFO and octave; 30: neither) */
    p->oct = v[31];
    p->vib_depth = v[32];                       /* (35 values: its vibrato too) */
    p->vib_rate = v[33];
    p->vib_delay = v[34];
}

/* a PSG instrument, as a patch: channel 16 CC 40 k begins it (slot k), CC 41 c its name (up to 11), then values as CC
 * 42 (low 7 bits) 43 (top bit): its length n (1-64), its noise (0x80 | type, or 0), its n attenuations; CC 44 45 the
 * 14-bit sum of the name and the values. Answers CC 46 k (taken) or 47 (not). CC 48 n: the PSG set has n. */
static struct { uint8_t on, slot, n, lo, nlen; uint8_t v[2 + PSG_ENV_MAX]; char name[12]; uint32_t sum, want; } pp;

static int psgi_rx(uint32_t d1, uint32_t d2)    /* 1: the CC was the PSG instrument message's */
{
    switch (d1) {
    case 0x40: pp.on = 1; pp.slot = (uint8_t)d2; pp.n = pp.nlen = 0; pp.sum = pp.want = 0; return 1;
    case 0x41: if (pp.on && pp.nlen < 11u) { pp.name[pp.nlen++] = (char)d2; pp.sum += d2; } return 1;
    case 0x42: pp.lo = (uint8_t)d2; return 1;
    case 0x43:
        if (pp.on && pp.n < sizeof pp.v) { pp.v[pp.n] = (uint8_t)(pp.lo | (d2 & 1u) << 7); pp.sum += pp.v[pp.n++]; }
        return 1;
    case 0x44: pp.want = d2; return 1;
    case 0x45: {
        uint32_t k;
        pp.want |= d2 << 7;
        if (pp.on && pp.slot < PSG_SET_MAX && pp.n >= 3u && pp.v[0] >= 1u && pp.v[0] <= PSG_ENV_MAX &&
            pp.n == 2u + pp.v[0] && (pp.sum & 0x3FFFu) == pp.want) {
            psgi_t *i = &psg_set[pp.slot];
            for (k = 0; k < pp.nlen; k++) i->name[k] = pp.name[k];
            i->name[k] = 0;
            i->len = pp.v[0];
            i->noise = pp.v[1];
            for (k = 0; k < i->len; k++) i->env[k] = pp.v[2u + k] & 15u;
            panel_stale = 1;
            midi_send(0xBF, 0x46, pp.slot);
        } else
            midi_send(0xBF, 0x47, pp.slot & 0x7Fu);
        pp.on = 0;
    } return 1;
    case 0x48:
        psg_set_n = (uint8_t)(d2 < PSG_SET_MAX ? d2 : PSG_SET_MAX);
        noise_default();
        for (d1 = 0; d1 < 4u; d1++)              /* (instruments past the new size: plain) */
            if (psc[d1].inst > psg_set_n) psc[d1].inst = 0;
        panel_stale = 1;
        return 1;
    }
    return 0;
}

static int patch_rx(uint32_t d1, uint32_t d2)   /* 1: the CC was the patch message's */
{
    switch (d1) {
    case 0x50:
        pf.on = 1;
        pf.ch = (uint8_t)d2;
        pf.n = pf.nlen = 0;
        pf.sum = pf.want = 0;
        pf.v[30] = pf.v[31] = pf.v[32] = pf.v[33] = pf.v[34] = 0;
        return 1;
    case 0x59:
        if (pf.on && pf.nlen < 11u) { pf.name[pf.nlen++] = (char)d2; pf.sum += d2; }
        return 1;
    case 0x5A:
        set_n = (uint8_t)(d2 < SET_MAX ? d2 : SET_MAX);
        panel_stale = 1;
        return 1;
    case 0x52: pf.lo = (uint8_t)d2; return 1;
    case 0x53:
        if (pf.on && pf.n < 35u) { pf.v[pf.n] = (uint8_t)(pf.lo | (d2 & 1u) << 7); pf.sum += pf.v[pf.n++]; }
        return 1;
    case 0x54: pf.want = d2; return 1;
    case 0x55: {
        uint32_t k;
        pf.want |= d2 << 7;
        pf.name[pf.nlen] = 0;
        if (pf.on && (pf.n == 30u || pf.n == 32u || pf.n == 35u) && (pf.sum & 0x3FFFu) == pf.want && (pf.ch <= 6u || (pf.ch >= 16u && pf.ch < 16u + SET_MAX))) {
            const char *name = pf.nlen ? pf.name : "FROM SONG";
            if (pf.ch >= 16u) regs_to_patch(&set_bank[pf.ch - 16u], pf.v, name);
            else
                for (k = 0; k < 6u; k++)
                    if (pf.ch == 6u || pf.ch == k) {
                        regs_to_patch(&fmc[k].p, pf.v, name);
                        fmc[k].pan = (uint8_t)(pf.v[1] >> 6);
                        new_instrument(k);
                    }
            panel_stale = 1;
            midi_send(0xBF, 0x56, pf.ch);
        } else
            midi_send(0xBF, 0x57, pf.ch & 0x7Fu);
        pf.on = 0;
    } return 1;
    }
    return 0;
}

static int song_rx(uint32_t st, uint32_t d1, uint32_t d2)    /* 1: the message was the song's */
{
    uint32_t ch = st & 15u, type = st & 0xF0u, r;
    if ((ch == 14u || ch == 15u) && (type == 0xA0u || type == 0xE0u)) {
        uint32_t w = d1 | d2 << 7 | ((type == 0xE0u ? 1u : 0u) + (ch == 15u ? 2u : 0u)) << 14;
        if (!upl.c_on) return 1;                /* (outside a chunk: a stray, dropped) */
        if (upl.c_len + 2u > CHUNK) upl.c_bad = 1;
        else {
            upl.c_buf[upl.c_len] = (uint8_t)w;
            upl.c_buf[upl.c_len + 1u] = (uint8_t)(w >> 8);
            upl.c_len += 2u;
        }
        return 1;
    }
    if (ch != 15u || type != 0xB0u) return 0;
    if (patch_rx(d1, d2) || psgi_rx(d1, d2)) return 1;
    if (d1 >= 0x78u && d1 <= 0x7Cu) upl.want_crc |= d2 << (7u * (d1 - 0x78u));
    else if (d1 >= 0x7Du) upl.want_len |= d2 << (7u * (d1 - 0x7Du));
    else if (d1 >= 0x6Bu && d1 <= 0x6Fu && !upl.c_on) return 1;    /* (a chunk's parts with no begin: its was lost) */
    else switch (d1) {
    case 0x6A:                                  /* a chunk begins: where it goes, 7 + 7 + 7 bits */
        upl.c_on = 1;
        upl.c_bad = 0;
        upl.c_len = upl.c_crc = 0;
        upl.c_off = d2;
        break;
    case 0x6B: upl.c_off |= d2 << 7; break;
    case 0x6C: upl.c_off |= d2 << 14; break;
    case 0x6D: upl.c_crc |= d2; break;          /* its CRC-16, 7 + 7 + 2 bits; the last ends it */
    case 0x6E: upl.c_crc |= d2 << 7; break;
    case 0x6F: {
        uint32_t held;
        int ok;
        upl.c_crc |= (d2 & 3u) << 14;
        upl.c_on = 0;
        held = upl.on == UP_STREAM ? ring_fill : upl.len;
        {                                       /* the CRC covers where the chunk goes too: a lost part of it fails */
            uint8_t off[3] = {(uint8_t)upl.c_off, (uint8_t)(upl.c_off >> 8), (uint8_t)(upl.c_off >> 16)};
            ok = crc16(crc16(0xFFFFu, off, 3), upl.c_buf, upl.c_len) == upl.c_crc;
        }
        if (ok && !upl.c_bad && ((upl.c_off - held) & 0x1FFFFFu) == 0)
            chunk_commit();                     /* only where the data ends, whole: anything else is dropped */
        held = upl.on == UP_STREAM ? ring_fill : upl.len;
        midi_send(0xBF, 0x65, held & 0x7Fu);    /* what is held now: the page sends on from there */
        midi_send(0xBF, 0x66, (held >> 7) & 0x7Fu);
        midi_send(0xBF, 0x67, (held >> 14) & 0x7Fu);
    } break;
    case 0x70:                                  /* the PCM bank begins: the song stops, the buffers are ours */
        song_request(SONG_STOP);
        upl.on = UP_BANK;
        upl.len = upl.want_len = upl.want_crc = 0;
        upl.err = upl.loaded = upl.c_on = 0;
        midi_send(0xBF, 0x6E, 0x70);            /* (heard: the page sends it again until this comes) */
        break;
    case 0x72:                                  /* the bank is complete: check it */
        upl.on = UP_NONE;
        if (upl.err || upl.want_len > BANK_MAX) r = 3;
        else if (upl.len != ((upl.want_len + 1u) & ~1u)) r = 1;
        else if (crc32(bank_buf, upl.want_len) != upl.want_crc) r = 2;
        else {
            int e = vgm_open_stream(&vgm, bank_buf, upl.want_len, ring_buf, RING_MASK, &ring_fill);
            r = e ? 10u + (uint32_t)e : 0u;
            upl.loaded = !e;
        }
        midi_send(0xBF, 0x73, r);
        break;
    case 0x4E:                                  /* keep the sound set in flash (the page, after sending it) */
        midi_send(0xBF, 0x6E, 0x4E);
        song_request(SONG_STOP);
        draw_text_box(0, 168, 240, &FONT_S, "SAVING THE SOUNDS", C_ACC, 1);
        r = store_save();                       /* (the flash erase stops the sound for under a second) */
        midi_send(0xBF, 0x4D, r);
        panel_stale = 1;
        break;
#ifdef DINGHY_HOST
    case 0x4C: {                                /* host tests only: as a reboot, the set from the store alone */
        uint8_t *z = (uint8_t *)set_bank;
        uint32_t i;
        for (i = 0; i < sizeof set_bank; i++) z[i] = 0;
        set_n = psg_set_n = kit_n = 0;
        store_load();
        midi_send(0xBF, 0x4C, (uint32_t)set_n << 2 | (psg_set_n ? 2u : 0u) | (kit_n ? 1u : 0u));
    } break;
#endif
    case 0x61:                                  /* the drum kit begins: no kit meanwhile */
        kit_n = 0;
        q(DEV_DRUM, 0xFF);                      /* (a drum still sounding stops reading it) */
        upl.on = UP_KIT;
        upl.len = upl.want_len = upl.want_crc = 0;
        upl.err = upl.c_on = 0;
        panel_stale = 1;
        midi_send(0xBF, 0x6E, 0x61);
        break;
    case 0x63:                                  /* the kit is complete: check it, read its table */
        upl.on = UP_NONE;
        if (upl.err || upl.want_len > KIT_MAX) r = 3;
        else if (upl.len != ((upl.want_len + 1u) & ~1u)) r = 1;
        else if (crc32(kit_buf, upl.want_len) != upl.want_crc) r = 2;
        else r = kit_open(upl.want_len);
        midi_send(0xBF, 0x73, r);
        panel_stale = 1;
        break;
    case 0x60:                                  /* the commands begin: an empty ring */
        song_request(SONG_STOP);
        vgm.pos = 0;
        ring_fill = 0;
        upl.on = upl.loaded ? UP_STREAM : UP_NONE;
        upl.c_on = 0;
        upl.sent_kb = 0;
        upl.sent_starve = vgm.starve_count;
        midi_send(0xBF, 0x6E, 0x60);
        break;
    case 0x75:                                  /* (the page may send these more than once) */
        if (d2 && upl.loaded && upl.on == UP_STREAM && !vgm.playing) song_request(SONG_START);
        else if (!d2 && vgm.playing) song_request(SONG_STOP);
        midi_send(0xBF, 0x74, vgm.playing);
        break;
    }
    return 1;
}

static void song_report(void)                   /* to the page: what the player has taken, and any dry spell */
{
    uint32_t kb = vgm.pos >> 10;
    if (upl.on != UP_STREAM) return;
    if ((kb & 0x7Fu) != upl.sent_kb) {
        upl.sent_kb = kb & 0x7Fu;
        midi_send(0xBF, 0x77, upl.sent_kb);
    }
    if (vgm.starve_count != upl.sent_starve) {
        upl.sent_starve = vgm.starve_count;
        midi_send(0xBF, 0x7B, upl.sent_starve & 0x7Fu);
    }
}

static void restore_panel(void)                 /* after a song (it reset the chips): the patches back */
{
    uint32_t k;
    fm(0, 0x22, lfo_reg);
    for (k = 0; k < 6u; k++) { fmc[k].on = 0; fm_patch(k); }
    for (k = 0; k < 4u; k++) { psc[k].on = 0; psg_vol(k, 15); }
}

static int song_text(char *s)                   /* 1: a song is loading or playing (its status in s) */
{
    char *p = s;
    const char *t;
    if (upl.on == UP_BANK) {
        uint32_t kb = upl.len >> 10, d[6], n = 0;
        t = "LOADING DRUMS ";
        while (*t) *p++ = *t++;
        do { d[n++] = kb % 10u; kb /= 10u; } while (kb);
        while (n) *p++ = (char)('0' + d[--n]);
        *p++ = ' '; *p++ = 'K'; *p++ = 'B';
    } else if (vgm.playing) {
        uint32_t sec = vgm.played / 44100u;
        t = vgm.starved ? "WAITING " : "PLAYING ";
        while (*t) *p++ = *t++;
        if (sec >= 600u) *p++ = (char)('0' + sec / 600u % 10u);
        *p++ = (char)('0' + sec / 60u % 10u);
        *p++ = ':';
        *p++ = (char)('0' + sec % 60u / 10u);
        *p++ = (char)('0' + sec % 10u);
    } else
        return 0;
    *p = 0;
    return 1;
}

/* ---- the drivers' software vibrato (an instrument's vib_*): after its delay, a triangle from 0 upwards, the
 * pitch rewritten as the driver did (it stepped at 60 Hz; this every 10 ms) */
static void vibrato(void)
{
    static uint32_t last;
    uint32_t ch;
    if (fm1_ms - last < 10u) return;
    last = fm1_ms;
    for (ch = 0; ch < 6u; ch++) {
        fmch_t *v = &fmc[ch];
        uint32_t t = fm1_ms - v->t_on, delay = v->p.vib_delay * 10u, period;
        int32_t x, tri, c = 0;
        if (!v->on || !v->p.vib_depth || !v->p.vib_rate) continue;
        if (t >= delay) {
            period = 10000u / v->p.vib_rate;    /* ms */
            x = (int32_t)((t - delay) % period * 4096u / period);
            tri = x < 1024 ? x : x < 3072 ? 2048 - x : x - 4096;
            c = (int32_t)v->p.vib_depth * tri / 1024;
        }
        if (c != v->vib) {
            v->vib = (int16_t)c;
            fm_pitch(ch);
        }
    }
}

/* ---- the panel */
enum { PANEL_FM = 0, PANEL_PSG = 6, PANEL_NOISE = 9, PANEL_DRUMS = 10, PANEL_POLY = 11, NPANEL = 12 };

static uint32_t part_fm(uint32_t sel, uint32_t *c1)   /* the part's first FM channel, *c1 past its last; 6: not FM */
{
    *c1 = sel < PANEL_PSG ? sel + 1u : 6u;
    return sel < PANEL_PSG ? sel : sel == PANEL_POLY ? 0u : 6u;
}

/* ---- playing the panel's part: its keys, the arpeggiator, MIDI in PANEL mode */
static uint8_t key_vel = 100, midi_panel, dbl;  /* dbl: POLY's doubling, cents between the pair (0: off) */
static uint8_t voice_of[128];                   /* by note: voice_on's code + 1 (0: not sounding) */

#define V_PAIR 0x40u                            /* (in a code: this FM channel and the next, doubled) */
static uint32_t voice_on(uint32_t sel, uint32_t note, uint32_t vel)
{
    if (sel < PANEL_PSG) { fm_note_on(sel, note, vel, 0); return sel; }
    if (sel < PANEL_DRUMS) { psg_note_on(sel - PANEL_PSG, note, vel); return sel; }
    if (sel == PANEL_DRUMS) { drum(note - 53u, vel); return sel; }   /* key 0 (F3, octave 0) = the kit's first */
    {                                           /* POLY: a free FM channel or pair, else the one held longest */
        static uint32_t next;                   /* (with a kit, channel 6 is the drums') */
        uint32_t w = dbl ? 2u : 1u, slots = (kit_n ? 5u : 6u) / w, c, pick = slots;
        for (c = 0; c < slots; c++) {
            uint32_t k = (next + c) % slots;
            if (!fmc[k * w].on && !fmc[k * w + w - 1u].on) { pick = k; break; }
        }
        if (pick == slots) pick = next % slots;
        next = pick + 1u;
        if (!dbl) { fm_note_on(pick, note, vel, 0); return pick; }
        fm_note_on(pick * 2u, note, vel, -(int32_t)(dbl / 2u));
        fm_note_on(pick * 2u + 1u, note, vel, (int32_t)(dbl - dbl / 2u));
        return pick * 2u | V_PAIR;
    }
}
static void voice_off(uint32_t code, uint32_t note)
{
    uint32_t c = code & ~V_PAIR;
    if (c < PANEL_PSG) {
        fm_note_off(c, note);
        if (code & V_PAIR) fm_note_off(c + 1u, note);
    } else if (c < PANEL_DRUMS) psg_note_off(c - PANEL_PSG, note);   /* (DRUMS: one-shot) */
}

enum { ARP_UP, ARP_DOWN, ARP_UPDOWN, ARP_RANDOM };
static struct {
    uint8_t on, mode, octs, gate, n, sounding, note, code;
    uint16_t bpm;
    uint8_t notes[16];                          /* held, lowest first */
    uint32_t step, t_next, t_off, rnd;
} arp;

static void arp_stop(void)
{
    if (arp.sounding) { voice_off(arp.code, arp.note); arp.sounding = 0; }
}
static void arp_add(uint32_t note)
{
    uint32_t i, j;
    for (i = 0; i < arp.n && arp.notes[i] < note; i++) ;
    if ((i < arp.n && arp.notes[i] == note) || arp.n == sizeof arp.notes) return;
    if (!arp.n) { arp.step = 0; arp.t_next = fm1_ms; }
    for (j = arp.n++; j > i; j--) arp.notes[j] = arp.notes[j - 1u];
    arp.notes[i] = (uint8_t)note;
}
static void arp_remove(uint32_t note)
{
    uint32_t i;
    for (i = 0; i < arp.n && arp.notes[i] != note; i++) ;
    if (i == arp.n) return;
    for (arp.n--; i < arp.n; i++) arp.notes[i] = arp.notes[i + 1u];
}
static void arp_run(uint32_t sel)               /* sixteenths at its tempo, through the held notes and octaves */
{
    uint32_t len, i, note, period;
    if (arp.sounding && (int32_t)(fm1_ms - arp.t_off) >= 0) arp_stop();
    if (!arp.on || !arp.n) { arp_stop(); return; }
    if ((int32_t)(fm1_ms - arp.t_next) < 0) return;
    period = 15000u / arp.bpm;
    len = arp.n * arp.octs;
    if (arp.mode == ARP_RANDOM) {
        arp.rnd = arp.rnd * 1103515245u + 12345u;
        i = (arp.rnd >> 16) % len;
    } else {
        i = arp.step++ % (arp.mode == ARP_UPDOWN && len > 1u ? 2u * len - 2u : len);
        if (i >= len) i = 2u * len - 2u - i;
        if (arp.mode == ARP_DOWN) i = len - 1u - i;
    }
    note = arp.notes[i % arp.n] + 12u * (i / arp.n);
    arp_stop();
    if (note < 128u) {
        arp.code = (uint8_t)voice_on(sel, note, key_vel);
        arp.note = (uint8_t)note;
        arp.sounding = 1;
    }
    arp.t_off = fm1_ms + period * arp.gate / 100u;
    arp.t_next += period;
    if ((int32_t)(fm1_ms - arp.t_next) > 0) arp.t_next = fm1_ms + period;   /* (fell behind: no burst to catch up) */
}

static void key_down(uint32_t sel, uint32_t note, uint32_t vel)
{
    if (note > 127u) return;
    if (arp.on) { arp_add(note); return; }
    if (voice_of[note]) voice_off(voice_of[note] - 1u, note);
    voice_of[note] = (uint8_t)(voice_on(sel, note, vel) + 1u);
}
static void key_up(uint32_t note)
{
    if (note > 127u) return;
    arp_remove(note);
    if (voice_of[note]) { voice_off(voice_of[note] - 1u, note); voice_of[note] = 0; }
}
static void part_bend(uint32_t sel, int32_t bend)
{
    uint32_t c1, c = part_fm(sel, &c1);
    for (; c < c1; c++) { fmc[c].bend = (int16_t)bend; fm_pitch(c); }
    if (sel >= PANEL_PSG && sel < PANEL_NOISE) { psc[sel - PANEL_PSG].bend = (int16_t)bend; psg_pitch(sel - PANEL_PSG); }
}

/* ---- the knobs: a page of four, chosen by the buttons; ALGORITHM is always the algorithm */
enum { PG_SOUND, PG_TONE, PG_ENV, PG_LFO, PG_FX, PG_ARP, PG_GLO, NPAGE };
static const char *const PAGE_NAME[NPAGE] = {"SOUND", "TONE", "ENVELOPE", "LFO", "VIBRATO + DOUBLE", "ARPEGGIO", "GLOBAL"};
static const char *const KNOB_NAME[NPAGE][4] = {
    {"BRIGHT", "ATTACK", "REL", "FEEDBK"}, {"LEVEL", "MULT", "DETUNE", "DECAY2"},
    {"ATTACK", "DECAY", "SUSTN", "REL"}, {"RATE", "FM DEP", "AM DEP", "AM OPS"},
    {"DEPTH", "SPEED", "DELAY", "DOUBLE"}, {"MODE", "BPM", "OCTS", "GATE"}, {"VELO", "BEND", "MIDI", "TUNE"}};   /* (6 fit) */
static const int16_t RANGE[NPAGE][4][3] = {     /* min, max, a detent's step */
    {{0, 127, 2}, {0, 31, 1}, {0, 15, 1}, {0, 7, 1}}, {{0, 127, 1}, {0, 15, 1}, {-3, 3, 1}, {0, 31, 1}},
    {{0, 31, 1}, {0, 31, 1}, {0, 15, 1}, {0, 15, 1}}, {{0, 8, 1}, {0, 7, 1}, {0, 3, 1}, {0, 15, 1}},
    {{0, 100, 2}, {0, 100, 2}, {0, 100, 2}, {0, 50, 1}}, {{0, 3, 1}, {40, 300, 4}, {1, 4, 1}, {10, 100, 10}},
    {{1, 127, 4}, {0, 24, 1}, {0, 1, 1}, {-50, 50, 1}}};
#define IS_PATCH(pg, k) ((pg) <= PG_FX && !((pg) == PG_LFO && (k) == 0u) && !((pg) == PG_FX && (k) == 3u))
static uint8_t page, op_sel;
#define NONE (-1000)                            /* (a knob with nothing to turn) */

static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static int32_t mod_tl(const patch_t *p)         /* the loudest modulator's TL; -1: all are carriers */
{
    int32_t k, m = -1;
    for (k = 0; k < 4; k++)
        if (!(CARRIERS[p->alg & 7u] >> k & 1u) && (m < 0 || p->op[k].tl < m)) m = p->op[k].tl;
    return m;
}
static int32_t car_max(const patch_t *p, uint32_t what)   /* the carriers' fastest attack (0) or release (1) */
{
    int32_t k, m = 0;
    for (k = 0; k < 4; k++)
        if (CARRIERS[p->alg & 7u] >> k & 1u) {
            int32_t v = what ? p->op[k].rr : p->op[k].ar;
            if (v > m) m = v;
        }
    return m;
}

/* knob k of page pg: its value now; with set, it moves to v first. p: the part's patch (0: not an FM part) */
static int32_t knob(uint32_t pg, uint32_t k, patch_t *p, int set, int32_t v)
{
    op_t *o = p ? &p->op[op_sel] : 0;
    int32_t d, j;
    if (IS_PATCH(pg, k) && !p) return NONE;
    switch (pg * 4u + k) {
    case PG_SOUND * 4u + 0u:                    /* the modulators, together: the timbre's brightness */
        if (mod_tl(p) < 0) return NONE;
        if (set) for (d = v - (127 - mod_tl(p)), j = 0; j < 4; j++)
            if (!(CARRIERS[p->alg & 7u] >> j & 1u)) p->op[j].tl = (uint8_t)clampi(p->op[j].tl - d, 0, 127);
        return 127 - mod_tl(p);
    case PG_SOUND * 4u + 1u:
        if (set) for (d = v - car_max(p, 0), j = 0; j < 4; j++) p->op[j].ar = (uint8_t)clampi(p->op[j].ar + d, 0, 31);
        return car_max(p, 0);
    case PG_SOUND * 4u + 2u:
        if (set) for (d = v - car_max(p, 1), j = 0; j < 4; j++) p->op[j].rr = (uint8_t)clampi(p->op[j].rr + d, 0, 15);
        return car_max(p, 1);
    case PG_SOUND * 4u + 3u: if (set) p->fb = (uint8_t)v; return p->fb;
    case PG_TONE * 4u + 0u: if (set) o->tl = (uint8_t)(127 - v); return 127 - o->tl;
    case PG_TONE * 4u + 1u: if (set) o->mul = (uint8_t)v; return o->mul;
    case PG_TONE * 4u + 2u: if (set) o->dt = (uint8_t)(v >= 0 ? v : 4 - v); return o->dt < 4u ? o->dt : 4 - o->dt;
    case PG_TONE * 4u + 3u: if (set) o->d2r = (uint8_t)v; return o->d2r;
    case PG_ENV * 4u + 0u: if (set) o->ar = (uint8_t)v; return o->ar;
    case PG_ENV * 4u + 1u: if (set) o->d1r = (uint8_t)v; return o->d1r;
    case PG_ENV * 4u + 2u: if (set) o->sl = (uint8_t)(15 - v); return 15 - o->sl;
    case PG_ENV * 4u + 3u: if (set) o->rr = (uint8_t)v; return o->rr;
    case PG_LFO * 4u + 0u:                      /* the chip's, shared by every channel */
        if (set) { lfo_reg = (uint8_t)(v ? 8 | (v - 1) : 0); fm(0, 0x22, lfo_reg); if (p) p->lfo = 0x80u | lfo_reg; }
        return lfo_reg & 8u ? (lfo_reg & 7u) + 1 : 0;
    case PG_LFO * 4u + 1u: if (set) p->pms = (uint8_t)v; return p->pms;
    case PG_LFO * 4u + 2u: if (set) p->ams = (uint8_t)v; return p->ams;
    case PG_LFO * 4u + 3u:
        if (set) for (j = 0; j < 4; j++) p->op[j].am = (uint8_t)(v >> j & 1);
        for (d = 0, j = 0; j < 4; j++) d |= p->op[j].am << j;
        return d;
    case PG_FX * 4u + 0u: if (set) p->vib_depth = (uint8_t)v; return p->vib_depth;
    case PG_FX * 4u + 1u: if (set) p->vib_rate = (uint8_t)v; return p->vib_rate;
    case PG_FX * 4u + 2u: if (set) p->vib_delay = (uint8_t)v; return p->vib_delay;
    case PG_FX * 4u + 3u: if (set) dbl = (uint8_t)v; return dbl;
    case PG_ARP * 4u + 0u: if (set) arp.mode = (uint8_t)v; return arp.mode;
    case PG_ARP * 4u + 1u: if (set) arp.bpm = (uint16_t)v; return arp.bpm;
    case PG_ARP * 4u + 2u: if (set) arp.octs = (uint8_t)v; return arp.octs;
    case PG_ARP * 4u + 3u: if (set) arp.gate = (uint8_t)v; return arp.gate;
    case PG_GLO * 4u + 0u: if (set) key_vel = (uint8_t)v; return key_vel;
    case PG_GLO * 4u + 1u:
        if (set) {
            for (j = 0; j < 6; j++) fmc[j].bend_range = (uint8_t)v;
            for (j = 0; j < 4; j++) psc[j].bend_range = (uint8_t)v;
        }
        return fmc[0].bend_range;
    case PG_GLO * 4u + 2u: if (set) midi_panel = (uint8_t)v; return midi_panel;
    default: if (set) tune = (int8_t)v; return tune;
    }
}

static char *put_num(char *s, int32_t v, int sign)
{
    char t[8];
    uint32_t n = 0, u = (uint32_t)(v < 0 ? -v : v);
    if (v < 0) *s++ = '-';
    else if (sign && v) *s++ = '+';
    do { t[n++] = (char)('0' + u % 10u); u /= 10u; } while (u);
    while (n) *s++ = t[--n];
    *s = 0;
    return s;
}
static void knob_text(uint32_t pg, uint32_t k, int32_t v, char *s)
{
    static const char *const LFO_HZ[8] = {"4.0", "5.6", "6.0", "6.4", "6.9", "9.6", "48", "72"};
    static const char *const MODE[4] = {"UP", "DOWN", "UP-DN", "RANDOM"};
    const char *t = 0;
    if (v == NONE) t = "-";
    else if (pg == PG_LFO && k == 0u) t = v ? LFO_HZ[v - 1] : "OFF";
    else if (pg == PG_LFO && k == 3u) {
        uint32_t j;
        for (j = 0; j < 4u; j++) s[j] = v >> j & 1 ? (char)('1' + j) : '.';
        s[4] = 0;
        return;
    } else if (pg == PG_FX && k != 1u && !v) t = "OFF";
    else if (pg == PG_FX && k == 1u) {          /* tenths of a Hz */
        s = put_num(s, v / 10, 0);
        *s++ = '.'; *s++ = (char)('0' + v % 10); *s = 0;
        return;
    } else if (pg == PG_FX && k == 2u) v *= 10; /* (10 ms steps: shown in ms) */
    else if (pg == PG_ARP && k == 0u) t = MODE[v & 3];
    else if (pg == PG_GLO && k == 2u) t = v ? "PANEL" : "GENMDM";
    if (t) {
        while ((*s++ = *t++)) {}
        return;
    }
    s = put_num(s, v, (pg == PG_TONE && k == 2u) || (pg == PG_GLO && k == 3u));
    if (pg == PG_ARP && k == 3u) { *s++ = '%'; *s = 0; }
}

static void part_edited(uint32_t sel)           /* the part's first channel's patch changed: to its others, the chip */
{
    uint32_t c1, c0 = part_fm(sel, &c1), c;
    for (c = c0; c < c1; c++) {
        if (c != c0) fmc[c].p = fmc[c0].p;
        fm_patch(c);
    }
}
static int edited(uint32_t c)                   /* channel c's patch differs from the set's instrument it came from */
{
    const uint8_t *a = (const uint8_t *)&fmc[c].p, *b = (const uint8_t *)&set_bank[fmc[c].preset];
    uint32_t i;
    if (!set_n) return 0;
    for (i = 0; i < sizeof(patch_t); i++)
        if (a[i] != b[i]) return 1;
    return 0;
}
static int knobs_turn(uint32_t sel)             /* 1: something moved */
{
    uint32_t c1, c0 = part_fm(sel, &c1), k;
    patch_t *p = c0 < 6u ? &fmc[c0].p : 0;
    int32_t turn = fm1_enc_take(ENC_ALGORITHM), moved = 0;
    if (turn && p) {
        p->alg = (uint8_t)clampi((int32_t)p->alg + turn, 0, 7);
        part_edited(sel);
        moved = 1;
    }
    for (k = 0; k < 4u; k++) {
        const int16_t *r = RANGE[page][k];
        int32_t v;
        if (!(turn = fm1_enc_take(ENC_KNOB1 + k))) continue;
        if ((v = knob(page, k, p, 0, 0)) == NONE) continue;
        knob(page, k, p, 1, clampi(v + turn * r[2], r[0], r[1]));
        if (IS_PATCH(page, k) || (page == PG_LFO && p)) part_edited(sel);
        moved = 1;
    }
    return moved;
}

static void panel_name(uint32_t sel, char *s)
{
    static const char *const N[NPANEL] = {"FM 1", "FM 2", "FM 3", "FM 4", "FM 5", "FM 6",
                                          "PSG 1", "PSG 2", "PSG 3", "NOISE", "DRUMS", "POLY"};
    const char *p = N[sel];
    while ((*s++ = *p++)) ;
}

static void draw_panel(uint32_t sel, int32_t oct)
{
    char s[24];
    panel_name(sel, s);
    draw_text_box(0, 92, 240, &FONT_L, s, C_WHITE, 1);
    if (sel < PANEL_PSG || sel == PANEL_POLY) {
        uint32_t c = sel == PANEL_POLY ? 0 : sel, i = 0;
        const char *p = set_n || fmc[c].p.name[0] ? fmc[c].p.name : "NO SOUNDS: USE THE PAGE";   /* (a grab needs no set) */
        while (*p) s[i++] = *p++;
        if (edited(c)) { s[i++] = ' '; s[i++] = '*'; }   /* (changed by the knobs: SAVE keeps it) */
        s[i] = 0;
    } else if (sel < PANEL_DRUMS) {           /* a PSG part: its instrument */
        uint32_t c = sel - PANEL_PSG, i = 0;
        const char *p = psc[c].inst ? psg_set[psc[c].inst - 1u].name : psg_set_n ? "PLAIN (PRESETS: SHAPES)" : "PLAIN";
        while (*p) s[i++] = *p++;
        s[i] = 0;
    } else if (sel == PANEL_DRUMS) {
        const char *p = kit_n ? "KIT: " : "NO KIT: USE THE PAGE";
        uint32_t i = 0;
        while (*p) s[i++] = *p++;
        if (kit_n) {
            if (kit_n >= 10u) s[i++] = (char)('0' + kit_n / 10u);
            s[i++] = (char)('0' + kit_n % 10u);
            p = " SOUNDS";
            while (*p) s[i++] = *p++;
        }
        s[i] = 0;
    } else s[0] = 0;
    draw_text_box(0, 132, 240, &FONT_S, s, C_SOFT, 1);
    {
        char o[12] = "OCT +0";
        o[4] = oct < 0 ? '-' : '+';
        o[5] = (char)('0' + (oct < 0 ? -oct : oct));
        draw_text_box(10, 206, 110, &FONT_S, o, C_OFF, 0);
    }
}

static const char *msg;                         /* a moment's word in the header (SAVED), until msg_until */
static uint32_t msg_until;

/* the lines redrawn every half second: only when their text changed, as each costs the main loop ~5 ms of SPI
 * (MIDI waits meanwhile). draw_frame and mon_open forget them */
static char shown_head[32], shown_time[24], shown_load[16];
static void draw_changed(char *shown, uint32_t cap, uint32_t x, uint32_t y, uint32_t w, const char *s, uint16_t c, int align)
{
    uint32_t i;
    for (i = 0; i < cap && shown[i] == s[i] && s[i]; i++) ;
    if (i < cap && shown[i] == s[i]) return;
    for (i = 0; i + 1u < cap && s[i]; i++) shown[i] = s[i];
    shown[i] = 0;
    draw_text_box(x, y, w, &FONT_S, s, c, align);
}

static void draw_head(uint32_t sel)             /* the page's name, or the song's status, or a message */
{
    char s[32], *p = s;
    const char *t = PAGE_NAME[page];
    uint32_t c1;
    if (msg && (int32_t)(fm1_ms - msg_until) < 0) t = msg;
    else if (song_text(s)) t = 0;
    else {
        msg = 0;
        while (*t) *p++ = *t++;
        if (page == PG_TONE || page == PG_ENV) { t = "  OP "; while (*t) *p++ = *t++; *p++ = (char)('1' + op_sel); }
        if (page <= PG_ENV && part_fm(sel, &c1) < 6u) {
            t = "  ALG ";
            while (*t) *p++ = *t++;
            *p++ = (char)('1' + fmc[part_fm(sel, &c1)].p.alg);
        }
        *p = 0;
        t = 0;
    }
    if (t) { while ((*p++ = *t++)) ; }
    draw_changed(shown_head, sizeof shown_head, 0, 150, 240, s, C_ACC, 1);
}

static void draw_knobs(uint32_t sel)            /* the page's four knobs: names, then values */
{
    uint32_t c1, c0 = part_fm(sel, &c1), k;
    patch_t *p = c0 < 6u ? &fmc[c0].p : 0;
    char s[12];
    cv_begin(240, 16, C_BLACK);
    for (k = 0; k < 4u; k++)
        cv_text((int32_t)(60u * k) + (60 - text_w(&FONT_S, KNOB_NAME[page][k])) / 2, 0, &FONT_S, KNOB_NAME[page][k], C_OFF);
    cv_blit(0, 168);
    cv_begin(240, 16, C_BLACK);
    for (k = 0; k < 4u; k++) {
        knob_text(page, k, knob(page, k, p, 0, 0), s);
        cv_text((int32_t)(60u * k) + (60 - text_w(&FONT_S, s)) / 2, 0, &FONT_S, s, C_WHITE);
    }
    cv_blit(0, 185);
    lcd_sync();
}

/* ---- SEQ: what the chips are doing, a row a channel, read from their state (a song's, or the panel's own) */
static uint8_t mon;                             /* the monitor is on the screen */

static void chip_patch(uint32_t c, patch_t *p, const char *name)   /* FM channel c's registers now, as a patch */
{
    uint32_t o = (c / 3u) * 0x100u + c % 3u, k, j;
    uint8_t v[35];
    v[0] = chip.regs[0xB0 + o];
    v[1] = chip.regs[0xB4 + o];
    for (k = 0; k < 4u; k++)
        for (j = 0; j < 7u; j++) v[2u + 7u * k + j] = chip.regs[0x30u + 0x10u * j + OPOFF[k] + o];
    v[30] = (v[1] & 0x37u) ? (uint8_t)(0x80u | chip.regs[0x22]) : 0u;
    v[31] = v[32] = v[33] = v[34] = 0;
    regs_to_patch(p, v, name);
}
static int32_t set_match(const patch_t *a)      /* the sound set's instrument this is (carrier levels aside: a song
                                                   moves them for its volume); -1: none */
{
    uint32_t i, k;
    for (i = 0; i < set_n; i++) {
        const patch_t *b = &set_bank[i];
        if (a->alg != b->alg || a->fb != b->fb || a->ams != b->ams || a->pms != b->pms) continue;
        for (k = 0; k < 4u; k++) {
            const op_t *x = &a->op[k], *y = &b->op[k];
            if (x->dt != y->dt || x->mul != y->mul || x->ks != y->ks || x->ar != y->ar || x->am != y->am ||
                x->d1r != y->d1r || x->d2r != y->d2r || x->sl != y->sl || x->rr != y->rr || x->ssg != y->ssg ||
                (!(CARRIERS[a->alg] >> k & 1u) && x->tl != y->tl))
                break;
        }
        if (k == 4u) return (int32_t)i;
    }
    return -1;
}

static int32_t fm_note_now(uint32_t c)          /* the MIDI note nearest FM channel c's pitch */
{
    uint32_t o = (c / 3u) * 0x100u + c % 3u, f = chip.regs[0xA0 + o] | (chip.regs[0xA4 + o] & 7u) << 8, i, best = 0;
    int32_t blk = chip.regs[0xA4 + o] >> 3 & 7;
    if (!f) return -1;
    while (f < 626u) { f <<= 1; blk--; }        /* (626: between B and C at block 4) */
    while (f >= 1252u) { f >>= 1; blk++; }
    for (i = 1; i < 13u; i++)
        if ((f > FNUM[i] ? f - FNUM[i] : FNUM[i] - f) < (f > FNUM[best] ? f - FNUM[best] : FNUM[best] - f)) best = i;
    return 12 * (blk + 1) + (int32_t)best;
}
static int32_t psg_note_now(uint32_t c)
{
    uint32_t x = (uint32_t)psg.reg[2u * c] << 4, i, best = 0, oct = 0;
    if (!x) return -1;
    while (x < (PSG_PER[11] + PSG_PER[12]) / 2u) { x <<= 1; oct++; }
    for (i = 1; i < 12u; i++)
        if ((x > PSG_PER[i] ? x - PSG_PER[i] : PSG_PER[i] - x) < (x > PSG_PER[best] ? x - PSG_PER[best] : PSG_PER[best] - x)) best = i;
    return (int32_t)(12u * oct + best);
}
static char *note_name(char *s, int32_t n)
{
    static const char N[] = "C C#D D#E F F#G G#A A#B ";
    s[0] = N[2 * (n % 12)];
    if (N[2 * (n % 12) + 1] != ' ') *++s = '#';
    s[1] = n < 12 ? '-' : (char)('0' + (n / 12 - 1) % 10);
    s[2] = 0;
    return s + 2;
}

/* the monitor is drawn a small piece at a time (a note, a meter, a name), one per pass of the main loop: the
 * panel's SPI takes ~5 ms for a whole row, and MIDI (a song's stream) must be read between */
typedef struct { char note[4]; uint8_t key, bar; char name[12]; } mon_row_t;
static mon_row_t mon_now[10], mon_shown[10];
static uint32_t mon_dirty;                      /* bit 3r + piece (0 note, 1 meter, 2 name): to draw */

static void mon_scan(void)                      /* each row's state now; the pieces that differ marked */
{
    uint32_t r;
    for (r = 0; r < 10u; r++) {
        mon_row_t *m = &mon_now[r];
        uint32_t bar = 0, key = 0, i;
        const char *t = "";
        int32_t n = -1;
        if (r < 6u) {                           /* FM: the loudest carrier, its envelope and level */
            const opn2_ch_t *ch = &chip.ch[r];
            uint32_t alg = chip.regs[0xB0 + (r / 3u) * 0x100u + r % 3u] & 7u, att = 0x3FFu, k;
            for (k = 0; k < 4u; k++)
                if (CARRIERS[alg] >> k & 1u) {
                    uint32_t a = ch->op[k].env + ch->op[k].total_level;   /* (both in 0.09 dB steps) */
                    if (a < att) att = a;
                    if (ch->op[k].key) key = 1;
                }
            bar = att >= 0x280u ? 0u : 1u + (0x27Fu - att) * 8u / 0x280u;
            if (r == 5u && chip.dac_enable) {
                int32_t d = (int32_t)(chip.dac_data >> 1) - 128;
                bar = (uint32_t)(d < 0 ? -d : d) / 16u;
                key = 1;
                m->note[0] = 'D'; m->note[1] = 'A'; m->note[2] = 'C'; m->note[3] = 0;
                t = "DRUMS";
            } else {
                patch_t p;
                int32_t s = -1;
                chip_patch(r, &p, "");
                s = set_match(&p);
                if (s >= 0) t = set_bank[s].name;
                else {
                    char *w = m->name;
                    const char *u = "ALG";
                    while (*u) *w++ = *u++;
                    *w++ = (char)('1' + alg);
                    u = " FB";
                    while (*u) *w++ = *u++;
                    *w++ = (char)('0' + p.fb);
                    *w = 0;
                    t = 0;
                }
                n = fm_note_now(r);
                if (n >= 0 && bar) note_name(m->note, n);
                else { m->note[0] = m->note[1] = m->note[2] = '-'; m->note[3] = 0; }
            }
        } else if (r < 9u) {                    /* PSG tones */
            uint32_t v = (uint32_t)psg.reg[2u * (r - 6u) + 1u] & 15u;
            bar = (15u - v) * 8u / 15u;
            key = v < 15u;
            n = psg_note_now(r - 6u);
            if (n >= 0 && key) note_name(m->note, n);
            else { m->note[0] = m->note[1] = m->note[2] = '-'; m->note[3] = 0; }
        } else {                                /* noise: white or periodic, its rate (3: tone 3's pitch) */
            uint32_t v = (uint32_t)psg.reg[7] & 15u, md = (uint32_t)psg.reg[6];
            bar = (15u - v) * 8u / 15u;
            key = v < 15u;
            m->note[0] = key ? (md & 4u ? 'W' : 'P') : '-';
            m->note[1] = key ? (char)('0' + (md & 3u)) : '-';
            m->note[2] = 0;
            t = key ? (md & 4u ? "WHITE" : "PERIODIC") : "";
        }
        if (t) {
            for (i = 0; i < 11u && t[i]; i++) m->name[i] = t[i];
            m->name[i] = 0;
        }
        m->bar = (uint8_t)bar;
        m->key = (uint8_t)key;
        for (i = 0; i < 4u && m->note[i] == mon_shown[r].note[i] && m->note[i]; i++) ;
        if (m->note[i] != mon_shown[r].note[i] || m->key != mon_shown[r].key) mon_dirty |= 1u << (3u * r);
        if (m->bar != mon_shown[r].bar) mon_dirty |= 2u << (3u * r);
        for (i = 0; i < 12u && m->name[i] == mon_shown[r].name[i] && m->name[i]; i++) ;
        if (i < 12u && m->name[i] != mon_shown[r].name[i]) mon_dirty |= 4u << (3u * r);
    }
}

static void mon_step(void)                      /* one piece, if any is due */
{
    static uint32_t at;
    uint32_t k, r, y;
    mon_row_t *m;
    if (!mon_dirty) return;
    for (k = 0; k < 30u && !(mon_dirty >> ((at + k) % 30u) & 1u); k++) ;
    at = (at + k) % 30u;
    mon_dirty &= ~(1u << at);
    r = at / 3u;
    y = 66u + 16u * r;
    m = &mon_now[r];
    switch (at % 3u) {
    case 0:
        cv_begin(36, 16, C_BLACK);
        cv_text(0, 0, &FONT_S, m->note, m->key ? C_WHITE : C_OFF);
        cv_blit(38, y);
        mon_shown[r].key = m->key;
        for (k = 0; k < 4u; k++) mon_shown[r].note[k] = m->note[k];
        break;
    case 1:
        cv_begin(54, 8, C_BLACK);
        for (k = 0; k < 8u; k++) cv_rect(7 * (int32_t)k, 0, 5, 8, k < m->bar ? C_ACC : RGB(40, 40, 46));
        cv_blit(76, y + 4u);
        mon_shown[r].bar = m->bar;
        break;
    default:
        cv_begin(102, 16, C_BLACK);
        cv_text(0, 0, &FONT_S, m->name, C_SOFT);
        cv_blit(138, y);
        for (k = 0; k < 12u; k++) mon_shown[r].name[k] = m->name[k];
        break;
    }
    at = (at + 1u) % 30u;
}

static void mon_open(void)                      /* the monitor's ground and labels; every piece then due */
{
    static const char *const L[10] = {"FM1", "FM2", "FM3", "FM4", "FM5", "FM6", "PSG1", "PSG2", "PSG3", "NSE"};
    uint32_t r;
    lcd_fill(0, 64, 240, 163, C_BLACK);
    shown_time[0] = 0x7F;
    for (r = 0; r < 10u; r++) {
        draw_text_box(2, 66u + 16u * r, 36, &FONT_S, L[r], C_OFF, 0);
        mon_shown[r].note[0] = mon_shown[r].name[0] = 0x7F;   /* (matches nothing: all redrawn) */
        mon_shown[r].bar = 0xFF;
    }
    mon_scan();
}

static void draw_mon_time(void)                 /* top right, by the title: the song's time, or LIVE */
{
    char s[24], *p = s;
    if (msg && (int32_t)(fm1_ms - msg_until) < 0) {
        const char *t = msg;
        while ((*p++ = *t++)) ;
    } else if (vgm.playing) {
        uint32_t sec = vgm.played / 44100u;
        p = put_num(p, (int32_t)(sec / 60u), 0);
        *p++ = ':';
        *p++ = (char)('0' + sec % 60u / 10u);
        *p++ = (char)('0' + sec % 10u);
        *p = 0;
    } else {
        const char *t = "LIVE";
        while ((*p++ = *t++)) ;
    }
    draw_changed(shown_time, sizeof shown_time, 176, 10, 60, s, C_OFF, 2);
}

/* REC: each FM channel's sound now onto the panel's FM parts (and MIDI channels 1-6): the set's instrument when it
 * is one, else the registers as they are, the loudest carrier at full level */
static void grab(void)
{
    uint32_t c, k;
    for (c = 0; c < 6u; c++) {
        patch_t p;
        int32_t m;
        char name[12] = "GRAB FM1";
        name[7] = (char)('1' + c);
        chip_patch(c, &p, name);
        if ((m = set_match(&p)) >= 0) {
            fmc[c].p = set_bank[m];
            fmc[c].preset = (uint8_t)m;
        } else {
            uint32_t lift = 127u;
            for (k = 0; k < 4u; k++)
                if (CARRIERS[p.alg] >> k & 1u && p.op[k].tl < lift) lift = p.op[k].tl;
            for (k = 0; k < 4u; k++)
                if (CARRIERS[p.alg] >> k & 1u) p.op[k].tl = (uint8_t)(p.op[k].tl - lift);
            fmc[c].p = p;
        }
        if (fmc[c].p.lfo & 0x80u) lfo_reg = fmc[c].p.lfo & 0x0Fu;
        fmc[c].pan = 3;
        if (!vgm.playing) new_instrument(c);   /* (playing: restore_panel puts them on the chip at its end) */
    }
}

static void draw_load(uint32_t ticks)          /* of the interrupt's 5.8 ms */
{
    char s[16] = "CPU ", *p = s + 4;
    uint32_t pct = ticks * 100u / 139320u, t[4], n = 0;
    do { t[n++] = pct % 10u; pct /= 10u; } while (pct);
    while (n) *p++ = (char)('0' + t[--n]);
    *p++ = '%';
    *p = 0;
    draw_changed(shown_load, sizeof shown_load, 120, 206, 110, s, C_OFF, 2);
}

/* "OPN-1" in chrome: the letters drawn in a marker colour on the canvas, then each row given the gradient of a
 * polished metal: sky above the horizon, a dark band at it, warm ground below */
static uint16_t chrome(uint32_t y, uint32_t h)
{
    uint32_t t = y * 256u / h, r, g, b;
    if (t < 120u) { r = 150 + t * 105 / 120; g = 170 + t * 85 / 120; b = 220 + t * 35 / 120; }      /* sky to white */
    else if (t < 136u) { r = 70; g = 78; b = 100; }                                                 /* the horizon */
    else { t -= 136u; r = 90 + t * 150 / 120; g = 84 + t * 130 / 120; b = 96 + t * 90 / 120; }     /* ground */
    return RGB(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
}
static void draw_frame(void)                    /* the parts that never change */
{
    uint32_t i, n;
    const char *title = "OPN-1";
    lcd_fill(0, 0, 240, 240, C_BLACK);
    shown_head[0] = shown_time[0] = shown_load[0] = 0x7F;   /* (matches nothing: drawn again) */
    int32_t x = (int32_t)(240u - text_w(&FONT_L, title)) / 2 - 1;
    cv_begin(240, 40, C_BLACK);
    for (n = 0; n < 4u; n++)                       /* a shadow, then the letters twice as thick */
        cv_text(x + 2 + (int32_t)(n & 1u), 5 + (int32_t)(n >> 1), &FONT_L, title, C_RED_DK);
    for (n = 0; n < 4u; n++) cv_text(x + (int32_t)(n & 1u), 3 + (int32_t)(n >> 1), &FONT_L, title, C_MARK);
    for (n = 0; n < cv_w * cv_h; n++)
        if (cv_px[n] == swap16(C_MARK)) cv_px[n] = swap16(chrome(n / cv_w - 3u, 34u));
    cv_blit(0, 6);
    lcd_fill(0, 50, 240, 5, C_ACC);             /* the stripes */
    lcd_fill(0, 58, 240, 2, C_ACC);
    lcd_fill(0, 63, 240, 1, C_RED_DK);
    draw_text_box(0, 68, 240, &FONT_S, "16-BIT FM SOUND", C_OFF, 1);
    lcd_fill(0, 228, 240, 2, C_ACC);
    for (i = 0; i < 240u; i += 8u) lcd_fill(i, 232, 4, 1, C_RED_DK);
}

static void app_main(void)
{
    uint32_t prev = 0, k, sel = PANEL_POLY, drawn_sel = ~0u, t_load = 0, t_mon = 0, was_playing = 0, mon_auto = 0;
    int32_t oct = 0, drawn_oct = 99;
    uint8_t held[27] = {0};

    opn2_reset(&chip);
    psg_reset(&psg);
    store_load();                               /* the last sound set sent, if any */
    noise_default();
    for (k = 0; k < 4u; k++) { psg_vol(k, 15); psc[k].bend_range = 2; }
    arp.octs = 1;
    arp.gate = 50;
    arp.bpm = 120;
    arp.rnd = 1;
    for (k = 0; k < 6u; k++) {
        fmc[k].vel = 100;
        fmc[k].bend_range = 2;
        fmc[k].pan = 3;
        if (set_n) load_preset(k, 0);
        else fm_patch(k);                       /* (an empty patch: silent) */
    }
    draw_frame();

    for (;;) {
        uint32_t keys, down, up, b, rel, base;
        uint8_t st, d1, d2;
        int32_t turn;
        platform_poll();

        base = (uint32_t)(53 + 12 * oct);       /* key 0 = F3 */
        keys = fm1_in.notes;
        down = keys & ~prev;
        up = prev & ~keys;
        prev = keys;
        for (k = 0; k < 27u; k++) {
            if (down >> k & 1u) {
                held[k] = (uint8_t)(base + k);
                key_down(sel, held[k], key_vel);
            }
            if (up >> k & 1u) key_up(held[k]);
            fm1_led_key(KEY_LED(k), keys >> k & 1u);
        }

        b = fm1_input_edges(&rel);
        if (b & (1u << BTN_OCT_DOWN) && oct > -3) oct--;
        if (b & (1u << BTN_OCT_UP) && oct < 3) oct++;
        if (b & (1u << BTN_PLAY) && vgm.playing) song_request(SONG_STOP);   /* (the page starts songs: it streams them) */
        if (b & (1u << BTN_HOME)) page = PG_SOUND;
        if (b & (1u << BTN_LFO)) page = PG_LFO;
        if (b & (1u << BTN_FX)) page = PG_FX;
        if (b & (1u << BTN_GLO)) page = PG_GLO;
        if (b & (1u << BTN_EDIT)) { if (page == PG_TONE) op_sel = (op_sel + 1u) & 3u; page = PG_TONE; }   /* again: the next operator */
        if (b & (1u << BTN_ENV)) { if (page == PG_ENV) op_sel = (op_sel + 1u) & 3u; page = PG_ENV; }
        if (b & (1u << BTN_SEL)) {              /* the next operator on its pages, else the next part */
            if (page == PG_TONE || page == PG_ENV) op_sel = (op_sel + 1u) & 3u;
            else sel = (sel + 1u) % NPANEL;
        }
        if (b & (1u << BTN_ARP)) {              /* its page and on; on its page, on or off */
            if (page == PG_ARP && arp.on) { arp.on = 0; arp.n = 0; arp_stop(); }
            else arp.on = 1;
            page = PG_ARP;
        }
        if (b & (1u << BTN_SAVE)) {             /* the part's sound, as the knobs left it, over its instrument in flash */
            uint32_t c1, c0 = part_fm(sel, &c1);
            if (c0 < 6u && set_n) {
                set_bank[fmc[c0].preset] = fmc[c0].p;
                msg = store_save() ? "SAVE FAILED" : "SAVED";
            } else msg = set_n ? "NOTHING TO SAVE HERE" : "NO SOUNDS TO SAVE";
            msg_until = fm1_ms + 1500u;
        }
        if (b & (1u << BTN_REC)) {              /* the chips' sounds now onto the FM parts */
            grab();
            msg = "GRABBED";
            msg_until = fm1_ms + 1500u;
            t_load = fm1_ms - 500u;             /* (the word at once) */
        }
        if (b & (1u << BTN_SEQ)) {              /* the monitor, or back to the panel */
            mon = !mon;
            mon_auto = 0;
            if (mon) mon_open();
            else { draw_frame(); drawn_oct = 99; }
            t_load = fm1_ms - 500u;
        }
        if (b & ~(1u << BTN_OCT_DOWN | 1u << BTN_OCT_UP | 1u << BTN_PLAY)) drawn_sel = ~0u;
        if (knobs_turn(sel)) drawn_sel = ~0u;
        turn = fm1_enc_take(ENC_SELECT);
        if (turn) sel = (uint32_t)((int32_t)sel + turn + 64 * NPANEL) % NPANEL;
        turn = fm1_enc_take(ENC_PRESETS);
        if (turn && (sel < PANEL_PSG || sel == PANEL_POLY)) {
            uint32_t c0 = sel == PANEL_POLY ? 0 : sel, c1 = sel == PANEL_POLY ? 6 : sel + 1u, c;
            uint32_t n = n_instruments(), i = n ? (uint32_t)((int32_t)fmc[c0].preset + turn + 64 * (int32_t)n) % n : 0;
            for (c = c0; c < c1; c++) load_preset(c, i);
            drawn_sel = ~0u;
        } else if (turn && sel >= PANEL_PSG && sel < PANEL_DRUMS) {   /* a PSG part: its instrument, or plain */
            uint32_t c = sel - PANEL_PSG;
            uint32_t n = psg_set_n + 1u, i = psc[c].inst, k;
            int32_t step = turn > 0 ? 1 : -1;
            for (; turn; turn -= step)          /* (the noise part: its shapes only, and plain) */
                for (k = 0; k < n; k++) {
                    i = (uint32_t)((int32_t)i + step + (int32_t)n) % n;
                    if (c != 3u || !i || is_noise_inst(i)) break;
                }
            psc[c].inst = (uint8_t)i;
            drawn_sel = ~0u;
        }

        while (midi_get(&st, &d1, &d2)) {       /* GenMDM's layout */
            uint32_t ch = st & 15u, type = st & 0xF0u;
            if (song_rx(st, d1, d2)) continue;
            if (midi_panel && ch != 9u) {       /* PANEL: any channel plays the part on the screen, as its keys do */
                if (type == 0x90u && d2) key_down(sel, d1, d2);
                else if (type == 0x80u || type == 0x90u) key_up(d1);
                else if (type == 0xE0u) part_bend(sel, (int32_t)(d1 | d2 << 7) - 8192);
                continue;
            }
            if (ch < 6u) {
                if (type == 0x90u && d2) fm_note_on(ch, d1, d2, 0);
                else if (type == 0x80u || type == 0x90u) fm_note_off(ch, d1);
                else if (type == 0xB0u) fm_cc(ch, d1, d2);
                else if (type == 0xC0u) load_preset(ch, d1);
                else if (type == 0xE0u) { fmc[ch].bend = (int16_t)((d1 | d2 << 7) - 8192); fm_pitch(ch); }
            } else if (ch == 9u && kit_n) {     /* with a kit, channel 10 is the drums: note 36 (C2) the first */
                if (type == 0x90u && d2 && d1 >= 36u) drum(d1 - 36u, d2);
            } else if (ch <= 10u) {             /* 7-9 the tones, 10 the noise (without a kit), 11 the noise always */
                uint32_t pc = ch == 10u ? 3u : ch - 6u;
                if (type == 0x90u && d2) psg_note_on(pc, d1, d2);
                else if (type == 0x80u || type == 0x90u) psg_note_off(pc, d1);
                else if (type == 0xE0u && pc < 3u) { psc[pc].bend = (int16_t)((d1 | d2 << 7) - 8192); psg_pitch(pc); }
                else if (type == 0xB0u && d1 == 81u) psc[pc].bend_range = (uint8_t)(d2 * 18u >> 7);
                else if (type == 0xC0u) { psc[pc].inst = (uint8_t)(d1 < psg_set_n ? d1 + 1u : 0u); drawn_sel = ~0u; }
            }
            if (type == 0xC0u && ch < 6u) drawn_sel = ~0u;
        }

        fm1_led_key(BTN_OCT_DOWN, oct < 0);
        fm1_led_key(BTN_OCT_UP, oct > 0);
        fm1_led_key(BTN_HOME, page == PG_SOUND);
        fm1_led_key(BTN_EDIT, page == PG_TONE);
        fm1_led_key(BTN_ENV, page == PG_ENV);
        fm1_led_key(BTN_LFO, page == PG_LFO);
        fm1_led_key(BTN_FX, page == PG_FX);
        fm1_led_key(BTN_GLO, page == PG_GLO);
        fm1_led_key(BTN_ARP, arp.on);
        fm1_led_key(BTN_SEQ, mon);
        if (panel_stale) { panel_stale = 0; drawn_sel = ~0u; if (mon) mon_open(); }
        if (inst_changed) {                     /* the panel's instrument changed: to the octave its song used */
            uint32_t c = sel < PANEL_PSG ? sel : sel == PANEL_POLY ? 0u : 6u;
            if (c < 6u && (inst_changed >> c & 1u) && fmc[c].p.oct) {
                oct = (int32_t)fmc[c].p.oct - 64;
                if (oct < -3) oct = -3;
                if (oct > 3) oct = 3;
            }
            inst_changed = 0;
        }
        if (!mon && (sel != drawn_sel || oct != drawn_oct)) {
            drawn_sel = sel;
            drawn_oct = oct;
            draw_panel(sel, oct);
            draw_head(sel);
            draw_knobs(sel);
        }
        song_report();
        if (vgm.playing != was_playing) {       /* a song started or ended */
            was_playing = vgm.playing;
            midi_send(0xBF, 0x74, was_playing);
            fm1_led_key(BTN_PLAY, (int)was_playing);
            if (!was_playing) restore_panel();
            if (was_playing && !mon) {          /* a song: the monitor comes up, and goes at its end */
                mon = mon_auto = 1;
                mon_open();
            } else if (!was_playing && mon_auto) {
                mon = mon_auto = 0;
                draw_frame();
                drawn_sel = ~0u;
                drawn_oct = 99;
            }
        }
        if (mon && fm1_ms - t_mon >= 50u) {
            t_mon = fm1_ms;
            mon_scan();
        }
        if (mon) mon_step();
        arp_run(sel);
        vibrato();
        psg_shapes();
        if (fm1_ms - t_load >= 500u) {          /* the worst interrupt of the last half second */
            t_load = fm1_ms;
            if (mon) draw_mon_time();
            else {
                draw_load(load_ticks);
                draw_head(sel);
            }
            load_ticks = 0;
        }
    }
}
