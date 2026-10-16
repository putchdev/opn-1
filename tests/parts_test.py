#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The parts engine (firmware/src/opn1.c) in the host build: voices shared between parts, stealing, layers on one
MIDI channel, delayed and release triggers, GenMDM mode, and the setup read back.
  tests/parts_test.py"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import library_test as L  # noqa: E402

PATCH = L.VALUES  # (any whole patch: the voices are what is tested, not the sound)


def voices(replies, k):
    """the k-th voice report: [(owner, note, on)] for the six voices"""
    rep = [v for d1, v in replies if d1 == 0x4B]
    block = rep[24 * k:24 * k + 24]
    return [tuple(block[4 * i + 1:4 * i + 4]) for i in range(6)]


def sounding(vs):
    return sorted((o, n) for o, n, on in vs if on)


def main():
    m, failed = [], 0

    def cc(d1, d2):
        m.append((d1, d2))

    def note(ch, n, on=True):
        m.append(("raw", (0x90 if on else 0x80) | ch, n, 100 if on else 0))

    def wait(ms):
        m.append(("wait", ms))

    def report():
        cc(0x4B, 0)
        wait(5)

    m += L.patch_msgs(0x5B, 0, "A", PATCH) + L.patch_msgs(0x5B, 1, "B", PATCH)
    for p in range(6):
        cc(0x20, p); cc(0x25, 0)
    wait(20)
    for n in (60, 64, 67):                      # 0: a chord on part 1
        note(0, n)
    report()
    for n in (70, 72, 74, 76):                  # 1: seven notes on six voices: the oldest stolen
        note(0, n)
    report()
    for n in (60, 64, 67, 70, 72, 74, 76):
        note(0, n, False)
    wait(20)
    cc(0x20, 1); cc(0x21, 0); cc(0x25, 1)       # part 2 onto MIDI channel 1: a layer
    note(0, 50)
    report()                                    # 2
    note(0, 50, False)
    cc(0x20, 1); cc(0x23, 1); cc(0x24, 10)      # part 2 delayed by 100 ms
    note(0, 52)
    report()                                    # 3: part 1 only, yet
    wait(150)
    report()                                    # 4: both
    note(0, 52, False)
    wait(10)
    cc(0x20, 1); cc(0x23, 2); cc(0x24, 20)      # part 2 at note-off, held 200 ms
    note(0, 55)
    report()                                    # 5: part 1 only
    note(0, 55, False)
    wait(5)
    report()                                    # 6: part 2's tail only
    wait(250)
    report()                                    # 7: nothing
    cc(0x26, 1)                                 # GenMDM: part 1 is FM 1, one note at a time
    note(0, 60); note(0, 62)
    report()                                    # 8
    cc(0x27, 1)                                 # part 2's setup read back
    wait(10)
    m.append(("raw", 0xC2, 1, 0))               # a program change on part 3's channel: the FM-1 says so
    wait(10)
    cc(0x26, 0); cc(0x20, 1); cc(0x21, 16)      # back to one synth: part 1 on every channel by default
    note(4, 40)
    note(6, 41)                                 # (channel 7: the PSG's, not All's)
    report()                                    # 9

    script, t = [], 100
    for e in m:
        if e[0] == "wait":
            t += e[1]
            continue
        if e[0] == "raw":
            script.append(f"{t} midi {e[1]} {e[2]} {e[3]}")
        else:
            script.append(f"{t} midi 0xBF {e[0]} {e[1]}")
        t += 1
    script.append(f"{t + 100} end")
    r = L.run_script(script)

    def check(what, ok):
        nonlocal failed
        print(("Passed  " if ok else "FAILED  ") + what)
        failed += not ok

    check("a chord takes three voices", sounding(voices(r, 0)) == [(1, 60), (1, 64), (1, 67)])
    check("the seventh note steals the oldest", sounding(voices(r, 1)) == [(1, n) for n in (64, 67, 70, 72, 74, 76)])
    check("two parts on one channel layer", sounding(voices(r, 2)) == [(1, 50), (2, 50)])
    check("a delayed part waits", sounding(voices(r, 3)) == [(1, 52)])
    check("then joins", sounding(voices(r, 4)) == [(1, 52), (2, 52)])
    check("a release part is silent while held", sounding(voices(r, 5)) == [(1, 55)])
    check("and sounds at note-off", sounding(voices(r, 6)) == [(2, 55)])
    check("for its time", sounding(voices(r, 7)) == [])
    v8 = voices(r, 8)
    check("GenMDM: one note on FM 1", sounding(v8) == [(1, 62)] and v8[0][:2] == (1, 62))
    tail = r[r.index((0x20, 1), r.index((0x2F, 0x7F))):]
    check("part 2's setup reads back", tail[:8] == [(0x20, 1), (0x21, 0), (0x22, 127), (0x23, 2), (0x24, 20), (0x25, 1), (0x26, 1), (0x2F, 1)])
    check("a slot change is reported", (0x2A, 1) in tail)
    check("by default part 1 plays All but the PSG's channels", sounding(voices(r, 9)) == [(1, 40)])
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
