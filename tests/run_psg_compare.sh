#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Check firmware/src/psg.c against MAME's Sega PSG code, tick for tick.
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host/cmp
B=build/host/cmp
cc -std=c99 -O2 -Wall -c firmware/src/psg.c -o $B/psg.o
c++ -std=c++14 -O2 -Wall -Itests/ref -Ifirmware/src tests/psg_compare.cpp $B/psg.o -o $B/psg_compare
$B/psg_compare
