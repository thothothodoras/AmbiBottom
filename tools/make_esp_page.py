#!/usr/bin/env python3
"""make_esp_page.py (v0.37) - the AmbiBottom usermod's page (/ambi on the ESP)
made from the Linux bridge's monitor page, so there is one page to maintain.

It reads the page out of src/monitor_server.cpp (kPage), changes only what
differs on the ESP, and writes:
  firmware/wled-usermod/AmbiBottom/ambi_page.html   the page as served (readable, for review and the simulator)
  firmware/wled-usermod/AmbiBottom/ambi_page_gz.h   the same, gzipped, as a PROGMEM array for the firmware

What changes on the ESP:
  - every fetch('/api/...') goes to /ambi/api/... (on the ESP /api belongs to the
    Hue bridge the TV talks to);
  - no Server-Sent Events: the page polls /ambi/api/state every 250 ms;
  - panels that need the Linux bridge are hidden (identity,
    discovery, login, the LED log, Restart / Exit);
  - texts that talk about the terminal / settings.json say what the ESP does instead;
  - a small "Ταινία" card at the top of the right-hand column (the TV stays first): Start / Stop, Demo, five simple WLED effects
    (Rainbow, Colorloop, Pacifica, Aurora, Colorwaves; default: one of them at random),
    brightness and speed of the demo;
  - the bottom view starts on "Ταινία" (what the strip shows), not on the lamps;
  - v0.40: the Linux-only panels' HTML is left out (identity, discovery, access, login dialog);
    an "ESP & WLED" panel in the Bridge tab: WLED's Length and data pin, lamps 3 / 5, auto Start,
    forget the TV, restart, and the ESP's event log.

usage: tools/make_esp_page.py [--check]   (--check: exit 1 if the files are not up to date)
"""
import gzip
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "src", "monitor_server.cpp")
OUT_DIR = os.path.join(REPO, "firmware", "wled-usermod", "AmbiBottom")
OUT_HTML = os.path.join(OUT_DIR, "ambi_page.html")        # v0.45: English (the default)
OUT_HTML_EL = os.path.join(OUT_DIR, "ambi_page_el.html")  # v0.45: Greek, as the browser ends up with it (reference, tests)
OUT_EL_PATCH = os.path.join(OUT_DIR, "ambi_page_el_patch.json")  # v0.49: what the firmware serves for Greek
OUT_EL_LOADER = os.path.join(OUT_DIR, "ambi_page_el_loader.html")

# v0.49 (space): Greek is no longer a second full page (66 KB gz) but a patch on the English one
# (~22 KB gz). Both pages are split at the same delimiters (< > ' " `, the GREEK_RUN boundaries, so the
# token lists line up 1:1); the patch lists the tokens that differ. /ambi/el serves a tiny loader that
# fetches /ambi/en and /ambi/el.json, swaps the tokens and writes the page. The build proves that
# English + patch == the Greek page, byte for byte.
TOKEN_SPLIT = re.compile(r"""([<>'"`])""")
EL_LOADER = """<!doctype html><html lang="el"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>AmbiBottom</title><style>body{margin:0;background:#0a0e17;color:#8998b3;font:14px sans-serif;padding:30px}</style></head><body>Φόρτωση…<script>
(async()=>{try{const[h,pt]=await Promise.all([fetch('/ambi/en',{cache:'no-store'}).then(r=>r.text()),fetch('/ambi/el.json',{cache:'no-store'}).then(r=>r.json())]);
const t=h.split(/([<>'"`])/);if(t.length!==pt.n)throw new Error('size');for(const[i,x]of pt.p)t[i]=x;
document.open();document.write(t.join(''));document.close();}catch(e){location.replace('/ambi/en');}})();
</script></body></html>
"""


def el_patch(en, el):
    te, tl = TOKEN_SPLIT.split(en), TOKEN_SPLIT.split(el)
    if len(te) != len(tl):
        sys.exit(f"Greek patch: token counts differ ({len(te)} / {len(tl)})")
    patch = [[i, b] for i, (a, b) in enumerate(zip(te, tl)) if a != b]
    t = list(te)
    for i, x in patch:
        t[i] = x
    if "".join(t) != el:
        sys.exit("Greek patch: English + patch != the Greek page")
    return json.dumps({"n": len(te), "p": patch}, ensure_ascii=False, separators=(",", ":"))
I18N = os.path.join(REPO, "tools", "i18n", "el_to_en.json")
OUT_H = os.path.join(OUT_DIR, "ambi_page_gz.h")


def extract_page(cpp):
    start = cpp.find('const char* kPage = R"PAGE(')
    if start < 0:
        sys.exit("kPage not found in src/monitor_server.cpp")
    start = cpp.index('R"PAGE(', start) + len('R"PAGE(')
    end = cpp.index(')PAGE"', start)
    return cpp[start:end]


def sub_once(text, old, new, count=1):
    n = text.count(old)
    if n != count:
        sys.exit(f"expected {count} x {old[:70]!r} in the Linux page, found {n}: the page changed, update make_esp_page.py")
    return text.replace(old, new)


ESP_CSS = """<style>
/* v0.49 (user: the top of the page is crooked on a phone): the top-right block was a
   right-aligned column (EN, WLED, status, lamps each on its own line). Now: title, then one row
   with PIN / EN / WLED, then the status chip and the lamps button at full width. */
@media(max-width:680px){
  .topbar{flex-direction:column;align-items:stretch!important;gap:12px}
  .top-actions{flex-direction:row!important;flex-wrap:wrap;align-items:center!important;justify-content:flex-start;gap:6px}
  .top-actions .inputs-status{flex:1 1 100%;order:10}
  .top-actions .inputs-chip{width:100%;justify-content:center}
  .top-actions .lamps-open-btn{flex:1 1 100%;order:11;justify-content:center}
  .chip-conn #connectionText{display:inline!important}  /* room for it now */
}
</style>
<style>
/* v0.37 ESP (AmbiBottom): panels that need the Linux bridge; the big "stopped" banner (the card says it) */
#identityPanel,#discoveryPanel,#accessPanel,#btnRestart,#btnExit,#loginButton,#serviceBanner,
a[href="/ambi/settings"]{display:none!important}
/* the ESP card: small, in the right-hand column, the TV stays first (user 2026-11-18) */
.esp-panel{padding:16px 18px}
.esp-head{display:flex;justify-content:space-between;align-items:flex-start;gap:10px;margin-bottom:12px}
.esp-head strong{font-size:15px;letter-spacing:-.01em}
.esp-head .eyebrow{margin-bottom:4px}
.esp-row{display:flex;flex-wrap:wrap;gap:8px;align-items:center}
.esp-row button{flex:1;min-width:64px;padding:8px 10px;border-radius:8px;border:1px solid #31425c;background:#172238;color:#d6e6f6;font-weight:700;font-size:12px}
.esp-row button:hover:not(:disabled){background:#233454}
.esp-row button.on{background:#1f4a39;border-color:#3f8a69;color:#c9f2de}
.esp-row button:disabled{opacity:.4;cursor:default}
#espState{font-size:10px;padding:6px 9px}
#espState.run{color:#9be7c2;border-color:#3f7a5f;background:#16302a}
#espState.stop{color:#c2d1e4}
.cam-actions{display:flex;flex-wrap:wrap;gap:8px;margin:10px 0}
.cam-actions button,.cam-file{padding:8px 12px;border:1px solid #407d8a;border-radius:7px;background:#173747;color:#d6eff0;font-weight:700;font-size:12px;cursor:pointer}
.cam-actions button.secondary{border-color:#35425a;background:#151d2c;color:#b8c7da}
.cam-actions button:disabled{opacity:.45;cursor:default}
.cam-wrap{background:#0a1322;border:1px solid #263148;border-radius:10px;overflow:hidden}
.cam-wrap canvas{display:block;width:100%;height:auto;touch-action:none}
.cam-read{display:flex;flex-wrap:wrap;gap:6px 16px;margin-top:10px;font-size:11px;color:#9fb2c9}
.cam-read b{color:#e8edf6;font-variant-numeric:tabular-nums}
.cam-sw{display:inline-block;width:12px;height:12px;border-radius:3px;border:1px solid #35425a;margin-right:5px;vertical-align:-2px}
.cam-manual{display:flex;gap:8px;align-items:center;font-size:11px;color:#aab9ce;margin-top:10px}
.wb-slots{margin-top:14px;padding-top:12px;border-top:1px solid #263148}
.wb-slot{display:grid;grid-template-columns:minmax(0,1fr) auto auto auto auto;gap:6px;align-items:center;margin:6px 0}
.wb-slot input{min-width:0;padding:7px 9px;border:1px solid #35425a;border-radius:7px;background:#0d1626;color:#e8edf6;font-size:12px}
.wb-slot-val{font-size:11px;color:#9fb2c9;font-variant-numeric:tabular-nums;white-space:nowrap}
.wb-slot button{padding:7px 10px;border:1px solid #407d8a;border-radius:7px;background:#173747;color:#d6eff0;font-weight:700;font-size:11px;cursor:pointer}
.wb-slot button.secondary{border-color:#35425a;background:#151d2c;color:#b8c7da}
.wb-slot button:disabled{opacity:.45;cursor:default}
@media(max-width:520px){.wb-slot{grid-template-columns:minmax(0,1fr) auto auto auto}.wb-slot-val{grid-column:1/-1;order:5}}
.js-on{display:flex;gap:9px;align-items:flex-start;font-size:11px;color:#aab9ce;line-height:1.5;margin:10px 0 12px}
.js-on input{margin-top:3px;accent-color:#65cdd0}
.fold{margin-top:12px;border-top:1px solid #243149;padding-top:10px}
.fold>summary{cursor:pointer;font-size:11px;color:#9fb2c9;font-weight:650;list-style:none}
.fold>summary::-webkit-details-marker{display:none}
.fold>summary::before{content:'▸';margin-right:6px;color:#6db0c7}.fold[open]>summary::before{content:'▾'}
.side-panel .strip-log-panel{padding:20px 23px}
.side-panel .strip-log-panel .log-switch{margin:10px 0}
.lower{grid-template-columns:1fr!important}
.esp-bri{display:grid;grid-template-columns:auto 1fr 44px;gap:10px;align-items:center;margin-top:12px;font-size:12px;color:#c2d1e4;font-weight:650}
.esp-bri input{width:100%;accent-color:#65cdd0}.esp-bri output{text-align:right;font-variant-numeric:tabular-nums;color:#dceeff}
.esp-more{margin-top:12px;border-top:1px solid #243149;padding-top:10px}
.esp-more summary{cursor:pointer;font-size:11px;color:#9fb2c9;font-weight:650;list-style:none;display:flex;justify-content:space-between;gap:8px}
.esp-more summary::-webkit-details-marker{display:none}
.esp-more summary::before{content:'▸';margin-right:6px;color:#6db0c7}
.esp-more[open] summary::before{content:'▾'}
.esp-grid{display:grid;grid-template-columns:auto 1fr auto;gap:8px 10px;align-items:center;margin-top:10px;font-size:11px;color:#aab9ce}
.esp-grid select{grid-column:2/4;width:100%;border:1px solid #35425a;background:#151d2c;padding:6px;border-radius:7px;color:#dce7f3;font-size:11px}
.esp-grid input[type=range]{width:100%;accent-color:#65cdd0}
.esp-grid output{min-width:34px;text-align:right;font-variant-numeric:tabular-nums;color:#d9effb}
#espHeap{font-weight:600;color:#7f93ad}
.mem-bar{display:block;height:5px;border-radius:4px;background:#26344c;overflow:hidden;margin-top:2px}
.mem-bar b{display:block;height:100%;width:0;background:linear-gradient(90deg,#4dbda7,#70d8cc);transition:width .3s}
.mem-bar b.warn{background:#f2ae65}.mem-bar b.bad{background:#e0777c}
#espNote{font-size:10px;color:#8190a9;line-height:1.55;margin:10px 0 0}
/* v0.40: "ESP & WLED" panel in the Bridge tab, and the event log */
#devWledMa{color:#d9effb}
.esp-dev select,.esp-dev input[type=number]{border:1px solid #35425a;background:#151d2c;padding:7px 8px;border-radius:7px;color:#dce7f3;font-size:12px;min-width:110px}
/* v0.41: Bridge tab alignment - the service panel spans two rows (no gap under the device panel) */
.bridge-grid{grid-auto-flow:row dense}
/* v0.46: settings = sticky side column + one main column (see bridge_layout) */
.bridge-cols{display:grid;grid-template-columns:minmax(260px,300px) minmax(0,1fr);gap:18px;align-items:start}
.bridge-side{position:sticky;top:12px;display:flex;flex-direction:column;gap:18px}
.bridge-main{display:flex;flex-direction:column;gap:18px;min-width:0}
.bridge-main>*{grid-column:auto!important;margin:0}
.bridge-index{padding:14px;display:flex;flex-direction:column;gap:2px}
.bridge-index a{color:#a9bfd3;text-decoration:none;font-size:12px;font-weight:650;padding:7px 9px;border-radius:7px}
.bridge-index a:hover{background:#1b2937;color:#e8edf6}
.bridge-main>[id]{scroll-margin-top:12px}
@media(max-width:900px){.bridge-cols{grid-template-columns:1fr}.bridge-side{position:static}.bridge-index{flex-direction:row;flex-wrap:wrap}}
.esp-dev .esp-dev-form>div{min-width:0}
.esp-dev .esp-dev-form .hint{min-height:2.9em;line-height:1.45}
.esp-dev .dev-switch,.esp-dev .dev-actions{grid-column:1/-1}
.esp-dev .dev-switch .disc-switch{display:flex;margin:14px 0 4px;font-weight:400;font-size:11px;color:#9eafc5}
.dev-actions{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-top:12px}
.esp-dev .dev-inline{display:flex;align-items:center;gap:6px;margin:0;font-size:11px;font-weight:600;color:#b8c7da;white-space:nowrap}
.esp-dev .dev-inline input{accent-color:#65cdd0}
.dev-actions .dev-spacer{flex:1}
.dev-actions button{padding:9px 14px;border:1px solid #407d8a;border-radius:7px;background:#173747;color:#d6eff0;font-weight:700;font-size:12px;cursor:pointer}
.dev-actions button.secondary{border-color:#35425a;background:#151d2c;color:#b8c7da}
.dev-actions button:hover:not(:disabled){filter:brightness(1.2)}
.dev-actions button:disabled{opacity:.55;cursor:default}
/* v0.43: the Demo only with its option on (Συσκευή); the PIN pill */
.esp-panel.no-demo .demo-only{display:none!important}
#pinPill{cursor:pointer;color:#f3bd73;border-color:#765c32;background:#3e3020}
.pin-dialog{border:1px solid #35425a;border-radius:14px;background:#111725;color:#e8edf6;padding:20px 22px;max-width:320px;width:calc(100% - 32px)}
.pin-dialog::backdrop{background:#02050bcc}
.pin-dialog h2{margin:0 0 6px;font-size:17px}.pin-dialog p{font-size:12px;color:#9eafc5;margin:0 0 12px;line-height:1.5}
.pin-dialog input{width:100%;font-size:22px;letter-spacing:.5em;text-align:center;padding:10px;border:1px solid #35425a;border-radius:8px;background:#0b1220;color:#e4eef8}
.pin-err{color:#f2ae65!important;min-height:1.2em;margin:8px 0 4px!important}
.pin-actions{display:flex;gap:8px;justify-content:flex-end}
.pin-actions button{padding:9px 14px;border:1px solid #407d8a;border-radius:7px;background:#173747;color:#d6eff0;font-weight:700;font-size:12px;cursor:pointer}
.pin-actions button.secondary{border-color:#35425a;background:#151d2c;color:#b8c7da}
/* v0.44: diagnosis, capture, night */
.diag-btns{display:flex;gap:6px;flex-wrap:wrap}
.diag-sum{display:flex;flex-wrap:wrap;gap:8px;margin:0 0 10px;font-size:11px;color:#9eafc5}
.diag-sum span{border:1px solid #2b3b52;border-radius:8px;padding:6px 10px;background:#111a29}
.diag-sum b{font-size:15px;color:#57d6a1;margin-right:4px}.diag-sum .w b{color:#f2ae65}.diag-sum .e b{color:#e0777c}.diag-sum .n b{color:#8ea2bb}
.sec-list .err{color:#e0777c;font-weight:800}.sec-list .na{color:#8ea2bb;font-weight:800}
.diag-report{width:100%;min-height:160px;margin-top:10px;font:11px/1.45 ui-monospace,monospace;background:#0b1220;color:#c4d2e4;border:1px solid #35425a;border-radius:8px;padding:8px}
.esp-dev .dev-night{grid-column:1/-1}
.esp-dev .dev-night input[type=number]{width:62px;min-width:0;padding:3px 6px;font-size:11px}
.esp-dev .dev-night select{min-width:0;padding:3px 6px;font-size:11px}
/* v0.42: security card */
.sec-list{list-style:none;margin:0 0 6px;padding:0;display:grid;gap:8px}
.sec-list li{display:grid;grid-template-columns:22px 1fr;gap:8px;align-items:start;font-size:12px;color:#c4d2e4;line-height:1.5;padding:9px 11px;border:1px solid #243149;border-radius:9px;background:#111a29}
.sec-list li b{color:#e4eef8}.sec-list li small{display:block;color:#8ea2bb;font-size:10.5px}
.sec-list .ok{color:#57d6a1;font-weight:800}.sec-list .warn{color:#f2ae65;font-weight:800}
.esp-log{margin-top:14px;border-top:1px solid #243149;padding-top:10px}
.esp-log summary{cursor:pointer;font-size:12px;color:#9fb2c9;font-weight:650}
.esp-log ol{list-style:none;margin:8px 0 0;padding:0;max-height:340px;overflow:auto;font-size:11px;line-height:1.5;color:#c4d2e4}
.esp-log li{padding:3px 0;border-bottom:1px solid #1d2840;overflow-wrap:anywhere}
.esp-log time{color:#6db0c7;font-variant-numeric:tabular-nums;margin-right:6px}
</style>
</head>"""

ESP_PANEL = """<aside class="side-panel">
    <dialog id="pinDialog" class="pin-dialog" aria-labelledby="pinTitle">
      <form id="pinForm" novalidate>
        <h2 id="pinTitle">PIN ρυθμίσεων</h2>
        <p>Το PIN του WLED (4 ψηφία). Ξεκλειδώνει τις αλλαγές για 15 λεπτά.</p>
        <input id="pinInput" type="password" inputmode="numeric" pattern="[0-9]*" maxlength="4" autocomplete="off" aria-label="PIN">
        <p id="pinErr" class="pin-err" role="alert"></p>
        <div class="pin-actions"><button type="button" id="pinCancel" class="secondary">Άκυρο</button><button type="submit" id="pinOk">Ξεκλείδωμα</button></div>
      </form>
    </dialog>
    <div class="panel esp-panel no-demo" id="espPanel" aria-labelledby="espTitle">
      <div class="esp-head"><div><div class="eyebrow">AMBIBOTTOM · ESP32</div><strong id="espTitle">Ταινία</strong></div><span id="espState" class="pill stop">—</span></div>
      <div class="esp-bri"><label for="espTvBri" title="Πόσο δυνατά δείχνει η ταινία τα χρώματα της TV (πάντα κάτω από το όριο ρεύματος)">☀ Φωτεινότητα</label><input id="espTvBri" type="range" min="5" max="100" step="5" value="100" title="Πόσο δυνατά δείχνει η ταινία τα χρώματα της TV (πάντα κάτω από το όριο ρεύματος)"><output id="espTvBriVal">—</output></div>
      <details class="esp-more" id="espMore">
        <summary><span id="espMoreTitle">Start / Stop</span><span id="espHeap" title="Ελεύθερη μνήμη (heap) του ESP τώρα · η μικρότερη από τότε που άνοιξε η σελίδα">—</span></summary>
        <div class="esp-row">
          <button type="button" id="espStart">Start</button>
          <button type="button" id="espStop">Stop</button>
          <button type="button" id="espDemo" class="demo-only" title="Ένα εφέ του WLED στην ταινία όσο η TV δεν στέλνει">Demo</button>
        </div>
        <div class="esp-grid">
          <label for="espFx" class="demo-only">Εφέ</label><select id="espFx" class="demo-only"><option value="-1">Τυχαίο</option></select>
          <label for="espBri" class="demo-only">Demo</label><input id="espBri" class="demo-only" type="range" min="5" max="100" step="5" value="25"><output id="espBriVal" class="demo-only">25 %</output>
          <label for="espSpeed" class="demo-only">Ταχύτητα</label><input id="espSpeed" class="demo-only" type="range" min="0" max="255" step="5" value="60"><output id="espSpeedVal" class="demo-only">60</output>
        </div>
        <p id="espNote">Μετά από κάθε εκκίνηση: σταματημένο (εκτός αν είναι ενεργό το «Αυτόματο Start»), η ταινία είναι του WLED. Start: η ταινία δείχνει την TV μέχρι το Stop. Demo (αν είναι ενεργό στη «Συσκευή»): 5 απλά εφέ όσο η TV δεν στέλνει· σβήνει μόνο του όταν η TV αρχίσει να στέλνει.</p>
      </details>
    </div>"""

ESP_SCRIPT = r"""<script>
'use strict';
/* v0.37 ESP glue: Start / Stop / Demo. The monitor script calls window.ambiEsp(state) on every update. */
(()=>{
  const $=id=>document.getElementById(id);
  let last=null,busy=false,synced=false,fxLoaded=false,sendTimer=null,minHeap=1e9;
  const SIMPLE=['Rainbow','Colorloop','Pacifica','Aurora','Colorwaves'];  // palette effects: no colour to set
  async function post(url,body){
    const r=await fetch(url,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
    const j=await r.json().catch(()=>({}));
    if(!r.ok)throw new Error(j.error||('http '+r.status));
    return j;
  }
  // The ESP's web server turns requests away while the page's first burst is running
  // (seen 2026-11-18: an empty reply): one at a time, a little later, with retries.
  async function getJson(url,tries){
    let err;
    for(let i=0;i<tries;i++){
      try{const r=await fetch(url,{cache:'no-store'});if(!r.ok)throw new Error('http '+r.status);return JSON.parse(await r.text());}
      catch(e){err=e;await new Promise(ok=>setTimeout(ok,700+500*i));}
    }
    throw err;
  }
  async function loadEffects(){
    try{
      const names=await getJson('/json/eff',6);
      const sel=$('espFx'),keep=sel.value;
      // only the five simple ones (user 2026-11-18), in this order; ids from this WLED
      for(const n of SIMPLE){
        const i=names.indexOf(n);
        if(i<0)continue;
        const o=document.createElement('option');o.value=String(i);o.textContent=n;sel.appendChild(o);
      }
      sel.value=keep;fxLoaded=true;
    }catch(e){$('espNote').textContent='Η λίστα εφέ του WLED δεν διαβάστηκε ('+e.message+'). Το «Τυχαίο» δουλεύει.';}
  }
  function demoBody(on){return {on,fx:Number($('espFx').value),bri_pct:Number($('espBri').value),speed:Number($('espSpeed').value)};}
  async function service(action){
    busy=true;render();
    try{await post('/ambi/api/service',{action});}
    catch(e){$('espNote').textContent='Δεν έγινε: '+(e.message==='pin_required'?'πάτα «🔒 PIN» πάνω δεξιά.':e.message);}
    finally{busy=false;}
  }
  $('espStart').addEventListener('click',()=>service('start'));
  $('espStop').addEventListener('click',()=>service('stop'));
  $('espDemo').addEventListener('click',async()=>{
    const on=!(last&&last.esp_demo&&last.esp_demo.on);
    busy=true;render();
    try{
      await post('/ambi/api/demo',demoBody(on));
      if(on&&last&&last.service!=='running')await post('/ambi/api/service',{action:'start'});
    }catch(e){$('espNote').textContent='Δεν έγινε: '+(e.message==='pin_required'?'πάτα «🔒 PIN» πάνω δεξιά.':e.message);}
    finally{busy=false;}
  });
  function changed(){
    $('espBriVal').textContent=$('espBri').value+' %';$('espSpeedVal').textContent=$('espSpeed').value;
    clearTimeout(sendTimer);
    sendTimer=setTimeout(()=>{post('/ambi/api/demo',demoBody(Boolean(last&&last.esp_demo&&last.esp_demo.on))).catch(e=>{$('espNote').textContent='Δεν έγινε: '+e.message;});},250);
  }
  for(const id of ['espFx','espBri','espSpeed'])$(id).addEventListener(id==='espFx'?'change':'input',changed);
  // v0.38: RAM and flash in "Τεχνικά δεδομένα"
  const kb=v=>v>=1048576?(v/1048576).toFixed(2)+' MB':Math.round(v/1024)+' KB';
  function bar(id,used,total,warnAt,badAt){
    const el=$(id);if(!el)return;
    const p=total>0?Math.max(0,Math.min(100,100*used/total)):0;
    el.style.width=p+'%';el.className=p>=badAt?'bad':p>=warnAt?'warn':'';
  }
  function renderMem(v){
    if(v.free_heap>0&&v.heap_total>0){
      $('memRam').textContent=`${kb(v.free_heap)} από ${kb(v.heap_total)}`;
      bar('memRamBar',v.heap_total-v.free_heap,v.heap_total,75,88);   // the bar = used
    }
    if(v.heap_min>0)$('memRamMin').textContent=`${kb(v.heap_min)} · ${v.heap_max_block>0?kb(v.heap_max_block):'—'}`;
    if(v.app_size>0&&v.app_slot>0){
      $('memApp').textContent=`${kb(v.app_size)} / ${kb(v.app_slot)} (${Math.round(100*v.app_size/v.app_slot)} %)`;
      bar('memAppBar',v.app_size,v.app_slot,90,97);
    }
    if(v.fs_total>0){
      $('memFs').textContent=`${kb(v.fs_used)} / ${kb(v.fs_total)}`;
      bar('memFsBar',v.fs_used,v.fs_total,80,95);
    }
  }
  function render(){
    const s=last,d=s&&s.esp_demo,running=s&&s.service==='running';
    const tv=s&&s.sources&&s.sources.hue&&s.sources.hue.status==='live';
    $('espStart').disabled=!s||busy||running;
    $('espStop').disabled=!s||busy||!running;
    $('espDemo').disabled=!s||busy;
    $('espDemo').classList.toggle('on',Boolean(d&&d.on));
    $('espDemo').textContent=d&&d.on?'Demo: ON':'Demo';
    const st=$('espState');
    st.className='pill '+(running?'run':'stop');
    st.textContent=!s?'—':!running?'STOP · WLED'
      :tv?'TV':d&&d.showing?'DEMO'+(d.fx_name?' · '+d.fx_name:'')
      :'ΑΝΑΜΟΝΗ TV';
    st.title=!s?'':!running?'Σταματημένο: η ταινία είναι του WLED (εφέ, app). Start για να την πάρει η σελίδα.'
      :tv?'Η ταινία δείχνει τα χρώματα της TV':d&&d.showing?'Η TV δεν στέλνει: εφέ demo του WLED':'Η TV δεν στέλνει: η ταινία μένει σβηστή μέχρι να στείλει (ή Demo)';
    if(s&&s.device&&s.device.free_heap>0){
      const kb=Math.round(s.device.free_heap/1024);minHeap=Math.min(minHeap,kb);
      $('espHeap').textContent=`${kb} KB · min ${minHeap}`;
      $('espHeap').style.color=kb<40?'#ff8f7a':kb<60?'#f3bd73':'';
    }
    if(s&&s.device)renderMem(s.device);
    if(s&&d&&!synced){
      synced=true;
      $('espBri').value=String(d.bri_pct);$('espSpeed').value=String(d.speed);
      $('espBriVal').textContent=d.bri_pct+' %';$('espSpeedVal').textContent=String(d.speed);
      if(fxLoaded&&d.fx>=0)$('espFx').value=String(d.fx);
    }
  }
  // v0.37: brightness of the TV's colours = the strip setting "brightness" (0..1, before the limit)
  let tvBriTimer=null;
  async function loadTvBri(){
    try{
      const j=await getJson('/ambi/api/settings/strip',6);
      const v=Math.round(100*Number(j.settings.brightness));
      if(Number.isFinite(v)){$('espTvBri').value=String(v);$('espTvBriVal').textContent=v+' %';}
    }catch(e){$('espTvBriVal').textContent='?';}
  }
  $('espTvBri').addEventListener('input',()=>{
    $('espTvBriVal').textContent=$('espTvBri').value+' %';
    clearTimeout(tvBriTimer);
    tvBriTimer=setTimeout(()=>{post('/ambi/api/settings/strip',{brightness:Number($('espTvBri').value)/100})
      .catch(e=>{$('espNote').textContent='Δεν έγινε: '+(e.message==='pin_required'?'πάτα «🔒 PIN» πάνω δεξιά.':e.message);});},300);
  });
  try{if(localStorage.getItem('ambi.espMore')==='1')$('espMore').open=true;}catch(e){}
  $('espMore').addEventListener('toggle',()=>{try{localStorage.setItem('ambi.espMore',$('espMore').open?'1':'0');}catch(e){}});
  window.ambiEsp=s=>{last=s;render();};
  setTimeout(async()=>{await loadEffects();await loadTvBri();await loadDev();await loadSec();loadLog();},1200);
  render();
})();
</script>
</body>"""

ESP_DEVICE_PANEL = """<section class="panel identity-panel esp-dev" id="espDevPanel" aria-labelledby="espDevTitle">
      <div class="panel-top"><div><div class="eyebrow">ESP &amp; WLED</div><h2 id="espDevTitle">Συσκευή</h2></div></div>
      <form id="espDevForm" class="id-form esp-dev-form" novalidate>
        <div><label for="devName">Όνομα (όπως το δείχνει η TV)</label>
        <div class="id-row"><input id="devName" type="text" maxlength="32" autocomplete="off" spellcheck="false" disabled></div>
        <span class="hint">Έως 32 λατινικοί χαρακτήρες, χωρίς &lt; &gt; &amp; " '. Είναι και το όνομα του WLED. Αλλαγή = επανεκκίνηση.</span></div>
        <div><label for="devLamps">Λάμπες που δίνουμε στην TV</label>
        <div class="id-row"><select id="devLamps" disabled><option value="3">3</option><option value="5">5</option></select></div>
        <span class="hint">Αλλαγή = επανεκκίνηση και ξανά «Διαμόρφωση» στο Ambilight+hue της TV.</span></div>
        <div><label for="devLength">Σύνολο LED της ταινίας</label>
        <div class="id-row"><input id="devLength" type="number" min="1" max="1000" step="1" disabled></div>
        <span class="hint">Όλα τα LED της ταινίας (το «Length» του WLED). Πόσα ανάβουν: «Πλήθος LED» στην «Ταινία LED &amp; ρεύμα».</span></div>
        <div><label for="devPin">Pin δεδομένων</label>
        <div class="id-row"><select id="devPin" disabled></select></div>
        <span class="hint">Όπου είναι το καλώδιο δεδομένων της ταινίας. Αλλαγή = επανεκκίνηση.</span></div>
        <div><label for="devButton">Κουμπί του Atom</label>
        <div class="id-row"><select id="devButton" disabled><option value="ambi">Pairing (Ambi)</option><option value="wled">WLED (άναμμα / σβήσιμο)</option></select></div>
        <span class="hint" id="devButtonHint">Pairing: σύντομο πάτημα = pairing 30 s, κρατημένο 10 s = ξέχασε την TV.</span></div>
        <div><label for="devTz">Ώρα του ESP</label>
        <div class="id-row"><select id="devTz" disabled><option value="0">UTC</option><option value="1">GMT/BST (UK)</option><option value="2">CET/CEST</option><option value="3">EET/EEST (Ελλάδα, Κύπρος)</option><option value="4">US-EST/EDT</option><option value="5">US-CST/CDT</option><option value="6">US-MST/MDT</option><option value="7">US-AZ</option><option value="8">US-PST/PDT</option><option value="9">CST (AWST, PHST)</option><option value="10">JST (KST)</option><option value="11">AEST/AEDT</option><option value="12">NZST/NZDT</option><option value="13">North Korea</option><option value="14">IST (India)</option><option value="15">CA-Saskatchewan</option><option value="16">ACST</option><option value="17">ACST/ACDT</option><option value="18">HST (Hawaii)</option><option value="19">NOVT (Novosibirsk)</option><option value="20">AKST/AKDT (Anchorage)</option><option value="21">MX-CST</option><option value="22">PKT (Pakistan)</option><option value="23">BRT (Brasília)</option><option value="24">AWST (Perth)</option></select>
          <label class="dev-inline"><input type="checkbox" id="devNtp" disabled> από internet (NTP)</label></div>
        <span class="hint" id="devTime">—</span></div>
        <div class="dev-night"><label for="devEspMa"><strong>Τροφοδοσία:</strong> ρεύμα του ESP από την ίδια USB θύρα με την ταινία</label> <input id="devEspMa" type="number" min="0" max="400" step="10" disabled> mA
          <span class="hint">0 = το ESP έχει δική του θύρα (όπως εδώ: Atom σε μία θύρα της TV, ταινία σε άλλη). Αν μοιράζονται μία θύρα: ~120 για AtomS3. Το «Όριο ρεύματος» μένει το όριο της θύρας· η ταινία παίρνει το όριο μείον αυτό, και το ίδιο μπαίνει στο WLED.</span></div>
        <div class="dev-night"><label class="disc-switch"><input type="checkbox" id="devNightOn" disabled> <span><strong>Νυχτερινό όριο</strong><br>Τη νύχτα τα χρώματα της TV στο <input id="devNightPct" type="number" min="5" max="100" step="5" disabled> % από <select id="devNightFrom" disabled></select> ως <select id="devNightTo" disabled></select>. Θέλει την ώρα του ESP.</span></label></div>
        <div class="dev-switch"><label class="disc-switch"><input type="checkbox" id="devDemo" disabled> <span><strong>Demo</strong><br>Κουμπί «Demo» στην κάρτα «Ταινία»: εφέ του WLED όσο η TV δεν στέλνει (για δοκιμές χωρίς TV). Σβήνει μόνο του όταν η TV αρχίσει να στέλνει.</span></label></div>
        <div class="dev-switch"><label class="disc-switch"><input type="checkbox" id="devAuto" disabled> <span><strong>Αυτόματο Start</strong><br>Μετά από κάθε εκκίνηση (ρεύμα, update) ξεκινά μόνο του, χωρίς Start.</span></label></div>
        <div class="dev-actions"><button id="devSave" type="submit" disabled>Αποθήκευση</button>
          <span class="dev-spacer"></span>
          <button id="devForget" type="button" class="secondary" disabled>Ξέχασε την TV</button>
          <button id="devRestart" type="button" class="secondary" disabled>Επανεκκίνηση ESP</button></div>
      </form>
      <p id="devNote" class="id-note" role="status">Έλεγχος…</p>
      <p class="disclaimer">Ρεύμα σε ένα σημείο: το «Όριο ρεύματος» και τα «mA ανά LED» της «Ταινία LED &amp; ρεύμα» γράφονται και στο WLED (Maximum current, LED current)· αν τα αλλάξεις στο WLED, τα παίρνει η σελίδα. Στο WLED τώρα: <span id="devWledMa">—</span>.</p>
      <details class="esp-log" id="espLog"><summary>Συμβάντα (log) <span id="espLogCount"></span></summary>
        <p class="log-help">Τι έκανε η TV και το ESP (pairing, σύνδεση, επανασυνδέσεις, αλλαγές από τη σελίδα). Ώρα = από την εκκίνηση του ESP· τα νεότερα πάνω.</p>
        <ol id="espLogList"></ol></details>
    </section>
    <section class="panel identity-panel esp-sec" id="espSecPanel" aria-labelledby="espSecTitle">
      <div class="panel-top"><div><div class="eyebrow">ΑΣΦΑΛΕΙΑ</div><h2 id="espSecTitle">Ποιος μπορεί να αλλάξει τι</h2></div><a class="subtle-label" href="/settings/sec" title="WLED: Config → Security &amp; Updates">Ρυθμίσεις ασφαλείας WLED ↗</a></div>
      <ul class="sec-list" id="secList"><li>Έλεγχος…</li></ul>
      <label class="disc-switch"><input type="checkbox" id="secHueOnly" disabled> <span><strong>Hue μόνο από την TV</strong><br><span id="secHueHint">Το bridge απαντά μόνο στη διεύθυνση της TV. Το pairing θέλει πάντα το κουμπί ή το Pair (30 s).</span></span></label>
      <p id="secNote" class="id-note" role="status"></p>
      <p class="disclaimer">Το ESP μιλά απλό HTTP: μέσα στο σπίτι οι κωδικοί περνούν ανοιχτοί. Ποτέ port forwarding στον router για αυτή τη συσκευή. Αν ο router το έχει, βάλε τις «έξυπνες» συσκευές σε χωριστό Wi-Fi (guest / IoT).</p>
    </section>
    <section class="panel identity-panel esp-diag" id="espDiagPanel" aria-labelledby="espDiagTitle">
      <div class="panel-top"><div><div class="eyebrow">ΔΙΑΓΝΩΣΗ</div><h2 id="espDiagTitle">Έλεγχος συστήματος</h2></div>
        <div class="diag-btns"><button type="button" id="diagRun" class="ghost-btn">Έλεγχος τώρα</button><button type="button" id="diagCopy" class="ghost-btn" disabled>Αντιγραφή αναφοράς</button></div></div>
      <div class="diag-sum" id="diagSum"><span><b id="diagOk">–</b> OK</span><span class="w"><b id="diagWarn">–</b> Προσοχή</span><span class="e"><b id="diagErr">–</b> Σφάλμα</span><span class="n"><b id="diagNa">–</b> Δεν ελέγχθηκε</span></div>
      <ul class="sec-list diag-list" id="diagList"><li><span class="na">·</span><div><b>Πάτα «Έλεγχος τώρα»</b><small>Μόνο διαβάζει μετρητές: δεν κάνει pairing, δεν ξεκινά σύνδεση, δεν πειράζει την TV ή την ταινία.</small></div></li></ul>
      <textarea id="diagReport" class="diag-report" readonly hidden></textarea>
      <p class="disclaimer">Δεν μετρά ρεύμα, θερμοκρασία ή κολλήσεις (δεν υπάρχουν αισθητήρες): «όριο 810 mA» σημαίνει μόνο ότι η ρύθμιση είναι σωστή. Η αναφορά δεν περιέχει κωδικούς ή κλειδιά.</p>
    </section>
    <section class="panel identity-panel esp-cap" id="espCapPanel" aria-labelledby="espCapTitle">
      <div class="panel-top"><div><div class="eyebrow">ΔΟΚΙΜΕΣ</div><h2 id="espCapTitle">Καταγραφή Hue</h2></div></div>
      <p class="id-intro">Για δοκιμές με Gaming, HDR, Dolby Vision, αλλαγή πηγής, Lounge: κρατά κάθε καρέ που στέλνει η TV <b>όπως ήρθε</b> (πριν από φωτεινότητα, όριο και εξομάλυνση), μαζί με ό,τι πήγε στην ταινία και τις δύο κάτω γωνίες του JointSpace. Άφησε τη σελίδα ανοιχτή όσο καταγράφει· σταματά μόνη της στα 10 λεπτά.</p>
      <div class="dev-actions"><button type="button" id="capStart">Έναρξη</button><button type="button" id="capStop" class="secondary" disabled>Τέλος</button><span class="dev-spacer"></span><button type="button" id="capCsv" class="secondary" disabled>Λήψη CSV</button></div>
      <p id="capNote" class="id-note" role="status">—</p>
    </section>
    <section class="panel strip-settings" id="stripSettingsPanel\""""

JS_PAIR_HTML = """      <label class="js-on"><input type="checkbox" id="jsOn" disabled> <span><b>Χρήση JointSpace</b> (οι γωνίες της ταινίας από τις πλευρές της TV). Κλειστό = καμία σύνδεση στην TV και ~45 KB περισσότερη ελεύθερη μνήμη όσο παίζει η TV· οι γωνίες τότε από τις λάμπες (με 5 λάμπες ταιριάζουν καλύτερα).</span></label>
      <div class="js-pair" id="jsPairBox">
        <p><b>Pair με την TV:</b> το ESP ζητά από την TV credentials· η TV δείχνει έναν PIN στην οθόνη, τον γράφεις εδώ, και αποθηκεύονται μόνα τους. Ή, αν έχεις ήδη credentials, γράψε τα στη φόρμα από κάτω.</p>
        <div class="id-row"><input id="jsPairIp" type="text" inputmode="decimal" maxlength="15" placeholder="IP της TV (κενό = αυτόματα)" disabled>
        <button type="button" id="jsPairStart" disabled>Pair με την TV</button></div>
        <form id="jsPairForm" class="id-row" hidden><input id="jsPairPin" type="text" inputmode="numeric" pattern="[0-9]*" maxlength="8" autocomplete="one-time-code" placeholder="PIN από την οθόνη της TV">
        <button type="submit" id="jsPairSend">Αποστολή PIN</button></form>
        <p id="jsPairNote" role="status" class="hint"></p>
      </div>
"""

JS_PAIR_SCRIPT = r"""  // v0.45: JointSpace pairing (the TV shows a PIN)
  const jsPairErr={tv_address_unknown:'Δεν ξέρω την IP της TV: γράψ’ την στο πεδίο (ή ξεκίνα πρώτα το Ambilight+hue).',
    invalid_pin:'Λάθος PIN. Γράψε τον PIN που δείχνει τώρα η TV.',pin_timeout:'Πέρασε ο χρόνος· πάτα ξανά «Pair με την TV».',
    concurrent_pairing:'Η TV ήδη κάνει άλλο pairing· περίμενε λίγο και ξαναδοκίμασε.',network_timeout:'Η TV δεν απάντησε (είναι ανοιχτή; σωστή IP;).',
    network_unavailable:'Η TV δεν απάντησε στη θύρα 1926 (είναι ανοιχτή; σωστή IP;).',authentication_failed:'Η TV δεν δέχτηκε το αίτημα.',
    save_failed:'Η TV είπε ναι αλλά δεν αποθηκεύτηκαν στο ESP· ξαναδοκίμασε.',pin_digits:'Ο PIN είναι μόνο ψηφία.',low_memory:'Λίγη ελεύθερη μνήμη στο ESP τώρα (π.χ. όσο παίζει η TV)· ξαναδοκίμασε σε λίγο.',
    no_pairing_waiting:'Δεν περιμένει PIN κανένα pairing· πάτα «Pair με την TV».',pin_required:'Βάλε πρώτα το PIN ρυθμίσεων (🔒 PIN πάνω δεξιά).'};
  let jsPairTimer=null;
  function jsPairShow(j){
    const st=j&&j.state||'idle',note=$('jsPairNote');
    $('jsPairForm').hidden=!(st==='waiting_pin'||st==='granting');
    $('jsPairStart').disabled=st==='requesting'||st==='granting'||$('tvUser').disabled;
    if(st==='requesting')note.textContent='Ρωτώ την TV…';
    else if(st==='waiting_pin')note.textContent=(j.error?(jsPairErr[j.error]||j.error)+' ':'')+`Η TV δείχνει έναν PIN: γράψ’ τον εδώ (απομένουν ${j.timeout_s} s).`;
    else if(st==='granting')note.textContent='Στέλνω τον PIN…';
    else if(st==='done')note.textContent='✓ Έγινε pairing. Τα credentials αποθηκεύτηκαν στο ESP· η κατάσταση JointSpace δείχνει τη σύνδεση όσο η TV στέλνει Hue.';
    else if(st==='failed')note.textContent='✗ '+(jsPairErr[j.error]||j.error||'Απέτυχε.');
    else note.textContent='';
    const busy=st==='requesting'||st==='waiting_pin'||st==='granting';
    clearTimeout(jsPairTimer);
    if(busy)jsPairTimer=setTimeout(jsPairPoll,1000);
    else if(st==='done')loadSettings();
  }
  async function jsPairPoll(){try{const r=await fetch('/ambi/api/jointspace/pair',{cache:'no-store'});jsPairShow(await r.json());}catch{jsPairTimer=setTimeout(jsPairPoll,2000);}}
  async function jsPairPost(path,body){
    const r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
    let j={};try{j=await r.json();}catch{}
    if(!r.ok){$('jsPairNote').textContent='✗ '+(jsPairErr[j.error]||(r.status===401?jsPairErr.pin_required:j.error)||('HTTP '+r.status));return;}
    jsPairShow(j);
  }
  $('jsPairStart').addEventListener('click',()=>jsPairPost('/ambi/api/jointspace/pair',{tv_ip:$('jsPairIp').value.trim()}));
  $('jsPairForm').addEventListener('submit',e=>{e.preventDefault();const pin=$('jsPairPin').value.trim();$('jsPairPin').value='';jsPairPost('/ambi/api/jointspace/pair/pin',{pin});});
  // the pair controls follow the form's edit right (PIN)
  new MutationObserver(()=>{$('jsPairStart').disabled=$('tvUser').disabled;$('jsPairIp').disabled=$('tvUser').disabled;}).observe($('tvUser'),{attributes:true,attributeFilter:['disabled']});
  jsPairPoll();
  // v0.48: JointSpace on / off
  async function jsOnLoad(){try{const r=await fetch('/ambi/api/settings/jointspace',{cache:'no-store'});const j=await r.json();$('jsOn').checked=j.enabled!==false;$('jsOn').disabled=!j.can_edit;}catch(e){}}
  $('jsOn').addEventListener('change',async()=>{
    try{const r=await fetch('/ambi/api/settings/jointspace',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({enabled:$('jsOn').checked})});
      if(!r.ok)throw new Error(r.status===401?'pin':'http');$('jsPairNote').textContent=$('jsOn').checked?'JointSpace: ανοιχτό.':'JointSpace: κλειστό.';}
    catch(e){$('jsOn').checked=!$('jsOn').checked;$('jsPairNote').textContent=e.message==='pin'?jsPairErr.pin_required:'✗ Δεν αποθηκεύτηκε.';}
  });
  setTimeout(jsOnLoad,1600);
"""

ESP_CAM_PANEL = """<section class="panel identity-panel esp-cam" id="espCamPanel" aria-labelledby="espCamTitle">
      <div class="panel-top"><div><div class="eyebrow">ΧΡΩΜΑ</div><h2 id="espCamTitle">Βαθμονόμηση με κάμερα</h2></div></div>
      <p class="hint">Στρέψε το κινητό στον τοίχο ώστε να φαίνεται η λάμψη στο <b>πλάι</b> και <b>κάτω</b> από την TV, και σύρε τα δύο κουτάκια πάνω τους (σημεία του τοίχου με το ίδιο χρώμα). Η σελίδα συγκρίνει μόνο την απόχρωση και διορθώνει την «Ισορροπία λευκού» μέχρι να ταιριάξουν. Μετρά μόνο όσο η TV δείχνει λευκό / γκρι. Η ζωντανή κάμερα θέλει https· αλλιώς «Φωτογραφία».</p>
      <div class="cam-actions">
        <button type="button" id="camStart">Κάμερα</button>
        <label class="cam-file">Φωτογραφία<input type="file" id="camFile" accept="image/*" capture="environment" hidden></label>
        <button type="button" id="camAuto" disabled>Αυτόματη διόρθωση</button>
        <button type="button" id="camUndo" class="secondary" disabled>Αναίρεση</button>
        <button type="button" id="camStop" class="secondary" disabled>Τέλος</button>
      </div>
      <div class="cam-wrap"><video id="camVideo" playsinline muted hidden></video><canvas id="camCanvas" width="640" height="360" aria-label="Εικόνα κάμερας με τα δύο κουτάκια"></canvas></div>
      <div class="cam-read">
        <span><i class="cam-sw" id="camSwSide"></i>Πλάι <b id="camSide">—</b></span>
        <span><i class="cam-sw" id="camSwBottom"></i>Κάτω <b id="camBottom">—</b></span>
        <span>Διαφορά <b id="camDiff">—</b></span>
        <span>TV <b id="camGate">—</b></span>
      </div>
      <label class="cam-manual"><input type="checkbox" id="camManual"> Η TV δείχνει λευκό / γκρι (όταν το JointSpace είναι κλειστό)</label>
      <p id="camNote" class="hint" role="status"></p>
      <div class="wb-slots" id="wbSlots">
        <div class="eyebrow">ΘΕΣΕΙΣ ΜΝΗΜΗΣ · ΙΣΟΡΡΟΠΙΑ ΛΕΥΚΟΥ</div>
        <p class="hint">Κράτα έως 3 βαθμονομήσεις στο ESP (π.χ. μέρα / βράδυ). «Αποθήκευση» = η ισορροπία λευκού που ισχύει τώρα· «Φόρτωση» = την εφαρμόζει.</p>
        <div class="wb-slot" data-n="1"><input class="wb-slot-name" maxlength="24" placeholder="Θέση 1" aria-label="Όνομα θέσης 1" disabled><span class="wb-slot-val">—</span><button type="button" class="wb-slot-save secondary" disabled>Αποθήκευση</button><button type="button" class="wb-slot-load" disabled>Φόρτωση</button><button type="button" class="wb-slot-clear secondary" aria-label="Άδειασμα θέσης 1" disabled>×</button></div>
        <div class="wb-slot" data-n="2"><input class="wb-slot-name" maxlength="24" placeholder="Θέση 2" aria-label="Όνομα θέσης 2" disabled><span class="wb-slot-val">—</span><button type="button" class="wb-slot-save secondary" disabled>Αποθήκευση</button><button type="button" class="wb-slot-load" disabled>Φόρτωση</button><button type="button" class="wb-slot-clear secondary" aria-label="Άδειασμα θέσης 2" disabled>×</button></div>
        <div class="wb-slot" data-n="3"><input class="wb-slot-name" maxlength="24" placeholder="Θέση 3" aria-label="Όνομα θέσης 3" disabled><span class="wb-slot-val">—</span><button type="button" class="wb-slot-save secondary" disabled>Αποθήκευση</button><button type="button" class="wb-slot-load" disabled>Φόρτωση</button><button type="button" class="wb-slot-clear secondary" aria-label="Άδειασμα θέσης 3" disabled>×</button></div>
        <p id="wbSlotNote" class="hint" role="status"></p>
      </div>
    </section>"""

ESP_CAM_SCRIPT = r"""
  // v0.49: colour calibration with the camera (no AI): two boxes, mean colour in linear light,
  // chromaticity only, a damped step on the white balance until side and bottom agree
  const cam={stream:null,img:null,auto:false,busy:false,used:null,vt:-1,steps:0,undo:null,timer:null,boxes:{side:{x:.12,y:.45,w:.14,h:.18},bottom:{x:.43,y:.78,w:.14,h:.14}},drag:null,last:null,ok:0,wb:null};
  const lin=v=>{v/=255;return v<=0.04045?v/12.92:Math.pow((v+0.055)/1.055,2.4);};
  function camMean(ctx,b,W,H){
    const x=Math.max(0,Math.round(b.x*W)),y=Math.max(0,Math.round(b.y*H)),w=Math.max(2,Math.round(b.w*W)),h=Math.max(2,Math.round(b.h*H));
    const d=ctx.getImageData(x,y,Math.min(w,W-x),Math.min(h,H-y)).data;let r=0,g=0,bl=0,n=0;
    for(let i=0;i<d.length;i+=4){if(d[i]>=250||d[i+1]>=250||d[i+2]>=250)continue;r+=lin(d[i]);g+=lin(d[i+1]);bl+=lin(d[i+2]);n++;}
    if(n<20)return null;r/=n;g/=n;bl/=n;const s=r+g+bl;if(s<0.004)return null;return{c:[r/s,g/s,bl/s],raw:[r,g,bl]};
  }
  function camDraw(){
    const cv=$('camCanvas'),ctx=cv.getContext('2d',{willReadFrequently:true});const src=cam.stream?$('camVideo'):cam.img;if(!src)return;
    const sw=src.videoWidth||src.naturalWidth,sh=src.videoHeight||src.naturalHeight;if(!sw||!sh)return;
    cv.width=640;cv.height=Math.round(640*sh/sw);ctx.drawImage(src,0,0,cv.width,cv.height);
    const side=camMean(ctx,cam.boxes.side,cv.width,cv.height),bot=camMean(ctx,cam.boxes.bottom,cv.width,cv.height);
    for(const [k,col] of [['side','#53d1d4'],['bottom','#f2ae65']]){const b=cam.boxes[k];ctx.strokeStyle=col;ctx.lineWidth=3;ctx.strokeRect(b.x*cv.width,b.y*cv.height,b.w*cv.width,b.h*cv.height);}
    const pct=m=>m?m.c.map(v=>Math.round(v*100)).join(' / ')+' %':'—';
    const sw8=m=>m?'rgb('+m.raw.map(v=>Math.round(255*Math.pow(v/Math.max(...m.raw),1/2.2))).join(',')+')':'transparent';
    $('camSide').textContent=pct(side);$('camBottom').textContent=pct(bot);$('camSwSide').style.background=sw8(side);$('camSwBottom').style.background=sw8(bot);
    cam.last=side&&bot?{side,bot,diff:Math.max(...side.c.map((v,i)=>Math.abs(v-bot.c[i])/v))}:null;
    $('camDiff').textContent=cam.last?Math.round(cam.last.diff*100)+' %':'—';
  }
  function camGate(){  // the TV shows a neutral, bright picture on all three sides (JointSpace), or the user says so
    if($('camManual').checked)return true;
    const s=last&&last.sides;if(!s||!s.left||!s.left.length)return false;
    const all=[...s.left,...s.top,...s.right].map(c=>Array.isArray(c)?c:[c.r,c.g,c.b]);
    const m=[0,1,2].map(i=>all.reduce((a,c)=>a+c[i],0)/all.length),mx=Math.max(...m),mn=Math.min(...m);
    return mx>=20&&(mx-mn)/mx<0.12&&all.every(c=>Math.abs(c[0]-m[0])<mx*0.25&&Math.abs(c[1]-m[1])<mx*0.25&&Math.abs(c[2]-m[2])<mx*0.25);
  }
  function camAutoOff(){cam.auto=false;$('camAuto').textContent='Αυτόματη διόρθωση';}
  function camErr(e){const m=e&&e.message;return '✗ '+(m==='http 401'||m==='pin_required'?'Βάλε πρώτα το PIN (🔒 PIN).':m);}
  /* v0.49.1: a photo never changes - stop and say why instead of waiting for ever */
  function camWait(msg){$('camNote').textContent=msg;if(!cam.stream)camAutoOff();}
  async function camStep(){
    camDraw();const g=camGate();$('camGate').textContent=g?'ουδέτερο ✓':'περιμένω λευκό / γκρι';
    if(!cam.auto||cam.busy)return;  /* never two steps at once (a slow save let a 2nd step reuse the same photo) */
    if(!g){const js=last&&last.sources&&last.sources.jointspace&&last.sources.jointspace.status==='live';
      camWait(js?'Περιμένω: η TV να δείξει λευκό ή γκρι.':'Το JointSpace δεν τρέχει: όταν η TV δείχνει λευκό / γκρι, τσέκαρε το «Η TV δείχνει λευκό / γκρι».');return;}
    if(!cam.last){camWait('Περιμένω: να φαίνονται και τα δύο κουτάκια (όχι καμένα).');return;}
    if(!cam.stream&&cam.used===cam.img){camWait('Αυτή η φωτογραφία χρησιμοποιήθηκε ήδη — βγάλε νέα.');return;}
    /* v0.49.2: live camera - a frame is used once too (a frozen video in a hidden tab compounded the same step) */
    if(cam.stream){const v=$('camVideo');if(document.hidden||v.paused||v.readyState<2||v.currentTime===cam.vt){camWait('Περιμένω: νέα εικόνα από την κάμερα.');return;}cam.vt=v.currentTime;}
    cam.busy=true;
    try{
      if(!cam.wb){const j=await getJson('/ambi/api/settings/strip',2);cam.wb=[j.settings.wb_r,j.settings.wb_g,j.settings.wb_b];if(!cam.undo)cam.undo=cam.wb.slice();}
      if(cam.last.diff<0.025){if(!cam.stream||++cam.ok>=3){camAutoOff();$('camNote').textContent='✓ Ταιριάζουν (διαφορά '+Math.round(cam.last.diff*100)+' %). Ισορροπία λευκού: '+cam.wb.join(' / ');}return;}
      cam.ok=0;
      if(++cam.steps>40){camAutoOff();$('camNote').textContent='✗ 40 βήματα χωρίς να ταιριάξουν: έλεγξε τα κουτάκια και το «Χρώμα τοίχου» της TV.';return;}
      /* damped (^0.6); against the strongest channel a step lowers a channel by at most a third, photo or camera */
      const k0=cam.last.side.c.map((v,i)=>Math.pow(v/cam.last.bot.c[i],0.6)),km=Math.max(...k0),k=k0.map(v=>Math.max(0.67,v/km));
      let w=cam.wb.map((v,i)=>v*k[i]);const mx=Math.max(...w);
      w=w.map(v=>Math.round(Math.max(0.2,Math.min(1,v/mx))*100)/100);
      await post('/ambi/api/settings/strip',{wb_r:w[0],wb_g:w[1],wb_b:w[2]});
      cam.wb=w;if(!cam.stream)cam.used=cam.img;$('camUndo').disabled=false;
      if(window.ambiLoadStrip)window.ambiLoadStrip(false);  /* the white balance fields show the new values */
      $('camNote').textContent='Βήμα: '+w.join(' / ')+' (διαφορά '+Math.round(cam.last.diff*100)+' %)'+(w.some(v=>v<=0.2)?' ⚠ Ένα κανάλι έφτασε στο κατώτατο 0.2: έλεγξε τα κουτάκια και το «Χρώμα τοίχου» της TV.':'');
      if(!cam.stream){camAutoOff();$('camNote').textContent+=' — βγάλε νέα φωτογραφία για έλεγχο.';}
    }catch(e){camAutoOff();$('camNote').textContent=camErr(e);}
    finally{cam.busy=false;}
  }
  function camRun(){clearInterval(cam.timer);cam.timer=cam.stream?setInterval(camStep,1500):null;$('camAuto').disabled=false;$('camStop').disabled=false;camStep();}
  $('camStart').addEventListener('click',async()=>{
    if(!navigator.mediaDevices||!window.isSecureContext){$('camNote').textContent='Η ζωντανή κάμερα θέλει https (π.χ. μέσω reverse proxy). Χρησιμοποίησε «Φωτογραφία».';return;}
    try{cam.stream=await navigator.mediaDevices.getUserMedia({video:{facingMode:'environment',width:{ideal:1280}},audio:false});const v=$('camVideo');v.srcObject=cam.stream;await v.play();cam.img=null;camRun();}
    catch(e){$('camNote').textContent='✗ Κάμερα: '+e.message;}
  });
  $('camFile').addEventListener('change',e=>{const f=e.target.files&&e.target.files[0];e.target.value='';if(!f)return;const im=new Image();im.onload=()=>{cam.img=im;camAutoOff();$('camNote').textContent='';camRun();};im.src=URL.createObjectURL(f);});
  $('camAuto').addEventListener('click',()=>{if(cam.auto){camAutoOff();$('camNote').textContent='';return;}cam.auto=true;cam.ok=0;cam.steps=0;cam.vt=-1;cam.wb=null;$('camAuto').textContent='Παύση διόρθωσης';camStep();});
  $('camUndo').addEventListener('click',async()=>{if(!cam.undo)return;camAutoOff();
    try{await post('/ambi/api/settings/strip',{wb_r:cam.undo[0],wb_g:cam.undo[1],wb_b:cam.undo[2]});$('camNote').textContent='Αναίρεση: ισορροπία λευκού '+cam.undo.join(' / ')+'.';
      cam.wb=null;cam.undo=null;cam.used=null;$('camUndo').disabled=true;if(window.ambiLoadStrip)window.ambiLoadStrip(false);}
    catch(e){$('camNote').textContent=camErr(e);}});
  $('camStop').addEventListener('click',()=>{camAutoOff();clearInterval(cam.timer);if(cam.stream)cam.stream.getTracks().forEach(t=>t.stop());cam.stream=null;$('camAuto').disabled=true;$('camStop').disabled=true;$('camNote').textContent='';});
  // v0.50: three named white-balance slots, kept on the board (NVS)
  function wbSlotsShow(j){
    document.querySelectorAll('#wbSlots .wb-slot').forEach((row,i)=>{const s=(j.slots&&j.slots[i])||{empty:true},nm=row.querySelector('.wb-slot-name');
      if(document.activeElement!==nm)nm.value=s.empty?'':(s.name||'');
      row.querySelector('.wb-slot-val').textContent=s.empty?'κενή':s.wb.join(' / ');
      nm.disabled=!j.can_edit;row.querySelector('.wb-slot-save').disabled=!j.can_edit;
      row.querySelector('.wb-slot-load').disabled=!j.can_edit||!!s.empty;row.querySelector('.wb-slot-clear').disabled=!j.can_edit||!!s.empty;});
  }
  async function wbSlotsLoad(){try{wbSlotsShow(await getJson('/ambi/api/settings/wb_slots',3));}catch(e){}}
  document.querySelectorAll('#wbSlots .wb-slot').forEach((row,i)=>{const n=i+1;
    const act=async action=>{const nm=row.querySelector('.wb-slot-name').value.trim()||('Θέση '+n);
      try{const j=await post('/ambi/api/settings/wb_slots',{slot:n,action,name:nm});wbSlotsShow(j);
        $('wbSlotNote').textContent=action==='save'?'✓ Αποθηκεύτηκε στη θέση '+n+'.':action==='load'?'✓ Φορτώθηκε η θέση '+n+'.':'Η θέση '+n+' άδειασε.';
        if(action==='load'){cam.wb=null;if(window.ambiLoadStrip)window.ambiLoadStrip(true);}}
      catch(e){$('wbSlotNote').textContent=e.message==='name_1_24_plain'?'Όνομα: 1–24 χαρακτήρες, χωρίς σύμβολα HTML.':camErr(e);}};
    row.querySelector('.wb-slot-save').addEventListener('click',()=>act('save'));
    row.querySelector('.wb-slot-load').addEventListener('click',()=>act('load'));
    row.querySelector('.wb-slot-clear').addEventListener('click',()=>{if(confirm('Να αδειάσει η θέση N;'.replace('N',n)))act('clear');});});
  setTimeout(wbSlotsLoad,2200);
  {const cv=$('camCanvas');const at=e=>{const r=cv.getBoundingClientRect();return{x:(e.clientX-r.left)/r.width,y:(e.clientY-r.top)/r.height};};
    cv.addEventListener('pointerdown',e=>{const p=at(e);let best=null,bd=9;for(const k of ['side','bottom']){const b=cam.boxes[k],d=Math.hypot(p.x-(b.x+b.w/2),p.y-(b.y+b.h/2));if(d<bd){bd=d;best=k;}}cam.drag=best;cv.setPointerCapture(e.pointerId);e.preventDefault();});
    cv.addEventListener('pointermove',e=>{if(!cam.drag)return;const p=at(e),b=cam.boxes[cam.drag];b.x=Math.max(0,Math.min(1-b.w,p.x-b.w/2));b.y=Math.max(0,Math.min(1-b.h,p.y-b.h/2));camDraw();});
    cv.addEventListener('pointerup',()=>{cam.drag=null;});}
"""

ESP_DEVICE_SCRIPT = r"""
  // v0.40: "ESP & WLED" (Length, pin, lamps, auto Start), forget the TV, restart, the event log
  let dev=null;
  const devErr={night_pct_5_100:'Νυχτερινό όριο: 5–100 %.',esp_ma_0_400:'Ρεύμα ESP: 0–400 mA.',night_hour_0_23:'Ώρα: 0–23.',tz_out_of_range:'Ζώνη ώρας: άκυρη.',wled_length_out_of_range:'Σύνολο LED: 1–1000.',pin_not_allowed:'Αυτό το pin δεν επιτρέπεται.',
    name_1_to_32_plain:'Όνομα: 1–32 λατινικοί χαρακτήρες, χωρίς < > & " \'.',button_ambi_or_wled:'Κουμπί: Pairing ή WLED.',
    pin_in_use:'Αυτό το pin το χρησιμοποιεί κάτι άλλο.',lamps_3_or_5:'Λάμπες: 3 ή 5.',pin_required:'Βάλε πρώτα το PIN ρυθμίσεων στο WLED.'};
  function renderDev(){
    if(!dev)return;
    $('outTargetLabel').textContent='Έξοδος: '+(dev.chip||'ESP')+' · '+(dev.name||'WLED');  // v0.49
    if(dev.page_el===false)document.querySelectorAll('.lang-link').forEach(a=>a.hidden=true);  // v0.48: English-only build
    $('devLength').value=String(dev.wled_length);$('devLength').max=String(dev.max_length);
    const pins=dev.pins.includes(dev.pin)?dev.pins:[dev.pin].concat(dev.pins);
    $('devPin').innerHTML=pins.map(p=>`<option value="${p}">G${p}</option>`).join('');
    $('devPin').value=String(dev.pin);
    $('devLamps').value=String(dev.lamps);$('devAuto').checked=Boolean(dev.auto_start);
    $('devName').value=dev.name||'';
    $('devDemo').checked=Boolean(dev.demo_enabled);
    if(!$('devNightFrom').options.length)for(const id of ['devNightFrom','devNightTo'])$(id).innerHTML=[...Array(24).keys()].map(h=>`<option value="${h}">${String(h).padStart(2,'0')}:00</option>`).join('');
    $('devNightOn').checked=Boolean(dev.night_on);$('devNightPct').value=String(dev.night_pct??40);
    $('devEspMa').value=String(dev.esp_ma??0);  // v0.49
    $('devNightFrom').value=String(dev.night_from??23);$('devNightTo').value=String(dev.night_to??7);
    $('espPanel').classList.toggle('no-demo',!dev.demo_enabled);
    $('espMoreTitle').textContent=dev.demo_enabled?'Start / Stop & Demo':'Start / Stop';
    $('pinPill').hidden=Boolean(dev.can_edit);
    if(!$('devTz').querySelector(`option[value="${dev.tz}"]`))$('devTz').insertAdjacentHTML('beforeend',`<option value="${dev.tz}">Ζώνη WLED #${dev.tz}</option>`);
    $('devTz').value=String(dev.tz);$('devNtp').checked=Boolean(dev.ntp);
    $('devTime').textContent=dev.time_ok?`Τώρα στο ESP: ${dev.time}. Τη χρειάζεται η «Καταπόνηση LED» (μέρες, επεισόδια).`
      :'Το ESP δεν ξέρει την ώρα ακόμα: την παίρνει από αυτή τη σελίδα όταν ανοίξει, ή από το internet με NTP (μένει και μετά από εκκίνηση).';
    $('devButton').value=dev.button==='ambi'?'ambi':'wled';
    $('devButtonHint').textContent=(dev.button==='ambi'?'Τώρα: pairing. Σύντομο πάτημα = pairing 30 s, κρατημένο 10 s = ξέχασε την TV.'
      :dev.button==='wled'?'Τώρα: του WLED (άναμμα / σβήσιμο, presets). Pairing μόνο από τη σελίδα (Pair 30 s).'
      :'Τώρα: κανείς. Διάλεξε ποιος το παίρνει.')+' (G'+dev.button_pin+')';
    $('devWledMa').textContent=dev.wled_ma_max>0?`όριο ${dev.wled_ma_max} mA, ${dev.wled_ma_per_led} mA ανά LED`:'όριο ρεύματος ΚΛΕΙΣΤΟ';
    for(const id of ['devName','devButton','devTz','devNtp','devLength','devPin','devLamps','devDemo','devAuto','devNightOn','devNightPct','devNightFrom','devNightTo','devEspMa','devSave','devForget','devRestart'])$(id).disabled=!dev.can_edit;
    $('devNote').textContent=dev.can_edit?'':'Μόνο προβολή: πάτα «🔒 PIN» πάνω δεξιά.';
  }
  async function loadDev(){
    try{
      dev=await getJson('/ambi/api/settings/device',4);renderDev();
      // v0.41: the ESP does not know the time (no NTP): give it this browser's clock, as WLED's own page does
      if(!dev.time_ok){
        await fetch('/json/state',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({time:Math.floor(Date.now()/1000)})}).catch(()=>{});
        setTimeout(async()=>{try{dev=await getJson('/ambi/api/settings/device',3);renderDev();}catch(e){}},1500);
      }
    }
    catch(e){$('devNote').textContent='Δεν διαβάστηκαν ('+e.message+').';}
  }
  function waitBack(note){
    $('devNote').textContent=note;
    const t0=Date.now();
    const t=setInterval(async()=>{
      const secs=(Date.now()-t0)/1000;
      try{const r=await fetch('/ambi/api/settings/device',{cache:'no-store'});
        if(r.ok){const j=await r.json();if(j.uptime_s<=secs+2){clearInterval(t);dev=j;renderDev();$('devNote').textContent='Το ESP ξεκίνησε ξανά.';return;}}}catch(e){}
      if(secs>120){clearInterval(t);$('devNote').textContent='Το ESP δεν απάντησε ακόμα: ανανέωσε τη σελίδα σε λίγο.';}
    },2000);
  }
  $('espDevForm').addEventListener('submit',async e=>{
    e.preventDefault();if(!dev)return;
    const body={},len=Number($('devLength').value),pin=Number($('devPin').value),lamps=Number($('devLamps').value),auto=$('devAuto').checked;
    if(len!==dev.wled_length)body.wled_length=len;
    if(pin!==dev.pin)body.pin=pin;
    if(lamps!==dev.lamps)body.lamps=lamps;
    if(auto!==Boolean(dev.auto_start))body.auto_start=auto;
    const name=$('devName').value.trim(),button=$('devButton').value;
    if(name!==(dev.name||''))body.name=name;
    if(button!==dev.button)body.button=button;
    const nOn=$('devNightOn').checked,nPct=Number($('devNightPct').value),nFrom=Number($('devNightFrom').value),nTo=Number($('devNightTo').value);
    if(nOn!==Boolean(dev.night_on))body.night_on=nOn;
    if(nPct!==dev.night_pct)body.night_pct=nPct;
    if(nFrom!==dev.night_from)body.night_from=nFrom;
    if(nTo!==dev.night_to)body.night_to=nTo;
    const espMa=Number($('devEspMa').value);  // v0.49
    if(espMa!==Number(dev.esp_ma||0))body.esp_ma=espMa;
    const demoOn=$('devDemo').checked;
    if(demoOn!==Boolean(dev.demo_enabled))body.demo_enabled=demoOn;
    const tz=Number($('devTz').value),ntp=$('devNtp').checked;
    if(tz!==dev.tz)body.tz=tz;
    if(ntp!==Boolean(dev.ntp))body.ntp=ntp;
    if(!Object.keys(body).length){$('devNote').textContent='Καμία αλλαγή.';return;}
    if(body.lamps!==undefined&&!confirm('Αλλαγή λαμπών: το ESP θα κάνει επανεκκίνηση και η TV θέλει ξανά «Διαμόρφωση» στο Ambilight+hue. Συνέχεια;'))return;
    if(body.lamps===undefined&&(body.pin!==undefined||body.name!==undefined)&&!confirm('Αλλαγή '+(body.pin!==undefined?'pin':'ονόματος')+': το ESP θα κάνει επανεκκίνηση. Συνέχεια;'))return;
    try{
      const j=await post('/ambi/api/settings/device',body);
      if(j.restart)waitBack('Αποθηκεύτηκε. Επανεκκίνηση…');
      else{$('devNote').textContent='Αποθηκεύτηκε.';setTimeout(loadDev,1500);}
    }catch(err){$('devNote').textContent='Δεν έγινε: '+(devErr[err.message]||err.message);}
  });
  $('devForget').addEventListener('click',async()=>{
    if(!confirm('Να ξεχάσει την TV; Για να ξαναπαίξει: Pair (30 s) και «Διαμόρφωση» στο Ambilight+hue της TV.'))return;
    try{await post('/ambi/api/pair/forget',{});$('devNote').textContent='Η TV ξεχάστηκε. Pair + «Διαμόρφωση» στην TV για να ξανασυνδεθεί.';}
    catch(e){$('devNote').textContent='Δεν έγινε: '+(devErr[e.message]||e.message);}
  });
  $('devRestart').addEventListener('click',async()=>{
    if(!confirm('Επανεκκίνηση του ESP; Η ταινία σβήνει για ~10 s'+(dev&&dev.auto_start?'.':' και μετά μένει σταματημένη (Start).')))return;
    try{await post('/ambi/api/restart',{});waitBack('Επανεκκίνηση…');}
    catch(e){$('devNote').textContent='Δεν έγινε: '+(devErr[e.message]||e.message);}
  });
  // v0.42: security card
  let sec=null;
  function renderSec(){
    if(!sec)return;
    const item=(ok,title,how)=>`<li><span class="${ok?'ok':'warn'}">${ok?'✓':'!'}</span><div><b>${title}</b><small>${how}</small></div></li>`;
    $('secList').innerHTML=[
      item(sec.pin,sec.pin?'PIN ρυθμίσεων: ναι':'PIN ρυθμίσεων: όχι',sec.pin?'Χωρίς το PIN κανείς δεν αλλάζει ρυθμίσεις, Start/Stop ή pairing από αυτή τη σελίδα ή το WLED.'
        :'Όποιος είναι στο Wi-Fi σου μπορεί να αλλάξει τα πάντα. WLED → Config → Security &amp; Updates → Settings PIN (4 ψηφία).'),
      item(sec.ota_lock,sec.ota_lock?'Κλείδωμα update (OTA lock): ναι':'Κλείδωμα update (OTA lock): όχι',sec.ota_lock?'Νέο firmware μόνο με τον κωδικό σου.'
        :'Όποιος είναι στο Wi-Fi σου μπορεί να ανεβάσει firmware. Ίδια σελίδα WLED → «Enable OTA lock» με κωδικό. (Τότε και τα δικά μου updates θα θέλουν τον κωδικό.)'),
      item(!sec.ap_default_pass,sec.ap_default_pass?'Wi-Fi του WLED: εργοστασιακός κωδικός':'Wi-Fi του WLED: δικός σου κωδικός',
        sec.ap_default_pass?'Όταν το ESP δεν βρίσκει το Wi-Fi σου ανοίγει δικό του δίκτυο με κωδικό «wled1234», που τον ξέρουν όλοι. WLED → Config → WiFi Setup → AP password.'
        :'Το δίκτυο που ανοίγει όταν δεν βρίσκει το Wi-Fi σου έχει δικό σου κωδικό.'),
      item(!sec.arduino_ota,sec.arduino_ota?'ArduinoOTA: ανοιχτό':'ArduinoOTA: κλειστό',sec.arduino_ota?'Δεύτερος δρόμος για firmware (από προγραμματιστικά εργαλεία). Αν δεν το χρησιμοποιείς: Security &amp; Updates → κλείσ’ το.'
        :'Firmware ανεβαίνει μόνο από τη σελίδα update του WLED.'),
      item(Boolean(sec.hue_only_ip),sec.hue_only_ip?`Hue: μόνο η TV (${sec.hue_only_ip})`:'Hue: όλο το δίκτυο',sec.hue_only_ip?'Άλλες συσκευές δεν μπορούν να μιλήσουν στο bridge.'
        :'Οποιαδήποτε συσκευή βλέπει το bridge· για pairing χρειάζεται πάντα το κουμπί / Pair. Κλείδωσέ το παρακάτω αν η TV έχει σταθερή IP.'),
    ].join('');
    $('secHueOnly').checked=Boolean(sec.hue_only_ip);
    $('secHueOnly').disabled=!sec.can_edit||(!sec.hue_only_ip&&!sec.tv_ip_seen);
    $('secHueHint').textContent=sec.tv_ip_seen?`Το bridge απαντά μόνο στην TV (${sec.tv_ip_seen}). Θέλει σταθερή IP για την TV στον router (DHCP reservation): αν αλλάξει, η TV δεν θα το βρίσκει μέχρι να το ξεκλειδώσεις εδώ.`
      :'Θέλει πρώτα η TV να έχει στείλει εικόνα από την εκκίνηση (για να ξέρει το ESP τη διεύθυνσή της).';
    $('secNote').textContent=sec.can_edit?'':'Αλλαγές: πάτα «🔒 PIN» πάνω δεξιά.';
  }
  // v0.43: unlock with the WLED settings PIN from this page (WLED keeps it 15 min after the last change)
  // v0.44 (user: the PIN showed in clear): our own dialog with a password field, not prompt()
  $('pinPill').addEventListener('click',()=>{$('pinErr').textContent='';$('pinInput').value='';$('pinDialog').showModal();setTimeout(()=>$('pinInput').focus(),30);});
  $('pinCancel').addEventListener('click',()=>$('pinDialog').close());
  $('pinForm').addEventListener('submit',async e=>{
    e.preventDefault();
    const pin=$('pinInput').value.trim();
    if(!/^\d{4}$/.test(pin)){$('pinErr').textContent='4 ψηφία.';return;}
    $('pinOk').disabled=true;
    try{await fetch('/json/state',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({pin})});}catch(err){}
    $('pinInput').value='';
    try{dev=await getJson('/ambi/api/settings/device',3);}catch(err){}
    $('pinOk').disabled=false;
    if(dev&&dev.can_edit){$('pinDialog').close();location.reload();}
    else $('pinErr').textContent='Λάθος PIN (ή πολλές προσπάθειες: περίμενε λίγα δευτερόλεπτα).';
  });
  async function loadSec(){try{sec=await getJson('/ambi/api/security',3);renderSec();}catch(e){$('secList').innerHTML='<li>Δεν διαβάστηκε ('+esc(e.message)+').</li>';}}
  $('secHueOnly').addEventListener('change',async()=>{
    const on=$('secHueOnly').checked;
    if(on&&!confirm('Το bridge θα απαντά μόνο στην TV ('+(sec&&sec.tv_ip_seen)+'). Αν αλλάξει η IP της TV, δεν θα το βρίσκει μέχρι να το ξεκλειδώσεις εδώ. Συνέχεια;')){$('secHueOnly').checked=false;return;}
    try{sec=await post('/ambi/api/security',{hue_only_tv:on});renderSec();$('secNote').textContent='Αποθηκεύτηκε.';}
    catch(e){$('secHueOnly').checked=!on;$('secNote').textContent='Δεν έγινε: '+(e.message==='tv_not_seen'?'η TV δεν έχει στείλει ακόμα εικόνα.':e.message==='pin_required'?'πάτα «🔒 PIN» πάνω δεξιά.':e.message);}
  });
  // v0.44: «Διάγνωση» - reads counters only (never starts a connection, pairing or restart)
  let diagText='';
  const kbS=v=>v>=0?Math.round(v/1024)+' KB':'—';
  const dur=s=>s<0?'—':s<90?Math.round(s)+' s':s<5400?Math.round(s/60)+' λεπ.':(s/3600).toFixed(1)+' ώρ.';
  const resetText={power_on:'άναμμα',external:'κουμπί reset',software:'επανεκκίνηση από λογισμικό (update, σελίδα)',panic:'ΚΡΑΣΑΡΙΣΜΑ (panic)',
    interrupt_watchdog:'watchdog (interrupt)',task_watchdog:'watchdog (κόλλημα)',watchdog:'watchdog',brownout:'ΠΤΩΣΗ ΤΑΣΗΣ (brownout)',deep_sleep:'deep sleep',unknown:'άγνωστο'};
  async function runDiag(){
    $('diagRun').disabled=true;$('diagRun').textContent='Έλεγχος…';
    let g,sec,dv;
    try{g=await getJson('/ambi/api/diag',3);sec=await getJson('/ambi/api/security',3);dv=await getJson('/ambi/api/settings/device',3);}
    catch(e){$('diagList').innerHTML=`<li><span class="err">✗</span><div><b>Το ESP δεν απάντησε</b><small>${esc(e.message)}</small></div></li>`;$('diagRun').disabled=false;$('diagRun').textContent='Έλεγχος τώρα';return;}
    const a=g.app||{},R=[];
    const add=(st,title,detail,hint)=>R.push({st,title,detail,hint:hint||''});
    const streaming=a.frame_age_s>=0&&a.frame_age_s<2;
    // 1. strip settings
    {const p=[];let st='ok';
      if(g.leds_used>g.wled_length){st='err';p.push(`χρησιμοποιούνται ${g.leds_used} LED ενώ η ταινία (Length) έχει ${g.wled_length}`);}
      if(g.wled_ma_max===0){st='err';p.push('το όριο ρεύματος του WLED είναι ΚΛΕΙΣΤΟ');}
      else if(g.wled_ma_max!==(g.wled_ma_target??g.budget_ma)){st=st==='err'?st:'warn';p.push(`WLED ${g.wled_ma_max} ≠ ${g.wled_ma_target??g.budget_ma} mA (ταινία ${g.strip_ma??g.budget_ma} + ESP ${g.ma_for_esp??0})`);}
      if(g.budget_ma>900){st='err';p.push('πάνω από τα 900 mA της θύρας USB');}
      if(g.ma_per_led<30||g.ma_per_led>50){st=st==='err'?st:'warn';p.push(`mA ανά LED ${g.ma_per_led} (μετρημένο ~39–41)`);}
      add(st,'Ρυθμίσεις ταινίας',`${g.leds_used}/${g.wled_length} LED, όριο ${g.budget_ma} mA (ταινία ${g.strip_ma??g.budget_ma}, WLED ${g.wled_ma_max}), ${g.ma_per_led} mA/LED, ${g.lamps} λάμπες, G${g.pin}`+(p.length?' — '+p.join('· '):''),
        st==='ok'?'':'Συσκευή / Ταινία LED & ρεύμα: διόρθωσε την τιμή.');}
    // 2. RAM
    {const f=g.heap_free,b=g.heap_block;const st=(f<20480||b<10240)?'err':(f<30720||b<16384)?'warn':'ok';
      add(st,'Μνήμη RAM',`ελεύθερη ${kbS(f)}, μεγαλύτερο κομμάτι ${kbS(b)}, χαμηλότερη από την εκκίνηση ${kbS(g.heap_min)}`,
        st==='ok'?'':'Κλείσε άλλες καρτέλες της σελίδας· αν πέφτει με τον χρόνο, στείλε την αναφορά.');}
    // 3. watchdog / resets
    {const bad=['panic','interrupt_watchdog','task_watchdog','watchdog','brownout'].includes(g.reset_reason);
      const st=bad?'err':g.watchdog_s>0?'ok':'warn';
      add(st,'Επανεκκινήσεις & watchdog',`τελευταία εκκίνηση: ${resetText[g.reset_reason]||g.reset_reason}, πριν ${dur(g.uptime_s)}· εκκινήσεις ${g.boots}, απρόσμενες ${g.bad_resets}${g.last_bad_reset?' (τελευταία: '+(resetText[g.last_bad_reset]||g.last_bad_reset)+')':''}· watchdog ${g.watchdog_s>0?g.watchdog_s+' s':'ΚΛΕΙΣΤΟΣ'}`,
        bad?'Κράσαρε ή έπεσε η τάση: στείλε την αναφορά. Αν λέει brownout, έλεγξε τροφοδοσία / καλώδιο του Atom.':g.watchdog_s>0?'':'Χωρίς watchdog ένα κόλλημα δεν επανέρχεται μόνο του.');}
    // 4. Wi-Fi
    {const r=g.rssi;const st=r===0?'err':r<-80?'err':r<-70||g.wifi_drops>3?'warn':'ok';
      add(st,'Wi-Fi',`σήμα ${r} dBm, αποσυνδέσεις από την εκκίνηση: ${g.wifi_drops}`,st==='ok'?'':'Φέρε το router / mesh πιο κοντά ή άλλαξε θέση στο Atom.');}
    // 5. JointSpace
    if(!a.js_configured)add('na','TV (JointSpace)','δεν έχουν μπει τα κλειδιά: χωρίς αυτά οι γωνίες δεν «δένουν» με τις 3 πλευρές','Ρυθμίσεις σύνδεσης TV (JointSpace).');
    else if(!streaming)add('na','TV (JointSpace)',`η TV δεν στέλνει τώρα· τελευταία επιτυχία πριν ${dur(a.js_ok_age_s)}`,'Το ESP ρωτά την TV μόνο όσο στέλνει Hue.');
    else if(a.js_status==='live')add('ok','TV (JointSpace)',`διαβάζει τις 3 πλευρές· αποτυχίες από την εκκίνηση: ${a.js_fail}`);
    else add(a.js_error==='authentication_failed'?'err':'warn','TV (JointSpace)',`${a.js_status}${a.js_error?' ('+a.js_error+')':''}, αποτυχίες στη σειρά: ${a.js_fail_streak}`,
      a.js_error==='authentication_failed'?'Τα κλειδιά JointSpace δεν δουλεύουν: ξαναβάλ’ τα (Ρυθμίσεις σύνδεσης TV).':'Έλεγξε ότι η TV είναι στο δίκτυο.');
    // 6. Hue / DTLS
    if(!a.paired)add('err','Hue: pairing','καμία TV δεν είναι paired','Pair (30 s) και «Διαμόρφωση» στο Ambilight+hue της TV.');
    else if(a.tv_locked)add('err','Hue: σύνδεση','η TV «κλειδώθηκε» μετά από αποτυχημένο handshake','Κάνε επανεκκίνηση της TV (όχι pairing ξανά).');
    else if(a.hs_fail_streak>=2)add('err','Hue: σύνδεση',`${a.hs_fail_streak} αποτυχημένα handshakes στη σειρά (${a.hs_last_error||'—'})`,'Μην επαναλαμβάνεις το pairing· στείλε την αναφορά.');
    else if(a.stream_active&&!streaming)add('err','Hue: σύνδεση',`η TV ζήτησε stream αλλά δεν έρχονται καρέ (τελευταίο πριν ${dur(a.frame_age_s)})`,'Ambilight+hue off / on στην TV.');
    else if(a.old_session)add('warn','Hue: σύνδεση','παλιά σύνδεση μετά από επανεκκίνηση του ESP: η TV ξανασυνδέεται μόνη της (~15–30 s)');
    else add(streaming?'ok':'na','Hue: σύνδεση',`${a.paired} TV paired· handshakes OK ${a.hs_ok} / αποτυχίες ${a.hs_fail}`+(streaming?'':' · η TV δεν στέλνει τώρα'));
    // 7. Hue rate
    if(streaming){const st=a.fps<10?'err':a.fps<18?'warn':a.bad_frames>0?'warn':'ok';
      add(st,'Hue: ρυθμός',`${a.fps} καρέ/s, άκυρα καρέ ${a.bad_frames}`,st==='ok'?'':'Ασταθές Wi-Fi ή άλλη λειτουργία της TV (Gaming / HDR).');}
    else add('na','Hue: ρυθμός','η TV δεν στέλνει τώρα');
    // 8. pipeline
    if(!a.running)add('na','Ταινία: έξοδος','σε Stop: την ταινία την έχει το WLED');
    else if(streaming){const st=g.output_age_ms<0||g.output_age_ms>500?'err':'ok';
      add(st,'Ταινία: έξοδος',`τελευταίο καρέ στην ταινία πριν ${g.output_age_ms} ms${a.night_now?' · νυχτερινό όριο ενεργό':''}`,st==='ok'?'':'Τα χρώματα έρχονται αλλά δεν φτάνουν στην ταινία: στείλε την αναφορά.');}
    else add('na','Ταινία: έξοδος','η TV δεν στέλνει: η ταινία μένει σβηστή');
    // 9. flash
    {const pf=g.fs_total>0?100*(g.fs_total-g.fs_used)/g.fs_total:0,pa=g.app_slot>0?100*g.app_size/g.app_slot:0;
      const st=g.fs_total<=0?'err':pf<10||pa>95?'warn':'ok';
      add(st,'Μνήμη flash',`αρχεία ${kbS(g.fs_used)} / ${kbS(g.fs_total)}, firmware ${Math.round(pa)} % του χώρου`,st==='ok'?'':'Χώρος σχεδόν γεμάτος.');}
    // 10. security
    {const p=[];if(!sec.pin)p.push('χωρίς PIN');if(!sec.ota_lock)p.push('OTA lock ανοιχτό');if(sec.ap_default_pass)p.push('Wi-Fi του WLED με εργοστασιακό κωδικό');
      add(p.length?'warn':'ok','Ασφάλεια WLED',p.length?p.join('· '):'PIN, OTA lock και κωδικός Wi-Fi ορισμένα',p.length?'Κάρτα «Ασφάλεια» παραπάνω.':'');}
    add(g.udp_realtime||g.udp_sync_recv?'warn':'ok','Χρώματα από άλλες συσκευές',g.udp_realtime||g.udp_sync_recv?`ανοιχτά: ${[g.udp_realtime?'UDP realtime (E1.31/DDP)':'',g.udp_sync_recv?'λήψη από άλλα WLED':''].filter(Boolean).join(', ')}`:'κλειστά (UDP realtime, λήψη sync)',
      g.udp_realtime||g.udp_sync_recv?'WLED → Config → Sync Interfaces: κλείσε τα Receive.':'');
    add(g.cfg_needs_pin?'ok':'warn','CVE-2026-71264 (WLED /json/cfg)',g.cfg_needs_pin?'μπάλωμα σε αυτό το firmware: οι ρυθμίσεις θέλουν PIN':'δεν επαληθεύτηκε διόρθωση');
    // 11. clock
    if(!g.time_ok)add('warn','Ώρα',`το ESP δεν ξέρει την ώρα${g.ntp?' (NTP ανοιχτό, χωρίς συγχρονισμό ακόμα)':''}`,'Συσκευή → Ώρα: «από internet (NTP)», ή άνοιξε αυτή τη σελίδα.');
    else if(g.ntp&&g.ntp_age_s>86400)add('warn','Ώρα',`NTP χωρίς συγχρονισμό εδώ και ${dur(g.ntp_age_s)}`);
    else add('ok','Ώρα',g.ntp?`NTP, τελευταίος συγχρονισμός πριν ${dur(g.ntp_age_s)}`:'από τον browser (χωρίς NTP χάνεται μετά από εκκίνηση)');
    // 12. stability
    {const st=[g.stack_dtls,g.stack_js,g.stack_loop,g.stack_web].some(v=>v>=0&&v<1024)?'warn':g.bad_resets>2?'warn':'ok';
      add(st,'Σταθερότητα',`σε λειτουργία ${dur(g.uptime_s)}· στοίβα που περισσεύει: DTLS ${g.stack_dtls}, JointSpace ${g.stack_js}, loop ${g.stack_loop}, web ${g.stack_web} bytes`,st==='ok'?'':'Λίγη στοίβα ή επαναλαμβανόμενα κρασαρίσματα: στείλε την αναφορά.');}
    const icon={ok:'✓',warn:'!',err:'✗',na:'·'},cls={ok:'ok',warn:'warn',err:'err',na:'na'};
    $('diagList').innerHTML=R.map(r=>`<li><span class="${cls[r.st]}">${icon[r.st]}</span><div><b>${esc(r.title)}</b><small>${esc(r.detail)}${r.hint?'<br>→ '+esc(r.hint):''}</small></div></li>`).join('');
    const cnt=k=>R.filter(r=>r.st===k).length;
    $('diagOk').textContent=cnt('ok');$('diagWarn').textContent=cnt('warn');$('diagErr').textContent=cnt('err');$('diagNa').textContent=cnt('na');
    const mark={ok:'[OK]  ',warn:'[ΠΡΟΣ]',err:'[ΣΦΑΛ]',na:'[--]  '};
    diagText=`AmbiBottom διάγνωση · ${g.version} · ${new Date().toISOString()}\n`+R.map(r=>`${mark[r.st]} ${r.title}: ${r.detail}`).join('\n')+'\n\nraw: '+JSON.stringify(g);
    $('diagReport').value=diagText;$('diagReport').hidden=false;$('diagCopy').disabled=false;
    $('diagRun').disabled=false;$('diagRun').textContent='Έλεγχος ξανά';
  }
  $('diagRun').addEventListener('click',runDiag);
  $('diagCopy').addEventListener('click',()=>{
    const t=$('diagReport');t.hidden=false;t.focus();t.select();
    let ok=false;try{ok=document.execCommand('copy');}catch(e){}
    if(!ok&&navigator.clipboard)navigator.clipboard.writeText(diagText).then(()=>{$('diagCopy').textContent='Αντιγράφηκε ✓';}).catch(()=>{});
    else if(ok)$('diagCopy').textContent='Αντιγράφηκε ✓';
    setTimeout(()=>{$('diagCopy').textContent='Αντιγραφή αναφοράς';},2500);
  });
  // v0.44: «Καταγραφή Hue» - the ESP keeps 128 rows, this page collects them every second
  let capRows=[],capNext=0,capTimer=null,capLost=0,capT0=0;
  const capHead=['seq','t_ms','colorspace','lamps'].concat([1,2,3,4,5].flatMap(i=>[`l${i}_a`,`l${i}_b`,`l${i}_c`]),
    ['first_r','first_g','first_b','mid_r','mid_g','mid_b','last_r','last_g','last_b','ma','js_left_r','js_left_g','js_left_b','js_right_r','js_right_g','js_right_b']);
  function capRender(on){
    $('capStart').disabled=on;$('capStop').disabled=!on;$('capCsv').disabled=!capRows.length;
    const secs=capRows.length?(capRows[capRows.length-1][1]-capRows[0][1])/1000:0;
    $('capNote').textContent=(on?'Καταγράφει… ':capRows.length?'Τέλος. ':'')+(capRows.length?`${capRows.length} καρέ, ${secs.toFixed(1)} s`:'')+(capLost?` · χάθηκαν ${capLost} (η σελίδα άργησε)`:'')
      +(on?'':capRows.length?' — «Λήψη CSV» για το αρχείο.':'Η TV πρέπει να στέλνει (Ambilight+hue).');
  }
  async function capPoll(){
    try{const j=await getJson('/ambi/api/capture?from='+capNext,1);capLost+=j.lost||0;
      for(const r of j.rows)capRows.push(r);if(j.rows.length)capNext=j.rows[j.rows.length-1][0];
      if(!j.on){clearInterval(capTimer);capTimer=null;}capRender(Boolean(j.on));}catch(e){}
  }
  $('capStart').addEventListener('click',async()=>{
    try{const j=await post('/ambi/api/capture',{on:true});capRows=[];capLost=0;capNext=j.next||0;capT0=Date.now();
      clearInterval(capTimer);capTimer=setInterval(capPoll,1000);capRender(true);}
    catch(e){$('capNote').textContent='Δεν ξεκίνησε: '+(e.message==='pin_required'?'πάτα «🔒 PIN» πάνω δεξιά.':e.message);}
  });
  $('capStop').addEventListener('click',async()=>{
    await capPoll();try{await post('/ambi/api/capture',{on:false});}catch(e){}
    clearInterval(capTimer);capTimer=null;capRender(false);
  });
  $('capCsv').addEventListener('click',()=>{
    const csv=[capHead.join(',')].concat(capRows.map(r=>r.join(','))).join('\n')+'\n';
    const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([csv],{type:'text/csv'}));
    a.download='ambi_hue_'+new Date(capT0||Date.now()).toISOString().slice(0,19).replace(/[:T]/g,'-')+'.csv';
    document.body.appendChild(a);a.click();setTimeout(()=>{URL.revokeObjectURL(a.href);a.remove();},1000);
  });
  const fmtT=t=>{t=Math.floor(t);const h=Math.floor(t/3600),m=Math.floor(t/60)%60,x=t%60;return (h?h+':'+String(m).padStart(2,'0'):String(m))+':'+String(x).padStart(2,'0');};
  const esc=t=>String(t).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
  async function loadLog(){
    if(!$('espLog').open)return;
    try{
      const r=await fetch('/ambi/api/log',{cache:'no-store'});if(!r.ok)return;
      const l=await r.json();
      $('espLogCount').textContent='· '+l.length;
      $('espLogList').innerHTML=l.slice().reverse().map(x=>`<li><time>${fmtT(x.t)}</time>${esc(x.e)}${x.n?` <span class="faint">×${x.n}, έως ${fmtT(x.last)}</span>`:''}</li>`).join('')||'<li>Τίποτα ακόμα.</li>';
    }catch(e){}
  }
  try{if(localStorage.getItem('ambi.espLog')==='1')$('espLog').open=true;}catch(e){}
  $('espLog').addEventListener('toggle',()=>{try{localStorage.setItem('ambi.espLog',$('espLog').open?'1':'0');}catch(e){}loadLog();});
  setInterval(loadLog,3000);
"""

SERVICE_BANNER_OLD = ('<div><strong>Η υπηρεσία είναι σταματημένη</strong><span>Το bridge δεν απαντά στην TV (Hue, JointSpace) '
                      'και δεν στέλνει στην ταινία. Τρέχουν μόνο το terminal και αυτή η σελίδα. Start εδώ ή <b>s</b> στο terminal.</span></div>')
SERVICE_BANNER_NEW = ('<div><strong>Σταματημένο: η ταινία είναι του WLED</strong><span>Έτσι ξεκινά το ESP μετά από κάθε εκκίνηση (εκτός με «Αυτόματο Start»). '
                      'Η TV βλέπει ακόμα το bridge (pairing, stream), αλλά ό,τι στέλνει δεν πάει στην ταινία· τα εφέ και η εφαρμογή '
                      'του WLED δουλεύουν κανονικά. Start: η σελίδα παίρνει την ταινία μέχρι το Stop.</span></div>')
NOTE_OLD = ("(stopped?'Η υπηρεσία είναι σταματημένη. ':'Η υπηρεσία τρέχει. ')+'Stop: σταματά την επικοινωνία με την TV και την ταινία, "
            "η σελίδα και το terminal μένουν. Restart: ξανά από το νεότερο build. Έξοδος: κλείνει το πρόγραμμα (ξεκινά ξανά μόνο από "
            "το terminal: ambi). Στο terminal: q stop, s start, r δύο φορές restart, x έξοδος.'")
NOTE_NEW = ("(stopped?'Σταματημένο: η ταινία είναι του WLED. ':'Τρέχει: η ταινία είναι της σελίδας (TV ή Demo). ')+'Η TV βλέπει "
            "το bridge και στις δύο καταστάσεις. Μετά από εκκίνηση του ESP: σταματημένο, εκτός αν είναι ενεργό το «Αυτόματο Start» (Συσκευή).'")


def build(page):
    p = page
    p = sub_once(p, "<title>Ambilight · Live Monitor</title>", "<title>AmbiBottom · ESP</title>")
    p = sub_once(p, "</head>", ESP_CSS)
    # v0.45: the language the user chose last (EN / ΕΛ button) when opening plain /ambi
    p = sub_once(p, "</head>", "<script>try{if(document.documentElement.lang==='en'&&localStorage.getItem('ambiLang')==='el'"
                               "&&/^\\/ambi\\/?$/.test(location.pathname))location.replace('/ambi/el');}catch(e){}</script>\n</head>")
    n = len(re.findall(r"fetch\('/api/", p))
    if n < 10:
        sys.exit(f"only {n} fetch('/api/...') calls found: the page changed, update make_esp_page.py")
    p = p.replace("fetch('/api/", "fetch('/ambi/api/")
    p = sub_once(p, "if(window.EventSource){", "if(false&&window.EventSource){")
    p = sub_once(p, "setInterval(poll,100);", "setInterval(poll,250);")
    # v0.49: the ESP's heap - no request while the tab is hidden (phones keep dozens of tabs open)
    # and never two state requests at once (they piled up while the ESP was busy)
    p = sub_once(p, "  async function poll(){\n    try{\n      const r=await fetch('/ambi/api/state',{cache:'no-store'});",
                 "  let pollBusy=false;\n  async function poll(){\n    if(document.hidden||pollBusy)return;\n    pollBusy=true;\n"
                 # v0.49.2: one stuck request must not stop polling for good: give up after 3 s
                 "    const ac=new AbortController(),tm=setTimeout(()=>ac.abort(),3000);\n"
                 "    try{\n      const r=await fetch('/ambi/api/state',{cache:'no-store',signal:ac.signal});")
    p = sub_once(p, "      render(await r.json());\n    }catch(err){}\n  }",
                 "      render(await r.json());\n    }catch(err){}finally{clearTimeout(tm);pollBusy=false;}\n  }")
    p = sub_once(p, "const view = {sides:'zones', bottom:'lamps', fill:true, glow:true};",
                 "const view = {sides:'zones', bottom:'strip', fill:true, glow:true};")
    p = sub_once(p, "    renderBridgePanel(s);\n    updateDrawer(s); draw();\n  }",
                 "    renderBridgePanel(s);\n    updateDrawer(s); draw();\n    if(window.ambiEsp)window.ambiEsp(s);\n  }")
    p = sub_once(p, "    renderBridgePanel(null);\n    updateDrawer(null); draw();\n  }",
                 "    renderBridgePanel(null);\n    updateDrawer(null); draw();\n    if(window.ambiEsp)window.ambiEsp(null);\n  }")
    p = sub_once(p, '  <aside class="side-panel">', ESP_PANEL)
    p = sub_once(p, SERVICE_BANNER_OLD, SERVICE_BANNER_NEW)
    p = sub_once(p, NOTE_OLD, NOTE_NEW)
    p = sub_once(p, "!edit?'Start/Stop/Restart/Έξοδος: μόνο από τον browser του ίδιου Linux ή μετά από «Σύνδεση» πάνω δεξιά.'",
                 "!edit?'Start/Stop: πάτα «🔒 PIN» πάνω δεξιά.'")
    p = sub_once(p, "'Μόνο προβολή. Για αλλαγή: «Σύνδεση» πάνω δεξιά (αν έχει οριστεί login), ή ο browser του ίδιου Linux ως "
                    "http://127.0.0.1:'+location.port+'/monitor.'", "'Μόνο προβολή: πάτα «🔒 PIN» πάνω δεξιά.'", count=3)
    p = sub_once(p, "Εφαρμόζονται αμέσως στο επόμενο frame και αποθηκεύονται στο <code>settings.json</code> (--state-dir).",
                 "Εφαρμόζονται αμέσως στο επόμενο frame και αποθηκεύονται στη μνήμη του ESP (μένουν μετά από εκκίνηση και OTA). "
                 "Πλήθος LED = πόσα LED χρησιμοποιούνται από την αρχή της ταινίας· τα υπόλοιπα μένουν σβηστά. "
                 "Στο WLED (LED Preferences → Length) βάλε όλα όσα έχει η ταινία.")
    p = sub_once(p, '<span class="hint">Ίδιο με το «Length» στο WLED.</span>',
                 '<span class="hint">Όσα LED ανάβουν, από την αρχή της ταινίας. Τα υπόλοιπα μένουν σβηστά. Το πολύ όσα λέει το «Length» του WLED (όλη η ταινία).</span>')
    p = sub_once(p, "if(j.limits&&j.limits.current_budget_ma_max)$('setBudget').max=j.limits.current_budget_ma_max;",
                 "if(j.limits&&j.limits.current_budget_ma_max)$('setBudget').max=j.limits.current_budget_ma_max;"
                 "if(j.limits&&j.limits.led_count_max)$('setLedCount').max=j.limits.led_count_max;")
    # v0.40: the measured value of this strip (2026-11-19), not the Linux bridge's reference strip
    p = sub_once(p, "'Δική σου τιμή (διαφέρει από το 45 της ταινίας αναφοράς).'",
                 "'Δική σου τιμή. Γράφεται και στο «LED current» του WLED. Συνήθης WS2812B: ~40 mA (μετρημένο 39 mA σε λευκό τέρμα).'")
    p = sub_once(p, "'Το 45 μετρήθηκε στην ταινία αναφοράς (BTF WS2812B ECO 30 LED/m, ~44 mA). Άλλη ταινία: μέτρησέ την (README: «Measuring mA per LED»).'",
                 "'Συνήθης WS2812B 5 V: μετρημένο ~39 mA σε λευκό τέρμα (13,5 ανά χρώμα), άρα 40 με λίγο περιθώριο. Μέτρησε τη δική σου αν μπορείς. Γράφεται και στο «LED current» του WLED.'")
    # v0.41: the LED wear log runs on the ESP too
    p = sub_once(p, "Αποθήκευση στο <code>strip_stats.json</code> κάθε λεπτό και στην έξοδο.",
                 "Αποθήκευση στη μνήμη του ESP (αρχείο <code>ambi_stats.json</code> του WLED) κάθε 5 λεπτά όσο αλλάζει· "
                 "έως 5 λεπτά μπορεί να χαθούν αν κοπεί το ρεύμα. Μέρες και επεισόδια θέλουν ώρα (Συσκευή → Ώρα του ESP).")
    p = sub_once(p, "'Μηδενισμός μόνο από τον browser του ίδιου Linux ή μετά από «Σύνδεση».'", "'Αλλαγές: πάτα «🔒 PIN» πάνω δεξιά.'")
    p = sub_once(p, "<summary>Τελευταίες 30 μέρες</summary>", "<summary>Τελευταίες 14 μέρες</summary>")
    p = sub_once(p, "Στο terminal το ίδιο κάνει το πλήκτρο <strong>p</strong>.",
                 "Το ίδιο κάνει ένα σύντομο πάτημα στο κουμπί του Atom.")
    # v0.38: back to WLED's own page (the user asked for a way both ways)
    p = sub_once(p, '<div class="top-actions">',
                 '<div class="top-actions"><button type="button" class="pill" id="pinPill" hidden title="Οι αλλαγές θέλουν το PIN ρυθμίσεων του WLED">🔒 PIN</button>'
                 '<a class="pill lang-link" href="/ambi/en" hreflang="en" title="English" onclick="try{localStorage.setItem(\'ambiLang\',\'en\')}catch(e){}">EN</a>'
                 '<a class="pill" href="/" title="Η σελίδα του WLED (εφέ, ρυθμίσεις, update)" '
                 'style="text-decoration:none">WLED ↗</a>')
    # v0.38: memory of the ESP in "Τεχνικά δεδομένα" (user: RAM and flash, nicer)
    p = sub_once(p, '<div><span>Προέλευση δεδομένων</span><strong id="origin">—</strong></div></div>',
                 '<div><span>Προέλευση δεδομένων</span><strong id="origin">—</strong></div>'
                 '<div class="mem"><span>RAM ελεύθερη τώρα</span><strong id="memRam">—</strong><i class="mem-bar"><b id="memRamBar"></b></i></div>'
                 '<div class="mem"><span>RAM: χαμηλότερη από την εκκίνηση · μεγαλύτερο κομμάτι</span><strong id="memRamMin">—</strong></div>'
                 '<div class="mem"><span>Flash: firmware / χώρος για update</span><strong id="memApp">—</strong><i class="mem-bar"><b id="memAppBar"></b></i></div>'
                 '<div class="mem"><span>Flash: αρχεία WLED (ρυθμίσεις, presets)</span><strong id="memFs">—</strong><i class="mem-bar"><b id="memFsBar"></b></i></div>'
                 '</div>')
    # v0.49 (user): the board as it reports itself, e.g. «Έξοδος: ESP32-S3 · WLED» (chip · WLED name =
    # the bridge name the TV sees); set by renderDev()
    p = sub_once(p, "<span>WLED (DDP)</span>", '<span id="outTargetLabel">Έξοδος</span>')
    p = sub_once(p, "'ανενεργό (χωρίς --wled-ip)'", "'σταματημένο (Start στην κάρτα «Ταινία»)'")
    p = sub_once(p, "<span>Καρέ / s προς WLED</span>", "<span>Καρέ / s στην ταινία</span>")
    p = sub_once(p, '<span id="demoBadge" class="pill demo hidden">SYNTHETIC DEMO</span>',
                 '<span id="demoBadge" class="pill demo hidden">DEMO · ΕΦΕ WLED</span>')
    # v0.37: JointSpace runs on the ESP too (credentials form kept, texts for the ESP)
    p = sub_once(p, "JointSpace credentials για τις τρεις εργοστασιακές πλευρές και τη μείξη στις γωνίες. Η IP της TV ορίζεται με "
                    "<code>--tv-ip</code> στην εκκίνηση του bridge, όχι εδώ.",
                 "JointSpace credentials για τις τρεις εργοστασιακές πλευρές και τη μείξη στις γωνίες. Η IP της TV βρίσκεται μόνη "
                 "της από τη σύνδεση Hue. Το ESP ρωτά την TV μόνο όσο εκείνη στέλνει Hue.")
    p = sub_once(p, "Αποθηκεύονται ιδιωτικά σε αυτό το Linux (--state-dir) για τις επόμενες εκκινήσεις.",
                 "Αποθηκεύονται στη μνήμη του ESP (μένουν μετά από εκκίνηση και OTA) και δεν εμφανίζονται ποτέ ξανά.")
    p = sub_once(p, "note.textContent='Αποθηκεύτηκαν στο Linux. Η κατάσταση JointSpace δείχνει αν η σύνδεση πέτυχε.';",
                 "note.textContent='Αποθηκεύτηκαν στο ESP. Η κατάσταση JointSpace δείχνει αν η σύνδεση πέτυχε (όσο η TV στέλνει Hue).';")
    p = sub_once(p, "note.textContent='Δεν αποθηκεύτηκαν. Άνοιξε από localhost και έλεγξε τις τιμές και τα δικαιώματα του φακέλου.';",
                 "note.textContent='Δεν αποθηκεύτηκαν: έλεγξε τις τιμές (και το PIN ρυθμίσεων του WLED, αν έχει).';")
    # v0.45: JointSpace pairing from the page (the TV shows a PIN); the manual form stays for
    # whoever already has credentials
    p = sub_once(p, '      <form id="credentialsForm">', JS_PAIR_HTML + '      <form id="credentialsForm">')
    p = sub_once(p, "  loadSettings();\n  // ---- v0.15 (Phase 3)", "  loadSettings();\n" + JS_PAIR_SCRIPT + "  // ---- v0.15 (Phase 3)")
    # v0.45: the project's public name (no trademark as the page's title)
    p = sub_once(p, '<h1>AMBILIGHT <em>MONITOR</em></h1><p>4th-Side Lab <span class="microdot">·</span> ambi-bridge-cpp</p>',
                 '<h1>AMBI<em>BOTTOM</em></h1><p>4η πλευρά για Ambilight <span class="microdot">·</span> WLED usermod</p>')
    p = sub_once(p, '<footer><span>AMBILIGHT LAB · 4TH SIDE PROJECT</span><span id="footVersion">ambi-bridge-cpp monitor dashboard</span></footer>',
                 '<footer><span>AMBIBOTTOM · ΔΕΝ ΣΧΕΤΙΖΕΤΑΙ ΜΕ ΤΗ PHILIPS / SIGNIFY / TP VISION</span><span id="footVersion">AmbiBottom</span></footer>')
    p = sub_once(p, "$('footVersion').textContent='ambi-bridge-cpp '+s.bridge_version;", "$('footVersion').textContent='AmbiBottom '+s.bridge_version;")
    p = sub_once(p, "</body>", ESP_SCRIPT)
    p = sub_once(p, "  window.ambiEsp=s=>{last=s;render();};\n", "  window.ambiEsp=s=>{last=s;render();};\n" + ESP_DEVICE_SCRIPT + ESP_CAM_SCRIPT)
    # v0.40 (user: remove what the ESP does not need): the Linux-only panels' HTML is left out
    # (it was only hidden); their script finds a detached placeholder instead of the element.
    for pat in (r'<section class="panel identity-panel" id="identityPanel".*?</section>',
                r'<section class="panel discovery-panel" id="discoveryPanel".*?</section>',
                r'<section class="panel identity-panel access-panel" id="accessPanel".*?</section>',
                r'<dialog id="loginDialog".*?</dialog>'):
        p, n = re.subn(pat, "", p, count=1, flags=re.S)
        if n != 1:
            sys.exit(f"{pat[:50]!r} not found in the Linux page: update make_esp_page.py")
    p = sub_once(p, "const $ = id => document.getElementById(id);",
                 "const $ = id => document.getElementById(id) || ($.none[id] = $.none[id] || document.createElement('div'));"
                 " $.none = {};  // v0.40 ESP: panels left out")
    p = sub_once(p, '<section class="panel strip-settings" id="stripSettingsPanel"', ESP_DEVICE_PANEL)
    # v0.49: the Linux-only identity / discovery panels are not on the ESP: no more requests for them
    # (discovery asked every 5 s on the Settings tab, both answered 404)
    p = sub_once(p, "  async function loadIdentity(){\n", "  async function loadIdentity(){return;  // v0.49 ESP: no identity API\n")
    p = sub_once(p, "  async function loadDiscovery(){\n", "  async function loadDiscovery(){return;  // v0.49 ESP: no discovery API\n")
    p = sub_once(p, "  setInterval(()=>{if(!$('paneBridge').hidden)loadDiscovery();},5000);\n", "")
    p = sub_once(p, '<section class="panel strip-settings" id="stripSettingsPanel"', ESP_CAM_PANEL + '\n<section class="panel strip-settings" id="stripSettingsPanel"')
    p = bridge_layout(p)
    p = live_layout(p)
    return p


# v0.47 (user: «να φαίνεται κυρίως πάνω το ρεύμα, μετά η καταπόνηση που αν πατάω βελάκι να δείχνει
# και τα άλλα αλλιώς να έχει τη μπάρα, μετά το Start/Stop με βελάκι, το τηλεκοντρόλ μικρό»)
def _blocks(p, start_pat):
    a = p.index(start_pat)
    seg = p[a:]
    out, depth, start = [], 0, None
    for m in re.finditer(r"<(/?)(section|div|details|aside|dialog)\b([^>]*)>", seg):
        if not m.group(1):
            depth += 1
            if depth == 2:
                i = re.search(r'id="([^"]+)"', m.group(3))
                c = re.search(r'class="([^"]+)"', m.group(3))
                cur, start = (i.group(1) if i else c.group(1)), m.start()
        else:
            depth -= 1
            if depth == 1:
                out.append((cur, a + start, a + m.end()))
            if depth == 0:
                return out, a, a + m.end()
    sys.exit("live layout: unbalanced tags")


def live_layout(p):
    # the wear panel leaves the lower row ...
    kids, _, _ = _blocks(p, '<section class="lower">')
    wear = [k for k in kids if k[0] == "stripLogPanel"]
    if len(wear) != 1:
        sys.exit("live layout: no stripLogPanel in the lower row")
    _, ws, we = wear[0]
    wear_html = p[ws:we]
    p = p[:ws] + p[we:]
    # ... its scope switch goes into its folded details; the bar and the totals line stay
    m = re.search(r'\s*<div class="mode-switch log-switch".*?</div>', wear_html, re.S)
    if not m:
        sys.exit("live layout: no log-switch")
    sw = m.group(0).strip()
    wear_html = wear_html[:m.start()] + wear_html[m.end():]
    wear_html = sub_once(wear_html, "<summary>Λεπτομέρειες: πιο φορτωμένα LED, επεισόδια, μέρες, μηδενισμός</summary>",
                         "<summary>Λεπτομέρειες: πιο φορτωμένα LED, επεισόδια, μέρες, μηδενισμός</summary>" + sw)
    # the current panel: readout + meter visible, the rest folded
    kids, a, e = _blocks(p, '<aside class="side-panel">')
    names = [k[0] for k in kids]
    want = ["pinDialog", "espPanel", "panel calibration-panel"]
    if names != want:
        sys.exit(f"live layout: side column is {names}, expected {want}")
    blk = {k: p[s:t] for k, s, t in kids}
    cur = blk["panel calibration-panel"]
    i = cur.index('<div class="diagnostic-grid" style="margin-top:18px">')
    j = cur.rindex("</div>")
    cur = cur[:i] + '<details class="fold" id="outMore"><summary>Λεπτομέρειες εξόδου</summary>' + cur[i:j] + "</details>\n    " + cur[j:]
    side = ('<aside class="side-panel">\n    ' + cur + "\n    " + wear_html + "\n    " +
            blk["espPanel"] + "\n  </aside>")
    p = p[:a] + side + p[e:]
    # v0.48 (user: «όταν πατάω PIN στις ρυθμίσεις δε δείχνει τίποτα»): the dialog lived in the Live
    # pane, hidden on the other tab; it goes straight under <body>
    m = re.search(r"<body[^>]*>", p)
    return p[:m.end()] + "\n" + blk["pinDialog"] + p[m.end():]


def _children(seg):
    """Top-level elements of the first element in `seg`: [(id, start, end)] (div / section / details)."""
    out, depth, start, cur = [], 0, None, None
    for m in re.finditer(r"<(/?)(section|div|details)\b([^>]*)>", seg):
        if not m.group(1):
            depth += 1
            if depth == 2:
                i = re.search(r'id="([^"]+)"', m.group(3))
                cur, start = (i.group(1) if i else None), m.start()
        else:
            depth -= 1
            if depth == 1:
                out.append((cur, start, m.end()))
            if depth == 0:
                return out, m.end()
    sys.exit("bridge grid: unbalanced tags")


# v0.46 (user: «πάλι χάλασε το align στις ρυθμίσεις, βρες έναν έξυπνο τρόπο»): cards of very
# different heights in one auto grid always leave holes. Now: a sticky left column (the bridge card
# and an index of the cards) and ONE right column with every card at full width, in a fixed order.
BRIDGE_ORDER = [("espDevPanel", "Συσκευή"), ("stripSettingsPanel", "Ταινία LED & ρεύμα"),
                ("credentialsPanel", "Σύνδεση TV (JointSpace)"), ("espSecPanel", "Ασφάλεια"),
                ("espDiagPanel", "Διάγνωση"), ("espCapPanel", "Καταγραφή Hue"),
                ("espCamPanel", "Βαθμονόμηση με κάμερα")]


def bridge_layout(p):
    a = p.index('<section class="bridge-grid">')
    kids, end = _children(p[a:])
    blocks = {k: p[a + s:a + e] for k, s, e in kids}
    want = {"servicePanel"} | {k for k, _ in BRIDGE_ORDER}
    if set(blocks) != want:
        sys.exit(f"bridge grid: expected {sorted(want)}, found {sorted(blocks)}")
    # v0.49: scroll only - a "#id" in the address is read as a tab name by the page and hid the Settings tab
    index = "".join(f'<a href="#bridge" data-goto="{k}" onclick="document.getElementById(\'{k}\').scrollIntoView({{behavior:\'smooth\',block:\'start\'}});return false;">{t}</a>' for k, t in BRIDGE_ORDER)
    html = ('<section class="bridge-cols">\n<div class="bridge-side">\n' + blocks["servicePanel"] +
            '\n<nav class="panel bridge-index" aria-label="Κάρτες">' + index + '</nav>\n</div>\n<div class="bridge-main">\n' +
            "\n".join(blocks[k] for k, _ in BRIDGE_ORDER) + '\n</div>\n</section>')
    return p[:a] + html + p[a + end:]


GREEK_RUN = re.compile(r"[^<>'\"`]*[\u0370-\u03FF\u1F00-\u1FFF][^<>'\"`]*")


def to_english(page):
    """v0.45: the English page = the Greek page with every Greek text run replaced from
    tools/i18n/el_to_en.json (keys = the runs, stripped). A run without a translation stops the
    build with the list, so nothing Greek slips into the English page."""
    with open(I18N, encoding="utf-8") as f:
        tr = json.load(f)
    missing = []

    def one(m):
        t = m.group(0)
        k = t.strip()
        if k not in tr:
            missing.append(k)
            return t
        lead = t[:len(t) - len(t.lstrip())]
        trail = t[len(t.rstrip()):]
        return lead + tr[k] + trail

    out = GREEK_RUN.sub(one, page)
    if missing:
        sys.exit("no English for these texts (add them to tools/i18n/el_to_en.json):\n" +
                 "\n".join(json.dumps(k, ensure_ascii=False) for k in sorted(set(missing))))
    out = out.replace('<html lang="el">', '<html lang="en">', 1)
    a = """<a class="pill lang-link" href="/ambi/en" hreflang="en" title="English" onclick="try{localStorage.setItem('ambiLang','en')}catch(e){}">EN</a>"""
    if out.count(a) != 1:
        sys.exit("language link not found")
    out = out.replace(a, a.replace("/ambi/en", "/ambi/el").replace('"en"', '"el"').replace("'en'", "'el'")
                         .replace('title="English"', 'title="Ελληνικά"').replace(">EN</a>", ">ΕΛ</a>"), 1)
    return out


def header(pages):
    lines = ["// Generated by tools/make_esp_page.py from src/monitor_server.cpp (kPage) - do not edit.",
             "// The AmbiBottom page (/ambi), gzipped: English (AMBI_PAGE_GZ); Greek = a patch on it (AMBI_PAGE_EL_PATCH_GZ)",
             "// applied in the browser by AMBI_PAGE_EL_LOADER (v0.49).",
             "#pragma once", "#include <Arduino.h>", "",
             "// v0.48: AMBIBOTTOM_PAGE_EL=0 (e.g. the 4 MB ESP32 build) leaves Greek out (v0.49: the ~22 KB patch)",
             "#ifndef AMBIBOTTOM_PAGE_EL", "#define AMBIBOTTOM_PAGE_EL 1", "#endif", ""]
    for name, gz, html_len in pages:
        if name == "AMBI_PAGE_EL_PATCH_GZ":
            lines.append("#if AMBIBOTTOM_PAGE_EL")
        lines += [f"// {len(gz)} bytes gzipped, {html_len} bytes as HTML",
                  f"static const size_t {name}_LEN = {len(gz)};",
                  f"static const uint8_t {name}[] PROGMEM = {{"]
        for i in range(0, len(gz), 20):
            lines.append("  " + ",".join(str(b) for b in gz[i:i + 20]) + ",")
        lines += ["};"]
        if name == "AMBI_PAGE_EL_PATCH_GZ":
            lines.append("static const char AMBI_PAGE_EL_LOADER[] PROGMEM = R\"AMBI(" + EL_LOADER + ")AMBI\";")
            lines.append("#endif")
        lines.append("")
    return "\n".join(lines)


def main():
    check = "--check" in sys.argv[1:]
    with open(SRC, encoding="utf-8") as f:
        el = build(extract_page(f.read()))
    en = to_english(el)
    patch = el_patch(en, el)
    pages, outs = [], [(OUT_HTML_EL, el), (OUT_EL_PATCH, patch), (OUT_EL_LOADER, EL_LOADER)]
    for name, txt, fn in (("AMBI_PAGE_GZ", en, OUT_HTML), ("AMBI_PAGE_EL_PATCH_GZ", patch, None)):
        data = txt.encode("utf-8")
        pages.append((name, gzip.compress(data, compresslevel=9, mtime=0), len(data)))
        if fn:
            outs.append((fn, txt))
    outs.append((OUT_H, header(pages)))
    if check:
        ok = all(os.path.exists(fn) and open(fn, encoding="utf-8").read() == txt for fn, txt in outs)
        print("ESP page up to date" if ok else "ESP page OUT OF DATE: run tools/make_esp_page.py")
        sys.exit(0 if ok else 1)
    for fn, txt in outs:
        with open(fn, "w", encoding="utf-8") as f:
            f.write(txt)
    print("wrote " + ", ".join(f"{n} {len(g)} B gz ({l} B)" for n, g, l in pages))


if __name__ == "__main__":
    main()
