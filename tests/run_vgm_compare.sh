#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Check firmware/src/vgm.c (with opn2.c and psg.c) against a one-sample-at-a-time reference on ymfm and MAME's PSG.
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host/cmp
B=build/host/cmp
for f in opn2 psg vgm; do cc -std=c99 -O2 -Wall -Ifirmware/src -c firmware/src/$f.c -o $B/$f.o; done
for f in opn ssg adpcm; do
    [ $B/ymfm_$f.o -nt tests/ref/ymfm/ymfm_$f.cpp ] || c++ -std=c++14 -O2 -c tests/ref/ymfm/ymfm_$f.cpp -o $B/ymfm_$f.o
done
c++ -std=c++14 -O2 -Wall -Wno-sign-compare -Itests/ref -Itests/ref/ymfm -Ifirmware/src tests/vgm_compare.cpp \
    $B/opn2.o $B/psg.o $B/vgm.o $B/ymfm_opn.o $B/ymfm_ssg.o $B/ymfm_adpcm.o -o $B/vgm_compare
$B/vgm_compare
