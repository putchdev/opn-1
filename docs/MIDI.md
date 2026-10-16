# OPN-1 MIDI protocol

How to talk to the OPN-1 firmware on an M-VAVE FM-1 over USB MIDI. Everything here is taken from
`firmware/src/opn1.c` (the device side) and `web/index.html` (the reference sender). If the two disagree with
this document, the code wins.

- MIDI also comes in on the FM-1's TRS jack (31250 baud). Those messages are merged with USB MIDI and treated
  the same, so section 1 applies to both. The transfers in section 2 need replies, so they work over USB only.
- The device appears as a class compliant USB MIDI port (named `OPN-1 FM-1`; the USB rescue updater shows as `Felucca Update`).
- Channels below are written 1 to 16. In status bytes they are 0 to 15, so channel 16 CC is `0xBF`.
- SysEx is not used; it belongs to the firmware updater.
- The link can lose messages (Linux USB MIDI drops some under load). Every transfer is confirmed and every control
  message is repeated until answered. A sender must do the same.

## 1. Playing

The FM sounds are six **parts**. Each part has a sound (a library slot, as edited), a MIDI channel, a level and a
trigger. The six YM2612 FM channels are voices the parts share (five with a drum kit loaded). The MIDI mode, on the
FM-1's GLOBAL page (MIDI knob) or by CC 26 below, says how they play:

- **MULTI** (the default): each part plays as many notes as there are free voices. A new note takes a free voice,
  else the oldest note's (this part's first). Parts on the same MIDI channel play together, as a layer. By default
  part 1 is on All (channels 1 to 6 and 12 to 15, the ones the PSG, drums and transfers leave) and parts 2 to 6 are off, so the FM-1 is one synth
  playing the selected patch. Its screen and the page call this setup **Multi** once it is changed.
- **GENMDM:** part n is FM channel n, one note at a time, on channel n: GenMDM v100's layout.
- **PANEL:** any channel (but 10) plays the part shown on the FM-1, as its keys do.

A part's trigger:

- **Key:** at note-on.
- **Delay:** its time after note-on. If the note ends first, it never sounds.
- **Release:** at note-off, at the note-on's velocity, held for its time.

So one part can be a sound's attack, another its body, another its tail. In GENMDM mode every part is Key.

| MIDI channel | Plays |
|---|---|
| A part's channel | That part (MULTI; a part on All takes 1 to 6 and 12 to 15); 1 to 6, FM parts 1 to 6 (GENMDM) |
| 7 to 9 | PSG tone channels 1 to 3 |
| 10 | Drum kit if one is loaded (note 36 = sound 0, 37 = sound 1, ...), otherwise PSG noise |
| 11 | PSG noise, always |
| 15, 16 | Reserved for transfers (section 2). Pitch bend and poly aftertouch on 15 and 16 are always taken as data |

**Notes.** Note on with velocity 0 is note off.

- **FM velocity** lowers the carriers' level by `(127 - vel) >> 2` TL steps.
- **PSG velocity** sets the attenuation to `15 - (vel >> 3)`.
- **Drums:** velocity sets the level. One drum sounds at a time, through the YM2612 DAC, which takes over FM
  channel 6 as on the console.
- **Noise:** the note's pitch class picks the noise mode written to the PSG (C=0, C#=0, D=1, D#=1, E=2, F=4, F#=5,
  G=6, G#=6, A=3, A#=3, B=7). Modes 0 to 3 are periodic and 4 to 7 white; modes 3 and 7 follow tone channel 3's
  pitch.
- **Noise with a PSG instrument** (program change on 10 or 11, or the FM-1's NOISE part, which starts on the set's
  first noise instrument): the instrument fixes the noise mode, as in the game. If that mode follows tone channel 3,
  the note sets tone 3's pitch and tone 3 is silenced, which is how games pitch noise, so PSG channel 9 cannot play
  at the same time.

PSG and drum channels play only when no FM part is on that channel.

**Pitch bend** works on the FM parts and PSG tones 7 to 9. The range is 2 semitones by default and is set
by CC 81.

**Program change:**

- On a part's channel it picks library slot n for that part (0 to 127: bank 1 slot 1 is 0, bank 4 slot 32 is 127). An empty
  slot is silent.
- On channels 7 to 11 it picks PSG instrument n, or plain (no volume shape) if n is past the PSG set's size.

**CCs on a part's channel** edit that part's sound. Values are 0 to 127, scaled down by taking the top bits. Operator order is O1 to O4.

| CC | Parameter | Scaling |
|---|---|---|
| 14 | Algorithm | v >> 4 (0 to 7) |
| 15 | Feedback | v >> 4 |
| 16 to 19 | Total level O1 to O4 | 127 = loudest (TL = 127 - v) |
| 20 to 23 | Multiple | v >> 3 |
| 24 to 27 | Detune | v >> 4 (register encoding) |
| 39 to 42 | Rate scaling | v >> 5 |
| 43 to 46 | Attack rate | v >> 2 |
| 47 to 50 | Decay 1 rate | v >> 2 |
| 51 to 54 | Decay 2 rate | v >> 3 |
| 55 to 58 | Sustain level | v >> 3 |
| 59 to 62 | Release rate | v >> 3 |
| 70 to 73 | AM on for O1 to O4 | v >> 6 |
| 75 | FM depth (PMS) | v >> 4 |
| 76 | AM depth (AMS) | v >> 5 |
| 77 | Stereo | v >> 5: 1 right, 2 left, 3 both |
| 1 | LFO speed (chip wide) | v >> 4 |
| 74 | LFO on (chip wide) | v >> 6 |
| 81 | Bend range | v * 18 >> 7 semitones |
| 9 | Instrument | v >> 3 |

On PSG channels 7 to 11, only CC 81 (bend range) is used.

**PANEL mode.** The FM-1's GLO page has a MIDI setting. Set to PANEL, notes and pitch bend on every channel except
10 play whatever part is selected on the FM-1's screen, through its arpeggiator if that is on. GENMDM is the
layout above.

## 2. Transfers (channels 15 and 16)

All control messages are CCs on channel 16 (`0xBF`). The device also replies on channel 16. A CC number can mean
one thing from the host and another from the device (for example 6E), so read them by direction.

### 2.1 Chunks

Binary data (a song's PCM bank, its command stream, a drum kit) moves in chunks of at most 256 bytes.

1. **Begin:** CC 6A, 6B, 6C give the offset, in bytes from the start of this upload, mod 2^21, 7 bits each, low
   first.
2. **Data:** up to 128 messages, each carrying 16 bits (two bytes, low byte first).
   - The status byte holds the top two bits: `AE` = 0, `EE` = 1, `AF` = 2, `EF` = 3 (poly aftertouch or pitch bend
     on channel 15 or 16).
   - The two data bytes hold the low 14 bits: d1 is bits 0 to 6, d2 is bits 7 to 13.
3. **End:** CC 6D, 6E, 6F give the CRC-16 (7, 7 and 2 bits, low first). The CRC is CRC-16/CCITT-FALSE (poly
   0x1021, init 0xFFFF), computed over the three offset bytes (little endian) followed by the data bytes. CC 6F
   ends the chunk.

**What the device keeps.** It keeps a chunk only if:

- the CRC matches,
- the chunk fits, and
- its offset equals the number of bytes it already holds.

Whether it kept the chunk or not, it answers every CC 6F with its held byte count: **CC 65, 66, 67** (7 bits each,
low first, mod 2^21). The sender continues from that count.

**Odd lengths.** A data message always carries two bytes, so an odd chunk gets a pad byte. Only the final chunk of
an upload may be odd. Anywhere else, the pad would land inside the data.

Data messages outside a chunk, and chunk parts (6B to 6F) without a 6A, are dropped.

**The reference sender** (`link` in `web/index.html`):

- It keeps up to 2048 bytes in flight beyond the held count.
- It goes back to the held count when the count stops advancing, or after 150 ms of silence.
- Each go-back halves the chunk size, down to a minimum of 32 bytes. Eight good chunks in a row double it again,
  up to 256.

### 2.2 Control messages and replies

Every message in this table may arrive more than once, and the device tolerates that. The sender repeats each one
(the page waits 300 ms between tries, up to 20 tries) until the reply comes.

| Host sends (CC on ch 16) | Meaning | Device replies |
|---|---|---|
| 70 0 | Song PCM bank begins (stops any song) | 6E 70 |
| 78 to 7C | CRC-32 of the image, 7 bits each, low first | none |
| 7D to 7F | Length of the image, 7 bits each, low first | none |
| chunks | The image | 65 66 67 per chunk |
| 72 0 | Bank complete: check it | 73 status |
| 60 0 | Command stream begins (empty ring) | 6E 60 |
| chunks | The commands | 65 66 67 per chunk |
| 75 1 / 75 0 | Play / stop | 74 (1 playing, 0 not) |
| 61 0 | Drum kit begins (the kit is unloaded meanwhile) | 6E 61 |
| 78 to 7F, chunks | As for the bank | |
| 63 0 | Kit complete: check it | 73 status |
| 4E 0 | Keep the current sound set in flash | 6E 4E at once, then 4D status |

CRC-32 is the standard zlib/IEEE CRC over the image's true length (before any pad byte). The CRC and length CCs
belong to the upload begun by the last 70 or 61, so send them after it.

**Status values:**

- **CC 73 for a bank:** 0 ok, 1 length mismatch, 2 CRC mismatch, 3 too big. 10 plus a `vgm.h` error code: 11 bad
  magic, 12 size, 13 bad command, 14 too many blocks.
- **CC 73 for a kit:** 0 ok, 1 length, 2 CRC, 3 too big, 12 kit table does not fit the bytes.
- **CC 4D (save):** 0 kept, 3 too big, 4 erase failed, 5 write failed. The sound stops for under a second while the
  flash erases.

**Sent by the device on its own:**

| CC | Meaning |
|---|---|
| 77 n | Kilobytes of the command stream played so far, mod 128 (sent each time it advances) |
| 7B n | Count of dry spells (the ring ran empty), mod 128 |
| 74 n | Playing (1) or not (0). Sent whenever that changes, including when a song ends |

### 2.3 Streaming a song

The device stores no music. A VGM is split by the sender and streamed while it plays.

**The PCM bank image** holds the VGM's YM2612 PCM data blocks (command 0x67, type 0x00), in file order:

```
u32 n
u32 length[n]
bytes of block 0, block 1, ... concatenated
```

All values are little endian. The image is at most 128 KB with at most 64 blocks. Send a bank even when the song
has no PCM: `n = 0`, a 4 byte image. Play is refused until a bank has loaded.

**The command stream** is the VGM's commands from the data offset on:

- Remove the 0x67 data blocks.
- Unroll any loop: repeat the loop section yourself for as long as you want it to play.
- End with 0x66 if the song should stop.

The device plays:

- 0x50 (PSG), 0x52 and 0x53 (YM2612)
- waits 0x61, 0x62, 0x63, 0x70 to 0x7F
- 0x80 to 0x8F (DAC write plus wait), 0xE0 (seek in the bank)
- DAC streams 0x90 to 0x95

Commands for other chips are skipped. No single command may exceed 12 bytes.

**The ring** is 128 KB. Never send more than it has room for:

```
room = 131072 - margin - (bytes sent - played_kb * 1024)
```

`played_kb` is CC 77 unwrapped from mod 128. The page uses a 4096 byte margin. A chunk that does not fit is
refused (the held count does not move), which costs a go-back.

**Sequence:**

1. 70, CRC, length, the bank's chunks, 72; wait for 73 = 0.
2. 60; wait for 6E 60.
3. Send commands until about 120 KB are held. The page prefills 131072 - 8192 bytes. Starting with less risks
   running dry.
4. 75 1; wait for 74 1.
5. Keep sending as room frees up (CC 77).
6. The song is over when 74 0 arrives. Send 75 0 to stop early. The FM-1's PLAY button also stops it.

Reference implementations: `tests/upload_script.py` (bank and stream, minimal) and `playSong` in
`web/index.html` (full, with loss handling).

## 3. The library and sound sets

The FM-1 keeps:

- a library of 128 FM patches, 4 banks of 32 slots, all empty to begin with,
- up to 16 PSG instruments, and
- an optional drum kit.

They live in RAM. CC 4E writes them to flash, and they are reloaded at boot. Nothing is built in. A 1.0 store
(a 32 instrument sound set) is read into bank 1.

1.0's page sends a sound set in this order, which fills bank 1:

1. CC 5A 0
2. each FM instrument
3. CC 5A n, three times
4. CC 48 0
5. each PSG instrument
6. CC 48 n, three times
7. the kit
8. CC 4E (save)

### 3.1 FM instrument (patch frame)

| CC | Meaning |
|---|---|
| 50 t | Begin. t = 0 to 5: load straight onto that FM channel. t = 6: all six. t = 16 + k: bank 1 slot k (0 to 31) |
| 5B s | Begin, for library slot s (0 to 127). The reply is CC 3E s (taken) or 3F s (refused) |
| 59 c | Name, one ASCII character per message, up to 11 (optional) |
| 52 lo, 53 hi | One value: low 7 bits, then the top bit. Repeat for 30, 32 or 35 values |
| 54 lo, 55 hi | Checksum: the sum of the name's characters and all values, mod 2^14, in 7 bit halves |

The 30 values are register contents:

1. `B0`: feedback << 3 | algorithm
2. `B4`: pan << 6 | AMS << 4 | PMS. Pan: 3 both, 2 left, 1 right. It is used when loading onto a channel; the page
   sends 3.
3. Then for each operator O1, O2, O3, O4 (register offsets +0, +8, +4, +12), seven values: `30` dt << 4 | mul,
   `40` TL, `50` ks << 6 | ar, `60` am << 7 | d1r, `70` d2r, `80` sl << 4 | rr, `90` SSG-EG.

Optional extras follow:

- **With 32 values:**
  - LFO: `0x80 | register 0x22`. The instrument sets the chip's LFO when chosen. 0 means leave the LFO alone.
  - Octave: 64 + the FM-1 panel octave the song used (-3 to +3). 0 means leave the octave alone.
- **With 35 values,** also vibrato:
  - depth in cents (0 means none),
  - rate in 0.1 Hz,
  - delay in 10 ms steps.

The device replies **CC 56 t** (taken) or **CC 57 t** (refused: wrong count, bad checksum or bad target). On no
reply, send the whole frame again.

**CC 5A n** empties bank 1's slots from n on (1.0's set size). It gets no reply.

The library has its own messages, all on channel 16:

| CC | Meaning | Reply |
|---|---|---|
| 5E s | Empty slot s | 3D s |
| 58 b | Empty bank b (0 to 3) | 3D b |
| 5C s | Read slot s | 30 s, then 31 c for each name character, 32 lo 33 hi for each of the 35 values (as in the table above; pan is sent as both), then 34 lo 35 hi, the checksum. An empty slot replies 36 s alone |

Emptying and writing slots changes RAM only: send CC 4E to keep the library.

### 3.2 PSG instrument

| CC | Meaning |
|---|---|
| 40 k | Begin, slot k (0 to 15) |
| 41 c | Name characters, up to 11 |
| 42 lo, 43 hi | Values: length n (1 to 64), noise (`0x80 \| noise mode`, or 0 for none), then n attenuation steps (0 to 15) |
| 44 lo, 45 hi | Checksum, as for patches |

The attenuation list is a volume shape: one entry per 1/60 s from note on, the last one held. Each entry is added
to the velocity's attenuation. The noise byte, when set, fixes the noise mode instead of the note choosing it.

Replies are **CC 46 k** (taken) or **CC 47 k** (refused). **CC 48 n** sets the PSG set size (no reply).

### 3.3 Drum kit image

Send it with 61, CRC, length, chunks, 63 (section 2.2). The image is at most 48 KB:

```
u32 n                       (at most 32)
n x { u32 length, u32 rate_hz }   (rate 1000 to 50000)
sample bytes, one sound after another, 8 bit unsigned (as the YM2612 DAC takes them)
```

### 3.4 Multi (the parts' setup)

All on channel 16. Changes are in RAM until CC 4E, which also keeps the parts. SAVE on the FM-1 keeps them too.

| CC | Meaning | Reply |
|---|---|---|
| 20 p | Pick part p (0 to 5) for the messages below | none |
| 21 c | Its MIDI channel: 0 to 15, 16 for none (the FM-1's keys still play it), or 17 for All (1 to 6 and 12 to 15) | 2F p |
| 22 v | Its level, 0 to 127 | 2F p |
| 23 t | Its trigger: 0 key, 1 delay, 2 release | 2F p |
| 24 t | Its time, in 10 ms steps (the delay, or how long a release part holds) | 2F p |
| 25 s | Its sound: library slot s | 2F p |
| 26 m | The MIDI mode: 0 multi, 1 GenMDM, 2 panel | 2F 7F |
| 27 p | Read part p | 20 p, then 21 to 26 with its values, then 2F p |

The FM-1 sends **CC 28+p s** (28 to 2D) whenever part p's slot changes, for example by PRESETS or a program change.

## 4. Source map

| Topic | Where |
|---|---|
| Device side, all of the above | `firmware/src/opn1.c`: `song_rx`, `patch_rx`, `psgi_rx`, `kit_open`, `store_save`, main loop MIDI dispatch |
| VGM playback | `firmware/src/vgm.c`, `vgm.h` |
| Reference sender | `web/index.html`: `link`, `control`, `sendImage`, `playSong`, `sendPatch`, `sendPsg`, `sendSounds` |
| Minimal script sender | `tests/upload_script.py` |
| Simulated device for testing a sender | `tests/web_link_test.py` (headless page against a fake FM-1), `tests/host.c` (scripted MIDI in) |
