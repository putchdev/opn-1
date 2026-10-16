#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""web/index.html's transfers against a simulated FM-1 that loses messages, in headless Chrome.
  tests/web_link_test.py SONG.vgm [DROP_PERCENT] [SECONDS]    a song streamed while it plays
  tests/web_link_test.py SONG.vgm [DROP_PERCENT] sounds        Send sounds to FM-1: instruments, PSG shapes, the kit
The page runs unchanged; WebMIDI is replaced by a fake FM-1 with the firmware's chunk rules (firmware/src/opn1.c)
that drops each incoming message with the given chance and plays the stream at the song's rate. Passes when the
bank and the streamed commands arrive byte for byte and the stream never runs dry."""
import http.server
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading

FAKE = r"""<script>
// ---- a fake FM-1 behind a fake WebMIDI (the chunk rules of firmware/src/opn1.c)
const TEST = { drop: %DROP% / 100, seconds: %SECONDS%, sounds: %SOUNDS%, rng: 12345 };
const rnd = () => (TEST.rng = (TEST.rng * 1103515245 + 12345) >>> 0) / 4294967296;
const dev = { on: 0, seq: 0, c_on: 0, c_bad: 0, c_seq: 0, c_buf: [], c_crc: 0, bank: [], ring: [], wantLen: 0, wantCrc: 0,
              pos: 0, playing: false, starved: 0, sentKb: 0, dropped: 0, done: false, rate: 0 };
const fakeIn = { name: "FM-1 MIDI 1", onmidimessage: null };
function reply(a, b, c) {   // (as soon as the page's send returns: timers run late under headless virtual time)
  Promise.resolve().then(() => fakeIn.onmidimessage && fakeIn.onmidimessage({ data: Uint8Array.from([a, b, c]) }));
}
function crc16d(b, c = 0xFFFF) { for (const x of b) { c ^= x << 8; for (let k = 0; k < 8; k++) c = (c & 0x8000 ? c << 1 ^ 0x1021 : c << 1) & 0xFFFF; } return c; }
function devMsg(st, d1, d2) {
  const ch = st & 15, type = st & 0xF0;
  if ((ch === 14 || ch === 15) && (type === 0xA0 || type === 0xE0)) {
    const w = d1 | d2 << 7 | ((type === 0xE0 ? 1 : 0) + (ch === 15 ? 2 : 0)) << 14;
    if (!dev.c_on) return;
    if (dev.c_buf.length + 2 > 256) dev.c_bad = 1; else dev.c_buf.push(w & 255, w >> 8);
    return;
  }
  if (st !== 0xBF) return;
  if (d1 >= 0x6B && d1 <= 0x6F && !dev.c_on) return;   // (as the firmware: parts of a chunk whose begin was lost)
  if (d1 >= 0x78 && d1 <= 0x7C) { dev.wantCrc |= d2 << (7 * (d1 - 0x78)); return; }
  if (d1 >= 0x7D) { dev.wantLen |= d2 << (7 * (d1 - 0x7D)); return; }
  switch (d1) {
  case 0x6A: dev.c_on = 1; dev.c_bad = 0; dev.c_buf = []; dev.c_crc = 0; dev.c_off = d2; break;
  case 0x6B: dev.c_off |= d2 << 7; break;
  case 0x6C: dev.c_off |= d2 << 14; break;
  case 0x6D: dev.c_crc |= d2; break;
  case 0x6E: dev.c_crc |= d2 << 7; break;
  case 0x6F: {
    dev.c_crc |= (d2 & 3) << 14; dev.c_on = 0;
    const tgt = dev.on === 1 ? dev.bank : dev.ring, room = dev.on === 2 ? dev.ring.length + dev.c_buf.length - dev.pos <= 131072 : true;
    const o = dev.c_off, okOff = ((o - tgt.length) & 0x1FFFFF) === 0, okCrc = crc16d(dev.c_buf, crc16d([o & 255, o >> 8 & 255, o >> 16 & 255])) === dev.c_crc;
    dev.rej = dev.rej || { off: 0, crc: 0, room: 0, bad: 0 };
    if (!okOff) dev.rej.off++; else if (!okCrc) dev.rej.crc++; else if (!room) dev.rej.room++; else if (dev.c_bad) dev.rej.bad++;
    if (dev.on && !dev.c_bad && okOff && okCrc && room) tgt.push(...dev.c_buf);
    const h = (dev.on === 1 ? dev.bank : dev.ring).length;
    reply(0xBF, 0x65, h & 127); reply(0xBF, 0x66, h >> 7 & 127); reply(0xBF, 0x67, h >> 14 & 127);
  } break;
  case 0x55: dev.patches = (dev.patches || 0) + 1; reply(0xBF, 0x56, 0); break;   // (frames: their content is the firmware's test)
  case 0x45: dev.psgs = (dev.psgs || 0) + 1; reply(0xBF, 0x46, 0); break;
  case 0x4E: reply(0xBF, 0x6E, 0x4E); dev.saved = (dev.saved || 0) + 1; reply(0xBF, 0x4D, 0); break;
  case 0x61: dev.on = 1; dev.bank = []; dev.wantLen = dev.wantCrc = 0; dev.kitMode = true; reply(0xBF, 0x6E, 0x61); break;
  case 0x63: dev.on = 0; reply(0xBF, 0x73, dev.bank.length === ((dev.wantLen + 1) & ~1) ? 0 : 1); break;
  case 0x70: dev.on = 1; dev.bank = []; dev.wantLen = dev.wantCrc = 0; dev.playing = false; reply(0xBF, 0x6E, 0x70); break;
  case 0x72: dev.on = 0; reply(0xBF, 0x73, dev.bank.length === ((dev.wantLen + 1) & ~1) ? 0 : 1); break;   // (the length only: the test compares the bytes)
  case 0x60: dev.on = 2; dev.ring = []; dev.pos = 0; dev.sentKb = 0; reply(0xBF, 0x6E, 0x60); break;
  case 0x75:
    if (d2 && !dev.playing) {
      dev.playing = true; reply(0xBF, 0x74, 1);
      const t0 = performance.now();
      let tl = t0;
      const tick = setInterval(() => {      // play: take the commands at the song's rate, by the clock
        const now = performance.now(), want = dev.rate * (now - tl) / 10;
        tl = now;
        if (dev.ring.length - dev.pos < want) { dev.starved++; reply(0xBF, 0x7B, dev.starved & 127); }
        dev.pos = Math.min(dev.ring.length, dev.pos + want);
        if ((dev.pos >> 10 & 127) !== dev.sentKb) { dev.sentKb = dev.pos >> 10 & 127; reply(0xBF, 0x77, dev.sentKb); }
        if (performance.now() - t0 > TEST.seconds * 1000) { clearInterval(tick); dev.playing = false; reply(0xBF, 0x74, 0); dev.done = true; }
      }, 10);
    } else if (!d2 && dev.playing) { dev.playing = false; reply(0xBF, 0x74, 0); }
    else reply(0xBF, 0x74, dev.playing ? 1 : 0);
    break;
  }
}
const fakeOut = { name: "FM-1 MIDI 1", id: "fm1", send(data) {
  for (let i = 0; i + 2 < data.length; i += 3) {
    if (rnd() < TEST.drop) { dev.dropped++; continue; }   // lost on the way
    devMsg(data[i], data[i + 1], data[i + 2]);
  }
} };
navigator.requestMIDIAccess = async () => ({
  outputs: new Map([["fm1", fakeOut]]), inputs: new Map([["fm1in", fakeIn]]), onstatechange: null });
</script>"""

DRIVER = r"""<script>
(async () => {
  await new Promise(r => setTimeout(r, 200));
  const b = new Uint8Array(await (await fetch("song.vgm")).arrayBuffer());
  const sp = vgmSplit(b);
  dev.rate = u32(b, 0x18) ? sp.cmds.length / (u32(b, 0x18) / 44100) / 100 : 10;   // bytes per 10 ms (no length: 1 KB/s)
  const t0 = performance.now();
  if (TEST.sounds) {                            // Send sounds to FM-1: the kit must arrive whole
    await loadFiles([new File([b], "song.vgm")]);
    await sendSounds();
    const k = kitImage(state.drums);
    const kitOk = k.n === 0 || (dev.bank.length >= k.img.length && k.img.every((x, i) => dev.bank[i] === x));
    document.getElementById("result").textContent = `RESULT kit ${kitOk ? "ok" : "BAD"} (${k.n} sounds, ${dev.bank.length} B), ` +
      `instruments ${dev.patches || 0}, PSG shapes ${dev.psgs || 0}, saved ${dev.saved || 0}, dropped ${dev.dropped} messages, dry 0, status "${$("status").textContent}"`;
    return;
  }
  state.songs = [{ name: "test", secs: 0, sp, warn: [] }];
  $("loop").checked = true;
  setInterval(() => { if (!dev.done) document.getElementById("result").textContent =
    `PROGRESS bank ${dev.bank.length} ring ${dev.ring.length} pos ${dev.pos|0} on ${dev.on} link held ${link.committed} sent ${link.sent} ` +
    `size ${link.size} resends ${link.resends} dropped ${dev.dropped} playing ${dev.playing}/${song.playing} status "${$("status").textContent}"`; }, 500);
  playSong(0);
  while (!dev.done) await new Promise(r => setTimeout(r, 100));
  await new Promise(r => setTimeout(r, 300));
  // what should have arrived: the bank image, and the commands unrolled (loop section again and again)
  const want = [], c = sp.cmds, stop = sp.loopAt >= 0 ? c.length - 1 : c.length;
  for (let at = 0; want.length < dev.ring.length;) { if (at >= stop) { if (sp.loopAt < 0) break; at = sp.loopAt; } want.push(c[at++]); }
  let bankOk = dev.bank.length >= sp.image.length && sp.image.every((x, i) => dev.bank[i] === x);
  const firstBad = sp.image.findIndex((x, i) => dev.bank[i] !== x);
  const badInfo = firstBad < 0 ? "" : ` first bad byte ${firstBad}: got ${dev.bank.slice(firstBad, firstBad + 6)} want ${Array.from(sp.image.slice(firstBad, firstBad + 6))}`;
  let ringOk = dev.ring.every((x, i) => i >= want.length || want[i] === x);
  document.getElementById("result").textContent =
    `RESULT bank ${bankOk ? "ok" : "BAD"} (${dev.bank.length} B${badInfo}), stream ${ringOk ? "ok" : "BAD"} (${dev.ring.length} B, played ${dev.pos} B), ` +
    `dry ${dev.starved}, dropped ${dev.dropped} messages, ${link.resends} go-back(s) (${link.whyBack.timeout} timeout, ${link.whyBack.refused} refused) rej ${JSON.stringify(dev.rej)}, status "${$("status").textContent}", ` +
    `${((performance.now() - t0) / 1000).toFixed(1)} s`;
})();
</script><pre id="result"></pre>"""

def main():
    song, drop = sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 2
    sounds = len(sys.argv) > 3 and sys.argv[3] == "sounds"
    secs = 20 if sounds else float(sys.argv[3]) if len(sys.argv) > 3 else 20
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    page = open(os.path.join(root, "web", "index.html")).read()
    page = page.replace("<head>", "<head>" + FAKE.replace("%DROP%", str(drop)).replace("%SECONDS%", str(secs)).replace("%SOUNDS%", "true" if sounds else "false"), 1)
    page = page.replace("</body>", DRIVER + "</body>", 1)
    d = tempfile.mkdtemp()
    open(os.path.join(d, "t.html"), "w").write(page)
    shutil.copy(song, os.path.join(d, "song.vgm"))
    class Quiet(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a): super().__init__(*a, directory=d)
        def log_message(self, *a): pass
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Quiet)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    out = subprocess.run(["google-chrome", "--headless=new", "--disable-gpu", "--no-sandbox", f"--user-data-dir={d}/c",
                          f"--virtual-time-budget={int((secs + 60) * 1000)}", "--dump-dom",
                          f"http://127.0.0.1:{srv.server_address[1]}/t.html"], capture_output=True, text=True, timeout=600).stdout
    srv.shutdown()
    shutil.rmtree(d, ignore_errors=True)
    m = re.search(r'<pre id="result">((?:RESULT|PROGRESS)[^<]*)</pre>', out)
    line = m.group(1) if m else "RESULT none: the page did not finish"
    print(line)
    sys.exit(0 if line.startswith("RESULT") and "BAD" not in line and "dry 0," in line else 1)

main()
