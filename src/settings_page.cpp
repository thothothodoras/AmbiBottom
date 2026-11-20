// v0.32: the strip settings as a page of their own, served by the monitor at
// /ambi/settings. Plain HTML + a little script, no external files. It reads
// GET /api/settings/strip and saves with POST /api/settings/strip/validated,
// which checks every value (settings::validate_json) and saves nothing when
// one is wrong. Who may save is the monitor's usual rule (this machine's
// browser, or a logged-in device).
#include "settings_page.hpp"

namespace settings_page {

const char* kPage = R"PAGE(<!doctype html>
<html lang="el"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Ρυθμίσεις ταινίας — ambi-bridge</title>
<style>
:root{color-scheme:dark;--bg:#0a0e17;--panel:#111725;--border:#263044;--muted:#8998b3;--txt:#e8edf6;--acc:#53d1d4;--bad:#ff8f7a;--ok:#7bd88f}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--txt);font:15px/1.45 system-ui,-apple-system,"Segoe UI",sans-serif}
main{max-width:760px;margin:0 auto;padding:20px 16px 60px}
h1{font-size:22px;margin:0 0 4px}h2{font-size:15px;margin:0 0 12px;color:var(--acc);letter-spacing:.04em}
a{color:var(--acc)}p.lead{color:var(--muted);margin:0 0 18px}
section{background:var(--panel);border:1px solid var(--border);border-radius:10px;padding:16px;margin:0 0 14px}
.row{display:grid;grid-template-columns:1fr 150px;gap:6px 14px;align-items:center;padding:8px 0;border-top:1px solid #1b2334}
.row:first-of-type{border-top:0}
.row label{font-weight:600}.row .hint{grid-column:1/-1;color:var(--muted);font-size:13px}
.row .err{grid-column:1/-1;color:var(--bad);font-size:13px;min-height:0}
input[type=number],select{width:100%;padding:8px 10px;border-radius:7px;border:1px solid #344763;background:#0d1320;color:var(--txt);font:inherit}
input.bad,select.bad{border-color:var(--bad)}
input:disabled,select:disabled{opacity:.55}
.lamp{display:grid;grid-template-columns:90px 1fr 60px 140px;gap:10px;align-items:center;padding:6px 0}.lamp label.off{white-space:nowrap}
.lamp input[type=range]{width:100%}
.bar{position:relative;height:26px;border-radius:6px;background:#0d1320;border:1px solid #344763;margin:10px 0 4px}
.bar i{position:absolute;top:-4px;width:2px;height:32px;background:var(--acc)}.bar b{position:absolute;top:4px;font-size:11px;transform:translateX(-50%);color:var(--txt)}
.actions{display:flex;gap:12px;align-items:center;flex-wrap:wrap}
button{padding:10px 20px;border-radius:8px;border:1px solid #2f7f86;background:#124247;color:#eaffff;font:inherit;font-weight:700;cursor:pointer}
button.ghost{background:transparent;border-color:#344763;color:var(--txt);font-weight:500}
button:disabled{opacity:.5;cursor:default}
#msg{font-size:14px}#msg.ok{color:var(--ok)}#msg.bad{color:var(--bad)}
.note{color:var(--muted);font-size:13px}
@media(max-width:560px){.row{grid-template-columns:1fr}.lamp{grid-template-columns:70px 1fr 60px}.lamp label.off{grid-column:1/-1}}
</style></head><body><main>
<h1>Ρυθμίσεις ταινίας LED</h1>
<p class="lead">Αποθηκεύονται στο <code>settings.json</code> και ισχύουν αμέσως. <a href="/">← Πίσω στο monitor</a></p>
<p id="ro" class="note" hidden>Μόνο ανάγνωση: αλλαγές γίνονται από τον browser αυτού του μηχανήματος ή μετά από σύνδεση στο monitor.</p>
<form id="f" novalidate>
<section><h2>ΤΑΙΝΙΑ ΚΑΙ ΡΕΥΜΑ</h2>
 <div class="row"><label for="led_count">Αριθμός LED</label><input id="led_count" type="number" min="1" max="1000" step="1">
  <div class="hint">Όσα LED έχει η ταινία μετά το κόψιμο.</div><div class="err" data-for="led_count"></div></div>
 <div class="row"><label for="ma_per_led_full_white">mA ανά LED (πλήρες λευκό)</label><input id="ma_per_led_full_white" type="number" min="1" max="200" step="0.5">
  <div class="hint">Προεπιλογή 45, μετρημένη στην ταινία αναφοράς (BTF WS2812B ECO 30 LED/m). Για άλλη ταινία βάλε τη δική σου μέτρηση.</div><div class="err" data-for="ma_per_led_full_white"></div></div>
 <div class="row"><label for="current_budget_ma">Όριο ρεύματος (mA)</label><input id="current_budget_ma" type="number" min="0" max="900" step="10">
  <div class="hint">Σκληρό όριο μόνο για τα LED. Προεπιλογή 450 mA (τροφοδοσία από USB της TV)· όχι μετρημένη τιμή για το δικό σου τροφοδοτικό. Ανώτατη τιμή <span id="maxb">900</span> mA (ορίζεται όταν χτίζεται το πρόγραμμα). <span id="need"></span></div><div class="err" data-for="current_budget_ma"></div></div>
 <div class="row"><label for="brightness">Φωτεινότητα (%)</label><input id="brightness" type="number" min="0" max="100" step="1">
  <div class="hint">Εφαρμόζεται πριν από το όριο ρεύματος. 100 = όπως τα στέλνει η TV.</div><div class="err" data-for="brightness"></div></div>
</section>
<section><h2>ΣΥΜΠΕΡΙΦΟΡΑ</h2>
 <div class="row"><label for="rise_ms">rise_ms — χρόνος ανόδου (ms)</label><input id="rise_ms" type="number" min="0" max="10000" step="10">
  <div class="hint">Πόσο γρήγορα επιτρέπεται ένα άλμα προς το φωτεινό. 0 = αμέσως.</div><div class="err" data-for="rise_ms"></div></div>
 <div class="row"><label for="peak_emphasis">peak_emphasis (0–1)</label><input id="peak_emphasis" type="number" min="0" max="1" step="0.05">
  <div class="hint">Όταν το καρέ ξεπερνά το όριο: 0 = όλα τα LED χαμηλώνουν το ίδιο, 1 = οι κορυφές μένουν όσο γίνεται φωτεινές.</div><div class="err" data-for="peak_emphasis"></div></div>
</section>
<section><h2>ΜΟΝΤΕΛΟ ΡΕΥΜΑΤΟΣ, ΦΘΟΡΑ, ΓΩΝΙΕΣ (v0.33)</h2>
 <p class="note">Με τις προεπιλογές (1 / 1 / 1, 0, linear, 23) το αποτέλεσμα είναι ακριβώς αυτό του v0.32. Καμία από αυτές τις τιμές δεν είναι μετρημένη.</p>
 <div class="row"><label for="ma_scale_r">Ρεύμα κόκκινου καναλιού (×)</label><input id="ma_scale_r" type="number" min="0.1" max="3" step="0.01">
  <div class="hint">Πόσο τραβά το κόκκινο σε σχέση με το 1/3 του λευκού. Μέτρησε με USB meter: (mA μόνο κόκκινο) ÷ (mA λευκού ÷ 3). Μικρότερη τιμή από την πραγματική = το όριο αφήνει περισσότερο ρεύμα απ' όσο λέει.</div><div class="err" data-for="ma_scale_r"></div></div>
 <div class="row"><label for="ma_scale_g">Ρεύμα πράσινου καναλιού (×)</label><input id="ma_scale_g" type="number" min="0.1" max="3" step="0.01">
  <div class="err" data-for="ma_scale_g"></div></div>
 <div class="row"><label for="ma_scale_b">Ρεύμα μπλε καναλιού (×)</label><input id="ma_scale_b" type="number" min="0.1" max="3" step="0.01">
  <div class="err" data-for="ma_scale_b"></div></div>
 <div class="row"><label for="wear_strength">wear_strength (0–1)</label><input id="wear_strength" type="number" min="0" max="1" step="0.05">
  <div class="hint">Όταν το καρέ ξεπερνά το όριο: τα LED που έχουν δουλέψει περισσότερο (mAh από την αρχή του ιστορικού) χαμηλώνουν πρώτα. 0 = ανενεργό. Προσοχή: το ιστορικό μετρά και ό,τι έπαιξε πριν μπει ή κοπεί η ταινία.</div><div class="err" data-for="wear_strength"></div></div>
 <div class="row"><label for="corner_js_curve">Καμπύλη πλευρών JointSpace</label><select id="corner_js_curve">
   <option value="linear">linear — όπως ως τώρα</option><option value="srgb">srgb — ίδια καμπύλη με τις λάμπες Hue</option></select>
  <div class="hint">Αφορά τις γωνίες/άκρες όταν παίρνουν χρώμα από το JointSpace. Ποια ταιριάζει με τις ταινίες της TV δεν έχει μετρηθεί.</div><div class="err" data-for="corner_js_curve"></div></div>
 <div class="row"><label for="corner_js_full_scale">Τιμή JointSpace για «πλήρες»</label><input id="corner_js_full_scale" type="number" min="1" max="255" step="1">
  <div class="hint">23 μετρήθηκε μόνο με φωτεινότητα Ambilight 3/10· με άλλη φωτεινότητα στην TV θέλει άλλη τιμή.</div><div class="err" data-for="corner_js_full_scale"></div></div>
</section>
<section><h2>ΑΝΤΙΣΤΟΙΧΙΣΗ ΛΑΜΠΩΝ ΣΕ ΘΕΣΕΙΣ</h2>
 <div class="row"><label for="strip_layout">Διάταξη</label><select id="strip_layout">
   <option value="tv">tv — όπως διαβάζει η TV (προεπιλογή)</option><option value="inset">inset — λάμπες στο 25/50/75 %</option><option value="custom">custom — ορίζω εγώ τις θέσεις</option></select>
  <div class="hint">Με «custom» κάθε λάμπα της ροής μπαίνει στη θέση που δίνεις: 0 % = αρχή ταινίας, 100 % = τέλος. Οι λάμπες είναι με τη σειρά που τις στέλνει η TV (1, 2, 3…).</div><div class="err" data-for="strip_layout"></div></div>
 <div id="lamps"></div>
 <div class="bar" id="bar"></div>
 <div class="note" id="lampNote"></div><div class="err" data-for="lamp_positions" style="color:var(--bad);font-size:13px"></div>
</section>
<div class="actions"><button id="save" type="submit">Αποθήκευση</button><button id="reset" type="button" class="ghost">Προεπιλογές</button><span id="msg"></span></div>
</form>
<script>
const $=id=>document.getElementById(id);
const nums=['led_count','ma_per_led_full_white','current_budget_ma','rise_ms','peak_emphasis','ma_scale_r','ma_scale_g','ma_scale_b','wear_strength','corner_js_full_scale'];
let lampCount=3,canEdit=false,defaults={},positions=[];
function autoPositions(n){return Array.from({length:n},(_,i)=>n===1?0.5:i/(n-1));}
function drawLamps(){
  const custom=$('strip_layout').value==='custom';
  const box=$('lamps');box.innerHTML='';
  for(let i=0;i<lampCount;i++){
    const shown=positions[i]>=0,pct=shown?Math.round(positions[i]*100):50;
    const d=document.createElement('div');d.className='lamp';
    d.innerHTML=`<strong>Λάμπα ${i+1}</strong><input type="range" min="0" max="100" step="1" value="${pct}" data-i="${i}" aria-label="Θέση λάμπας ${i+1}" ${custom&&shown&&canEdit?'':'disabled'}>`+
      `<span>${shown?pct+' %':'—'}</span><label class="off"><input type="checkbox" data-off="${i}" ${shown?'':'checked'} ${custom&&canEdit?'':'disabled'}> δεν φαίνεται</label>`;
    box.appendChild(d);
  }
  box.querySelectorAll('input[type=range]').forEach(r=>r.addEventListener('input',e=>{positions[+e.target.dataset.i]=e.target.value/100;drawLamps();}));
  box.querySelectorAll('input[type=checkbox]').forEach(c=>c.addEventListener('change',e=>{const i=+e.target.dataset.off;positions[i]=e.target.checked?-1:0.5;drawLamps();}));
  const bar=$('bar');bar.innerHTML='';
  positions.forEach((p,i)=>{if(p<0)return;const m=document.createElement('i');m.style.left=`calc(${p*100}% - 1px)`;bar.appendChild(m);const t=document.createElement('b');t.style.left=`${Math.min(96,Math.max(4,p*100))}%`;t.textContent=i+1;bar.appendChild(t);});
  bar.style.opacity=custom?1:.4;
  $('lampNote').textContent=custom?`Η ροή έχει ${lampCount} λάμπες. Οι θέσεις ισχύουν μόνο όταν το πλήθος τους ταιριάζει με τη ροή· αλλιώς ισχύει η διάταξη «tv».`:'Οι θέσεις ενεργοποιούνται με διάταξη «custom».';
}
function need(){
  const n=+$('led_count').value,b=+$('current_budget_ma').value;
  const per=+$('ma_per_led_full_white').value*((+$('ma_scale_r').value||1)+(+$('ma_scale_g').value||1)+(+$('ma_scale_b').value||1))/3;
  if(n>0&&per>0)$('need').textContent=`Πλήρες λευκό σε ${n} LED θα ήθελε περίπου ${Math.round(n*per)} mA· με όριο ${Math.round(b)} mA το λευκό βγαίνει στο ${Math.min(100,Math.round(100*b/(n*per)))} %.`;
}
function fill(s){
  nums.forEach(k=>{$(k).value=s[k];});
  $('brightness').value=Math.round((s.brightness??1)*100);
  $('strip_layout').value=s.strip_layout||'tv';
  $('corner_js_curve').value=s.corner_js_curve||'linear';
  const lp=Array.isArray(s.lamp_positions)?s.lamp_positions:[];
  positions=lp.length===lampCount?lp.slice():autoPositions(lampCount);
  drawLamps();need();
}
function setErrors(list){
  document.querySelectorAll('.err').forEach(e=>e.textContent='');
  document.querySelectorAll('.bad').forEach(e=>e.classList.remove('bad'));
  const names={'a number is needed':'Χρειάζεται αριθμός','must be a whole number':'Χρειάζεται ακέραιος','unknown setting':'Άγνωστη ρύθμιση'};
  (list||[]).forEach(er=>{
    const f=er.field==='brightness'?'brightness':er.field;
    const box=document.querySelector(`.err[data-for="${f}"]`);
    let m=names[er.message]||er.message.replace('must be between','Πρέπει να είναι από').replace(' and ',' έως ').replace('must be one of:','Επιτρεπτές τιμές:');
    if(er.field==='brightness')m=m.replace('από 0 έως 1','από 0 έως 100');
    if(box)box.textContent=m;
    const inp=$(f);if(inp)inp.classList.add('bad');
  });
}
function collect(){
  const o={};
  nums.forEach(k=>{const v=$(k).value.trim();o[k]=v===''?null:Number(v);});
  const b=$('brightness').value.trim();o.brightness=b===''?null:Number(b)/100;
  o.strip_layout=$('strip_layout').value;
  o.corner_js_curve=$('corner_js_curve').value;
  o.lamp_positions=positions.map(p=>p<0?-1:Math.round(p*100)/100);
  return o;
}
async function load(){
  try{
    const id=await (await fetch('/api/identity',{cache:'no-store'})).json();
    if(id&&id.running&&id.running.lamps)lampCount=id.running.lamps;
  }catch(e){}
  const r=await (await fetch('/api/settings/strip',{cache:'no-store'})).json();
  canEdit=!!r.can_edit;defaults=r.defaults||{};
  if(r.limits&&r.limits.current_budget_ma_max){$('current_budget_ma').max=r.limits.current_budget_ma_max;$('maxb').textContent=r.limits.current_budget_ma_max;}
  $('ro').hidden=canEdit;
  document.querySelectorAll('#f input,#f select,#f button').forEach(e=>e.disabled=!canEdit);
  fill(r.settings||defaults);
}
$('f').addEventListener('input',need);
$('strip_layout').addEventListener('change',drawLamps);
$('reset').addEventListener('click',()=>{fill(Object.assign({},defaults,{lamp_positions:[]}));$('msg').className='';$('msg').textContent='Προεπιλογές στη φόρμα — πάτα Αποθήκευση για να ισχύσουν.';});
$('f').addEventListener('submit',async e=>{
  e.preventDefault();$('msg').className='';$('msg').textContent='Αποθήκευση…';setErrors([]);
  try{
    const res=await fetch('/api/settings/strip/validated',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(collect())});
    const j=await res.json();
    if(res.status===422){setErrors(j.errors);$('msg').className='bad';$('msg').textContent='Δεν αποθηκεύτηκε: διόρθωσε τα πεδία με κόκκινο.';return;}
    if(!res.ok){$('msg').className='bad';$('msg').textContent=res.status===403?'Δεν επιτρέπεται από αυτή τη συσκευή.':'Σφάλμα '+res.status;return;}
    fill(j.settings);$('msg').className=j.saved_to_disk?'ok':'bad';
    $('msg').textContent=j.saved_to_disk?'Αποθηκεύτηκε και ισχύει.':'Ισχύει, αλλά ΔΕΝ γράφτηκε στο settings.json (σφάλμα δίσκου).';
  }catch(err){$('msg').className='bad';$('msg').textContent='Το bridge δεν απάντησε.';}
});
load();
</script></main></body></html>
)PAGE";

}  // namespace settings_page
