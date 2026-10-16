#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Check firmware/src/opn2.c against ymfm (must be identical) and Nuked-OPN2 (reported). WAVs in build/host/cmp/.
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host/cmp
B=build/host/cmp
cc -std=c99 -O2 -Wall -c firmware/src/opn2.c -o $B/opn2.o
cc -O2 -c tests/ref/nuked-opn2/ym3438.c -o $B/ym3438.o
for f in opn ssg adpcm; do
    [ $B/ymfm_$f.o -nt tests/ref/ymfm/ymfm_$f.cpp ] || c++ -std=c++14 -O2 -c tests/ref/ymfm/ymfm_$f.cpp -o $B/ymfm_$f.o
done
c++ -std=c++14 -O2 -Wall -Itests/ref/ymfm -Ifirmware/src -Itests/ref/nuked-opn2 \
    tests/opn2_compare.cpp $B/opn2.o $B/ym3438.o $B/ymfm_opn.o $B/ymfm_ssg.o $B/ymfm_adpcm.o -o $B/opn2_compare
$B/opn2_compare $B
