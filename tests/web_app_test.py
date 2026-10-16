#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""web/index.html's library, editor and parts in headless Chrome, against a fake FM-1 with the firmware's
library and parts messages (docs/MIDI.md 3) that loses some of what the page sends.
  tests/web_app_test.py [DROP_PERCENT]
An .opm file (made here, 2612.org's centred detune) is imported, sent to bank 2, edited, saved and cleared; the
parts' mode and a channel are set. Passes when the fake FM-1 ends up holding exactly what it should."""
import http.server
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading

OPM = """//MiOPMdrv sound bank Paramer Ver2002.04.22
//LFO: LFRQ AMD PMD WF NFRQ
//@:[Num] [Name]
//CH: PAN\tFL CON AMS PMS SLOT NE
//[OPname]: AR D1R D2R\tRR D1L\tTL\tKS MUL DT1 DT2 AMS-EN

@:0 Instrument 0
LFO: 0 0 0 0 0
CH: 64 7 2 0 0 120 0
M1: 14 14 0 15 1 24 2 1 3 0 0
C1: 14 14 0 15 15 78 2 7 0 0 0
M2: 13 14 0 15 1 22 2 1 5 0 0
C2: 19 3 0 15 0 16 1 1 3 0 0

@:1 Instrument 1
LFO: 0 0 0 0 0
CH: 64 5 4 1 3 120 0
M1: 31 10 5 7 2 30 0 2 6 0 128
C1: 28 8 3 8 3 0 1 1 3 0 0
M2: 31 12 4 6 2 40 0 4 1 0 0
C2: 25 9 2 9 4 4 1 2 3 0 0

@:2 no Name
LFO: 0 0 0 0 0
CH: 64 0 0 0 0 64 0
M1: 31 0 0 4 0 0 0 1 0 0 0
C1: 31 0 0 4 0 0 0 1 0 0 0
M2: 31 0 0 4 0 0 0 1 0 0 0
C2: 31 0 0 4 0 0 0 1 0 0 0

@:3 Instrument 3
LFO: 0 0 0 0 0
CH: 64 1 1 0 0 120 0
M1: 31 0 0 4 0 2289080 0 1 0 0 0
C1: 31 0 0 4 0 0 0 1 0 0 0
M2: 31 0 0 4 0 0 0 -1 0 0 0
C2: 31 0 0 4 0 0 0 1 0 0 0
"""


def expected(fl, con, ams, pms, ops):
    """the 30 register values the firmware should get: an independent reading of the format"""
    v = [fl << 3 | con, 0xC0 | ams << 4 | pms]
    for ar, d1r, d2r, rr, d1l, tl, ks, mul, dt1, _dt2, amsen in ops:
        dt = {0: 7, 1: 6, 2: 5, 3: 0, 4: 1, 5: 2, 6: 3}[dt1]
        v += [dt << 4 | mul, tl, ks << 6 | ar, (0x80 if amsen else 0) | d1r, d2r, d1l << 4 | rr, 0]
    return v


WANT0 = expected(7, 2, 0, 0, [[14, 14, 0, 15, 1, 24, 2, 1, 3, 0, 0], [14, 14, 0, 15, 15, 78, 2, 7, 0, 0, 0],
                               [13, 14, 0, 15, 1, 22, 2, 1, 5, 0, 0], [19, 3, 0, 15, 0, 16, 1, 1, 3, 0, 0]])
WANT1 = expected(5, 4, 1, 3, [[31, 10, 5, 7, 2, 30, 0, 2, 6, 0, 128], [28, 8, 3, 8, 3, 0, 1, 1, 3, 0, 0],
                               [31, 12, 4, 6, 2, 40, 0, 4, 1, 0, 0], [25, 9, 2, 9, 4, 4, 1, 2, 3, 0, 0]])

FAKE = r"""<script>
const DROP = %DROP% / 100;
let rng = 12345;
const rnd = () => (rng = (rng * 1103515245 + 12345) >>> 0) / 4294967296;
const dev = { lib: Array(128).fill(null), parts: [0, 1, 2, 3, 4, 5].map(p => ({ chan: p, level: 127, trig: 0, time: 10, slot: 0 })),
              mode: 0, cfg: 0, pf: null, saved: 0, frames: [], dropped: 0 };
const fakeIn = { name: "FM-1 MIDI 1", onmidimessage: null };
function reply(a, b, c) { Promise.resolve().then(() => fakeIn.onmidimessage && fakeIn.onmidimessage({ data: Uint8Array.from([a, b, c]) })); }
function devMsg(st, d1, d2) {
  if (st !== 0xBF) return;
  const pf = dev.pf;
  switch (d1) {
  case 0x50: case 0x5B: dev.pf = { slot: d1 === 0x5B, t: d2, name: "", vals: [], lo: 0, want: 0 }; break;
  case 0x59: if (pf) pf.name += String.fromCharCode(d2); break;
  case 0x52: if (pf) pf.lo = d2; break;
  case 0x53: if (pf) pf.vals.push(pf.lo | d2 << 7); break;
  case 0x54: if (pf) pf.want = d2; break;
  case 0x55: {
    if (!pf) break;
    const sum = ([...pf.name].reduce((a, c) => a + c.charCodeAt(0), 0) + pf.vals.reduce((a, x) => a + x, 0)) & 0x3FFF;
    const ok = sum === (pf.want | d2 << 7) && [30, 32, 35].includes(pf.vals.length);
    if (ok && pf.slot) dev.lib[pf.t] = { name: pf.name, vals: pf.vals.concat([0, 0, 0, 0, 0]).slice(0, 35) };
    if (ok && !pf.slot) dev.frames.push({ t: pf.t, vals: pf.vals });
    reply(0xBF, pf.slot ? (ok ? 0x3E : 0x3F) : (ok ? 0x56 : 0x57), pf.t);
    dev.pf = null;
  } break;
  case 0x5C: {
    const p = dev.lib[d2];
    if (!p) { reply(0xBF, 0x36, d2); break; }
    let sum = 0;
    reply(0xBF, 0x30, d2);
    for (const c of p.name) { reply(0xBF, 0x31, c.charCodeAt(0)); sum += c.charCodeAt(0); }
    for (const x of p.vals) { reply(0xBF, 0x32, x & 127); reply(0xBF, 0x33, x >> 7); sum += x; }
    reply(0xBF, 0x34, sum & 127); reply(0xBF, 0x35, sum >> 7 & 127);
  } break;
  case 0x5E: dev.lib[d2] = null; reply(0xBF, 0x3D, d2); break;
  case 0x58: for (let i = 0; i < 32; i++) dev.lib[d2 * 32 + i] = null; reply(0xBF, 0x3D, d2); break;
  case 0x4E: reply(0xBF, 0x6E, 0x4E); dev.saved++; reply(0xBF, 0x4D, 0); break;
  case 0x20: dev.cfg = d2; break;
  case 0x21: dev.parts[dev.cfg].chan = d2; reply(0xBF, 0x2F, dev.cfg); break;
  case 0x22: dev.parts[dev.cfg].level = d2; reply(0xBF, 0x2F, dev.cfg); break;
  case 0x23: dev.parts[dev.cfg].trig = d2; reply(0xBF, 0x2F, dev.cfg); break;
  case 0x24: dev.parts[dev.cfg].time = d2; reply(0xBF, 0x2F, dev.cfg); break;
  case 0x25: dev.parts[dev.cfg].slot = d2; reply(0xBF, 0x2F, dev.cfg); break;
  case 0x26: dev.mode = d2; reply(0xBF, 0x2F, 0x7F); break;
  case 0x27: {
    const p = dev.parts[d2];
    reply(0xBF, 0x20, d2); reply(0xBF, 0x21, p.chan); reply(0xBF, 0x22, p.level); reply(0xBF, 0x23, p.trig);
    reply(0xBF, 0x24, p.time); reply(0xBF, 0x25, p.slot); reply(0xBF, 0x26, dev.mode); reply(0xBF, 0x2F, d2);
  } break;
  }
}
const fakeOut = { name: "FM-1 MIDI 1", id: "fm1", send(data) {
  for (let i = 0; i + 2 < data.length; i += 3) {
    if (rnd() < DROP) { dev.dropped++; continue; }
    devMsg(data[i], data[i + 1], data[i + 2]);
  }
} };
navigator.requestMIDIAccess = async () => ({ outputs: new Map([["fm1", fakeOut]]), inputs: new Map([["fm1in", fakeIn]]), onstatechange: null });
window.confirm = () => true;
const errs = [];
addEventListener("error", e => errs.push(e.message + " @" + e.lineno));
addEventListener("unhandledrejection", e => errs.push("rejected: " + (e.reason && e.reason.message || e.reason)));
setInterval(() => { const r = document.getElementById("result"); if (r && !r.textContent.startsWith("RESULT")) r.textContent = "STEP " + (window.step || 0) + " " + errs.join(" | ") + " status: " + (document.getElementById("status") || {}).textContent; }, 200);
</script>"""

DRIVER = r"""<script>
(async () => {
  const sleep = ms => new Promise(r => setTimeout(r, ms));
  const idle = async () => { await sleep(50); while (busy) await sleep(50); await sleep(50); };
  const checks = [];
  const check = (what, ok) => { checks.push((ok ? "Passed  " : "FAILED  ") + what); window.step = checks.join(" / "); };
  const eq = (a, b) => JSON.stringify(a) === JSON.stringify(b);
  await sleep(600); await idle();                        // (connecting reads the parts and the library)
  check("connecting reads the empty library", state.lib.every(x => x === null));
  await loadFiles([new File([%OPM%], "Sonic_the_Hedgehog_2_-_24_-_Extra_Life.opm")]);
  check("two good patches imported (empty and broken ones skipped)", state.imports.length === 2);
  check("named from the file", state.imports[0].name === "STH2 24 1");
  $("normalise").checked = false;
  $("tbank").value = "1"; $("tbank").onchange();
  $("tslot").value = "4"; $("tslot").onchange();
  $("sendimp").click(); await idle();
  check("patch 1 in B2 05 as the format says", dev.lib[36] && eq(dev.lib[36].vals.slice(0, 30), %WANT0%));
  check("patch 2 in B2 06 as the format says", dev.lib[37] && eq(dev.lib[37].vals.slice(0, 30), %WANT1%));
  check("nothing else written", dev.lib.filter(Boolean).length === 2);
  check("kept in flash", dev.saved >= 1);
  state.lib.fill(undefined); $("readlib").click(); await idle();
  check("read back from the FM-1", state.lib[36]?.name === "STH2 24 1" && eq(state.lib[37]?.regs, %WANT1%));
  check("the editor's patch menu lists them", [...$("edslot").options].some(o => o.value === "36") && [...$("edslot").options].some(o => o.value === "37"));
  state.sel = 36; renderLib(); $("editslot").click(); await idle();
  check("Edit puts the slot on part 1", dev.parts[0].slot === 36 && $("edname").value === "STH2 24 1");
  document.querySelectorAll("#algs button")[4].click(); await sleep(3000);
  check("an edit plays live on part 1", dev.frames.length && dev.frames[dev.frames.length - 1].t === 0 && (dev.frames[dev.frames.length - 1].vals[0] & 7) === 4);
  $("edslot").value = "37"; $("edslot").onchange(); await idle();
  check("the patch menu moves part 1 to another slot", dev.parts[0].slot === 37 && $("edname").value === "STH2 24 2");
  fakeIn.onmidimessage({ data: Uint8Array.from([0xBF, 0x28, 36]) }); await sleep(300); await idle();
  check("the editor follows PRESETS on the FM-1", state.ed.slot === 36 && $("edname").value === "STH2 24 1");
  document.querySelectorAll("#algs button")[4].click(); await sleep(3000);
  $("edsave").click(); await idle();
  check("Save writes it to its slot", (dev.lib[36].vals[0] & 7) === 4 && (dev.lib[36].vals[0] >> 3) === 7);
  showTab("parts");
  $("mode").value = "1"; $("mode").onchange(); await idle();
  check("MIDI mode set", dev.mode === 1);
  $("mode").value = "0"; $("mode").onchange(); await idle();
  const ch = $("partstable").rows[2].cells[2].firstChild; ch.value = "0"; ch.onchange({ target: ch }); await idle();
  check("part 2 onto MIDI channel 1", dev.parts[1].chan === 0);
  state.bank = 1; $("clearbank").click(); await idle();
  check("bank 2 cleared", dev.lib.every(x => x === null) && state.lib.slice(32, 64).every(x => x === null));
  document.getElementById("result").textContent = "RESULT\n" + checks.join("\n") + `\n(dropped ${dev.dropped} messages, status "${$("status").textContent}")`;
})();
</script><pre id="result"></pre>"""


def main():
    drop = float(sys.argv[1]) if len(sys.argv) > 1 else 2
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    page = open(os.path.join(root, "web", "index.html")).read()
    page = page.replace("<head>", "<head>" + FAKE.replace("%DROP%", str(drop)), 1)
    import json
    page = page.replace("</body>", DRIVER.replace("%OPM%", json.dumps(OPM)).replace("%WANT0%", json.dumps(WANT0))
                        .replace("%WANT1%", json.dumps(WANT1)) + "</body>", 1)
    d = tempfile.mkdtemp()
    open(os.path.join(d, "t.html"), "w").write(page)

    class Quiet(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a): super().__init__(*a, directory=d)
        def log_message(self, *a): pass
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Quiet)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    out = subprocess.run(["google-chrome", "--headless=new", "--disable-gpu", "--no-sandbox", f"--user-data-dir={d}/c",
                          "--virtual-time-budget=120000", "--dump-dom", f"http://127.0.0.1:{srv.server_address[1]}/t.html"],
                         capture_output=True, text=True, timeout=600).stdout
    srv.shutdown()
    shutil.rmtree(d, ignore_errors=True)
    m = re.search(r'<pre id="result">(RESULT[^<]*)</pre>', out)
    s = re.search(r'<pre id="result">([^<]*)</pre>', out)
    text = m.group(1).replace("&quot;", '"') if m else "RESULT none: the page did not finish: " + (s.group(1) if s else "")
    print(text)
    sys.exit(0 if m and "FAILED" not in text else 1)


main()
