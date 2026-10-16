#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The preset library (firmware/src/opn1.c) in the host build: patches written to slots, read back, kept in flash
across a reload, emptied, and 1.0's page's sound set landing in bank 1.
  tests/library_test.py"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "build/host"


def patch_msgs(begin, target, name, values, sum_fix=0):
    m = [(begin, target)] + [(0x59, ord(c)) for c in name]
    for v in values:
        m += [(0x52, v & 0x7F), (0x53, v >> 7)]
    s = (sum(map(ord, name)) + sum(values) + sum_fix) & 0x3FFF
    return m + [(0x54, s & 0x7F), (0x55, s >> 7)]


# B0, B4 (both sides), each operator's 30-90, then lfo, oct and vibrato: values with a top bit among them
VALUES = [0x2A, 0xC5] + sum(([0x10 | k, 30 + k, 0x40 | 31, 0x80 | 12, 4 + k, 0x28, 0] for k in range(4)), []) + \
    [0x80 | 0x0B, 66, 20, 50, 10]


def run(msgs):
    script, t = [], 100
    for d1, d2 in msgs:
        if d1 == "wait":
            t += d2
            continue
        script.append(f"{t} midi 0xBF {d1} {d2}")
        t += 1
    script.append(f"{t + 200} end")
    return run_script(script)


def run_script(script):
    """build the host app, run the script lines; the FM-1's channel 16 CCs, in order"""
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / "library_script.txt"
    path.write_text("\n".join(script) + "\n")
    exe = OUT / "opn1"
    subprocess.run(["cc", "-O2", "-Wall", "-Wno-unused-function", "-DDINGHY_HOST", '-DAPP_VER="test"',
                    "-Ifirmware/src", "-Ifirmware/gen", "-o", str(exe), "firmware/src/opn1.c",
                    "firmware/src/opn2.c", "firmware/src/psg.c", "firmware/src/vgm.c"], cwd=ROOT, check=True)
    out = subprocess.run([str(exe), str(OUT / "library"), str(path)], cwd=ROOT, check=True,
                         capture_output=True, text=True).stdout
    return [(int(a), int(b)) for st, a, b in re.findall(r"MIDI out (BF) (\d+) (\d+)", out)]


def read_back(replies, slot):
    """the slot's dump from the replies: (name, values) or None for empty"""
    for i, (d1, d2) in enumerate(replies):
        if d1 == 0x36 and d2 == slot:
            return None
        if d1 == 0x30 and d2 == slot:
            name, vals, lo, j = "", [], 0, i + 1
            while replies[j][0] != 0x34:
                d, v = replies[j]
                if d == 0x31: name += chr(v)
                elif d == 0x32: lo = v
                elif d == 0x33: vals.append(lo | v << 7)
                j += 1
            s = replies[j][1] | replies[j + 1][1] << 7
            assert s == (sum(map(ord, name)) + sum(vals)) & 0x3FFF, "dump sum"
            return name, vals
    raise AssertionError(f"no dump of slot {slot}")


def main():
    failed = 0

    def check(what, ok):
        nonlocal failed
        print(("Passed  " if ok else "FAILED  ") + what)
        failed += not ok

    msgs = patch_msgs(0x5B, 40, "TESTPAT", VALUES) + patch_msgs(0x5B, 41, "BADSUM", VALUES, 1)
    msgs += [(0x5C, 40), (0x5C, 41), (0x5C, 127)]
    msgs += [(0x4E, 0), ("wait", 1500), (0x4C, 0), ("wait", 50), (0x5C, 40)]
    msgs += [(0x5E, 40), ("wait", 5), (0x5C, 40)]
    msgs += [(0x5A, 0)] + patch_msgs(0x50, 16, "OLDPAGE", VALUES[:30]) + [(0x5A, 1), (0x5C, 0), (0x5C, 1)]
    msgs += patch_msgs(0x5B, 70, "BANK3", VALUES) + [(0x58, 2), ("wait", 5), (0x5C, 70)]
    r = run(msgs)

    check("slot 40 taken", (0x3E, 40) in r)
    check("bad sum refused", (0x3F, 41) in r and (0x3E, 41) not in r)
    first = r[:r.index((0x4D, 0))] if (0x4D, 0) in r else r
    got = read_back(first, 40)
    check("slot 40 reads back as sent", got == ("TESTPAT", VALUES))
    check("refused slot 41 is empty", read_back(first, 41) is None)
    check("slot 127 is empty", read_back(first, 127) is None)
    check("kept in flash", (0x4D, 0) in r)
    after = r[r.index(next(x for x in r if x[0] == 0x4C)):]
    check("slot 40 survives a reload", read_back(after, 40) == ("TESTPAT", VALUES))
    cleared = after[after.index((0x3D, 40)):]
    check("emptied slot reads empty", read_back(cleared, 40) is None)
    old = read_back(cleared, 0)
    check("1.0 page's patch lands in bank 1", old is not None and old[0] == "OLDPAGE" and old[1][:30] == [VALUES[0], 0xC0 | VALUES[1] & 0x37] + VALUES[2:30])
    check("1.0 page's set size empties the rest of bank 1", read_back(cleared, 1) is None)
    check("bank 3 emptied", (0x3E, 70) in r and read_back(r[r.index((0x3D, 2)):], 70) is None)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
