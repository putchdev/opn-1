// SPDX-License-Identifier: GPL-3.0-only
// Our YM2612 core (firmware/src/opn2.c) against two references, on the same register writes:
//   ymfm       must match sample for sample (opn2.c is a port of it)
//   Nuked-OPN2 cycle-accurate, so the measure of how close we are to the chip; reported, not required
// Run tests/run_opn2_compare.sh. WAVs (at the chip's rate) go to build/host/cmp/ for listening.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "ymfm_opn.h"
extern "C" {
#include "opn2.h"
#include "ym3438.h"
}

struct Ev { uint32_t at; uint8_t part, reg, val; };   // at: the sample the write is due
struct Scenario { std::string name; uint32_t samples; std::vector<Ev> ev; };

// ---- the three cores behind one interface; one register write per sample at most, the same for all

struct Cores {
    ymfm::ymfm_interface intf;
    ymfm::ym2612 ref{intf};
    opn2_t ours;
    ym3438_t nuked;
    Cores()
    {
        ref.reset();
        opn2_reset(&ours);
        OPN2_SetChipType(ym3438_mode_ym2612);
        OPN2_Reset(&nuked);
    }
    void write(const Ev &e)
    {
        int16_t buf[2];
        ref.write(e.part * 2, e.reg);
        ref.write(e.part * 2 + 1, e.val);
        opn2_write_reg(&ours, e.part, e.reg, e.val);
        OPN2_Write(&nuked, e.part * 2, e.reg);          // Nuked latches a write on a clock: space them
        for (int i = 0; i < 12; i++) OPN2_Clock(&nuked, buf);
        OPN2_Write(&nuked, e.part * 2 + 1, e.val);
        nuked_clocks_owed = 12;
        for (int i = 0; i < 12; i++) OPN2_Clock(&nuked, buf), nuked_l += buf[0], nuked_r += buf[1];
    }
    int nuked_clocks_owed = 0;
    int32_t nuked_l = 0, nuked_r = 0;
    void sample(int32_t *o)
    {
        ymfm::ym2612::output_data od;
        int16_t buf[2];
        ref.generate(&od, 1);
        o[0] = od.data[0], o[1] = od.data[1];
        opn2_clock(&ours, &o[2], &o[3]);
        // Nuked: a sample is 24 clocks; a write this sample already ran 12 of them
        int32_t l = nuked_clocks_owed ? nuked_l : 0, r = nuked_clocks_owed ? nuked_r : 0;
        for (int i = nuked_clocks_owed; i < 24; i++) OPN2_Clock(&nuked, buf), l += buf[0], r += buf[1];
        nuked_clocks_owed = 0, nuked_l = nuked_r = 0;
        o[4] = l, o[5] = r;
    }
};

static void write_wav(const std::string &path, const std::vector<int32_t> &x, int ch, int stride, double gain)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) { perror(path.c_str()); exit(1); }
    uint32_t n = (uint32_t)(x.size() / stride), rate = OPN2_RATE_NTSC, bytes = n * 4;
    uint32_t hdr[] = {0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16, 0x00020001, rate, rate * 4, 0x00100004,
                      0x61746164, bytes};
    fwrite(hdr, 4, 11, f);
    for (uint32_t i = 0; i < n; i++)
        for (int c = 0; c < 2; c++) {
            double v = x[i * stride + ch + c] * gain;
            int16_t s = (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
            fwrite(&s, 2, 1, f);
        }
    fclose(f);
}

// Nuked against ours: the best gain and lag (Nuked's writes land a little later), then the residual in dB
static double match_db(const std::vector<int32_t> &x, int a, int b, int *lag_out)
{
    size_t n = x.size() / 6;
    double best = 1e9;
    int best_lag = 0;
    for (int lag = -64; lag <= 64; lag++) {
        double sab = 0, sbb = 0, saa = 0;
        for (size_t i = 128; i + 128 < n; i++)
            for (int c = 0; c < 2; c++) {
                double va = x[i * 6 + a + c], vb = x[(i + lag) * 6 + b + c];
                sab += va * vb, sbb += vb * vb, saa += va * va;
            }
        if (saa == 0 || sbb == 0) continue;
        double g = sab / sbb, err = 0;
        for (size_t i = 128; i + 128 < n; i++)
            for (int c = 0; c < 2; c++) {
                double d = x[i * 6 + a + c] - g * x[(i + lag) * 6 + b + c];
                err += d * d;
            }
        double db = 10 * log10(err / saa + 1e-12);
        if (db < best) best = db, best_lag = lag;
    }
    *lag_out = best_lag;
    return best;
}

// The same comparison on loudness (RMS per 256 samples, DC removed): blind to phase, so to when exactly a write lands
static double envelope_db(const std::vector<int32_t> &x, int a, int b)
{
    size_t n = x.size() / 6, blk = 256;
    std::vector<double> ea, eb;
    for (size_t s = 0; s + blk <= n; s += blk) {
        double ma[2] = {0, 0}, mb[2] = {0, 0}, va = 0, vb = 0;
        for (size_t i = s; i < s + blk; i++)
            for (int c = 0; c < 2; c++) ma[c] += x[i * 6 + a + c], mb[c] += x[i * 6 + b + c];
        for (size_t i = s; i < s + blk; i++)
            for (int c = 0; c < 2; c++) {
                double da = x[i * 6 + a + c] - ma[c] / blk, d2 = x[i * 6 + b + c] - mb[c] / blk;
                va += da * da, vb += d2 * d2;
            }
        ea.push_back(sqrt(va / blk)), eb.push_back(sqrt(vb / blk));
    }
    double sab = 0, sbb = 0, saa = 0, err = 0;
    for (size_t i = 0; i < ea.size(); i++) sab += ea[i] * eb[i], sbb += eb[i] * eb[i], saa += ea[i] * ea[i];
    if (saa == 0 || sbb == 0) return 0;
    for (size_t i = 0; i < ea.size(); i++) err += (ea[i] - sab / sbb * eb[i]) * (ea[i] - sab / sbb * eb[i]);
    return 10 * log10(err / saa + 1e-12);
}

static uint32_t rng = 1;
static uint32_t rnd(uint32_t n) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) % n; }

// ---- scenarios

static void patch(std::vector<Ev> &ev, uint32_t at, int ch, int alg, int fb, const int (&op)[4][7])
{
    int part = ch / 3, c = ch % 3;
    static const int SLOT[4] = {0, 8, 4, 12};               // O1..O4 register slots
    ev.push_back({at, (uint8_t)part, (uint8_t)(0xB0 + c), (uint8_t)(fb << 3 | alg)});
    for (int o = 0; o < 4; o++) {                            // DT/MUL, TL, KS/AR, AM/D1R, D2R, SL/RR, SSG
        static const int REG[7] = {0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90};
        for (int k = 0; k < 7; k++)
            ev.push_back({at, (uint8_t)part, (uint8_t)(REG[k] + SLOT[o] + c), (uint8_t)op[o][k]});
    }
}
static void note(std::vector<Ev> &ev, uint32_t at, int ch, int block, int fnum, uint32_t len, int ops = 0xF)
{
    int part = ch / 3, c = ch % 3, code = c | part << 2;
    ev.push_back({at, (uint8_t)part, (uint8_t)(0xA4 + c), (uint8_t)(block << 3 | fnum >> 8)});
    ev.push_back({at, (uint8_t)part, (uint8_t)(0xA0 + c), (uint8_t)(fnum & 0xFF)});
    ev.push_back({at, 0, 0x28, (uint8_t)(ops << 4 | code)});
    ev.push_back({at + len, 0, 0x28, (uint8_t)code});
}

static std::vector<Scenario> scenarios()
{
    std::vector<Scenario> s;
    const uint32_t SEC = OPN2_RATE_NTSC;
    {   // every algorithm, one after another, on channel 1, with feedback
        Scenario sc{"algorithms", 8 * SEC / 2 + SEC / 2, {}};
        for (int a = 0; a < 8; a++) {
            const int op[4][7] = {{0x71, 0x23, 0x1F, 0x05, 0x02, 0x27, 0}, {0x0D, 0x2D, 0x99, 0x05, 0x02, 0x27, 0},
                                  {0x33, 0x26, 0x5F, 0x05, 0x02, 0x27, 0}, {0x01, 0x10, 0x1F, 0x07, 0x02, 0x2A, 0}};
            patch(sc.ev, a * SEC / 2, 0, a, a % 8, op);
            note(sc.ev, a * SEC / 2 + 10, 0, 4, 644, SEC / 3);
        }
        s.push_back(sc);
    }
    {   // six channels, a chord, slow attacks and releases, key scaling, detune
        Scenario sc{"chord", 3 * SEC, {}};
        const int fn[6] = {644, 810, 963, 644, 810, 963};
        for (int ch = 0; ch < 6; ch++) {
            const int op[4][7] = {{0x12 + ch, 0x2A, 0x4C, 0x0A, 0x04, 0x36, 0}, {0x51, 0x30, 0x8E, 0x08, 0x03, 0x45, 0},
                                  {0x24, 0x2C, 0xD0, 0x09, 0x02, 0x36, 0}, {0x02, 0x08, 0x0C + ch, 0x06, 0x01, 0x18, 0}};
            patch(sc.ev, 0, ch, ch % 8, 6, op);
            note(sc.ev, 100 + ch * 300, ch, 3 + ch / 3, fn[ch], 2 * SEC);
        }
        s.push_back(sc);
    }
    {   // LFO: AM and PM on every rate and sensitivity
        Scenario sc{"lfo", 8 * SEC / 2, {}};
        for (int r = 0; r < 8; r++) {
            uint32_t t = r * SEC / 2;
            const int op[4][7] = {{0x01, 0x30, 0x1F, 0x80, 0, 0x0F, 0}, {0x01, 0x30, 0x1F, 0x80, 0, 0x0F, 0},
                                  {0x02, 0x28, 0x1F, 0x80, 0, 0x0F, 0}, {0x01, 0x10, 0x1F, 0x80, 0, 0x0F, 0}};
            sc.ev.push_back({t, 0, 0x22, (uint8_t)(0x08 | r)});
            patch(sc.ev, t, 1, 4, 3, op);
            sc.ev.push_back({t, 0, 0xB5, (uint8_t)(0xC0 | (r & 3) << 4 | r)});
            note(sc.ev, t + 10, 1, 4, 700, SEC / 3);
        }
        s.push_back(sc);
    }
    {   // SSG-EG: the eight modes
        Scenario sc{"ssg-eg", 8 * SEC / 2, {}};
        for (int m = 0; m < 8; m++) {
            uint32_t t = m * SEC / 2;
            const int op[4][7] = {{0x01, 0x7F, 0x1F, 0, 0, 0x0F, 0}, {0x01, 0x7F, 0x1F, 0, 0, 0x0F, 0},
                                  {0x01, 0x7F, 0x1F, 0, 0, 0x0F, 0}, {0x01, 0x00, 0x1F, 0x0C, 0x08, 0x5F, 0x08 | m}};
            patch(sc.ev, t, 2, 7, 0, op);
            note(sc.ev, t + 10, 2, 4, 1000, SEC / 3);
        }
        s.push_back(sc);
    }
    {   // channel 3 special mode: four frequencies on one channel
        Scenario sc{"ch3-special", SEC, {}};
        const int op[4][7] = {{0x01, 0x20, 0x1F, 0, 0, 0x0F, 0}, {0x01, 0x20, 0x1F, 0, 0, 0x0F, 0},
                              {0x01, 0x20, 0x1F, 0, 0, 0x0F, 0}, {0x01, 0x20, 0x1F, 0, 0, 0x0F, 0}};
        patch(sc.ev, 0, 2, 7, 0, op);
        sc.ev.push_back({0, 0, 0x27, 0x40});
        const int f[3] = {644, 810, 963};
        for (int k = 0; k < 3; k++) {
            sc.ev.push_back({0, 0, (uint8_t)(0xAC + k), (uint8_t)(4 << 3 | f[k] >> 8)});
            sc.ev.push_back({0, 0, (uint8_t)(0xA8 + k), (uint8_t)(f[k] & 0xFF)});
        }
        note(sc.ev, 50, 2, 3, 644, SEC / 2);
        s.push_back(sc);
    }
    {   // the DAC: a sawtooth on channel 6, with an FM note on channel 1 beside it
        Scenario sc{"dac", SEC, {}};
        const int op[4][7] = {{0x01, 0x20, 0x1F, 0, 0, 0x0F, 0}, {0x01, 0x20, 0x1F, 0, 0, 0x0F, 0},
                              {0x01, 0x20, 0x1F, 0, 0, 0x0F, 0}, {0x01, 0x20, 0x1F, 0, 0, 0x0F, 0}};
        patch(sc.ev, 0, 0, 4, 2, op);
        note(sc.ev, 10, 0, 4, 644, SEC / 2);
        sc.ev.push_back({100, 0, 0x2B, 0x80});
        for (uint32_t t = 200; t < SEC - 200; t += 6)
            sc.ev.push_back({t, 0, 0x2A, (uint8_t)((t / 6) * 7)});
        sc.ev.push_back({SEC - 100, 0, 0x2B, 0x00});
        s.push_back(sc);
    }
    for (int seed = 1; seed <= 8; seed++) {      // random patches, notes and parameter changes
        Scenario sc{"random-" + std::to_string(seed), 2 * SEC, {}};
        rng = (uint32_t)seed * 7919u;
        for (uint32_t t = 0; t < sc.samples - SEC / 4; t += 1 + rnd(400)) {
            int ch = rnd(6), part = ch / 3, c = ch % 3, slot = rnd(4) * 4;
            switch (rnd(10)) {
            case 0: case 1: note(sc.ev, t, ch, rnd(8), rnd(2048), 1 + rnd(SEC / 2), 1 + rnd(15)); break;
            case 2: sc.ev.push_back({t, 0, 0x22, (uint8_t)rnd(16)}); break;
            case 3: sc.ev.push_back({t, (uint8_t)part, (uint8_t)(0xB0 + c), (uint8_t)rnd(64)}); break;
            case 4: sc.ev.push_back({t, (uint8_t)part, (uint8_t)(0xB4 + c), (uint8_t)(0x40 + rnd(192))}); break;
            case 5: sc.ev.push_back({t, (uint8_t)part, (uint8_t)(0x40 + slot + c), (uint8_t)rnd(0x50)}); break;
            case 6: sc.ev.push_back({t, (uint8_t)part, (uint8_t)(0x90 + slot + c), (uint8_t)(rnd(4) ? 0 : 8 | rnd(8))}); break;
            default: {
                static const int REG[6] = {0x30, 0x50, 0x60, 0x70, 0x80, 0x50};
                sc.ev.push_back({t, (uint8_t)part, (uint8_t)(REG[rnd(6)] + slot + c), (uint8_t)rnd(256)});
            }
            }
        }
        s.push_back(sc);
    }
    return s;
}

// As the firmware runs it: writes only between blocks, opn2_render() a block at a time. ymfm gets the same
// writes at the same samples, so the two must still agree exactly.
static const uint32_t BLOCK = 32;
static bool blocks_match(const std::vector<Ev> &ev, uint32_t samples)
{
    ymfm::ymfm_interface intf;
    ymfm::ym2612 ref{intf};
    static opn2_t ours;
    ref.reset();
    opn2_reset(&ours);
    std::vector<int32_t> a(2 * BLOCK);
    size_t next = 0;
    for (uint32_t i = 0; i < samples; i += BLOCK) {
        for (; next < ev.size() && ev[next].at < i + BLOCK; next++) {   // this block's writes, at its start
            ref.write(ev[next].part * 2, ev[next].reg), ref.write(ev[next].part * 2 + 1, ev[next].val);
            opn2_write_reg(&ours, ev[next].part, ev[next].reg, ev[next].val);
        }
        opn2_render(&ours, a.data(), BLOCK);
        for (uint32_t j = 0; j < BLOCK; j++) {
            ymfm::ym2612::output_data od;
            ref.generate(&od, 1);
            if (od.data[0] != a[2 * j] || od.data[1] != a[2 * j + 1]) return false;
        }
    }
    return true;
}

int main(int argc, char **argv)
{
    std::string out = argc > 1 ? argv[1] : "build/host/cmp";
    int failed = 0;
    printf("%-14s %-22s %s\n", "scenario", "ours vs ymfm", "ours vs Nuked: waveform residual, loudness residual");
    for (auto &sc : scenarios()) {
        Cores *k = new Cores;
        std::vector<int32_t> x((size_t)sc.samples * 6);
        std::vector<Ev> q;
        size_t next = 0, qi = 0;
        // a stable order: by due sample, then as written
        std::vector<Ev> ev = sc.ev;
        std::stable_sort(ev.begin(), ev.end(), [](const Ev &a, const Ev &b) { return a.at < b.at; });
        for (uint32_t i = 0; i < sc.samples; i++) {
            while (next < ev.size() && ev[next].at <= i) q.push_back(ev[next++]);
            if (qi < q.size()) k->write(q[qi++]);
            k->sample(&x[(size_t)i * 6]);
        }
        uint32_t diffs = 0, first = 0;
        int32_t maxd = 0;
        for (uint32_t i = 0; i < sc.samples; i++)
            for (int c = 0; c < 2; c++) {
                int32_t d = abs(x[i * 6 + c] - x[i * 6 + 2 + c]);
                if (d) { if (!diffs) first = i; diffs++; if (d > maxd) maxd = d; }
            }
        int lag;
        double db = match_db(x, 2, 4, &lag);
        char a[64];
        if (diffs) snprintf(a, sizeof a, "FAIL %u differ (max %d, first at %u)", diffs, maxd, first), failed++;
        else snprintf(a, sizeof a, "identical");
        printf("%-14s %-22s %6.1f dB (lag %3d)  %6.1f dB\n", sc.name.c_str(), a, db, lag, envelope_db(x, 2, 4));
        write_wav(out + "/" + sc.name + "-ours.wav", x, 2, 6, 1.0);
        write_wav(out + "/" + sc.name + "-nuked.wav", x, 4, 6, 8.0);
        delete k;
        if (!blocks_match(ev, sc.samples)) {
            printf("%-14s FAIL in blocks of %u\n", sc.name.c_str(), BLOCK);
            failed++;
        }
    }
    printf(failed ? "%d scenario(s) differ from ymfm\n" : "all identical to ymfm\n", failed);
    return failed ? 1 : 0;
}
