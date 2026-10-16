#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""A host script (tests/host.c) that sends a VGM the way web/index.html does: the PCM bank, then the
commands (loops unrolled), in chunks the FM-1 confirms; then plays it.
  tests/upload_script.py SONG.vgm [--drop K] > script.txt; tests/run_host.sh script.txt
--drop K loses one message of bank chunk K (256 bytes each), then sends again from K, as the page does when the
FM-1's held count (BF 101 102 103) stops at K's start. Expect: BF 115 0 (bank ok), BF 116 1 (playing), BF 119 n."""
import struct
import sys
import zlib

def split(b):
    """as vgm_split in tests/vgm_compare.cpp: (bank image, commands, loop offset or None)"""
    u32 = lambda a: struct.unpack_from("<I", b, a)[0]
    ver = u32(8)
    start = 0x34 + u32(0x34) if ver >= 0x150 and u32(0x34) else 0x40
    end, loop = min(len(b), u32(4) + 4), (0x1C + u32(0x1C) if u32(0x1C) else 0)
    def clen(p):
        c = b[p]
        if c == 0x67: return 7 + u32(p + 3)
        if 0x30 <= c <= 0x3F or c in (0x4F, 0x50, 0x94): return 2
        if 0x40 <= c <= 0x5F or c == 0x61 or 0xA0 <= c <= 0xBF: return 3
        if c in (0x62, 0x63, 0x66) or 0x70 <= c <= 0x8F: return 1
        if c == 0x68: return 12
        if c in (0x90, 0x91, 0x95) or c >= 0xE0: return 5
        if c == 0x92: return 6
        if c == 0x93: return 11
        if 0xC0 <= c <= 0xDF: return 4
        raise SystemExit(f"unknown command {c:#x}")
    lens, bank, cmds, loop_at, p = [], b"", bytearray(), None, start
    while p < end:
        if p == loop: loop_at = len(cmds)
        k = clen(p)
        if b[p] == 0x67:
            if b[p + 2] == 0: lens.append(k - 7); bank += b[p + 7:p + k]
        else:
            cmds += b[p:p + k]
        if b[p] == 0x66: break
        p += k
    return struct.pack("<I", len(lens)) + b"".join(struct.pack("<I", n) for n in lens) + bank, bytes(cmds), loop_at

def crc16(b, c=0xFFFF):
    for x in b:
        c ^= x << 8
        for _ in range(8): c = (c << 1 ^ 0x1021 if c & 0x8000 else c << 1) & 0xFFFF
    return c

if __name__ == "__main__":
    song = open(sys.argv[1], "rb").read()
    drop = int(sys.argv[sys.argv.index("--drop") + 1]) if "--drop" in sys.argv else -1
    image, cmds, loop_at = split(song)
    stream = cmds if loop_at is None else cmds[:-1]       # looping: no final 0x66, then the loop section again
    while loop_at is not None and len(stream) < 120 * 1024:
        stream += cmds[loop_at:-1]
    stream = stream[:120 * 1024]

    lines, t = [], 100
    def at(ms, st, d1, d2): lines.append(f"{ms} midi 0x{st:02X} {d1} {d2}")
    def chunk(off, data, lose=False):
        global t
        STATUS = [0xAE, 0xEE, 0xAF, 0xEF]               # the top two bits of each 16
        t += 1
        at(t, 0xBF, 0x6A, off & 127); at(t, 0xBF, 0x6B, off >> 7 & 127); at(t, 0xBF, 0x6C, off >> 14 & 127)
        padded = data + (b"\0" if len(data) % 2 else b"")
        for i in range(0, len(padded), 2):
            w = padded[i] | padded[i + 1] << 8
            if lose and i == 64: continue               # one message lost on the way
            at(t, STATUS[w >> 14], w & 0x7F, (w >> 7) & 0x7F)
        c = crc16(padded, crc16(bytes([off & 255, off >> 8 & 255, off >> 16 & 255])))   # (where it goes, then the bytes)
        at(t, 0xBF, 0x6D, c & 127); at(t, 0xBF, 0x6E, c >> 7 & 127); at(t, 0xBF, 0x6F, c >> 14)
    def chunks(data, lose_at=-1):
        parts = [data[i:i + 256] for i in range(0, len(data), 256)]
        for n, part in enumerate(parts):
            chunk(256 * n, part, n == lose_at)
            if lose_at >= 0 and n == lose_at + 2:       # it holds up to lose_at's start: send on from there
                for m in range(lose_at, n + 1): chunk(256 * m, parts[m])

    crc, n = zlib.crc32(image) & 0xFFFFFFFF, len(image)
    at(t, 0xBF, 0x70, 0)
    for i in range(5): at(t, 0xBF, 0x78 + i, (crc >> (7 * i)) & 0x7F)
    for i in range(3): at(t, 0xBF, 0x7D + i, (n >> (7 * i)) & 0x7F)
    chunks(image, drop)
    t += 5
    at(t, 0xBF, 0x72, 0)
    t += 5
    at(t, 0xBF, 0x60, 0)
    chunks(stream)
    t += 5
    at(t, 0xBF, 0x75, 1)
    print("\n".join(lines))
    print(f"{t + 4000} end")
