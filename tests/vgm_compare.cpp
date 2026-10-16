// SPDX-License-Identifier: GPL-3.0-only
// The VGM player (firmware/src/vgm.c: block rendering between commands, on opn2.c and psg.c) against a plain
// interpreter written separately here, one chip sample at a time on ymfm and MAME's PSG. Both read the same
// synthetic VGM, which uses every command the player handles: FM and PSG writes, all the waits, key-off/on a
// sample or two apart, two PCM blocks with 0xE0 seeks and 0x8n DAC writes, a DAC stream started by 0x95 and
// by 0x93, and a loop. The two must agree on every sample. Run tests/run_vgm_compare.sh.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "mame_segapsg.h"
#include "ymfm_opn.h"
extern "C" {
#include "vgm.h"
}

static const int FM_GAIN = 192, PSG_GAIN = 96;
static const uint32_t PSG_STEP = 275251u, CHIP_PER_VGM = 79159u, HZ_TO_INC = 80631u;

// ---- a VGM writer
struct Vgm {
    std::vector<uint8_t> b = std::vector<uint8_t>(0x80, 0);
    uint32_t loop_at = 0;
    void u8(uint32_t v) { b.push_back((uint8_t)v); }
    void u32(uint32_t v) { for (int i = 0; i < 4; i++) u8(v >> (8 * i)); }
    void ym(int part, int reg, int val) { u8(0x52 + part); u8(reg); u8(val); }
    void psg(int v) { u8(0x50); u8(v); }
    void wait(uint32_t n)
    {
        while (n) {
            uint32_t w = n > 65535 ? 65535 : n;
            if (w == 735) u8(0x62);
            else if (w == 882) u8(0x63);
            else if (w <= 16) u8(0x70 + w - 1);
            else { u8(0x61); u8(w & 255); u8(w >> 8); }
            n -= w;
        }
    }
    void block(const std::vector<uint8_t> &d) { u8(0x67); u8(0x66); u8(0x00); u32((uint32_t)d.size()); for (auto x : d) u8(x); }
    std::vector<uint8_t> finish()
    {
        u8(0x66);
        auto put = [&](uint32_t at, uint32_t v) { for (int i = 0; i < 4; i++) b[at + i] = (uint8_t)(v >> (8 * i)); };
        b[0] = 'V', b[1] = 'g', b[2] = 'm', b[3] = ' ';
        put(0x04, (uint32_t)b.size() - 4);
        put(0x08, 0x171);
        put(0x0C, 3579545);
        put(0x1C, loop_at ? loop_at - 0x1C : 0);
        put(0x2C, 7670453);
        put(0x34, 0x80 - 0x34);
        return b;
    }
};

static std::vector<uint8_t> make_song()
{
    Vgm v;
    std::vector<uint8_t> kick(1500), snare(900);
    for (size_t i = 0; i < kick.size(); i++) kick[i] = (uint8_t)(128 + 100 * sin(i * 0.05 * (1.0 - i / 1800.0)) * (1.0 - i / 1500.0));
    uint32_t r = 7;
    for (size_t i = 0; i < snare.size(); i++) r = r * 1103515245u + 12345u, snare[i] = (uint8_t)(128 + ((int)(r >> 24) - 128) * (900 - (int)i) / 900);
    v.block(kick);
    v.block(snare);
    // FM: a patch on channels 1, 2 and 4 (part 1), LFO on
    v.ym(0, 0x22, 0x0A);
    for (int ch : {0, 1, 3}) {
        int part = ch / 3, c = ch % 3;
        v.ym(part, 0xB0 + c, 0x3C | (ch & 3));
        v.ym(part, 0xB4 + c, 0xC0 | 0x11);
        for (int s = 0; s < 4; s++) {
            int o = s * 4 + c;
            v.ym(part, 0x30 + o, 0x01 + s);
            v.ym(part, 0x40 + o, s == 3 ? 0x08 : 0x20 + 4 * s);
            v.ym(part, 0x50 + o, 0x1F);
            v.ym(part, 0x60 + o, 0x88);
            v.ym(part, 0x70 + o, 0x03);
            v.ym(part, 0x80 + o, 0x37);
            v.ym(part, 0x90 + o, s == 1 ? 0x0A : 0);
        }
    }
    v.ym(0, 0x2B, 0x80);                        // DAC on (channel 6)
    // the stream: id 0 on the YM2612 port 0 register 0x2A, bank 0, 11025 Hz
    v.u8(0x90); v.u8(0); v.u8(0x02); v.u8(0); v.u8(0x2A);
    v.u8(0x91); v.u8(0); v.u8(0x00); v.u8(1); v.u8(0);
    v.u8(0x92); v.u8(0); v.u32(11025);
    v.loop_at = (uint32_t)v.b.size();
    static const int FN[] = {644, 723, 811, 859, 965, 1083, 1215, 1288};
    for (int step = 0; step < 16; step++) {
        int ch = step % 3 == 2 ? 3 : step % 3, part = ch / 3, c = ch % 3, code = c | part << 2;
        int fn = FN[step % 8];
        v.ym(part, 0xA4 + c, (3 + step % 2) << 3 | fn >> 8);
        v.ym(part, 0xA0 + c, fn & 0xFF);
        v.ym(0, 0x28, code);                    // key off, then on 1-3 samples later: must retrigger
        v.wait(1 + step % 3);
        v.ym(0, 0x28, 0xF0 | code);
        v.psg(0x80 | (step % 3) << 5 | (step & 15)); v.psg(0x08 + step);   // a PSG tone
        v.psg(0x90 | (step % 3) << 5 | (step & 7));
        if (step % 4 == 0) {                    // kick by 0x8n from the bank start
            v.u8(0xE0); v.u32(0);
            for (int i = 0; i < 400; i++) v.u8(0x80 + (i % 3 == 0 ? 2 : 3));
        } else if (step % 4 == 2) {             // snare by the stream: fast start, block 1
            v.u8(0x95); v.u8(0); v.u8(1); v.u8(0); v.u8(0);
            v.wait(2000);
        } else if (step == 5) {                 // the stream by 0x93: offset 300, 500 bytes
            v.u8(0x93); v.u8(0); v.u32(300); v.u8(1); v.u32(500);
            v.wait(1500);
        } else {
            v.psg(0xE4 | (step & 3)); v.psg(0xF2);   // noise
            v.wait(735);
            v.psg(0xFF);
        }
        v.wait(882 + 37 * step);
        if (step == 9) { v.u8(0x94); v.u8(0); }
    }
    return v.finish();
}

// ---- the reference: a plain interpreter, one chip sample at a time
struct Ref {
    const std::vector<uint8_t> &b;
    ymfm::ymfm_interface intf;
    ymfm::ym2612 ym{intf};
    mame_segapsg psg;
    std::vector<uint8_t> bank;
    std::vector<uint32_t> blk_at, blk_len;
    uint32_t pos, loop, end, bank_pos = 0, frac = 0;
    uint64_t t_vgm = 0;                         // VGM time of the next command
    int32_t held = 0;
    bool playing = true;
    struct S { bool on = false, loop = false; uint32_t port = 0, reg = 0, step = 1, base = 0, freq = 0, inc = 0, start = 0, len = 0, pos = 0, left = 0, phase = 0; } st;

    explicit Ref(const std::vector<uint8_t> &v) : b(v)
    {
        auto r32 = [&](uint32_t a) { return b[a] | b[a + 1] << 8 | b[a + 2] << 16 | (uint32_t)b[a + 3] << 24; };
        ym.reset();
        pos = 0x34 + r32(0x34);
        loop = 0x1C + r32(0x1C);
        end = r32(4) + 4;
        for (uint32_t p = pos; p < end && b[p] == 0x67; p += 7 + r32(p + 3)) {   // (the blocks lead the file)
            blk_at.push_back((uint32_t)bank.size());
            blk_len.push_back(r32(p + 3));
            bank.insert(bank.end(), b.begin() + p + 7, b.begin() + p + 7 + r32(p + 3));
        }
    }
    uint32_t r32(uint32_t a) const { return b[a] | b[a + 1] << 8 | b[a + 2] << 16 | (uint32_t)b[a + 3] << 24; }
    uint8_t bank_at(uint32_t a) const { return a < bank.size() ? bank[a] : 0x80; }
    void ymw(int part, int reg, int val) { ym.write(part * 2, reg); ym.write(part * 2 + 1, val); }

    void sample(uint64_t t, int32_t *o)         // everything due at chip sample t, then the sample
    {
        while (playing && ((t_vgm * CHIP_PER_VGM) >> 16) <= t) {
            uint8_t c = b[pos];
            switch (c) {
            case 0x67: pos += 7 + r32(pos + 3); break;
            case 0x50: psg.write(b[pos + 1]); pos += 2; break;
            case 0x52: case 0x53: ymw(c - 0x52, b[pos + 1], b[pos + 2]); pos += 3; break;
            case 0x61: t_vgm += b[pos + 1] | b[pos + 2] << 8; pos += 3; break;
            case 0x62: t_vgm += 735; pos++; break;
            case 0x63: t_vgm += 882; pos++; break;
            case 0x66: pos = loop; break;
            case 0xE0: bank_pos = r32(pos + 1); pos += 5; break;
            case 0x90: st.port = b[pos + 3]; st.reg = b[pos + 4]; pos += 5; break;
            case 0x91: st.step = b[pos + 3] ? b[pos + 3] : 1; st.base = b[pos + 4]; pos += 5; break;
            case 0x92: st.freq = r32(pos + 2); st.inc = (st.freq * HZ_TO_INC) >> 16; pos += 6; break;
            case 0x93: {
                uint32_t off = r32(pos + 2);
                st.start = st.pos = off == 0xFFFFFFFFu ? st.start : off + st.base;
                st.len = st.left = r32(pos + 7);    // (mode 1 in the song: a count of bytes)
                st.loop = b[pos + 6] & 0x80;
                st.phase = 0;
                st.on = st.len != 0;
                pos += 11;
            } break;
            case 0x94: st.on = false; pos += 2; break;
            case 0x95: {
                uint32_t id = b[pos + 2] | b[pos + 3] << 8;
                st.start = st.pos = blk_at[id] + st.base;
                st.len = st.left = blk_len[id] / st.step;
                st.loop = b[pos + 4] & 1;
                st.phase = 0;
                st.on = st.len != 0;
                pos += 5;
            } break;
            default:
                if (c >= 0x70 && c <= 0x7F) t_vgm += (c & 15) + 1;
                else if (c >= 0x80 && c <= 0x8F) { ymw(0, 0x2A, bank_at(bank_pos++)); t_vgm += c & 15; }
                pos++;
            }
        }
        while (st.on && st.phase >= 65536u) {
            st.phase -= 65536u;
            ymw(st.port, st.reg, bank_at(st.pos));
            st.pos += st.step;
            if (--st.left == 0) { if (st.loop) st.pos = st.start, st.left = st.len; else st.on = false; }
        }
        ymfm::ym2612::output_data od;
        ym.generate(&od, 1);
        frac += PSG_STEP;                       // the PSG: the mean of the ticks in this sample
        uint32_t k = frac >> 16;
        frac &= 0xFFFF;
        if (k) {
            int32_t sum = 0;
            for (uint32_t i = 0; i < k; i++) { int16_t m = 0; for (int j = 0; j < 8; j++) m = psg.sample(); sum += m; }
            held = sum / (int32_t)k;
        }
        o[0] = (od.data[0] * FM_GAIN + held * PSG_GAIN) >> 8;
        o[1] = (od.data[1] * FM_GAIN + held * PSG_GAIN) >> 8;
        if (st.on) st.phase += st.inc;
    }
};

// ---- streaming: the file split as web/index.html splits it (vgm_split), fed through a small ring
struct Split { std::vector<uint8_t> image, cmds; uint32_t loop = 0; bool has_loop = false; };
static Split vgm_split(const std::vector<uint8_t> &b)
{
    auto r32 = [&](uint32_t a) { return b[a] | b[a + 1] << 8 | b[a + 2] << 16 | (uint32_t)b[a + 3] << 24; };
    Split o;
    uint32_t ver = r32(8), start = ver >= 0x150 && r32(0x34) ? 0x34 + r32(0x34) : 0x40;
    uint32_t end = std::min<uint32_t>((uint32_t)b.size(), r32(4) + 4), loop = r32(0x1C) ? 0x1C + r32(0x1C) : 0;
    std::vector<uint32_t> lens;
    std::vector<uint8_t> bank;
    auto clen = [&](uint32_t p) -> uint32_t {
        uint8_t c = b[p];
        if (c == 0x67) return 7 + r32(p + 3);
        if ((c >= 0x30 && c <= 0x3F) || c == 0x4F || c == 0x50 || c == 0x94) return 2;
        if (c >= 0x40 && c <= 0x5F) return 3;
        if (c == 0x61) return 3;
        if (c == 0x62 || c == 0x63 || c == 0x66 || (c >= 0x70 && c <= 0x8F)) return 1;
        if (c == 0x68) return 12;
        if (c == 0x90 || c == 0x91 || c == 0x95) return 5;
        if (c == 0x92) return 6;
        if (c == 0x93) return 11;
        if (c >= 0xA0 && c <= 0xBF) return 3;
        if (c >= 0xC0 && c <= 0xDF) return 4;
        if (c >= 0xE0) return 5;
        return 0;
    };
    for (uint32_t p = start; p < end;) {
        if (p == loop) o.loop = (uint32_t)o.cmds.size(), o.has_loop = true;
        uint32_t k = clen(p);
        if (!k) break;
        if (b[p] == 0x67) {
            if (b[p + 2] == 0x00) { lens.push_back(k - 7); bank.insert(bank.end(), b.begin() + p + 7, b.begin() + p + k); }
        } else o.cmds.insert(o.cmds.end(), b.begin() + p, b.begin() + p + k);
        if (b[p] == 0x66) break;
        p += k;
    }
    auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) o.image.push_back((uint8_t)(v >> (8 * i))); };
    put32((uint32_t)lens.size());
    for (auto l : lens) put32(l);
    o.image.insert(o.image.end(), bank.begin(), bank.end());
    return o;
}

// the sender: the commands up to (not with) the final 0x66, then the loop section again and again
struct Sender {
    const Split &s;
    uint32_t at = 0;
    bool loops;
    Sender(const Split &x, bool l) : s(x), loops(l) {}
    bool next(uint8_t *byte)
    {
        uint32_t stop = (uint32_t)s.cmds.size() - (loops && s.has_loop ? 1 : 0);   // (drop the 0x66 when looping)
        if (at >= stop) {
            if (!(loops && s.has_loop)) return false;
            at = s.loop;
        }
        *byte = s.cmds[at++];
        return true;
    }
};

static int stream_check(const std::vector<uint8_t> &song, const char *label, uint32_t samples)
{
    static vgm_t a, b;
    static opn2_t ca, cb;
    static psg_t pa, pb;
    static uint8_t ring[4096];
    static volatile uint32_t fill;
    Split sp = vgm_split(song);
    if (vgm_open(&a, song.data(), (uint32_t)song.size()) != VGM_OK) { printf("%s: vgm_open failed\n", label); return 1; }
    a.loops = 1;
    fill = 0;
    if (vgm_open_stream(&b, sp.image.data(), (uint32_t)sp.image.size(), ring, sizeof ring - 1, &fill) != VGM_OK) {
        printf("%s: vgm_open_stream failed\n", label);
        return 1;
    }
    vgm_play(&a, &ca, &pa);
    vgm_play(&b, &cb, &pb);
    Sender tx(sp, true);
    std::vector<int32_t> oa(2 * 300), ob(2 * 300);
    static const uint32_t PIECE[] = {32, 32, 7, 64, 1, 300, 32, 13};
    uint32_t diffs = 0, first = 0;
    for (uint32_t t = 0, i = 0; t < samples; i++) {
        uint8_t x;
        while (fill - b.pos < sizeof ring && tx.next(&x)) ring[fill & (sizeof ring - 1)] = x, fill = fill + 1;
        uint32_t n = PIECE[i % 8];
        if (n > samples - t) n = samples - t;
        vgm_render(&a, &ca, &pa, oa.data(), n, FM_GAIN, PSG_GAIN);
        vgm_render(&b, &cb, &pb, ob.data(), n, FM_GAIN, PSG_GAIN);
        for (uint32_t j = 0; j < 2 * n; j++)
            if (oa[j] != ob[j]) { if (!diffs) first = t + j / 2; diffs++; }
        t += n;
    }
    printf("%s: split into %zu B of PCM and %zu B of commands (loop at %u); streamed through a %zu B ring: ",
           label, sp.image.size(), sp.cmds.size(), sp.loop, sizeof ring);
    if (diffs || b.starve_count) { printf("FAIL %u differ (first at %u), ran dry %u times\n", diffs, first, b.starve_count); return 1; }
    printf("identical to the whole file\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1) {                             // a real file (uncompressed .vgm): the stream check on it, a minute
        FILE *f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 1; }
        std::vector<uint8_t> b;
        int c;
        while ((c = fgetc(f)) != EOF) b.push_back((uint8_t)c);
        fclose(f);
        return stream_check(b, argv[1], 53267 * 60);
    }
    std::vector<uint8_t> song = make_song();
    if (FILE *f = fopen("build/host/cmp/test.vgm", "wb")) { fwrite(song.data(), 1, song.size(), f); fclose(f); }   // for tests/upload_script.py
    static vgm_t v;
    static opn2_t chip;
    static psg_t psg;
    int e = vgm_open(&v, song.data(), (uint32_t)song.size());
    if (e != VGM_OK) { printf("vgm_open: %d\n", e); return 1; }
    v.loops = 1;
    vgm_play(&v, &chip, &psg);
    Ref ref(song);

    // render in uneven pieces, as the audio interrupt asks; far enough to loop twice
    const uint32_t N = 53267 * 30;
    std::vector<int32_t> ours(2 * N), theirs(2 * N);
    static const uint32_t PIECE[] = {32, 32, 7, 64, 1, 300, 32, 13};
    for (uint32_t t = 0, i = 0; t < N; i++) {
        uint32_t n = PIECE[i % 8];
        if (n > N - t) n = N - t;
        vgm_render(&v, &chip, &psg, &ours[2 * t], n, FM_GAIN, PSG_GAIN);
        t += n;
    }
    for (uint32_t t = 0; t < N; t++) ref.sample(t, &theirs[2 * t]);

    uint32_t diffs = 0, first = 0, loud = 0;
    for (uint32_t i = 0; i < 2 * N; i++) {
        if (ours[i] != theirs[i]) { if (!diffs) first = i / 2; diffs++; }
        if (abs(ours[i]) > 2000) loud++;
    }
    printf("song %zu bytes, %u PCM blocks, %u samples (%u loud)\n", song.size(), v.nblk, N, loud);
    if (diffs) { printf("FAIL: %u values differ, first at sample %u\n", diffs, first); return 1; }
    printf("identical to the reference, sample for sample\n");
    if (stream_check(song, "test song", N)) return 1;
    FILE *f = fopen("build/host/cmp/vgm-ours.raw", "wb");
    if (f) { for (auto x : ours) { int16_t s = (int16_t)(x > 32767 ? 32767 : x < -32767 ? -32767 : x); fwrite(&s, 2, 1, f); } fclose(f); }
    return 0;
}
