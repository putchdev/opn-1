# Licensing

OPN-1 is free software under the GNU General Public License, version 3 only (`GPL-3.0-only`, full text in
`LICENSE`). You may use, study, change and share it. If you give someone a firmware built from it, give them its
complete source under the same licence too.

## Who made what

| | |
| --- | --- |
| The platform: `firmware/hal/`, `firmware/loader/`, `crt0.S`, `app.ld`, `firmware/src/` (`usb.c`, `midi_uart.c`, `ota.c`, `recovery.c`, `lcd.c`, `gfx.c`, `libc.c`, `bootguard.h`, `uac_fir.h`), `tools/` | Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments ([Felucca](https://github.com/hugelton/Felucca)), with the changes of [SLOOP](https://github.com/isod89/sloop-fm1) (isod89) |
| The wiring `firmware/src/platform.c`, the host mode `tests/host.c`, `tests/run_host.sh`, `docs/API.md`, `docs/RULES.md` | zvenson ([Dinghy](https://github.com/zvenson/dinghy)), with OPN-1's changes |
| The app `firmware/src/opn1.c`, `vgm.c`, `vgm.h`, the page `web/index.html`, the tests in `tests/` not listed above, the other docs | The OPN-1 authors |

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Terminus font 8x16 (ter-u16n), rendered into `firmware/gen/felucca_font.h` | SIL OFL 1.1 | `assets/fonts/Terminus-LICENSE.txt` |
| `firmware/src/opn2.c`, `opn2.h`: the YM2612 core, a C port of ymfm by Aaron Giles | BSD-3-Clause | the notice in `opn2.c` |
| `firmware/src/psg.c`, `psg.h`: the Sega PSG, a C port of MAME's sn76496.cpp (Nicola Salmoria and others) | BSD-3-Clause | the notice in `psg.c` |
| `tests/ref/mame_segapsg.h`: MAME's Sega PSG code with the device framework replaced (tests only) | BSD-3-Clause | the notice in the file |
| ymfm by Aaron Giles, unmodified, commit 81aec25, in `tests/ref/ymfm/` (tests only, not in the firmware) | BSD-3-Clause | `tests/ref/ymfm/LICENSE` |
| Nuked-OPN2 (YM3438/YM2612 emulator) by Nuke.YKT, unmodified, commit 335747d, in `tests/ref/nuked-opn2/` (tests only, not in the firmware) | LGPL-2.1-or-later | `tests/ref/nuked-opn2/LICENSE` |
| JieLi AC79 SDK: `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` are read from your SDK checkout at build time and placed in the package; no SDK files are in this tree | Apache-2.0 | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |

No game music or game data is included. The page reads the VGM files you give it, in your browser.

## No warranty

OPN-1 comes with no warranty, as sections 15 and 16 of the GPL set out. Installing it on an FM-1 is done at the
owner's own risk, and its authors accept no responsibility for damaged or unusable devices. See the disclaimer in
`README.md`.

## Trademarks

Sega, Mega Drive and Genesis are trademarks of SEGA Corporation. "Felucca" and "Hügelton Instruments" are names of
Hügelton Instruments; "M-VAVE" and "FM-1" belong to their owners. OPN-1 is not made or endorsed by any of them.
