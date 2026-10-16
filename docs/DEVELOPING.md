# Developing OPN-1

## Set up once

You need Linux on x86-64 (or macOS with Docker), Python 3, the JieLi toolchain and three files from the JieLi AC79
SDK:

```sh
pip3 install Pillow mido python-rtmidi
tools/get_toolchain.sh                      # JieLi's clang for the chip, to ~/.jieli/toolchain
git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
    https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
```

On macOS the build runs the toolchain in a `linux/amd64` Docker container by itself (have Docker running).
`JIELI_TOOLCHAIN` and `AC79_SDK` point elsewhere if your copies live somewhere else.

## Build and install

```sh
./build.sh                                     # -> build/opn1.fwsc, identity FM-1_950
./build.sh --release 0.1                       # -> build/opn1-0.1.fwsc, identity FM-1_951, "0.1" on the screen
python3 tools/fm1_install.py build/opn1.fwsc   # over USB; asks before it writes
```

The build refuses what would brick or break an FM-1 firmware: a 64-bit division, a call into the chip ROM, too
much RAM, a register touched outside `firmware/hal/`, a call from RAM code into flash. [RULES.md](RULES.md) says
why each one matters.

## The host simulator

`tests/run_host.sh` builds the firmware for your computer and runs it against a script of key presses, knob turns
and MIDI messages. Out come the sound (`build/host/opn1.wav`) and the screen (`build/host/opn1-<ms>.png`).

```sh
tests/run_host.sh                       # a short built-in script
tests/run_host.sh my_script.txt         # your own: see the top of tests/host.c for the format
```

To try a song on the simulator, turn a VGM into a script of the page's transfer messages:

```sh
python3 tests/upload_script.py SONG.vgm > song.txt && tests/run_host.sh song.txt
```

## Tests

| Command | Checks |
| --- | --- |
| `tests/run_opn2_compare.sh` | `opn2.c` against ymfm (must be identical) and Nuked-OPN2 (reported) |
| `tests/run_psg_compare.sh` | `psg.c` against MAME's Sega PSG, tick for tick |
| `tests/run_vgm_compare.sh` | `vgm.c` against a sample-at-a-time reference, from a whole file and from a stream |
| `tests/web_link_test.py SONG.vgm 1` | The page streaming a song to a simulated FM-1 that loses 1% of messages (headless Chrome) |
| `tests/web_link_test.py SONG.vgm 0 sounds` | The page sending a game's sounds and saving them |

The page tests need `google-chrome` and a VGM of your own; no game music is kept in this repository.

## Layout

```
firmware/src/opn1.c       the app: voices, panel, knob pages, monitor, transfers, the flash store
firmware/src/opn2.c, .h   the YM2612 (ymfm port, BSD-3-Clause)
firmware/src/psg.c, .h    the SN76489 PSG (MAME port, BSD-3-Clause)
firmware/src/vgm.c, .h    the VGM player, from a file or a streamed ring
firmware/src/platform.c   the platform: boot, USB, updates, audio interrupt, panel (from Dinghy / Felucca / SLOOP)
firmware/hal/, loader/    hardware access and the update loader (Felucca / SLOOP)
web/index.html            the companion page: one file, no build step
tests/                    the host simulator, the comparison tests and their references (tests/ref/)
tools/                    build, packaging, installer, toolchain fetch
```

The app's main loop never touches the chips: it queues register writes, which the audio interrupt applies before
it renders. While a song plays, the chips belong to the song and the panel's writes are dropped; the panel's
sounds are written back when it ends.

## Releasing

1. `./build.sh --release X.Y` (one digit each; the identity becomes `FM-1_95Y`).
2. A GitHub release tagged `vX.Y` with `build/opn1-X.Y.fwsc` attached, and what changed in its notes.
3. Attach `LICENSING.md` and `LICENSE` to the release as well. The package contains BSD-3-Clause code (the ymfm and
   MAME ports) and three Apache-2.0 files from JieLi's SDK, whose licences ask for their notices to go with the
   binary.
