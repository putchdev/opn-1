// SPDX-License-Identifier: GPL-3.0-only
// Our Sega PSG (firmware/src/psg.c) against MAME's code (tests/ref/mame_segapsg.h), tick for tick, on scripted
// and random writes. MAME runs at 8 stream samples a chip update; its update lands on the 8th (index 7).
// Run tests/run_psg_compare.sh.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "mame_segapsg.h"
extern "C" {
#include "psg.h"
}

static uint32_t rng = 1;
static uint32_t rnd(uint32_t n) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) % n; }

int main()
{
    int failed = 0;
    for (int seed = 0; seed < 40; seed++) {
        mame_segapsg ref;
        psg_t ours;
        psg_reset(&ours);
        rng = (uint32_t)seed * 7919u + 1u;
        const uint32_t TICKS = 200000;
        uint32_t diffs = 0, first = 0;
        for (uint32_t t = 0; t < TICKS; t++) {
            if (seed == 0) {                    // scripted: each channel, then the noise modes
                if (t == 0) for (uint8_t b : {0x80 | 0x0E, 0x0F, 0x90, 0xA0 | 5, 0x10, 0xB2, 0xC0 | 3, 0x3F, 0xD4})
                        ref.write(b), psg_write(&ours, b);
                if (t % 20000 == 10000) {
                    uint8_t b = (uint8_t)(0xE0 | (t / 20000) % 8), v = 0xF1;
                    ref.write(b), psg_write(&ours, b), ref.write(v), psg_write(&ours, v);
                }
            } else if (rnd(300) == 0) {         // random bytes, latches and data, as a game writes them
                uint8_t b = (uint8_t)rnd(256);
                ref.write(b), psg_write(&ours, b);
            }
            int16_t m = 0;
            for (int k = 0; k < 8; k++) m = ref.sample();
            int32_t o = psg_tick(&ours);
            if (m != o) { if (!diffs) first = t; diffs++; }
        }
        if (diffs) {
            printf("seed %2d: FAIL %u ticks differ, first at %u\n", seed, diffs, first);
            failed++;
        }
    }
    printf(failed ? "%d run(s) differ from MAME\n" : "all 40 runs identical to MAME, tick for tick\n", failed);
    return failed ? 1 : 0;
}
