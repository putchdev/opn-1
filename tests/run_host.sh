#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Run the app (APP=opn1) on this computer (tests/host.c): build/host/APP.wav and
# the screens as PNG. Extra arguments: a script file (see tests/host.c).
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host
APP="${APP:-opn1}"
case "$APP" in opn1) EXTRA="firmware/src/opn2.c firmware/src/psg.c firmware/src/vgm.c" ;; *) EXTRA= ;; esac
cc -O2 -Wall -Wno-unused-function -DDINGHY_HOST -DAPP_VER="\"${APP_VER:-dev}\"" -Ifirmware/src -Ifirmware/gen -o build/host/$APP firmware/src/$APP.c $EXTRA
build/host/$APP build/host/$APP "$@"
python3 - <<'PY'
from pathlib import Path
from PIL import Image
for p in sorted(Path("build/host").glob("*-*.ppm")):
    Image.open(p).resize((480, 480), Image.NEAREST).save(p.with_suffix(".png"))
    p.unlink()
    print("screen ->", p.with_suffix(".png"))
PY
